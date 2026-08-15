/*
 * OutboxIdHandler.h
 *
 * webOS: attach a serviceMessageId to a message the user sent FROM THE APP so a reaction can later
 * target it. App-sent messages are persisted by the app's own send path with NO serviceMessageId (the
 * network assigns the id asynchronously). When the prpl learns the server id (e.g. Telegram's
 * updateMessageSendSucceeded) it emits the "webos-im-outbox-id" signal; this handler finds the
 * matching Outbox row and merges the serviceMessageId onto it.
 *
 * Correlation is heuristic (nothing links the app row to the network id exactly): among the account's
 * recent Outbox rows that have no serviceMessageId yet, prefer one whose messageText matches the sent
 * text, else take the most recent. Good for the normal "react to what you just sent" case; the only
 * ambiguity is two identical messages sent within seconds (harmless - the ids are interchangeable).
 */
#ifndef OUTBOXIDHANDLER_H_
#define OUTBOXIDHANDLER_H_

#include "core/MojService.h"
#include "db/MojDbServiceClient.h"
#include "db/MojDb.h"
#include "IMServiceApp.h"

class OutboxIdHandler : public MojSignalHandler
{
public:
	OutboxIdHandler(MojService* service, IMServiceApp::Listener* listener);
	virtual ~OutboxIdHandler();

	// serviceName+username identify the owning account; serviceMessageId is the network id to store;
	// text is the sent message body (used only as a match hint). Self-retained through the async DB ops.
	MojErr handleOutboxId(const char* serviceName, const char* username, const char* serviceMessageId,
			const char* text);

private:
	MojDbClient::Signal::Slot<OutboxIdHandler> m_findSlot;
	MojErr findResult(MojObject& result, MojErr err);

	MojDbClient::Signal::Slot<OutboxIdHandler> m_mergeSlot;
	MojErr mergeResult(MojObject& result, MojErr err);

	MojString m_serviceName;
	MojString m_username;
	MojString m_serviceMessageId;
	MojString m_text;

	MojService* m_service;
	IMServiceApp::Listener* m_listener;
	MojDbServiceClient m_dbClient;
};

#endif /* OUTBOXIDHANDLER_H_ */
