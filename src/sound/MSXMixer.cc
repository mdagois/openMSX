#include "MSXMixer.hh"
#include "Mixer.hh"
#include "SoundDevice.hh"
#include "MSXCommandController.hh"
#include "InfoTopic.hh"
#include "TclObject.hh"
#include "ThrottleManager.hh"
#include "GlobalSettings.hh"
#include "IntegerSetting.hh"
#include "StringSetting.hh"
#include "BooleanSetting.hh"
#include "CommandException.hh"
#include "AviRecorder.hh"
#include "Filename.hh"
#include "CliComm.hh"
#include "Math.hh"
#include "StringOp.hh"
#include "memory.hh"
#include "stl.hh"
#include "unreachable.hh"
#include "vla.hh"
#include <algorithm>
#include <tuple>
#include <cmath>
#include <cstring>
#include <cassert>

using std::remove;
using std::string;
using std::vector;

namespace openmsx {

class SoundDeviceInfoTopic : public InfoTopic
{
public:
	SoundDeviceInfoTopic(InfoCommand& machineInfoCommand, MSXMixer& mixer);
	virtual void execute(array_ref<TclObject> tokens,
	                     TclObject& result) const;
	virtual string help(const vector<string>& tokens) const;
	virtual void tabCompletion(vector<string>& tokens) const;
private:
	MSXMixer& mixer;
};


MSXMixer::SoundDeviceInfo::SoundDeviceInfo()
{
}

MSXMixer::SoundDeviceInfo::SoundDeviceInfo(SoundDeviceInfo&& rhs)
	: device         (std::move(rhs.device))
	, defaultVolume  (std::move(rhs.defaultVolume))
	, volumeSetting  (std::move(rhs.volumeSetting))
	, balanceSetting (std::move(rhs.balanceSetting))
	, channelSettings(std::move(rhs.channelSettings))
	, left1          (std::move(rhs.left1))
	, right1         (std::move(rhs.right1))
	, left2          (std::move(rhs.left2))
	, right2         (std::move(rhs.right2))
{
}

MSXMixer::SoundDeviceInfo& MSXMixer::SoundDeviceInfo::operator=(SoundDeviceInfo&& rhs)
{
	device          = std::move(rhs.device);
	defaultVolume   = std::move(rhs.defaultVolume);
	volumeSetting   = std::move(rhs.volumeSetting);
	balanceSetting  = std::move(rhs.balanceSetting);
	channelSettings = std::move(rhs.channelSettings);
	left1           = std::move(rhs.left1);
	right1          = std::move(rhs.right1);
	left2           = std::move(rhs.left2);
	right2          = std::move(rhs.right2);
	return *this;
}

MSXMixer::SoundDeviceInfo::ChannelSettings::ChannelSettings()
{
}

MSXMixer::SoundDeviceInfo::ChannelSettings::ChannelSettings(ChannelSettings&& rhs)
	: recordSetting(std::move(rhs.recordSetting))
	, muteSetting  (std::move(rhs.muteSetting))
{
}

MSXMixer::SoundDeviceInfo::ChannelSettings&
MSXMixer::SoundDeviceInfo::ChannelSettings::operator=(ChannelSettings&& rhs)
{
	recordSetting = std::move(rhs.recordSetting);
	muteSetting   = std::move(rhs.muteSetting);
	return *this;
}


MSXMixer::MSXMixer(Mixer& mixer_, Scheduler& scheduler,
                   MSXCommandController& msxCommandController_,
                   GlobalSettings& globalSettings)
	: Schedulable(scheduler)
	, mixer(mixer_)
	, commandController(msxCommandController_)
	, masterVolume(mixer.getMasterVolume())
	, speedSetting(globalSettings.getSpeedSetting())
	, throttleManager(globalSettings.getThrottleManager())
	, prevTime(getCurrentTime(), 44100)
	, soundDeviceInfo(make_unique<SoundDeviceInfoTopic>(
		msxCommandController_.getMachineInfoCommand(), *this))
	, recorder(nullptr)
	, synchronousCounter(0)
{
	hostSampleRate = 44100;
	fragmentSize = 0;

	muteCount = 1;
	unmute(); // calls Mixer::registerMixer()

	reschedule2();

	masterVolume.attach(*this);
	speedSetting.attach(*this);
	throttleManager.attach(*this);
}

MSXMixer::~MSXMixer()
{
	if (recorder) {
		recorder->stop();
	}
	assert(infos.empty());

	throttleManager.detach(*this);
	speedSetting.detach(*this);
	masterVolume.detach(*this);

	mute(); // calls Mixer::unregisterMixer()
}

void MSXMixer::registerSound(SoundDevice& device, double volume,
                             int balance, unsigned numChannels)
{
	// TODO read volume/balance(mode) from config file
	const string& name = device.getName();
	SoundDeviceInfo info;
	info.device = &device;
	info.defaultVolume = volume;
	info.volumeSetting = make_unique<IntegerSetting>(
		commandController, name + "_volume",
		"the volume of this sound chip", 75, 0, 100);
	info.balanceSetting = make_unique<IntegerSetting>(
		commandController, name + "_balance",
		"the balance of this sound chip", balance, -100, 100);

	info.volumeSetting->attach(*this);
	info.balanceSetting->attach(*this);

	for (unsigned i = 0; i < numChannels; ++i) {
		SoundDeviceInfo::ChannelSettings channelSettings;
		string ch_name = StringOp::Builder() << name << "_ch" << i + 1;

		channelSettings.recordSetting = make_unique<StringSetting>(
			commandController, ch_name + "_record",
			"filename to record this channel to",
			"", Setting::DONT_SAVE);
		channelSettings.recordSetting->attach(*this);

		channelSettings.muteSetting = make_unique<BooleanSetting>(
			commandController, ch_name + "_mute",
			"sets mute-status of individual sound channels",
			false, Setting::DONT_SAVE);
		channelSettings.muteSetting->attach(*this);

		info.channelSettings.push_back(std::move(channelSettings));
	}

	device.setOutputRate(getSampleRate());
	infos.push_back(std::move(info));
	updateVolumeParams(infos.back());

	commandController.getCliComm().update(CliComm::SOUNDDEVICE, device.getName(), "add");
}

void MSXMixer::unregisterSound(SoundDevice& device)
{
	auto it = find_if_unguarded(infos,
		[&](const SoundDeviceInfo& i) { return i.device == &device; });
	it->volumeSetting->detach(*this);
	it->balanceSetting->detach(*this);
	for (auto& s : it->channelSettings) {
		s.recordSetting->detach(*this);
		s.muteSetting->detach(*this);
	}
	if (it != (end(infos) - 1)) std::swap(*it, *(end(infos) - 1));
	infos.pop_back();
	commandController.getCliComm().update(CliComm::SOUNDDEVICE, device.getName(), "remove");
}

void MSXMixer::setSynchronousMode(bool synchronous)
{
	// TODO ATM synchronous is not used anymore
	if (synchronous) {
		++synchronousCounter;
		if (synchronousCounter == 1) {
			setMixerParams(fragmentSize, hostSampleRate);
		}
	} else {
		assert(synchronousCounter > 0);
		--synchronousCounter;
		if (synchronousCounter == 0) {
			setMixerParams(fragmentSize, hostSampleRate);
		}
	}
}

double MSXMixer::getEffectiveSpeed() const
{
	return synchronousCounter
	     ? 1.0
	     : speedSetting.getInt() / 100.0;
}

void MSXMixer::updateStream(EmuTime::param time)
{
	unsigned count = prevTime.getTicksTill(time);

	// call generate() even if count==0 and even if muted
	short mixBuffer[8192 * 2];
	assert(count <= 8192);
	generate(mixBuffer, time, count);

	if (!muteCount && fragmentSize) {
		mixer.uploadBuffer(*this, mixBuffer, count);
	}

	if (recorder) {
		recorder->addWave(count, mixBuffer);
	}

	prevTime += count;
}


// Various (inner) loops that multiply one buffer by a constant and add the
// result to a second buffer. Either buffer can be mono or stereo, so if
// necessary the mono buffer is expanded to stereo. It's possible the
// accumulation buffer is still empty (as-if it contains zeros), in that case
// we skip the accumulation step.

// acc[0:n] = mul[0:n] * f
static inline void mul(int* __restrict acc, const int* __restrict mul, int n, int f)
{
#ifdef __arm__
	// ARM assembly version
	unsigned dummy1, dummy2, dummy3;
	asm volatile (
	"0:\n\t"
		"ldmia	%[in]!,{r3-r6}\n\t"
		"mul	r3,%[f],r3\n\t"
		"mul	r4,%[f],r4\n\t"
		"mul	r5,%[f],r5\n\t"
		"mul	r6,%[f],r6\n\t"
		"stmia	%[out]!,{r3-r6}\n\t"
		"subs	%[n],%[n],#4\n\t"
		"bgt	0b\n\t"
		: [in]  "=r"    (dummy1)
		, [out] "=r"    (dummy2)
		, [n]   "=r"    (dummy3)
		:       "[in]"  (mul)
		,       "[out]" (acc)
		,       "[n]"   (n)
		, [f]   "r"     (f)
		: "memory", "r3","r4","r5","r6"
	);
	return;
#endif

	// C++ version
	int i = 0;
	do {
		acc[i] = mul[i] * f;
	} while (++i < n);
}

// acc[0:n] += mul[0:n] * f
static inline void mulAcc(int* __restrict acc, const int* __restrict mul, int n, int f)
{
#ifdef __arm__
	// ARM assembly version
	unsigned dummy1, dummy2, dummy3;
	asm volatile (
	"0:\n\t"
		"ldmia	%[in]!,{r3,r4,r5,r6}\n\t"
		"ldmia	%[out],{r8,r9,r10,r12}\n\t"
		"mla	r3,%[f],r3,r8\n\t"
		"mla	r4,%[f],r4,r9\n\t"
		"mla	r5,%[f],r5,r10\n\t"
		"mla	r6,%[f],r6,r12\n\t"
		"stmia	%[out]!,{r3,r4,r5,r6}\n\t"
		"subs	%[n],%[n],#4\n\t"
		"bgt	0b\n\t"
		: [in]  "=r"    (dummy1)
		, [out] "=r"    (dummy2)
		, [n]   "=r"    (dummy3)
		:       "[in]"  (mul)
		,       "[out]" (acc)
		,       "[n]"   (n)
		, [f]   "r"     (f)
		: "memory"
		, "r3","r4","r5","r6"
		, "r8","r9","r10","r12"
	);
	return;
#endif

	// C++ version
	int i = 0;
	do {
		acc[i] += mul[i] * f;
	} while (++i < n);
}

// acc[0:2n+0:2] = mul[0:n] * l
// acc[1:2n+1:2] = mul[0:n] * r
static inline void mulExpand(
	int* __restrict acc, const int* __restrict mul, int n, int l, int r)
{
	int i = 0;
	do {
		int t = mul[i];
		acc[2 * i + 0] = l * t;
		acc[2 * i + 1] = r * t;
	} while (++i < n);
}

// acc[0:2n+0:2] += mul[0:n] * l
// acc[1:2n+1:2] += mul[0:n] * r
static inline void mulExpandAcc(
	int* __restrict acc, const int* __restrict mul, int n, int l, int r)
{
	int i = 0;
	do {
		int t = mul[i];
		acc[2 * i + 0] += l * t;
		acc[2 * i + 1] += r * t;
	} while (++i < n);
}

// acc[0:2n+0:2] = mul[0:2n+0:2] * l1 + mul[1:2n+1:2] * l2
// acc[1:2n+1:2] = mul[0:2n+0:2] * r1 + mul[1:2n+1:2] * r2
static inline void mulMix2(
	int* __restrict acc, const int* __restrict mul, int n,
	int l1, int l2, int r1, int r2)
{
	int i = 0;
	do {
		int t1 = mul[2 * i + 0];
		int t2 = mul[2 * i + 1];
		acc[2 * i + 0] = l1 * t1 + l2 * t2;
		acc[2 * i + 1] = r1 * t1 + r2 * t2;
	} while (++i < n);
}

// acc[0:2n+0:2] += mul[0:2n+0:2] * l1 + mul[1:2n+1:2] * l2
// acc[1:2n+1:2] += mul[0:2n+0:2] * r1 + mul[1:2n+1:2] * r2
static inline void mulMix2Acc(
	int* __restrict acc, const int* __restrict mul, int n,
	int l1, int l2, int r1, int r2)
{
	int i = 0;
	do {
		int t1 = mul[2 * i + 0];
		int t2 = mul[2 * i + 1];
		acc[2 * i + 0] += l1 * t1 + l2 * t2;
		acc[2 * i + 1] += r1 * t1 + r2 * t2;
	} while (++i < n);
}


// DC removal filter routines:
//
//  formula:
//     y(n) = x(n) - x(n-1) + R * y(n-1)
//  with:
//     R = 1 - (2*pi * cut-off-frequency / samplerate)
//  we take R = 511/512
//   44100Hz --> cutt-off freq = 14Hz
//   22050Hz                     7Hz
// Note: we divide by 512 iso shift-right by 9 because we want
//       to round towards zero (shift rounds to -inf).

// No new input, previous output was (non-zero) mono.
static inline int filterMonoNull(int x0, int y, short* out, int n)
{
	assert(n > 0);
	y = ((511 * y) / 512) - x0;
	short s0 = Math::clipIntToShort(y);
	out[0] = s0;
	out[1] = s0;
	for (int i = 1; i < n; ++i) {
		y = (511 * y) / 512;
		short s = Math::clipIntToShort(y);
		out[2 * i + 0] = s;
		out[2 * i + 1] = s;
	}
	return y;
}

// No new input, previous output was (non-zero) stereo.
static inline std::tuple<int, int> filterStereoNull(
	int xl0, int xr0, int yl, int yr, short* out, int n)
{
	assert(n > 0);
	yl = ((511 * yl) / 512) - xl0;
	yr = ((511 * yr) / 512) - xr0;
	out[0] = Math::clipIntToShort(yl);
	out[1] = Math::clipIntToShort(yr);
	for (int i = 1; i < n; ++i) {
		yl = (511 * yl) / 512;
		yr = (511 * yr) / 512;
		out[2 * i + 0] = Math::clipIntToShort(yl);
		out[2 * i + 1] = Math::clipIntToShort(yr);
	}
	return std::make_tuple(yl, yr);
}

// New input is mono, previous output was also mono.
static inline std::tuple<int, int> filterMonoMono(
	int x0, int y, const int* in, short* out, int n)
{
	assert(n > 0);
#ifdef __arm__
	// Note: there are two functional differences in the
	//       asm and c++ code below:
	//  - divide by 512 is replaced by ASR #9
	//    (different for negative numbers)
	//  - the outLeft variable is set to the clipped value
	// Though this difference is very small, and we need
	// the extra speed.
	unsigned dummy1, dummy2, dummy3, dummy4;
	asm volatile (
	"0:\n\t"
		"rsb	%[y],%[y],%[y],LSL #9\n\t"
		"rsb	%[y],%[x],%[y],ASR #9\n\t"
		"ldr	%[x],[%[in]],#4\n\t"
		"asrs	%[x],%[x],#8\n\t"
		"add	%[y],%[y],%[x]\n\t"
		"lsls	%[t],%[y],#16\n\t"
		"cmp	%[y],%[t],ASR #16\n\t"
		"it ne\n\t"
		"subne	%[y],%[m],%[y],ASR #31\n\t"
		"strh	%[y],[%[out]],#2\n\t"
		"strh	%[y],[%[out]],#2\n\t"
		"subs	%[n],%[n],#1\n\t"
		"bne	0b\n\t"
		: [y]   "=r"    (y)
		, [x]   "=r"    (x0)
		, [in]  "=r"    (dummy1)
		, [out] "=r"    (dummy2)
		, [n]   "=r"    (dummy3)
		, [t]   "=&r"   (dummy4)
		:       "[y]"   (y)
		,       "[x]"   (x0)
		,       "[in]"  (in)
		,       "[out]" (out)
		,       "[n]"   (n)
		, [m]   "r"     (0x7FFF)
		: "memory"
	);
	return std::make_tuple(x0, y);
#endif

	// C++ version
	int i = 0;
	do {
		int x = in[i] >> 8;
		y = x - x0 + ((511 * y) / 512);
		x0 = x;
		short s = Math::clipIntToShort(y);
		out[2 * i + 0] = s;
		out[2 * i + 1] = s;
	} while (++i < n);
	return std::make_tuple(x0, y);
}

// New input is mono, previous output was stereo
static inline std::tuple<int, int, int> filterStereoMono(
	int xl0, int xr0, int yl, int yr, const int* in, short* out, int n)
{
	assert(n > 0);
	int x = in[0] >> 8;
	yl = x - xl0 + ((511 * yl) / 512);
	yr = x - xr0 + ((511 * yr) / 512);
	out[0] = Math::clipIntToShort(yl);
	out[1] = Math::clipIntToShort(yr);
	for (int i = 1; i < n; ++i) {
		int x1 = in[i] >> 8;
		yl = x1 - x + ((511 * yl) / 512);
		yr = x1 - x + ((511 * yr) / 512);
		x = x1;
		out[2 * i + 0] = Math::clipIntToShort(yl);
		out[2 * i + 1] = Math::clipIntToShort(yr);
	}
	return std::make_tuple(x, yl, yr);
}

// New input is stereo, (previous output either mono/stereo)
static inline std::tuple<int, int, int, int> filterStereoStereo(
	int xl0, int xr0, int yl, int yr, const int* in, short* out, int n)
{
	assert(n > 0);
	int i = 0;
	do {
		int xl = in[2 * i + 0] >> 8;
		int xr = in[2 * i + 1] >> 8;
		yl = xl - xl0 + ((511 * yl) / 512);
		yr = xr - xr0 + ((511 * yr) / 512);
		xl0 = xl;
		xr0 = xr;
		out[2 * i + 0] = Math::clipIntToShort(yl);
		out[2 * i + 1] = Math::clipIntToShort(yr);
	} while (++i < n);
	return std::make_tuple(xl0, xr0, yl, yr);
}

// We have both mono and stereo input (and produce stereo output)
static inline std::tuple<int, int, int, int> filterBothStereo(
	int xl0, int xr0, int yl, int yr, const int* inM, const int* inS, short* out, int n)
{
	assert(n > 0);
	int i = 0;
	do {
		int m = inM[i];
		int xl = (inS[2 * i + 0] + m) >> 8;
		int xr = (inS[2 * i + 1] + m) >> 8;
		yl = xl - xl0 + ((511 * yl) / 512);
		yr = xr - xr0 + ((511 * yr) / 512);
		xl0 = xl;
		xr0 = xr;
		out[2 * i + 0] = Math::clipIntToShort(yl);
		out[2 * i + 1] = Math::clipIntToShort(yr);
	} while (++i < n);
	return std::make_tuple(xl0, xr0, yl, yr);
}


void MSXMixer::generate(short* output, EmuTime::param time, unsigned samples)
{
	// The code below is specialized for a lot of cases (before this
	// routine was _much_ shorter). This is done because this routine
	// ends up relatively high (top 5) in a profile run.
	// After these specialization this routine runs about two times
	// faster for the common cases (mono output or no sound at all).
	// In total emulation time this gave a speedup of about 2%.

	VLA(int, stereoBuf, 2 * samples + 3);
	VLA(int, monoBuf, samples + 3);
	VLA_SSE_ALIGNED(int, tmpBuf, 2 * samples + 3);

	static const unsigned HAS_MONO_FLAG = 1;
	static const unsigned HAS_STEREO_FLAG = 2;
	unsigned usedBuffers = 0;

	// FIXME: The Infos should be ordered such that all the mono
	// devices are handled first
	for (auto& info : infos) {
		// When samples==0, call updateBuffer() but skip mixing
		SoundDevice& device = *info.device;
		if (device.updateBuffer(samples, tmpBuf, time) &&
		    (samples > 0)) {
			int l1 = info.left1;
			int r1 = info.right1;
			if (!device.isStereo()) {
				if (l1 == r1) {
					if (!(usedBuffers & HAS_MONO_FLAG)) {
						usedBuffers |= HAS_MONO_FLAG;
						mul(monoBuf, tmpBuf, samples, l1);
					} else {
						mulAcc(monoBuf, tmpBuf, samples, l1);
					}
				} else {
					if (!(usedBuffers & HAS_STEREO_FLAG)) {
						usedBuffers |= HAS_STEREO_FLAG;
						mulExpand(stereoBuf, tmpBuf, samples, l1, r1);
					} else {
						mulExpandAcc(stereoBuf, tmpBuf, samples, l1, r1);
					}
				}
			} else {
				int l2 = info.left2;
				int r2 = info.right2;
				if (!(usedBuffers & HAS_STEREO_FLAG)) {
					usedBuffers |= HAS_STEREO_FLAG;
					mulMix2(stereoBuf, tmpBuf, samples, l1, l2, r1, r2);
				} else {
					mulMix2Acc(stereoBuf, tmpBuf, samples, l1, l2, r1, r2);
				}
			}
		}
	}

	// DC removal filter
	switch (usedBuffers) {
	case 0: // no new input
		if (samples == 0) break;
		if ((outLeft == outRight) && (prevLeft == prevRight)) {
			if ((outLeft == 0) && (prevLeft == 0)) {
				// Output was zero, new input is zero,
				// after DC-filter output will still be zero.
				memset(output, 0, 2 * samples * sizeof(short));
			} else {
				// Output was not zero, but it was the same left and right.
				outLeft = filterMonoNull(prevLeft, outLeft, output, samples);
				outRight = outLeft;
			}
		} else {
			std::tie(outLeft, outRight) = filterStereoNull(
				prevLeft, prevRight, outLeft, outRight, output, samples);
		}
		prevLeft = prevRight = 0;
		break;

	case HAS_MONO_FLAG: // only mono
		if ((outLeft == outRight) && (prevLeft == prevRight)) {
			// previous output was also mono
			std::tie(prevLeft, outLeft) = filterMonoMono(
				prevLeft, outLeft, monoBuf, output, samples);
			outRight = outLeft;
		} else {
			// previous output was stereo, rarely triggers but needed for correctness
			std::tie(prevLeft, outLeft, outRight) = filterStereoMono(
				prevLeft, prevRight, outLeft, outRight, monoBuf, output, samples);
		}
		prevRight = prevLeft;
		break;

	case HAS_STEREO_FLAG: // only stereo
		std::tie(prevLeft, prevRight, outLeft, outRight) = filterStereoStereo(
			prevLeft, prevRight, outLeft, outRight, stereoBuf, output, samples);
		break;

	default: // mono + stereo
		std::tie(prevLeft, prevRight, outLeft, outRight) = filterBothStereo(
			prevLeft, prevRight, outLeft, outRight, monoBuf, stereoBuf, output, samples);
	}
}

bool MSXMixer::needStereoRecording() const
{
	return any_of(begin(infos), end(infos),
		[](const SoundDeviceInfo& info) {
			return info.device->isStereo() ||
			       info.balanceSetting->getInt() != 0; });
}

void MSXMixer::mute()
{
	if (muteCount == 0) {
		mixer.unregisterMixer(*this);
	}
	++muteCount;
}

void MSXMixer::unmute()
{
	--muteCount;
	if (muteCount == 0) {
		prevLeft = outLeft = 0;
		prevRight = outRight = 0;
		mixer.registerMixer(*this);
	}
}

void MSXMixer::reInit()
{
	prevTime.reset(getCurrentTime());
	prevTime.setFreq(hostSampleRate / getEffectiveSpeed());
	reschedule();
}
void MSXMixer::reschedule()
{
	removeSyncPoints();
	reschedule2();
}
void MSXMixer::reschedule2()
{
	unsigned size = (!muteCount && fragmentSize) ? fragmentSize : 512;
	setSyncPoint(prevTime.getFastAdd(size));
}

void MSXMixer::setMixerParams(unsigned newFragmentSize, unsigned newSampleRate)
{
	// TODO old code checked that values did actually change,
	//      investigate if this optimization is worth it
	hostSampleRate = newSampleRate;
	fragmentSize = newFragmentSize;

	reInit(); // must come before call to setOutputRate()

	for (auto& info : infos) {
		info.device->setOutputRate(newSampleRate);
	}
}

const DynamicClock& MSXMixer::getHostSampleClock() const
{
	return prevTime;
}

void MSXMixer::setRecorder(AviRecorder* newRecorder)
{
	if ((recorder != nullptr) != (newRecorder != nullptr)) {
		setSynchronousMode(newRecorder != nullptr);
	}
	recorder = newRecorder;
}

unsigned MSXMixer::getSampleRate() const
{
	return hostSampleRate;
}

void MSXMixer::update(const Setting& setting)
{
	if (&setting == &masterVolume) {
		updateMasterVolume();
	} else if (&setting == &speedSetting) {
		if (synchronousCounter == 0) {
			setMixerParams(fragmentSize, hostSampleRate);
		} else {
			// Avoid calling reInit() while recording because
			// each call causes a small hiccup in the sound (and
			// while recording this call anyway has no effect).
			// This was noticable while sliding the speed slider
			// in catapult (becuase this causes many changes in
			// the speed setting).
		}
	} else if (dynamic_cast<const IntegerSetting*>(&setting)) {
		auto it = find_if_unguarded(infos,
			[&](const SoundDeviceInfo& i) {
				return (i.volumeSetting .get() == &setting) ||
				       (i.balanceSetting.get() == &setting); });
		updateVolumeParams(*it);
	} else if (dynamic_cast<const StringSetting*>(&setting)) {
		changeRecordSetting(setting);
	} else if (dynamic_cast<const BooleanSetting*>(&setting)) {
		changeMuteSetting(setting);
	} else {
		UNREACHABLE;
	}
}

void MSXMixer::changeRecordSetting(const Setting& setting)
{
	for (auto& info : infos) {
		unsigned channel = 0;
		for (auto& s : info.channelSettings) {
			if (s.recordSetting.get() == &setting) {
				info.device->recordChannel(
					channel,
					Filename(s.recordSetting->getString()));
				return;
			}
			++channel;
		}
	}
	UNREACHABLE;
}

void MSXMixer::changeMuteSetting(const Setting& setting)
{
	for (auto& info : infos) {
		unsigned channel = 0;
		for (auto& s : info.channelSettings) {
			if (s.muteSetting.get() == &setting) {
				info.device->muteChannel(
					channel, s.muteSetting->getBoolean());
				return;
			}
			++channel;
		}
	}
	UNREACHABLE;
}

void MSXMixer::update(const ThrottleManager& /*throttleManager*/)
{
	//reInit();
	// TODO Should this be removed?
}

void MSXMixer::updateVolumeParams(SoundDeviceInfo& info)
{
	int mVolume = masterVolume.getInt();
	int dVolume = info.volumeSetting->getInt();
	double volume = info.defaultVolume * mVolume * dVolume / (100.0 * 100.0);
	int balance = info.balanceSetting->getInt();
	double l1, r1, l2, r2;
	if (info.device->isStereo()) {
		if (balance < 0) {
			double b = (balance + 100.0) / 100.0;
			l1 = volume;
			r1 = 0.0;
			l2 = volume * sqrt(std::max(0.0, 1.0 - b));
			r2 = volume * sqrt(std::max(0.0,       b));
		} else {
			double b = balance / 100.0;
			l1 = volume * sqrt(std::max(0.0, 1.0 - b));
			r1 = volume * sqrt(std::max(0.0,       b));
			l2 = 0.0;
			r2 = volume;
		}
	} else {
		// make sure that in case of rounding errors
		// we don't take sqrt() of negative numbers
		double b = (balance + 100.0) / 200.0;
		l1 = volume * sqrt(std::max(0.0, 1.0 - b));
		r1 = volume * sqrt(std::max(0.0,       b));
		l2 = r2 = 0.0; // dummy
	}
	int amp = 256 * info.device->getAmplificationFactor();
	info.left1  = int(l1 * amp);
	info.right1 = int(r1 * amp);
	info.left2  = int(l2 * amp);
	info.right2 = int(r2 * amp);
}

void MSXMixer::updateMasterVolume()
{
	for (auto& p : infos) {
		updateVolumeParams(p);
	}
}

void MSXMixer::executeUntil(EmuTime::param time, int /*userData*/)
{
	updateStream(time);
	reschedule2();
}


// Sound device info

SoundDevice* MSXMixer::findDevice(string_ref name) const
{
	auto it = find_if(begin(infos), end(infos),
		[&](const SoundDeviceInfo& i) {
			return i.device->getName() == name; });
	return (it != end(infos)) ? it->device : nullptr;
}

SoundDeviceInfoTopic::SoundDeviceInfoTopic(
		InfoCommand& machineInfoCommand, MSXMixer& mixer_)
	: InfoTopic(machineInfoCommand, "sounddevice")
	, mixer(mixer_)
{
}

void SoundDeviceInfoTopic::execute(array_ref<TclObject> tokens,
                                   TclObject& result) const
{
	switch (tokens.size()) {
	case 2:
		for (auto& info : mixer.infos) {
			result.addListElement(info.device->getName());
		}
		break;
	case 3: {
		SoundDevice* device = mixer.findDevice(tokens[2].getString());
		if (!device) {
			throw CommandException("Unknown sound device");
		}
		result.setString(device->getDescription());
		break;
	}
	default:
		throw CommandException("Too many parameters");
	}
}

string SoundDeviceInfoTopic::help(const vector<string>& /*tokens*/) const
{
	return "Shows a list of available sound devices.\n";
}

void SoundDeviceInfoTopic::tabCompletion(vector<string>& tokens) const
{
	if (tokens.size() == 3) {
		vector<string_ref> devices;
		for (auto& info : mixer.infos) {
			devices.push_back(info.device->getName());
		}
		completeString(tokens, devices);
	}
}

} // namespace openmsx
