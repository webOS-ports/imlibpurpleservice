/*
 * EditHandler.cpp  - see EditHandler.h
 */
#include "EditHandler.h"
#include "IMMessage.h"          // PALM_DB_IMMESSAGE_KIND, MOJDB_* field names
#include "db/MojDbQuery.h"
#include "IMServiceApp.h"

EditHandler::EditHandler(MojService* service, IMServiceApp::Listener* listener)
: m_findSlot(this, &EditHandler::findResult),
  m_mergeSlot(this, &EditHandler::mergeResult),
  m_service(service),
  m_listener(listener),
  m_dbClient(service)
{
}

EditHandler::~EditHandler()
{
}

MojErr EditHandler::handleEdit(const char* serviceName, const char* username,
		const char* serviceMessageId, const char* newText)
{
	MojErr err;
	if (serviceName == NULL || username == NULL || serviceMessageId == NULL || *serviceMessageId == '\0') {
		MojLogError(IMServiceApp::s_log, _T("EditHandler: missing serviceName/username/serviceMessageId"));
		return MojErrInvalidArg;
	}
	err = m_serviceName.assign(serviceName);              MojErrCheck(err);
	err = m_username.assign(username);                    MojErrCheck(err);
	err = m_serviceMessageId.assign(serviceMessageId);    MojErrCheck(err);
	err = m_newText.assign(newText ? newText : "");       MojErrCheck(err);

	MojLogInfo(IMServiceApp::s_log, _T("EditHandler: applying edit to %s message serviceMessageId %s"),
			m_serviceName.data(), m_serviceMessageId.data());

	// Find the exact message by (serviceName, username, serviceMessageId) - the same 3-prop index
	// ReceiptHandler uses for by-id lookups. There is at most one match (limit is a safety cap).
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

MojErr EditHandler::findResult(MojObject& result, MojErr err)
{
	if (err) {
		MojString e; MojErrToString(err, e);
		MojLogError(IMServiceApp::s_log, _T("EditHandler::findResult: db find failed: %d - %s"), err, e.data());
		return MojErrNone; // never fatal
	}

	MojObject results;
	result.get(_T("results"), results);

	MojObject::ConstArrayIterator itr = results.arrayBegin();
	if (itr == results.arrayEnd()) {
		// The edited message isn't on the device (never received / already pruned). Nothing to update.
		MojLogInfo(IMServiceApp::s_log, _T("EditHandler::findResult: no message with serviceMessageId %s to edit"),
				m_serviceMessageId.data());
		return MojErrNone;
	}

	MojObject row = *itr;
	MojString dbId;
	if (row.getRequired(MOJDB_ID, dbId) != MojErrNone) {
		MojLogError(IMServiceApp::s_log, _T("EditHandler::findResult: matched row has no _id"));
		return MojErrNone;
	}

	MojObject mergeProps;
	err = mergeProps.putString(MOJDB_MSG_TEXT, m_newText); MojErrCheck(err);
	err = mergeProps.putString(MOJDB_ID, dbId);           MojErrCheck(err);
	err = m_dbClient.merge(m_mergeSlot, mergeProps);
	MojErrCheck(err);
	return MojErrNone;
}

MojErr EditHandler::mergeResult(MojObject& result, MojErr err)
{
	MojUnused(result);
	if (err) {
		MojString e; MojErrToString(err, e);
		MojLogError(IMServiceApp::s_log, _T("EditHandler::mergeResult: merge failed: %d - %s"), err, e.data());
		return MojErrNone;
	}
	MojLogInfo(IMServiceApp::s_log, _T("EditHandler::mergeResult: edited message serviceMessageId %s in place"),
			m_serviceMessageId.data());
	return MojErrNone;
}
