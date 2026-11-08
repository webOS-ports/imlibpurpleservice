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

Member member(const char* address, const char* name)
{
	Member m;
	m.address = address;
	m.name = name;
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
