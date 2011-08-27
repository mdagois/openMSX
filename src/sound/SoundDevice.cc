// $Id$

#include "SoundDevice.hh"
#include "MSXMixer.hh"
#include "XMLElement.hh"
#include "WavWriter.hh"
#include "Filename.hh"
#include "StringOp.hh"
#include "HostCPU.hh"
#include "MemoryOps.hh"
#include "MSXException.hh"
#include "aligned.hh"
#include "vla.hh"
#include "unreachable.hh"
#include "build-info.hh"
#include <cstring>
#include <cassert>
#include <set>

using std::string;
using std::set;

namespace openmsx {

static const unsigned MAX_FACTOR = 16; // 200kHz (PSG) -> 22kHz
static const unsigned MAX_SAMPLES = 8192 * MAX_FACTOR;
ALIGNED(static int mixBuffer[SoundDevice::MAX_CHANNELS * MAX_SAMPLES * 2], 16); // align for SSE access

static string makeUnique(MSXMixer& mixer, const string& name)
{
	string result = name;
	if (mixer.findDevice(result)) {
		unsigned n = 0;
		do {
			result = StringOp::Builder() << name << " (" << ++n << ')';
		} while (mixer.findDevice(result));
	}
	return result;
}

SoundDevice::SoundDevice(MSXMixer& mixer_, const string& name_,
			 const string& description_,
			 unsigned numChannels_, bool stereo_)
	: mixer(mixer_)
	, name(makeUnique(mixer, name_))
	, description(description_)
	, numChannels(numChannels_)
	, stereo(stereo_ ? 2 : 1)
	, numRecordChannels(0)
	, balanceCenter(true)
{
	assert(numChannels <= MAX_CHANNELS);
	assert(stereo == 1 || stereo == 2);

	// initially no channels are muted
	for (unsigned i = 0; i < numChannels; ++i) {
		channelMuted[i] = false;
		channelBalance[i] = 0;
	}
}

SoundDevice::~SoundDevice()
{
}

const std::string& SoundDevice::getName() const
{
	return name;
}

const std::string& SoundDevice::getDescription() const
{
	return description;
}

bool SoundDevice::isStereo() const
{
	return stereo == 2 || !balanceCenter;
}

int SoundDevice::getAmplificationFactor() const
{
	return 1;
}

void SoundDevice::registerSound(const XMLElement& config)
{
	const XMLElement& soundConfig = config.getChild("sound");
	double volume = soundConfig.getChildDataAsInt("volume") / 32767.0;
	int devBalance = 0;
	string mode = soundConfig.getChildData("mode", "mono");
	if (mode == "mono") {
		devBalance = 0;
	} else if (mode == "left") {
		devBalance = -100;
	} else if (mode == "right") {
		devBalance = 100;
	} else {
		throw MSXException("balance \"" + mode + "\" illegal");
	}

	XMLElement::Children balances;
	soundConfig.getChildren("balance", balances);
	for (XMLElement::Children::const_iterator it = balances.begin();
	     it != balances.end(); ++it) {
		int balance = (*it)->getDataAsInt();

		if (!(*it)->hasAttribute("channel")) {
			devBalance = balance;
			continue;
		}

		// TODO Support other balances
		if (balance != 0 && balance != -100 && balance != 100) {
			throw MSXException(StringOp::Builder() <<
			                   "balance " << balance << " illegal");
		}
		if (balance != 0) {
			balanceCenter = false;
		}

		const string range = (*it)->getAttribute("channel");
		set<unsigned> channels;
		StringOp::parseRange(range, channels, 1, numChannels);
		for (set<unsigned>::const_iterator it = channels.begin();
		     it != channels.end(); ++it) {
			channelBalance[(*it) - 1] = balance;
		}
	}

	mixer.registerSound(*this, volume, devBalance, numChannels);
}

void SoundDevice::unregisterSound()
{
	mixer.unregisterSound(*this);
}

void SoundDevice::updateStream(EmuTime::param time)
{
	mixer.updateStream(time);
}

void SoundDevice::setInputRate(unsigned sampleRate)
{
	inputSampleRate = sampleRate;
}

void SoundDevice::recordChannel(unsigned channel, const Filename& filename)
{
	assert(channel < numChannels);
	bool wasRecording = writer[channel].get() != NULL;
	if (!filename.empty()) {
		writer[channel].reset(new Wav16Writer(
			filename, stereo, inputSampleRate));
	} else {
		writer[channel].reset();
	}
	bool recording = writer[channel].get() != NULL;
	if (recording != wasRecording) {
		if (recording) {
			if (numRecordChannels == 0) {
				mixer.setSynchronousMode(true);
			}
			++numRecordChannels;
			assert(numRecordChannels <= numChannels);
		} else {
			assert(numRecordChannels > 0);
			--numRecordChannels;
			if (numRecordChannels == 0) {
				mixer.setSynchronousMode(false);
			}
		}
	}
}

void SoundDevice::muteChannel(unsigned channel, bool muted)
{
	assert(channel < numChannels);
	channelMuted[channel] = muted;
}

bool SoundDevice::mixChannels(int* dataOut, unsigned samples)
{
#if ASM_X86
	assert((long(dataOut) & 15) == 0); // must be 16-byte aligned
#endif
	assert(samples <= MAX_SAMPLES);
	if (samples == 0) return true;
	unsigned outputStereo = isStereo() ? 2 : 1;

	MemoryOps::MemSet<unsigned, false> mset;
	mset(reinterpret_cast<unsigned*>(dataOut), outputStereo * samples, 0);

	VLA(int*, bufs, numChannels);
	unsigned separateChannels = 0;
	unsigned pitch = (samples * stereo + 3) & ~3; // align for SSE access
	// TODO optimization: All channels with the same balance (according to
	// channelBalance[]) could use the same buffer when balanceCenter is
	// false
	for (unsigned i = 0; i < numChannels; ++i) {
		if (!channelMuted[i] && !writer[i].get() && balanceCenter) {
			// no need to keep this channel separate
			bufs[i] = dataOut;
		} else {
			// muted or recorded channels must go separate
			bufs[i] = &mixBuffer[pitch * separateChannels];
			++separateChannels;
		}
	}
	mset(reinterpret_cast<unsigned*>(mixBuffer),
	     pitch * separateChannels, 0);

	// note: some SoundDevices (DACSound16S and CassettePlayer) replace the
	//	 (single) channel data instead of adding to the exiting data.
	//	 ATM that's ok because the existing data is anyway zero.
	generateChannels(bufs, samples);

	if (separateChannels == 0) {
		for (unsigned i = 0; i < numChannels; ++i) {
			if (bufs[i]) {
				return true;
			}
		}
		return false;
	}

	// record channels
	for (unsigned i = 0; i < numChannels; ++i) {
		if (writer[i].get()) {
			assert(bufs[i] != dataOut);
			if (bufs[i]) {
				writer[i]->write(
					bufs[i], stereo, samples, getAmplificationFactor());
			} else {
				writer[i]->writeSilence(stereo, samples);
			}
		}
	}

	// remove muted channels (explictly by user or by device itself)
	bool anyUnmuted = false;
	unsigned numMix = 0;
	VLA(int, mixBalance, numChannels);
	for (unsigned i = 0; i < numChannels; ++i) {
		if ((bufs[i] != 0) && !channelMuted[i]) {
			anyUnmuted = true;
			if (bufs[i] != dataOut) {
				bufs[numMix] = bufs[i];
				mixBalance[numMix] = channelBalance[i];
				++numMix;
			}
		}
	}

	if (numMix == 0) {
		// all extra channels muted
		return anyUnmuted;
	}

	// actually mix channels
	if (!balanceCenter) {
		unsigned i = 0;
		do {
			int left0  = 0;
			int right0 = 0;
			int left1  = 0;
			int right1 = 0;
			unsigned j = 0;
			do {
				if (mixBalance[j] <= 0) {
					left0  += bufs[j][i + 0];
					left1  += bufs[j][i + 1];
				}
				if (mixBalance[j] >= 0) {
					right0 += bufs[j][i + 0];
					right1 += bufs[j][i + 1];
				}
				j++;
			} while (j < numMix);
			dataOut[i * 2 + 0] = left0;
			dataOut[i * 2 + 1] = right0;
			dataOut[i * 2 + 2] = left1;
			dataOut[i * 2 + 3] = right1;
			i += 2;
		} while (i < samples);

		return true;
	}

	// In the past we had ARM and x86-SSE2 optimized assembly routines for
	// the stuff below. Currently this code is only rarely used anymore
	// (only when recording or muting individual soundchip channels), so
	// it's not worth the extra complexity anymore.
	unsigned num = samples * stereo;
	unsigned i = 0;
	do {
		int out0 = dataOut[i + 0];
		int out1 = dataOut[i + 1];
		int out2 = dataOut[i + 2];
		int out3 = dataOut[i + 3];
		unsigned j = 0;
		do {
			out0 += bufs[j][i + 0];
			out1 += bufs[j][i + 1];
			out2 += bufs[j][i + 2];
			out3 += bufs[j][i + 3];
			++j;
		} while (j < numMix);
		dataOut[i + 0] = out0;
		dataOut[i + 1] = out1;
		dataOut[i + 2] = out2;
		dataOut[i + 3] = out3;
		i += 4;
	} while (i < num);

	return true;
}

const DynamicClock& SoundDevice::getHostSampleClock() const
{
	return mixer.getHostSampleClock();
}
double SoundDevice::getEffectiveSpeed() const
{
	return mixer.getEffectiveSpeed();
}

} // namespace openmsx
