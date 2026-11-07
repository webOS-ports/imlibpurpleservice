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
 *   edited   true once the text was replaced by an edit; the app marks the bubble "Edited"
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

	// The merge for row dbId after a delete for everyone: the text, the file and the quote and
	// reactions on it go.
	MojErr deleted(const MojString& dbId, MojObject& out);
}

#endif /* MESSAGECHANGES_H_ */
