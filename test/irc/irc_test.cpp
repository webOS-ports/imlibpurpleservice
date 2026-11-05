/*
 * irc_test.cpp
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport).
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free Software
 * Foundation.
 *
 * IrcSupport.cpp against the real libpurple and its real prpl-irc, signed in to a small IRC
 * server that runs in this process on the same glib main loop. The server answers registration,
 * JOIN and PART the way a real one does (a JOIN comes back with the channel spelt the way the
 * server knows it) and records every line the client sends, which is what the tests check.
 */

#include "IrcSupport.h"

#include <gtest/gtest.h>
#include <purple.h>

#include <arpa/inet.h>
#include <ftw.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#ifndef PURPLE_PLUGIN_DIR
#error "PURPLE_PLUGIN_DIR must name the folder holding libirc.so"
#endif

namespace {

const char* const UI = "irc-test";

// --- libpurple's event loop on glib -------------------------------------------------------

struct IoClosure
{
	PurpleInputFunction function;
	guint result;
	gpointer data;
};

gboolean ioInvoke(GIOChannel* source, GIOCondition condition, gpointer data)
{
	IoClosure* closure = (IoClosure*)data;
	int cond = 0;
	if (condition & (G_IO_IN | G_IO_HUP | G_IO_ERR))
		cond |= PURPLE_INPUT_READ;
	if (condition & (G_IO_OUT | G_IO_HUP | G_IO_ERR | G_IO_NVAL))
		cond |= PURPLE_INPUT_WRITE;
	closure->function(closure->data, g_io_channel_unix_get_fd(source), (PurpleInputCondition)cond);
	return TRUE;
}

guint ioAdd(gint fd, PurpleInputCondition condition, PurpleInputFunction function, gpointer data)
{
	IoClosure* closure = g_new0(IoClosure, 1);
	closure->function = function;
	closure->data = data;
	int cond = 0;
	if (condition & PURPLE_INPUT_READ)
		cond |= G_IO_IN | G_IO_HUP | G_IO_ERR;
	if (condition & PURPLE_INPUT_WRITE)
		cond |= G_IO_OUT | G_IO_HUP | G_IO_ERR | G_IO_NVAL;
	GIOChannel* channel = g_io_channel_unix_new(fd);
	closure->result = g_io_add_watch_full(channel, G_PRIORITY_DEFAULT, (GIOCondition)cond,
		ioInvoke, closure, g_free);
	g_io_channel_unref(channel);
	return closure->result;
}

PurpleEventLoopUiOps eventLoop = {
	g_timeout_add, g_source_remove, ioAdd, g_source_remove, NULL, g_timeout_add_seconds,
	NULL, NULL, NULL
};

// --- the IRC server ---------------------------------------------------------------------------

struct Server
{
	int listenFd = -1;
	int clientFd = -1;
	guint listenWatch = 0;
	guint clientWatch = 0;
	int port = 0;
	std::string buffer;
	std::vector<std::string> received;
	std::string nick;
	// Channels the server knows, by lower-case name, as the server spells them.
	std::map<std::string, std::string> known;

	void send(const std::string& line)
	{
		std::string out = line + "\r\n";
		ssize_t n = write(clientFd, out.data(), out.size());
		(void)n;
	}

	static std::string lower(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), ::tolower);
		return s;
	}

	std::string spelling(const std::string& channel)
	{
		std::map<std::string, std::string>::iterator it = known.find(lower(channel));
		return it == known.end() ? channel : it->second;
	}

	void handle(const std::string& line)
	{
		received.push_back(line);
		std::string cmd = line.substr(0, line.find(' '));
		std::string args = line.find(' ') == std::string::npos ? "" : line.substr(line.find(' ') + 1);
		if (cmd == "NICK")
		{
			std::string newNick = args[0] == ':' ? args.substr(1) : args;
			if (nick.empty())
			{
				nick = newNick;
				send(":irc.test 001 " + nick + " :Welcome to the test network " + nick);
				send(":irc.test 376 " + nick + " :End of /MOTD command.");
			}
			else
			{
				send(":" + nick + "!u@h NICK :" + newNick);
				nick = newNick;
			}
		}
		else if (cmd == "PING")
			send(":irc.test PONG irc.test " + args);
		else if (cmd == "JOIN")
		{
			std::string list = args.substr(0, args.find(' '));
			std::string::size_type i = 0;
			while (i <= list.size())
			{
				std::string::size_type comma = list.find(',', i);
				if (comma == std::string::npos)
					comma = list.size();
				std::string channel = spelling(list.substr(i, comma - i));
				send(":" + nick + "!u@h JOIN :" + channel);
				send(":irc.test 353 " + nick + " = " + channel + " :" + nick + " alice");
				send(":irc.test 366 " + nick + " " + channel + " :End of /NAMES list.");
				i = comma + 1;
			}
		}
		else if (cmd == "PART")
		{
			std::string list = args.substr(0, args.find(' '));
			std::string::size_type i = 0;
			while (i <= list.size())
			{
				std::string::size_type comma = list.find(',', i);
				if (comma == std::string::npos)
					comma = list.size();
				send(":" + nick + "!u@h PART " + spelling(list.substr(i, comma - i)) + " :bye");
				i = comma + 1;
			}
		}
	}

	static gboolean onClient(GIOChannel*, GIOCondition, gpointer data)
	{
		Server* s = (Server*)data;
		char buf[4096];
		ssize_t n = read(s->clientFd, buf, sizeof(buf));
		if (n <= 0)
		{
			s->clientWatch = 0;
			return FALSE;
		}
		s->buffer.append(buf, (size_t)n);
		std::string::size_type eol;
		while ((eol = s->buffer.find("\r\n")) != std::string::npos)
		{
			std::string line = s->buffer.substr(0, eol);
			s->buffer.erase(0, eol + 2);
			s->handle(line);
		}
		return TRUE;
	}

	static gboolean onAccept(GIOChannel*, GIOCondition, gpointer data)
	{
		Server* s = (Server*)data;
		int fd = accept(s->listenFd, NULL, NULL);
		if (fd < 0)
			return TRUE;
		if (s->clientFd >= 0)
		{
			close(fd);   // one client at a time
			return TRUE;
		}
		s->clientFd = fd;
		GIOChannel* ch = g_io_channel_unix_new(fd);
		s->clientWatch = g_io_add_watch(ch, (GIOCondition)(G_IO_IN | G_IO_HUP | G_IO_ERR), onClient, s);
		g_io_channel_unref(ch);
		return TRUE;
	}

	void start()
	{
		listenFd = socket(AF_INET, SOCK_STREAM, 0);
		ASSERT_GE(listenFd, 0);
		int one = 1;
		setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		ASSERT_EQ(0, bind(listenFd, (sockaddr*)&addr, sizeof(addr)));
		ASSERT_EQ(0, listen(listenFd, 1));
		socklen_t len = sizeof(addr);
		getsockname(listenFd, (sockaddr*)&addr, &len);
		port = ntohs(addr.sin_port);
		GIOChannel* ch = g_io_channel_unix_new(listenFd);
		listenWatch = g_io_add_watch(ch, G_IO_IN, onAccept, this);
		g_io_channel_unref(ch);
	}

	void stop()
	{
		if (clientWatch)
			g_source_remove(clientWatch);
		if (listenWatch)
			g_source_remove(listenWatch);
		if (clientFd >= 0)
			close(clientFd);
		if (listenFd >= 0)
			close(listenFd);
		clientFd = listenFd = -1;
		clientWatch = listenWatch = 0;
	}

	bool got(const std::string& line) const
	{
		return std::find(received.begin(), received.end(), line) != received.end();
	}

	std::string dump() const
	{
		std::string out = "\nthe server got:\n";
		for (size_t i = 0; i < received.size(); ++i)
			out += "  " + received[i] + "\n";
		return out;
	}

	size_t count(const std::string& line) const
	{
		return (size_t)std::count(received.begin(), received.end(), line);
	}
};

template <typename Pred>
bool waitFor(Pred pred, int ms = 3000)
{
	gint64 until = g_get_monotonic_time() + (gint64)ms * 1000;
	while (!pred())
	{
		if (g_get_monotonic_time() > until)
			return false;
		g_main_context_iteration(NULL, FALSE);
		g_usleep(1000);
	}
	return true;
}

// Run the loop for a while, for checking that something did NOT happen. prpl-irc queues what it
// sends and flushes the queue from a one-second timer (irc_send_handler_cb), so anything shorter
// would let a line that should not have gone out arrive after the check.
void settle(int ms = 1500)
{
	gint64 until = g_get_monotonic_time() + (gint64)ms * 1000;
	while (g_get_monotonic_time() < until)
	{
		g_main_context_iteration(NULL, FALSE);
		g_usleep(1000);
	}
}

std::vector<PurpleAccount*> s_changed;
void channelsChanged(PurpleAccount* account) { s_changed.push_back(account); }
int s_watchHandle;

static int removeEntry(const char* path, const struct stat*, int, struct FTW*)
{
	return remove(path) == 0 ? 0 : -1;
}

class LibpurpleEnvironment : public ::testing::Environment
{
public:
	void SetUp() override
	{
		char tmpl[] = "irc-test-XXXXXX";
		const char* dir = mkdtemp(tmpl);
		ASSERT_NE(nullptr, dir);
		char* abs = realpath(dir, NULL);
		ASSERT_NE(nullptr, abs);
		m_dir = abs;
		free(abs);
		purple_util_set_user_dir(m_dir.c_str());
		purple_debug_set_enabled(getenv("IRC_TEST_DEBUG") != NULL);
		purple_eventloop_set_ui_ops(&eventLoop);
		purple_plugins_add_search_path(PURPLE_PLUGIN_DIR);
		ASSERT_TRUE(purple_core_init(UI));
		purple_set_blist(purple_blist_new());
		purple_blist_load();
		ASSERT_NE(nullptr, purple_find_prpl(Irc::PRPL_ID)) << "no prpl-irc in " << PURPLE_PLUGIN_DIR;
		Irc::watchJoins(&s_watchHandle, channelsChanged);
	}

	void TearDown() override
	{
		Irc::unwatchJoins(&s_watchHandle);
		purple_core_quit();
		// The user dir libpurple wrote (accounts.xml, blist.xml, ...), deepest first; FTW_PHYS so a
		// link is removed and never followed.
		if (!m_dir.empty())
			nftw(m_dir.c_str(), removeEntry, 16, FTW_DEPTH | FTW_PHYS);
	}

private:
	std::string m_dir;
};

// --- names and keys -------------------------------------------------------------------------

TEST(IrcNames, ChannelNames)
{
	EXPECT_TRUE(Irc::isChannelName("#linux"));
	EXPECT_TRUE(Irc::isChannelName("##linux"));
	EXPECT_TRUE(Irc::isChannelName("&local"));
	EXPECT_TRUE(Irc::isChannelName("+modeless"));
	EXPECT_TRUE(Irc::isChannelName("!12345chan"));
	EXPECT_TRUE(Irc::isChannelName("#café"));
	EXPECT_FALSE(Irc::isChannelName("#"));
	EXPECT_FALSE(Irc::isChannelName("linux"));
	EXPECT_FALSE(Irc::isChannelName("#two words"));
	EXPECT_FALSE(Irc::isChannelName("#a,#b"));
	EXPECT_FALSE(Irc::isChannelName("#a:b"));
	EXPECT_FALSE(Irc::isChannelName("#bell\a"));
	EXPECT_FALSE(Irc::isChannelName(std::string("#nul\0x", 6)));
	EXPECT_FALSE(Irc::isChannelName("#" + std::string(200, 'a')));
	EXPECT_TRUE(Irc::isChannelName("#" + std::string(199, 'a')));
	EXPECT_FALSE(Irc::isChannelName("#bad\xff"));
}

TEST(IrcNames, FromInput)
{
	EXPECT_EQ("#linux", Irc::channelFromInput("linux"));
	EXPECT_EQ("#Linux", Irc::channelFromInput("  #Linux \n"));
	EXPECT_EQ("&local", Irc::channelFromInput("&local"));
	EXPECT_EQ("", Irc::channelFromInput(""));
	EXPECT_EQ("", Irc::channelFromInput("   "));
	EXPECT_EQ("", Irc::channelFromInput("two words"));
	EXPECT_EQ("", Irc::channelFromInput("#"));
}

TEST(IrcNames, ChannelList)
{
	std::vector<std::string> got = Irc::parseChannelList("#webos, luneos\n#LuneOS  ##linux,,bad:name\r\n#webos-ports");
	std::vector<std::string> want = { "#webos", "#luneos", "##linux", "#webos-ports" };
	EXPECT_EQ(want, got);
	EXPECT_TRUE(Irc::parseChannelList("").empty());
	EXPECT_TRUE(Irc::parseChannelList(" , \n ").empty());
}

TEST(IrcNames, Keys)
{
	EXPECT_EQ("#luneos@irc.libera.chat", Irc::channelKey("#LuneOS", "IRC.Libera.Chat"));
	std::string channel, net;
	ASSERT_TRUE(Irc::splitChannelKey("#luneos@irc.libera.chat", &channel, &net));
	EXPECT_EQ("#luneos", channel);
	EXPECT_EQ("irc.libera.chat", net);
	// A channel may hold an '@'; the network cannot, so the last one splits.
	ASSERT_TRUE(Irc::splitChannelKey("#a@b@irc.oftc.net", &channel, &net));
	EXPECT_EQ("#a@b", channel);
	EXPECT_EQ("irc.oftc.net", net);
	EXPECT_FALSE(Irc::splitChannelKey("alice", NULL, NULL));
	EXPECT_FALSE(Irc::splitChannelKey("alice@irc.libera.chat", NULL, NULL));
	EXPECT_FALSE(Irc::splitChannelKey("#luneos@", NULL, NULL));
	EXPECT_FALSE(Irc::splitChannelKey("@irc.libera.chat", NULL, NULL));
	EXPECT_FALSE(Irc::splitChannelKey("#luneos", NULL, NULL));
}

// --- the buddy list, offline ----------------------------------------------------------------

class IrcOffline : public ::testing::Test
{
protected:
	void SetUp() override
	{
		m_account = purple_account_new("tester@Irc.Example.ORG", Irc::PRPL_ID);
		purple_accounts_add(m_account);
	}
	void TearDown() override
	{
		purple_accounts_delete(m_account);
	}
	PurpleAccount* m_account = nullptr;
};

TEST_F(IrcOffline, AccountAndNetwork)
{
	EXPECT_TRUE(Irc::isAccount(m_account));
	EXPECT_EQ("irc.example.org", Irc::network(m_account));
	EXPECT_FALSE(Irc::isAccount(NULL));
	EXPECT_EQ("", Irc::network(NULL));
}

TEST_F(IrcOffline, RememberAndForget)
{
	ASSERT_NE(nullptr, Irc::rememberChannel(m_account, "#One"));
	ASSERT_NE(nullptr, Irc::rememberChannel(m_account, "#two"));
	EXPECT_EQ(nullptr, Irc::rememberChannel(m_account, "not a channel"));

	// Kept under a group named after the network.
	PurpleChat* chat = Irc::findChat(m_account, "#ONE");
	ASSERT_NE(nullptr, chat);
	ASSERT_NE(nullptr, purple_chat_get_group(chat));
	EXPECT_STREQ("irc.example.org", purple_group_get_name(purple_chat_get_group(chat)));

	// Same channel in another case: the one chat, taking the new spelling.
	EXPECT_EQ(chat, Irc::rememberChannel(m_account, "#one"));
	std::vector<std::string> want = { "#one", "#two" };
	EXPECT_EQ(want, Irc::rememberedChannels(m_account));

	EXPECT_TRUE(Irc::forgetChannel(m_account, "#ONE"));
	EXPECT_FALSE(Irc::forgetChannel(m_account, "#one"));
	EXPECT_EQ(std::vector<std::string>{ "#two" }, Irc::rememberedChannels(m_account));

	// The group goes with its last channel.
	EXPECT_TRUE(Irc::forgetChannel(m_account, "#two"));
	EXPECT_EQ(nullptr, purple_find_group("irc.example.org"));
}

TEST_F(IrcOffline, ChannelKeyIsKept)
{
	PurpleChat* chat = Irc::rememberChannel(m_account, "#secret", "hunter2");
	ASSERT_NE(nullptr, chat);
	EXPECT_STREQ("hunter2", (const char*)g_hash_table_lookup(purple_chat_get_components(chat), "password"));
	// Remembering it again without a key keeps the key it has.
	Irc::rememberChannel(m_account, "#secret");
	EXPECT_STREQ("hunter2", (const char*)g_hash_table_lookup(purple_chat_get_components(chat), "password"));
	Irc::forgetChannel(m_account, "#secret");
}

TEST_F(IrcOffline, OtherAccountsAreApart)
{
	PurpleAccount* other = purple_account_new("tester@irc.other.net", Irc::PRPL_ID);
	purple_accounts_add(other);
	Irc::rememberChannel(m_account, "#same");
	Irc::rememberChannel(other, "#same");
	EXPECT_NE(Irc::findChat(m_account, "#same"), Irc::findChat(other, "#same"));
	Irc::forgetChannel(other, "#same");
	EXPECT_NE(nullptr, Irc::findChat(m_account, "#same"));
	EXPECT_EQ(nullptr, Irc::findChat(other, "#same"));
	Irc::forgetChannel(m_account, "#same");
	purple_accounts_delete(other);
}

TEST_F(IrcOffline, SeedOnce)
{
	Irc::seedChannels(m_account, "#a, b");
	std::vector<std::string> want = { "#a", "#b" };
	EXPECT_EQ(want, Irc::rememberedChannels(m_account));
	// The person left #a since; a later sign-in with the same list does not bring it back.
	Irc::forgetChannel(m_account, "#a");
	Irc::seedChannels(m_account, "#a, b");
	EXPECT_EQ(std::vector<std::string>{ "#b" }, Irc::rememberedChannels(m_account));
	Irc::forgetChannel(m_account, "#b");
}

TEST_F(IrcOffline, NotIrc)
{
	PurpleAccount* xmpp = purple_account_new("someone@example.org", "prpl-jabber");
	purple_accounts_add(xmpp);
	EXPECT_FALSE(Irc::isAccount(xmpp));
	EXPECT_EQ(nullptr, Irc::rememberChannel(xmpp, "#x"));
	Irc::seedChannels(xmpp, "#x");
	EXPECT_TRUE(Irc::rememberedChannels(xmpp).empty());
	EXPECT_FALSE(Irc::leaveChannel(xmpp, "#x"));
	purple_accounts_delete(xmpp);
}

TEST(IrcCommandsOffline, NotACommand)
{
	Irc::CommandOutcome o = Irc::runCommand(NULL, "hello");
	EXPECT_EQ(Irc::NOT_A_COMMAND, o.result);
	EXPECT_EQ("hello", o.text);
	o = Irc::runCommand(NULL, "//etc/hosts is a file");
	EXPECT_EQ(Irc::NOT_A_COMMAND, o.result);
	EXPECT_EQ("/etc/hosts is a file", o.text);
	o = Irc::runCommand(NULL, "/ shrug");
	EXPECT_EQ(Irc::NOT_A_COMMAND, o.result);
	EXPECT_EQ("/ shrug", o.text);
	o = Irc::runCommand(NULL, "/");
	EXPECT_EQ(Irc::NOT_A_COMMAND, o.result);
	o = Irc::runCommand(NULL, "");
	EXPECT_EQ(Irc::NOT_A_COMMAND, o.result);
	// A command with no IRC conversation to run in.
	o = Irc::runCommand(NULL, "/me waves");
	EXPECT_EQ(Irc::COMMAND_FAILED, o.result);
}

// --- signed in ------------------------------------------------------------------------------

class IrcOnline : public ::testing::Test
{
protected:
	void SetUp() override
	{
		m_server.start();
		m_server.known["#luneos"] = "#LuneOS";
		m_account = purple_account_new("tester@127.0.0.1", Irc::PRPL_ID);
		purple_account_set_int(m_account, "port", m_server.port);
		purple_account_set_bool(m_account, "ssl", FALSE);
		// prpl-irc sends five lines at once and then one every two seconds, as a real network
		// wants. These tests send more than five; an interval of 0 lets them through unpaced.
		purple_account_set_int(m_account, "ratelimit-interval", 0);
		purple_accounts_add(m_account);
		Irc::seedChannels(m_account, "#luneos, #webos");
		s_changed.clear();
		purple_account_set_enabled(m_account, UI, TRUE);
		purple_account_connect(m_account);
		ASSERT_TRUE(waitFor([&] { return purple_account_is_connected(m_account); }))
			<< "never signed in; lines: " << m_server.received.size();
	}

	void TearDown() override
	{
		purple_account_disconnect(m_account);
		std::vector<std::string> left = Irc::rememberedChannels(m_account);
		for (size_t i = 0; i < left.size(); ++i)
			Irc::forgetChannel(m_account, left[i]);
		purple_accounts_delete(m_account);
		m_server.stop();
		settle(50);
	}

	PurpleConversation* chat(const std::string& name)
	{
		return purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, name.c_str(), m_account);
	}

	bool inChannel(const std::string& name)
	{
		PurpleConversation* c = chat(name);
		return c != NULL && !purple_conv_chat_has_left(PURPLE_CONV_CHAT(c));
	}

	Server m_server;
	PurpleAccount* m_account = nullptr;
};

TEST_F(IrcOnline, NetworkFindsTheAccount)
{
	EXPECT_EQ(m_account, Irc::accountForNetwork("127.0.0.1"));
	EXPECT_EQ(nullptr, Irc::accountForNetwork("irc.elsewhere.net"));

	// A second account on the same network that is not signed in does not take its place...
	PurpleAccount* idle = purple_account_new("other@127.0.0.1", Irc::PRPL_ID);
	purple_accounts_add(idle);
	purple_accounts_reorder(idle, 0);   // ahead of the signed-in one in the account list
	EXPECT_EQ(m_account, Irc::accountForNetwork("127.0.0.1"));
	purple_accounts_delete(idle);
	// ...and a network with no account signed in still finds its account, to forget channels in.
	PurpleAccount* offline = purple_account_new("tester@IRC.Offline.Net", Irc::PRPL_ID);
	purple_accounts_add(offline);
	EXPECT_EQ(offline, Irc::accountForNetwork("irc.offline.net"));
	purple_accounts_delete(offline);
}

TEST_F(IrcOnline, SignInJoinsRememberedChannels)
{
	Irc::joinRemembered(m_account);
	ASSERT_TRUE(waitFor([&] { return inChannel("#luneos") && inChannel("#webos"); }));
	EXPECT_TRUE(m_server.got("JOIN #luneos"));
	EXPECT_TRUE(m_server.got("JOIN #webos"));
	// The server spells it #LuneOS; the remembered channel takes that spelling.
	std::vector<std::string> want = { "#LuneOS", "#webos" };
	EXPECT_EQ(want, Irc::rememberedChannels(m_account));
	// And each join was reported.
	EXPECT_EQ(2u, (size_t)std::count(s_changed.begin(), s_changed.end(), m_account));

	// Joining again while in them sends nothing more.
	Irc::joinRemembered(m_account);
	Irc::joinChannel(m_account, "#webos");
	settle();
	EXPECT_EQ(1u, m_server.count("JOIN #luneos"));
	EXPECT_EQ(1u, m_server.count("JOIN #webos"));
}

TEST_F(IrcOnline, JoinANewChannel)
{
	Irc::joinChannel(m_account, "#new");
	ASSERT_TRUE(waitFor([&] { return inChannel("#new"); }));
	EXPECT_TRUE(m_server.got("JOIN #new"));
	EXPECT_NE(nullptr, Irc::findChat(m_account, "#new"));
}

TEST_F(IrcOnline, JoinWithAKey)
{
	Irc::joinChannel(m_account, "#secret", "hunter2");
	ASSERT_TRUE(waitFor([&] { return inChannel("#secret"); }));
	EXPECT_TRUE(m_server.got("JOIN #secret hunter2"));
}

TEST_F(IrcOnline, LeaveParts)
{
	Irc::joinChannel(m_account, "#going");
	ASSERT_TRUE(waitFor([&] { return inChannel("#going"); }));
	EXPECT_TRUE(Irc::leaveChannel(m_account, "#going"));
	ASSERT_TRUE(waitFor([&] { return m_server.got("PART #going"); })) << m_server.dump();
	EXPECT_EQ(nullptr, Irc::findChat(m_account, "#going"));
	EXPECT_EQ(nullptr, chat("#going"));
	// Leaving what it is not in, and does not remember, is nothing.
	EXPECT_FALSE(Irc::leaveChannel(m_account, "#never"));
}

TEST_F(IrcOnline, LeaveByKeyParts)
{
	// Messaging leaves by the channel's key, which is lower case; the server spells it #LuneOS.
	Irc::joinChannel(m_account, "#luneos");
	ASSERT_TRUE(waitFor([&] { return inChannel("#LuneOS"); }));
	std::string channel;
	ASSERT_TRUE(Irc::splitChannelKey(Irc::channelKey("#LuneOS", Irc::network(m_account)), &channel, NULL));
	EXPECT_EQ("#luneos", channel);
	EXPECT_TRUE(Irc::leaveChannel(m_account, channel));
	ASSERT_TRUE(waitFor([&] { return m_server.got("PART #LuneOS"); })) << m_server.dump();
	EXPECT_EQ(nullptr, Irc::findChat(m_account, "#LuneOS"));
}

TEST_F(IrcOnline, ServerJoinIsRemembered)
{
	// A join the account did not ask for (a /join from another path, a forced join): it is in the
	// channel now, so it is kept.
	m_server.send(":tester!u@h JOIN :#forced");
	ASSERT_TRUE(waitFor([&] { return inChannel("#forced"); }));
	EXPECT_NE(nullptr, Irc::findChat(m_account, "#forced"));
}

TEST_F(IrcOnline, Action)
{
	Irc::joinChannel(m_account, "#webos");
	ASSERT_TRUE(waitFor([&] { return inChannel("#webos"); }));
	Irc::CommandOutcome o = Irc::runCommand(chat("#webos"), "/me waves");
	EXPECT_EQ(Irc::COMMAND_DONE, o.result);
	ASSERT_TRUE(waitFor([&] { return m_server.got("PRIVMSG #webos :\001ACTION waves\001"); })) << m_server.dump();
}

TEST_F(IrcOnline, JoinAndPartCommands)
{
	Irc::joinChannel(m_account, "#webos");
	ASSERT_TRUE(waitFor([&] { return inChannel("#webos"); }));

	Irc::CommandOutcome o = Irc::runCommand(chat("#webos"), "/join #other");
	EXPECT_EQ(Irc::COMMAND_DONE, o.result);
	ASSERT_TRUE(waitFor([&] { return inChannel("#other"); }));
	EXPECT_NE(nullptr, Irc::findChat(m_account, "#other"));

	// /part with no channel leaves the one it was typed in.
	o = Irc::runCommand(chat("#other"), "/part");
	EXPECT_EQ(Irc::COMMAND_DONE, o.result);
	EXPECT_EQ(std::vector<std::string>{ "#other" }, o.left);
	EXPECT_EQ(nullptr, Irc::findChat(m_account, "#other"));
	ASSERT_TRUE(waitFor([&] { return !inChannel("#other"); }));

	// /part naming channels leaves those, wherever it was typed.
	Irc::joinChannel(m_account, "#a");
	Irc::joinChannel(m_account, "#b");
	ASSERT_TRUE(waitFor([&] { return inChannel("#a") && inChannel("#b"); }));
	o = Irc::runCommand(chat("#webos"), "/part #a,#b see you");
	EXPECT_EQ(Irc::COMMAND_DONE, o.result);
	std::vector<std::string> both = { "#a", "#b" };
	EXPECT_EQ(both, o.left);
	ASSERT_TRUE(waitFor([&] { return !inChannel("#a") && !inChannel("#b"); }));
	EXPECT_TRUE(inChannel("#webos"));
	EXPECT_NE(nullptr, Irc::findChat(m_account, "#webos"));
}

TEST_F(IrcOnline, OtherCommands)
{
	Irc::joinChannel(m_account, "#webos");
	ASSERT_TRUE(waitFor([&] { return inChannel("#webos"); }));
	PurpleConversation* c = chat("#webos");

	EXPECT_EQ(Irc::COMMAND_DONE, Irc::runCommand(c, "/msg alice hi there").result);
	ASSERT_TRUE(waitFor([&] { return m_server.got("PRIVMSG alice :hi there"); })) << m_server.dump();

	EXPECT_EQ(Irc::COMMAND_DONE, Irc::runCommand(c, "/NICK tester2").result);
	ASSERT_TRUE(waitFor([&] { return m_server.got("NICK :tester2"); })) << m_server.dump();

	EXPECT_EQ(Irc::COMMAND_DONE, Irc::runCommand(c, "/topic hello world").result);
	ASSERT_TRUE(waitFor([&] { return m_server.got("TOPIC #webos :hello world"); })) << m_server.dump();
}

TEST_F(IrcOnline, CommandsThatDoNotRun)
{
	Irc::joinChannel(m_account, "#webos");
	ASSERT_TRUE(waitFor([&] { return inChannel("#webos"); }));
	PurpleConversation* c = chat("#webos");

	Irc::CommandOutcome o = Irc::runCommand(c, "/quit bye");
	EXPECT_EQ(Irc::COMMAND_FAILED, o.result);
	EXPECT_FALSE(o.error.empty());

	o = Irc::runCommand(c, "/nosuchcommand x");
	EXPECT_EQ(Irc::COMMAND_FAILED, o.result);
	EXPECT_NE(std::string::npos, o.error.find("nosuchcommand"));

	settle();
	for (size_t i = 0; i < m_server.received.size(); ++i)
	{
		EXPECT_NE(0u, m_server.received[i].compare(0, 4, "QUIT")) << m_server.received[i];
		EXPECT_EQ(std::string::npos, m_server.received[i].find("nosuchcommand")) << m_server.received[i];
	}
	EXPECT_TRUE(purple_account_is_connected(m_account));
}

TEST_F(IrcOnline, PrivateConversationCommands)
{
	PurpleConversation* im = purple_conversation_new(PURPLE_CONV_TYPE_IM, m_account, "alice");
	ASSERT_NE(nullptr, im);
	EXPECT_EQ(Irc::COMMAND_DONE, Irc::runCommand(im, "/me nods").result);
	ASSERT_TRUE(waitFor([&] { return m_server.got("PRIVMSG alice :\001ACTION nods\001"); })) << m_server.dump();
	// /part in a private conversation names nothing to leave.
	Irc::CommandOutcome o = Irc::runCommand(im, "/part");
	EXPECT_TRUE(o.left.empty());
	purple_conversation_destroy(im);
}

TEST_F(IrcOnline, SendToAChannelKey)
{
	Irc::joinChannel(m_account, "#webos");
	ASSERT_TRUE(waitFor([&] { return inChannel("#webos"); }));

	Irc::SendOutcome o = Irc::send(m_account, "#webos@127.0.0.1", "a < b & c <3 &amp;");
	EXPECT_TRUE(o.sent) << o.error;
	ASSERT_TRUE(waitFor([&] { return m_server.got("PRIVMSG #webos :a < b & c <3 &amp;"); })) << m_server.dump();

	// The key is lower case; the channel is #LuneOS on the server.
	Irc::joinChannel(m_account, "#luneos");
	ASSERT_TRUE(waitFor([&] { return inChannel("#LuneOS"); }));
	o = Irc::send(m_account, "#luneos@127.0.0.1", "hello");
	EXPECT_TRUE(o.sent) << o.error;
	ASSERT_TRUE(waitFor([&] { return m_server.got("PRIVMSG #LuneOS :hello"); })) << m_server.dump();
}

TEST_F(IrcOnline, SendSeveralLines)
{
	Irc::joinChannel(m_account, "#webos");
	ASSERT_TRUE(waitFor([&] { return inChannel("#webos"); }));
	EXPECT_TRUE(Irc::send(m_account, "#webos@127.0.0.1", "one\ntwo").sent);
	ASSERT_TRUE(waitFor([&] { return m_server.got("PRIVMSG #webos :one") && m_server.got("PRIVMSG #webos :two"); }))
		<< m_server.dump();
}

TEST_F(IrcOnline, SendToANick)
{
	Irc::SendOutcome o = Irc::send(m_account, "alice", "hi <alice>");
	EXPECT_TRUE(o.sent) << o.error;
	ASSERT_TRUE(waitFor([&] { return m_server.got("PRIVMSG alice :hi <alice>"); })) << m_server.dump();
}

TEST_F(IrcOnline, SendACommand)
{
	Irc::joinChannel(m_account, "#webos");
	ASSERT_TRUE(waitFor([&] { return inChannel("#webos"); }));
	Irc::SendOutcome o = Irc::send(m_account, "#webos@127.0.0.1", "/me & co wave");
	EXPECT_TRUE(o.sent) << o.error;
	ASSERT_TRUE(waitFor([&] { return m_server.got("PRIVMSG #webos :\001ACTION & co wave\001"); })) << m_server.dump();

	o = Irc::send(m_account, "#webos@127.0.0.1", "//not a command");
	EXPECT_TRUE(o.sent) << o.error;
	ASSERT_TRUE(waitFor([&] { return m_server.got("PRIVMSG #webos :/not a command"); })) << m_server.dump();

	o = Irc::send(m_account, "#webos@127.0.0.1", "/part");
	EXPECT_TRUE(o.sent) << o.error;
	EXPECT_EQ(std::vector<std::string>{ "#webos" }, o.left);

	o = Irc::send(m_account, "#webos@127.0.0.1", "/nosuchcommand");
	EXPECT_FALSE(o.sent);
}

TEST_F(IrcOnline, SendWhereItCannot)
{
	// Another network's channel: nothing goes out on this one.
	Irc::SendOutcome o = Irc::send(m_account, "#webos@irc.elsewhere.net", "hello");
	EXPECT_FALSE(o.sent);
	EXPECT_FALSE(o.error.empty());

	// A channel it is not in: it joins, and this message fails rather than vanish.
	o = Irc::send(m_account, "#later@127.0.0.1", "hello");
	EXPECT_FALSE(o.sent);
	ASSERT_TRUE(waitFor([&] { return inChannel("#later"); }));
	settle();
	for (size_t i = 0; i < m_server.received.size(); ++i)
		EXPECT_EQ(std::string::npos, m_server.received[i].find("PRIVMSG #later")) << m_server.received[i];
	// Now it is in, the same message goes.
	EXPECT_TRUE(Irc::send(m_account, "#later@127.0.0.1", "hello").sent);
	ASSERT_TRUE(waitFor([&] { return m_server.got("PRIVMSG #later :hello"); })) << m_server.dump();

	EXPECT_FALSE(Irc::send(m_account, "#webos@127.0.0.1", "").sent);
	EXPECT_FALSE(Irc::send(m_account, "", "hello").sent);
	EXPECT_FALSE(Irc::send(NULL, "alice", "hello").sent);
}

TEST_F(IrcOnline, NothingSentSignedOut)
{
	purple_account_disconnect(m_account);
	Irc::SendOutcome o = Irc::send(m_account, "alice", "hello");
	EXPECT_FALSE(o.sent);
	// Remembering and leaving still work offline, for the next sign-in.
	Irc::joinChannel(m_account, "#offline");
	EXPECT_NE(nullptr, Irc::findChat(m_account, "#offline"));
	EXPECT_TRUE(Irc::leaveChannel(m_account, "#offline"));
	EXPECT_EQ(nullptr, Irc::findChat(m_account, "#offline"));
}

} // namespace

int main(int argc, char** argv)
{
	::testing::InitGoogleTest(&argc, argv);
	::testing::AddGlobalTestEnvironment(new LibpurpleEnvironment);
	return RUN_ALL_TESTS();
}
