/*
 * typing_test.cpp
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport).
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free Software
 * Foundation.
 *
 * TypingChannel.cpp: who is typing to us, for the Messaging app's watchTyping. The table keeps one
 * state per (service, account, buddy) and reports only changes; a subscriber gets the typers
 * there are first, then one event per change, and none once it went away.
 */

#include "TypingChannel.h"
#include "IMServiceApp.h"
#include "core/MojObjectBuilder.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

MojLogger IMServiceApp::s_log(_T("imlibpurple.test"));

namespace {

std::string str(const MojObject& obj, const char* key)
{
	MojString value;
	bool found = false;
	EXPECT_EQ(MojErrNone, obj.get(key, value, found)) << key;
	EXPECT_TRUE(found) << key;
	return value.data();
}

// A service with no bus behind it: a subscription only has to be recorded.
class FakeService : public MojService
{
public:
	MojErr createRequest(MojRefCountedPtr<MojServiceRequest>&) override { return MojErrNotImplemented; }
	MojErr dispatch() override { return MojErrNone; }

protected:
	MojErr sendImpl(MojServiceRequest*, const MojChar*, const MojChar*, Token&) override { return MojErrNotImplemented; }
	MojErr cancelImpl(MojServiceRequest*) override { return MojErrNone; }
	MojErr dispatchReplyImpl(MojServiceRequest*, MojServiceMessage*, MojObject&, MojErr) override { return MojErrNone; }
	MojErr enableSubscriptionImpl(MojServiceMessage*) override { return MojErrNone; }
	MojErr removeSubscriptionImpl(MojServiceMessage*) override { return MojErrNone; }
};

FakeService& fakeService()
{
	static FakeService service;
	return service;
}

// A service message that keeps what is replied to it, and can be cancelled as a client going
// away cancels one.
class FakeMessage : public MojServiceMessage
{
public:
	FakeMessage() : MojServiceMessage(&fakeService(), NULL) {}
	// Closed as the bus closes a finished message, which takes it out of the service's
	// subscriptions: the service walks those when it goes, at exit.
	~FakeMessage() override { (void)close(); }
	MojObjectVisitor& writer() override { return m_builder; }
	const MojChar* appId() const override { return _T("org.webosports.app.messaging"); }
	const MojChar* category() const override { return _T("/"); }
	const MojChar* method() const override { return _T("watchTyping"); }
	const MojChar* senderAddress() const override { return _T("test"); }
	const MojChar* senderId() const override { return _T("test"); }
	const MojChar* senderExePath() const override { return _T(""); }
	const MojChar* senderTrustLevel() const override { return _T("dev"); }
	MojErr payload(MojObjectVisitor&) const override { return MojErrNone; }
	MojErr payload(MojObject&) const override { return MojErrNone; }
	Token token() const override { return 1; }
	bool hasData() const override { return false; }
	const MojChar* queue() const override { return _T(""); }

	MojErr cancel() { return dispatchCancel(); }

	std::vector<MojObject> replies;

protected:
	MojErr replyImpl() override
	{
		replies.push_back(m_builder.object());
		m_builder.reset();
		return MojErrNone;
	}

private:
	MojObjectBuilder m_builder;
};

} // namespace

TEST(TypingState, KeepsOneStatePerBuddyAndReportsOnlyChanges)
{
	TypingState s;
	EXPECT_TRUE(s.set("type_telegram", "me", "anna", TypingState::Typing));
	EXPECT_FALSE(s.set("type_telegram", "me", "anna", TypingState::Typing)) << "the same again is no change";
	EXPECT_TRUE(s.set("type_telegram", "me", "anna", TypingState::Paused));
	EXPECT_TRUE(s.set("type_whatsapp", "me", "anna", TypingState::Typing)) << "another service, another buddy";
	ASSERT_EQ(2u, s.snapshot().size());
	EXPECT_TRUE(s.set("type_telegram", "me", "anna", TypingState::None));
	EXPECT_FALSE(s.set("type_telegram", "me", "anna", TypingState::None)) << "not typing was not typing";
	ASSERT_EQ(1u, s.snapshot().size());
	EXPECT_EQ("type_whatsapp", s.snapshot()[0].serviceName);
}

TEST(TypingState, IgnoresAnEntryWithAPartMissing)
{
	TypingState s;
	EXPECT_FALSE(s.set("", "me", "anna", TypingState::Typing));
	EXPECT_FALSE(s.set("type_telegram", "", "anna", TypingState::Typing));
	EXPECT_FALSE(s.set("type_telegram", "me", "", TypingState::Typing));
	EXPECT_TRUE(s.snapshot().empty());
}

TEST(TypingState, KeysCannotRunIntoEachOther)
{
	// "a" + "bc" and "ab" + "c" are two accounts and two buddies, not one.
	TypingState s;
	EXPECT_TRUE(s.set("svc", "a", "bc", TypingState::Typing));
	EXPECT_TRUE(s.set("svc", "ab", "c", TypingState::Typing));
	EXPECT_EQ(2u, s.snapshot().size());
}

TEST(TypingState, SigningOffClearsOnlyThatAccount)
{
	TypingState s;
	s.set("type_telegram", "me", "anna", TypingState::Typing);
	s.set("type_telegram", "me", "ben", TypingState::Paused);
	s.set("type_telegram", "other", "anna", TypingState::Typing);
	std::vector<TypingState::Entry> dropped = s.clearAccount("type_telegram", "me");
	ASSERT_EQ(2u, dropped.size());
	EXPECT_EQ(TypingState::None, dropped[0].state) << "each dropped typer is reported as stopped";
	EXPECT_EQ(TypingState::None, dropped[1].state);
	ASSERT_EQ(1u, s.snapshot().size());
	EXPECT_EQ("other", s.snapshot()[0].username);
}

TEST(TypingState, NamesAndParsesTheStates)
{
	EXPECT_STREQ("typing", TypingState::name(TypingState::Typing));
	EXPECT_STREQ("paused", TypingState::name(TypingState::Paused));
	EXPECT_STREQ("none", TypingState::name(TypingState::None));
	EXPECT_EQ(TypingState::Typing, TypingState::parse("typing"));
	EXPECT_EQ(TypingState::Paused, TypingState::parse("paused"));
	EXPECT_EQ(TypingState::None, TypingState::parse("none"));
	EXPECT_EQ(TypingState::None, TypingState::parse("TYPING")) << "anything else is not typing";
	EXPECT_EQ(TypingState::None, TypingState::parse(NULL));
}

TEST(TypingChannel, EventPayloadCarriesWhoAndState)
{
	TypingState::Entry e;
	e.serviceName = "type_whatsapp";
	e.username = "+31600000000";
	e.from = "+31611111111";
	e.state = TypingState::Paused;
	MojObject out;
	ASSERT_EQ(MojErrNone, TypingChannel::eventPayload(e, out));
	bool subscribed = false;
	EXPECT_TRUE(out.get(_T("subscribed"), subscribed));
	EXPECT_TRUE(subscribed);
	EXPECT_EQ("type_whatsapp", str(out, "serviceName"));
	EXPECT_EQ("+31600000000", str(out, "username"));
	EXPECT_EQ("+31611111111", str(out, "from"));
	EXPECT_EQ("paused", str(out, "state"));
}

TEST(TypingChannel, SubscriberGetsTheTypersThenEachChangeUntilItGoes)
{
	TypingChannel channel(NULL);
	channel.publish("type_telegram", "me", "anna", TypingState::Typing);

	MojRefCountedPtr<FakeMessage> msg(new FakeMessage());
	ASSERT_EQ(MojErrNone, channel.subscribe(msg.get()));
	ASSERT_EQ(1u, msg->replies.size());
	MojObject typing;
	ASSERT_TRUE(msg->replies[0].get(_T("typing"), typing));
	ASSERT_EQ(1u, typing.size());
	MojObject first;
	ASSERT_TRUE(typing.at(0, first));
	EXPECT_EQ("anna", str(first, "from"));
	EXPECT_EQ("typing", str(first, "state"));

	channel.publish("type_telegram", "me", "anna", TypingState::Typing);   // no change
	channel.publish("type_telegram", "me", "anna", TypingState::None);
	channel.publish("type_telegram", "me", "ben", TypingState::Typing);
	channel.clearAccount("type_telegram", "me");
	ASSERT_EQ(4u, msg->replies.size()) << "snapshot, anna stopped, ben typing, ben stopped on sign-off";
	EXPECT_EQ("none", str(msg->replies[1], "state"));
	EXPECT_EQ("anna", str(msg->replies[1], "from"));
	EXPECT_EQ("typing", str(msg->replies[2], "state"));
	EXPECT_EQ("ben", str(msg->replies[2], "from"));
	EXPECT_EQ("none", str(msg->replies[3], "state"));

	ASSERT_EQ(MojErrNone, msg->cancel());
	channel.publish("type_telegram", "me", "carl", TypingState::Typing);
	EXPECT_EQ(4u, msg->replies.size()) << "nothing to a subscriber that went away";
}

TEST(TypingChannel, IgnoresNullNames)
{
	TypingChannel channel(NULL);
	MojRefCountedPtr<FakeMessage> msg(new FakeMessage());
	ASSERT_EQ(MojErrNone, channel.subscribe(msg.get()));
	channel.publish(NULL, "me", "anna", TypingState::Typing);
	channel.publish("type_telegram", NULL, "anna", TypingState::Typing);
	channel.publish("type_telegram", "me", NULL, TypingState::Typing);
	channel.clearAccount(NULL, "me");
	EXPECT_EQ(1u, msg->replies.size());
	EXPECT_EQ(MojErrNone, msg->cancel());   // the client goes, as it does on the bus
}

TEST(TypingChannel, OutlivedByItsServiceItIsNotCalledBack)
{
	// The service closes its open subscriptions when it goes, which at shutdown is after the
	// channel was deleted: their cancel must not reach the deleted channel.
	MojRefCountedPtr<FakeMessage> msg(new FakeMessage());
	{
		TypingChannel channel(NULL);
		ASSERT_EQ(MojErrNone, channel.subscribe(msg.get()));
	}
	EXPECT_EQ(MojErrNone, msg->cancel());
}
