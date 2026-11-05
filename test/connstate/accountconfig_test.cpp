// Tests for accountSettings(): login() gets the sign-in app's settings as the
// com.palm.config.libpurple:1 row onCreate stored, which OnEnabledHandler copies whole into the
// imloginstate record. The row below is the one sargo held for an IRC account (2026-11-05); read
// at the top level, its port, TLS and channels were never applied and the account went out on
// 6667 in plain text. See test/connstate/run.sh.

#include <gtest/gtest.h>

#include "AccountConfig.h"

namespace {

MojObject json(const char* text)
{
	MojObject obj;
	EXPECT_EQ(MojErrNone, obj.fromJson(text));
	return obj;
}

} // namespace

TEST(AccountSettings, TheStoredRowGivesItsSettings)
{
	MojObject row = json("{\"_id\":\"OUVRsGlWp5w\",\"_kind\":\"com.palm.config.libpurple:1\",\"_rev\":34499,"
	                     "\"accountId\":\"OUVRrwZ7qos\",\"config\":{\"channels\":\"#webos-ports\",\"port\":6697,"
	                     "\"realname\":\"\",\"sasl\":false,\"saslname\":\"\",\"ssl\":true}}");
	MojObject s = accountSettings(row);
	EXPECT_TRUE(s.contains(_T("port")));
	EXPECT_FALSE(s.contains(_T("accountId")));
	MojInt64 port = 0;
	EXPECT_TRUE(s.get(_T("port"), port));
	EXPECT_EQ(6697, port);
	bool ssl = false;
	EXPECT_TRUE(s.get(_T("ssl"), ssl));
	EXPECT_TRUE(ssl);
	MojString channels;
	bool found = false;
	EXPECT_EQ(MojErrNone, s.get(_T("channels"), channels, found));
	EXPECT_TRUE(found);
	EXPECT_STREQ("#webos-ports", channels.data());
}

TEST(AccountSettings, SettingsGivenAsTheyAreStayAsTheyAre)
{
	MojObject bare = json("{\"port\":6667,\"ssl\":false}");
	MojObject s = accountSettings(bare);
	EXPECT_TRUE(s.contains(_T("port")));
	EXPECT_TRUE(s.contains(_T("ssl")));
}

TEST(AccountSettings, NothingStoredIsNothing)
{
	EXPECT_FALSE(accountSettings(MojObject()).contains(_T("port")));
	EXPECT_FALSE(accountSettings(json("{}")).contains(_T("port")));
	// A "config" that is not an object is not the stored row's settings.
	MojObject odd = json("{\"config\":\"x\",\"port\":1}");
	EXPECT_TRUE(accountSettings(odd).contains(_T("port")));
}
