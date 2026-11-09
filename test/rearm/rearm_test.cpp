/*
 * rearm_test.cpp
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport).
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free Software
 * Foundation.
 *
 * ActivityDefinitions.cpp: the activitymanager/create requests the transport sends when it starts
 * to arm its DB-watch activities again, made from the configurator's definition files.
 */

#include "ActivityDefinitions.h"

#include <gtest/gtest.h>

#include <dirent.h>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>

#ifndef ACTIVITY_DIR
#error "ACTIVITY_DIR: the tree's files/etc/palm/activities/com.palm.imlibpurple"
#endif

namespace {

std::string json(const MojObject& obj)
{
	MojString s;
	EXPECT_EQ(MojErrNone, obj.toJson(s));
	return s.data();
}

std::string str(const MojObject& obj, const MojChar* key)
{
	MojString s;
	bool found = false;
	EXPECT_EQ(MojErrNone, obj.get(key, s, found));
	return found ? std::string(s.data()) : std::string("<absent>");
}

MojObject sub(const MojObject& obj, const MojChar* key)
{
	MojObject out;
	EXPECT_TRUE(obj.get(key, out)) << key;
	return out;
}

bool flag(const MojObject& obj, const MojChar* key)
{
	bool b = false;
	return obj.get(key, b) && b;
}

/* A definition as the configurator's files have it, with what each test needs changed. */
std::string definition(const std::string& activityExtra = "",
		const std::string& callback = "palm://com.palm.imlibpurple/sendCommand",
		const std::string& outerExtra = "")
{
	return "{" + outerExtra + "\"start\":true,\"activity\":{\"name\":\"IM Service pending commands watch\","
		"\"type\":{\"foreground\":true,\"power\":true,\"explicit\":true},"
		"\"trigger\":{\"method\":\"palm://com.palm.db/watch\",\"key\":\"fired\","
		"\"params\":{\"subscribe\":true,\"query\":{\"from\":\"com.palm.imcommand.libpurple:1\"}}},"
		+ activityExtra +
		"\"callback\":{\"method\":\"" + callback + "\",\"params\":{}}}}";
}

std::string tempDir()
{
	const char* dir = getenv("TEST_TMPDIR");
	EXPECT_TRUE(dir != NULL && *dir) << "run through test/rearm/run.sh: TEST_TMPDIR, never /tmp";
	return dir ? dir : ".";
}

void writeFile(const std::string& path, const std::string& text)
{
	std::ofstream out(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
	out << text;
	ASSERT_TRUE(out.good()) << path;
}

} // namespace

/* The files the configurator reads are the ones the transport re-creates: all of them, no other. */
TEST(ActivityDefinitions, ListsEveryFileTheConfiguratorReads)
{
	std::set<std::string> listed;
	for (size_t i = 0; i < ActivityDefinitions::FILE_COUNT; ++i)
		listed.insert(ActivityDefinitions::FILES[i]);
	EXPECT_EQ(ActivityDefinitions::FILE_COUNT, listed.size()) << "a file listed twice";

	std::set<std::string> shipped;
	DIR* d = opendir(ACTIVITY_DIR);
	ASSERT_TRUE(d != NULL) << ACTIVITY_DIR;
	for (struct dirent* e = readdir(d); e != NULL; e = readdir(d)) {
		std::string n(e->d_name);
		if (n != "." && n != "..")
			shipped.insert(n);
	}
	closedir(d);
	EXPECT_EQ(shipped, listed);
	EXPECT_STREQ("/etc/palm/activities/com.palm.imlibpurple", ActivityDefinitions::DIR);
}

/* Each shipped file becomes its create request: the same activity, started, replacing the old. */
TEST(ActivityDefinitions, ShippedFilesBecomeReplacingCreates)
{
	struct Expect { const char* file; const char* name; const char* method; const char* from; };
	const Expect expected[] = {
		{ "com.palm.imcommand.libpurple", "IM Service pending commands watch",
		  "palm://com.palm.imlibpurple/sendCommand", "com.palm.imcommand.libpurple:1" },
		{ "com.palm.immessage.libpurple", "IM Service pending messages watch",
		  "palm://com.palm.imlibpurple/sendIM", "com.palm.immessage.libpurple:1" },
		{ "com.palm.imloginstate.libpurple", "Libpurple loginstate",
		  "palm://com.palm.imlibpurple/loginStateChanged", "com.palm.imloginstate.libpurple:1" },
	};
	for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
		const Expect& e = expected[i];
		MojObject payload;
		std::string name;
		ASSERT_TRUE(ActivityDefinitions::load(ACTIVITY_DIR, e.file, payload, name)) << e.file;
		EXPECT_EQ(e.name, name);
		EXPECT_TRUE(flag(payload, _T("start"))) << e.file;
		EXPECT_TRUE(flag(payload, _T("replace"))) << e.file;
		EXPECT_FALSE(payload.contains(_T("subscribe"))) << e.file;

		MojObject activity = sub(payload, _T("activity"));
		EXPECT_EQ(e.name, str(activity, _T("name")));
		EXPECT_FALSE(activity.contains(_T("creator"))) << e.file;
		EXPECT_EQ(e.method, str(sub(activity, _T("callback")), _T("method")));

		// The watch itself is the file's, untouched: subscribe:true is what makes a new watch
		// fire at once on what is already pending.
		MojObject trigger = sub(activity, _T("trigger"));
		EXPECT_EQ("palm://com.palm.db/watch", str(trigger, _T("method")));
		EXPECT_EQ("fired", str(trigger, _T("key")));
		MojObject params = sub(trigger, _T("params"));
		EXPECT_TRUE(flag(params, _T("subscribe"))) << e.file;
		EXPECT_EQ(e.from, str(sub(params, _T("query")), _T("from")));

		MojObject type = sub(activity, _T("type"));
		EXPECT_TRUE(flag(type, _T("explicit"))) << e.file;
		EXPECT_TRUE(flag(sub(activity, _T("requirements")), _T("internet"))) << e.file;
	}
}

TEST(ActivityDefinitions, StartsAndReplacesWhateverTheFileSays)
{
	MojObject payload;
	std::string name;
	std::string text = definition();
	text.replace(text.find("\"start\":true"), 12, "\"start\":false,\"replace\":false");
	ASSERT_TRUE(ActivityDefinitions::toCreatePayload(text.c_str(), payload, name));
	EXPECT_TRUE(flag(payload, _T("start")));
	EXPECT_TRUE(flag(payload, _T("replace")));
}

/* The activity manager takes a creator only from the configurator; the transport is not the
 * activity's parent. */
TEST(ActivityDefinitions, LeavesOutWhatTheTransportMayNotSend)
{
	MojObject payload;
	std::string name;
	std::string text = definition("\"creator\":{\"serviceId\":\"com.palm.imlibpurple\"},",
			"palm://com.palm.imlibpurple/sendCommand",
			"\"subscribe\":true,\"detailedEvents\":true,\"firstUseSafe\":true,");
	ASSERT_TRUE(ActivityDefinitions::toCreatePayload(text.c_str(), payload, name));
	EXPECT_FALSE(sub(payload, _T("activity")).contains(_T("creator"))) << json(payload);
	EXPECT_FALSE(payload.contains(_T("subscribe"))) << json(payload);
	EXPECT_FALSE(payload.contains(_T("detailedEvents"))) << json(payload);
	EXPECT_FALSE(payload.contains(_T("firstUseSafe"))) << json(payload);
	EXPECT_EQ("IM Service pending commands watch", name);
}

TEST(ActivityDefinitions, TakesLunaCallbacksToo)
{
	MojObject payload;
	std::string name;
	std::string text = definition("", "luna://com.palm.imlibpurple/sendIM");
	EXPECT_TRUE(ActivityDefinitions::toCreatePayload(text.c_str(), payload, name));
}

/* Only our own watches: anything else in the file is refused, and the outputs keep their values. */
TEST(ActivityDefinitions, RefusesWhatIsNotOneOfOurWatches)
{
	const std::string bad[] = {
		"",
		"not json",
		"[1,2]",
		"\"text\"",
		"{\"start\":true}",
		"{\"start\":true,\"activity\":\"x\"}",
		"{\"start\":true,\"activity\":{\"trigger\":{},\"callback\":{\"method\":\"palm://com.palm.imlibpurple/sendIM\"}}}",
		"{\"activity\":{\"name\":\"\",\"trigger\":{},\"callback\":{\"method\":\"palm://com.palm.imlibpurple/sendIM\"}}}",
		"{\"activity\":{\"name\":\"n\",\"callback\":{\"method\":\"palm://com.palm.imlibpurple/sendIM\"}}}",
		"{\"activity\":{\"name\":\"n\",\"trigger\":true,\"callback\":{\"method\":\"palm://com.palm.imlibpurple/sendIM\"}}}",
		"{\"activity\":{\"name\":\"n\",\"trigger\":{}}}",
		"{\"activity\":{\"name\":\"n\",\"trigger\":{},\"callback\":{}}}",
		"{\"activity\":{\"name\":\"n\",\"trigger\":{},\"callback\":\"palm://com.palm.imlibpurple/sendIM\"}}",
		definition("", "palm://com.palm.other/sendCommand"),
		definition("", "palm://com.palm.imlibpurple.other/sendCommand"),
		definition("", "palm://com.palm.imlibpurple"),
		definition("", "com.palm.imlibpurple/sendCommand"),
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
		MojObject payload;
		ASSERT_EQ(MojErrNone, payload.putString(_T("kept"), _T("yes")));
		std::string name = "kept";
		EXPECT_FALSE(ActivityDefinitions::toCreatePayload(bad[i].c_str(), payload, name)) << bad[i];
		EXPECT_EQ("{\"kept\":\"yes\"}", json(payload)) << bad[i];
		EXPECT_EQ("kept", name) << bad[i];
	}
	MojObject payload;
	std::string name;
	EXPECT_FALSE(ActivityDefinitions::toCreatePayload(NULL, payload, name));
}

TEST(ActivityDefinitions, ReadsAFileUpToItsLimit)
{
	const std::string dir = tempDir();
	MojObject payload;
	std::string name;

	EXPECT_FALSE(ActivityDefinitions::load(dir, "no-such-definition", payload, name));
	EXPECT_FALSE(ActivityDefinitions::load(dir, NULL, payload, name));

	// Padding inside the JSON keeps it valid, so only the size can refuse it.
	std::string text = definition();
	std::string fits = text + std::string(ActivityDefinitions::MAX_SIZE - text.size(), ' ');
	writeFile(dir + "/fits", fits);
	EXPECT_TRUE(ActivityDefinitions::load(dir, "fits", payload, name));
	EXPECT_EQ("IM Service pending commands watch", name);

	writeFile(dir + "/too-long", fits + " ");
	name = "kept";
	EXPECT_FALSE(ActivityDefinitions::load(dir, "too-long", payload, name));
	EXPECT_EQ("kept", name);
}
