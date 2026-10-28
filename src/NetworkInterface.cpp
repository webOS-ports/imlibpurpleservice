/*
 * NetworkInterface.cpp
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport). See NetworkInterface.h.
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free Software
 * Foundation.
 */

#include "NetworkInterface.h"
#include "IMServiceApp.h"

/*
 * One interface ("wired", "wifi" or "wan") of a connectionmanager status: true when it is connected
 * and has an address, which then goes to ipAddress. Wired and WiFi carry a "state" and an
 * "ipAddress". LuneOS's WAN object has neither: it says "connected": true/false and lists each data
 * context in "connectedContexts", with the address under ipv4.address, so that shape is read too.
 * A missing interface is simply not connected. ipAddress is left alone otherwise: a lost interface
 * keeps its old address, so the accounts that were using it can be found and signed off.
 */
bool readNetworkInterface(const MojObject& internet, const MojChar* name, MojString& ipAddress)
{
	MojObject iface;
	if (!internet.get(name, iface))
		return false;

	bool connected = false;
	MojString state;
	bool found = false;
	if (iface.get(_T("state"), state, found) == MojErrNone && found)
		connected = (state.compare("connected") == 0);
	else
		iface.get(_T("connected"), connected);
	if (!connected)
		return false;

	MojString address;
	found = false;
	if (iface.get(_T("ipAddress"), address, found) != MojErrNone || !found)
	{
		address.clear();
		MojObject contexts;
		if (iface.get(_T("connectedContexts"), contexts))
		{
			for (MojObject::ConstArrayIterator it = contexts.arrayBegin(); it != contexts.arrayEnd() && address.empty(); ++it)
			{
				MojObject ipv4;
				bool contextConnected = true;
				it->get(_T("connected"), contextConnected);
				if (contextConnected && it->get(_T("ipv4"), ipv4))
				{
					found = false;
					if (ipv4.get(_T("address"), address, found) != MojErrNone || !found)
						address.clear();
				}
			}
		}
	}
	if (address.empty())
	{
		// It may claim to be connected, but there's no interface IP address
		MojLogError(IMServiceApp::s_log, _T("Marking %s as disconnected because ipAddress is missing"), name);
		return false;
	}
	ipAddress = address;
	MojLogInfo(IMServiceApp::s_log, _T("ConnectionStateHandler::connectionManagerResult %s ipAddress=%s"), name, ipAddress.data());
	return true;
}
