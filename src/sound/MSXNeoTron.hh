#ifndef MSXNEOTRON_HH
#define MSXNEOTRON_HH

#include "MSXDevice.hh"
#include "ymfm_opn.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace openmsx {

class DeviceConfig;
class MSXMotherBoard;

class NeoTronYM2610 final : public ymfm::ymfm_interface
{
public:
	static constexpr uint32_t CLOCK_FREQ = 8'000'000;
	static constexpr unsigned INPUT_RATE = CLOCK_FREQ / 16;

	NeoTronYM2610(const DeviceConfig& config, EmuTime time);
	~NeoTronYM2610();

	void reset(EmuTime time);
	[[nodiscard]] uint8_t read(uint8_t offset) const;
	void write(uint8_t offset, uint8_t value, EmuTime time);

	void setSampleMemory(uint8_t memory);
	void setSampleAddressLow(uint8_t value);
	void setSampleAddressHigh(uint8_t value);
	void writeSampleData(uint8_t value);

	template<typename Archive>
	void serialize(Archive& ar, unsigned version);

private:
	class Timer;
	class AudioGroup;
	enum class Group : unsigned { FM, SSG, ADPCMA, ADPCMB };

	void updateSoundStream(EmuTime time);
	void generateChannels(Group group, std::span<float*> bufs, unsigned num);
	void timerExpired(unsigned timer, EmuTime time);
	void writeSample(std::vector<uint8_t>& memory, uint32_t address, uint8_t data);

	void ymfm_set_timer(uint32_t timer, int32_t durationInClocks) override;
	void ymfm_set_busy_end(uint32_t clocks) override;
	bool ymfm_is_busy() override;
	uint8_t ymfm_external_read(ymfm::access_class type, uint32_t address) override;
	void ymfm_external_write(ymfm::access_class type, uint32_t address, uint8_t data) override;

	MSXMotherBoard& motherBoard;
	ymfm::ym2610b chip;
	std::array<std::unique_ptr<Timer>, 2> timers;
	std::array<std::unique_ptr<AudioGroup>, 4> audioGroups;
	std::vector<ymfm::ym2610::channel_output_data> outputBuffer;
	std::vector<uint8_t> adpcmARam;
	std::vector<uint8_t> adpcmBRam;
	std::vector<uint8_t> stateBuffer;
	EmuTime accessTime = EmuTime::zero();
	EmuTime busyEnd = EmuTime::zero();
	std::array<EmuTime, 2> timerEnds{EmuTime::infinity(), EmuTime::infinity()};
	uint8_t sampleMemory = 1;
	uint32_t sampleWriteAddress = 0;
};

class MSXNeoTron final : public MSXDevice
{
public:
	explicit MSXNeoTron(DeviceConfig& config);

	void reset(EmuTime time) override;
	[[nodiscard]] byte readMem(uint16_t address, EmuTime time) override;
	[[nodiscard]] byte peekMem(uint16_t address, EmuTime time) const override;
	void writeMem(uint16_t address, byte value, EmuTime time) override;

	template<typename Archive>
	void serialize(Archive& ar, unsigned version);

private:
	[[nodiscard]] byte readMemory(uint16_t address) const;

	NeoTronYM2610 ym2610;
};

} // namespace openmsx

#endif
