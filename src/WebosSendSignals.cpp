/*
 * WebosSendSignals.cpp
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport).
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free Software
 * Foundation.
 *
 * See WebosSendSignals.h.
 */

#include "WebosSendSignals.h"

namespace WebosSendSignals {

namespace {

const char* const EditSignal = "webos-im-send-edit";
const char* const DeleteSignal = "webos-im-send-delete";
const char* const CreateSignal = "webos-im-group-create";
const char* const InviteSignal = "webos-im-group-invite";
const char* const RemoveSignal = "webos-im-group-remove";
const char* const RenameSignal = "webos-im-group-rename";
const char* const LeaveSignal = "webos-im-group-leave";
const char* const OpenSignal = "webos-im-chat-open";

PurpleValue* accountValue() { return purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT); }
PurpleValue* stringValue() { return purple_value_new(PURPLE_TYPE_STRING); }
PurpleValue* booleanValue() { return purple_value_new(PURPLE_TYPE_BOOLEAN); }

// purple_signal_emit_return_1 stops at the first handler that returns non-NULL: TRUE, from the prpl
// that owns the account. NULL when none did, or none is connected.
bool taken(const void* ret) { return ret != NULL; }

} // namespace

void registerSignals(void* handle)
{
	purple_signal_register(handle, EditSignal, purple_marshal_BOOLEAN__POINTER_POINTER_POINTER_POINTER,
			booleanValue(), 4, accountValue(), stringValue(), stringValue(), stringValue());
	purple_signal_register(handle, DeleteSignal, purple_marshal_BOOLEAN__POINTER_POINTER_POINTER,
			booleanValue(), 3, accountValue(), stringValue(), stringValue());
	purple_signal_register(handle, CreateSignal, purple_marshal_BOOLEAN__POINTER_POINTER_POINTER,
			booleanValue(), 3, accountValue(), stringValue(), stringValue());
	purple_signal_register(handle, InviteSignal, purple_marshal_BOOLEAN__POINTER_POINTER_POINTER,
			booleanValue(), 3, accountValue(), stringValue(), stringValue());
	purple_signal_register(handle, RemoveSignal, purple_marshal_BOOLEAN__POINTER_POINTER_POINTER,
			booleanValue(), 3, accountValue(), stringValue(), stringValue());
	purple_signal_register(handle, RenameSignal, purple_marshal_BOOLEAN__POINTER_POINTER_POINTER,
			booleanValue(), 3, accountValue(), stringValue(), stringValue());
	purple_signal_register(handle, LeaveSignal, purple_marshal_BOOLEAN__POINTER_POINTER,
			booleanValue(), 2, accountValue(), stringValue());
	purple_signal_register(handle, OpenSignal, purple_marshal_BOOLEAN__POINTER_POINTER,
			booleanValue(), 2, accountValue(), stringValue());
}

bool edit(void* handle, PurpleAccount* account, const char* chat, const char* serviceMessageId, const char* text)
{
	return taken(purple_signal_emit_return_1(handle, EditSignal, account, chat, serviceMessageId, text));
}

bool deleteMessage(void* handle, PurpleAccount* account, const char* chat, const char* serviceMessageId)
{
	return taken(purple_signal_emit_return_1(handle, DeleteSignal, account, chat, serviceMessageId));
}

bool createGroup(void* handle, PurpleAccount* account, const char* name, const char* members)
{
	return taken(purple_signal_emit_return_1(handle, CreateSignal, account, name, members));
}

bool inviteToGroup(void* handle, PurpleAccount* account, const char* chat, const char* members)
{
	return taken(purple_signal_emit_return_1(handle, InviteSignal, account, chat, members));
}

bool removeFromGroup(void* handle, PurpleAccount* account, const char* chat, const char* members)
{
	return taken(purple_signal_emit_return_1(handle, RemoveSignal, account, chat, members));
}

bool renameGroup(void* handle, PurpleAccount* account, const char* chat, const char* name)
{
	return taken(purple_signal_emit_return_1(handle, RenameSignal, account, chat, name));
}

bool leaveGroup(void* handle, PurpleAccount* account, const char* chat)
{
	return taken(purple_signal_emit_return_1(handle, LeaveSignal, account, chat));
}

bool openChat(void* handle, PurpleAccount* account, const char* chat)
{
	return taken(purple_signal_emit_return_1(handle, OpenSignal, account, chat));
}

} // namespace WebosSendSignals
