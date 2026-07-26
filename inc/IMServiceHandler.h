/*
 * IMServiceHandler.h
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


#ifndef IMSERVICEHANDLER_H_
#define IMSERVICEHANDLER_H_

#include <map>
#include <vector>
#include <string>
#include "core/MojService.h"
#include "core/MojServiceMessage.h"
#include "db/MojDbServiceClient.h"
#include "db/MojDb.h"
#include "IMLoginState.h"
#include "ConnectionStateHandler.h"
#include "IMServiceApp.h"
#include "DisplayController.h"
#include "AuthChannel.h"

class IMServiceHandler : public MojService::CategoryHandler, public IMServiceCallbackInterface, public IMServiceApp::Listener
{
public:

	static const int SERVICE_CRASHED_ERROR_CODE = -1;

	IMServiceHandler(MojService* service);
	virtual ~IMServiceHandler();
	MojErr init();

	// libpurple callback
	// webOS Servers/Rooms: channelName/serverId/serverName are set for multi-user-chat (MUC)
	// messages (Discord channels etc.), NULL for 1:1 IMs.
	virtual bool incomingIM(const char* serviceName, const char* username, const char* usernameFrom, const char* message, time_t timestamp = 0,
			const char* channelName = NULL, const char* channelDisplayName = NULL, const char* serverId = NULL, const char* serverName = NULL, bool muted = false,
			const char* usernameFromDisplay = NULL, const char* serviceMessageId = NULL,
			const char* quotedMessageId = NULL, const char* quotedText = NULL, const char* quotedFrom = NULL, bool outgoing = false);
	virtual bool handleReaction(const char* serviceName, const char* username, const char* targetServiceMessageId,
			const char* emoji, const char* sender);
	virtual bool handleReactionSet(const char* serviceName, const char* username, const char* targetServiceMessageId,
			const char* serialized);
	virtual bool handleOutboxId(const char* serviceName, const char* username, const char* serviceMessageId,
			const char* text);
	virtual bool updateBuddyStatus(const char* accountId, const char* serviceName, const char* username, int availability,
			const char* customMessage, const char* groupName, const char* buddyAvatarLoc);
	// Perf (#2): batched presence for one account (see IMServiceCallbackInterface / BuddyStatusHandler).
	virtual bool updateBuddyStatusBatch(const char* accountId, const char* serviceName, MojObject& updates);
	virtual bool receivedBuddyInvite(const char* serviceName, const char* username, const char* usernameFrom, const char* message);
	virtual bool buddyInviteDeclined(const char* serviceName, const char* username, const char* usernameFrom);
	// webOS Servers/Rooms M3: upsert the enumerated guild->channel roster into db8 (see .cpp).
	virtual bool syncServersChannels(const char* serviceName, const char* username, MojObject& serversObj);

	static MojErr logMojObjectJsonString(const MojChar* format, const MojObject mojObject);
	// strip message body to protect private data
	static MojErr privatelogIMMessage(const MojChar* format, MojObject mojObject, const MojChar* messageTextKey);

	// called by each signal handler when they start and stop processing so we know when to shut down
	void ProcessStarting();
	void ProcessDone();

private:
	static const MojInt64 SHUTDOWN_DELAY_SECONDS = 30; // number of seconds to wait before shutting down
	static const Method s_methods[];
	static const char* const COM_PALM_MESSAGING_KIND;

	// Pointer to the service used for creating/sending requests.
	MojService*	m_service;

	// Database client used to make any requests (put, watch, find, etc.) to Mojo DB
	MojDbServiceClient m_dbClient;
	// Temp DB client (com.palm.tempdb) - imbuddystatus lives there
	MojDbServiceClient m_tempdbClient;

    MojDbClient::Signal::Slot<IMServiceHandler> m_deleteConfigSlot;
    MojErr deleteConfigResult(MojObject& payload, MojErr err);
    MojDbClient::Signal::Slot<IMServiceHandler> m_putConfigSlot;
    MojErr putConfigResult(MojObject& payload, MojErr err);

    /* On a keepData delete, record the kept account (com.palm.imretaineddata:1) so a later
     * "Delete Account Data" UI can list it and purge it on demand. Stores accountId + the purge keys
     * (username/serviceName) + a friendly label (alias/templateId, forwarded by the account service). */
    MojDbClient::Signal::Slot<IMServiceHandler> m_putRetainedDataSlot;
    MojErr putRetainedDataResult(MojObject& payload, MojErr err);
    MojDbClient::Signal::Slot<IMServiceHandler> m_deleteRetainedDataSlot;
    MojErr deleteRetainedDataResult(MojObject& payload, MojErr err);

    /* On account delete, purge the account's db8 chat data so the Messaging app
     * doesn't keep showing old conversations after the account is gone. Mirrors
     * OnEnabledHandler::accountDisabled() but driven from onDelete (the disable path
     * does not reliably run on delete - the account is already gone from the account
     * manager, so its username/serviceName can't be resolved there). */
    MojErr purgeAccountData(const char* accountId, const char* username, const char* serviceName, bool keepData);

    /* Batch-delete every leftover db8 record for a whole service (contacts, messages, commands,
     * buddystatus), keyed on serviceName/ims.type rather than accountId -- for orphans left behind
     * when an account was removed without a clean purge (its accountId is no longer known). Reuses
     * the purgeAccountData del slots. */
    MojErr purgeServiceData(const char* serviceName);
    /* At startup, consume a plain-text sentinel (PURGE_SENTINEL_PATH: one serviceName per line) and
     * purgeServiceData() each -- a luna-send-free way to reset a service's data from the shell. */
    void checkPurgeSentinel();
    MojDbClient::Signal::Slot<IMServiceHandler> m_deleteImLoginStateSlot;
    MojErr deleteImLoginStateResult(MojObject& payload, MojErr err);
    MojDbClient::Signal::Slot<IMServiceHandler> m_deleteImMessagesSlot;
    MojErr deleteImMessagesResult(MojObject& payload, MojErr err);
    MojDbClient::Signal::Slot<IMServiceHandler> m_deleteImCommandsSlot;
    MojErr deleteImCommandsResult(MojObject& payload, MojErr err);
    MojDbClient::Signal::Slot<IMServiceHandler> m_deleteContactsSlot;
    MojErr deleteContactsResult(MojObject& payload, MojErr err);
    MojDbClient::Signal::Slot<IMServiceHandler> m_deleteImBuddyStatusSlot;
    MojErr deleteImBuddyStatusResult(MojObject& payload, MojErr err);
    /* onDelete-only purge of the messaging-app-owned server/channel roster kinds
     * (com.palm.imserver:1 / com.palm.imchannel:1). syncServersChannels writes these rows with
     * serviceName (+remoteId/serverId) but NO accountId - the imserver byAccountId index is never
     * populated - so the purge keys on serviceName, matching how syncServersChannels itself queries. */
    MojDbClient::Signal::Slot<IMServiceHandler> m_deleteImServersSlot;
    MojErr deleteImServersResult(MojObject& payload, MojErr err);
    MojDbClient::Signal::Slot<IMServiceHandler> m_deleteImChannelsSlot;
    MojErr deleteImChannelsResult(MojObject& payload, MojErr err);

    /* webOS Servers/Rooms M3: enumerated server->channel roster upsert. A login-time (and debounced
     * post-buddy-change) sync that reconciles this account's imserver/imchannel rows against the live
     * roster INCREMENTALLY - NOT a wipe+recreate. The old delete+recreate gave every recreated
     * imchannel a new _id and dropped its chatThreadId, which orphaned the channel's chatthread and
     * spawned a duplicate on the next message. Instead we diff by the byRemoteId index, like
     * BuddyListConsolidator does for contacts: MERGE existing rows in place (keeping _id AND
     * chatThreadId - we simply don't include chatThreadId in the merge), PUT only genuinely new rows,
     * and DEL only rows that disappeared from the roster. Chain: find imserver -> (merge existing /
     * put new / del gone) -> find imchannel -> (merge existing / put new / del gone). serverId on a
     * channel is resolved from the server _id map (existing _ids + the put results for new servers).
     * m_syncServers holds the pending {remoteId,name,channels:[...]} roster across the async hops. */
    MojObject m_syncServers;
    MojString m_syncServiceName;
    bool m_syncInFlight;   // a roster reconcile is running; serialize to protect the shared state below
    std::map<std::string, MojString> m_syncServerIdByRemote; // server remoteId -> _id (existing + new)
    std::vector<std::string> m_syncNewServerRemotes;         // remoteIds of NEW servers, in put order
    MojErr syncServersChannelsStart();
    MojErr syncFindChannels();  // helper: kick off the imchannel find once server _ids are all resolved
    // put-chain (drives m_syncInFlight): find servers -> put new servers -> find channels -> put new channels
    MojDbClient::Signal::Slot<IMServiceHandler> m_syncFindServersSlot;
    MojErr syncFindServersResult(MojObject& payload, MojErr err);
    MojDbClient::Signal::Slot<IMServiceHandler> m_syncPutServersSlot;
    MojErr syncPutServersResult(MojObject& payload, MojErr err);
    MojDbClient::Signal::Slot<IMServiceHandler> m_syncFindChannelsSlot;
    MojErr syncFindChannelsResult(MojObject& payload, MojErr err);
    MojDbClient::Signal::Slot<IMServiceHandler> m_syncPutChannelsSlot;
    MojErr syncPutChannelsResult(MojObject& payload, MojErr err);
    // fire-and-forget cleanup (merge changed rows / delete removed rows); results only logged
    MojDbClient::Signal::Slot<IMServiceHandler> m_syncMergeServersSlot;
    MojErr syncMergeServersResult(MojObject& payload, MojErr err);
    MojDbClient::Signal::Slot<IMServiceHandler> m_syncMergeChannelsSlot;
    MojErr syncMergeChannelsResult(MojObject& payload, MojErr err);
    MojDbClient::Signal::Slot<IMServiceHandler> m_syncDelGoneServersSlot;
    MojErr syncDelGoneServersResult(MojObject& payload, MojErr err);
    MojDbClient::Signal::Slot<IMServiceHandler> m_syncDelGoneChannelsSlot;
    MojErr syncDelGoneChannelsResult(MojObject& payload, MojErr err);

	IMLoginState* m_loginState;
	ConnectionState m_connectionState;
	DisplayController* m_displayController;
	// Interactive-login (Discord QR) challenge channel surfaced to the accounts UI.
	AuthChannel* m_authChannel;

	// count of active processes (signal handlers)
	MojInt64 m_activeProcesses;

	// shutdown timer active
	MojInt64 m_shutdownCallbackId;

	MojErr onEnabled(MojServiceMessage* serviceMsg, const MojObject payload);
	MojErr onCreate(MojServiceMessage* serviceMsg, const MojObject payload);
	MojErr onDelete(MojServiceMessage* serviceMsg, const MojObject payload);
	/* palm://com.palm.imlibpurple/purgeRetainedData {accountId, username, serviceName} - wipe the
	 * on-device data that was kept when the account was removed with keepData, then delete the
	 * com.palm.imretaineddata marker. Driven by the Accounts app "Delete Account Data" UI. */
	MojErr purgeRetainedData(MojServiceMessage* serviceMsg, const MojObject payload);

	MojErr handleLoginStateChange(MojServiceMessage* msg, const MojObject payload);
	MojErr loginForTesting(MojServiceMessage* msg, const MojObject payload);

	// Discord QR / interactive-login channel (see AuthChannel):
	//  startQRLogin     {serviceName, username}         - spin up the pending remote-auth login
	//  getAuthChallenge {serviceName, username}         - (subscribe) push the QR/state snapshot
	//  submitAuthInput  {serviceName, username, action} - action: refresh | cancel
	MojErr startQRLogin(MojServiceMessage* msg, const MojObject payload);
	MojErr getAuthChallenge(MojServiceMessage* msg, const MojObject payload);
	MojErr submitAuthInput(MojServiceMessage* msg, const MojObject payload);
	// webOS Servers/Rooms M3: join a channel on open so its history is fetched (see .cpp).
	MojErr openChannel(MojServiceMessage* msg, const MojObject payload);

	// Kicks off the send process. Queries DB for outgoing messages and sends them.
	MojErr IMSend(MojServiceMessage* msg, const MojObject payload);

	// Kicks off the send command process. Queries DB for outgoing commands and sends them.
	MojErr IMSendCmd(MojServiceMessage* msg, const MojObject payload);

	// can we shut down?
	bool OkToShutdown();

	// GLib Main Event Loop callback to shutdown the process.
	static gboolean ShutdownCallback(void* data);

};

#endif /* IMSERVICEHANDLER_H_ */
