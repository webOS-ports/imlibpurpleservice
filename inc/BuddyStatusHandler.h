/*
 * BuddyStatusHandler.h
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
 * BuddyStatusHandler class handles buddy presence updates
 */

#ifndef BuddyStatusHandler_H_
#define BuddyStatusHandler_H_

#include "core/MojService.h"
#include "db/MojDbServiceClient.h"
#include "db/MojDb.h"
#include "IMServiceApp.h"

// MojoDB fields
// buddy status fields
#define MOJDB_GROUP          _T("group")
#define MOJDB_AVAILABILITY   _T("availability")
#define MOJDB_BUDDYSTATUS_BUDDYNAME   _T("username")
#define MOJDB_BUDDYSTATUS_ACCOUNTID   _T("accountId")

// imcommand properties
#define MOJDB_COMMAND				_T("command")
#define MOJDB_PARAMS				_T("params")
#define MOJDB_FROM_USER		   		_T("fromUsername")
#define MOJDB_SERVICE_NAME          _T("serviceName") // gmail, aol
#define MOJDB_HANDLER				_T("handler")
#define MOJDB_ID					_T("_id")
#define MOJODB_TARGET_USER          _T("targetUsername")

class BuddyStatusHandler : public MojSignalHandler
{
public:

	static const int errCannotProcessInput = 100;
	static const int errGenericDBError = 101;

	BuddyStatusHandler(MojService* service, IMServiceApp::Listener* listener);
	virtual ~BuddyStatusHandler();
	MojErr updateBuddyStatus(const char* accountId, const char* serviceName, const char* username, int availability,
			const char* customMessage, const char* groupName, const char* buddyAvatarLoc);
	// Perf (#2): batched presence update for ONE account. `updates` is an array of
	// { username, availability, status, group }. Does a single find(byAccountId) then ONE batched
	// merge (existing rows, by _id) + ONE batched put (new rows) instead of a find+merge PER buddy -
	// the ~32x-faster path measured on-device. Avatars are handled by the per-buddy path (rare).
	MojErr updateBuddyStatusBatch(const char* accountId, const char* serviceName, MojObject& updates);
	MojErr receivedBuddyInvite(const char* serviceName, const char* username, const char* usernameFrom, const char* customMessage);


private:

	MojDbClient::Signal::Slot<BuddyStatusHandler> m_saveStatusSlot;
	MojErr saveStatusResult(MojObject& result, MojErr err);

	// Perf (#2) batched-presence slots + carried state (see updateBuddyStatusBatch).
	MojString m_batchAccountId;
	MojString m_batchServiceName;
	MojObject m_batchUpdates;   // array of { username, availability, status, group }
	MojDbClient::Signal::Slot<BuddyStatusHandler> m_batchFindSlot;
	MojErr batchFindResult(MojObject& result, MojErr err);
	MojDbClient::Signal::Slot<BuddyStatusHandler> m_batchMergeSlot;
	MojErr batchMergeResult(MojObject& result, MojErr err);
	MojDbClient::Signal::Slot<BuddyStatusHandler> m_batchPutSlot;
	MojErr batchPutResult(MojObject& result, MojErr err);

	MojDbClient::Signal::Slot<BuddyStatusHandler> m_saveContactSlot;
	MojErr saveContactResult(MojObject& result, MojErr err);

	MojDbClient::Signal::Slot<BuddyStatusHandler> m_findCommandSlot;
	MojErr findCommandResult(MojObject& result, MojErr err);

	MojDbClient::Signal::Slot<BuddyStatusHandler> m_saveCommandSlot;
	MojErr saveCommandResult(MojObject& result, MojErr err);

	MojDbClient::Signal::Slot<BuddyStatusHandler> m_findContactSlot;
	MojErr findContactResult(MojObject& result, MojErr err);

	MojErr createBuddyQuery(const char* username, const char* accountId, MojDbQuery& query);
	MojErr createContactQuery(const char* username, const char* serviceName, MojDbQuery& query);
	MojErr createCommandQuery(MojDbQuery& query);
	MojErr createCommandObject(MojObject& returnObj);

	// status fields we update
	MojInt32 m_availability = 0;
	MojString m_customMessage;
	MojString m_groupName;
	MojString m_buddyAvatarLoc;

	// current user and service name
	MojString m_username;
	MojString m_serviceName;
	MojString m_fromUsername;

	// Pointer to the service used for creating/sending requests.
	MojService*	m_service;

	// Database client used to make any requests (put, watch, find, etc.) to Mojo DB
	MojDbServiceClient m_dbClient;
	MojDbServiceClient m_tempdbClient;

	// listener to tell when we are ready to shutdown
	IMServiceApp::Listener* m_listener;
};

#endif /* BuddyStatusHandler_H_ */
