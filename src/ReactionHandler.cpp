/*
 * ReactionHandler.cpp  - see ReactionHandler.h
 */
#include "ReactionHandler.h"
#include "IMMessage.h"          // PALM_DB_IMMESSAGE_KIND, MOJDB_* field names
#include "db/MojDbQuery.h"
#include "IMServiceApp.h"
#include "sanitize.h"           // encodeAstralEntities - same emoji-safe encoding as message text
#include <string.h>             // strchr/memchr/strlen for parsing the serialized reaction set
#include <stdlib.h>             // free

ReactionHandler::ReactionHandler(MojService* service, IMServiceApp::Listener* listener)
: m_findSlot(this, &ReactionHandler::findResult),
  m_mergeSlot(this, &ReactionHandler::mergeResult),
  m_service(service),
  m_listener(listener),
  m_dbClient(service),
  m_replace(false),
  m_setReactions(MojObject::TypeArray)
{
}

ReactionHandler::~ReactionHandler()
{
}

MojErr ReactionHandler::handleReaction(const char* serviceName, const char* username, const char* targetId,
		const char* emoji, const char* sender)
{
	MojErr err;
	if (serviceName == NULL || username == NULL || targetId == NULL || *targetId == '\0') {
		MojLogError(IMServiceApp::s_log, _T("ReactionHandler: missing serviceName/username/targetId"));
		return MojErrInvalidArg;
	}
	err = m_serviceName.assign(serviceName);          MojErrCheck(err);
	err = m_username.assign(username);                MojErrCheck(err);
	err = m_targetId.assign(targetId);                MojErrCheck(err);
	// Encode the emoji the SAME way message text is (astral code points -> &#NNNNN; entities) so it
	// survives the LS2/JS bridge to the app and emojify() renders it as an inline image. Without this
	// a BMP emoji (❤️) survives raw but astral ones (😂👍) get mangled and show as tofu squares.
	{
		char *safeEmoji = encodeAstralEntities(emoji ? emoji : "");
		err = m_emoji.assign(safeEmoji ? safeEmoji : "");
		free(safeEmoji);
		MojErrCheck(err);
	}
	err = m_sender.assign(sender ? sender : "");      MojErrCheck(err);

	MojLogInfo(IMServiceApp::s_log, _T("ReactionHandler: %s reaction '%s' from '%s' on message %s (%s)"),
			m_emoji.empty() ? _T("remove") : _T("add"), m_emoji.data(), m_sender.data(), m_targetId.data(), m_serviceName.data());

	// Find the target message row by its serviceMessageId, scoped to the owning account.
	MojDbQuery query;
	err = query.from(PALM_DB_IMMESSAGE_KIND);                              MojErrCheck(err);
	err = query.where(MOJDB_SERVICENAME, MojDbQuery::OpEq, m_serviceName); MojErrCheck(err);
	err = query.where(MOJDB_USERNAME, MojDbQuery::OpEq, m_username);       MojErrCheck(err);
	err = query.where(MOJDB_SERVICE_MSG_ID, MojDbQuery::OpEq, m_targetId); MojErrCheck(err);
	query.limit(1);

	err = m_dbClient.find(m_findSlot, query, /* watch */ false);
	MojErrCheck(err);
	return MojErrNone;
}

MojErr ReactionHandler::handleReactionSet(const char* serviceName, const char* username, const char* targetId,
		const char* serialized)
{
	MojErr err;
	if (serviceName == NULL || username == NULL || targetId == NULL || *targetId == '\0') {
		MojLogError(IMServiceApp::s_log, _T("ReactionHandler: missing serviceName/username/targetId (set)"));
		return MojErrInvalidArg;
	}
	err = m_serviceName.assign(serviceName);          MojErrCheck(err);
	err = m_username.assign(username);                MojErrCheck(err);
	err = m_targetId.assign(targetId);                MojErrCheck(err);
	m_replace = true;

	// Parse the "count<SP>emoji\n..." records into a fresh {emoji,count} array. Each emoji is encoded
	// the same way message text is (astral -> &#NNNNN;) so it survives to the app and emojify()s.
	m_setReactions.clear(MojObject::TypeArray);
	const char* p = serialized ? serialized : "";
	while (*p) {
		const char* nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		// record = "count SP emoji"; split on the first space.
		const char* sp = (const char*)memchr(p, ' ', len);
		if (sp) {
			MojInt64 count = 0;
			for (const char* d = p; d < sp; ++d) { if (*d >= '0' && *d <= '9') count = count * 10 + (*d - '0'); }
			const char* em = sp + 1;
			size_t emLen = (size_t)((p + len) - em);
			if (count > 0 && emLen > 0) {
				MojString rawEmoji;
				err = rawEmoji.assign(em, emLen);       MojErrCheck(err);
				char* safeEmoji = encodeAstralEntities(rawEmoji.data());
				MojObject r;
				err = r.putString(_T("emoji"), safeEmoji ? safeEmoji : rawEmoji.data());
				free(safeEmoji);
				MojErrCheck(err);
				err = r.putInt(_T("count"), count);     MojErrCheck(err);
				err = m_setReactions.push(r);           MojErrCheck(err);
			}
		}
		if (!nl) break;
		p = nl + 1;
	}

	MojLogInfo(IMServiceApp::s_log, _T("ReactionHandler: replace %zu reaction group(s) on message %s (%s)"),
			(size_t)m_setReactions.size(), m_targetId.data(), m_serviceName.data());

	MojDbQuery query;
	err = query.from(PALM_DB_IMMESSAGE_KIND);                              MojErrCheck(err);
	err = query.where(MOJDB_SERVICENAME, MojDbQuery::OpEq, m_serviceName); MojErrCheck(err);
	err = query.where(MOJDB_USERNAME, MojDbQuery::OpEq, m_username);       MojErrCheck(err);
	err = query.where(MOJDB_SERVICE_MSG_ID, MojDbQuery::OpEq, m_targetId); MojErrCheck(err);
	query.limit(1);

	err = m_dbClient.find(m_findSlot, query, /* watch */ false);
	MojErrCheck(err);
	return MojErrNone;
}

MojErr ReactionHandler::findResult(MojObject& result, MojErr err)
{
	if (err) {
		MojString e; MojErrToString(err, e);
		MojLogError(IMServiceApp::s_log, _T("ReactionHandler::findResult: db find failed: %d - %s"), err, e.data());
		return MojErrNone; // never fatal: a dropped reaction must not disturb the connection
	}

	MojObject results;
	result.get(_T("results"), results);
	if (results.empty()) {
		// The target message isn't in our db (older than history, or the prpl didn't store its id).
		MojLogInfo(IMServiceApp::s_log, _T("ReactionHandler::findResult: no message with serviceMessageId %s"), m_targetId.data());
		return MojErrNone;
	}

	MojObject row = *(results.arrayBegin());
	MojString dbId;
	err = row.getRequired(MOJDB_ID, dbId);
	MojErrCheck(err);

	// REPLACE mode (aggregated prpls): swap the whole reactions array for the parsed {emoji,count} set.
	if (m_replace) {
		MojObject mergeProps;
		err = mergeProps.put(MOJDB_REACTIONS, m_setReactions); MojErrCheck(err);
		err = mergeProps.putString(MOJDB_ID, dbId);            MojErrCheck(err);
		err = m_dbClient.merge(m_mergeSlot, mergeProps);
		MojErrCheck(err);
		return MojErrNone;
	}

	// Rebuild the reactions array: drop any prior reaction from THIS sender (react-change replaces),
	// then append the new one unless this is a removal (empty emoji).
	MojObject oldReactions;
	bool haveOld = row.get(MOJDB_REACTIONS, oldReactions);
	MojObject newReactions(MojObject::TypeArray);
	if (haveOld && oldReactions.type() == MojObject::TypeArray) {
		MojObject::ConstArrayIterator itr = oldReactions.arrayBegin();
		while (itr != oldReactions.arrayEnd()) {
			MojObject r = *itr;
			MojString s;
			bool haveSender = false;
			r.get(_T("sender"), s, haveSender);
			if (!(haveSender && s == m_sender)) {
				err = newReactions.push(r);
				MojErrCheck(err);
			}
			itr++;
		}
	}
	if (!m_emoji.empty()) {
		MojObject r;
		err = r.putString(_T("emoji"), m_emoji);   MojErrCheck(err);
		err = r.putString(_T("sender"), m_sender); MojErrCheck(err);
		err = newReactions.push(r);                MojErrCheck(err);
	}

	MojObject mergeProps;
	err = mergeProps.put(MOJDB_REACTIONS, newReactions); MojErrCheck(err);
	err = mergeProps.putString(MOJDB_ID, dbId);          MojErrCheck(err);

	err = m_dbClient.merge(m_mergeSlot, mergeProps);
	MojErrCheck(err);
	return MojErrNone;
}

MojErr ReactionHandler::mergeResult(MojObject& result, MojErr err)
{
	MojUnused(result);
	if (err) {
		MojString e; MojErrToString(err, e);
		MojLogError(IMServiceApp::s_log, _T("ReactionHandler::mergeResult: merge failed: %d - %s"), err, e.data());
		return MojErrNone;
	}
	MojLogInfo(IMServiceApp::s_log, _T("ReactionHandler::mergeResult: reaction stored on message %s"), m_targetId.data());
	return MojErrNone;
}
