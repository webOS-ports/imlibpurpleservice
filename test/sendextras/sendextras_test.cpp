/*
 * sendextras_test.cpp
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport).
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free Software
 * Foundation.
 *
 * SendExtras.cpp: what an outgoing message carries besides its text (a location, a contact card,
 * mentions), read off the immessage, written as the strings a prpl reads from the conversation,
 * and put there and taken off again around a send. WebosSendSignals.cpp: the edit, delete and
 * group signals, on libpurple's own signal system, as a prpl connects to them.
 */

#include "SendExtras.h"
#include "WebosSendSignals.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <map>
#include <string>

using SendExtras::Mention;

namespace {

MojObject json(const char* text)
{
	MojObject obj;
	EXPECT_EQ(MojErrNone, obj.fromJson(text)) << text;
	return obj;
}

const char* const Card = "BEGIN:VCARD\r\nVERSION:3.0\r\nFN:Anna Kowalski\r\nTEL;TYPE=CELL:+31611111111\r\nEND:VCARD\r\n";

} // namespace

TEST(SendExtras, LocationIsLatitudeLongitudeAndName)
{
	EXPECT_EQ("52.3731000\x1f" "4.8925000\x1f" "Dam", SendExtras::location(52.3731, 4.8925, "Dam"));
	EXPECT_EQ("-33.8567844\x1f" "151.2152967\x1f", SendExtras::location(-33.8567844, 151.2152967, ""));
	EXPECT_EQ("90.0000000\x1f" "-180.0000000\x1f" "pole", SendExtras::location(90, -180, "pole")) << "the edges are on the earth";
}

TEST(SendExtras, NoLocationOffTheEarth)
{
	EXPECT_EQ("", SendExtras::location(90.0001, 0, "x"));
	EXPECT_EQ("", SendExtras::location(0, -180.0001, "x"));
	EXPECT_EQ("", SendExtras::location(std::nan(""), 0, "x"));
	EXPECT_EQ("", SendExtras::location(0, std::numeric_limits<double>::infinity(), "x"));
}

TEST(SendExtras, ANameCannotSplitTheLocation)
{
	EXPECT_EQ("1.0000000\x1f" "2.0000000\x1f" "ab", SendExtras::location(1, 2, "a\x1f" "b"));
}

TEST(SendExtras, ContactIsAVCardOrNothing)
{
	EXPECT_EQ(Card, SendExtras::contact(Card));
	EXPECT_EQ("begin:vcard\nFN:x\nend:vcard", SendExtras::contact("begin:vcard\nFN:x\nend:vcard")) << "any case, no last line end";
	EXPECT_EQ("", SendExtras::contact("Anna +31611111111"));
	EXPECT_EQ("", SendExtras::contact("BEGIN:VCARD\r\nFN:Anna\r\n")) << "a card cut short";
	EXPECT_EQ("", SendExtras::contact(""));
	EXPECT_EQ("", SendExtras::contact("END:VCARD"));
}

TEST(SendExtras, MentionsAreIdAndNameRecords)
{
	std::vector<Mention> list;
	Mention anna = { "31611111111@s.whatsapp.net", "Anna" };
	Mention noId = { "", "Nobody" };
	Mention ben = { "3162\x1e" "2222222@s.whatsapp.net", "Ben\x1f" };
	list.push_back(anna);
	list.push_back(noId);
	list.push_back(ben);
	EXPECT_EQ("31611111111@s.whatsapp.net\x1f" "Anna\x1e" "31622222222@s.whatsapp.net\x1f" "Ben",
			SendExtras::mentions(list)) << "the one without an id left out, separators out of the fields";
	EXPECT_EQ("", SendExtras::mentions(std::vector<Mention>()));
}

TEST(SendExtras, ReadOffTheMessage)
{
	SendExtras::Extras e = SendExtras::fromMessage(json(
		"{\"messageText\":\"x\",\"location\":{\"latitude\":52,\"longitude\":4.5,\"name\":\"Here\"},"
		"\"contactCard\":{\"vcard\":\"BEGIN:VCARD\\nFN:A\\nEND:VCARD\"},"
		"\"mentions\":[{\"id\":\"a1\",\"name\":\"A\"},7,{\"name\":\"no id\"},{\"id\":\"b2\",\"name\":\"B\"}]}"));
	EXPECT_EQ("52.0000000\x1f" "4.5000000\x1f" "Here", e.location) << "a whole number is a coordinate too";
	EXPECT_EQ("BEGIN:VCARD\nFN:A\nEND:VCARD", e.contact);
	EXPECT_EQ("a1\x1f" "A\x1e" "b2\x1f" "B", e.mentions);
	EXPECT_FALSE(e.empty());
}

TEST(SendExtras, NothingUsableIsNothing)
{
	EXPECT_TRUE(SendExtras::fromMessage(json("{\"messageText\":\"x\"}")).empty());
	EXPECT_TRUE(SendExtras::fromMessage(json(
		"{\"location\":{\"latitude\":\"52\",\"longitude\":4},\"contactCard\":\"BEGIN:VCARD\","
		"\"mentions\":{\"id\":\"a\",\"name\":\"A\"}}")).empty()) << "a string latitude, a card not in an object, mentions not a list";
	EXPECT_TRUE(SendExtras::fromMessage(json("{\"location\":{\"latitude\":52}}")).empty()) << "no longitude";
}

TEST(SendExtras, MembersSplitAndJoin)
{
	std::vector<std::string> m = SendExtras::splitMembers("a\x1f\x1f" "b\x1f");
	ASSERT_EQ(2u, m.size());
	EXPECT_EQ("a", m[0]);
	EXPECT_EQ("b", m[1]);
	EXPECT_TRUE(SendExtras::splitMembers("").empty());
	EXPECT_EQ(1u, SendExtras::splitMembers("only").size());
	m.push_back("");
	m.push_back("c\x1f" "d");
	EXPECT_EQ("a\x1f" "b\x1f" "cd", SendExtras::joinMembers(m));
}

namespace {

// A conversation's data, as libpurple keeps it: values it does not free.
struct MapData {
	std::map<std::string, gpointer> values;
	gpointer get(const char* key) const
	{
		std::map<std::string, gpointer>::const_iterator it = values.find(key);
		return it == values.end() ? NULL : it->second;
	}
	void set(const char* key, gpointer value) { values[key] = value; }
	std::string str(const char* key) const
	{
		gpointer v = get(key);
		return v ? std::string(static_cast<const char*>(v)) : std::string("(none)");
	}
};

} // namespace

TEST(SendExtras, OnTheConversationForTheSendOnly)
{
	SendExtras::Extras e;
	e.location = "1\x1f" "2\x1f";
	e.mentions = "a\x1f" "A";
	MapData data;
	SendExtras::set(data, &e);
	EXPECT_EQ(e.location, data.str(SendExtras::LocationKey));
	EXPECT_EQ("(none)", data.str(SendExtras::ContactKey)) << "only what the message has";
	EXPECT_EQ(e.mentions, data.str(SendExtras::MentionsKey));
	EXPECT_STREQ("webos-send-location", SendExtras::LocationKey);
	EXPECT_STREQ("webos-send-contact", SendExtras::ContactKey);
	EXPECT_STREQ("webos-send-mentions", SendExtras::MentionsKey);

	SendExtras::clear(data);
	EXPECT_EQ(NULL, data.get(SendExtras::LocationKey));
	EXPECT_EQ(NULL, data.get(SendExtras::MentionsKey));
	SendExtras::clear(data);   // nothing there: nothing to free
}

TEST(SendExtras, NothingLeftFromTheMessageBefore)
{
	SendExtras::Extras first;
	first.contact = Card;
	SendExtras::Extras second;
	second.mentions = "b\x1f" "B";
	MapData data;
	SendExtras::set(data, &first);
	SendExtras::set(data, &second);   // as if the send between had not cleared it
	EXPECT_EQ(NULL, data.get(SendExtras::ContactKey)) << "the card of the message before would go along";
	EXPECT_EQ("b\x1f" "B", data.str(SendExtras::MentionsKey));
	SendExtras::set(data, NULL);
	EXPECT_EQ(NULL, data.get(SendExtras::MentionsKey));
	// Under ASan/valgrind: nothing set here is leaked.
}

namespace {

PurpleAccount* const Ours = reinterpret_cast<PurpleAccount*>(0x1000);
PurpleAccount* const Theirs = reinterpret_cast<PurpleAccount*>(0x2000);

struct Seen {
	int calls;
	std::string args;
};

gboolean edit_cb(PurpleAccount* account, const char* chat, const char* id, const char* text, gpointer data)
{
	Seen* seen = static_cast<Seen*>(data);
	seen->calls++;
	seen->args = std::string(chat) + "|" + id + "|" + text;
	return account == Ours;
}

gboolean three_cb(PurpleAccount* account, const char* a, const char* b, gpointer data)
{
	Seen* seen = static_cast<Seen*>(data);
	seen->calls++;
	seen->args = std::string(a) + "|" + b;
	return account == Ours;
}

gboolean leave_cb(PurpleAccount* account, const char* chat, gpointer data)
{
	Seen* seen = static_cast<Seen*>(data);
	seen->calls++;
	seen->args = chat;
	return account == Ours;
}

// Another prpl's handler on a three-argument signal: never its account.
gboolean other_prpl_cb(PurpleAccount*, const char*, const char*, gpointer data)
{
	static_cast<Seen*>(data)->calls++;
	return FALSE;
}

class Signals : public ::testing::Test
{
protected:
	void SetUp() override
	{
		purple_signals_init();
		handle = purple_conversations_get_handle();
		WebosSendSignals::registerSignals(handle);
	}
	void TearDown() override
	{
		purple_signals_disconnect_by_handle(&owner);
		purple_signals_disconnect_by_handle(&other);
		purple_signals_unregister_by_instance(handle);
		purple_signals_uninit();
	}
	void* handle = NULL;
	int owner = 0;
	int other = 0;
};

} // namespace

TEST_F(Signals, NoPrplNoTaker)
{
	EXPECT_FALSE(WebosSendSignals::edit(handle, Ours, "anna", "m1", "new"));
	EXPECT_FALSE(WebosSendSignals::deleteMessage(handle, Ours, "anna", "m1"));
	EXPECT_FALSE(WebosSendSignals::createGroup(handle, Ours, "G", "a"));
	EXPECT_FALSE(WebosSendSignals::inviteToGroup(handle, Ours, "g", "a"));
	EXPECT_FALSE(WebosSendSignals::removeFromGroup(handle, Ours, "g", "a"));
	EXPECT_FALSE(WebosSendSignals::renameGroup(handle, Ours, "g", "New name"));
	EXPECT_FALSE(WebosSendSignals::leaveGroup(handle, Ours, "g"));
	EXPECT_FALSE(WebosSendSignals::openChat(handle, Ours, "g"));
}

TEST_F(Signals, EditGoesToThePrplWithItsArguments)
{
	Seen seen = { 0, "" };
	purple_signal_connect(handle, "webos-im-send-edit", &owner, PURPLE_CALLBACK(edit_cb), &seen);
	EXPECT_TRUE(WebosSendSignals::edit(handle, Ours, "anna", "chat:42", "fixed"));
	EXPECT_EQ("anna|chat:42|fixed", seen.args);
	EXPECT_FALSE(WebosSendSignals::edit(handle, Theirs, "anna", "chat:42", "fixed")) << "not its account";
	EXPECT_EQ(2, seen.calls);
}

TEST_F(Signals, OnePrplDecliningDoesNotHideTheOneThatTakesIt)
{
	// Several prpls connect to the same signal; the one for another account is called first
	// (handlers run in ascending priority) and returns FALSE.
	Seen others = { 0, "" };
	Seen seen = { 0, "" };
	purple_signal_connect_priority(handle, "webos-im-send-delete", &other, PURPLE_CALLBACK(other_prpl_cb), &others,
			PURPLE_SIGNAL_PRIORITY_LOWEST);
	purple_signal_connect(handle, "webos-im-send-delete", &owner, PURPLE_CALLBACK(three_cb), &seen);
	EXPECT_TRUE(WebosSendSignals::deleteMessage(handle, Ours, "anna", "m7"));
	EXPECT_EQ(1, others.calls);
	EXPECT_EQ(1, seen.calls);
	EXPECT_EQ("anna|m7", seen.args);
}

TEST_F(Signals, Groups)
{
	Seen create = { 0, "" }, invite = { 0, "" }, leave = { 0, "" };
	purple_signal_connect(handle, "webos-im-group-create", &owner, PURPLE_CALLBACK(three_cb), &create);
	purple_signal_connect(handle, "webos-im-group-invite", &owner, PURPLE_CALLBACK(three_cb), &invite);
	purple_signal_connect(handle, "webos-im-group-leave", &owner, PURPLE_CALLBACK(leave_cb), &leave);
	EXPECT_TRUE(WebosSendSignals::createGroup(handle, Ours, "Trip", "a\x1f" "b"));
	EXPECT_EQ("Trip|a\x1f" "b", create.args);
	EXPECT_TRUE(WebosSendSignals::inviteToGroup(handle, Ours, "g1", "c"));
	EXPECT_EQ("g1|c", invite.args);
	EXPECT_TRUE(WebosSendSignals::leaveGroup(handle, Ours, "g1"));
	EXPECT_EQ("g1", leave.args);
	EXPECT_FALSE(WebosSendSignals::leaveGroup(handle, Theirs, "g1"));
}

TEST_F(Signals, RemoveFromGroupIsItsOwnSignal)
{
	// Removing must not reach a prpl as an invite (or the other way round): a member asked to be
	// removed would be added.
	Seen invite = { 0, "" }, remove = { 0, "" };
	purple_signal_connect(handle, "webos-im-group-invite", &owner, PURPLE_CALLBACK(three_cb), &invite);
	purple_signal_connect(handle, "webos-im-group-remove", &owner, PURPLE_CALLBACK(three_cb), &remove);
	EXPECT_TRUE(WebosSendSignals::removeFromGroup(handle, Ours, "g1", "c\x1f" "d"));
	EXPECT_EQ("g1|c\x1f" "d", remove.args);
	EXPECT_EQ(0, invite.calls);
	EXPECT_TRUE(WebosSendSignals::inviteToGroup(handle, Ours, "g1", "e"));
	EXPECT_EQ("g1|e", invite.args);
	EXPECT_EQ(1, remove.calls);
}

TEST_F(Signals, RemoveFromGroupNotTakenForAnotherAccount)
{
	Seen others = { 0, "" };
	purple_signal_connect(handle, "webos-im-group-remove", &other, PURPLE_CALLBACK(other_prpl_cb), &others);
	EXPECT_FALSE(WebosSendSignals::removeFromGroup(handle, Ours, "g1", "c"));
	EXPECT_EQ(1, others.calls);
}

TEST_F(Signals, RenameGroupIsItsOwnSignal)
{
	// A rename carries the group and its new name, and reaches no prpl as a create (which would
	// make a new group of that name) or as any other group change.
	Seen create = { 0, "" }, invite = { 0, "" }, remove = { 0, "" }, rename = { 0, "" };
	purple_signal_connect(handle, "webos-im-group-create", &owner, PURPLE_CALLBACK(three_cb), &create);
	purple_signal_connect(handle, "webos-im-group-invite", &owner, PURPLE_CALLBACK(three_cb), &invite);
	purple_signal_connect(handle, "webos-im-group-remove", &owner, PURPLE_CALLBACK(three_cb), &remove);
	purple_signal_connect(handle, "webos-im-group-rename", &owner, PURPLE_CALLBACK(three_cb), &rename);
	EXPECT_TRUE(WebosSendSignals::renameGroup(handle, Ours, "g1", "Trip to Rome"));
	EXPECT_EQ(1, rename.calls);
	EXPECT_EQ("g1|Trip to Rome", rename.args);
	EXPECT_EQ(0, create.calls);
	EXPECT_EQ(0, invite.calls);
	EXPECT_EQ(0, remove.calls);
	EXPECT_TRUE(WebosSendSignals::createGroup(handle, Ours, "Trip", "a"));
	EXPECT_EQ(1, rename.calls);
}

TEST_F(Signals, RenameGroupNotTakenForAnotherAccount)
{
	// Today's prpls: none takes the rename for an account that is not its own, so the command fails.
	Seen others = { 0, "" }, rename = { 0, "" };
	purple_signal_connect(handle, "webos-im-group-rename", &other, PURPLE_CALLBACK(other_prpl_cb), &others);
	EXPECT_FALSE(WebosSendSignals::renameGroup(handle, Ours, "g1", "x"));
	EXPECT_EQ(1, others.calls);
	purple_signal_connect(handle, "webos-im-group-rename", &owner, PURPLE_CALLBACK(three_cb), &rename);
	EXPECT_FALSE(WebosSendSignals::renameGroup(handle, Theirs, "g1", "x")) << "not its account";
	EXPECT_EQ(1, rename.calls);
}

TEST_F(Signals, OpenChatIsItsOwnSignal)
{
	// Opening a chat for its members is no join, no leave and no other group change.
	Seen open = { 0, "" }, leave = { 0, "" };
	purple_signal_connect(handle, "webos-im-group-leave", &owner, PURPLE_CALLBACK(leave_cb), &leave);
	purple_signal_connect(handle, "webos-im-chat-open", &owner, PURPLE_CALLBACK(leave_cb), &open);
	EXPECT_TRUE(WebosSendSignals::openChat(handle, Ours, "1410895923755880"));
	EXPECT_EQ(1, open.calls);
	EXPECT_EQ("1410895923755880", open.args);
	EXPECT_EQ(0, leave.calls);
	EXPECT_TRUE(WebosSendSignals::leaveGroup(handle, Ours, "g1"));
	EXPECT_EQ(1, open.calls);
}

// Another prpl's handler on a two-argument signal: never its account.
static gboolean other_prpl_two_cb(PurpleAccount*, const char*, gpointer data)
{
	static_cast<Seen*>(data)->calls++;
	return FALSE;
}

TEST_F(Signals, OpenChatNotTakenForAnotherAccount)
{
	Seen others = { 0, "" }, open = { 0, "" };
	purple_signal_connect(handle, "webos-im-chat-open", &other, PURPLE_CALLBACK(other_prpl_two_cb), &others);
	EXPECT_FALSE(WebosSendSignals::openChat(handle, Ours, "g1"));
	EXPECT_EQ(1, others.calls);
	purple_signal_connect(handle, "webos-im-chat-open", &owner, PURPLE_CALLBACK(leave_cb), &open);
	EXPECT_FALSE(WebosSendSignals::openChat(handle, Theirs, "g1")) << "not its account";
	EXPECT_TRUE(WebosSendSignals::openChat(handle, Ours, "g1"));
	EXPECT_EQ(2, open.calls);
}
