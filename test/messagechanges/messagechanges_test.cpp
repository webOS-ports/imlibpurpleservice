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
