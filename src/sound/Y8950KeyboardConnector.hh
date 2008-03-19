// $Id$

#ifndef Y8950KEYBOARDCONNECTOR_HH
#define Y8950KEYBOARDCONNECTOR_HH

#include "Y8950KeyboardDevice.hh"
#include "Connector.hh"

namespace openmsx {

class PluggingController;

class DummyY8950KeyboardDevice : public Y8950KeyboardDevice
{
public:
	virtual void write(byte data, const EmuTime& time);
	virtual byte read(const EmuTime& time);

	virtual const std::string& getDescription() const;
	virtual void plugHelper(Connector& connector, const EmuTime& time);
	virtual void unplugHelper(const EmuTime& time);
};

class Y8950KeyboardConnector : public Connector
{
public:
	explicit Y8950KeyboardConnector(PluggingController& pluggingController);
	virtual ~Y8950KeyboardConnector();

	void write(byte data, const EmuTime& time);
	byte read(const EmuTime& time);
	Y8950KeyboardDevice& getPluggedKeyb() const;

	// Connector
	virtual const std::string& getDescription() const;
	virtual const std::string& getClass() const;
	virtual void plug(Pluggable& dev, const EmuTime& time);

private:
	PluggingController& pluggingController;
	byte data;
};

} // namespace openmsx

#endif
