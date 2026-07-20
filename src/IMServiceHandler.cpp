/*
 * IMServiceHandler.cpp
 *
 * Copyright 2010 Palm, Inc. All rights reserved.
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free
 * Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License,
 * Version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-
 * 1301, USA
 *
 * IMLibpurpleservice uses libpurple.so to implement a fully functional IM
 * Transport service for use on a mobile device.
 *
 * IMServiceHandler class is the top level signal handler for the application
 */


#include <set>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "IMServiceHandler.h"
#include "LibpurpleAdapter.h"
#include "IncomingIMHandler.h"
#include "IMMessage.h"
#include "OutgoingIMCommandHandler.h"
#include "OnEnabledHandler.h"
#include "IMServiceApp.h"
#include "BuddyStatusHandler.h"
#include "IMDefines.h"

#define IMVersionString  "IMLibpurpleService 7-13 12:30pm starting...."

const IMServiceHandler::Method IMServiceHandler::s_methods[] = {
	{_T("loginForTesting"), (Callback) &IMServiceHandler::loginForTesting},
	{_T("onEnabled"), (Callback) &IMServiceHandler::onEnabled},
    {_T("onCreate"), (Callback) &IMServiceHandler::onCreate},
    {_T("onDelete"), (Callback) &IMServiceHandler::onDelete},
	{_T("loginStateChanged"), (Callback) &IMServiceHandler::handleLoginStateChange},
	{_T("sendIM"), (Callback) &IMServiceHandler::IMSend}, // callback for activity manager
	{_T("sendCommand"), (Callback) &IMServiceHandler::IMSendCmd}, // callback for activity manager
	{_T("startQRLogin"), (Callback) &IMServiceHandler::startQRLogin},
	{_T("getAuthChallenge"), (Callback) &IMServiceHandler::getAuthChallenge},
	{_T("submitAuthInput"), (Callback) &IMServiceHandler::submitAuthInput},
	{_T("openChannel"), (Callback) &IMServiceHandler::openChannel},
	{NULL, NULL} };

IMServiceHandler::IMServiceHandler(MojService* service)
: m_service(service),
  m_dbClient(service),
  m_tempdbClient(service, MojDbServiceDefs::TempServiceName),
  m_deleteConfigSlot(this, &IMServiceHandler::deleteConfigResult),
  m_putConfigSlot(this, &IMServiceHandler::putConfigResult),
  m_deleteImLoginStateSlot(this, &IMServiceHandler::deleteImLoginStateResult),
  m_deleteImMessagesSlot(this, &IMServiceHandler::deleteImMessagesResult),
  m_deleteImCommandsSlot(this, &IMServiceHandler::deleteImCommandsResult),
  m_deleteContactsSlot(this, &IMServiceHandler::deleteContactsResult),
  m_deleteImBuddyStatusSlot(this, &IMServiceHandler::deleteImBuddyStatusResult),
  m_syncFindServersSlot(this, &IMServiceHandler::syncFindServersResult),
  m_syncPutServersSlot(this, &IMServiceHandler::syncPutServersResult),
  m_syncFindChannelsSlot(this, &IMServiceHandler::syncFindChannelsResult),
  m_syncPutChannelsSlot(this, &IMServiceHandler::syncPutChannelsResult),
  m_syncMergeServersSlot(this, &IMServiceHandler::syncMergeServersResult),
  m_syncMergeChannelsSlot(this, &IMServiceHandler::syncMergeChannelsResult),
  m_syncDelGoneServersSlot(this, &IMServiceHandler::syncDelGoneServersResult),
  m_syncDelGoneChannelsSlot(this, &IMServiceHandler::syncDelGoneChannelsResult),
  m_connectionState(service)
{
	MojLogTrace(IMServiceApp::s_log);
	m_loginState = NULL;
	m_activeProcesses = 0;
	m_shutdownCallbackId = 0;
	m_displayController = NULL;
	m_authChannel = NULL;
	m_syncInFlight = false;
}

IMServiceHandler::~IMServiceHandler()
{
	MojLogTrace(IMServiceApp::s_log);
	delete m_loginState;
	delete m_displayController;
	delete m_authChannel;
}


/*
 * Initialize the Service
 *
 */
MojErr IMServiceHandler::init()
{
	MojLogTrace(IMServiceApp::s_log);
	MojLogInfo(IMServiceApp::s_log, IMVersionString);

	MojErr err = addMethods(s_methods);
	MojErrCheck(err);

	// set up the incoming message callback pointer
	LibpurpleAdapter::assignIMServiceHandler(this);

	// create the display controller to keep track of screen changes
	m_displayController = new DisplayController(m_service);
	m_displayController->createSubscription();

	// create the interactive-login (QR) challenge channel and hand it to the adapter
	m_authChannel = new AuthChannel(m_service);
	LibpurpleAdapter::assignAuthChannel(m_authChannel);

	// consume a one-shot purge sentinel, if present (shell-driven service reset without luna-send)
	checkPurgeSentinel();

	return MojErrNone;
}

MojErr IMServiceHandler::onCreate(MojServiceMessage* serviceMsg, const MojObject payload)
{
    MojString accountId;
    MojErr err = payload.getRequired("accountId", accountId);
    if (err != MojErrNone || accountId.empty()) {
		MojLogError(IMServiceApp::s_log, _T("IMServiceHandler::onCreate accountId empty or error %d"), err);
        serviceMsg->replyError(err);
		return err;
	}

#ifndef IMLIBPURPLE_LEGACY_DB8
    MojObject config;
    payload.get("config", config);

    MojObject res;
    res.putString("accountId", accountId);
    res.put("config", config);
    res.putString("_kind", "com.palm.config.libpurple:1");

    // Write config to db:
    m_dbClient.put(m_putConfigSlot, res);
#endif // !IMLIBPURPLE_LEGACY_DB8

    serviceMsg->replySuccess();
    return MojErrNone;
}

MojErr IMServiceHandler::onDelete(MojServiceMessage* serviceMsg, const MojObject payload)
{
    MojString accountId;
    bool found = false;
    payload.get("accountId", accountId, found);
    if (!found || accountId.empty()) {
        MojErr err = MojErrInvalidArg;
		MojLogError(IMServiceApp::s_log, _T("IMServiceHandler::onDelete accountId empty or error %d"), err);
        serviceMsg->replyError(err);
		return err;
	}

    /* webOS Teams port: remove the persisted PurpleAccount (accounts.xml + buddy list
     * + stored OAuth refresh_token) tagged with this webOS accountId, so re-adding the
     * account starts from a genuinely clean state. Grab the username + serviceName first
     * so we can purge this account's db8 chat data below. */
    std::string delUsername, delServiceName;
    LibpurpleAdapter::deleteAccountByWebosId(accountId.data(), &delUsername, &delServiceName);

    /* Purge the account's db8 chat records so the Messaging app doesn't keep showing old
     * conversations/contacts after the account is deleted. The disable path
     * (OnEnabledHandler::accountDisabled) does this on toggle-off, but on a real delete
     * the account is already gone from the account manager, so its username/serviceName
     * can't be resolved there and the purge never runs. Do it explicitly here. */
    purgeAccountData(accountId.data(),
                     delUsername.empty() ? NULL : delUsername.c_str(),
                     delServiceName.empty() ? NULL : delServiceName.c_str());

#ifndef IMLIBPURPLE_LEGACY_DB8
    MojDbQuery query;
    query.from("com.palm.config.libpurple:1");
    query.where("accountId", MojDbQuery::OpEq, accountId);

    m_dbClient.del(m_deleteConfigSlot, query);
#endif // !IMLIBPURPLE_LEGACY_DB8

    serviceMsg->replySuccess();
    return MojErrNone;
}

/*
 * Purge all db8 records belonging to a deleted account. Mirrors
 * OnEnabledHandler::accountDisabled():
 *   - imloginstate / contact / imbuddystatus  keyed by accountId
 *   - immessage / imcommand                    keyed by username (+serviceName)
 * The immessage/imcommand records carry username + serviceName (not accountId), so those
 * are only purged when we managed to resolve the username from the PurpleAccount. The
 * ChatThreader service removes the now-empty chats.
 */
MojErr IMServiceHandler::purgeAccountData(const char* accountId, const char* username, const char* serviceName)
{
	MojLogInfo(IMServiceApp::s_log, _T("purgeAccountData: accountId=%s username=%s serviceName=%s"),
	           accountId ? accountId : "", username ? username : "", serviceName ? serviceName : "");

	MojErr err;

	// MojDbQuery::where takes a MojObject; MojObject's const char* ctor is private, so
	// wrap raw strings in MojString (which converts to MojObject implicitly, as elsewhere).
	MojString accountIdStr;
	err = accountIdStr.assign(accountId ? accountId : "");
	MojErrCheck(err);

	// imloginstate - keyed by accountId
	MojDbQuery queryLoginState;
	queryLoginState.from(IM_LOGINSTATE_KIND);
	queryLoginState.where(_T("accountId"), MojDbQuery::OpEq, accountIdStr);
	err = m_dbClient.del(m_deleteImLoginStateSlot, queryLoginState);
	MojErrCheck(err);

	// contact - keyed by accountId
	MojDbQuery queryContact;
	queryContact.from(IM_CONTACT_KIND);
	queryContact.where(_T("accountId"), MojDbQuery::OpEq, accountIdStr);
	err = m_dbClient.del(m_deleteContactsSlot, queryContact);
	MojErrCheck(err);

	// imbuddystatus - keyed by accountId, lives in tempdb
	MojDbQuery queryBuddyStatus;
	queryBuddyStatus.from(IM_BUDDYSTATUS_KIND);
	queryBuddyStatus.where(_T("accountId"), MojDbQuery::OpEq, accountIdStr);
	err = m_tempdbClient.del(m_deleteImBuddyStatusSlot, queryBuddyStatus);
	MojErrCheck(err);

	// immessage + imcommand are keyed by username (the account owner), not accountId.
	if (username != NULL && *username != '\0')
	{
		MojString usernameStr;
		err = usernameStr.assign(username);
		MojErrCheck(err);
		bool haveService = (serviceName != NULL && *serviceName != '\0');
		MojString serviceNameStr;
		if (haveService)
		{
			err = serviceNameStr.assign(serviceName);
			MojErrCheck(err);
		}

		MojDbQuery queryMessage;
		queryMessage.from(IM_IMMESSAGE_KIND);
		if (haveService)
			queryMessage.where(_T("serviceName"), MojDbQuery::OpEq, serviceNameStr);
		queryMessage.where(_T("username"), MojDbQuery::OpEq, usernameStr);
		err = m_dbClient.del(m_deleteImMessagesSlot, queryMessage);
		MojErrCheck(err);

		MojDbQuery queryCommand;
		queryCommand.from(IM_IMCOMMAND_KIND);
		if (haveService)
			queryCommand.where(_T("serviceName"), MojDbQuery::OpEq, serviceNameStr);
		queryCommand.where(_T("fromUsername"), MojDbQuery::OpEq, usernameStr);
		err = m_dbClient.del(m_deleteImCommandsSlot, queryCommand);
		MojErrCheck(err);
	}
	else
	{
		MojLogError(IMServiceApp::s_log, _T("purgeAccountData: no username resolved - immessage/imcommand not purged for accountId %s"), accountId ? accountId : "");
	}

	return MojErrNone;
}

// Sentinel a shell (novacom) can drop to purge a whole service's leftover data at next start --
// one serviceName per line ("type_whatsapp"), '#' comments and blanks ignored. Consumed (deleted)
// after processing so it fires exactly once. Lives on /media/internal so it survives a reboot but
// is trivially writable without luna-send.
#define PURGE_SENTINEL_PATH "/media/internal/.im-purge-services"

MojErr IMServiceHandler::purgeServiceData(const char* serviceName)
{
	if (serviceName == NULL || *serviceName == '\0')
		return MojErrNone;
	MojLogInfo(IMServiceApp::s_log, _T("purgeServiceData: purging all db8 records for serviceName=%s"), serviceName);

	MojErr err;
	MojString svc;
	err = svc.assign(serviceName);
	MojErrCheck(err);

	// contacts - com.palm.contact.libpurple. No accountId available for an orphan, but the kind is
	// queryable by ims.type (leading prop of the byUsernameAndServiceName index) and a buddy's IM
	// entry type IS the serviceName.
	MojDbQuery queryContact;
	queryContact.from(IM_CONTACT_KIND);
	err = queryContact.where(_T("ims.type"), MojDbQuery::OpEq, svc);
	MojErrCheck(err);
	err = m_dbClient.del(m_deleteContactsSlot, queryContact);
	MojErrCheck(err);

	// immessage - serviceAndUsername index, leading prop serviceName.
	MojDbQuery queryMessage;
	queryMessage.from(IM_IMMESSAGE_KIND);
	err = queryMessage.where(_T("serviceName"), MojDbQuery::OpEq, svc);
	MojErrCheck(err);
	err = m_dbClient.del(m_deleteImMessagesSlot, queryMessage);
	MojErrCheck(err);

	// imcommand - pendingBuddyInvite index, leading prop serviceName.
	MojDbQuery queryCommand;
	queryCommand.from(IM_IMCOMMAND_KIND);
	err = queryCommand.where(_T("serviceName"), MojDbQuery::OpEq, svc);
	MojErrCheck(err);
	err = m_dbClient.del(m_deleteImCommandsSlot, queryCommand);
	MojErrCheck(err);

	// imbuddystatus - tempdb (byUserName index, leading prop serviceName). tempdb clears on reboot
	// anyway, but purge here too so an in-session reset takes effect immediately.
	MojDbQuery queryBuddyStatus;
	queryBuddyStatus.from(IM_BUDDYSTATUS_KIND);
	err = queryBuddyStatus.where(_T("serviceName"), MojDbQuery::OpEq, svc);
	MojErrCheck(err);
	err = m_tempdbClient.del(m_deleteImBuddyStatusSlot, queryBuddyStatus);
	MojErrCheck(err);

	return MojErrNone;
}

void IMServiceHandler::checkPurgeSentinel()
{
	FILE* f = fopen(PURGE_SENTINEL_PATH, "r");
	if (f == NULL)
		return;
	MojLogInfo(IMServiceApp::s_log, _T("checkPurgeSentinel: %s present -- purging listed services"), PURGE_SENTINEL_PATH);
	char line[256];
	while (fgets(line, sizeof(line), f) != NULL)
	{
		char* s = line;
		while (*s == ' ' || *s == '\t')
			++s;
		size_t n = strlen(s);
		while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t'))
			s[--n] = '\0';
		if (*s == '\0' || *s == '#')
			continue;
		purgeServiceData(s);
	}
	fclose(f);
	// One-shot: remove the sentinel so it does not purge again on the next start.
	if (unlink(PURGE_SENTINEL_PATH) != 0)
		MojLogError(IMServiceApp::s_log, _T("checkPurgeSentinel: failed to remove %s"), PURGE_SENTINEL_PATH);
}

MojErr IMServiceHandler::deleteImLoginStateResult(MojObject& payload, MojErr err)
{
	if (err != MojErrNone)
		MojLogError(IMServiceApp::s_log, _T("purgeAccountData: del(imloginstate) failed: %d"), err);
	return MojErrNone;
}

MojErr IMServiceHandler::deleteImMessagesResult(MojObject& payload, MojErr err)
{
	if (err != MojErrNone)
		MojLogError(IMServiceApp::s_log, _T("purgeAccountData: del(immessage) failed: %d"), err);
	return MojErrNone;
}

MojErr IMServiceHandler::deleteImCommandsResult(MojObject& payload, MojErr err)
{
	if (err != MojErrNone)
		MojLogError(IMServiceApp::s_log, _T("purgeAccountData: del(imcommand) failed: %d"), err);
	return MojErrNone;
}

MojErr IMServiceHandler::deleteContactsResult(MojObject& payload, MojErr err)
{
	if (err != MojErrNone)
		MojLogError(IMServiceApp::s_log, _T("purgeAccountData: del(contact) failed: %d"), err);
	return MojErrNone;
}

MojErr IMServiceHandler::deleteImBuddyStatusResult(MojObject& payload, MojErr err)
{
	if (err != MojErrNone)
		MojLogError(IMServiceApp::s_log, _T("purgeAccountData: del(imbuddystatus) failed: %d"), err);
	return MojErrNone;
}

/*
 * webOS Servers/Rooms M3: upsert the enumerated guild->channel roster (from
 * LibpurpleAdapter::enumerateServersChannels) into db8. db8 has no native upsert, so we clear this
 * account's imserver/imchannel and recreate them. imchannel.serverId must reference the imserver's
 * db8-assigned _id, so the writes chain: del imchannel -> del imserver -> put imserver (capture the
 * assigned ids) -> put imchannel. m_syncServers holds the pending roster across the async hops (one
 * account at a time; a fresh sync overwrites any in flight - the more recent roster wins).
 */
bool IMServiceHandler::syncServersChannels(const char* serviceName, const char* username, MojObject& serversObj)
{
	MojLogInfo(IMServiceApp::s_log, _T("syncServersChannels: serviceName=%s servers=%d"),
		serviceName ? serviceName : "", (int)serversObj.size());

	if (serviceName == NULL || *serviceName == '\0')
		return false;

	// SAFETY: never wipe on an empty roster. An empty enumeration (out-of-sync guild trees) must NOT
	// delete the account's existing (message-driven) server/channel records. Only a non-empty roster
	// drives the delete+recreate. (enumerateServersChannels already guards this; belt and suspenders.)
	if (serversObj.size() == 0)
	{
		MojLogInfo(IMServiceApp::s_log, _T("syncServersChannels: empty roster; leaving existing records untouched"));
		return true;
	}

	// SERIALIZE: the delete->recreate chain below runs across async db8 callbacks on shared member
	// state (m_syncServiceName / m_syncServers). Concurrent enumerations (Discord + Telegram both
	// firing on their post-sync debounce) would clobber that state mid-chain and delete the wrong
	// account's records in a never-converging churn. If a sync is already in flight, skip this one -
	// the blist-changed trigger / next login re-enumerates, and the roster-signature guard means an
	// unchanged roster won't re-sync anyway.
	if (m_syncInFlight)
	{
		MojLogInfo(IMServiceApp::s_log, _T("syncServersChannels: a sync is already in flight; skipping %s"), serviceName);
		return true;
	}
	m_syncInFlight = true;

	MojErr err = m_syncServiceName.assign(serviceName);
	if (err != MojErrNone)
	{
		m_syncInFlight = false;
		return false;
	}
	m_syncServers = serversObj;

	err = syncServersChannelsStart();
	if (err != MojErrNone)
	{
		m_syncInFlight = false;
		MojLogError(IMServiceApp::s_log, _T("syncServersChannels: start failed: %d"), err);
		return false;
	}
	return true;
}

// Stage 1: find this account's existing imserver rows so we can diff (not wipe) against the roster.
MojErr IMServiceHandler::syncServersChannelsStart()
{
	m_syncServerIdByRemote.clear();
	m_syncNewServerRemotes.clear();

	MojDbQuery query;
	MojErr err = query.from(_T("com.palm.imserver:1"));
	MojErrCheck(err);
	err = query.where(_T("serviceName"), MojDbQuery::OpEq, m_syncServiceName);
	MojErrCheck(err);
	err = m_dbClient.find(m_syncFindServersSlot, query);
	MojErrCheck(err);
	return MojErrNone;
}

// Stage 2: diff servers by remoteId. MERGE existing (keep _id), PUT new (need their assigned _ids to
// stamp onto channels), DEL those that vanished from the roster.
MojErr IMServiceHandler::syncFindServersResult(MojObject& payload, MojErr err)
{
	if (err != MojErrNone)
	{
		MojLogError(IMServiceApp::s_log, _T("syncServersChannels: find(imserver) failed: %d"), err);
		m_syncInFlight = false;
		return MojErrNone;
	}

	// existing remoteId -> _id
	std::map<std::string, MojString> existing;
	MojObject results;
	payload.get(_T("results"), results);
	for (MojObject::ConstArrayIterator it = results.arrayBegin(); it != results.arrayEnd(); ++it)
	{
		MojString rid, id;
		bool f = false;
		it->get(_T("remoteId"), rid, f);
		it->get(_T("_id"), id, f);
		if (!rid.empty())
			existing[std::string(rid.data())] = id;
	}

	MojObject::ObjectVec mergeServers, putServers;
	MojObject delGoneServers;   // array of _id values
	std::set<std::string> incoming;

	for (MojObject::ConstArrayIterator sIt = m_syncServers.arrayBegin(); sIt != m_syncServers.arrayEnd(); ++sIt)
	{
		MojString remoteId, name;
		bool f = false;
		sIt->get(_T("remoteId"), remoteId, f);
		sIt->get(_T("name"), name, f);
		if (remoteId.empty())
			continue;
		incoming.insert(std::string(remoteId.data()));

		std::map<std::string, MojString>::iterator e = existing.find(std::string(remoteId.data()));
		if (e != existing.end())
		{
			// existing -> merge in place (keep _id); update the display fields only
			MojObject m;
			MojErrCheck(m.putString(_T("_kind"), _T("com.palm.imserver:1")));
			MojErrCheck(m.put(_T("_id"), e->second));
			MojErrCheck(m.putString(_T("displayName"), name));
			MojErrCheck(m.putString(_T("name"), name));
			MojErrCheck(mergeServers.push(m));
			m_syncServerIdByRemote[std::string(remoteId.data())] = e->second; // _id known now
		}
		else
		{
			// new -> put (assigned _id comes back in syncPutServersResult, matched by this order)
			MojObject s;
			MojErrCheck(s.putString(_T("_kind"), _T("com.palm.imserver:1")));
			MojErrCheck(s.putString(_T("serviceName"), m_syncServiceName));
			MojErrCheck(s.putString(_T("remoteId"), remoteId));
			MojErrCheck(s.putString(_T("displayName"), name));
			MojErrCheck(s.putString(_T("name"), name));
			MojErrCheck(putServers.push(s));
			m_syncNewServerRemotes.push_back(std::string(remoteId.data()));
		}
	}

	// ADDITIVE/MERGE-ONLY: do NOT delete servers that are absent from this roster snapshot. The async
	// prpls (tdlib/whatsmeow) populate the blist in waves after login, so any single enumeration is a
	// PARTIAL view - a server missing right now is almost always still-loading, not "left". Deleting it
	// cascaded into deleting its channels, which orphaned their chatthreads and spawned duplicate threads
	// when the server reloaded. Left-behind servers are handled by an explicit cleanup instead.
	(void)incoming; (void)delGoneServers;

	// fire cleanup (does not gate m_syncInFlight - the put-chain does)
	if (!mergeServers.empty())
		m_dbClient.merge(m_syncMergeServersSlot, mergeServers.begin(), mergeServers.end());

	// put new servers if any (need their _ids for channels); otherwise the server map is complete now.
	if (!putServers.empty())
	{
		MojErr merr = m_dbClient.put(m_syncPutServersSlot, putServers.begin(), putServers.end());
		MojErrCheck(merr);
	}
	else
	{
		return syncFindChannels();
	}
	return MojErrNone;
}

// Stage 3: new servers created -> record their _ids (in the order they were put), then find channels.
MojErr IMServiceHandler::syncPutServersResult(MojObject& payload, MojErr err)
{
	if (err != MojErrNone)
	{
		MojLogError(IMServiceApp::s_log, _T("syncServersChannels: put(imserver) failed: %d"), err);
		m_syncInFlight = false;
		return MojErrNone;
	}

	MojObject results;
	payload.get(_T("results"), results);
	size_t i = 0;
	for (MojObject::ConstArrayIterator rIt = results.arrayBegin();
	     rIt != results.arrayEnd() && i < m_syncNewServerRemotes.size(); ++rIt, ++i)
	{
		MojString id;
		bool f = false;
		rIt->get(_T("id"), id, f);
		if (f)
			m_syncServerIdByRemote[m_syncNewServerRemotes[i]] = id;
	}
	return syncFindChannels();
}

// Stage 4: find existing imchannel rows so we can diff (not wipe) the channels too.
MojErr IMServiceHandler::syncFindChannels()
{
	MojDbQuery query;
	MojErr err = query.from(_T("com.palm.imchannel:1"));
	MojErrCheck(err);
	err = query.where(_T("serviceName"), MojDbQuery::OpEq, m_syncServiceName);
	MojErrCheck(err);
	err = m_dbClient.find(m_syncFindChannelsSlot, query);
	MojErrCheck(err);
	return MojErrNone;
}

// Stage 5: diff channels by remoteId. MERGE existing (keep _id AND chatThreadId - chatThreadId is
// deliberately NOT included so db8 leaves it untouched, which is what stops the duplicate-thread
// orphaning), PUT new (serverId from the server _id map), DEL those that vanished.
MojErr IMServiceHandler::syncFindChannelsResult(MojObject& payload, MojErr err)
{
	if (err != MojErrNone)
	{
		MojLogError(IMServiceApp::s_log, _T("syncServersChannels: find(imchannel) failed: %d"), err);
		m_syncInFlight = false;
		return MojErrNone;
	}

	std::map<std::string, MojString> existing; // channel remoteId -> _id
	MojObject results;
	payload.get(_T("results"), results);
	for (MojObject::ConstArrayIterator it = results.arrayBegin(); it != results.arrayEnd(); ++it)
	{
		MojString rid, id;
		bool f = false;
		it->get(_T("remoteId"), rid, f);
		it->get(_T("_id"), id, f);
		if (!rid.empty())
			existing[std::string(rid.data())] = id;
	}

	MojObject::ObjectVec mergeChannels, putChannels;
	MojObject delGoneChannels;
	std::set<std::string> incoming;

	for (MojObject::ConstArrayIterator sIt = m_syncServers.arrayBegin(); sIt != m_syncServers.arrayEnd(); ++sIt)
	{
		MojString serverRemote;
		bool f = false;
		sIt->get(_T("remoteId"), serverRemote, f);
		std::map<std::string, MojString>::iterator sm = m_syncServerIdByRemote.find(std::string(serverRemote.data()));
		if (sm == m_syncServerIdByRemote.end())
			continue; // server _id unresolved (shouldn't happen); skip its channels
		MojString serverId = sm->second;

		MojObject channelArr;
		if (!sIt->get(_T("channels"), channelArr))
			continue;

		for (MojObject::ConstArrayIterator cIt = channelArr.arrayBegin(); cIt != channelArr.arrayEnd(); ++cIt)
		{
			MojString remoteId, name, parentId;
			bool cf = false;
			cIt->get(_T("remoteId"), remoteId, cf);
			cIt->get(_T("name"), name, cf);
			bool hasParent = false;
			cIt->get(_T("parentId"), parentId, hasParent);
			MojInt64 position = 0;
			cIt->get(_T("position"), position);
			if (remoteId.empty())
				continue;
			incoming.insert(std::string(remoteId.data()));

			std::map<std::string, MojString>::iterator e = existing.find(std::string(remoteId.data()));
			if (e != existing.end())
			{
				// existing -> merge in place. NOTE: chatThreadId intentionally omitted so the existing
				// channel->chatthread link survives (this is what prevents the duplicate threads).
				MojObject m;
				MojErrCheck(m.putString(_T("_kind"), _T("com.palm.imchannel:1")));
				MojErrCheck(m.put(_T("_id"), e->second));
				MojErrCheck(m.putString(_T("serverId"), serverId));
				MojErrCheck(m.putString(_T("displayName"), name));
				MojErrCheck(m.putString(_T("name"), name));
				if (hasParent)
					MojErrCheck(m.putString(_T("parentId"), parentId));
				MojErrCheck(m.putInt(_T("position"), position));
				MojErrCheck(mergeChannels.push(m));
			}
			else
			{
				// new -> put
				MojObject channel;
				MojErrCheck(channel.putString(_T("_kind"), _T("com.palm.imchannel:1")));
				MojErrCheck(channel.putString(_T("serviceName"), m_syncServiceName));
				MojErrCheck(channel.putString(_T("remoteId"), remoteId));
				MojErrCheck(channel.putString(_T("serverId"), serverId));
				MojErrCheck(channel.putString(_T("displayName"), name));
				MojErrCheck(channel.putString(_T("name"), name));
				if (hasParent)
					MojErrCheck(channel.putString(_T("parentId"), parentId));
				MojErrCheck(channel.putInt(_T("position"), position));
				MojErrCheck(putChannels.push(channel));
			}
		}
	}

	// ADDITIVE/MERGE-ONLY: do NOT delete channels absent from this (possibly partial) roster snapshot.
	// A channel missing from one enumeration is almost always still loading (tdlib loads chats in waves),
	// not removed. Deleting it orphaned the channel's chatthread and produced a duplicate thread when the
	// channel reloaded - the exact recurring-duplicate bug. See the servers block above.
	(void)incoming; (void)delGoneChannels;

	if (!mergeChannels.empty())
		m_dbClient.merge(m_syncMergeChannelsSlot, mergeChannels.begin(), mergeChannels.end());

	if (!putChannels.empty())
	{
		MojErr merr = m_dbClient.put(m_syncPutChannelsSlot, putChannels.begin(), putChannels.end());
		MojErrCheck(merr);
	}
	else
	{
		MojLogInfo(IMServiceApp::s_log, _T("syncServersChannels: incremental roster reconcile complete (no new channels)"));
		m_syncInFlight = false;
	}
	return MojErrNone;
}

MojErr IMServiceHandler::syncPutChannelsResult(MojObject& payload, MojErr err)
{
	if (err != MojErrNone)
		MojLogError(IMServiceApp::s_log, _T("syncServersChannels: put(imchannel) failed: %d"), err);
	else
		MojLogInfo(IMServiceApp::s_log, _T("syncServersChannels: incremental roster reconcile complete"));
	m_syncInFlight = false;   // put-chain complete: allow the next enumeration to sync
	return MojErrNone;
}

// fire-and-forget cleanup result slots: log only (they don't gate the put-chain / m_syncInFlight).
MojErr IMServiceHandler::syncMergeServersResult(MojObject& payload, MojErr err)
{
	if (err != MojErrNone)
		MojLogError(IMServiceApp::s_log, _T("syncServersChannels: merge(imserver) failed: %d"), err);
	return MojErrNone;
}
MojErr IMServiceHandler::syncMergeChannelsResult(MojObject& payload, MojErr err)
{
	if (err != MojErrNone)
		MojLogError(IMServiceApp::s_log, _T("syncServersChannels: merge(imchannel) failed: %d"), err);
	return MojErrNone;
}
MojErr IMServiceHandler::syncDelGoneServersResult(MojObject& payload, MojErr err)
{
	if (err != MojErrNone)
		MojLogError(IMServiceApp::s_log, _T("syncServersChannels: del(gone imserver) failed: %d"), err);
	return MojErrNone;
}
MojErr IMServiceHandler::syncDelGoneChannelsResult(MojObject& payload, MojErr err)
{
	if (err != MojErrNone)
		MojLogError(IMServiceApp::s_log, _T("syncServersChannels: del(gone imchannel) failed: %d"), err);
	return MojErrNone;
}

/*
 * webOS Servers/Rooms M3: openChannel - the Messaging app calls this when the user opens a channel in
 * the Servers tab, so the transport joins the channel (prpl fetches its history + accepts sends).
 *   { "serviceName": "type_discord", "channel": "<channel key/snowflake>", "username": "<optional>" }
 */
MojErr IMServiceHandler::openChannel(MojServiceMessage* serviceMsg, const MojObject payload)
{
	MojLogTrace(IMServiceApp::s_log);

	MojString serviceName, channel, username;
	bool found = false;
	payload.get(_T("serviceName"), serviceName, found);
	bool haveChannel = false;
	payload.get(_T("channel"), channel, haveChannel);
	payload.get(_T("username"), username, found);   // optional

	if (serviceName.empty() || !haveChannel || channel.empty())
	{
		MojLogError(IMServiceApp::s_log, _T("openChannel: serviceName and channel are required"));
		serviceMsg->replyError(MojErrInvalidArg);
		return MojErrNone;
	}

	MojLogInfo(IMServiceApp::s_log, _T("openChannel: serviceName=%s channel=%s"), serviceName.data(), channel.data());
	LibpurpleAdapter::openChannel(serviceName.data(), username.empty() ? NULL : username.data(), channel.data());

	serviceMsg->replySuccess();
	return MojErrNone;
}

MojErr IMServiceHandler::onEnabled(MojServiceMessage* serviceMsg, const MojObject payload)
{
	MojRefCountedPtr<OnEnabledHandler> handler(new OnEnabledHandler(m_service, this));
	MojErr err = handler->start(payload);
	if (err == MojErrNone) {
		serviceMsg->replySuccess();
	} else {
		MojString error;
		MojString msg;
		MojErrToString(err, error);
		msg.format(_T("OnEnabledHandler.start() failed: error %d - %s"), err, error.data());
		MojLogError(IMServiceApp::s_log, _T("%s"), msg.data());

		serviceMsg->replyError(err);
	}
	return MojErrNone;
}

MojErr IMServiceHandler::deleteConfigResult(MojObject& payload, MojErr err)
{
    return err;
}

MojErr IMServiceHandler::putConfigResult(MojObject& payload, MojErr err)
{
    return err;
}

/**
 * Send IM handler.
 * callback Used by Activity Manager - gets fired from a DB watch
 *
 * parms from activity manager:
 *     {"$activity":{"activityId":1,"trigger":{"fired":true,"returnValue":true}}}
 */
MojErr IMServiceHandler::IMSend(MojServiceMessage* serviceMsg, const MojObject parms)
{
	MojLogInfo(IMServiceApp::s_log, _T("send IM request received."));

	// log the parameters
	logMojObjectJsonString(_T("IMSend parameters: %s"), parms);

	// if we were called by the activity manager we will get an activity id here to adopt
	// get the $activity object
	MojObject activityObj;
	MojInt64 activityId = 0;
	bool found = parms.get(_T("$activity"), activityObj);
	if (found)
		found = activityObj.get(_T("activityId"), activityId);
	if (!found) {
		MojString msg;
		msg.format(_T("IMSend failed: parameter has no activityId"));
		MojLogError(IMServiceApp::s_log, "%s", msg.data());
		// send error back to caller
		serviceMsg->replyError(MojErrInvalidArg, msg);
		return MojErrInvalidArg;
	}

	// check for returnValue = false in trigger parameter
	MojObject trigger;
	bool retVal = false;
	found = activityObj.get(_T("trigger"), trigger);
	if (found)
		found = trigger.get(_T("returnValue"), retVal);
	if (!found || !retVal) {
		MojString msg;
		msg.format(_T("IMSend failed: trigger does not have returnValue: true"));
		MojLogError(IMServiceApp::s_log, "%s", msg.data());
		// send error back to caller
		serviceMsg->replyError(MojErrInvalidArg, msg);
		return MojErrInvalidArg;
	}
	// create the outgoing message handler
	// This object is ref counted and will be deleted after the slot is invoked
	// Therefore we don't need to hold on to it or worry about deleting it
	MojRefCountedPtr<OutgoingIMHandler> handler(new OutgoingIMHandler(m_service, activityId, this));

	// query the DB for outgoing messages and send them
	MojErr err = handler->start();

	// Reply to caller
	if (err) {
		MojString error;
		MojString msg;
		MojErrToString(err, error);
		msg.format(_T("OutgoingIMHandler.start() failed: error %d - %s"), err, error.data());
		MojLogError(IMServiceApp::s_log, _T("%s"), msg.data());

		// send error back to caller
		serviceMsg->replyError(err, msg);
		return MojErrInternal;
	}


	// Reply to caller
	MojObject reply;
	reply.putString(_T("response"), _T("IMService processing outbox."));
	serviceMsg->replySuccess(reply);

	return MojErrNone;
}

/**
 * Send IM Command handler.
 * callback Used by Activity Manager - gets fired from a DB watch
 *
 * parms from activity manager:
 *     {"$activity":{"activityId":1,"trigger":{"fired":true,"returnValue":true}}}
 */
MojErr IMServiceHandler::IMSendCmd(MojServiceMessage* serviceMsg, const MojObject parms)
{
	MojLogInfo(IMServiceApp::s_log, _T("IMSendCmd request received."));

	// log the parameters
	logMojObjectJsonString(_T("IMSendCmd parameters: %s"), parms);

	// if we were called by the activity manager we will get an activity id here to adopt
	// get the $activity object
	MojObject activityObj;
	MojInt64 activityId = 0;
	bool found = parms.get(_T("$activity"), activityObj);
	if (found)
		found = activityObj.get(_T("activityId"), activityId);
	if (!found) {
		MojString msg;
		msg.format(_T("IMSendCmd failed: parameter has no activityId"));
		MojLogError(IMServiceApp::s_log, "%s", msg.data());
		// send error back to caller
		serviceMsg->replyError(MojErrInvalidArg, msg);
		return MojErrInvalidArg;
	}

	// check for returnValue = false in trigger parameter
	MojObject trigger;
	bool retVal = false;
	found = activityObj.get(_T("trigger"), trigger);
	if (found)
		found = trigger.get(_T("returnValue"), retVal);
	if (!found || !retVal) {
		MojString msg;
		msg.format(_T("IMSendCmd failed: trigger does not have returnValue: true"));
		MojLogError(IMServiceApp::s_log, "%s", msg.data());
		// send error back to caller
		serviceMsg->replyError(MojErrInvalidArg, msg);
		return MojErrInvalidArg;
	}

	// create the outgoing command handler
	// This object is ref counted and will be deleted after the slot is invoked
	// Therefore we don't need to hold on to it or worry about deleting it
	MojRefCountedPtr<OutgoingIMCommandHandler> handler(new OutgoingIMCommandHandler(m_service, activityId, this));

	// query the DB for outgoing commands and send them
	MojErr err = handler->start();

	if (err) {
		MojString error;
		MojString msg;
		MojErrToString(err, error);
		msg.format(_T("OutgoingIMCommandHandler.start() failed: error %d - %s"), err, error.data());
		MojLogError(IMServiceApp::s_log, _T("%s"), msg.data());
		// send error back to caller
		serviceMsg->replyError(err, msg);
		return MojErrInternal;
	}


	// Reply to caller
	MojObject reply;
	reply.putString(_T("response"), _T("IMService processing command list"));
	serviceMsg->replySuccess(reply);

	return MojErrNone;
}


/*
 * New incoming IM message
 */
bool IMServiceHandler::incomingIM(const char* serviceName, const char* username, const char* usernameFrom, const char* message, time_t timestamp,
		const char* channelName, const char* channelDisplayName, const char* serverId, const char* serverName, bool muted,
		const char* usernameFromDisplay)
{

	MojLogTrace(IMServiceApp::s_log);

	// log the parameters
	// don't log the message text
	// webOS Servers/Rooms: also log channel/server for multi-user-chat messages
	MojLogInfo (IMServiceApp::s_log, _T("incomingIM - IM received. serviceName: %s username: %s usernameFrom: %s channel: %s server: %s muted: %d"),
			serviceName, username, usernameFrom, channelName ? channelName : "", serverName ? serverName : "", muted);

	// no error - process the IM
	MojRefCountedPtr<IMMessage> imMessage(new IMMessage);

	// set the message fields based on the incoming parameters. muted (chat muted on the server
	// side, e.g. a muted Telegram chat) becomes flags.noNotification so the Messaging app stores
	// the message but suppresses the notification banner.
	MojErr err = imMessage->initFromCallback(serviceName, username, usernameFrom, message, timestamp, channelName, channelDisplayName, serverId, serverName, muted, usernameFromDisplay);

	if (!err) {
		// handle the message
		MojRefCountedPtr<IncomingIMHandler> incomingIMHandler(new IncomingIMHandler(m_service, this));
		err = incomingIMHandler->saveNewIMMessage(imMessage);
	}
	if (err) {
		MojString error;
		MojErrToString(err, error);
		MojLogError(IMServiceApp::s_log, _T("incomingIM failed: %d - %s"), err, error.data());
		return false;
	}

	return true;
}

/*
 * Change in buddy status
 */
bool IMServiceHandler::updateBuddyStatus(const char* accountId, const char* serviceName, const char* username, int availability,
		const char* customMessage, const char* groupName, const char* buddyAvatarLoc)
{

	MojLogTrace(IMServiceApp::s_log);

	// log the parameters
	MojLogInfo (IMServiceApp::s_log, _T("updateBuddyStatus - accountId: %s, serviceName: %s, username: %s, availability: %i, customMessage: %s, groupName: %s, buddyAvatarLoc: %s"),
			accountId, serviceName, username, availability, customMessage, groupName, buddyAvatarLoc);

	// handle the message
	MojRefCountedPtr<BuddyStatusHandler> buddyStatusHandler(new BuddyStatusHandler(m_service, this));
	MojErr err = buddyStatusHandler->updateBuddyStatus(accountId, serviceName, username, availability, customMessage, groupName, buddyAvatarLoc);

	if (err) {
		MojString error;
		MojErrToString(err, error);
		MojLogError(IMServiceApp::s_log, _T("updateBuddyStatus failed: %d - %s"), err, error.data());
		return false;
	}

	return true;
}

// Perf (#2): batched presence for one account - the adapter flushes a coalesced window of per-buddy
// presence ticks here as one array, and BuddyStatusHandler turns it into a single find + batched
// merge/put (vs a find+merge per buddy). Short-lived handler like updateBuddyStatus above.
bool IMServiceHandler::updateBuddyStatusBatch(const char* accountId, const char* serviceName, MojObject& updates)
{
	MojLogInfo(IMServiceApp::s_log, _T("updateBuddyStatusBatch - accountId: %s, serviceName: %s, count: %d"),
			accountId, serviceName, (int)updates.size());

	MojRefCountedPtr<BuddyStatusHandler> buddyStatusHandler(new BuddyStatusHandler(m_service, this));
	MojErr err = buddyStatusHandler->updateBuddyStatusBatch(accountId, serviceName, updates);
	if (err) {
		MojString error;
		MojErrToString(err, error);
		MojLogError(IMServiceApp::s_log, _T("updateBuddyStatusBatch failed: %d - %s"), err, error.data());
		return false;
	}
	return true;
}

/*
 * Receive a request for authorization (accept/deny) from another user to add us to their buddy list
 */
bool IMServiceHandler::receivedBuddyInvite(const char* serviceName, const char* username, const char* usernameFrom, const char* customMessage)
{
	// log the parameters
	MojLogInfo (IMServiceApp::s_log, _T("receivedBuddyInvite - serviceName: %s username: %s usernameFrom: %s message: %s"), serviceName, username, usernameFrom, customMessage);

	// Create an imcommand for the application to prompt user for acceptance
	MojRefCountedPtr<BuddyStatusHandler> buddyStatusHandler(new BuddyStatusHandler(m_service, this));
	MojErr err = buddyStatusHandler->receivedBuddyInvite(serviceName, username, usernameFrom, customMessage);

	if (err) {
		MojString error;
		MojErrToString(err, error);
		MojLogError(IMServiceApp::s_log, _T("receivedBuddyInvite failed: %d - %s"), err, error.data());
		return false;
	}

	return true;
}

/*
 * Receive a decline messages from a remote user we invited to be our buddy
 *
 * 07/13/2010 - Note: This doesn't get used currently - Libpurple doesn't seem to notify us in this case - at least I can't find it!!
 */
bool IMServiceHandler::buddyInviteDeclined(const char* serviceName, const char* username, const char* usernameFrom)
{
	// log the parameters
	MojLogInfo (IMServiceApp::s_log, _T("buddyInviteDeclined - serviceName: %s username: %s usernameFrom: %s"), serviceName, username, usernameFrom);

	// need to delete buddy and contact from DB
	return true;
}


MojErr IMServiceHandler::loginForTesting(MojServiceMessage* serviceMsg, const MojObject payload)
{
	if (m_loginState == NULL) {
		m_loginState = new IMLoginState(m_service, this);
	}

	m_loginState->loginForTesting(serviceMsg, payload);
	return MojErrNone;
}

/*
 * watch on the loginstate table was triggered
 */
MojErr IMServiceHandler::handleLoginStateChange(MojServiceMessage* serviceMsg, const MojObject payload)
{
	// Since the activity has the requirement "internet:true", the activity includes ConnectionManager
	// details. Use this information if our connectionState isn't yet initialized.
	m_connectionState.initConnectionStatesFromActivity(payload);

	if (m_loginState == NULL) {
		m_loginState = new IMLoginState(m_service, this);
	}

	return m_loginState->handleLoginStateChange(serviceMsg, payload);
}

/*
 * Discord QR / interactive-login channel. See AuthChannel.
 *
 * startQRLogin: kick off a PENDING (disposable) prpl remote-auth login so the prpl
 * generates the QR. The QR + subsequent state are surfaced over getAuthChallenge.
 */
MojErr IMServiceHandler::startQRLogin(MojServiceMessage* serviceMsg, const MojObject payload)
{
	MojString serviceName, username;
	MojErr err = payload.getRequired(_T("serviceName"), serviceName);
	MojErrCheck(err);
	err = payload.getRequired(_T("username"), username);
	MojErrCheck(err);

	MojLogInfo(IMServiceApp::s_log, _T("startQRLogin serviceName=%s username=%s"), serviceName.data(), username.data());
	LibpurpleAdapter::startQRLogin(serviceName.data(), username.data());

	MojObject reply;
	err = reply.putBool(_T("returnValue"), true);
	MojErrCheck(err);
	return serviceMsg->replySuccess(reply);
}

// getAuthChallenge: register a (subscription) snapshot feed for this account key.
MojErr IMServiceHandler::getAuthChallenge(MojServiceMessage* serviceMsg, const MojObject payload)
{
	MojString serviceName, username;
	MojErr err = payload.getRequired(_T("serviceName"), serviceName);
	MojErrCheck(err);
	err = payload.getRequired(_T("username"), username);
	MojErrCheck(err);

	if (m_authChannel == NULL)
		return serviceMsg->replyError(MojErrNotInitialized, _T("auth channel not ready"));

	return m_authChannel->subscribe(serviceMsg, serviceName.data(), username.data());
}

// submitAuthInput: the UI's answer to a challenge. For QR we only support
// refresh (get a fresh code) and cancel (tear the pending login down).
MojErr IMServiceHandler::submitAuthInput(MojServiceMessage* serviceMsg, const MojObject payload)
{
	MojString serviceName, username;
	MojErr err = payload.getRequired(_T("serviceName"), serviceName);
	MojErrCheck(err);
	err = payload.getRequired(_T("username"), username);
	MojErrCheck(err);

	MojString action;
	bool found = false;
	payload.get(_T("action"), action, found);
	const char* act = found ? action.data() : "";

	MojLogInfo(IMServiceApp::s_log, _T("submitAuthInput serviceName=%s username=%s action=%s"),
	           serviceName.data(), username.data(), act);

	if (strcmp(act, "cancel") == 0) {
		LibpurpleAdapter::cancelQRLogin(serviceName.data(), username.data());
	} else if (strcmp(act, "refresh") == 0) {
		LibpurpleAdapter::cancelQRLogin(serviceName.data(), username.data());
		LibpurpleAdapter::startQRLogin(serviceName.data(), username.data());
	} else if (strcmp(act, "captcha") == 0) {
		// The UI solved the hCaptcha; hand the response token back to the prpl, which
		// re-POSTs remote-auth/login with it. value carries the hCaptcha response token.
		MojString captchaKey;
		bool haveKey = false;
		payload.get(_T("value"), captchaKey, haveKey);
		if (!haveKey)
			payload.get(_T("captcha_key"), captchaKey, haveKey);
		bool ok = LibpurpleAdapter::submitCaptcha(serviceName.data(), username.data(),
		                                          haveKey ? captchaKey.data() : "");
		if (!ok)
			MojLogError(IMServiceApp::s_log, _T("submitAuthInput: captcha had no pending request"));
	}

	if (m_authChannel == NULL)
		return serviceMsg->replyError(MojErrNotInitialized, _T("auth channel not ready"));
	return m_authChannel->submitInput(serviceMsg, serviceName.data(), username.data(), act, NULL);
}

/**
 * Log the json for a MojObject - useful for debugging
 */
MojErr IMServiceHandler::logMojObjectJsonString(const MojChar* format, const MojObject mojObject) {

	if (IMServiceApp::s_log.level() <= MojLogger::LevelInfo) {
		MojString mojStringJson;
		mojObject.toJson(mojStringJson);
		MojLogInfo(IMServiceApp::s_log, format, mojStringJson.data());
	}

	return MojErrNone;
}

/**
 * Log the json for an incoming IM payload so we can troubleshoot errors in the field in
 * non debug builds.
 *
 * removes "private" data - ie. message body
 */
MojErr IMServiceHandler::privatelogIMMessage(const MojChar* format, MojObject IMObject, const MojChar* messageTextKey) {

	MojString mojStringJson;
	MojString msgText;

	// replace the message body
	// not much we can do about errors here...

	bool found = false;
	MojErr err = IMObject.del(messageTextKey, found);
	MojErrCheck(err);
	if (found) {
		msgText.assign(_T("***IM Body Removed***"));
		IMObject.put(messageTextKey, msgText);
	}

	// log it (non debug)
	IMObject.toJson(mojStringJson);
	MojLogNotice(IMServiceApp::s_log, format, mojStringJson.data());

	return MojErrNone;
}

/*
 * Called by each signal handler to indicate the are actively starting to process a message
 */
void IMServiceHandler::ProcessStarting()
{
	m_activeProcesses++;
}

/*
 * Called by each signal handler to indicate the are done processing
 */
void IMServiceHandler::ProcessDone()
{
	m_activeProcesses--;

	if (m_activeProcesses < 0) {
		MojLogError(IMServiceApp::s_log, _T("ProcessDone - active process count is negative!!"));
		m_activeProcesses = 0;
	}

	// remove a current shutdown timer if we have one already
	if (m_shutdownCallbackId) {
		MojLogNotice(IMServiceApp::s_log, "ProcessDone - shutdown delayed");
		g_source_remove(m_shutdownCallbackId);
		m_shutdownCallbackId = 0;
	}

	if (OkToShutdown()) {
		MojLogNotice(IMServiceApp::s_log, "ProcessDone - shutting down in %llu seconds", SHUTDOWN_DELAY_SECONDS);
		m_shutdownCallbackId = g_timeout_add_seconds(SHUTDOWN_DELAY_SECONDS, &ShutdownCallback, this);
	} else {
		MojLogInfo(IMServiceApp::s_log, "ProcessDone - %llu processes still active, accounts still online", m_activeProcesses);
	}
}

/*
 * timer callback
 */
gboolean IMServiceHandler::ShutdownCallback(void* data)
{
	assert(data);
	IMServiceHandler* handler = static_cast<IMServiceHandler*>(data);
	handler->m_shutdownCallbackId = 0;

	if (handler->OkToShutdown()) {
		MojLogNotice(IMServiceApp::s_log, "im service shutting down...");
		IMServiceApp::Shutdown();
	} else {
		MojLogNotice(IMServiceApp::s_log, "im service shutdown aborted; clients still active");
	}

	return false; // return false to make sure we don't get called again
}

/*
 * Are all processes done?
 */
bool IMServiceHandler::OkToShutdown()
{
	// When launched as the resident upstart daemon (imdaemon.sh sets IM_RESIDENT=1), never
	// self-terminate on idle: the daemon owns the process lifecycle and respawns us immediately, so
	// an idle self-shutdown just produces a shutdown<->respawn churn every SHUTDOWN_DELAY window that
	// tears down and reloads every prpl each cycle -- including purple-signal's in-process JVM, which
	// looks like "Signal keeps crashing" and never lets slow accounts (Signal's JVM, WhatsApp's
	// contact sync) reach a stable connection. Staying resident lets the login-state machine retry
	// with backoff until accounts reconnect. The on-demand .service path (no IM_RESIDENT) keeps the
	// original idle-out behaviour. An external SIGTERM (reboot / upstart stop) still shuts us down.
	static const bool resident = (getenv("IM_RESIDENT") != NULL);
	if (resident)
		return false;

	// ask libpurple if all accounts are logged off
	if (0 == m_activeProcesses) {
		return LibpurpleAdapter::allAccountsOffline();
	}

	return false;
}

