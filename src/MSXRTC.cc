// $Id$

#include <cassert>
#include "MSXRTC.hh"
#include "MSXMotherBoard.hh"


MSXRTC::MSXRTC()
{
	PRT_DEBUG("Creating an MSXRTC object");
}

MSXRTC::~MSXRTC()
{
	PRT_DEBUG("Detructing an MSXRTC object");
	delete rp5c01;
}

void MSXRTC::init()
{
	MSXDevice::init();
	bool emuTimeBased;
	if (deviceConfig->getParameter("mode")=="RealTime")
		emuTimeBased = false;
	else	emuTimeBased = true;
	rp5c01 = new RP5C01(emuTimeBased);
	MSXMotherBoard::instance()->register_IO_Out(0xB4,this);
	MSXMotherBoard::instance()->register_IO_Out(0xB5,this);
	MSXMotherBoard::instance()->register_IO_In (0xB5,this);
}

void MSXRTC::reset()
{
	MSXDevice::reset();
	rp5c01->reset();
}

byte MSXRTC::readIO(byte port, Emutime &time)
{
	assert(port==0xB5);
	return rp5c01->readPort(registerLatch, time) | 0xf0;	//TODO check this
}

void MSXRTC::writeIO(byte port, byte value, Emutime &time)
{
	switch (port) {
	case 0xB4:
		registerLatch = value&0x0f;
		break;
	case 0xB5:
		rp5c01->writePort(registerLatch, value&0x0f, time);
		break;
	default:
		assert(false);
	}
}

