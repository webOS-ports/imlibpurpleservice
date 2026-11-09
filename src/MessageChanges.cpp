/*
 * MessageChanges.cpp - see MessageChanges.h
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport).
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free Software
 * Foundation.
 */
#include "MessageChanges.h"
#include "IMMessage.h"          // MOJDB_* field names

namespace MessageChanges
{

// Mirrors what WhatsApp's own clients show for a revoked message.
const char* const DeletedText = "This message was deleted.";

bool editApplies(const MojObject& row)
{
	bool wasDeleted = false;
	return !(row.get(MOJDB_DELETED, wasDeleted) && wasDeleted);
}

MojErr edited(const MojString& dbId, const MojString& newText, MojObject& out)
{
	MojErr err = out.putString(MOJDB_ID, dbId);       MojErrCheck(err);
	err = out.putString(MOJDB_MSG_TEXT, newText);     MojErrCheck(err);
	err = out.putBool(MOJDB_EDITED, true);            MojErrCheck(err);
	return MojErrNone;
}

MojErr replaced(const MojString& dbId, const MojString& newText, MojObject& out)
{
	MojErr err = out.putString(MOJDB_ID, dbId);       MojErrCheck(err);
	err = out.putString(MOJDB_MSG_TEXT, newText);     MojErrCheck(err);
	return MojErrNone;
}

MojErr textChanged(const MojString& dbId, const MojString& newText, bool bySender, MojObject& out)
{
	return bySender ? edited(dbId, newText, out) : replaced(dbId, newText, out);
}

MojErr deleted(const MojString& dbId, MojObject& out)
{
	MojErr err = out.putString(MOJDB_ID, dbId);       MojErrCheck(err);
	err = out.putString(MOJDB_MSG_TEXT, DeletedText); MojErrCheck(err);
	err = out.putBool(MOJDB_DELETED, true);           MojErrCheck(err);
	// Nothing of what it said is drawn any more: no attachment, quote card or reactions.
	err = out.putString(MOJDB_FILE_PATH, _T(""));     MojErrCheck(err);
	err = out.putString(MOJDB_QUOTED_TEXT, _T(""));   MojErrCheck(err);
	err = out.putString(MOJDB_QUOTED_FROM, _T(""));   MojErrCheck(err);
	err = out.putString(MOJDB_QUOTED_MSG_ID, _T("")); MojErrCheck(err);
	const MojObject none(MojObject::TypeArray);
	err = out.put(MOJDB_REACTIONS, none);             MojErrCheck(err);
	return MojErrNone;
}

}
