/*
 * IrcSupport.cpp
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport). See IrcSupport.h.
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free Software
 * Foundation.
 */

#include "IrcSupport.h"

#include <cstring>
#include <set>

namespace Irc
{

const char* const PRPL_ID = "prpl-irc";
const char* const SERVICE_NAME = "type_irc";
const char* const NO_PASSWORD = "irc-no-password";

// prpl-irc's chat components (irc_chat_info): the channel, and its key ("password").
static const char* const COMPONENT_CHANNEL = "channel";
static const char* const COMPONENT_KEY = "password";

// Account setting: the sign-in app's channel list has been taken over (seedChannels).
static const char* const SETTING_SEEDED = "webos-irc-seeded";

static const size_t MAX_CHANNEL_LENGTH = 200;

static std::string lower(const std::string& s)
{
	gchar* l = g_utf8_strdown(s.c_str(), -1);
	std::string out = l ? l : "";
	g_free(l);
	return out;
}

static bool sameName(const std::string& a, const std::string& b)
{
	gchar* fa = g_utf8_casefold(a.c_str(), -1);
	gchar* fb = g_utf8_casefold(b.c_str(), -1);
	bool same = fa && fb && strcmp(fa, fb) == 0;
	g_free(fa);
	g_free(fb);
	return same;
}

static std::string trim(const std::string& s)
{
	const char* ws = " \t\r\n";
	std::string::size_type b = s.find_first_not_of(ws);
	if (b == std::string::npos)
		return std::string();
	std::string::size_type e = s.find_last_not_of(ws);
	return s.substr(b, e - b + 1);
}

bool isAccount(PurpleAccount* account)
{
	const char* id = account ? purple_account_get_protocol_id(account) : NULL;
	return id != NULL && strcmp(id, PRPL_ID) == 0;
}

std::string network(PurpleAccount* account)
{
	const char* user = account ? purple_account_get_username(account) : NULL;
	if (user == NULL)
		return std::string();
	// prpl-irc's one user split: "nick@server", split at the last '@'.
	const char* at = strrchr(user, '@');
	if (at == NULL || at[1] == '\0')
		return std::string();
	return lower(at + 1);
}

bool isChannelName(const std::string& name)
{
	if (name.size() < 2 || name.size() > MAX_CHANNEL_LENGTH)
		return false;
	if (strchr("#&+!", name[0]) == NULL)
		return false;
	for (std::string::size_type i = 0; i < name.size(); ++i)
	{
		char c = name[i];
		if (c == ' ' || c == ',' || c == ':' || c == '\a' || c == '\r' || c == '\n' || c == '\0')
			return false;
	}
	return g_utf8_validate(name.c_str(), (gssize)name.size(), NULL);
}

std::string channelFromInput(const std::string& typed)
{
	std::string name = trim(typed);
	if (name.empty())
		return std::string();
	if (strchr("#&+!", name[0]) == NULL)
		name = "#" + name;
	return isChannelName(name) ? name : std::string();
}

std::vector<std::string> parseChannelList(const std::string& text)
{
	std::vector<std::string> out;
	std::set<std::string> seen;
	std::string::size_type i = 0;
	while (i < text.size())
	{
		std::string::size_type end = text.find_first_of(", \t\r\n", i);
		if (end == std::string::npos)
			end = text.size();
		std::string channel = channelFromInput(text.substr(i, end - i));
		if (!channel.empty())
		{
			std::string folded = lower(channel);
			if (seen.insert(folded).second)
				out.push_back(channel);
		}
		i = end + 1;
	}
	return out;
}

std::string channelKey(const std::string& channel, const std::string& net)
{
	return lower(channel) + "@" + lower(net);
}

bool splitChannelKey(const std::string& key, std::string* channel, std::string* net)
{
	std::string::size_type at = key.rfind('@');
	if (at == std::string::npos || at == 0 || at + 1 >= key.size())
		return false;
	std::string c = key.substr(0, at);
	if (!isChannelName(c))
		return false;
	if (channel)
		*channel = c;
	if (net)
		*net = key.substr(at + 1);
	return true;
}

PurpleAccount* accountForNetwork(const std::string& net)
{
	std::string want = lower(net);
	for (GList* c = purple_connections_get_all(); c != NULL; c = c->next)
	{
		PurpleConnection* gc = (PurpleConnection*)c->data;
		PurpleAccount* account = gc ? purple_connection_get_account(gc) : NULL;
		if (isAccount(account) && network(account) == want)
			return account;
	}
	for (GList* a = purple_accounts_get_all(); a != NULL; a = a->next)
	{
		PurpleAccount* account = (PurpleAccount*)a->data;
		if (isAccount(account) && network(account) == want)
			return account;
	}
	return NULL;
}

static const char* chatChannel(PurpleChat* chat)
{
	GHashTable* comps = purple_chat_get_components(chat);
	return comps ? (const char*)g_hash_table_lookup(comps, COMPONENT_CHANNEL) : NULL;
}

PurpleChat* findChat(PurpleAccount* account, const std::string& channel)
{
	if (account == NULL || channel.empty())
		return NULL;
	for (PurpleBlistNode* g = purple_blist_get_root(); g != NULL; g = g->next)
	{
		for (PurpleBlistNode* n = g->child; n != NULL; n = n->next)
		{
			if (!PURPLE_BLIST_NODE_IS_CHAT(n))
				continue;
			PurpleChat* chat = (PurpleChat*)n;
			if (purple_chat_get_account(chat) != account)
				continue;
			const char* name = chatChannel(chat);
			if (name != NULL && sameName(name, channel))
				return chat;
		}
	}
	return NULL;
}

PurpleChat* rememberChannel(PurpleAccount* account, const std::string& channel, const char* key)
{
	if (!isAccount(account) || !isChannelName(channel))
		return NULL;

	PurpleChat* chat = findChat(account, channel);
	if (chat != NULL)
	{
		GHashTable* comps = purple_chat_get_components(chat);
		const char* had = chatChannel(chat);
		bool changed = false;
		if (had == NULL || channel != had)
		{
			g_hash_table_replace(comps, g_strdup(COMPONENT_CHANNEL), g_strdup(channel.c_str()));
			changed = true;
		}
		if (key != NULL && *key != '\0')
		{
			const char* hadKey = (const char*)g_hash_table_lookup(comps, COMPONENT_KEY);
			if (hadKey == NULL || strcmp(hadKey, key) != 0)
			{
				g_hash_table_replace(comps, g_strdup(COMPONENT_KEY), g_strdup(key));
				changed = true;
			}
		}
		if (changed)
			purple_blist_schedule_save();
		return chat;
	}

	GHashTable* comps = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_hash_table_insert(comps, g_strdup(COMPONENT_CHANNEL), g_strdup(channel.c_str()));
	if (key != NULL && *key != '\0')
		g_hash_table_insert(comps, g_strdup(COMPONENT_KEY), g_strdup(key));
	chat = purple_chat_new(account, NULL, comps);   // takes comps

	std::string groupName = network(account);
	if (groupName.empty())
		groupName = "IRC";
	PurpleGroup* group = purple_find_group(groupName.c_str());
	if (group == NULL)
	{
		group = purple_group_new(groupName.c_str());
		purple_blist_add_group(group, NULL);
	}
	// After the group's last node: purple_blist_add_chat with none puts it first, and the order
	// channels were given in is the order the Servers tab shows them in.
	PurpleBlistNode* last = ((PurpleBlistNode*)group)->child;
	while (last != NULL && last->next != NULL)
		last = last->next;
	purple_blist_add_chat(chat, group, last);
	return chat;
}

bool forgetChannel(PurpleAccount* account, const std::string& channel)
{
	PurpleChat* chat = findChat(account, channel);
	if (chat == NULL)
		return false;
	PurpleGroup* group = purple_chat_get_group(chat);
	purple_blist_remove_chat(chat);
	if (group != NULL && ((PurpleBlistNode*)group)->child == NULL)
		purple_blist_remove_group(group);
	return true;
}

std::vector<std::string> rememberedChannels(PurpleAccount* account)
{
	std::vector<std::string> out;
	if (account == NULL)
		return out;
	for (PurpleBlistNode* g = purple_blist_get_root(); g != NULL; g = g->next)
	{
		for (PurpleBlistNode* n = g->child; n != NULL; n = n->next)
		{
			if (!PURPLE_BLIST_NODE_IS_CHAT(n) || purple_chat_get_account((PurpleChat*)n) != account)
				continue;
			const char* name = chatChannel((PurpleChat*)n);
			if (name != NULL && *name != '\0')
				out.push_back(name);
		}
	}
	return out;
}

void seedChannels(PurpleAccount* account, const std::string& configured)
{
	if (!isAccount(account) || purple_account_get_bool(account, SETTING_SEEDED, FALSE))
		return;
	std::vector<std::string> channels = parseChannelList(configured);
	for (size_t i = 0; i < channels.size(); ++i)
		rememberChannel(account, channels[i]);
	purple_account_set_bool(account, SETTING_SEEDED, TRUE);
}

// The account's chat conversation for a channel, if it is in it now.
static PurpleConversation* joinedConversation(PurpleAccount* account, const std::string& channel)
{
	PurpleConversation* conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, channel.c_str(), account);
	if (conv == NULL || purple_conv_chat_has_left(PURPLE_CONV_CHAT(conv)))
		return NULL;
	return conv;
}

static void joinChat(PurpleAccount* account, PurpleChat* chat)
{
	PurpleConnection* gc = purple_account_get_connection(account);
	if (gc == NULL || !PURPLE_CONNECTION_IS_CONNECTED(gc))
		return;   // joined at sign-in instead
	const char* name = chatChannel(chat);
	if (name == NULL || joinedConversation(account, name) != NULL)
		return;
	serv_join_chat(gc, purple_chat_get_components(chat));
}

void joinChannel(PurpleAccount* account, const std::string& channel, const char* key)
{
	PurpleChat* chat = rememberChannel(account, channel, key);
	if (chat != NULL)
		joinChat(account, chat);
}

void joinRemembered(PurpleAccount* account)
{
	if (!isAccount(account))
		return;
	std::vector<std::string> channels = rememberedChannels(account);
	for (size_t i = 0; i < channels.size(); ++i)
	{
		PurpleChat* chat = findChat(account, channels[i]);
		if (chat != NULL)
			joinChat(account, chat);
	}
}

bool leaveChannel(PurpleAccount* account, const std::string& channel)
{
	if (!isAccount(account))
		return false;
	bool known = forgetChannel(account, channel);
	PurpleConversation* conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, channel.c_str(), account);
	if (conv != NULL)
	{
		known = known || !purple_conv_chat_has_left(PURPLE_CONV_CHAT(conv));
		// Destroying a chat that has not been left leaves it on the server first: serv_chat_leave,
		// which for prpl-irc sends the PART.
		purple_conversation_destroy(conv);
	}
	return known;
}

CommandOutcome runCommand(PurpleConversation* conv, const std::string& typed)
{
	CommandOutcome out;
	out.text = typed;

	// "/text" is a command, "//text" the message "/text"; "/" alone, or "/ text", is a message.
	if (typed.size() < 2 || typed[0] != '/' || typed[1] == ' ')
		return out;
	if (typed[1] == '/')
	{
		out.text = typed.substr(1);
		return out;
	}

	std::string cmdline = typed.substr(1);
	std::string::size_type sp = cmdline.find(' ');
	std::string cmd = lower(cmdline.substr(0, sp));
	std::string rest = (sp == std::string::npos) ? std::string() : trim(cmdline.substr(sp + 1));

	if (conv == NULL || !isAccount(purple_conversation_get_account(conv)))
	{
		out.result = COMMAND_FAILED;
		out.error = "not an IRC conversation";
		return out;
	}
	PurpleAccount* account = purple_conversation_get_account(conv);

	// /quit would sign the account out behind the account manager's back; the account's switch
	// in Messaging or Accounts is the way to sign out.
	if (cmd == "quit")
	{
		out.result = COMMAND_FAILED;
		out.error = "/quit is not available; sign the account out instead";
		return out;
	}

	// What a /part leaves: the channels it names (prpl-irc takes a comma list), or the channel it
	// was typed in.
	std::vector<std::string> parting;
	if (cmd == "part")
	{
		std::string first = rest.substr(0, rest.find(' '));
		if (!first.empty() && strchr("#&+!", first[0]) != NULL)
		{
			std::string::size_type i = 0;
			while (i <= first.size())
			{
				std::string::size_type comma = first.find(',', i);
				if (comma == std::string::npos)
					comma = first.size();
				std::string c = first.substr(i, comma - i);
				if (isChannelName(c))
					parting.push_back(c);
				i = comma + 1;
			}
		}
		else if (purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_CHAT)
			parting.push_back(purple_conversation_get_name(conv));
	}

	// libpurple looks the command up as given, and prpl-irc registers them in lower case; IRC
	// clients take /NICK and /Join as well.
	cmdline = rest.empty() ? cmd : (cmd + " " + rest);

	gchar* markup = g_markup_escape_text(cmdline.c_str(), -1);
	gchar* error = NULL;
	PurpleCmdStatus status = purple_cmd_do_command(conv, cmdline.c_str(), markup, &error);
	g_free(markup);

	switch (status)
	{
	case PURPLE_CMD_STATUS_OK:
		out.result = COMMAND_DONE;
		for (size_t i = 0; i < parting.size(); ++i)
		{
			forgetChannel(account, parting[i]);
			out.left.push_back(parting[i]);
		}
		break;
	case PURPLE_CMD_STATUS_NOT_FOUND:
		out.result = COMMAND_FAILED;
		out.error = "no such command: /" + cmd;
		break;
	case PURPLE_CMD_STATUS_WRONG_ARGS:
		out.result = COMMAND_FAILED;
		out.error = "wrong arguments for /" + cmd;
		break;
	default:
		out.result = COMMAND_FAILED;
		out.error = (error != NULL && *error != '\0') ? std::string(error) : ("/" + cmd + " failed");
		break;
	}
	g_free(error);
	return out;
}

SendOutcome send(PurpleAccount* account, const std::string& target, const std::string& typed)
{
	SendOutcome out;

	PurpleConnection* gc = account ? purple_account_get_connection(account) : NULL;
	if (!isAccount(account) || gc == NULL || !PURPLE_CONNECTION_IS_CONNECTED(gc))
	{
		out.error = "not signed in";
		return out;
	}

	std::string channel, net;
	bool toChannel = splitChannelKey(target, &channel, &net);
	if (toChannel && net != network(account))
	{
		out.error = "the channel is on " + net + ", not " + network(account);
		return out;
	}
	if (!toChannel && isChannelName(target))
	{
		channel = target;
		toChannel = true;
	}

	PurpleConversation* conv = NULL;
	if (toChannel)
	{
		conv = joinedConversation(account, channel);
		if (conv == NULL)
		{
			joinChannel(account, channel);
			out.error = "not in " + channel + " yet; joining it";
			return out;
		}
	}
	else
	{
		if (target.empty())
		{
			out.error = "no one to send to";
			return out;
		}
		conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, target.c_str(), account);
		if (conv == NULL)
			conv = purple_conversation_new(PURPLE_CONV_TYPE_IM, account, target.c_str());
	}

	CommandOutcome command = runCommand(conv, typed);
	out.left = command.left;
	if (command.result == COMMAND_DONE)
	{
		out.sent = true;
		return out;
	}
	if (command.result == COMMAND_FAILED)
	{
		out.error = command.error;
		return out;
	}
	if (command.text.empty())
	{
		out.error = "nothing to send";
		return out;
	}

	gchar* html = g_markup_escape_text(command.text.c_str(), -1);
	int rc;
	if (toChannel)
		rc = serv_chat_send(gc, purple_conv_chat_get_id(PURPLE_CONV_CHAT(conv)), html, PURPLE_MESSAGE_SEND);
	else
		rc = serv_send_im(gc, purple_conversation_get_name(conv), html, PURPLE_MESSAGE_SEND);
	g_free(html);
	out.sent = rc >= 0;
	if (!out.sent)
		out.error = "the IRC plug-in refused the message";
	return out;
}

static ChannelsChangedFunc s_channelsChanged = NULL;

static void chatJoinedCb(PurpleConversation* conv, gpointer)
{
	PurpleAccount* account = purple_conversation_get_account(conv);
	if (!isAccount(account))
		return;
	const char* name = purple_conversation_get_name(conv);
	if (name == NULL || !isChannelName(name))
		return;
	rememberChannel(account, name);
	if (s_channelsChanged != NULL)
		s_channelsChanged(account);
}

void watchJoins(void* handle, ChannelsChangedFunc changed)
{
	s_channelsChanged = changed;
	purple_signal_connect(purple_conversations_get_handle(), "chat-joined", handle,
		PURPLE_CALLBACK(chatJoinedCb), NULL);
}

void unwatchJoins(void* handle)
{
	purple_signal_disconnect(purple_conversations_get_handle(), "chat-joined", handle,
		PURPLE_CALLBACK(chatJoinedCb));
	s_channelsChanged = NULL;
}

} // namespace Irc
