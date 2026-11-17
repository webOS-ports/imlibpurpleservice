/*
 * fromname_test.cpp
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport).
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free Software
 * Foundation.
 *
 * IMMessage.cpp: the name an incoming message's sender is given (from.name), which the chat
 * threader takes as the thread's name. A Telegram 1:1 sender is a routable id ("id<userId>"), so
 * its name comes separately; other services' 1:1 senders keep what they had.
 */

#include "IMMessage.h"
#include "IMServiceApp.h"
#include "IMServiceHandler.h"

#include <gtest/gtest.h>

#include <string>

// What IMMessage.cpp needs from the rest of the service, which these tests leave out
MojLogger IMServiceApp::s_log(_T("fromname-test"));
MojErr IMServiceHandler::privatelogIMMessage(const MojChar*, MojObject, const MojChar*)
{
	return MojErrNone;
}

namespace {

struct Incoming {
	const char* serviceName = "type_telegram";
	const char* usernameFrom = "id0";
	const char* usernameFromDisplay = nullptr;
	const char* channelName = nullptr;
	bool outgoing = false;
};

MojObject fromOf(const Incoming& in)
{
	MojRefCountedPtr<IMMessage> message(new IMMessage());
	EXPECT_EQ(MojErrNone, message->initFromCallback(in.serviceName, "+31600000000", in.usernameFrom, "text", 0,
	                                                in.channelName, nullptr, nullptr, nullptr, false,
	                                                in.usernameFromDisplay, in.outgoing));
	MojObject object;
	EXPECT_EQ(MojErrNone, message->createDBObject(object));
	MojObject from;
	EXPECT_TRUE(object.get(_T("from"), from));
	return from;
}

std::string get(const MojObject& obj, const char* key)
{
	MojString value;
	bool found = false;
	EXPECT_EQ(MojErrNone, obj.get(key, value, found));
	return found ? std::string(value.data()) : std::string("(none)");
}

} // namespace

TEST(FromName, TelegramSenderNamedByItsAlias)
{
	Incoming in;
	in.usernameFromDisplay = "Telegram";
	MojObject from = fromOf(in);
	EXPECT_EQ("id0", get(from, "addr"));
	EXPECT_EQ("Telegram", get(from, "name"));
}

TEST(FromName, TelegramSenderWithoutAliasHasNoName)
{
	EXPECT_EQ("(none)", get(fromOf(Incoming()), "name"));
}

TEST(FromName, OtherServicesUnchanged)
{
	Incoming in;
	in.serviceName = "type_signal";
	in.usernameFrom = "95f174a3-4546-4f44-9b07-3db2cedb7a57";
	in.usernameFromDisplay = "Signal user";
	EXPECT_EQ("(none)", get(fromOf(in), "name"));
}

TEST(FromName, OwnMessageFromAnotherClientHasNoName)
{
	Incoming in;
	in.usernameFromDisplay = "Telegram";
	in.outgoing = true;
	EXPECT_EQ("(none)", get(fromOf(in), "name"));
}

TEST(FromName, GroupSenderStillNamed)
{
	Incoming in;
	in.usernameFrom = "id123";
	in.usernameFromDisplay = "Someone";
	in.channelName = "chat-1";
	EXPECT_EQ("Someone", get(fromOf(in), "name"));
}
