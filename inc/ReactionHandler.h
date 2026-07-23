/*
 * ReactionHandler.h
 *
 * webOS cross-prpl message reactions. A prpl emits the "webos-im-reaction" libpurple signal
 * (registered by LibpurpleAdapter) carrying (account, targetServiceMessageId, emoji, sender). This
 * handler finds the TARGET message in db8 by its serviceMessageId and merges/removes the sender's
 * reaction on the row's `reactions` array. The Messaging app renders that array as inline badges
 * instead of a separate "reacted with X" message. Mirrors IncomingIMHandler's async find->merge.
 *
 *  emoji == "" (or NULL) => the sender REMOVED their reaction.
 *  a non-empty emoji     => add or REPLACE this sender's reaction (react-change is one emoji per sender).
 */
#ifndef REACTIONHANDLER_H_
#define REACTIONHANDLER_H_

#include "core/MojService.h"
#include "db/MojDbServiceClient.h"
#include "db/MojDb.h"
#include "IMServiceApp.h"

class ReactionHandler : public MojSignalHandler
{
public:
	ReactionHandler(MojService* service, IMServiceApp::Listener* listener);
	virtual ~ReactionHandler();

	// Scope (serviceName+username) identifies the owning account; targetId is the prpl message id
	// stored as serviceMessageId on the target row. Self-retained through the async DB ops via the
	// slots, so callers may drop their MojRefCountedPtr immediately after calling this.
	MojErr handleReaction(const char* serviceName, const char* username, const char* targetId,
			const char* emoji, const char* sender);

	// Aggregated (REPLACE) path, for prpls that expose the whole reaction summary at once rather than
	// per-sender events (e.g. Telegram's updateMessageInteractionInfo). `serialized` is a list of
	// "count<SP>emoji" records separated by '\n'; the target row's `reactions` array is REPLACED with
	// {emoji,count} entries (an empty/whitespace `serialized` clears all reactions).
	MojErr handleReactionSet(const char* serviceName, const char* username, const char* targetId,
			const char* serialized);

private:
	MojDbClient::Signal::Slot<ReactionHandler> m_findSlot;
	MojErr findResult(MojObject& result, MojErr err);

	MojDbClient::Signal::Slot<ReactionHandler> m_mergeSlot;
	MojErr mergeResult(MojObject& result, MojErr err);

	MojString m_serviceName;
	MojString m_username;
	MojString m_targetId;
	MojString m_emoji;
	MojString m_sender;

	// REPLACE mode: when true, findResult swaps the row's reactions for m_setReactions wholesale
	// instead of the per-sender merge. Set by handleReactionSet.
	bool m_replace;
	MojObject m_setReactions;

	MojService* m_service;
	IMServiceApp::Listener* m_listener;
	MojDbServiceClient m_dbClient;
};

#endif /* REACTIONHANDLER_H_ */
