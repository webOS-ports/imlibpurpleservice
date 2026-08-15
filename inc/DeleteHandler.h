/*
 * DeleteHandler.h
 *
 * webOS: apply a "delete for everyone" in place. When a prpl reports that the sender revoked an
 * existing message (e.g. WhatsApp's ProtocolMessage/REVOKE), it emits the "webos-im-delete" signal
 * with the deleted message's server id (serviceMessageId). This handler finds the matching
 * immessage row by the (serviceName, username, serviceMessageId) index - the same lookup
 * EditHandler uses - and merges a "This message was deleted." placeholder onto its messageText, so
 * the conversation updates the ORIGINAL bubble in place instead of leaving the deleted content
 * visible forever. Self-retained through the async DB ops (mirrors EditHandler/OutboxIdHandler).
 */
#ifndef DELETEHANDLER_H_
#define DELETEHANDLER_H_

#include "core/MojService.h"
#include "db/MojDbServiceClient.h"
#include "db/MojDb.h"
#include "IMServiceApp.h"

class DeleteHandler : public MojSignalHandler
{
public:
	DeleteHandler(MojService* service, IMServiceApp::Listener* listener);
	virtual ~DeleteHandler();

	// serviceName+username identify the owning account; serviceMessageId is the deleted message's
	// network id.
	MojErr handleDelete(const char* serviceName, const char* username, const char* serviceMessageId);

private:
	MojDbClient::Signal::Slot<DeleteHandler> m_findSlot;
	MojErr findResult(MojObject& result, MojErr err);

	MojDbClient::Signal::Slot<DeleteHandler> m_mergeSlot;
	MojErr mergeResult(MojObject& result, MojErr err);

	MojString m_serviceName;
	MojString m_username;
	MojString m_serviceMessageId;

	MojService* m_service;
	IMServiceApp::Listener* m_listener;
	MojDbServiceClient m_dbClient;
};

#endif /* DELETEHANDLER_H_ */
