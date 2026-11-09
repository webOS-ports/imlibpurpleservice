/*
 * chatmembers_test.cpp
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport).
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free Software
 * Foundation.
 *
 * ChatMembers.cpp: the members of a group chat as getChatMembers answers with them.
 */

#include "ChatMembers.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using ChatMembers::Member;

namespace {

Member member(const char* address, const char* name, const char* phone = "")
{
	Member m;
	m.address = address;
	m.name = name;
	m.phone = phone;
	return m;
}

std::string json(const MojObject& obj)
{
	MojString s;
	EXPECT_EQ(MojErrNone, obj.toJson(s));
	return s.data();
}

} // namespace

TEST(ChatMembers, KeepsEachAddressOnceInOrder)
{
	std::vector<Member> in;
	in.push_back(member("+31611111111", "Anna"));
	in.push_back(member("+46701234567", "Marta"));
	in.push_back(member("+31611111111", "Anna K"));
	std::vector<Member> out = ChatMembers::clean(in, "");
	ASSERT_EQ(2u, out.size());
	EXPECT_EQ("+31611111111", out[0].address);
	EXPECT_EQ("Anna", out[0].name) << "the first name stays";
	EXPECT_EQ("+46701234567", out[1].address);
}

TEST(ChatMembers, LeavesUsOut)
{
	std::vector<Member> in;
	in.push_back(member("+15555550100", "Me"));
	in.push_back(member("+46701234567", "Marta"));
	std::vector<Member> out = ChatMembers::clean(in, "+15555550100");
	ASSERT_EQ(1u, out.size());
	EXPECT_EQ("+46701234567", out[0].address);
}

TEST(ChatMembers, NoOneWithoutAnAddress)
{
	std::vector<Member> in;
	in.push_back(member("", "Ghost"));
	in.push_back(member(" \x1f ", "Ghost"));
	EXPECT_TRUE(ChatMembers::clean(in, "").empty());
}

TEST(ChatMembers, TheAddressIsTheNameWhenThereIsNone)
{
	std::vector<Member> in;
	in.push_back(member("id1234", ""));
	in.push_back(member("id1234", "Bob"));
	in.push_back(member("+491701234567", "  "));
	std::vector<Member> out = ChatMembers::clean(in, "");
	ASSERT_EQ(2u, out.size());
	EXPECT_EQ("Bob", out[0].name) << "a later name fills one that had none";
	EXPECT_EQ("+491701234567", out[1].name);
}

TEST(ChatMembers, NamesCannotSplitTheAppsRecords)
{
	std::vector<Member> in;
	in.push_back(member("+31611111111", " An\x1fna\x1e "));
	std::vector<Member> out = ChatMembers::clean(in, "");
	ASSERT_EQ(1u, out.size());
	EXPECT_EQ("Anna", out[0].name);
}

TEST(ChatMembers, ThePayload)
{
	std::vector<Member> in;
	in.push_back(member("+31611111111", "Anna \"A\""));
	MojObject out;
	ASSERT_EQ(MojErrNone, ChatMembers::toPayload(in, true, out));
	EXPECT_EQ("{\"inChat\":true,\"members\":[{\"address\":\"+31611111111\",\"name\":\"Anna \\\"A\\\"\"}],\"returnValue\":true}",
		json(out));
}

TEST(ChatMembers, AnEmptyPayloadStillHasAList)
{
	MojObject out;
	ASSERT_EQ(MojErrNone, ChatMembers::toPayload(std::vector<Member>(), false, out));
	EXPECT_EQ("{\"inChat\":false,\"members\":[],\"returnValue\":true}", json(out));
}

TEST(ChatMembers, APhoneIsKeptWithItsMember)
{
	std::vector<Member> in;
	in.push_back(member("123456789012345@lid", "Anna", " +31611111111 "));
	in.push_back(member("223456789012345@lid", "Anna", ""));
	std::vector<Member> out = ChatMembers::clean(in, "");
	ASSERT_EQ(2u, out.size());
	EXPECT_EQ("+31611111111", out[0].phone);
	EXPECT_EQ("", out[1].phone);
}

TEST(ChatMembers, OnlyANumberIsAPhone)
{
	std::vector<Member> in;
	in.push_back(member("a@lid", "A", "31611111111"));
	in.push_back(member("b@lid", "B", "+"));
	in.push_back(member("c@lid", "C", "+3161 111"));
	in.push_back(member("d@lid", "D", "123456789012345@lid"));
	in.push_back(member("e@lid", "E", "+3161\x1f" "1111"));
	std::vector<Member> out = ChatMembers::clean(in, "");
	ASSERT_EQ(5u, out.size());
	for (size_t i = 0; i < out.size(); ++i)
		EXPECT_EQ("", out[i].phone) << out[i].address;
}

TEST(ChatMembers, ALaterPhoneFillsOneThatHadNone)
{
	std::vector<Member> in;
	in.push_back(member("a@lid", "Anna", ""));
	in.push_back(member("a@lid", "Anna", "+31611111111"));
	in.push_back(member("a@lid", "Anna", "+46701234567"));
	std::vector<Member> out = ChatMembers::clean(in, "");
	ASSERT_EQ(1u, out.size());
	EXPECT_EQ("+31611111111", out[0].phone) << "the first phone stays";
}

TEST(ChatMembers, ThePayloadHasThePhoneOnlyWhenKnown)
{
	std::vector<Member> in;
	in.push_back(member("123456789012345@lid", "Anna", "+31611111111"));
	in.push_back(member("223456789012345@lid", "Marta", ""));
	MojObject out;
	ASSERT_EQ(MojErrNone, ChatMembers::toPayload(in, true, out));
	EXPECT_EQ("{\"inChat\":true,\"members\":["
		"{\"address\":\"123456789012345@lid\",\"name\":\"Anna\",\"phone\":\"+31611111111\"},"
		"{\"address\":\"223456789012345@lid\",\"name\":\"Marta\"}],\"returnValue\":true}",
		json(out));
}

TEST(ChatMembers, ThePhoneAttributeIsWhatThePluginSets)
{
	// purple-combined's glue/gowhatsapp.h: GOWHATSAPP_CHAT_BUDDY_PHONE.
	EXPECT_STREQ("phone", ChatMembers::PHONE_ATTRIBUTE);
}

TEST(ChatMembers, AnyMemberListedAgainIsFilledIn)
{
	std::vector<Member> in;
	in.push_back(member("a@lid", "Anna", "+31611111111"));
	in.push_back(member("b@lid", "", ""));
	in.push_back(member("b@lid", "Marta", "+46701234567"));
	std::vector<Member> out = ChatMembers::clean(in, "");
	ASSERT_EQ(2u, out.size());
	EXPECT_EQ("Marta", out[1].name);
	EXPECT_EQ("+46701234567", out[1].phone);
	EXPECT_EQ("+31611111111", out[0].phone) << "another member's phone is left alone";
}

// Opening a chat that has no conversation yet (getChatMembers -> serv_join_chat): only where the
// prpl's join_chat just opens it and puts its members on it.

TEST(ChatMembers, TelegramGroupsAreOpenedForTheirMembers)
{
	EXPECT_TRUE(ChatMembers::joinOpensMembers("prpl-telegram", "chat-1001234567890"));
}

TEST(ChatMembers, WhatsAppGroupsAreOpenedForTheirMembers)
{
	EXPECT_TRUE(ChatMembers::joinOpensMembers("prpl-hehoe-whatsmeow", "120363012345678901@g.us"));
}

TEST(ChatMembers, AWhatsAppChannelIsNotOpened)
{
	// Its join fetches the Channel's posts.
	EXPECT_FALSE(ChatMembers::joinOpensMembers("prpl-hehoe-whatsmeow", "120363098765432109@newsletter"));
	EXPECT_FALSE(ChatMembers::joinOpensMembers("prpl-hehoe-whatsmeow", "@newsletter"));
	EXPECT_FALSE(ChatMembers::joinOpensMembers("prpl-hehoe-whatsmeow", "status@broadcast"));
	// Only the end says it is a Channel, as gowhatsapp_join_chat tests it (g_str_has_suffix).
	EXPECT_TRUE(ChatMembers::joinOpensMembers("prpl-hehoe-whatsmeow", "newsletter@g.us"));
	EXPECT_TRUE(ChatMembers::joinOpensMembers("prpl-hehoe-whatsmeow", "1@newsletter.g.us"));
}

TEST(ChatMembers, DiscordAndTheRestAreNotOpened)
{
	// purple-discord's join_chat fetches the channel's messages each time.
	EXPECT_FALSE(ChatMembers::joinOpensMembers("prpl-discord", "1410895923755880"));
	EXPECT_FALSE(ChatMembers::joinOpensMembers("prpl-eionrobb-discord", "1410895923755880"));
	EXPECT_FALSE(ChatMembers::joinOpensMembers("prpl-irc", "#luneos"));
	EXPECT_FALSE(ChatMembers::joinOpensMembers("prpl-gometa", "123"));
	EXPECT_FALSE(ChatMembers::joinOpensMembers("", "chat-1"));
	// A prefix of an allowed id is another prpl.
	EXPECT_FALSE(ChatMembers::joinOpensMembers("prpl-telegram-x", "chat-1"));
	EXPECT_FALSE(ChatMembers::joinOpensMembers("prpl-tele", "chat-1"));
}

TEST(ChatMembers, NoChatNoJoin)
{
	EXPECT_FALSE(ChatMembers::joinOpensMembers("prpl-telegram", ""));
	EXPECT_FALSE(ChatMembers::joinOpensMembers("prpl-hehoe-whatsmeow", ""));
}

// How a chat with no conversation is opened (openWay): joined where the join only opens it,
// otherwise asked for with webos-im-chat-open, which the prpl that owns it takes or not.

TEST(ChatMembers, AJoiningPrplsChatIsJoinedFromTheBuddyList)
{
	EXPECT_EQ(ChatMembers::OpenByJoin, ChatMembers::openWay("prpl-telegram", "chat-1001234567890", true, true));
	EXPECT_EQ(ChatMembers::OpenByJoin, ChatMembers::openWay("prpl-hehoe-whatsmeow", "120363012345678901@g.us", true, true));
}

// Not in the buddy list: joined with the prpl's chat_info_defaults, for a name the prpl gives its
// groups only; otherwise left, and not asked the other way either.
TEST(ChatMembers, AJoiningPrplsGroupOutsideTheBuddyListIsJoinedFromDefaults)
{
	EXPECT_EQ(ChatMembers::OpenByJoinDefaults, ChatMembers::openWay("prpl-hehoe-whatsmeow", "120363012345678901@g.us", false, true));
	EXPECT_EQ(ChatMembers::OpenByJoinDefaults, ChatMembers::openWay("prpl-hehoe-whatsmeow", "31611111111-1500000000@g.us", false, true));
	EXPECT_EQ(ChatMembers::OpenByJoinDefaults, ChatMembers::openWay("prpl-telegram", "chat-1001234567890", false, true));
	EXPECT_EQ(ChatMembers::OpenByJoinDefaults, ChatMembers::openWay("prpl-telegram", "chat123456789", false, true));
	// Never joined: a Channel or the status broadcast (left to the signal, which purple-combined
	// does not take), or anything that is not a group's name.
	EXPECT_EQ(ChatMembers::OpenBySignal, ChatMembers::openWay("prpl-hehoe-whatsmeow", "120363098765432109@newsletter", false, true));
	EXPECT_EQ(ChatMembers::OpenBySignal, ChatMembers::openWay("prpl-hehoe-whatsmeow", "status@broadcast", false, true));
	EXPECT_EQ(ChatMembers::DontOpen, ChatMembers::openWay("prpl-hehoe-whatsmeow", "31611111111@s.whatsapp.net", false, true));
	EXPECT_EQ(ChatMembers::DontOpen, ChatMembers::openWay("prpl-telegram", "Some Group", false, true));
	// Signed off: nothing.
	EXPECT_EQ(ChatMembers::DontOpen, ChatMembers::openWay("prpl-hehoe-whatsmeow", "120363012345678901@g.us", false, false));
}

TEST(ChatMembers, DefaultsOnlyForAGroupNameThePrplParses)
{
	EXPECT_TRUE(ChatMembers::joinFromDefaults("prpl-hehoe-whatsmeow", "120363012345678901@g.us"));
	EXPECT_TRUE(ChatMembers::joinFromDefaults("prpl-hehoe-whatsmeow", "1@g.us"));
	EXPECT_TRUE(ChatMembers::joinFromDefaults("prpl-hehoe-whatsmeow", "31611111111-1500000000@g.us"));
	// gowhatsapp_go_query_group_participants makes an unparsable JID a fatal account error.
	EXPECT_FALSE(ChatMembers::joinFromDefaults("prpl-hehoe-whatsmeow", "@g.us"));
	EXPECT_FALSE(ChatMembers::joinFromDefaults("prpl-hehoe-whatsmeow", "g.us"));
	EXPECT_FALSE(ChatMembers::joinFromDefaults("prpl-hehoe-whatsmeow", ""));
	EXPECT_FALSE(ChatMembers::joinFromDefaults("prpl-hehoe-whatsmeow", "-1@g.us"));
	EXPECT_FALSE(ChatMembers::joinFromDefaults("prpl-hehoe-whatsmeow", "1-@g.us"));
	EXPECT_FALSE(ChatMembers::joinFromDefaults("prpl-hehoe-whatsmeow", "1-2-3@g.us"));
	EXPECT_FALSE(ChatMembers::joinFromDefaults("prpl-hehoe-whatsmeow", "12a4@g.us"));
	EXPECT_FALSE(ChatMembers::joinFromDefaults("prpl-hehoe-whatsmeow", "1 2@g.us"));
	EXPECT_FALSE(ChatMembers::joinFromDefaults("prpl-hehoe-whatsmeow", "120363012345678901@g.us.x"));
	EXPECT_FALSE(ChatMembers::joinFromDefaults("prpl-hehoe-whatsmeow", "120363012345678901@g.usx"));
	EXPECT_FALSE(ChatMembers::joinFromDefaults("prpl-hehoe-whatsmeow", "1@x@g.us"));
	EXPECT_FALSE(ChatMembers::joinFromDefaults("prpl-hehoe-whatsmeow", "120363098765432109@newsletter"));
	EXPECT_FALSE(ChatMembers::joinFromDefaults("prpl-hehoe-whatsmeow", "status@broadcast"));
	EXPECT_FALSE(ChatMembers::joinFromDefaults("prpl-hehoe-whatsmeow", "chat-1"));

	EXPECT_TRUE(ChatMembers::joinFromDefaults("prpl-telegram", "chat-1001234567890"));
	EXPECT_TRUE(ChatMembers::joinFromDefaults("prpl-telegram", "chat1"));
	EXPECT_TRUE(ChatMembers::joinFromDefaults("prpl-telegram", "chat-9223372036854775808"));
	EXPECT_FALSE(ChatMembers::joinFromDefaults("prpl-telegram", "chat-92233720368547758080"));
	EXPECT_FALSE(ChatMembers::joinFromDefaults("prpl-telegram", "chat"));
	EXPECT_FALSE(ChatMembers::joinFromDefaults("prpl-telegram", "chat-"));
	EXPECT_FALSE(ChatMembers::joinFromDefaults("prpl-telegram", "chat--1"));
	EXPECT_FALSE(ChatMembers::joinFromDefaults("prpl-telegram", "chat1x"));
	EXPECT_FALSE(ChatMembers::joinFromDefaults("prpl-telegram", "xchat1"));
	EXPECT_FALSE(ChatMembers::joinFromDefaults("prpl-telegram", "chan123"));
	EXPECT_FALSE(ChatMembers::joinFromDefaults("prpl-telegram", "1234567"));
	EXPECT_FALSE(ChatMembers::joinFromDefaults("prpl-telegram", "cha"));
	EXPECT_FALSE(ChatMembers::joinFromDefaults("prpl-telegram", "120363012345678901@g.us"));

	// No other prpl, whatever the name.
	EXPECT_FALSE(ChatMembers::joinFromDefaults("prpl-discord", "120363012345678901@g.us"));
	EXPECT_FALSE(ChatMembers::joinFromDefaults("prpl-gometa", "chat1"));
	EXPECT_FALSE(ChatMembers::joinFromDefaults("", "chat1"));
	EXPECT_FALSE(ChatMembers::joinFromDefaults("prpl-telegram-x", "chat1"));
}

TEST(ChatMembers, AnyOtherPrplIsAskedWithTheSignal)
{
	// purple-discord: its join_chat fetches the channel's messages, so it gets the signal, in the
	// buddy list or not.
	EXPECT_EQ(ChatMembers::OpenBySignal, ChatMembers::openWay("prpl-discord", "1410895923755880", true, true));
	EXPECT_EQ(ChatMembers::OpenBySignal, ChatMembers::openWay("prpl-discord", "1410895923755880", false, true));
	EXPECT_EQ(ChatMembers::OpenBySignal, ChatMembers::openWay("prpl-irc", "#luneos", true, true));
	// A WhatsApp Channel is not joined (its posts would be fetched); purple-combined takes no
	// webos-im-chat-open, so the signal leaves it as it is.
	EXPECT_EQ(ChatMembers::OpenBySignal, ChatMembers::openWay("prpl-hehoe-whatsmeow", "120363098765432109@newsletter", true, true));
	EXPECT_EQ(ChatMembers::OpenBySignal, ChatMembers::openWay("", "chat-1", true, true));
}

TEST(ChatMembers, NothingIsOpenedSignedOffOrWithoutAChat)
{
	EXPECT_EQ(ChatMembers::DontOpen, ChatMembers::openWay("prpl-telegram", "chat-1", true, false));
	EXPECT_EQ(ChatMembers::DontOpen, ChatMembers::openWay("prpl-discord", "1410895923755880", true, false));
	EXPECT_EQ(ChatMembers::DontOpen, ChatMembers::openWay("prpl-discord", "", true, true));
	EXPECT_EQ(ChatMembers::DontOpen, ChatMembers::openWay("prpl-telegram", "", true, true));
}

TEST(ChatMembers, StillJoiningOnlyAfterAJoinThatMayStillBringMembers)
{
	// Nothing was asked of the prpl: never joining, whatever was found.
	EXPECT_FALSE(ChatMembers::stillJoining(false, false, 0));
	EXPECT_FALSE(ChatMembers::stillJoining(false, true, 0));
	EXPECT_FALSE(ChatMembers::stillJoining(false, true, 3));
	// Asked: joining while there is no conversation, or no one on it yet.
	EXPECT_TRUE(ChatMembers::stillJoining(true, false, 0));
	EXPECT_TRUE(ChatMembers::stillJoining(true, true, 0));
	// Asked and the members are there: done.
	EXPECT_FALSE(ChatMembers::stillJoining(true, true, 2));
}

TEST(ChatMembers, ThePayloadSaysJoiningOnlyWhenJoining)
{
	std::vector<Member> none;
	MojObject joining;
	ASSERT_EQ(MojErrNone, ChatMembers::toPayload(none, false, true, joining));
	EXPECT_EQ("{\"inChat\":false,\"joining\":true,\"members\":[],\"returnValue\":true}", json(joining));

	std::vector<Member> one(1, member("+31611111111", "Anna"));
	MojObject done;
	ASSERT_EQ(MojErrNone, ChatMembers::toPayload(one, true, false, done));
	EXPECT_EQ("{\"inChat\":true,\"members\":[{\"address\":\"+31611111111\",\"name\":\"Anna\"}],\"returnValue\":true}",
			json(done));

	// The old form is the same as not joining.
	MojObject old;
	ASSERT_EQ(MojErrNone, ChatMembers::toPayload(one, true, old));
	EXPECT_EQ(json(done), json(old));
}
