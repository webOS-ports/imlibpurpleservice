/*
 * messagechanges_test.cpp
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport).
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free Software
 * Foundation.
 *
 * MessageChanges.cpp: what an edit and a delete for everyone write onto an immessage row, and
 * that a deleted row takes no more edits. The merges are checked field by field against the
 * names the app reads (edited, deleted, messageText, filePath, quoted*, reactions).
 */

#include "MessageChanges.h"
#include "IMMessage.h"

#include <gtest/gtest.h>

#include <string>

namespace {

MojString str(const char* s)
{
	MojString out;
	EXPECT_EQ(MojErrNone, out.assign(s));
	return out;
}

std::string getString(const MojObject& obj, const char* key)
{
	MojString value;
	bool found = false;
	EXPECT_EQ(MojErrNone, obj.get(key, value, found)) << key;
	EXPECT_TRUE(found) << key;
	return value.data();
}

bool getBool(const MojObject& obj, const char* key)
{
	bool value = false;
	EXPECT_TRUE(obj.get(key, value)) << key;
	return value;
}

size_t keys(const MojObject& obj)
{
	size_t n = 0;
	for (MojObject::ConstIterator it = obj.begin(); it != obj.end(); ++it)
		++n;
	return n;
}

}  // namespace

TEST(MessageChanges, EditReplacesTheTextAndMarksIt)
{
	MojObject out;
	ASSERT_EQ(MojErrNone, MessageChanges::edited(str("++abc"), str("now &amp; then"), out));
	EXPECT_EQ("++abc", getString(out, MOJDB_ID));
	EXPECT_EQ("now &amp; then", getString(out, MOJDB_MSG_TEXT));
	EXPECT_TRUE(getBool(out, "edited"));
	// Nothing else on the row changes: the file, quote and reactions stay.
	EXPECT_EQ(3u, keys(out));
	EXPECT_FALSE(out.contains(MOJDB_DELETED));
}

TEST(MessageChanges, DeleteLeavesOnlyThatItWasDeleted)
{
	MojObject out;
	ASSERT_EQ(MojErrNone, MessageChanges::deleted(str("++abc"), out));
	EXPECT_EQ("++abc", getString(out, MOJDB_ID));
	EXPECT_TRUE(getBool(out, "deleted"));
	EXPECT_EQ("This message was deleted.", getString(out, MOJDB_MSG_TEXT));
	EXPECT_EQ(std::string(MessageChanges::DeletedText), getString(out, MOJDB_MSG_TEXT));
	EXPECT_EQ("", getString(out, MOJDB_FILE_PATH));
	EXPECT_EQ("", getString(out, MOJDB_QUOTED_TEXT));
	EXPECT_EQ("", getString(out, MOJDB_QUOTED_FROM));
	EXPECT_EQ("", getString(out, MOJDB_QUOTED_MSG_ID));
	MojObject reactions;
	ASSERT_TRUE(out.get(MOJDB_REACTIONS, reactions));
	EXPECT_EQ(MojObject::TypeArray, reactions.type());
	EXPECT_EQ(0u, reactions.size());
	EXPECT_FALSE(out.contains(MOJDB_EDITED));
	EXPECT_EQ(8u, keys(out));
}

TEST(MessageChanges, AnEditAppliesUnlessTheRowWasDeleted)
{
	MojObject plain;
	EXPECT_TRUE(MessageChanges::editApplies(plain));

	MojObject edited;
	ASSERT_EQ(MojErrNone, edited.putBool(MOJDB_EDITED, true));
	EXPECT_TRUE(MessageChanges::editApplies(edited));

	MojObject notDeleted;
	ASSERT_EQ(MojErrNone, notDeleted.putBool(MOJDB_DELETED, false));
	EXPECT_TRUE(MessageChanges::editApplies(notDeleted));

	MojObject deleted;
	ASSERT_EQ(MojErrNone, MessageChanges::deleted(str("++abc"), deleted));
	EXPECT_FALSE(MessageChanges::editApplies(deleted));
}

TEST(MessageChanges, AnEditAfterADeleteChangesNothingTheAppShows)
{
	// What the handlers do in order: delete, then a late edit is checked against the stored row.
	MojObject row;
	ASSERT_EQ(MojErrNone, MessageChanges::deleted(str("++abc"), row));
	ASSERT_FALSE(MessageChanges::editApplies(row));
	EXPECT_EQ("This message was deleted.", getString(row, MOJDB_MSG_TEXT));
}

TEST(MessageChanges, APrplsRewriteReplacesTheTextOnly)
{
	// "webos-im-replace": the prpl's own rewrite (a file's card as it downloads). The text changes;
	// edited is not written, so a message never edited by its sender is not drawn "Edited", and one
	// that was keeps it.
	MojObject out;
	ASSERT_EQ(MojErrNone, MessageChanges::replaced(str("++abc"), str("file:///a%20b.mp4\ncap"), out));
	EXPECT_EQ("++abc", getString(out, MOJDB_ID));
	EXPECT_EQ("file:///a%20b.mp4\ncap", getString(out, MOJDB_MSG_TEXT));
	EXPECT_FALSE(out.contains(MOJDB_EDITED));
	EXPECT_FALSE(out.contains(MOJDB_DELETED));
	EXPECT_EQ(2u, keys(out));
}

TEST(MessageChanges, TheSendersEditIsMarkedThePrplsRewriteIsNot)
{
	// What EditHandler writes for "webos-im-edit" (by the sender) and "webos-im-replace" (not).
	MojObject bySender;
	ASSERT_EQ(MojErrNone, MessageChanges::textChanged(str("++abc"), str("new"), true, bySender));
	EXPECT_EQ("new", getString(bySender, MOJDB_MSG_TEXT));
	EXPECT_TRUE(getBool(bySender, "edited"));
	EXPECT_EQ(3u, keys(bySender));

	MojObject byPrpl;
	ASSERT_EQ(MojErrNone, MessageChanges::textChanged(str("++abc"),
			str("luneos-file:?name=a.zip&amp;size=2000000&amp;type=document&amp;state=downloading"), false, byPrpl));
	EXPECT_EQ("luneos-file:?name=a.zip&amp;size=2000000&amp;type=document&amp;state=downloading",
			getString(byPrpl, MOJDB_MSG_TEXT));
	EXPECT_FALSE(byPrpl.contains(MOJDB_EDITED));
	EXPECT_EQ(2u, keys(byPrpl));
}

TEST(MessageChanges, ARewriteAfterADeleteIsNotApplied)
{
	// EditHandler checks editApplies for a rewrite as for an edit: a deleted file's card stays deleted.
	MojObject row;
	ASSERT_EQ(MojErrNone, MessageChanges::deleted(str("++abc"), row));
	EXPECT_FALSE(MessageChanges::editApplies(row));
}

// An incoming row's flags: none for an ordinary message, no banner for a muted chat's message or a
// prpl's own line, and no banner and read for an old message from the history the prpl was given,
// muted or not, so it is not counted as unread either.
TEST(MessageChanges, IncomingFlags)
{
	MojObject none;
	bool any = true;
	ASSERT_EQ(MojErrNone, MessageChanges::incomingFlags(false, false, none, any));
	EXPECT_FALSE(any);
	EXPECT_FALSE(none.contains(_T("noNotification")));
	EXPECT_FALSE(none.contains(_T("read")));

	MojObject muted;
	ASSERT_EQ(MojErrNone, MessageChanges::incomingFlags(true, false, muted, any));
	EXPECT_TRUE(any);
	EXPECT_TRUE(getBool(muted, "noNotification"));
	EXPECT_FALSE(muted.contains(_T("read"))) << "a muted chat's message still counts as unread";

	for (bool chatMuted : { false, true }) {
		MojObject old;
		any = false;
		ASSERT_EQ(MojErrNone, MessageChanges::incomingFlags(chatMuted, true, old, any));
		EXPECT_TRUE(any);
		EXPECT_TRUE(getBool(old, "noNotification"));
		EXPECT_TRUE(getBool(old, "read"));
	}
}
