/*
 * ChannelOptions.cpp
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport). See ChannelOptions.h.
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free Software
 * Foundation.
 */

#include "ChannelOptions.h"

#include <cstring>

namespace ChannelOptions
{

const char* const SETTING_SIGNAL = "webos-im-channel-setting";
const char* const CHANGED_SIGNAL = "webos-im-channels-changed";

bool supports(const PurpleAccount* account)
{
	const char* protoId = account ? purple_account_get_protocol_id(account) : NULL;
	return protoId != NULL && strcmp(protoId, "prpl-discord") == 0;
}

void registerSignals()
{
	static bool s_registered = false;
	if (s_registered)
		return;
	s_registered = true;

	void* convHandle = purple_conversations_get_handle();
	purple_signal_register(convHandle, SETTING_SIGNAL,
			purple_marshal_VOID__POINTER_POINTER_POINTER_POINTER_POINTER, NULL, 5,
			purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_STRING));
	purple_signal_register(convHandle, CHANGED_SIGNAL,
			purple_marshal_VOID__POINTER, NULL, 1,
			purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT));
}

Flags flagsOf(PurpleChat* chat)
{
	Flags flags;
	if (chat == NULL)
		return flags;
	PurpleBlistNode* node = reinterpret_cast<PurpleBlistNode*>(chat);   // its first member
	flags.muted = purple_blist_node_get_bool(node, "muted");
	flags.background = purple_blist_node_get_bool(node, "webos-background");
	flags.serverMuted = purple_blist_node_get_bool(node, "webos-server-muted");
	flags.serverBackground = purple_blist_node_get_bool(node, "webos-server-background");
	return flags;
}

const char* channelId(PurpleChat* chat)
{
	GHashTable* comps = chat ? purple_chat_get_components(chat) : NULL;
	const char* id = comps ? static_cast<const char*>(g_hash_table_lookup(comps, "id")) : NULL;
	return id != NULL && *id != '\0' ? id : NULL;
}

PurpleChat* findChannel(const PurpleAccount* account, const std::string& channelId)
{
	if (account == NULL || channelId.empty())
		return NULL;
	for (PurpleBlistNode* group = purple_blist_get_root(); group != NULL; group = group->next)
	{
		if (!PURPLE_BLIST_NODE_IS_GROUP(group))
			continue;
		for (PurpleBlistNode* child = group->child; child != NULL; child = child->next)
		{
			if (!PURPLE_BLIST_NODE_IS_CHAT(child))
				continue;
			PurpleChat* chat = reinterpret_cast<PurpleChat*>(child);
			const char* id = ChannelOptions::channelId(chat);
			if (purple_chat_get_account(chat) == account && id != NULL && channelId == id)
				return chat;
		}
	}
	return NULL;
}

PurpleChat* findServerChat(PurpleAccount* account, const std::string& server, ServerOf serverOf)
{
	if (account == NULL || server.empty() || serverOf == NULL)
		return NULL;
	for (PurpleBlistNode* group = purple_blist_get_root(); group != NULL; group = group->next)
	{
		if (!PURPLE_BLIST_NODE_IS_GROUP(group))
			continue;
		const char* groupName = purple_group_get_name(reinterpret_cast<PurpleGroup*>(group));
		if (groupName == NULL || serverOf(account, groupName) != server)
			continue;
		for (PurpleBlistNode* child = group->child; child != NULL; child = child->next)
		{
			if (!PURPLE_BLIST_NODE_IS_CHAT(child))
				continue;
			PurpleChat* chat = reinterpret_cast<PurpleChat*>(child);
			if (purple_chat_get_account(chat) == account && ChannelOptions::channelId(chat) != NULL)
				return chat;
		}
	}
	return NULL;
}

bool set(PurpleAccount* account, PurpleChat* chat, Scope scope, const char* setting, bool value)
{
	if (!supports(account) || chat == NULL || setting == NULL)
		return false;
	if (strcmp(setting, "muted") != 0 && strcmp(setting, "background") != 0)
		return false;
	const char* const id = channelId(chat);
	if (id == NULL)
		return false;
	purple_signal_emit(purple_conversations_get_handle(), SETTING_SIGNAL, account, id,
			scope == SERVER ? "server" : "channel", setting, value ? "1" : "0");
	return true;
}

}
