/*
 * ReceiptHandler.cpp - see ReceiptHandler.h
 */
#include "ReceiptHandler.h"
#include "IMMessage.h"          // PALM_DB_IMMESSAGE_KIND, MOJDB_* field names
#include "db/MojDbQuery.h"
#include "IMServiceApp.h"
#include <stdlib.h>             // strtoll
#include <string.h>             // strncmp

ReceiptHandler::ReceiptHandler(MojService* service, IMServiceApp::Listener* listener)
: m_findSlot(this, &ReceiptHandler::findResult),
  m_mergeSlot(this, &ReceiptHandler::mergeResult),
  m_watermarkMode(false),
  m_service(service),
  m_listener(listener),
  m_dbClient(service)
{
}

ReceiptHandler::~ReceiptHandler()
{
}

int ReceiptHandler::statusRank(const char* s)
{
	if (s == NULL) return 0;
	if (strcmp(s, MOJDB_DELIVERY_READ) == 0) return 2;
	if (strcmp(s, MOJDB_DELIVERY_DELIVERED) == 0) return 1;
	return 0;
}

int ReceiptHandler::statusRank(const MojString& s)
{
	return statusRank(s.data());
}

MojErr ReceiptHandler::handleReceiptById(const char* serviceName, const char* username,
		const char* serviceMessageId, const char* status)
{
	MojErr err;
	if (serviceName == NULL || username == NULL || serviceMessageId == NULL || *serviceMessageId == '\0' || status == NULL) {
		MojLogError(IMServiceApp::s_log, _T("ReceiptHandler: missing by-id args"));
		return MojErrInvalidArg;
	}
	m_watermarkMode = false;
	err = m_serviceName.assign(serviceName);            MojErrCheck(err);
	err = m_username.assign(username);                  MojErrCheck(err);
	err = m_serviceMessageId.assign(serviceMessageId);  MojErrCheck(err);
	err = m_status.assign(status);                      MojErrCheck(err);

	MojLogInfo(IMServiceApp::s_log, _T("ReceiptHandler: %s receipt for %s message id %s"),
			m_status.data(), m_serviceName.data(), m_serviceMessageId.data());

	// (serviceName, username, serviceMessageId) index -> exactly the target row(s).
	MojDbQuery query;
	err = query.from(PALM_DB_IMMESSAGE_KIND);                                     MojErrCheck(err);
	err = query.where(MOJDB_SERVICENAME, MojDbQuery::OpEq, m_serviceName);        MojErrCheck(err);
	err = query.where(MOJDB_USERNAME, MojDbQuery::OpEq, m_username);              MojErrCheck(err);
	err = query.where(MOJDB_SERVICE_MSG_ID, MojDbQuery::OpEq, m_serviceMessageId);MojErrCheck(err);
	query.limit(4);
	err = m_dbClient.find(m_findSlot, query, /* watch */ false);
	MojErrCheck(err);
	return MojErrNone;
}

MojErr ReceiptHandler::handleReceiptWatermark(const char* serviceName, const char* username,
		const char* scope, const char* watermark, const char* status)
{
	MojErr err;
	if (serviceName == NULL || username == NULL || scope == NULL || *scope == '\0' ||
	    watermark == NULL || *watermark == '\0' || status == NULL) {
		MojLogError(IMServiceApp::s_log, _T("ReceiptHandler: missing watermark args"));
		return MojErrInvalidArg;
	}
	m_watermarkMode = true;
	err = m_serviceName.assign(serviceName);  MojErrCheck(err);
	err = m_username.assign(username);        MojErrCheck(err);
	err = m_scope.assign(scope);              MojErrCheck(err);
	err = m_watermark.assign(watermark);      MojErrCheck(err);
	err = m_status.assign(status);            MojErrCheck(err);

	MojLogInfo(IMServiceApp::s_log, _T("ReceiptHandler: %s watermark %s scope %s (%s)"),
			m_status.data(), m_watermark.data(), m_scope.data(), m_serviceName.data());

	// (serviceName, username) index -> this account's messages; filter to outbox + scope in findResult.
	MojDbQuery query;
	err = query.from(PALM_DB_IMMESSAGE_KIND);                              MojErrCheck(err);
	err = query.where(MOJDB_SERVICENAME, MojDbQuery::OpEq, m_serviceName); MojErrCheck(err);
	err = query.where(MOJDB_USERNAME, MojDbQuery::OpEq, m_username);       MojErrCheck(err);
	query.limit(500);
	err = m_dbClient.find(m_findSlot, query, /* watch */ false);
	MojErrCheck(err);
	return MojErrNone;
}

MojErr ReceiptHandler::findResult(MojObject& result, MojErr err)
{
	if (err) {
		MojString e; MojErrToString(err, e);
		MojLogError(IMServiceApp::s_log, _T("ReceiptHandler::findResult: db find failed: %d - %s"), err, e.data());
		return MojErrNone; // never fatal
	}

	MojObject results;
	result.get(_T("results"), results);
	const int newRank = statusRank(m_status);

	MojString outbox; outbox.assign(IMMessage::folderStrings[Outbox]);

	// ---- BY-ID: upgrade the single matching row -------------------------------------------------
	if (!m_watermarkMode) {
		MojObject::ConstArrayIterator itr = results.arrayBegin();
		while (itr != results.arrayEnd()) {
			MojObject row = *itr; itr++;
			MojString existing; bool has = false;
			row.get(MOJDB_DELIVERY_STATUS, existing, has);
			if (has && statusRank(existing) >= newRank) return MojErrNone; // already >= -> nothing to do

			MojString dbId;
			if (row.getRequired(MOJDB_ID, dbId) != MojErrNone) continue;
			MojObject mergeProps;
			err = mergeProps.putString(MOJDB_DELIVERY_STATUS, m_status); MojErrCheck(err);
			err = mergeProps.putString(MOJDB_ID, dbId);                  MojErrCheck(err);
			err = m_dbClient.merge(m_mergeSlot, mergeProps);             MojErrCheck(err);
			return MojErrNone;
		}
		return MojErrNone; // no matching row (receipt for a message we don't have)
	}

	// ---- WATERMARK: upgrade every outbox row at/under the boundary -------------------------------
	// Parse scope = "<mode>:<arg>".
	const char* scope = m_scope.data();
	const char* colon = strchr(scope, ':');
	if (colon == NULL) return MojErrNone;
	MojString modeS; modeS.assign(scope, (MojSize)(colon - scope));
	const char* arg = colon + 1;   // prefix (msgid) or peer addr (id/ts)
	const bool byMsgid = (strcmp(modeS.data(), "msgid") == 0);
	const bool byId    = (strcmp(modeS.data(), "id") == 0);
	const bool byTs    = (strcmp(modeS.data(), "ts") == 0);
	const MojInt64 wm  = (MojInt64) strtoll(m_watermark.data(), NULL, 10);

	MojObject toMerge; // array of {_id, deliveryStatus}
	int matched = 0;
	MojObject::ConstArrayIterator itr = results.arrayBegin();
	while (itr != results.arrayEnd()) {
		MojObject row = *itr; itr++;

		MojString folder; bool f = false;
		row.get(MOJDB_FOLDER, folder, f);
		if (folder != outbox) continue;

		// already at/above target status?
		MojString existing; bool hasStatus = false;
		row.get(MOJDB_DELIVERY_STATUS, existing, hasStatus);
		if (hasStatus && statusRank(existing) >= newRank) continue;

		bool match = false;
		if (byMsgid) {
			// serviceMessageId starts with <prefix> ("<chatId>:"); compare numeric tail after last ':'
			MojString smid; bool hasS = false;
			row.get(MOJDB_SERVICE_MSG_ID, smid, hasS);
			if (hasS && strncmp(smid.data(), arg, strlen(arg)) == 0) {
				const char* tail = strrchr(smid.data(), ':');
				tail = tail ? tail + 1 : smid.data();
				if ((MojInt64) strtoll(tail, NULL, 10) <= wm) match = true;
			}
		} else if (byId || byTs) {
			// scope by peer address (any to.addr == arg)
			MojObject toArr;
			bool hasTo = row.get(MOJDB_TO, toArr);
			bool peerOk = false;
			if (hasTo) {
				MojObject::ConstArrayIterator ti = toArr.arrayBegin();
				while (ti != toArr.arrayEnd()) {
					MojObject a = *ti; ti++;
					MojString addr; bool ha = false;
					a.get(MOJDB_ADDRESS, addr, ha);
					if (ha && strcmp(addr.data(), arg) == 0) { peerOk = true; break; }
				}
			}
			if (peerOk) {
				if (byId) {
					MojString smid; bool hasS = false;
					row.get(MOJDB_SERVICE_MSG_ID, smid, hasS);
					if (hasS && (MojInt64) strtoll(smid.data(), NULL, 10) <= wm) match = true;
				} else { // byTs: compare the row's server timestamp
					MojObject tsObj;
					if (row.get(MOJDB_SERVER_TIMESTAMP, tsObj) && tsObj.intValue() <= wm) match = true;
				}
			}
		}
		if (!match) continue;

		MojString dbId;
		if (row.getRequired(MOJDB_ID, dbId) != MojErrNone) continue;
		MojObject one;
		err = one.putString(MOJDB_DELIVERY_STATUS, m_status); MojErrCheck(err);
		err = one.putString(MOJDB_ID, dbId);                  MojErrCheck(err);
		err = toMerge.push(one);                              MojErrCheck(err);
		matched++;
	}

	if (matched == 0) {
		MojLogInfo(IMServiceApp::s_log, _T("ReceiptHandler::findResult: watermark matched no outbox rows"));
		return MojErrNone;
	}
	err = m_dbClient.merge(m_mergeSlot, toMerge.arrayBegin(), toMerge.arrayEnd());
	MojErrCheck(err);
	return MojErrNone;
}

MojErr ReceiptHandler::mergeResult(MojObject& result, MojErr err)
{
	MojUnused(result);
	if (err) {
		MojString e; MojErrToString(err, e);
		MojLogError(IMServiceApp::s_log, _T("ReceiptHandler::mergeResult: merge failed: %d - %s"), err, e.data());
		return MojErrNone;
	}
	MojLogInfo(IMServiceApp::s_log, _T("ReceiptHandler::mergeResult: applied %s receipt"), m_status.data());
	return MojErrNone;
}
