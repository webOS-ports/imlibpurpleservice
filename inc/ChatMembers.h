/*
 * ChatMembers.h
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport).
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free Software
 * Foundation.
 *
 * Who is in a group chat, for the Messaging app's Group Info (getChatMembers). libpurple keeps a
 * chat's people on its conversation (purple_conv_chat_get_users) once the prpl has put them there;
 * LibpurpleAdapter::chatMembers reads them and gives each the address and name the app knows them
 * by, as an incoming group message's sender gets them, and the phone number the prpl keeps on the
 * chat buddy (attribute PHONE_ATTRIBUTE) when it has one: a WhatsApp group addressed by LID gives
 * its members as "<id>@lid", which says nothing of the number. This part tidies that list and
 * writes the reply:
 *
 *   { "returnValue": true, "inChat": <a conversation was found>, ["joining": true,]
 *     "members": [ { "address": "+31611111111", "name": "Anna" },
 *                  { "address": "123456789012345@lid", "name": "Marta", "phone": "+46701234567" },
 *                  ... ] }
 *
 * "phone" is there only when the number is known.
 *
 * libpurple has a chat's conversation only once the prpl opened it, which most do when the first
 * message in it arrives. For a group in the buddy list that has none yet, getChatMembers asks the
 * prpl to open it (serv_join_chat) when that prpl's join_chat only opens the conversation and puts
 * the members on it (joinOpensMembers). Any other prpl is asked to open it with the
 * webos-im-chat-open signal (WebosSendSignals.h), which one whose join_chat does more takes for
 * its own chats (purple-discord: a group DM). openWay holds the choice. The reply then also has
 * "joining": true when the members may still come: the app asks again.
 *
 * Nothing here touches libpurple, so it can be tested on its own.
 */

#ifndef CHATMEMBERS_H_
#define CHATMEMBERS_H_

#include <string>
#include <vector>
#include "core/MojObject.h"

namespace ChatMembers {

/** The chat buddy attribute (purple_conv_chat_cb_get_attribute) a prpl keeps a member's phone
 *  number in, "+<digits>"; purple-combined's WhatsApp plugin sets it. */
extern const char* const PHONE_ATTRIBUTE;

struct Member {
	std::string address;
	std::string name;
	std::string phone; ///< "+<digits>", or empty when not known
};

/**
 * The members to answer with: each address once (the first name that is not empty, the first
 * phone), none without an address, not own (our own address on the service), names without the
 * "\x1e"/"\x1f" separators the app's records use and trimmed, and the address as the name when
 * there is none. A phone is kept only as "+" and digits (spaces around it trimmed); anything else
 * is no phone. In the order given.
 */
std::vector<Member> clean(const std::vector<Member>& members, const std::string& own);

/**
 * Whether getChatMembers may open chat (as libpurple names it) with serv_join_chat when the account
 * has no conversation for it: the prpl's join_chat must open the conversation and put the members
 * on it without fetching history, marking anything read or sending anything but a query.
 *  - prpl-telegram (tdlib-purple): PurpleTdClient::joinChat opens it from what tdlib already has,
 *    at once, and fills the members tdlib has given.
 *  - prpl-hehoe-whatsmeow (purple-combined): gowhatsapp_enter_group_chat opens it and asks WhatsApp
 *    for the participants (GetGroupInfo), waiting for the answer. Not a Channel ("@newsletter"),
 *    whose join fetches its posts, nor "status@broadcast", which has no participants.
 *  - prpl-gometa (purple-combined's Facebook): gometa_join_chat only opens the conversation; the
 *    Go half then puts the members it has from Facebook's thread rows on it (gometa_go_chat_joined,
 *    a moment later, as a queued message), and asks Facebook for nothing.
 * Not purple-discord: its join_chat fetches the channel's messages every time. Nor any other prpl.
 */
bool joinOpensMembers(const std::string& protocolId, const std::string& chat);

/**
 * Whether a chat of a joinOpensMembers prpl that is NOT in the buddy list may still be joined, with
 * the components the prpl's own chat_info_defaults(gc, chat) makes (as Pidgin and Finch rejoin a
 * chat that has none). Only names that prpl gives its groups, so that what its join_chat does with
 * them is known:
 *  - prpl-hehoe-whatsmeow: a group JID, "<digits>@g.us" or "<digits>-<digits>@g.us".
 *    gowhatsapp_chat_info_defaults gives {name: chat, topic: ""}; gowhatsapp_join_chat puts the
 *    group in the buddy list and opens it, asking WhatsApp for its participants (GetGroupInfo,
 *    retried in the background when it fails). A JID it cannot parse is a fatal account error
 *    there (purple_error ERROR_FATAL), hence the strict form. Never "@newsletter" or
 *    "status@broadcast".
 *  - prpl-telegram: "chat<id>", the id a decimal number, negative for supergroups.
 *    tgprpl_chat_info_defaults gives {id: chat}; tgprpl_chat_join opens it only from what tdlib
 *    already has, sending nothing, and reports chat-join-failed for one it does not know or is
 *    not a member of.
 *  - prpl-gometa: a thread key, a positive decimal int64 (at most 19 digits, no leading zero).
 *    gometa_chat_info_defaults gives {id: chat}; gometa_join_chat opens it, sending nothing, and
 *    the Go half puts members on it only for a group it knows.
 */
bool joinFromDefaults(const std::string& protocolId, const std::string& chat);

/** How getChatMembers opens a chat (as libpurple names it) that has no conversation yet. */
enum OpenWay {
	DontOpen,	///< leave it: not connected, no chat, or a joining prpl's chat it cannot join
	OpenByJoin,	///< serv_join_chat with the buddy-list chat's components (joinOpensMembers)
	OpenByJoinDefaults,	///< serv_join_chat with the prpl's chat_info_defaults (joinFromDefaults)
	OpenBySignal	///< emit webos-im-chat-open; the prpl that owns the chat opens it, or none takes it
};

/**
 * Which way, if any. connected: the account is signed on (and has a connection);
 * inBuddyList: the chat is in the buddy list, whose components serv_join_chat needs.
 * A prpl whose join_chat only opens the chat is joined, and only that way: with the buddy-list
 * chat's components, else with the prpl's defaults where joinFromDefaults allows it. Any other is
 * asked with the signal, in or out of the buddy list.
 */
OpenWay openWay(const std::string& protocolId, const std::string& chat, bool inBuddyList, bool connected);

/** Whether the reply says "joining": true. joinAsked: serv_join_chat was called in this request.
 *  Then the members may still come when the conversation is not there or has no one on it yet. */
bool stillJoining(bool joinAsked, bool inChat, size_t memberCount);

/** The getChatMembers reply for members (already cleaned); "joining": true only when joining. */
MojErr toPayload(const std::vector<Member>& members, bool inChat, MojObject& out);
MojErr toPayload(const std::vector<Member>& members, bool inChat, bool joining, MojObject& out);

} // namespace ChatMembers

#endif /* CHATMEMBERS_H_ */
