#include "MSXMakoto.hh"

#include "DeviceConfig.hh"
#include "MSXMotherBoard.hh"
#include "Schedulable.hh"
#include "serialize.hh"

#include <algorithm>
#include <cassert>

namespace openmsx {

namespace {
constexpr size_t INITIAL_ADPCM_RAM = 64 * 1024;
constexpr size_t MAX_ADPCM_RAM = 16 * 1024 * 1024;
}

class MakotoYM2608::Timer final : private Schedulable
{
public:
    Timer(MakotoYM2608& owner_, unsigned index_)
        : Schedulable(owner_.motherBoard.getScheduler())
        , owner(owner_)
        , index(index_)
    {
    }

    void schedule(EmuTime time)
    {
        removeSyncPoints();
        setSyncPoint(time);
    }
    void cancel() { removeSyncPoints(); }

private:
    void executeUntil(EmuTime time) override { owner.timerExpired(index, time); }

private:
    MakotoYM2608& owner;
    unsigned index;
};

class MakotoYM2608::AudioGroup final : public ResampledSoundDevice
{
public:
    AudioGroup(MakotoYM2608& owner_, Group group_, const DeviceConfig& config,
               std::string_view name, static_string_view description, unsigned channels)
        : ResampledSoundDevice(config.getMotherBoard(), name, description,
                               channels, INPUT_RATE, true)
        , owner(owner_)
        , group(group_)
    {
        registerSound(config);
    }

    ~AudioGroup() { unregisterSound(); }

    void sync(EmuTime time) { updateStream(time); }

    [[nodiscard]] std::string getChannelLabel(unsigned channel) const override
    {
        if (group == Group::FM) return "FM " + std::to_string(channel + 1);
        if (group == Group::SSG) return std::string{"SSG "} + char('A' + channel);
        if (group == Group::ADPCMA) return "ADPCM-A " + std::to_string(channel + 1);
        return "ADPCM-B";
    }

private:
    void generateChannels(std::span<float*> bufs, unsigned num) override
    {
        owner.generateChannels(group, bufs, num);
    }

private:
    MakotoYM2608& owner;
    Group group;
};

MakotoYM2608::MakotoYM2608(const DeviceConfig& config, EmuTime time)
    : motherBoard(config.getMotherBoard())
    , chip(*this)
    , timers{std::make_unique<Timer>(*this, 0), std::make_unique<Timer>(*this, 1)}
    , adpcmRam(INITIAL_ADPCM_RAM, 0)
{
    audioGroups[unsigned(Group::FM)] = std::make_unique<AudioGroup>(
        *this, Group::FM, config, "Makoto-FM", "Makoto - YM2608 FM", 6);
    audioGroups[unsigned(Group::SSG)] = std::make_unique<AudioGroup>(
        *this, Group::SSG, config, "Makoto-SSG", "Makoto - YM2608 SSG", 3);
    audioGroups[unsigned(Group::ADPCMA)] = std::make_unique<AudioGroup>(
        *this, Group::ADPCMA, config, "Makoto-ADPCM-A", "Makoto - YM2608 ADPCM-A", 6);
    audioGroups[unsigned(Group::ADPCMB)] = std::make_unique<AudioGroup>(
        *this, Group::ADPCMB, config, "Makoto-ADPCM-B", "Makoto - YM2608 ADPCM-B", 1);
    reset(time);
}

MakotoYM2608::~MakotoYM2608() = default;

void MakotoYM2608::updateSoundStream(EmuTime time)
{
    audioGroups[unsigned(Group::FM)]->sync(time);
}

void MakotoYM2608::reset(EmuTime time)
{
    updateSoundStream(time);
    accessTime = time;
    busyEnd = time;
    for (auto& timer : timers) {
        timer->cancel();
    }
    timerEnds.fill(EmuTime::infinity());
    std::fill(adpcmRam.begin(), adpcmRam.end(), uint8_t(0));
    chip.set_fidelity(ymfm::OPN_FIDELITY_MAX);
    chip.reset();
    outputBuffer.clear();
}

uint8_t MakotoYM2608::readStatus0() const
{
    return const_cast<ymfm::ym2608&>(chip).read(0);
}

uint8_t MakotoYM2608::readStatus1() const
{
    return const_cast<ymfm::ym2608&>(chip).read(2);
}

void MakotoYM2608::writeAddress0(uint8_t value, EmuTime time)
{
    setAccessTime(time);
    chip.write_address(value);
}

void MakotoYM2608::writeData0(uint8_t value, EmuTime time)
{
    updateSoundStream(time);
    setAccessTime(time);
    chip.write_data(value);
}

void MakotoYM2608::writeAddress1(uint8_t value, EmuTime time)
{
    setAccessTime(time);
    chip.write_address_hi(value);
}

void MakotoYM2608::writeData1(uint8_t value, EmuTime time)
{
    updateSoundStream(time);
    setAccessTime(time);
    chip.write_data_hi(value);
}

void MakotoYM2608::ymfm_set_timer(uint32_t timer, int32_t durationInClocks)
{
    if (timer >= timers.size()) {
        return;
    }
    if (durationInClocks < 0) {
        timers[timer]->cancel();
        timerEnds[timer] = EmuTime::infinity();
    } else {
        auto end = accessTime + EmuDuration::sec(
            double(durationInClocks) / double(CLOCK_FREQ));
        timerEnds[timer] = end;
        timers[timer]->schedule(end);
    }
}

void MakotoYM2608::ymfm_set_busy_end(uint32_t clocks)
{
    busyEnd = accessTime + EmuDuration::sec(double(clocks) / double(CLOCK_FREQ));
}

bool MakotoYM2608::ymfm_is_busy()
{
    return motherBoard.getCurrentTime() < busyEnd;
}

void MakotoYM2608::timerExpired(unsigned timer, EmuTime time)
{
    timerEnds[timer] = EmuTime::infinity();
    updateSoundStream(time);
    setAccessTime(time);
    m_engine->engine_timer_expired(timer);
}

uint8_t MakotoYM2608::ymfm_external_read(ymfm::access_class type, uint32_t address)
{
    if (type != ymfm::ACCESS_ADPCM_A && type != ymfm::ACCESS_ADPCM_B) {
        return 0;
    }
    return address < adpcmRam.size() ? adpcmRam[address] : 0;
}

void MakotoYM2608::ymfm_external_write(
    ymfm::access_class type, uint32_t address, uint8_t data)
{
    if ((type != ymfm::ACCESS_ADPCM_A && type != ymfm::ACCESS_ADPCM_B) ||
        address >= MAX_ADPCM_RAM) {
        return;
    }
    if (address >= adpcmRam.size()) {
        size_t newSize = adpcmRam.size();
        while (newSize <= address && newSize < MAX_ADPCM_RAM) {
            newSize *= 2;
        }
        adpcmRam.resize(std::min(newSize, MAX_ADPCM_RAM), uint8_t(0));
    }
    adpcmRam[address] = data;
}

void MakotoYM2608::generateChannels(Group group, std::span<float*> bufs, unsigned num)
{
    if (group == Group::FM) {
        outputBuffer.resize(num);
        for (auto& output : outputBuffer) {
            chip.generate_channels(&output);
        }
    } else if (outputBuffer.size() != num) {
        // All three audio groups use the same sample clock. The FM group is
        // registered first and must produce the shared samples before the
        // SSG and ADPCM groups consume them.
        assert(false);
        return;
    }

    for (unsigned i = 0; i < num; ++i) {
        const auto& output = outputBuffer[i];

        if (group == Group::FM) {
            assert(bufs.size() == 6);
            for (unsigned channel = 0; channel < 6; ++channel) {
                bufs[channel][2 * i + 0] += float(output.data[2 * channel + 0]);
                bufs[channel][2 * i + 1] += float(output.data[2 * channel + 1]);
            }
        } else if (group == Group::SSG) {
            assert(bufs.size() == 3);
            for (unsigned channel = 0; channel < 3; ++channel) {
                auto sample = float(output.data[14 + channel]);
                bufs[channel][2 * i + 0] += sample;
                bufs[channel][2 * i + 1] += sample;
            }
        } else if (group == Group::ADPCMA) {
            assert(bufs.size() == 6);
            for (unsigned channel = 0; channel < 6; ++channel) {
                bufs[channel][2 * i + 0] += float(output.data[12 + 2 * channel + 0]);
                bufs[channel][2 * i + 1] += float(output.data[12 + 2 * channel + 1]);
            }
        } else {
            assert(bufs.size() == 1);
            bufs[0][2 * i + 0] += float(output.data[24]);
            bufs[0][2 * i + 1] += float(output.data[25]);
        }
    }
}

template<typename Archive>
void MakotoYM2608::serialize(Archive& ar, unsigned /*version*/)
{
    ar.serialize("ymfmState", stateBuffer,
                 "adpcmRam", adpcmRam,
                 "busyEnd", busyEnd,
                 "timerEnds", timerEnds);

    if constexpr (Archive::IS_LOADER) {
        ymfm::ymfm_saved_state loader(stateBuffer, false);
        chip.save_restore(loader);
        accessTime = motherBoard.getCurrentTime();
        outputBuffer.clear();
        for (unsigned i = 0; i < timers.size(); ++i) {
            timers[i]->cancel();
            if (timerEnds[i] != EmuTime::infinity()) {
                timers[i]->schedule(std::max(timerEnds[i], motherBoard.getCurrentTime()));
            }
        }
    } else {
        stateBuffer.clear();
        ymfm::ymfm_saved_state saver(stateBuffer, true);
        chip.save_restore(saver);
    }
}
INSTANTIATE_SERIALIZE_METHODS(MakotoYM2608);

MSXMakoto::MSXMakoto(DeviceConfig& config)
    : MSXDevice(config)
    , ym2608(config, getCurrentTime())
{
    reset(getCurrentTime());
}

void MSXMakoto::reset(EmuTime time)
{
    ym2608.reset(time);
}

byte MSXMakoto::readIO(uint16_t port, EmuTime /*time*/)
{
    switch (port & 3) {
    case 0: return ym2608.readStatus0();
    case 2: return ym2608.readStatus1();
    default: return 0xFF;
    }
}

byte MSXMakoto::peekIO(uint16_t port, EmuTime /*time*/) const
{
    switch (port & 3) {
    case 0: return ym2608.readStatus0();
    case 2: return ym2608.readStatus1();
    default: return 0xFF;
    }
}

void MSXMakoto::writeIO(uint16_t port, byte value, EmuTime time)
{
    switch (port & 3) {
    case 0:
        ym2608.writeAddress0(value, time);
        break;
    case 1:
        ym2608.writeData0(value, time);
        break;
    case 2:
        ym2608.writeAddress1(value, time);
        break;
    case 3:
        ym2608.writeData1(value, time);
        break;
    }
}

template<typename Archive>
void MSXMakoto::serialize(Archive& ar, unsigned /*version*/)
{
    ar.template serializeBase<MSXDevice>(*this);
    ar.serialize("ym2608", ym2608);
}
INSTANTIATE_SERIALIZE_METHODS(MSXMakoto);
REGISTER_MSXDEVICE(MSXMakoto, "MAKOTO");

} // namespace openmsx
