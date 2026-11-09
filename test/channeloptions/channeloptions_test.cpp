/*
 * channeloptions_test.cpp
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport).
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free Software
 * Foundation.
 *
 * ChannelOptions.cpp against the real libpurple: a buddy list laid out the way purple-discord
 * lays one out ("<server>: <category>" groups of chats keyed by an "id" component), the flags it
 * sets on them, and what the transport sends back on "webos-im-channel-setting". No prpl is
 * loaded; the account only names prpl-discord, which is all ChannelOptions looks at.
 */

#include "ChannelOptions.h"

#include <gtest/gtest.h>
#include <purple.h>

#include <ftw.h>
#include <stdio.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace {

const char* const UI = "channeloptions-test";

PurpleEventLoopUiOps eventLoop = {
	g_timeout_add, g_source_remove, NULL, g_source_remove, NULL, g_timeout_add_seconds,
	NULL, NULL, NULL
};

int removeEntry(const char* path, const struct stat*, int, struct FTW*)
{
	return remove(path) == 0 ? 0 : -1;
}

class LibpurpleEnvironment : public ::testing::Environment
{
public:
	void SetUp() override
	{
		char tmpl[] = "channeloptions-test-XXXXXX";
		const char* dir = mkdtemp(tmpl);
		ASSERT_NE(nullptr, dir);
		char* abs = realpath(dir, NULL);
		ASSERT_NE(nullptr, abs);
		m_dir = abs;
		free(abs);
		purple_util_set_user_dir(m_dir.c_str());
		purple_debug_set_enabled(getenv("CHANNELOPTIONS_TEST_DEBUG") != NULL);
		purple_eventloop_set_ui_ops(&eventLoop);
		ASSERT_TRUE(purple_core_init(UI));
		purple_set_blist(purple_blist_new());
		purple_blist_load();
		ChannelOptions::registerSignals();
		// Twice: the transport calls it from more than one place.
		ChannelOptions::registerSignals();
	}

	void TearDown() override
	{
		purple_core_quit();
		if (!m_dir.empty())
			nftw(m_dir.c_str(), removeEntry, 16, FTW_DEPTH | FTW_PHYS);
	}

private:
	std::string m_dir;
};

// The transport's deriveServerName for a Discord group: the part before ": ".
std::string serverOf(PurpleAccount*, const char* groupName)
{
	std::string g = groupName ? groupName : "";
	std::string::size_type sep = g.find(": ");
	return sep == std::string::npos ? g : g.substr(0, sep);
}

struct Emitted
{
	PurpleAccount* account;
	std::string channel, scope, setting, value;
};

std::vector<Emitted> s_emitted;
std::vector<PurpleAccount*> s_changed;
int s_handle;

void settingCb(PurpleAccount* account, const char* channel, const char* scope, const char* setting,
	const char* value, void*)
{
	s_emitted.push_back({account, channel ? channel : "", scope ? scope : "", setting ? setting : "",
		value ? value : ""});
}

void changedCb(PurpleAccount* account, void*)
{
	s_changed.push_back(account);
}

PurpleChat* addChat(PurpleAccount* account, const char* group, const char* id, const char* name)
{
	GHashTable* comps = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	if (id)
		g_hash_table_replace(comps, g_strdup("id"), g_strdup(id));
	g_hash_table_replace(comps, g_strdup("name"), g_strdup(name));
	PurpleChat* chat = purple_chat_new(account, NULL, comps);
	PurpleGroup* g = purple_find_group(group);
	if (g == NULL)
	{
		g = purple_group_new(group);
		purple_blist_add_group(g, NULL);
	}
	purple_blist_add_chat(chat, g, NULL);
	return chat;
}

class ChannelOptionsTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		discord = purple_account_new("me@example.com", "prpl-discord");
		purple_accounts_add(discord);
		other = purple_account_new("me2@example.com", "prpl-discord");
		purple_accounts_add(other);
		irc = purple_account_new("nick@irc.example.net", "prpl-irc");
		purple_accounts_add(irc);

		general = addChat(discord, "Guild A: Text Channels", "111", "general");
		random = addChat(discord, "Guild A: Text Channels", "112", "random");
		voice = addChat(discord, "Guild A: Voice", "113", "lounge");
		elsewhere = addChat(discord, "Guild B", "211", "news");
		noId = addChat(discord, "Guild C", NULL, "broken");
		emptyId = addChat(discord, "Guild C", "", "blank");
		othersGeneral = addChat(other, "Guild A: Text Channels", "111", "general");

		s_emitted.clear();
		s_changed.clear();
		void* conv = purple_conversations_get_handle();
		purple_signal_connect(conv, ChannelOptions::SETTING_SIGNAL, &s_handle, PURPLE_CALLBACK(settingCb), NULL);
		purple_signal_connect(conv, ChannelOptions::CHANGED_SIGNAL, &s_handle, PURPLE_CALLBACK(changedCb), NULL);
	}

	void TearDown() override
	{
		purple_signals_disconnect_by_handle(&s_handle);
		// With no prpl loaded the accounts have no presence, so purple_account_destroy logs
		// "purple_presence_destroy: assertion 'presence != NULL' failed" for each: harmless.
		for (PurpleAccount* a : {discord, other, irc})
			purple_accounts_delete(a);   // its chats go with it
		for (const char* g : {"Guild A: Text Channels", "Guild A: Voice", "Guild B", "Guild C"})
		{
			PurpleGroup* group = purple_find_group(g);
			if (group != NULL)
				purple_blist_remove_group(group);
		}
	}

	PurpleAccount* discord = nullptr;
	PurpleAccount* other = nullptr;
	PurpleAccount* irc = nullptr;
	PurpleChat* general = nullptr;
	PurpleChat* random = nullptr;
	PurpleChat* voice = nullptr;
	PurpleChat* elsewhere = nullptr;
	PurpleChat* noId = nullptr;
	PurpleChat* emptyId = nullptr;
	PurpleChat* othersGeneral = nullptr;
};

TEST_F(ChannelOptionsTest, OnlyDiscordTakesSettings)
{
	EXPECT_TRUE(ChannelOptions::supports(discord));
	EXPECT_FALSE(ChannelOptions::supports(irc));
	EXPECT_FALSE(ChannelOptions::supports(NULL));
}

TEST_F(ChannelOptionsTest, FindsTheAccountsOwnChannel)
{
	EXPECT_EQ(general, ChannelOptions::findChannel(discord, "111"));
	EXPECT_EQ(othersGeneral, ChannelOptions::findChannel(other, "111"));
	EXPECT_EQ(elsewhere, ChannelOptions::findChannel(discord, "211"));
	EXPECT_EQ(nullptr, ChannelOptions::findChannel(discord, "999"));
	EXPECT_EQ(nullptr, ChannelOptions::findChannel(discord, ""));
	EXPECT_EQ(nullptr, ChannelOptions::findChannel(NULL, "111"));
}

TEST_F(ChannelOptionsTest, FindsAServerByItsName)
{
	PurpleChat* a = ChannelOptions::findServerChat(discord, "Guild A", serverOf);
	ASSERT_NE(nullptr, a);
	EXPECT_EQ(discord, purple_chat_get_account(a));
	EXPECT_TRUE(a == general || a == random || a == voice);
	EXPECT_EQ(elsewhere, ChannelOptions::findServerChat(discord, "Guild B", serverOf));
	// A server whose chats have no id cannot stand for anything.
	EXPECT_EQ(nullptr, ChannelOptions::findServerChat(discord, "Guild C", serverOf));
	EXPECT_EQ(nullptr, ChannelOptions::findServerChat(discord, "Guild", serverOf));
	EXPECT_EQ(nullptr, ChannelOptions::findServerChat(irc, "Guild A", serverOf));
	EXPECT_EQ(nullptr, ChannelOptions::findServerChat(discord, "Guild A", NULL));
}

TEST_F(ChannelOptionsTest, TellsTheFlagsThePrplSet)
{
	ChannelOptions::Flags none = ChannelOptions::flagsOf(general);
	EXPECT_FALSE(none.muted);
	EXPECT_FALSE(none.background);
	EXPECT_FALSE(none.serverMuted);
	EXPECT_FALSE(none.serverBackground);

	PurpleBlistNode* node = (PurpleBlistNode*)random;
	purple_blist_node_set_bool(node, "muted", TRUE);
	purple_blist_node_set_bool(node, "webos-server-background", TRUE);
	ChannelOptions::Flags some = ChannelOptions::flagsOf(random);
	EXPECT_TRUE(some.muted);
	EXPECT_FALSE(some.background);
	EXPECT_FALSE(some.serverMuted);
	EXPECT_TRUE(some.serverBackground);

	purple_blist_node_set_bool(node, "webos-background", TRUE);
	purple_blist_node_set_bool(node, "webos-server-muted", TRUE);
	purple_blist_node_set_bool(node, "muted", FALSE);
	ChannelOptions::Flags other = ChannelOptions::flagsOf(random);
	EXPECT_FALSE(other.muted);
	EXPECT_TRUE(other.background);
	EXPECT_TRUE(other.serverMuted);

	EXPECT_FALSE(ChannelOptions::flagsOf(NULL).muted);
}

TEST_F(ChannelOptionsTest, AsksThePrplForAChannelSetting)
{
	ASSERT_TRUE(ChannelOptions::set(discord, random, ChannelOptions::CHANNEL, "muted", true));
	ASSERT_TRUE(ChannelOptions::set(discord, random, ChannelOptions::CHANNEL, "background", false));
	ASSERT_EQ(2u, s_emitted.size());
	EXPECT_EQ(discord, s_emitted[0].account);
	EXPECT_EQ("112", s_emitted[0].channel);
	EXPECT_EQ("channel", s_emitted[0].scope);
	EXPECT_EQ("muted", s_emitted[0].setting);
	EXPECT_EQ("1", s_emitted[0].value);
	EXPECT_EQ("background", s_emitted[1].setting);
	EXPECT_EQ("0", s_emitted[1].value);
}

TEST_F(ChannelOptionsTest, AsksThePrplForAServerSetting)
{
	PurpleChat* a = ChannelOptions::findServerChat(discord, "Guild A", serverOf);
	ASSERT_TRUE(ChannelOptions::set(discord, a, ChannelOptions::SERVER, "background", true));
	ASSERT_EQ(1u, s_emitted.size());
	EXPECT_EQ("server", s_emitted[0].scope);
	EXPECT_EQ("background", s_emitted[0].setting);
	EXPECT_EQ("1", s_emitted[0].value);
	ASSERT_NE(nullptr, ChannelOptions::channelId(a));
	EXPECT_EQ(ChannelOptions::channelId(a), s_emitted[0].channel);
	EXPECT_EQ(nullptr, ChannelOptions::channelId(noId));
	EXPECT_EQ(nullptr, ChannelOptions::channelId(emptyId));
	EXPECT_EQ(nullptr, ChannelOptions::channelId(NULL));
}

TEST_F(ChannelOptionsTest, RefusesWhatThePrplCannotDo)
{
	EXPECT_FALSE(ChannelOptions::set(discord, general, ChannelOptions::CHANNEL, "pinned", true));
	EXPECT_FALSE(ChannelOptions::set(discord, general, ChannelOptions::CHANNEL, NULL, true));
	EXPECT_FALSE(ChannelOptions::set(discord, noId, ChannelOptions::CHANNEL, "muted", true));
	EXPECT_FALSE(ChannelOptions::set(discord, emptyId, ChannelOptions::CHANNEL, "muted", true));
	EXPECT_FALSE(ChannelOptions::set(discord, NULL, ChannelOptions::CHANNEL, "muted", true));
	EXPECT_FALSE(ChannelOptions::set(irc, general, ChannelOptions::CHANNEL, "muted", true));
	EXPECT_TRUE(s_emitted.empty());
}

TEST_F(ChannelOptionsTest, CarriesThePrplsChangeNotice)
{
	purple_signal_emit(purple_conversations_get_handle(), ChannelOptions::CHANGED_SIGNAL, discord);
	ASSERT_EQ(1u, s_changed.size());
	EXPECT_EQ(discord, s_changed[0]);
}

}  // namespace

int main(int argc, char** argv)
{
	::testing::InitGoogleTest(&argc, argv);
	::testing::AddGlobalTestEnvironment(new LibpurpleEnvironment);
	return RUN_ALL_TESTS();
}
