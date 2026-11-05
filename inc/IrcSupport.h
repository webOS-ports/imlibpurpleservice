/*
 * IrcSupport.h
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport).
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free Software
 * Foundation.
 *
 * IRC through libpurple's own prpl-irc. What the other room protocols get from their plug-in
 * (a buddy list full of channels, a stable channel id, a network to group them under) IRC does
 * not have, so this file supplies it:
 *
 *   - the channels an account sits in are kept as chats in the buddy list, under a group named
 *     after the network, so they are joined again at every sign-in and show in the Servers tab;
 *   - a channel is known to db8 and the Messaging app as "<#channel>@<network>", in lower case:
 *     "#linux" on two networks is two channels, and the key alone says which account owns it;
 *   - a message typed as "/command ..." runs as that command, the way any IRC client does it.
 *
 * Only libpurple and glib are used here, so the tests can drive it against the real prpl-irc
 * without db8 or the bus.
 */

#ifndef IRCSUPPORT_H_
#define IRCSUPPORT_H_

#include <string>
#include <vector>

#include <purple.h>

namespace Irc
{

extern const char* const PRPL_ID;        // "prpl-irc"
extern const char* const SERVICE_NAME;   // "type_irc"

// What the sign-in app stores as the credential when the network wants no password. The account
// service cannot hold an empty one, and imlibpurpleservice refuses to sign in without one.
extern const char* const NO_PASSWORD;

bool isAccount(PurpleAccount* account);

// The server an account connects to, from its "nick@server" username, in lower case. This is
// the network's name in the Servers tab and the part after the '@' of every channel key.
std::string network(PurpleAccount* account);

// A channel name as IRC allows one: a #, &, + or ! prefix and no space, comma, colon, BEL, CR,
// LF or NUL, at most 200 bytes.
bool isChannelName(const std::string& name);

// What a person typed as a channel ("linux", " #Linux ") made into a channel name ("#linux",
// "#Linux"); "" if it cannot be one.
std::string channelFromInput(const std::string& typed);

// A list of channels as typed in the sign-in app (commas, spaces or new lines between them),
// each made a channel name, invalid ones dropped, the same channel in another case kept once.
std::vector<std::string> parseChannelList(const std::string& text);

// "<channel>@<network>", lower case, and back. splitChannelKey is false for anything that is not
// a channel key, such as a nick.
std::string channelKey(const std::string& channel, const std::string& net);
bool splitChannelKey(const std::string& key, std::string* channel, std::string* net);

// The IRC account for a network: a connected one if there is one, else any, else NULL.
PurpleAccount* accountForNetwork(const std::string& net);

// The buddy-list chat holding a channel for this account, matched without regard to case.
PurpleChat* findChat(PurpleAccount* account, const std::string& channel);

// Keep a channel in the buddy list (under the network's group), or drop it. rememberChannel
// returns the chat, new or existing; an existing one takes the new spelling and, if given, key.
PurpleChat* rememberChannel(PurpleAccount* account, const std::string& channel, const char* key = NULL);
bool forgetChannel(PurpleAccount* account, const std::string& channel);
std::vector<std::string> rememberedChannels(PurpleAccount* account);

// The first time an account signs in, the channels the sign-in app was given become its
// remembered channels. Later sign-ins leave the list to what the person joined and left.
void seedChannels(PurpleAccount* account, const std::string& configured);

// Join a channel and remember it; join every remembered channel (at sign-in). Joining a channel
// the account is already in does nothing.
void joinChannel(PurpleAccount* account, const std::string& channel, const char* key = NULL);
void joinRemembered(PurpleAccount* account);

// Leave a channel (PART, through the conversation's own teardown) and forget it. True if it was
// remembered or joined.
bool leaveChannel(PurpleAccount* account, const std::string& channel);

// A message as typed. "/cmd args" runs as an IRC command in conv, "//text" is sent as "/text",
// anything else is a plain message.
enum CommandResult
{
	NOT_A_COMMAND,   // send *text as a message
	COMMAND_DONE,    // it ran; nothing to send
	COMMAND_FAILED   // it did not run; *error says why
};
struct CommandOutcome
{
	CommandResult result = NOT_A_COMMAND;
	std::string text;                 // NOT_A_COMMAND: what to send
	std::string error;                // COMMAND_FAILED: why
	std::vector<std::string> left;    // channels a /part asked to leave (forgotten already)
};
CommandOutcome runCommand(PurpleConversation* conv, const std::string& typed);

// Send what was typed to target: a channel key ("#chan@network"), a bare channel, or a nick.
// A command runs (see runCommand); anything else goes out as a message, as plain text: prpl-irc
// reads what it is handed as HTML, so the text is escaped first and "a < b & c" arrives intact.
// A channel the account is not in yet is joined, and the message fails: it cannot be sent
// until the server has let the account in.
struct SendOutcome
{
	bool sent = false;
	std::string error;                // !sent: why
	std::vector<std::string> left;    // channels a /part left (forgotten already)
};
SendOutcome send(PurpleAccount* account, const std::string& target, const std::string& typed);

// Called with the account whenever its set of channels changed: a channel was joined (from here,
// from a /join, or by the server) or left. The transport refreshes the Servers tab from it.
typedef void (*ChannelsChangedFunc)(PurpleAccount* account);

// Follow joins on IRC accounts: a channel the account is now in is remembered, with the spelling
// the server gave it, and changed() is told. handle owns the signal connection.
void watchJoins(void* handle, ChannelsChangedFunc changed);
void unwatchJoins(void* handle);

} // namespace Irc

#endif /* IRCSUPPORT_H_ */
