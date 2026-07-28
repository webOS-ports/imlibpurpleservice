/*
 * IncomingIMHandler.cpp
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
 * IncomingIMHandler class handles incoming IM messages
 */


#include "IncomingIMHandler.h"
#include "db/MojDbQuery.h"
#include "IMServiceApp.h"
#include "IMServiceHandler.h"

/*
 * Note the order of the globals inited below, it matters
 */
IncomingIMHandler::IncomingIMHandler(MojService* service, IMServiceApp::Listener* listener)
: m_IMSaveMessageSlot(this, &IncomingIMHandler::IMSaveMessageResult),
  m_IMDedupFindSlot(this, &IncomingIMHandler::IMDedupFindResult),
  m_service(service),
  m_dbClient(service)
{
	// tell listener we are now active
	m_listener = listener;
	m_listener->ProcessStarting();
}


IncomingIMHandler::~IncomingIMHandler()
{
	// tell listener we are done
	m_listener->ProcessDone();
}

/**
 * callback from the save message call.
 */
MojErr IncomingIMHandler::IMSaveMessageResult(MojObject& result, MojErr saveErr)
{
	MojLogTrace(IMServiceApp::s_log);

	if (saveErr) {
		MojString error;
		MojErrToString(saveErr, error);
		MojLogError(IMServiceApp::s_log, _T("database put failed. error %d - %s"), saveErr, error.data());
	}
	else {
		IMServiceHandler::logMojObjectJsonString(_T("database put success: %s"), result);
	}

	return MojErrNone;
}

/*
 * Save the new incoming IM message to the DB
 *
 * @return MojErr if error - caller will log it
 */
MojErr IncomingIMHandler::saveNewIMMessage(MojRefCountedPtr<IMMessage> IMMessage) {

	MojErr err;

	// The message we are handling
	m_IMMessage = IMMessage;

	// build the db object (held in m_pendingDbObject across the async dedup find below)
	err = m_IMMessage->createDBObject(m_pendingDbObject);
	MojErrCheck(err);

	//add our kind to the object
	//luna://com.palm.db/put '{"objects":[{"_kind":"com.palm.test:1","foo":1,"bar":1000}]}'
	err = m_pendingDbObject.putString(_T("_kind"), PALM_DB_IMMESSAGE_KIND);
	MojErrCheck(err);

	// webOS db8-authoritative dedup: an incoming message whose serviceMessageId is ALREADY in db8 (same
	// account) must not be written again. Overlapping history fetches and reconnect/re-open backfills
	// re-deliver the same ids; without a check they pile up duplicate immessages (the historic x37
	// duplication). The previous guard was a per-plugin persisted "seen" id set, but that file diverged
	// from db8 -- after a message's db8 row was gone (db8 prune, app-side re-thread, etc.) the set still
	// said "seen", so backfill refused to re-write it and it stayed permanently missing (the Discord
	// "~5 messages in the gap" bug). Deduping against db8 itself is self-healing: a message not in db8
	// gets written even if it was seen before; a message in db8 is skipped. Query first, write in the
	// callback. Messages with no serviceMessageId can't be deduped -> write directly (unchanged behaviour).
	MojString serviceMessageId, serviceName, username;
	bool haveId = false, haveSvc = false, haveUser = false;
	m_pendingDbObject.get(MOJDB_SERVICE_MSG_ID, serviceMessageId, haveId);
	m_pendingDbObject.get(MOJDB_SERVICENAME, serviceName, haveSvc);
	m_pendingDbObject.get(MOJDB_USERNAME, username, haveUser);

	if (haveId && !serviceMessageId.empty() && haveSvc && haveUser) {
		// Same (serviceName, username, serviceMessageId) scoping the reaction/receipt handlers use, so it
		// hits the existing serviceMessageId index.
		MojDbQuery query;
		err = query.from(PALM_DB_IMMESSAGE_KIND);                                  MojErrCheck(err);
		err = query.where(MOJDB_SERVICENAME, MojDbQuery::OpEq, serviceName);        MojErrCheck(err);
		err = query.where(MOJDB_USERNAME, MojDbQuery::OpEq, username);              MojErrCheck(err);
		err = query.where(MOJDB_SERVICE_MSG_ID, MojDbQuery::OpEq, serviceMessageId);MojErrCheck(err);
		query.limit(1);
		err = m_dbClient.find(this->m_IMDedupFindSlot, query, false);
		MojErrCheck(err);
		return MojErrNone;
	}

	// No serviceMessageId to dedup on -> write directly.
	return putIMMessageToDb();
}

/**
 * Callback for the pre-write dedup find. Writes only when the message is NOT already in db8.
 */
MojErr IncomingIMHandler::IMDedupFindResult(MojObject& result, MojErr findErr)
{
	MojLogTrace(IMServiceApp::s_log);

	if (findErr) {
		// Never drop a message because the dedup lookup failed -- write it (worst case a rare duplicate,
		// far better than silently losing an incoming message).
		MojString error;
		MojErrToString(findErr, error);
		MojLogError(IMServiceApp::s_log, _T("dedup find failed (%d - %s); saving message anyway"), findErr, error.data());
		return putIMMessageToDb();
	}

	MojObject results;
	result.get(_T("results"), results);
	if (!results.empty()) {
		// Already present in db8 -> genuine duplicate, skip the write.
		MojLogInfo(IMServiceApp::s_log, _T("incoming message already in db8 (serviceMessageId dedup) - skipping duplicate"));
		return MojErrNone;
	}

	return putIMMessageToDb();
}

/**
 * Do the actual db8 put of m_pendingDbObject. Shared by the dedup-miss and no-serviceMessageId paths.
 */
MojErr IncomingIMHandler::putIMMessageToDb()
{
	MojErr err;

	// log it - OK to show body in debug log
	MojString json;
	err = m_pendingDbObject.toJson(json);
	MojErrCheck(err);
	MojLogDebug(IMServiceApp::s_log, _T("saving message to db: %s"), json.data());
	MojLogInfo(IMServiceApp::s_log, _T("saving message to db"));

	// save it - the save generates a call to the save result handler
	err = m_dbClient.put(this->m_IMSaveMessageSlot, m_pendingDbObject);
	MojErrCheck(err);

	return MojErrNone;
}

