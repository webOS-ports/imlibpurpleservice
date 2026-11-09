/*
 * MessageChanges.h
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport).
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free Software
 * Foundation.
 *
 * What EditHandler and DeleteHandler write onto an immessage row when the sender edits a message
 * or deletes it for everyone, kept apart from the db calls so it can be tested on its own.
 *
 *   edited   true once the text was replaced by an edit; the app marks the bubble "Edited". Not set
 *            where the prpl rewrites what it gave itself ("webos-im-replace").
 *   deleted  true once the sender deleted it for everyone; the app draws the bubble as deleted.
 *            messageText still says so in words, for the thread summary and older apps.
 *
 * A deleted message takes no more edits: its text is gone, and an edit arriving late (the
 * networks do not promise the order) must not bring it back.
 */
#ifndef MESSAGECHANGES_H_
#define MESSAGECHANGES_H_

#include "core/MojObject.h"
#include "core/MojString.h"

#define MOJDB_EDITED                _T("edited")
#define MOJDB_DELETED               _T("deleted")

namespace MessageChanges
{
	// The text a deleted message keeps in messageText.
	extern const char* const DeletedText;

	// Whether an edit may be applied to this stored row.
	bool editApplies(const MojObject& row);

	// The merge for row dbId after an edit to newText (HTML-escaped, like any incoming body).
	MojErr edited(const MojString& dbId, const MojString& newText, MojObject& out);

	// The merge for row dbId whose text the prpl rewrote itself, not the sender (the card of a file
	// as it downloads): the text alone. edited stays as it was, so the app says "Edited" for the
	// sender's edits only.
	MojErr replaced(const MojString& dbId, const MojString& newText, MojObject& out);

	// edited() where the sender changed the text, replaced() where the prpl did.
	MojErr textChanged(const MojString& dbId, const MojString& newText, bool bySender, MojObject& out);

	// The merge for row dbId after a delete for everyone: the text, the file and the quote and
	// reactions on it go.
	MojErr deleted(const MojString& dbId, MojObject& out);

	// The flags an incoming row is stored with: noNotification for a muted chat's message or a
	// prpl's own line (muted), read and noNotification for an old message from the history the
	// prpl was given (history): no banner, and not unread. any is false when it needs none.
	MojErr incomingFlags(bool muted, bool history, MojObject& flags, bool& any);
}

#endif /* MESSAGECHANGES_H_ */
