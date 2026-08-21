#include "MSXDarky.hh"

#include "DeviceConfig.hh"
#include "DummyAY8910Periphery.hh"
#include "serialize.hh"

#include <cassert>

namespace openmsx {

MSXDarky::MSXDarky(DeviceConfig& config)
	: MSXDevice(config)
	, ay8930_1("Darky-AY8930-1", DummyAY8910Periphery::instance(), config, getCurrentTime())
	, ay8930_2("Darky-AY8930-2", DummyAY8910Periphery::instance(), config, getCurrentTime())
{
	reset(getCurrentTime());
}

void MSXDarky::reset(EmuTime time)
{
	registerLatch.fill(0);
	switchedIO = 0;
	microcontroller = 0;
	ay8930_1.reset(time);
	ay8930_2.reset(time);
}

AY8910& MSXDarky::selectChip(uint16_t port)
{
	return (port & 0x08) ? ay8930_2 : ay8930_1;
}

const AY8910& MSXDarky::selectChip(uint16_t port) const
{
	return (port & 0x08) ? ay8930_2 : ay8930_1;
}

unsigned& MSXDarky::selectLatch(uint16_t port)
{
	return registerLatch[(port & 0x08) ? 1 : 0];
}

const unsigned& MSXDarky::selectLatch(uint16_t port) const
{
	return registerLatch[(port & 0x08) ? 1 : 0];
}

byte MSXDarky::readIO(uint16_t port, EmuTime time)
{
	switch (port & 0xff) {
	case 0x40:
		// The Darky driver complements this value. Returning 55h after it
		// selected AAh is the hardware's device-ID acknowledgement.
		return (switchedIO == 0xaa) ? 0x55 : 0xff;
	case 0x45:
	case 0x4d:
		return selectChip(port).readRegister(selectLatch(port), time);
	default:
		return 0xff;
	}
}

byte MSXDarky::peekIO(uint16_t port, EmuTime time) const
{
	switch (port & 0xff) {
	case 0x40: return (switchedIO == 0xaa) ? 0x55 : 0xff;
	case 0x45:
	case 0x4d: return selectChip(port).peekRegister(selectLatch(port), time);
	default: return 0xff;
	}
}

void MSXDarky::writeIO(uint16_t port, byte value, EmuTime time)
{
	switch (port & 0xff) {
	case 0x40:
		switchedIO = value;
		break;
	case 0x42:
		microcontroller = value;
		break;
	case 0x44:
	case 0x4c:
		selectLatch(port) = value & 0x0f;
		break;
	case 0x45:
	case 0x4d:
		selectChip(port).writeRegister(selectLatch(port), value, time);
		break;
	default:
		break;
	}
}

template<typename Archive>
void MSXDarky::serialize(Archive& ar, unsigned /*version*/)
{
	ar.template serializeBase<MSXDevice>(*this);
	ar.serialize("ay8930_1", ay8930_1,
	             "ay8930_2", ay8930_2,
	             "registerLatch", registerLatch,
	             "switchedIO", switchedIO,
	             "microcontroller", microcontroller);
}
INSTANTIATE_SERIALIZE_METHODS(MSXDarky);
REGISTER_MSXDEVICE(MSXDarky, "DARKY");

} // namespace openmsx
