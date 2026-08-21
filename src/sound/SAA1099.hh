#ifndef SAA1099_HH
#define SAA1099_HH

#include "ResampledSoundDevice.hh"

#include <array>
#include <cstdint>

namespace openmsx {

class DeviceConfig;

/** Philips SAA1099 six-voice stereo sound generator.
 *
 * SoundStar clocks this chip at 8 MHz.  The SAA1099 contains six tone
 * generators, two noise generators and two envelope generators.  A voice
 * has independently programmable left and right amplitudes.
 */
class SAA1099 final : public ResampledSoundDevice
{
public:
	explicit SAA1099(const DeviceConfig& config);
	~SAA1099();

	void reset(EmuTime time);
	void writeRegister(unsigned reg, uint8_t value, EmuTime time);

	void generateChannels(std::span<float*> buffers, unsigned num) override;

	template<typename Archive>
	void serialize(Archive& ar, unsigned version);

private:
	[[nodiscard]] unsigned tonePeriod(unsigned channel) const;
	[[nodiscard]] unsigned noisePeriod(unsigned noise) const;
	[[nodiscard]] unsigned envelopeVolume(unsigned group) const;
	void stepEnvelope(unsigned group);
	void resetState();

private:
	std::array<uint8_t, 32> regs{};
	std::array<unsigned, 6> toneCounter{};
	std::array<bool, 6> toneLevel{};
	std::array<unsigned, 2> noiseCounter{};
	std::array<uint32_t, 2> noiseLfsr{};
	std::array<unsigned, 2> envelopeStep{};
	std::array<bool, 2> envelopeUp{};
	bool soundEnabled = false;
};

} // namespace openmsx

#endif
