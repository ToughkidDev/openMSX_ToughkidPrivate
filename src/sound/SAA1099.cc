#include "SAA1099.hh"

#include "DeviceConfig.hh"
#include "serialize.hh"

#include <algorithm>

namespace openmsx {

// The SAA1099 internally updates its generators at clock / 256.  SoundStar
// supplies an 8 MHz clock, yielding a 31.25 kHz native stream.
static constexpr unsigned NATIVE_FREQ = 8'000'000 / 256;

SAA1099::SAA1099(const DeviceConfig& config)
	: ResampledSoundDevice(config.getMotherBoard(), "SoundStar-SAA1099",
	                       "SAA1099", 6, NATIVE_FREQ, true)
{
	resetState();
	registerSound(config);
}

SAA1099::~SAA1099()
{
	unregisterSound();
}

void SAA1099::resetState()
{
	regs.fill(0);
	toneCounter.fill(1);
	toneLevel.fill(false);
	noiseCounter.fill(1);
	noiseLfsr.fill(0x1ffff);
	envelopeStep.fill(0);
	envelopeUp.fill(false);
	soundEnabled = false;
}

void SAA1099::reset(EmuTime time)
{
	updateStream(time);
	resetState();
}

unsigned SAA1099::tonePeriod(unsigned channel) const
{
	auto octave = (regs[0x10 + channel / 2] >> ((channel & 1) * 4)) & 7;
	// The oscillator frequency doubles for every octave step. At the native
	// clock rate this is (512 - frequency) / 2^octave samples per half-wave.
	return std::max(1u, (512u - regs[0x08 + channel]) >> octave);
}

unsigned SAA1099::noisePeriod(unsigned noise) const
{
	auto frequency = (regs[0x16] >> (noise * 4)) & 3;
	if (frequency == 3) {
		// Noise source 0/1 follows tone channel 2/5 respectively.
		return tonePeriod(noise ? 5 : 2);
	}
	// Fixed noise frequencies are clock/128, clock/256 and clock/512.
	// At our clock/256 sample rate these correspond to two, one and one half
	// LFSR steps per sample. The first two intentionally alias to one sampled
	// step; stepping at a lower rate would audibly change the SoundStar noise.
	return frequency == 2 ? 2 : 1;
}

unsigned SAA1099::envelopeVolume(unsigned group) const
{
	return envelopeStep[group] & 15;
}

void SAA1099::stepEnvelope(unsigned group)
{
	const auto control = regs[0x18 + group];
	if (!(control & 0x80) || (control & 0x20)) return;

	auto& step = envelopeStep[group];
	auto& up = envelopeUp[group];
	auto mode = control & 7;
	switch (mode) {
	case 0: // one-shot decay
		if (step) --step;
		break;
	case 1: // repeating decay
		step = (step - 1) & 15;
		break;
	case 2: // one-shot triangle
		if (up) {
			if (step < 15) ++step; else up = false;
		} else if (step) {
			--step;
		}
		break;
	case 3: // repeating triangle
		if (up) {
			if (step < 15) ++step; else { up = false; --step; }
		} else {
			if (step) --step; else { up = true; ++step; }
		}
		break;
	case 4: // one-shot rise
		if (step < 15) ++step;
		break;
	case 5: // repeating rise
		step = (step + 1) & 15;
		break;
	case 6: // one-shot sawtooth
		if (step) --step;
		break;
	case 7: // repeating sawtooth
		step = (step - 1) & 15;
		break;
	}
}

void SAA1099::writeRegister(unsigned reg, uint8_t value, EmuTime time)
{
	reg &= 31;
	if (reg == 0x1c) {
		// Bit 0 is the global sound enable, bit 1 synchronously resets the
		// waveform generators.  The latter is used by software before playback.
		updateStream(time);
		regs[reg] = value;
		soundEnabled = value & 1;
		if (value & 2) {
			toneCounter.fill(1);
			toneLevel.fill(false);
			noiseCounter.fill(1);
			noiseLfsr.fill(0x1ffff);
		}
		return;
	}
	if (regs[reg] != value) {
		updateStream(time);
		regs[reg] = value;
		if (reg == 0x18 || reg == 0x19) {
			auto group = reg & 1;
			envelopeStep[group] = 15;
			envelopeUp[group] = false;
		}
	}
}

void SAA1099::generateChannels(std::span<float*> buffers, unsigned num)
{
	for (unsigned sample = 0; sample < num; ++sample) {
		std::array<bool, 6> rising{};
		for (unsigned ch = 0; ch < 6; ++ch) {
			if (--toneCounter[ch] == 0) {
				toneCounter[ch] = tonePeriod(ch);
				toneLevel[ch] = !toneLevel[ch];
				rising[ch] = toneLevel[ch];
			}
		}
		for (unsigned noise = 0; noise < 2; ++noise) {
			if (--noiseCounter[noise] == 0) {
				noiseCounter[noise] = noisePeriod(noise);
				// 17-bit polynomial x^17 + x^14 + 1.
				auto feedback = (noiseLfsr[noise] ^ (noiseLfsr[noise] >> 3)) & 1;
				noiseLfsr[noise] = (noiseLfsr[noise] >> 1) | (feedback << 16);
			}
		}
		if (rising[2]) stepEnvelope(0);
		if (rising[5]) stepEnvelope(1);

		if (!soundEnabled) continue;
		for (unsigned ch = 0; ch < 6; ++ch) {
			auto* buffer = buffers[ch];
			if (!buffer) continue;
			auto tone = (regs[0x14] >> ch) & 1;
			auto noise = (regs[0x15] >> ch) & 1;
			auto noiseLevel = noiseLfsr[ch / 3] & 1;
			if (!((tone && toneLevel[ch]) || (noise && noiseLevel))) continue;

			auto amplitude = regs[ch];
			auto left = amplitude & 15;
			auto right = amplitude >> 4;
			const auto group = ch / 3;
			const auto env = regs[0x18 + group];
			if (env & 0x80) {
				left = envelopeVolume(group);
				right = (env & 1) ? (15 - left) : left;
			}
			// Keep the sum of all six outputs below the normal mixer range.
			buffer[2 * sample + 0] += float(left) / 90.0f;
			buffer[2 * sample + 1] += float(right) / 90.0f;
		}
	}
}

float SAA1099::getAmplificationFactorImpl() const
{
	// generateChannels() already produces normalized 0.0..1.0 samples.
	// SoundDevice's default assumes signed 16-bit sample units and would make
	// this output 32768 times too quiet.
	return 1.0f;
}

template<typename Archive>
void SAA1099::serialize(Archive& ar, unsigned /*version*/)
{
	ar.serialize("regs", regs,
	             "toneCounter", toneCounter,
	             "toneLevel", toneLevel,
	             "noiseCounter", noiseCounter,
	             "noiseLfsr", noiseLfsr,
	             "envelopeStep", envelopeStep,
	             "envelopeUp", envelopeUp,
	             "soundEnabled", soundEnabled);
}
INSTANTIATE_SERIALIZE_METHODS(SAA1099);

} // namespace openmsx
