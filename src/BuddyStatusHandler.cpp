/**
 * BuddyStatusHandler.cpp
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

#include <map>
#include <string>
#include "BuddyStatusHandler.h"
#include "db/MojDbQuery.h"
#include "IMServiceApp.h"
#include "IMServiceHandler.h"
#include "IMDefines.h"
#include "IMMessage.h"
#include "sanitize.h"

/*
 * Note the order of the globals inited below, it matters
 */
BuddyStatusHandler::BuddyStatusHandler(MojService* service, IMServiceApp::Listener* listener)
: m_saveStatusSlot(this, &BuddyStatusHandler::saveStatusResult),
  m_saveContactSlot(this, &BuddyStatusHandler::saveContactResult),
  m_findCommandSlot(this, &BuddyStatusHandler::findCommandResult),
  m_saveCommandSlot(this, &BuddyStatusHandler::saveCommandResult),
  m_findContactSlot(this, &BuddyStatusHandler::findContactResult),
  m_batchFindSlot(this, &BuddyStatusHandler::batchFindResult),
  m_batchMergeSlot(this, &BuddyStatusHandler::batchMergeResult),
  m_batchPutSlot(this, &BuddyStatusHandler::batchPutResult),
  m_service(service),
  m_dbClient(service, MojDbServiceDefs::ServiceName),
  m_tempdbClient(service, MojDbServiceDefs::TempServiceName)
{
	// tell listener we are now active
	m_listener = listener;
	m_listener->ProcessStarting();
}


BuddyStatusHandler::~BuddyStatusHandler()
{
	// tell listener we are done
	m_listener->ProcessDone();
}

/*
 * Find the buddy the status change is for and update its status in the DB
 *
 * @return MojErr if error - caller will log it
 */
MojErr BuddyStatusHandler::updateBuddyStatus(const char* accountId, const char* serviceName, const char* username, int availability,
		const char* customMessage, const char* groupName, const char* buddyAvatarLoc)
{
	// remember the status fields to update
	m_availability = availability;
	// Encode astral emoji in the status message / group name so they survive the device's JS
	// runtimes (see sanitize.h). These are display-only fields (not address/match keys).
	char *safeCustom = customMessage ? encodeAstralEntities(customMessage) : NULL;
	m_customMessage.assign(safeCustom ? safeCustom : customMessage);
	if (safeCustom) free(safeCustom);
	char *safeGroup = groupName ? encodeAstralEntities(groupName) : NULL;
	m_groupName.assign(safeGroup ? safeGroup : groupName);
	if (safeGroup) free(safeGroup);


	//construct our where clause - find by username and accountId
	MojDbQuery query;
	MojErr err;
	err = createBuddyQuery(username, accountId, query);
	if (err) {
		MojString error;
		MojErrToString(err, error);
		MojLogError(IMServiceApp::s_log, _T("createBuddyQuery failed: error %d - %s"), err, error.data());
		return err;
	}

	// create the merge property
	MojObject propObject;
	// status fields to update
	propObject.putString(MOJDB_STATUS, m_customMessage);
	propObject.putInt(MOJDB_AVAILABILITY, m_availability);
	propObject.putString(MOJDB_GROUP, m_groupName);

	// log it
	MojString json;
	propObject.toJson(json);
	MojLogInfo(IMServiceApp::s_log, _T("saving buddy to db: %s"), json.data());

	// call merge
	err = m_tempdbClient.merge(m_saveStatusSlot, query, propObject);
	if (err) {
		MojString error;
		MojErrToString(err, error);
		MojLogError(IMServiceApp::s_log, _T("dbClient merge buddy status failed: error %d - %s"), err, error.data());
	}

	// if the buddy avatar location changed, we have to update the contact too
	if (NULL != buddyAvatarLoc) {
		m_buddyAvatarLoc.assign(buddyAvatarLoc);

		// construct the DB query - find by username and service name
		MojDbQuery contactsQuery;
		err = createContactQuery(username, serviceName, contactsQuery);
		if (err) {
			MojString error;
			MojErrToString(err, error);
			MojLogError(IMServiceApp::s_log, _T("createContactQuery failed: error %d - %s"), err, error.data());
			return err;
		}

		// first we want to see if this is a change from the current contact photo because contact linker will get launched any time any field in the contact is saved, which impacts performance
		// and battery life.
		err = m_dbClient.find(this->m_findContactSlot, contactsQuery, /* watch */ false );
		if (err) {
			MojString error;
			MojErrToString(err, error);
			MojLogError(IMServiceApp::s_log, _T("updateBuddyStatus find failed: error %d - %s"), err, error.data());
		}
	}

	return MojErrNone;
}

// Perf (#2): batched presence update for one account. One find(byAccountId) then ONE batched merge
// (existing rows, by _id) + ONE batched put (new rows). Replaces a find+merge PER buddy during a
// burst - on-device A/B measured ~32x fewer luna->db8 round-trips (23s -> 0.7s for 50 rows).
MojErr BuddyStatusHandler::updateBuddyStatusBatch(const char* accountId, const char* serviceName, MojObject& updates)
{
	MojErr err = m_batchAccountId.assign(accountId ? accountId : "");
	MojErrCheck(err);
	err = m_batchServiceName.assign(serviceName ? serviceName : "");
	MojErrCheck(err);
	m_batchUpdates = updates;

	MojDbQuery query;
	err = query.from(IM_BUDDYSTATUS_KIND);
	MojErrCheck(err);
	err = query.where(MOJDB_BUDDYSTATUS_ACCOUNTID, MojDbQuery::OpEq, m_batchAccountId);
	MojErrCheck(err);
	err = m_tempdbClient.find(m_batchFindSlot, query, /* watch */ false);
	MojErrCheck(err);
	return MojErrNone;
}

MojErr BuddyStatusHandler::batchFindResult(MojObject& result, MojErr findErr)
{
	if (findErr != MojErrNone)
	{
		MojLogError(IMServiceApp::s_log, _T("batchFindResult: find failed %d"), findErr);
		return MojErrNone;
	}

	// existing username -> _id
	std::map<std::string, MojObject> existing;
	MojObject results;
	result.get(_T("results"), results);
	for (MojObject::ConstArrayIterator it = results.arrayBegin(); it != results.arrayEnd(); ++it)
	{
		MojString uname;
		MojObject id;
		bool f = false;
		it->get(MOJDB_BUDDYSTATUS_BUDDYNAME, uname, f);
		it->get(_T("_id"), id);
		if (!uname.empty())
			existing[std::string(uname.data())] = id;
	}

	MojObject::ObjectVec mergeArr, putArr;
	for (MojObject::ConstArrayIterator uIt = m_batchUpdates.arrayBegin(); uIt != m_batchUpdates.arrayEnd(); ++uIt)
	{
		MojString uname, status, group;
		MojInt64 avail = 0;
		bool f = false;
		uIt->get(MOJDB_BUDDYSTATUS_BUDDYNAME, uname, f);
		uIt->get(MOJDB_STATUS, status, f);
		uIt->get(MOJDB_GROUP, group, f);
		uIt->get(MOJDB_AVAILABILITY, avail);
		if (uname.empty())
			continue;

		std::map<std::string, MojObject>::iterator e = existing.find(std::string(uname.data()));
		if (e != existing.end())
		{
			// existing -> merge in place by _id
			MojObject m;
			MojErrCheck(m.put(_T("_id"), e->second));
			MojErrCheck(m.putString(MOJDB_STATUS, status));
			MojErrCheck(m.putInt(MOJDB_AVAILABILITY, avail));
			MojErrCheck(m.putString(MOJDB_GROUP, group));
			MojErrCheck(mergeArr.push(m));
		}
		else
		{
			// new -> put
			MojObject b;
			MojErrCheck(b.putString(_T("_kind"), IM_BUDDYSTATUS_KIND));
			MojErrCheck(b.putString(MOJDB_BUDDYSTATUS_ACCOUNTID, m_batchAccountId));
			MojErrCheck(b.putString(MOJDB_SERVICE_NAME, m_batchServiceName));
			MojErrCheck(b.putString(MOJDB_BUDDYSTATUS_BUDDYNAME, uname));
			MojErrCheck(b.putString(MOJDB_STATUS, status));
			MojErrCheck(b.putInt(MOJDB_AVAILABILITY, avail));
			MojErrCheck(b.putString(MOJDB_GROUP, group));
			MojErrCheck(putArr.push(b));
		}
	}

	if (!mergeArr.empty())
	{
		MojErr err = m_tempdbClient.merge(m_batchMergeSlot, mergeArr.begin(), mergeArr.end());
		MojErrCheck(err);
	}
	if (!putArr.empty())
	{
		MojErr err = m_tempdbClient.put(m_batchPutSlot, putArr.begin(), putArr.end());
		MojErrCheck(err);
	}
	MojLogInfo(IMServiceApp::s_log, _T("updateBuddyStatusBatch: %s merged %d, put %d (1 find)"),
			m_batchServiceName.data(), (int)mergeArr.size(), (int)putArr.size());
	return MojErrNone;
}

MojErr BuddyStatusHandler::batchMergeResult(MojObject& result, MojErr err)
{
	if (err != MojErrNone)
		MojLogError(IMServiceApp::s_log, _T("batchMergeResult: merge failed %d"), err);
	return MojErrNone;
}

MojErr BuddyStatusHandler::batchPutResult(MojObject& result, MojErr err)
{
	if (err != MojErrNone)
		MojLogError(IMServiceApp::s_log, _T("batchPutResult: put failed %d"), err);
	return MojErrNone;
}

MojErr BuddyStatusHandler::findContactResult(MojObject& result, MojErr findErr)
{
	MojLogTrace(IMServiceApp::s_log);

	if (findErr) {

		MojString error;
		MojErrToString(findErr, error);
		MojLogError(IMServiceApp::s_log, _T("find existing contact failed: error %d - %s"), findErr, error.data());

	} else {

		// get the results
		MojString mojStringJson;
		result.toJson(mojStringJson);
		MojLogInfo(IMServiceApp::s_log, _T("findContactResult result: %s"), mojStringJson.data());

		MojObject results;

		// get results array in "results"
		result.get(_T("results"), results);

		// we should always get a result here
		if (!results.empty()) {
			// get the contact object
			MojObject contact;
			MojObject::ConstArrayIterator itr = results.arrayBegin();
			bool foundOne = false;
			while (itr != results.arrayEnd()) {
				if (foundOne) {
					MojLogError(IMServiceApp::s_log,
							_T("findContactResult: found more than one contact with same username/serviceName - using the first one"));
					break;
				}
				contact = *itr;
				foundOne = true;
				itr++;
			}

			// see if there is an existing avatar and if it matches
			/*"photos": [
				{
					"localPath": "/var/luna/data/im-avatars/2e9cd3fdce4419f58128ad6854543fdb58a2137b.png",
					"value": "/var/luna/data/im-avatars/2e9cd3fdce4419f58128ad6854543fdb58a2137b.png",
					"type": "type_square"
				}
			]
			*/
			MojObject photoArray, photo;
			MojErr err;
			bool found = contact.get("photos", photoArray);
			if (found && !photoArray.empty()) {
				// Note: libPurple doesn't support multiple avatars for a buddy - we only get one
				MojObject::ConstArrayIterator photoItr = photoArray.arrayBegin();
				photo = *photoItr;
				MojString path;
				err = photo.get("localPath", path, found);
				if (!err && found) {
					// see if the old path is the same as the new one
					MojLogInfo(IMServiceApp::s_log, _T("findContactResult: contact photo %s. buddy avatar %s"), path.data(), m_buddyAvatarLoc.data());
					if (0 == path.compare(m_buddyAvatarLoc.data())) {
						MojLogInfo(IMServiceApp::s_log, _T("findContactResult: contact photo did not change. No need to re-save"));
						return MojErrNone;
					}
				}
			}

			// Path changed - need to save
			MojString dbId;
			err = contact.getRequired(MOJDB_ID, dbId);
			if (err) {
				MojLogError(IMServiceApp::s_log, _T("findContactResult: contact had no dbId"));
				return err;
			}
			// create merge property
			MojObject newPhotosArray, newPhotoObj;
			newPhotoObj.put("localPath", m_buddyAvatarLoc);
			newPhotoObj.put("value", m_buddyAvatarLoc);
			newPhotoObj.putString("type", "type_square"); // AIM and GTalk generally send small, square-ish images
			newPhotosArray.push(newPhotoObj);
			MojObject contactMergeProps;
			contactMergeProps.put("photos", newPhotosArray);
			contactMergeProps.putString(MOJDB_ID, dbId);

			// log it
			MojString json;
			contactMergeProps.toJson(json);
			MojLogInfo(IMServiceApp::s_log, _T("saving contact to db: %s"), json.data());

			// save the new fields - call merge
			err = m_dbClient.merge(m_saveContactSlot, contactMergeProps);
			if (err) {
				MojString error;
				MojErrToString(err, error);
				MojLogError(IMServiceApp::s_log, _T("dbClient merge contact failed: error %d - %s"), err, error.data());
				return err;
			}
		}
		else {
			MojLogError(IMServiceApp::s_log, _T("findContactResult: no matching contact record found for %s"), m_serviceName.data());
		}
	}
	return MojErrNone;
}


/*
 * Receive a request for authorization from another user to be a buddy.
 *
 * Create an imcommand for the application to ask user to accept or reject the invite.
 */
MojErr BuddyStatusHandler::receivedBuddyInvite(const char* serviceName, const char* username, const char* usernameFrom, const char* customMessage)
{
	// remember the fields to create the command
	m_username.assign(username);
	m_serviceName.assign(serviceName);
	m_fromUsername.assign(usernameFrom);

	// first we have to query to see if this command already exists since we get notified of pending invites every time a user logs in
	MojDbQuery query;
	MojErr err;
	err = createCommandQuery(query);
	if (err) {
		MojString error;
		MojErrToString(err, error);
		MojLogError(IMServiceApp::s_log, _T("createCommandQuery failed: error %d - %s"), err, error.data());
		return err;
	}

	// query DB
	err = m_dbClient.find(this->m_findCommandSlot, query, /* watch */ false );
	if (err) {
		MojString error;
		MojErrToString(err, error);
		MojLogError(IMServiceApp::s_log, _T("receivedBuddyInvite find failed: error %d - %s"), err, error.data());
	}

	return MojErrNone;
}


/*
 * Create the query to find the buddy by username and accountId
 *
 * errors returned to caller
 */
MojErr BuddyStatusHandler::createBuddyQuery(const char* username, const char* accountId, MojDbQuery& query)
{

	MojString user;
	user.assign(username);
	MojErr err = query.where(MOJDB_BUDDYSTATUS_BUDDYNAME, MojDbQuery::OpEq, user);
	MojErrCheck(err);

	MojString account;
	account.assign(accountId);
	err = query.where(MOJDB_BUDDYSTATUS_ACCOUNTID, MojDbQuery::OpEq, account);
	MojErrCheck(err);

	//add our kind to the object
	err = query.from(IM_BUDDYSTATUS_KIND);
	MojErrCheck(err);

	// log the query
	MojObject queryObject;
	err = query.toObject(queryObject);
	MojErrCheck(err);
	IMServiceHandler::logMojObjectJsonString(_T("createBuddyQuery: %s"), queryObject);

	return MojErrNone;
}

/*
 * Create the query to find the buddy by username and accountId
 *
 * errors returned to caller
 */
MojErr BuddyStatusHandler::createContactQuery(const char* username, const char* serviceName, MojDbQuery& query)
{

	MojString user;
	user.assign(username);
	MojErr err = query.where("ims.value", MojDbQuery::OpEq, user);
	MojErrCheck(err);

	MojString service;
	service.assign(serviceName);
	err = query.where("ims.type", MojDbQuery::OpEq, service);
	MojErrCheck(err);

	// add our kind to the object
	err = query.from(IM_CONTACT_KIND);
	MojErrCheck(err);

	// log the query
	MojObject queryObject;
	err = query.toObject(queryObject);
	MojErrCheck(err);
	IMServiceHandler::logMojObjectJsonString(_T("createContactQuery: %s"), queryObject);

	return MojErrNone;
}

/*
 * Create the query to find an existing imcommand by username, service name and from username
 *
 * errors returned to caller
 */
MojErr BuddyStatusHandler::createCommandQuery(MojDbQuery& query)
{

	MojErr err = query.where(MOJODB_TARGET_USER, MojDbQuery::OpEq, m_username);
	MojErrCheck(err);

	err = query.where(MOJDB_SERVICE_NAME, MojDbQuery::OpEq, m_serviceName);
	MojErrCheck(err);

	err = query.where(MOJDB_FROM_USER, MojDbQuery::OpEq, m_fromUsername);
	MojErrCheck(err);

	// add our kind to the object
	err = query.from(IM_IMCOMMAND_KIND);
	MojErrCheck(err);

	// log the query
	MojObject queryObject;
	err = query.toObject(queryObject);
	MojErrCheck(err);
	IMServiceHandler::logMojObjectJsonString(_T("createCommandQuery: %s"), queryObject);

	return MojErrNone;
}

/**
 * callback from the save buddy status call.
 */
MojErr BuddyStatusHandler::saveStatusResult(MojObject& result, MojErr saveErr)
{
	MojLogTrace(IMServiceApp::s_log);

	if (saveErr) {
		MojString error;
		MojErrToString(saveErr, error);
		MojLogError(IMServiceApp::s_log, _T("saveStatusResult: database merge failed. error %d - %s"), saveErr, error.data());
	}
	else {
		IMServiceHandler::logMojObjectJsonString(_T("saveStatusResult: database merge success: %s"), result);
	}

	return MojErrNone;
}


/**
 * callback from the save contact call.
 */
MojErr BuddyStatusHandler::saveContactResult(MojObject& result, MojErr saveErr)
{
	MojLogTrace(IMServiceApp::s_log);

	if (saveErr) {
		MojString error;
		MojErrToString(saveErr, error);
		MojLogError(IMServiceApp::s_log, _T("saveContactResult: database merge failed. error %d - %s"), saveErr, error.data());
	}
	else {
		IMServiceHandler::logMojObjectJsonString(_T("saveContactResult: database merge success: %s"), result);
	}

	return MojErrNone;
}

/*
 * Callback for the query for existing invite command from this user
 */
MojErr BuddyStatusHandler::findCommandResult(MojObject& result, MojErr findErr)
{
	MojLogTrace(IMServiceApp::s_log);

	if (findErr) {

		MojString error;
		MojErrToString(findErr, error);
		MojLogError(IMServiceApp::s_log, _T("find existing imcommand failed: error %d - %s"), findErr, error.data());

	} else {

		// get the results
		MojString mojStringJson;
		result.toJson(mojStringJson);
		MojLogInfo(IMServiceApp::s_log, _T("findCommandResult result: %s"), mojStringJson.data());

		MojObject results;

		// get results array in "results"
		result.get(_T("results"), results);

		// if the command already exists, there is nothing to do
		if (results.empty()) {

			// save it
			MojObject dbObject;
			MojErr err = createCommandObject(dbObject);
			if (!err) {
				err = m_dbClient.put(this->m_saveCommandSlot, dbObject);
				if (err) {
					MojString error;
					MojErrToString(err, error);
					MojLogError(IMServiceApp::s_log, _T("findCommandResult - db put failed: error %d - %s"), err, error.data());
					return err;
				}
			}
			else {
				MojString error;
				MojErrToString(err, error);
				MojLogError(IMServiceApp::s_log, _T("findCommandResult - put failed: error %d - %s"), err, error.data());
				return err;
			}
		}
	}
	return MojErrNone;
}

/**
 * callback from the save buddy status call.
 */
MojErr BuddyStatusHandler::saveCommandResult(MojObject& result, MojErr saveErr)
{
	MojLogTrace(IMServiceApp::s_log);

	if (saveErr) {
		MojString error;
		MojErrToString(saveErr, error);
		MojLogError(IMServiceApp::s_log, _T("saveCommandResult: database merge failed. error %d - %s"), saveErr, error.data());
	}
	else {
		IMServiceHandler::logMojObjectJsonString(_T("saveCommandResult: database merge success: %s"), result);
	}

	return MojErrNone;
}

/**
 * Return imCommand Object formated to save to MojDb
 * command:
 * {"_kind":"com.palm.imcommand.libpurple:1","command":"receivedBuddyInvite", "serviceName":"type_gtalk", "targetUsername":"palm.gmail.com", "handler":"application", "fromUsername":"palm1@gmail.com"}
 *
 * Used for incoming messages
 */
MojErr BuddyStatusHandler::createCommandObject(MojObject& returnObj) {

	// service name
	MojErr err = returnObj.putString(MOJDB_SERVICE_NAME , m_serviceName);
	MojErrCheck(err);

	// target user name
	err = returnObj.putString(MOJODB_TARGET_USER, m_username);
	MojErrCheck(err);

	// from user name
	err = returnObj.putString(MOJDB_FROM_USER, m_fromUsername);
	MojErrCheck(err);

	// handler
	err = returnObj.putString(MOJDB_HANDLER, _T("application"));
	MojErrCheck(err);

	// command
	err = returnObj.putString(MOJDB_COMMAND, _T("receivedBuddyInvite"));
	MojErrCheck(err);

	// add our kind to the object
	err = returnObj.putString(_T("_kind"), IM_IMCOMMAND_KIND);
	MojErrCheck(err);

	// log it
	MojString json;
	err = returnObj.toJson(json);
	MojLogInfo(IMServiceApp::s_log, _T("saving command to db: %s"), json.data());

	return err;
}


