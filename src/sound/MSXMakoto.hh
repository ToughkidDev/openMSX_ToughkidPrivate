#ifndef MSXMAKOTO_HH
#define MSXMAKOTO_HH

#include "MSXDevice.hh"
#include "ResampledSoundDevice.hh"
#include "ymfm_opn.h"

#include <cstdint>
#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace openmsx {

class DeviceConfig;
class MSXMotherBoard;

class MakotoYM2608 final : public ymfm::ymfm_interface
{
public:
    static constexpr uint32_t CLOCK_FREQ = 8'000'000;
    // ymfm's highest YM2608 fidelity generates one sample per 8 input clocks.
    static constexpr unsigned INPUT_RATE = CLOCK_FREQ / 8;

    MakotoYM2608(const DeviceConfig& config, EmuTime time);
    ~MakotoYM2608();

    void reset(EmuTime time);

    [[nodiscard]] uint8_t readStatus0() const;
    [[nodiscard]] uint8_t readStatus1() const;
    void writeAddress0(uint8_t value, EmuTime time);
    void writeData0(uint8_t value, EmuTime time);
    void writeAddress1(uint8_t value, EmuTime time);
    void writeData1(uint8_t value, EmuTime time);

    template<typename Archive>
    void serialize(Archive& ar, unsigned version);

private:
    class Timer;
    class AudioGroup;
    enum class Group : uint8_t { FM, SSG, ADPCMA, ADPCMB };

    void updateSoundStream(EmuTime time);
    void generateChannels(Group group, std::span<float*> bufs, unsigned num);

    void ymfm_set_timer(uint32_t timer, int32_t durationInClocks) override;
    void ymfm_set_busy_end(uint32_t clocks) override;
    bool ymfm_is_busy() override;
    uint8_t ymfm_external_read(ymfm::access_class type, uint32_t address) override;
    void ymfm_external_write(ymfm::access_class type, uint32_t address, uint8_t data) override;

    void setAccessTime(EmuTime time) { accessTime = time; }
    void timerExpired(unsigned timer, EmuTime time);

    MSXMotherBoard& motherBoard;
    ymfm::ym2608 chip;
    std::array<std::unique_ptr<Timer>, 2> timers;
    std::array<std::unique_ptr<AudioGroup>, 4> audioGroups;
    std::vector<ymfm::ym2608::channel_output_data> outputBuffer;
    std::vector<uint8_t> adpcmRam;
    std::vector<uint8_t> stateBuffer;
    EmuTime accessTime = EmuTime::zero();
    EmuTime busyEnd = EmuTime::zero();
    std::array<EmuTime, 2> timerEnds{EmuTime::infinity(), EmuTime::infinity()};
};

class MSXMakoto final : public MSXDevice
{
public:
    explicit MSXMakoto(DeviceConfig& config);

    void reset(EmuTime time) override;
    [[nodiscard]] byte readIO(uint16_t port, EmuTime time) override;
    [[nodiscard]] byte peekIO(uint16_t port, EmuTime time) const override;
    void writeIO(uint16_t port, byte value, EmuTime time) override;

    template<typename Archive>
    void serialize(Archive& ar, unsigned version);

private:
    MakotoYM2608 ym2608;
};

} // namespace openmsx

#endif
