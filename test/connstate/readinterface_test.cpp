// Tests for readNetworkInterface(): which interfaces of a com.palm.connectionmanager status count
// as connected, and which address the transport signs in from. The payloads are the shapes
// connectionmanager sends on LuneOS (webos-connman-adapter) and the ones legacy webOS sent.
// See test/connstate/run.sh.

#include <gtest/gtest.h>

#include "NetworkInterface.h"
#include "IMServiceApp.h"

MojLogger IMServiceApp::s_log(_T("imlibpurple.test"));

namespace {

MojObject status(const char* json)
{
	MojObject obj;
	EXPECT_EQ(MojErrNone, obj.fromJson(json));
	return obj;
}

// What a qemux86-64 VM on ethernet reports: wired up, WiFi down, no modem.
const char* kWiredOnly =
	"{\"isInternetConnectionAvailable\":true,"
	"\"wifi\":{\"tetheringEnabled\":false,\"state\":\"disconnected\"},"
	"\"wan\":{\"connectedContexts\":[],\"connected\":false},"
	"\"wired\":{\"interfaceName\":\"eth0\",\"ipAddress\":\"10.0.2.15\",\"state\":\"connected\",\"onInternet\":\"yes\"}}";

} // namespace

TEST(ReadNetworkInterface, WiredCountsAsConnected)
{
	MojObject s = status(kWiredOnly);
	MojString ip;
	EXPECT_TRUE(readNetworkInterface(s, _T("wired"), ip));
	EXPECT_STREQ("10.0.2.15", ip.data());
}

TEST(ReadNetworkInterface, DisconnectedWifiAndModemlessWanAreNot)
{
	MojObject s = status(kWiredOnly);
	MojString ip;
	EXPECT_FALSE(readNetworkInterface(s, _T("wifi"), ip));
	// LuneOS's wan object has no "state", only "connected": false.
	EXPECT_FALSE(readNetworkInterface(s, _T("wan"), ip));
	EXPECT_TRUE(ip.empty());
}

TEST(ReadNetworkInterface, AnAddressAloneDoesNotMakeItConnected)
{
	// connectionmanager can still list the last address of an interface that is going down.
	MojObject s = status(
		"{\"wifi\":{\"state\":\"disconnected\",\"ipAddress\":\"192.168.1.20\"},"
		"\"wan\":{\"connected\":false,\"connectedContexts\":[{\"connected\":true,\"ipv4\":{\"address\":\"100.64.1.2\"}}]}}");
	MojString ip;
	EXPECT_FALSE(readNetworkInterface(s, _T("wifi"), ip));
	EXPECT_FALSE(readNetworkInterface(s, _T("wan"), ip));
	EXPECT_TRUE(ip.empty());
}

TEST(ReadNetworkInterface, MissingInterfaceIsNotConnected)
{
	MojObject s = status("{\"isInternetConnectionAvailable\":true}");
	MojString ip;
	EXPECT_FALSE(readNetworkInterface(s, _T("wired"), ip));
}

TEST(ReadNetworkInterface, LuneOSWanTakesTheAddressOfAConnectedContext)
{
	MojObject s = status(
		"{\"wan\":{\"connected\":true,\"onInternet\":true,\"connectedContexts\":["
		"{\"name\":\"ims\",\"connected\":false,\"ipv4\":{\"address\":\"10.9.9.9\"}},"
		"{\"name\":\"internet\",\"connected\":true,\"onInternet\":true,\"ipv4\":{\"address\":\"100.64.1.2\"}}]}}");
	MojString ip;
	EXPECT_TRUE(readNetworkInterface(s, _T("wan"), ip));
	EXPECT_STREQ("100.64.1.2", ip.data());
}

TEST(ReadNetworkInterface, LegacyWanWithStateAndAddress)
{
	MojObject s = status(
		"{\"wan\":{\"interfaceName\":\"wwan0\",\"ipAddress\":\"10.13.230.47\",\"network\":\"hsdpa\",\"state\":\"connected\"}}");
	MojString ip;
	EXPECT_TRUE(readNetworkInterface(s, _T("wan"), ip));
	EXPECT_STREQ("10.13.230.47", ip.data());
}

TEST(ReadNetworkInterface, ConnectedWithoutAnAddressIsNotConnected)
{
	MojObject s = status(
		"{\"wifi\":{\"state\":\"connected\"},"
		"\"wan\":{\"connected\":true,\"connectedContexts\":[{\"connected\":true}]}}");
	MojString ip;
	EXPECT_FALSE(readNetworkInterface(s, _T("wifi"), ip));
	EXPECT_FALSE(readNetworkInterface(s, _T("wan"), ip));
}

TEST(ReadNetworkInterface, ALostInterfaceKeepsItsLastAddress)
{
	// The accounts still bound to it are found by that address and signed off.
	MojString ip;
	MojObject up = status("{\"wifi\":{\"state\":\"connected\",\"ipAddress\":\"192.168.1.20\"}}");
	ASSERT_TRUE(readNetworkInterface(up, _T("wifi"), ip));
	MojObject down = status("{\"wifi\":{\"state\":\"disconnected\"}}");
	EXPECT_FALSE(readNetworkInterface(down, _T("wifi"), ip));
	EXPECT_STREQ("192.168.1.20", ip.data());
}
