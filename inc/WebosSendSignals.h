/*
 * WebosSendSignals.h
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport).
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free Software
 * Foundation.
 *
 * The signals that take what the Messaging app does to a message or a group to the prpls (B3, B4),
 * on purple_conversations_get_handle(). A prpl connects to them and returns TRUE for an account
 * of its own that it acted for:
 *
 *   webos-im-send-edit    (PurpleAccount*, const char* chat, const char* serviceMessageId, const char* text)
 *   webos-im-send-delete  (PurpleAccount*, const char* chat, const char* serviceMessageId)
 *   webos-im-group-create (PurpleAccount*, const char* name, const char* members)
 *   webos-im-group-invite (PurpleAccount*, const char* chat, const char* members)
 *   webos-im-group-remove (PurpleAccount*, const char* chat, const char* members)
 *   webos-im-group-rename (PurpleAccount*, const char* chat, const char* name)
 *   webos-im-group-leave  (PurpleAccount*, const char* chat)
 *   webos-im-chat-open    (PurpleAccount*, const char* chat)
 *   webos-im-download-file (PurpleAccount*, const char* chat, const char* serviceMessageId)
 *
 * members: buddy names joined by "\x1f"; name: the group's new name. Each emit says whether a prpl took it.
 *
 * webos-im-chat-open asks a prpl whose join_chat does more than open the chat (purple-discord's
 * fetches its messages) to open the chat conversation and put its members on it, and nothing else:
 * no history, nothing marked read, no line written. getChatMembers emits it for a chat that has no
 * conversation yet (ChatMembers::openWay). TRUE: the conversation is open, its members on it or
 * on their way.
 *
 * webos-im-download-file asks a prpl for the file of a message it offered rather than fetched
 * (tdlib-purple: a video, a music file, a document over 1 MB), as the user tapped its card. TRUE:
 * the prpl asked for it; it rewrites the message as the file comes ("webos-im-replace").
 */

#ifndef WEBOSSENDSIGNALS_H_
#define WEBOSSENDSIGNALS_H_

#include <purple.h>
#include <string>
#include "core/MojObject.h"

namespace WebosSendSignals {

/** Register the signals on handle (purple_conversations_get_handle()). Once, before any login. */
void registerSignals(void* handle);

bool edit(void* handle, PurpleAccount* account, const char* chat, const char* serviceMessageId, const char* text);
bool deleteMessage(void* handle, PurpleAccount* account, const char* chat, const char* serviceMessageId);
bool createGroup(void* handle, PurpleAccount* account, const char* name, const char* members);
bool inviteToGroup(void* handle, PurpleAccount* account, const char* chat, const char* members);
bool removeFromGroup(void* handle, PurpleAccount* account, const char* chat, const char* members);
bool renameGroup(void* handle, PurpleAccount* account, const char* chat, const char* name);
bool leaveGroup(void* handle, PurpleAccount* account, const char* chat);
bool openChat(void* handle, PurpleAccount* account, const char* chat);
bool downloadFile(void* handle, PurpleAccount* account, const char* chat, const char* serviceMessageId);

/** The serviceMessageId a downloadFile imcommand's params name ("messageId"); "" if none. */
std::string downloadFileMessageId(const MojObject& params);

} // namespace WebosSendSignals

#endif /* WEBOSSENDSIGNALS_H_ */
