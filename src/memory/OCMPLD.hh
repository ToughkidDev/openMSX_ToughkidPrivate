#ifndef OCMPLD_HH
#define OCMPLD_HH

// Emulation of the 1chipMSX running the OCM-PLD firmware (esemsx3, 304k IPL).
//
// The real machine boots from a small IPL ROM that loads the BIOS images
// (OCM-BIOS.DAT) from the SD card into ESE-RAM. All system ROMs (MAIN, SUB,
// MSX-MUSIC, Kanji BASIC, ...) are then just views into that ESE-RAM. To boot
// the original IPL and BIOS unmodified this is modeled as:
//
//  - OCMPLD: one core device (not in a slot) owning the ESE-RAM, the MegaSD
//    SPI interface (SD card + EPCS serial flash), the IPL/normal mode state,
//    the OCM switched I/O ports (ID 212 / D4h, plus ID 8 for the 5.37MHz
//    turbo), the kanji font ports, the OPLL and the F4h device.
//  - OCMSlot: thin devices placed in the (sub)slots; each one shows one
//    "window" of the core: the IPL ROM / XBASIC (3-3), the MegaSD (3-2), the
//    MAIN ROM (0-0), MSX-MUSIC ROM (0-2), OPT ROM (0-3) and SUB-ROM + Kanji
//    BASIC (3-1).
//
// Reference: https://github.com/ToughkidDev/ocm-pld-dev (esemsx3/src).

#include "MSXDevice.hh"
#include "MSXSwitchedDevice.hh"
#include "Ram.hh"
#include "Rom.hh"
#include "SdCard.hh"
#include "YM2413.hh"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace openmsx {

class OCMSlot;

class OCMEpcs
{
public:
	OCMEpcs();
	[[nodiscard]] std::vector<uint8_t>& getData() { return data; }
	uint8_t transfer(uint8_t value);
	void deselect();

	template<typename Archive>
	void serialize(Archive& ar, unsigned version);

private:
	std::vector<uint8_t> data; // 512kB EPCS4 content (not serialized)
	uint32_t address = 0;
	uint8_t command = 0;
	uint8_t count = 0; // number of bytes received in this transaction
};

class OCMPLD final : public MSXDevice, public MSXSwitchedDevice
{
public:
	enum class Window : uint8_t { IPL, MEGASD, MAIN, MUSIC, OPT, EXTD };

	explicit OCMPLD(DeviceConfig& config);
	~OCMPLD() override;

	void powerUp(EmuTime time) override;
	void reset(EmuTime time) override;

	[[nodiscard]] byte readIO(uint16_t port, EmuTime time) override;
	[[nodiscard]] byte peekIO(uint16_t port, EmuTime time) const override;
	void writeIO(uint16_t port, byte value, EmuTime time) override;

	// MSXSwitchedDevice (ID 212)
	[[nodiscard]] byte readSwitchedIO(uint16_t port, EmuTime time) override;
	[[nodiscard]] byte peekSwitchedIO(uint16_t port, EmuTime time) const override;
	void writeSwitchedIO(uint16_t port, byte value, EmuTime time) override;

	// interface for the slot windows
	void registerSlot(OCMSlot& slot);
	void unregisterSlot(OCMSlot& slot);
	[[nodiscard]] byte readWindow(Window w, uint16_t address, EmuTime time);
	[[nodiscard]] byte peekWindow(Window w, uint16_t address) const;
	void writeWindow(Window w, uint16_t address, byte value, EmuTime time);
	[[nodiscard]] const byte* getReadCacheLineWindow(Window w, uint16_t start) const;
	[[nodiscard]] byte* getWriteCacheLineWindow(Window w, uint16_t start);

	template<typename Archive>
	void serialize(Archive& ar, unsigned version);

private:
	class PanaSwitch final : public MSXSwitchedDevice
	{
	public:
		PanaSwitch(MSXMotherBoard& motherBoard, OCMPLD& ocm);
		[[nodiscard]] byte readSwitchedIO(uint16_t port, EmuTime time) override;
		[[nodiscard]] byte peekSwitchedIO(uint16_t port, EmuTime time) const override;
		void writeSwitchedIO(uint16_t port, byte value, EmuTime time) override;
	private:
		OCMPLD& ocm;
	};

	[[nodiscard]] bool isSpiWindow(uint16_t address) const;
	[[nodiscard]] bool isEpcsMode() const;
	[[nodiscard]] byte readSpi(uint16_t address);
	[[nodiscard]] byte peekSpi(uint16_t address) const;
	void writeSpi(uint16_t address, byte value);
	byte spiTransfer(byte value, bool csHigh);
	void spiChipSelectOnly(bool csHigh);

	[[nodiscard]] const byte* megaSdLine(uint16_t address) const;
	[[nodiscard]] byte megaSdRead(uint16_t address) const;
	[[nodiscard]] bool megaSdReadable() const;

	void setIplMode(bool ipl, EmuTime time);
	void setSlot0Expanded(bool expanded);
	void smartCommand(byte cmd, EmuTime time);
	void updateCpuSpeed();
	void invalidateSlots(Window w);
	void invalidateAllSlots();

private:
	static constexpr unsigned ESE_RAM_SIZE = 0x100000; // 1MB
	static constexpr unsigned IPL_SIZE = 0x200; // preloader, mirrored every 512 bytes

	Ram eseRam;
	std::array<byte, IPL_SIZE> iplRom;
	std::unique_ptr<SdCard> sdCard;
	OCMEpcs epcs;
	YM2413 opll;
	PanaSwitch panaSwitch;
	std::vector<OCMSlot*> slots;

	const byte dipSwitches; // physical DIP-SW, bit n = DIP n+1 is ON

	// IPL / MegaSD state
	bool iplMode = true;          // ff_ldbios_n = 0
	bool ignoreSlotWrite = false; // our own A8h write during reset
	bool megaSdEnabled = true;    // MegaSD_ack
	bool megaSdRequest = true;    // MegaSD_req
	bool slot0Expanded = true;    // Slot0Mode (false: slot 0 is a primary slot)
	bool cbiosMode = false;       // C-BIOS: OPT ROM also at 8000-BFFF in slot 0
	std::array<byte, 4> bank = {0, 0, 0, 0};
	byte spiMode = 0;             // bit7: low speed, bit0: data disabled
	byte spiLast = 0xFF;          // byte received by the previous transfer
	bool powerOnFlag = true;      // cleared by any write to the mode register

	// kanji
	unsigned kanjiAddr1 = 0;
	unsigned kanjiAddr2 = 0;
	bool jis2Enabled = false;

	// F4h device
	bool portF4Bit7 = true;
	bool portF4Mode = false;      // true = turboR (Z80 mode) style

	// switched I/O ID 212
	byte io41 = 0xFF;             // complement of the last smart command
	byte io42 = 0;                // virtual DIP-SW
	byte io43 = 0;                // lock mask
	byte io44 = 0;                // lights mask / dynamic port 4Bh ID
	byte psgVol = 4, masterVol = 0, sccVol = 4, opllVol = 4;
	byte customSpeed = 2;         // 8..2 = 4.10..8.06MHz
	bool tPanaRedir = false;
	bool tMegaSD = true;
	bool lastResetWarm = false;   // LastRst_ack

	// switched I/O ID 8
	bool io41id008n = true;       // false = 5.37MHz
};

class OCMSlot final : public MSXDevice
{
public:
	explicit OCMSlot(DeviceConfig& config);
	~OCMSlot() override;

	void init() override;

	[[nodiscard]] byte readMem(uint16_t address, EmuTime time) override;
	[[nodiscard]] byte peekMem(uint16_t address, EmuTime time) const override;
	void writeMem(uint16_t address, byte value, EmuTime time) override;
	[[nodiscard]] const byte* getReadCacheLine(uint16_t start) const override;
	[[nodiscard]] byte* getWriteCacheLine(uint16_t start) override;

	[[nodiscard]] OCMPLD::Window getWindow() const { return window; }

	template<typename Archive>
	void serialize(Archive& ar, unsigned version);

private:
	OCMPLD* ocm = nullptr;
	OCMPLD::Window window;
};

} // namespace openmsx

#endif
