#ifndef MSXDARKY_HH
#define MSXDARKY_HH

#include "AY8910.hh"
#include "MSXDevice.hh"

#include <array>

namespace openmsx {

class DeviceConfig;

/** Supersoniqs Darky: two AY8930 enhanced PSGs at ports 44h and 4Ch. */
class MSXDarky final : public MSXDevice
{
public:
	explicit MSXDarky(DeviceConfig& config);

	void reset(EmuTime time) override;
	[[nodiscard]] byte readIO(uint16_t port, EmuTime time) override;
	[[nodiscard]] byte peekIO(uint16_t port, EmuTime time) const override;
	void writeIO(uint16_t port, byte value, EmuTime time) override;

	template<typename Archive>
	void serialize(Archive& ar, unsigned version);

private:
	AY8910& selectChip(uint16_t port);
	[[nodiscard]] const AY8910& selectChip(uint16_t port) const;
	[[nodiscard]] unsigned& selectLatch(uint16_t port);
	[[nodiscard]] const unsigned& selectLatch(uint16_t port) const;

private:
	AY8910 ay8930_1;
	AY8910 ay8930_2;
	std::array<unsigned, 2> registerLatch{};
	byte switchedIO = 0;
	byte microcontroller = 0;
};

} // namespace openmsx

#endif
