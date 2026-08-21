#include "MSXSoundStar.hh"

#include "DeviceConfig.hh"
#include "serialize.hh"

namespace openmsx {

namespace {
constexpr std::string_view SOUNDSTAR_ID = "SAA1099";
}

MSXSoundStar::MSXSoundStar(DeviceConfig& config)
	: MSXDevice(config)
	, saa1099(config)
{
	reset(getCurrentTime());
}

void MSXSoundStar::reset(EmuTime time)
{
	registerLatch = 0;
	saa1099.reset(time);
}

byte MSXSoundStar::readIO(uint16_t /*port*/, EmuTime /*time*/)
{
	return 0xff; // The SoundStar write-only SAA1099 bus has no readback.
}

byte MSXSoundStar::peekIO(uint16_t /*port*/, EmuTime /*time*/) const
{
	return 0xff;
}

void MSXSoundStar::writeIO(uint16_t port, byte value, EmuTime time)
{
	switch (port & 0xff) {
	case 0x05:
		registerLatch = value & 31;
		break;
	case 0x04:
		saa1099.writeRegister(registerLatch, value, time);
		break;
	default:
		break;
	}
}

byte MSXSoundStar::readMem(uint16_t address, EmuTime /*time*/)
{
	return peekMem(address, EmuTime::zero());
}

byte MSXSoundStar::peekMem(uint16_t address, EmuTime /*time*/) const
{
	// SoundStar's VGMPlayer driver detects the card through this signature
	// in the cartridge ROM. The remaining ROM space is intentionally empty.
	if (address >= 0x4010 && address < 0x4010 + SOUNDSTAR_ID.size()) {
		return byte(SOUNDSTAR_ID[address - 0x4010]);
	}
	if (address == 0x4010 + SOUNDSTAR_ID.size()) return 0;
	return 0xff;
}

template<typename Archive>
void MSXSoundStar::serialize(Archive& ar, unsigned /*version*/)
{
	ar.template serializeBase<MSXDevice>(*this);
	ar.serialize("saa1099", saa1099, "registerLatch", registerLatch);
}
INSTANTIATE_SERIALIZE_METHODS(MSXSoundStar);
REGISTER_MSXDEVICE(MSXSoundStar, "SOUNDSTAR");

} // namespace openmsx
