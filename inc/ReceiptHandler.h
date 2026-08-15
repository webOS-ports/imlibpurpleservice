/*
 * ReceiptHandler.h
 *
 * webOS delivery/read receipts: when a prpl reports that the recipient DELIVERED or READ an outgoing
 * message, this handler stamps the matching Outbox row's `deliveryStatus` ("delivered" -> single tick,
 * "read" -> double tick) so the Messaging app can render a checkmark. Upgrades are monotonic (read
 * outranks delivered; a row is never downgraded), so out-of-order / duplicate receipts are safe.
 *
 * Two shapes, matching the two ways protocols report receipts:
 *  - BY-ID (WhatsApp, Signal): the receipt names the exact message id (== serviceMessageId). We look
 *    that one row up via the (serviceName,username,serviceMessageId) index and upgrade it.
 *  - WATERMARK (Telegram, Facebook, Teams): the receipt says "everything up to X is read/delivered".
 *    We scan the account's recent Outbox rows and upgrade every one at/under the watermark. `scope`
 *    encodes both the match field and the conversation:
 *      "msgid:<prefix>"  -> serviceMessageId starts with <prefix>; compare the numeric tail after the
 *                           last ':' against the watermark (Telegram "<chatId>:<messageId>").
 *      "id:<peerAddr>"   -> to.addr == peerAddr; compare serviceMessageId (numeric) (Teams).
 *      "ts:<peerAddr>"   -> to.addr == peerAddr; compare the row's server `timestamp` (Facebook).
 */
#ifndef RECEIPTHANDLER_H_
#define RECEIPTHANDLER_H_

#include "core/MojService.h"
#include "db/MojDbServiceClient.h"
#include "db/MojDb.h"
#include "IMServiceApp.h"

class ReceiptHandler : public MojSignalHandler
{
public:
	ReceiptHandler(MojService* service, IMServiceApp::Listener* listener);
	virtual ~ReceiptHandler();

	// BY-ID: mark the single Outbox row whose serviceMessageId == serviceMessageId. status is
	// "delivered" or "read".
	MojErr handleReceiptById(const char* serviceName, const char* username,
			const char* serviceMessageId, const char* status);

	// WATERMARK: mark every Outbox row at/under the watermark for the conversation named by scope
	// (see the class comment for the scope grammar). status is "delivered" or "read".
	MojErr handleReceiptWatermark(const char* serviceName, const char* username,
			const char* scope, const char* watermark, const char* status);

private:
	MojDbClient::Signal::Slot<ReceiptHandler> m_findSlot;
	MojErr findResult(MojObject& result, MojErr err);

	MojDbClient::Signal::Slot<ReceiptHandler> m_mergeSlot;
	MojErr mergeResult(MojObject& result, MojErr err);

	// rank: "" / other = 0, "delivered" = 1, "read" = 2. Only upgrade when new > current.
	static int statusRank(const MojString& s);
	static int statusRank(const char* s);

	bool      m_watermarkMode;     // false = by-id, true = watermark
	MojString m_serviceName;
	MojString m_username;
	MojString m_serviceMessageId;  // by-id: the target id
	MojString m_scope;             // watermark: "msgid:<prefix>" / "id:<peer>" / "ts:<peer>"
	MojString m_watermark;         // watermark: numeric boundary (string)
	MojString m_status;            // "delivered" | "read"

	MojService* m_service;
	IMServiceApp::Listener* m_listener;
	MojDbServiceClient m_dbClient;
};

#endif /* RECEIPTHANDLER_H_ */
