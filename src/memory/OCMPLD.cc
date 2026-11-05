#include "OCMPLD.hh"

#include "CacheLine.hh"
#include "LedStatus.hh"
#include "MSXCPU.hh"
#include "MSXCPUInterface.hh"
#include "MSXException.hh"
#include "XMLElement.hh"
#include "serialize.hh"

#include "narrow.hh"

#include <algorithm>
#include <cassert>

// Memory map of the 1chipMSX with OCM-PLD (slot 0 and 3 expanded):
//
//   0-0  0000-7FFF  MAIN ROM          = ESE-RAM 0x20000-0x27FFF
//        8000-BFFF  OPT ROM, only in C-BIOS mode
//   0-1             (extra mapper, not emulated: off by default)
//   0-2  4000-7FFF  MSX-MUSIC ROM     = ESE-RAM 0x2C000-0x2FFFF
//   0-3  4000-7FFF  OPT ROM           = ESE-RAM 0x3C000-0x3FFFF
//   1, 2            external cartridge slots (internal ESE-SCC is off by default)
//   3-0  0000-FFFF  memory mapper     (regular openMSX MemoryMapper)
//   3-1  0000-BFFF  SUB-ROM + Kanji   = ESE-RAM 0x30000-0x3BFFF
//   3-2  4000-BFFF  ESE-RAM / MegaSD  (ASCII8 style banks of 8kB)
//   3-3  IPL mode:   0000-3FFF and C000-FFFF IPL-ROM (512 bytes, mirrored)
//        normal:     4000-7FFF XBASIC = ESE-RAM 0x28000-0x2BFFF
//
// After a reset the machine is in IPL mode: A8h=FFh and the slot 3
// expansion register is 2Bh (page 0 = 3-3, page 1/2 = 3-2, page 3 = 3-0).
// The IPL ends with OUT (A8h),C0h; any write to A8h leaves IPL mode.
//
// MegaSD (3-2), registers written at 6000-7FFF (bank0..3 for 4000, 6000,
// 8000 and A000), bit 7 = write enable (bank1 is never writable). With
// bank0 = 40h-7Fh the 4000-5FFF area is the SPI interface:
//   4000-4FFF  data, chip select low     5000-57FF  data, chip select high
//   5800-5BFF  read: status (bit 0: power-on flag), write: mode
//   5C00-5FFF  read: last received byte, write: mode
// Mode bit 7 = low speed, bit 0 = data disabled (chip select only).
// Reading a data register returns the byte received by the previous
// transfer and starts a new one. Bank0 = 60h-6Fh selects the EPCS serial
// flash instead of the SD card.
//
// When the IPL finds a C-BIOS it switches slot 0 to a primary slot (smart
// command F9h) and enables C-BIOS mode (D4h).

namespace openmsx {

static constexpr unsigned EPCS_SIZE = 0x80000; // EPCS4
static constexpr unsigned EPCS_IPL_ADDR = 0x33400;
static constexpr unsigned EPCS_IPL_SIZE = 0x00B80;
static constexpr unsigned EPCS_PROFILE_ADDR = 0x33F80;
static constexpr unsigned EPCS_PROFILE_SIZE = 0x00080;
static constexpr unsigned EPCS_EPBIOS_ADDR = 0x34000;
static constexpr unsigned EPCS_EPBIOS_SIZE = 0x4C000;

static constexpr unsigned ESE_MAIN  = 0x20000;
static constexpr unsigned ESE_XBAS  = 0x28000;
static constexpr unsigned ESE_MUSIC = 0x2C000;
static constexpr unsigned ESE_EXTD  = 0x30000;
static constexpr unsigned ESE_OPT   = 0x3C000;
static constexpr unsigned ESE_JIS1  = 0x40000;
static constexpr unsigned ESE_JIS2  = 0x60000;

static constexpr unsigned FREQ_358 = 3579545;
static constexpr unsigned FREQ_537 = 5369318;

[[nodiscard]] static bool hasRom(const DeviceConfig& config, std::string_view id)
{
	return std::ranges::any_of(config.getXML()->getChildren("rom"), [&](const auto* c) {
		return c->getAttributeValue("id", {}) == id;
	});
}

static void copyRom(DeviceConfig& config, const std::string& name, static_string_view desc,
                    std::string_view id, std::span<uint8_t> dest)
{
	Rom rom(name, desc, config, id);
	auto n = std::min<size_t>(rom.size(), dest.size());
	for (size_t i = 0; i < n; ++i) dest[i] = rom[i];
}


// class OCMEpcs

OCMEpcs::OCMEpcs()
	: data(EPCS_SIZE, 0xFF)
{
}

void OCMEpcs::deselect()
{
	command = 0;
	count = 0;
}

uint8_t OCMEpcs::transfer(uint8_t value)
{
	if (count == 0) {
		command = value;
		address = 0;
		count = 1;
		return 0xFF;
	}
	switch (command) {
	case 0x03: // READ BYTES
		if (count <= 3) {
			address = (address << 8) | value;
			++count;
			return 0xFF;
		}
		return data[address++ & (EPCS_SIZE - 1)];
	case 0x0B: // FAST READ: one dummy byte after the address
		if (count <= 4) {
			if (count <= 3) address = (address << 8) | value;
			++count;
			return 0xFF;
		}
		return data[address++ & (EPCS_SIZE - 1)];
	case 0x05: // READ STATUS: never busy, not write protected
		return 0x00;
	case 0xAB: // READ SILICON ID (after 3 dummy bytes)
		if (count <= 3) {
			++count;
			return 0xFF;
		}
		return 0x12; // EPCS4
	default: // programming commands are not supported (read-only flash)
		return 0xFF;
	}
}

template<typename Archive>
void OCMEpcs::serialize(Archive& ar, unsigned /*version*/)
{
	ar.serialize("address", address,
	             "command", command,
	             "count",   count);
}
INSTANTIATE_SERIALIZE_METHODS(OCMEpcs);


// class OCMPLD::PanaSwitch: switched I/O ID 8 (5.37MHz turbo)

OCMPLD::PanaSwitch::PanaSwitch(MSXMotherBoard& motherBoard_, OCMPLD& ocm_)
	: MSXSwitchedDevice(motherBoard_, 0x08)
	, ocm(ocm_)
{
}

byte OCMPLD::PanaSwitch::readSwitchedIO(uint16_t port, EmuTime time)
{
	return peekSwitchedIO(port, time);
}

byte OCMPLD::PanaSwitch::peekSwitchedIO(uint16_t port, EmuTime /*time*/) const
{
	switch (port & 0x0F) {
	case 0x00: return byte(~0x08);
	case 0x01: return 0xFE | (ocm.io41id008n ? 1 : 0);
	case 0x03: return 0x00;
	default:   return 0xFF;
	}
}

void OCMPLD::PanaSwitch::writeSwitchedIO(uint16_t port, byte value, EmuTime /*time*/)
{
	if ((port & 0x0F) != 0x01) return;
	if (!ocm.tPanaRedir) {
		ocm.io41id008n = value & 1; // 0 = 5.37MHz
		ocm.io42 &= ~1;
	} else {
		ocm.io41id008n = true; // custom speed instead
		ocm.io42 = (ocm.io42 & ~1) | ((value & 1) ? 0 : 1);
	}
	ocm.updateCpuSpeed();
}


// class OCMPLD

OCMPLD::OCMPLD(DeviceConfig& config)
	: MSXDevice(config)
	, MSXSwitchedDevice(getMotherBoard(), 0xD4)
	, eseRam(config, getName() + " ESE-RAM", "OCM ESE-RAM (BIOS images, MegaSD)", ESE_RAM_SIZE)
	, sdCard(std::make_unique<SdCard>(DeviceConfig(config, config.findChild("sdcard1"))))
	, opll("MSX-MUSIC", config)
	, panaSwitch(getMotherBoard(), *this)
	, dipSwitches(narrow_cast<byte>(config.getChildDataAsInt("dipswitches", 0x80)))
{
	// IPL-ROM (preloader): it loads the IPL loader from the EPCS.
	iplRom.fill(0xFF);
	copyRom(config, getName() + " IPL-ROM", "OCM IPL-ROM (preloader)", "ipl", iplRom);

	// EPCS serial flash content.
	auto& epcsData = epcs.getData();
	copyRom(config, getName() + " IPL loader", "OCM IPL loader (EPCS)", "iplloader",
	        std::span{&epcsData[EPCS_IPL_ADDR], EPCS_IPL_SIZE});
	if (hasRom(config, "profile")) {
		copyRom(config, getName() + " smart profile", "OCM IGX smart profile (EPCS)", "profile",
		        std::span{&epcsData[EPCS_PROFILE_ADDR], EPCS_PROFILE_SIZE});
	}
	if (hasRom(config, "epbios")) {
		copyRom(config, getName() + " EPBIOS", "OCM EPBIOS (EPCS)", "epbios",
		        std::span{&epcsData[EPCS_EPBIOS_ADDR], EPCS_EPBIOS_SIZE});
	}
}

OCMPLD::~OCMPLD() = default;

void OCMPLD::powerUp(EmuTime time)
{
	// The FPGA reset sequence clears the start of ESE-RAM, so the IPL's
	// "AB" check fails and the BIOS gets loaded from the SD card.
	eseRam.clear(0x00);
	powerOnFlag = true;
	jis2Enabled = false;
	portF4Mode = false;
	reset(time);
}

void OCMPLD::reset(EmuTime time)
{
	// MegaSD
	bank = {0, 0, 0, 0};
	spiMode = 0;
	spiLast = 0xFF;
	epcs.deselect();

	kanjiAddr1 = 0;
	kanjiAddr2 = 0;

	// The reset button is a cold reset for the switched I/O logic.
	io41 = 0xFF;
	io42 = dipSwitches;
	io43 = 0;
	io44 = 0;
	psgVol = 4; masterVol = 0; sccVol = 4; opllVol = 4;
	customSpeed = 2;
	tPanaRedir = false;
	tMegaSD = true;
	io41id008n = true;
	lastResetWarm = false;
	megaSdRequest = megaSdEnabled = (dipSwitches & 0x80) != 0;

	portF4Bit7 = !portF4Mode;

	opll.reset(time);

	cbiosMode = false;
	setSlot0Expanded(true);

	// The OCM has no real 8255: port A is always an output and resets to
	// FFh, and the slot 3 expansion register resets to 2Bh.
	auto& cpuInterface = getCPUInterface();
	ignoreSlotWrite = true;
	cpuInterface.writeIO(0xAB, 0x82, time);
	cpuInterface.writeIO(0xA8, 0xFF, time);
	ignoreSlotWrite = false;
	cpuInterface.writeMem(0xFFFF, 0x2B, time);

	iplMode = true;
	invalidateAllSlots();
	updateCpuSpeed();
}

void OCMPLD::setIplMode(bool ipl, EmuTime /*time*/)
{
	if (iplMode == ipl) return;
	iplMode = ipl;
	invalidateSlots(Window::IPL);
	invalidateSlots(Window::MEGASD);
	updateCpuSpeed();
}

void OCMPLD::setSlot0Expanded(bool expanded)
{
	if (slot0Expanded != expanded) {
		slot0Expanded = expanded;
		invalidateAllSlots();
	}
	getCPUInterface().setExpanderHidden(0, !slot0Expanded);
}

void OCMPLD::updateCpuSpeed()
{
	unsigned freq = FREQ_358;
	if (iplMode) {
		freq = 8060000; // custom speed #7
	} else if (io42 & 1) {
		static constexpr std::array<unsigned, 9> customFreq = {
			8060000, 8060000, 8060000, 6960000, 6100000,
			5390000, 4900000, 4480000, 4100000,
		};
		freq = customFreq[std::min<unsigned>(customSpeed, 8)];
	} else if (!io41id008n) {
		freq = FREQ_537;
	}
	getCPU().setZ80Freq(freq);
	getLedStatus().setLed(LedStatus::TURBO, freq != FREQ_358);
}

void OCMPLD::registerSlot(OCMSlot& slot)
{
	slots.push_back(&slot);
}

void OCMPLD::unregisterSlot(OCMSlot& slot)
{
	std::erase(slots, &slot);
}

void OCMPLD::invalidateSlots(Window w)
{
	for (auto* s : slots) {
		if (s->getWindow() == w) s->invalidateDeviceRWCache();
	}
}

void OCMPLD::invalidateAllSlots()
{
	for (auto* s : slots) s->invalidateDeviceRWCache();
}


// I/O ports

byte OCMPLD::readIO(uint16_t port, EmuTime time)
{
	switch (port & 0xFF) {
	case 0xD9: {
		byte result = eseRam[ESE_JIS1 + kanjiAddr1];
		kanjiAddr1 = (kanjiAddr1 & ~0x1Fu) | ((kanjiAddr1 + 1) & 0x1F);
		return result;
	}
	case 0xDB: {
		if (!jis2Enabled) return 0xFF;
		byte result = eseRam[ESE_JIS2 + kanjiAddr2];
		kanjiAddr2 = (kanjiAddr2 & ~0x1Fu) | ((kanjiAddr2 + 1) & 0x1F);
		return result;
	}
	default:
		return peekIO(port, time);
	}
}

byte OCMPLD::peekIO(uint16_t port, EmuTime /*time*/) const
{
	switch (port & 0xFF) {
	case 0xD9:
		return eseRam[ESE_JIS1 + kanjiAddr1];
	case 0xDB:
		return jis2Enabled ? eseRam[ESE_JIS2 + kanjiAddr2] : 0xFF;
	case 0xF4:
		return (portF4Bit7 ? 0x80 : 0x00) | (portF4Mode ? 0x00 : 0x7F);
	default:
		return 0xFF;
	}
}

void OCMPLD::writeIO(uint16_t port, byte value, EmuTime time)
{
	switch (port & 0xFF) {
	case 0xA8:
		if (!ignoreSlotWrite) setIplMode(false, time);
		break;
	case 0x7C:
	case 0x7D:
		// the internal OPLL is disabled when slot 0 is a primary slot
		if (slot0Expanded || cbiosMode) opll.writePort(port & 1, value, time);
		break;
	case 0xD8: // pointer bits 10-5, clears bits 4-0
		kanjiAddr1 = (kanjiAddr1 & 0x1F800) | ((value & 0x3F) << 5);
		break;
	case 0xD9: // pointer bits 16-11, clears bits 4-0
		kanjiAddr1 = (kanjiAddr1 & 0x007E0) | ((value & 0x3F) << 11);
		break;
	case 0xDA:
		kanjiAddr2 = (kanjiAddr2 & 0x1F800) | ((value & 0x3F) << 5);
		break;
	case 0xDB:
		kanjiAddr2 = (kanjiAddr2 & 0x007E0) | ((value & 0x3F) << 11);
		break;
	case 0xF4:
		portF4Bit7 = (value & 0x80) != 0;
		break;
	default:
		break;
	}
}


// switched I/O, ID 212 (D4h)

byte OCMPLD::readSwitchedIO(uint16_t port, EmuTime time)
{
	return peekSwitchedIO(port, time);
}

byte OCMPLD::peekSwitchedIO(uint16_t port, EmuTime /*time*/) const
{
	auto b = [](bool x, int bit) { return byte(x ? (1 << bit) : 0); };
	switch (port & 0x0F) {
	case 0x0: return byte(~0xD4);
	case 0x1: return io41;
	case 0x2: return io42;
	case 0x3: return io43;
	case 0x4: return io44;
	case 0x5: return b(masterVol == 7, 7) | byte((~masterVol & 7) << 4)
	               | b(psgVol == 0, 3) | psgVol;
	case 0x6: return b(sccVol == 0, 7) | byte(sccVol << 4)
	               | b(opllVol == 0, 3) | opllVol;
	case 0x7: return b(megaSdRequest, 7) | b(io42 & 0x40, 6) | b(tPanaRedir, 4)
	               | b(tMegaSD, 3) | byte((1 - customSpeed) & 7);
	case 0x8: return b(true, 7) | b(lastResetWarm, 5) | b(!io41id008n, 4)
	               | b(true, 1) | b(!io41id008n, 0);
	case 0x9: return 0x40; // NTSC/PAL auto, machine ID 0 (1chipMSX)
	case 0xA: return 0x20; // legacy output = VGA+
	case 0xB:
		switch (io44) {
		case 0:  return 0x90; // slot 0 expanded, SDRAM 32MB
		case 1:  return 0x7E; // default vertical offset
		case 2:  return b(slot0Expanded, 5) | b(cbiosMode, 3) | 0x04;
		default: return 0xFF;
		}
	case 0xC: return dipSwitches;
	case 0xD: return 0x10; // VRAM slot IDs
	case 0xE: return 0x27; // OCM-PLD v3.9
	case 0xF: return 0xCC; // non-JP keyboard, v3.9.2, switched I/O revision 12
	default:  return 0xFF;
	}
}

void OCMPLD::writeSwitchedIO(uint16_t port, byte value, EmuTime time)
{
	switch (port & 0x0F) {
	case 0x1:
		io41 = byte(~value);
		smartCommand(value, time);
		break;
	case 0x2: // virtual DIP-SW (inverted); bits 6-7 are requests for the next reset
		io42 = (io42 & 0xC0) | (~value & 0x3F);
		megaSdRequest = !(value & 0x80);
		updateCpuSpeed();
		break;
	case 0x3: io43 = byte(~value); break;
	case 0x4: io44 = byte(~value); break;
	case 0x5:
		psgVol = ~value & 7;
		masterVol = (value >> 4) & 7;
		break;
	case 0x6:
		opllVol = ~value & 7;
		sccVol = (~value >> 4) & 7;
		break;
	case 0xC: // IPL mode only: VDP ID (V9938/V9958), not emulated
		break;
	case 0xE: // IPL mode only: JIS2 enable
		if (iplMode) jis2Enabled = !(value & 0x80);
		break;
	case 0xF: // IPL mode only: F4h device mode
		if (iplMode) portF4Mode = !(value & 0x80);
		break;
	default:
		break;
	}
}

void OCMPLD::smartCommand(byte cmd, EmuTime /*time*/)
{
	switch (cmd) {
	case 0x01: tPanaRedir = false; break;
	case 0x02: tPanaRedir = true; break;
	case 0x03: // 3.58MHz
		io41id008n = true;
		io42 &= ~1;
		break;
	case 0x04: case 0x05: case 0x06: case 0x07:
	case 0x08: case 0x09: case 0x0A: // custom speed #1-#7
		io41id008n = true;
		io42 |= 1;
		customSpeed = byte(8 - (cmd - 0x04));
		break;
	case 0x0B: tMegaSD = false; break;
	case 0x0C: tMegaSD = true; break;
	case 0x1D: case 0x1E: // MegaSD off/on
		megaSdRequest = cmd == 0x1E;
		if (iplMode) {
			megaSdEnabled = megaSdRequest;
			invalidateSlots(Window::MEGASD);
		}
		break;
	case 0xD4: // C-BIOS mode (IPL mode only)
		if (iplMode) {
			cbiosMode = true;
			invalidateSlots(Window::MAIN);
		}
		break;
	case 0xF9: case 0xFA: // slot 0 primary/expanded, immediate in IPL mode
		if (iplMode) setSlot0Expanded(cmd == 0xFA);
		break;
	default:
		// other smart commands (video, audio, slot and reset settings)
		// are not emulated
		break;
	}
	updateCpuSpeed();
}


// slot windows

bool OCMPLD::isEpcsMode() const
{
	return (bank[0] & 0xF0) == 0x60;
}

bool OCMPLD::isSpiWindow(uint16_t address) const
{
	return ((bank[0] & 0xC0) == 0x40) && (0x4000 <= address) && (address < 0x6000);
}

bool OCMPLD::megaSdReadable() const
{
	return iplMode || megaSdEnabled;
}

const byte* OCMPLD::megaSdLine(uint16_t address) const
{
	if (!megaSdReadable()) return unmappedRead.data();
	unsigned b = bank[(address >> 13) - 2] & 0x7F;
	return &eseRam[(b << 13) | (address & 0x1FFF)];
}

byte OCMPLD::spiTransfer(byte value, bool csHigh)
{
	if (isEpcsMode()) {
		// SD card chip select stays high
		if (csHigh) {
			epcs.deselect();
			return 0xFF;
		}
		return epcs.transfer(value);
	} else {
		epcs.deselect();
		return sdCard->transfer(value, csHigh);
	}
}

void OCMPLD::spiChipSelectOnly(bool csHigh)
{
	if (!isEpcsMode() || csHigh) epcs.deselect();
}

byte OCMPLD::readSpi(uint16_t address)
{
	if (address < 0x5800) {
		bool csHigh = (address & 0x1000) != 0;
		byte result = spiLast;
		if (spiMode & 0x01) {
			spiChipSelectOnly(csHigh);
		} else {
			spiLast = spiTransfer(0xFF, csHigh);
		}
		return result;
	}
	return peekSpi(address);
}

byte OCMPLD::peekSpi(uint16_t address) const
{
	if (address < 0x5800) return spiLast;
	if (address < 0x5C00) return powerOnFlag ? 0x01 : 0x00; // never busy
	return spiLast;
}

void OCMPLD::writeSpi(uint16_t address, byte value)
{
	if (address < 0x5800) {
		bool csHigh = (address & 0x1000) != 0;
		if (spiMode & 0x01) {
			spiChipSelectOnly(csHigh);
		} else {
			spiLast = spiTransfer(value, csHigh);
		}
	} else {
		spiMode = value;
		powerOnFlag = false;
	}
}

const byte* OCMPLD::getReadCacheLineWindow(Window w, uint16_t start) const
{
	switch (w) {
	case Window::IPL:
		if (iplMode) {
			if ((start < 0x4000) || (start >= 0xC000)) {
				return &iplRom[start & (IPL_SIZE - 1)];
			}
		} else if ((0x4000 <= start) && (start < 0x8000)) {
			return &eseRam[ESE_XBAS + (start & 0x3FFF)];
		}
		break;
	case Window::MEGASD:
		if ((0x4000 <= start) && (start < 0xC000)) {
			if (isSpiWindow(start)) return nullptr;
			return megaSdLine(start);
		}
		break;
	case Window::MAIN:
		if (start < 0x8000) return &eseRam[ESE_MAIN + start];
		if (cbiosMode && (start < 0xC000)) return &eseRam[ESE_OPT + (start & 0x3FFF)];
		break;
	case Window::MUSIC:
		if ((0x4000 <= start) && (start < 0x8000)) {
			return &eseRam[ESE_MUSIC + (start & 0x3FFF)];
		}
		break;
	case Window::OPT:
		if ((0x4000 <= start) && (start < 0x8000)) {
			return &eseRam[ESE_OPT + (start & 0x3FFF)];
		}
		break;
	case Window::EXTD:
		if (start < 0xC000) return &eseRam[ESE_EXTD + start];
		break;
	}
	return unmappedRead.data();
}

byte* OCMPLD::getWriteCacheLineWindow(Window w, uint16_t start)
{
	if (w != Window::MEGASD) return unmappedWrite.data();
	if ((start < 0x4000) || (start >= 0xC000)) return unmappedWrite.data();
	if ((0x6000 <= start) && (start < 0x8000)) return nullptr; // bank registers
	if (isSpiWindow(start)) return nullptr;
	byte b = bank[(start >> 13) - 2];
	if (!(b & 0x80)) return unmappedWrite.data();
	return &eseRam[((b & 0x7F) << 13) | (start & 0x1FFF)];
}

byte OCMPLD::readWindow(Window w, uint16_t address, EmuTime /*time*/)
{
	if ((w == Window::MEGASD) && isSpiWindow(address)) {
		return readSpi(address);
	}
	return peekWindow(w, address);
}

byte OCMPLD::peekWindow(Window w, uint16_t address) const
{
	if ((w == Window::MEGASD) && isSpiWindow(address)) {
		return peekSpi(address);
	}
	const byte* line = getReadCacheLineWindow(w, address & CacheLine::HIGH);
	assert(line);
	return line[address & CacheLine::LOW];
}

void OCMPLD::writeWindow(Window w, uint16_t address, byte value, EmuTime /*time*/)
{
	if (w != Window::MEGASD) return;
	if ((address < 0x4000) || (address >= 0xC000)) return;
	if ((0x6000 <= address) && (address < 0x8000)) {
		unsigned idx = (address >> 11) & 3;
		byte newValue = (idx == 1) ? (value & 0x7F) : value;
		if (bank[idx] != newValue) {
			bank[idx] = newValue;
			invalidateSlots(Window::MEGASD);
		}
		return;
	}
	if (isSpiWindow(address)) {
		writeSpi(address, value);
		return;
	}
	byte b = bank[(address >> 13) - 2];
	if (b & 0x80) {
		eseRam[((b & 0x7F) << 13) | (address & 0x1FFF)] = value;
	}
}


// version 1: initial version
template<typename Archive>
void OCMPLD::serialize(Archive& ar, unsigned /*version*/)
{
	ar.template serializeBase<MSXDevice>(*this);
	ar.serialize("eseRam",        eseRam,
	             "sdCard",        *sdCard,
	             "epcs",          epcs,
	             "opll",          opll,
	             "iplMode",       iplMode,
	             "megaSdEnabled", megaSdEnabled,
	             "megaSdRequest", megaSdRequest,
	             "slot0Expanded", slot0Expanded,
	             "cbiosMode",     cbiosMode,
	             "bank",          bank,
	             "spiMode",       spiMode,
	             "spiLast",       spiLast,
	             "powerOnFlag",   powerOnFlag,
	             "kanjiAddr1",    kanjiAddr1,
	             "kanjiAddr2",    kanjiAddr2,
	             "jis2Enabled",   jis2Enabled,
	             "portF4Bit7",    portF4Bit7,
	             "portF4Mode",    portF4Mode);
	ar.serialize("io41",          io41,
	             "io42",          io42,
	             "io43",          io43,
	             "io44",          io44,
	             "psgVol",        psgVol,
	             "masterVol",     masterVol,
	             "sccVol",        sccVol,
	             "opllVol",       opllVol,
	             "customSpeed",   customSpeed,
	             "tPanaRedir",    tPanaRedir,
	             "tMegaSD",       tMegaSD,
	             "lastResetWarm", lastResetWarm,
	             "io41id008n",    io41id008n);
	if constexpr (Archive::IS_LOADER) {
		getCPUInterface().setExpanderHidden(0, !slot0Expanded);
		invalidateAllSlots();
		updateCpuSpeed();
	}
}
INSTANTIATE_SERIALIZE_METHODS(OCMPLD);
REGISTER_MSXDEVICE(OCMPLD, "OCMPLD");


// class OCMSlot

[[nodiscard]] static OCMPLD::Window parseWindow(std::string_view s)
{
	using enum OCMPLD::Window;
	if (s == "ipl")    return IPL;
	if (s == "megasd") return MEGASD;
	if (s == "main")   return MAIN;
	if (s == "music")  return MUSIC;
	if (s == "opt")    return OPT;
	if (s == "extd")   return EXTD;
	throw MSXException("Unknown OCM slot window: ", s,
		" (expected ipl, megasd, main, music, opt or extd)");
}

OCMSlot::OCMSlot(DeviceConfig& config)
	: MSXDevice(config)
	, window(parseWindow(config.getChildData("window")))
{
}

OCMSlot::~OCMSlot()
{
	if (ocm) ocm->unregisterSlot(*this);
}

void OCMSlot::init()
{
	MSXDevice::init();
	const auto& refs = getReferences();
	ocm = refs.empty() ? nullptr : dynamic_cast<OCMPLD*>(refs.front());
	if (!ocm) {
		throw MSXException("OCMSlot \"", getName(), "\" needs a reference to an OCMPLD device");
	}
	ocm->registerSlot(*this);
}

byte OCMSlot::readMem(uint16_t address, EmuTime time)
{
	return ocm->readWindow(window, address, time);
}

byte OCMSlot::peekMem(uint16_t address, EmuTime /*time*/) const
{
	return ocm->peekWindow(window, address);
}

void OCMSlot::writeMem(uint16_t address, byte value, EmuTime time)
{
	ocm->writeWindow(window, address, value, time);
}

const byte* OCMSlot::getReadCacheLine(uint16_t start) const
{
	return ocm->getReadCacheLineWindow(window, start);
}

byte* OCMSlot::getWriteCacheLine(uint16_t start)
{
	return ocm->getWriteCacheLineWindow(window, start);
}

template<typename Archive>
void OCMSlot::serialize(Archive& ar, unsigned /*version*/)
{
	ar.template serializeBase<MSXDevice>(*this);
}
INSTANTIATE_SERIALIZE_METHODS(OCMSlot);
REGISTER_MSXDEVICE(OCMSlot, "OCMSlot");

} // namespace openmsx
