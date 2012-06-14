// $Id$

#ifndef TCLCALLBACKMESSAGES_HH
#define TCLCALLBACKMESSAGES_HH

#include "CliListener.hh"
#include <memory>

namespace openmsx {

class GlobalCliComm;
class CommandController;
class StringSetting;

class TclCallbackMessages : public CliListener
{
public:
	TclCallbackMessages(GlobalCliComm& cliComm, CommandController& controller);
	virtual ~TclCallbackMessages();

	virtual void log(CliComm::LogLevel level, string_ref message);

	virtual void update(CliComm::UpdateType type, string_ref machine,
	                    string_ref name, string_ref value);

private:
	GlobalCliComm& cliComm;
	const std::auto_ptr<StringSetting> messageCallbackSetting;
};

} // namespace openmsx

#endif
