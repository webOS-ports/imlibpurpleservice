/*
 * OutboxIdHandler.cpp  - see OutboxIdHandler.h
 */
#include "OutboxIdHandler.h"
#include "IMMessage.h"          // PALM_DB_IMMESSAGE_KIND, MOJDB_* field names
#include "db/MojDbQuery.h"
#include "IMServiceApp.h"

OutboxIdHandler::OutboxIdHandler(MojService* service, IMServiceApp::Listener* listener)
: m_findSlot(this, &OutboxIdHandler::findResult),
  m_mergeSlot(this, &OutboxIdHandler::mergeResult),
  m_service(service),
  m_listener(listener),
  m_dbClient(service)
{
}

OutboxIdHandler::~OutboxIdHandler()
{
}

MojErr OutboxIdHandler::handleOutboxId(const char* serviceName, const char* username,
		const char* serviceMessageId, const char* text)
{
	MojErr err;
	if (serviceName == NULL || username == NULL || serviceMessageId == NULL || *serviceMessageId == '\0') {
		MojLogError(IMServiceApp::s_log, _T("OutboxIdHandler: missing serviceName/username/serviceMessageId"));
		return MojErrInvalidArg;
	}
	err = m_serviceName.assign(serviceName);              MojErrCheck(err);
	err = m_username.assign(username);                    MojErrCheck(err);
	err = m_serviceMessageId.assign(serviceMessageId);    MojErrCheck(err);
	err = m_text.assign(text ? text : "");                MojErrCheck(err);

	MojLogInfo(IMServiceApp::s_log, _T("OutboxIdHandler: attaching serviceMessageId %s to a sent %s message"),
			m_serviceMessageId.data(), m_serviceName.data());

	// Find recent SENT outbox rows (outgoingMsg index: folder,status,localTimestamp), most recent first.
	// Outbox rows share a sentinel localTimestamp, so desc effectively orders by insertion (_id) - the
	// just-sent message is at/near the top. We scope to the account + pick the target in findResult.
	MojDbQuery query;
	MojString outbox;  outbox.assign(IMMessage::folderStrings[Outbox]);
	MojString sentOk;  sentOk.assign(IMMessage::statusStrings[Successful]);
	err = query.from(PALM_DB_IMMESSAGE_KIND);                          MojErrCheck(err);
	err = query.where(MOJDB_FOLDER, MojDbQuery::OpEq, outbox);         MojErrCheck(err);
	err = query.where(MOJDB_STATUS, MojDbQuery::OpEq, sentOk);         MojErrCheck(err);
	err = query.order(MOJDB_DEVICE_TIMESTAMP);                         MojErrCheck(err);
	query.desc(true);
	query.limit(25);

	err = m_dbClient.find(m_findSlot, query, /* watch */ false);
	MojErrCheck(err);
	return MojErrNone;
}

MojErr OutboxIdHandler::findResult(MojObject& result, MojErr err)
{
	if (err) {
		MojString e; MojErrToString(err, e);
		MojLogError(IMServiceApp::s_log, _T("OutboxIdHandler::findResult: db find failed: %d - %s"), err, e.data());
		return MojErrNone; // never fatal
	}

	MojObject results;
	result.get(_T("results"), results);

	// Among this account's recent outbox rows that have no serviceMessageId yet, prefer one whose
	// messageText matches the sent text (disambiguates interleaved sends); else take the most recent
	// (results are desc, so the first such candidate is the most recent).
	MojString mostRecentUnset;   bool haveMostRecent = false;
	MojString textMatchId;       bool haveTextMatch  = false;

	MojObject::ConstArrayIterator itr = results.arrayBegin();
	while (itr != results.arrayEnd()) {
		MojObject row = *itr;
		itr++;

		MojString svc, user;
		bool f = false;
		row.get(MOJDB_SERVICENAME, svc, f);
		if (svc != m_serviceName) continue;
		row.get(MOJDB_USERNAME, user, f);
		if (user != m_username) continue;

		// skip rows that already carry a serviceMessageId (phone carbons / already-attached)
		MojString existing;
		bool hasId = false;
		row.get(MOJDB_SERVICE_MSG_ID, existing, hasId);
		if (hasId && !existing.empty()) continue;

		MojString dbId;
		if (row.getRequired(MOJDB_ID, dbId) != MojErrNone) continue;

		if (!haveMostRecent) { mostRecentUnset = dbId; haveMostRecent = true; }

		if (!haveTextMatch && !m_text.empty()) {
			MojString mt;
			bool hasText = false;
			row.get(MOJDB_MSG_TEXT, mt, hasText);
			if (hasText && mt == m_text) { textMatchId = dbId; haveTextMatch = true; break; }
		}
	}

	if (!haveMostRecent && !haveTextMatch) {
		MojLogInfo(IMServiceApp::s_log, _T("OutboxIdHandler::findResult: no unlabeled outbox row to attach %s to"),
				m_serviceMessageId.data());
		return MojErrNone;
	}

	MojString targetId = haveTextMatch ? textMatchId : mostRecentUnset;
	MojObject mergeProps;
	err = mergeProps.putString(MOJDB_SERVICE_MSG_ID, m_serviceMessageId); MojErrCheck(err);
	err = mergeProps.putString(MOJDB_ID, targetId);                      MojErrCheck(err);
	err = m_dbClient.merge(m_mergeSlot, mergeProps);
	MojErrCheck(err);
	return MojErrNone;
}

MojErr OutboxIdHandler::mergeResult(MojObject& result, MojErr err)
{
	MojUnused(result);
	if (err) {
		MojString e; MojErrToString(err, e);
		MojLogError(IMServiceApp::s_log, _T("OutboxIdHandler::mergeResult: merge failed: %d - %s"), err, e.data());
		return MojErrNone;
	}
	MojLogInfo(IMServiceApp::s_log, _T("OutboxIdHandler::mergeResult: attached serviceMessageId %s to sent message"),
			m_serviceMessageId.data());
	return MojErrNone;
}
