#include "MSXNeoTron.hh"

#include "DeviceConfig.hh"
#include "MSXMotherBoard.hh"
#include "ResampledSoundDevice.hh"
#include "Schedulable.hh"
#include "serialize.hh"

#include <algorithm>
#include <cassert>
#include <string_view>

namespace openmsx {

namespace {
constexpr size_t INITIAL_ADPCM_RAM = 64 * 1024;
constexpr size_t MAX_ADPCM_RAM = 16 * 1024 * 1024;
constexpr std::string_view NEOTRON_ID = "OSC YM  OPNBYM2610B";
}

class NeoTronYM2610::Timer final : private Schedulable
{
public:
	Timer(NeoTronYM2610& owner_, unsigned index_)
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
	NeoTronYM2610& owner;
	unsigned index;
};

class NeoTronYM2610::AudioGroup final : public ResampledSoundDevice
{
public:
	AudioGroup(NeoTronYM2610& owner_, Group group_, const DeviceConfig& config,
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
		switch (group) {
		case Group::FM:     return "FM " + std::to_string(channel + 1);
		case Group::SSG:    return std::string{"SSG "} + char('A' + channel);
		case Group::ADPCMA: return "ADPCM-A " + std::to_string(channel + 1);
		case Group::ADPCMB: return "ADPCM-B";
		}
		UNREACHABLE;
	}

private:
	void generateChannels(std::span<float*> bufs, unsigned num) override
	{
		owner.generateChannels(group, bufs, num);
	}

private:
	NeoTronYM2610& owner;
	Group group;
};

NeoTronYM2610::NeoTronYM2610(const DeviceConfig& config, EmuTime time)
	: motherBoard(config.getMotherBoard())
	, chip(*this)
	, timers{std::make_unique<Timer>(*this, 0), std::make_unique<Timer>(*this, 1)}
	, adpcmARam(INITIAL_ADPCM_RAM, 0)
	, adpcmBRam(INITIAL_ADPCM_RAM, 0)
{
	audioGroups[unsigned(Group::FM)] = std::make_unique<AudioGroup>(
		*this, Group::FM, config, "NeoTron-FM", "NeoTron - YM2610B FM", 6);
	audioGroups[unsigned(Group::SSG)] = std::make_unique<AudioGroup>(
		*this, Group::SSG, config, "NeoTron-SSG", "NeoTron - YM2610B SSG", 3);
	audioGroups[unsigned(Group::ADPCMA)] = std::make_unique<AudioGroup>(
		*this, Group::ADPCMA, config, "NeoTron-ADPCM-A", "NeoTron - YM2610B ADPCM-A", 6);
	audioGroups[unsigned(Group::ADPCMB)] = std::make_unique<AudioGroup>(
		*this, Group::ADPCMB, config, "NeoTron-ADPCM-B", "NeoTron - YM2610B ADPCM-B", 1);
	reset(time);
}

NeoTronYM2610::~NeoTronYM2610() = default;

void NeoTronYM2610::updateSoundStream(EmuTime time)
{
	audioGroups[unsigned(Group::FM)]->sync(time);
}

void NeoTronYM2610::reset(EmuTime time)
{
	updateSoundStream(time);
	accessTime = time;
	busyEnd = time;
	for (auto& timer : timers) timer->cancel();
	timerEnds.fill(EmuTime::infinity());
	chip.set_fidelity(ymfm::OPN_FIDELITY_MAX);
	chip.reset();
	outputBuffer.clear();
	sampleMemory = 1;
	sampleWriteAddress = 0;
}

uint8_t NeoTronYM2610::read(uint8_t offset) const
{
	return const_cast<ymfm::ym2610b&>(chip).read(offset);
}

void NeoTronYM2610::write(uint8_t offset, uint8_t value, EmuTime time)
{
	if ((offset & 1) != 0) updateSoundStream(time);
	accessTime = time;
	chip.write(offset, value);
}

void NeoTronYM2610::setSampleMemory(uint8_t memory)
{
	sampleMemory = memory;
}

void NeoTronYM2610::setSampleAddressLow(uint8_t value)
{
	sampleWriteAddress = (sampleWriteAddress & 0xff0000) | (uint32_t(value) << 8);
}

void NeoTronYM2610::setSampleAddressHigh(uint8_t value)
{
	sampleWriteAddress = (sampleWriteAddress & 0x00ff00) | (uint32_t(value) << 16);
}

void NeoTronYM2610::writeSampleData(uint8_t value)
{
	writeSample(sampleMemory == 1 ? adpcmARam : adpcmBRam, sampleWriteAddress++, value);
}

void NeoTronYM2610::writeSample(std::vector<uint8_t>& memory, uint32_t address, uint8_t data)
{
	if (address >= MAX_ADPCM_RAM) return;
	if (address >= memory.size()) {
		size_t newSize = memory.size();
		while (newSize <= address && newSize < MAX_ADPCM_RAM) newSize *= 2;
		memory.resize(std::min(newSize, MAX_ADPCM_RAM), 0);
	}
	memory[address] = data;
}

void NeoTronYM2610::generateChannels(Group group, std::span<float*> bufs, unsigned num)
{
	if (group == Group::FM) {
		outputBuffer.resize(num);
		for (auto& output : outputBuffer) chip.generate_channels(&output);
	} else if (outputBuffer.size() != num) {
		assert(false);
		return;
	}

	for (unsigned i = 0; i < num; ++i) {
		const auto& output = outputBuffer[i];
		switch (group) {
		case Group::FM:
			assert(bufs.size() == 6);
			for (unsigned ch = 0; ch < 6; ++ch) {
				bufs[ch][2 * i + 0] += float(output.data[2 * ch + 0]);
				bufs[ch][2 * i + 1] += float(output.data[2 * ch + 1]);
			}
			break;
		case Group::SSG:
			assert(bufs.size() == 3);
			for (unsigned ch = 0; ch < 3; ++ch) {
				auto value = float(output.data[26 + ch]);
				bufs[ch][2 * i + 0] += value;
				bufs[ch][2 * i + 1] += value;
			}
			break;
		case Group::ADPCMA:
			assert(bufs.size() == 6);
			for (unsigned ch = 0; ch < 6; ++ch) {
				bufs[ch][2 * i + 0] += float(output.data[12 + 2 * ch + 0]);
				bufs[ch][2 * i + 1] += float(output.data[12 + 2 * ch + 1]);
			}
			break;
		case Group::ADPCMB:
			assert(bufs.size() == 1);
			bufs[0][2 * i + 0] += float(output.data[24]);
			bufs[0][2 * i + 1] += float(output.data[25]);
			break;
		}
	}
}

void NeoTronYM2610::ymfm_set_timer(uint32_t timer, int32_t durationInClocks)
{
	if (timer >= timers.size()) return;
	if (durationInClocks < 0) {
		timers[timer]->cancel();
		timerEnds[timer] = EmuTime::infinity();
	} else {
		auto end = accessTime + EmuDuration::sec(double(durationInClocks) / CLOCK_FREQ);
		timerEnds[timer] = end;
		timers[timer]->schedule(end);
	}
}

void NeoTronYM2610::ymfm_set_busy_end(uint32_t clocks)
{
	busyEnd = accessTime + EmuDuration::sec(double(clocks) / CLOCK_FREQ);
}

bool NeoTronYM2610::ymfm_is_busy()
{
	return motherBoard.getCurrentTime() < busyEnd;
}

void NeoTronYM2610::timerExpired(unsigned timer, EmuTime time)
{
	timerEnds[timer] = EmuTime::infinity();
	updateSoundStream(time);
	accessTime = time;
	m_engine->engine_timer_expired(timer);
}

uint8_t NeoTronYM2610::ymfm_external_read(ymfm::access_class type, uint32_t address)
{
	if (type == ymfm::ACCESS_ADPCM_A) return address < adpcmARam.size() ? adpcmARam[address] : 0;
	if (type == ymfm::ACCESS_ADPCM_B) return address < adpcmBRam.size() ? adpcmBRam[address] : 0;
	return 0;
}

void NeoTronYM2610::ymfm_external_write(ymfm::access_class type, uint32_t address, uint8_t data)
{
	if (type == ymfm::ACCESS_ADPCM_A) writeSample(adpcmARam, address, data);
	if (type == ymfm::ACCESS_ADPCM_B) writeSample(adpcmBRam, address, data);
}

template<typename Archive>
void NeoTronYM2610::serialize(Archive& ar, unsigned /*version*/)
{
	ar.serialize("ymfmState", stateBuffer,
	             "adpcmARam", adpcmARam,
	             "adpcmBRam", adpcmBRam,
	             "busyEnd", busyEnd,
	             "timerEnds", timerEnds,
	             "sampleMemory", sampleMemory,
	             "sampleWriteAddress", sampleWriteAddress);
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
INSTANTIATE_SERIALIZE_METHODS(NeoTronYM2610);

MSXNeoTron::MSXNeoTron(DeviceConfig& config)
	: MSXDevice(config)
	, ym2610(config, getCurrentTime())
{
	reset(getCurrentTime());
}

void MSXNeoTron::reset(EmuTime time)
{
	ym2610.reset(time);
}

byte MSXNeoTron::readMemory(uint16_t address) const
{
	if ((address & 0xfffc) == 0xbc00) return ym2610.read(address & 3);
	if (address >= 0x806c && address < 0x806c + NEOTRON_ID.size()) {
		return byte(NEOTRON_ID[address - 0x806c]);
	}

	// Minimal SIOS service ROM used by NeoTron's VGM driver. The service at
	// 802B selects a sample memory/address; 8025 copies BC bytes from HL to it.
	switch (address) {
	case 0x8019: case 0x801c: case 0x8022: return 0xc9; // reset, I/O enable, erase
	case 0x8025: return 0xc3; // JP 8100h (sample write routine)
	case 0x8026: return 0x00;
	case 0x8027: return 0x81;
	case 0x802b: return 0x32; // LD (807Eh),A
	case 0x802c: return 0x7e;
	case 0x802d: return 0x80;
	case 0x802e: return 0x7b; // LD A,E
	case 0x802f: return 0x32; // LD (807Fh),A
	case 0x8030: return 0x7f;
	case 0x8031: return 0x80;
	case 0x8032: return 0x7a; // LD A,D
	case 0x8033: return 0x32; // LD (8080h),A
	case 0x8034: return 0x80;
	case 0x8035: return 0x80;
	case 0x8036: return 0xc9;
	case 0x8100: return 0x7e; // LD A,(HL)
	case 0x8101: return 0x32; // LD (8081h),A
	case 0x8102: return 0x81;
	case 0x8103: return 0x80;
	case 0x8104: return 0x23; // INC HL
	case 0x8105: return 0x0b; // DEC BC
	case 0x8106: return 0x78; // LD A,B
	case 0x8107: return 0xb1; // OR C
	case 0x8108: return 0x20; // JR NZ,8100h
	case 0x8109: return 0xf6;
	case 0x810a: return 0xc9;
	default: return 0xff;
	}
}

byte MSXNeoTron::readMem(uint16_t address, EmuTime /*time*/)
{
	return readMemory(address);
}

byte MSXNeoTron::peekMem(uint16_t address, EmuTime /*time*/) const
{
	return readMemory(address);
}

void MSXNeoTron::writeMem(uint16_t address, byte value, EmuTime time)
{
	if ((address & 0xfffc) == 0xbc00) {
		ym2610.write(address & 3, value, time);
		return;
	}
	switch (address) {
	case 0x807e: ym2610.setSampleMemory(value); break;
	case 0x807f: ym2610.setSampleAddressLow(value); break;
	case 0x8080: ym2610.setSampleAddressHigh(value); break;
	case 0x8081: ym2610.writeSampleData(value); break;
	default: break;
	}
}

template<typename Archive>
void MSXNeoTron::serialize(Archive& ar, unsigned /*version*/)
{
	ar.template serializeBase<MSXDevice>(*this);
	ar.serialize("ym2610", ym2610);
}
INSTANTIATE_SERIALIZE_METHODS(MSXNeoTron);
REGISTER_MSXDEVICE(MSXNeoTron, "NEOTRON");

} // namespace openmsx
