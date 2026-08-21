#ifndef MSXSOUNDSTAR_HH
#define MSXSOUNDSTAR_HH

#include "MSXDevice.hh"
#include "SAA1099.hh"

namespace openmsx {

class DeviceConfig;

/** Supersoniqs SoundStar cartridge: an 8 MHz SAA1099 at ports 04h/05h. */
class MSXSoundStar final : public MSXDevice
{
public:
	explicit MSXSoundStar(DeviceConfig& config);

	void reset(EmuTime time) override;
	[[nodiscard]] byte readIO(uint16_t port, EmuTime time) override;
	[[nodiscard]] byte peekIO(uint16_t port, EmuTime time) const override;
	void writeIO(uint16_t port, byte value, EmuTime time) override;
	[[nodiscard]] byte readMem(uint16_t address, EmuTime time) override;
	[[nodiscard]] byte peekMem(uint16_t address, EmuTime time) const override;

	template<typename Archive>
	void serialize(Archive& ar, unsigned version);

private:
	SAA1099 saa1099;
	unsigned registerLatch = 0;
};

} // namespace openmsx

#endif
