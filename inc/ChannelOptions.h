/*
 * ChannelOptions.h
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport).
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free Software
 * Foundation.
 *
 * What the Messaging app's Servers tab can choose per server and per channel of a room account:
 * whether it is muted, and whether its messages arrive while the channel is not open. Only
 * prpl-discord offers them so far.
 *
 * The prpl keeps the state on each channel's blist chat, as booleans:
 *   "muted"                    the channel is muted, by itself or with its server
 *   "webos-background"         its messages arrive while it is not open
 *   "webos-server-muted"       its server is muted
 *   "webos-server-background"  its server delivers all of its channels
 * and emits "webos-im-channels-changed" (account) when any of them changed. The transport changes
 * them by emitting "webos-im-channel-setting" (account, channel id, scope, setting, value), scope
 * "channel" or "server" (that channel's server), setting "muted" or "background", value "1"/"0".
 *
 * Only libpurple and glib are used here, so the tests can drive it without db8 or the bus.
 */

#ifndef CHANNELOPTIONS_H_
#define CHANNELOPTIONS_H_

#include <string>

#include <purple.h>

namespace ChannelOptions
{

extern const char* const SETTING_SIGNAL;   // "webos-im-channel-setting"
extern const char* const CHANGED_SIGNAL;   // "webos-im-channels-changed"

struct Flags
{
	bool muted = false;
	bool background = false;
	bool serverMuted = false;
	bool serverBackground = false;
};

// Whether the account's prpl takes these settings.
bool supports(const PurpleAccount* account);

// Register both signals on purple_conversations_get_handle(); once, before any prpl signs in.
void registerSignals();

// The state a chat carries.
Flags flagsOf(PurpleChat* chat);

// The channel id a chat is joined by: its "id" component, owned by the chat; NULL if it has none.
const char* channelId(PurpleChat* chat);

// The account's chat whose channel id is channelId; NULL if there is none.
PurpleChat* findChannel(const PurpleAccount* account, const std::string& channelId);

// Maps a blist group to the server it belongs to, "" for none (the transport's deriveServerName).
typedef std::string (*ServerOf)(PurpleAccount* account, const char* groupName);

// Any one chat of the account under the server, which stands for the server in a setting.
PurpleChat* findServerChat(PurpleAccount* account, const std::string& server, ServerOf serverOf);

enum Scope { CHANNEL, SERVER };

// Ask the prpl to change a setting ("muted" or "background") of the chat's channel or of its
// server. False when the account does not take settings or the chat has no channel id.
bool set(PurpleAccount* account, PurpleChat* chat, Scope scope, const char* setting, bool value);

}

#endif /* CHANNELOPTIONS_H_ */
