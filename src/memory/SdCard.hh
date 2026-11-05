#ifndef SDCARD_HH
#define SDCARD_HH

#include "DiskImageUtils.hh"

#include "circular_buffer.hh"

#include <array>
#include <cstdint>
#include <memory>

namespace openmsx {

class DeviceConfig;
class HD;

class SdCard
{
public:
	explicit SdCard(const DeviceConfig& config);
	~SdCard();

	uint8_t transfer(uint8_t value, bool cs);

	[[nodiscard]] bool isPresent() const;
	[[nodiscard]] bool isWriteProtected() const;

	/** Number of bytes between the CRC of a written block and the data
	  * response token. Real cards send the token right after the CRC (0),
	  * the default of 1 is what the existing openMSX devices were tuned
	  * for. */
	void setWriteResponseDelay(uint8_t delay) { writeResponseDelay = delay; }

	template<typename Archive>
	void serialize(Archive& ar, unsigned version);

// private:
	enum class Mode : uint8_t {
		COMMAND,
		READ,
		MULTI_READ,
		WRITE,
		MULTI_WRITE
	};

private:
	void executeCommand();
	[[nodiscard]] uint8_t readCurrentByteFromCurrentSector();

private:
	const std::unique_ptr<HD> hd; // can be nullptr

	std::array<uint8_t, 6> cmdBuf;
	SectorBuffer sectorBuf;
	unsigned cmdIdx = 0;

	cb_queue<uint8_t> responseQueue;

	uint8_t transferDelayCounter = 0;
	uint8_t writeResponseDelay = 1; // not serialized: set by the owning device
	Mode mode = Mode::COMMAND;
	unsigned currentSector = 0;
	int currentByteInSector = 0;
};

} // namespace openmsx

#endif
