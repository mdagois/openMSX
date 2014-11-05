#ifndef MSXMUSIC_HH
#define MSXMUSIC_HH

#include "MSXDevice.hh"
#include "serialize_meta.hh"
#include <memory>

namespace openmsx {

class Rom;
class YM2413;

class MSXMusicBase : public MSXDevice
{
public:
	void reset(EmuTime::param time) override;
	void writeIO(word port, byte value, EmuTime::param time) override;
	byte peekMem(word address, EmuTime::param time) const override;
	byte readMem(word address, EmuTime::param time) override;
	const byte* getReadCacheLine(word start) const override;

	template<typename Archive>
	void serialize(Archive& ar, unsigned version);

protected:
	explicit MSXMusicBase(const DeviceConfig& config);
	~MSXMusicBase();

	void writeRegisterPort(byte value, EmuTime::param time);
	void writeDataPort(byte value, EmuTime::param time);

	const std::unique_ptr<Rom> rom;
	const std::unique_ptr<YM2413> ym2413;

private:
	int registerLatch;
};
SERIALIZE_CLASS_VERSION(MSXMusicBase, 2);


class MSXMusic : public MSXMusicBase
{
public:
	explicit MSXMusic(const DeviceConfig& config);

	template<typename Archive>
	void serialize(Archive& ar, unsigned version);
};
SERIALIZE_CLASS_VERSION(MSXMusic, 2); // must be same as MSXMusicBase


// Variant used in Panasonic_FS-A1WX and Panasonic_FS-A1WSX
class MSXMusicWX : public MSXMusicBase
{
public:
	explicit MSXMusicWX(const DeviceConfig& config);

	void reset(EmuTime::param time) override;
	byte peekMem(word address, EmuTime::param time) const override;
	byte readMem(word address, EmuTime::param time) override;
	const byte* getReadCacheLine(word start) const override;
	void writeMem(word address, byte value, EmuTime::param time) override;
	byte* getWriteCacheLine(word start) const override;

	template<typename Archive>
	void serialize(Archive& ar, unsigned version);

private:
	byte control;
};
SERIALIZE_CLASS_VERSION(MSXMusicWX, 2); // must be same as MSXMusicBase

} // namespace openmsx

#endif
