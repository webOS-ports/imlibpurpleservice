/*
 * EditHandler.h
 *
 * webOS: apply a message EDIT in place. When a prpl reports that the sender edited an existing message
 * (e.g. WhatsApp's ProtocolMessage/EditedMessage), it emits the "webos-im-edit" signal with the edited
 * message's server id (serviceMessageId) + the new (already HTML-escaped) text. This handler finds the
 * matching immessage row by the (serviceName, username, serviceMessageId) index and merges the new
 * messageText onto it -- so the conversation updates the ORIGINAL bubble in place instead of showing a
 * separate "[EDIT] ..." message. Self-retained through the async DB ops (mirrors OutboxIdHandler).
 */
#ifndef EDITHANDLER_H_
#define EDITHANDLER_H_

#include "core/MojService.h"
#include "db/MojDbServiceClient.h"
#include "db/MojDb.h"
#include "IMServiceApp.h"

class EditHandler : public MojSignalHandler
{
public:
	EditHandler(MojService* service, IMServiceApp::Listener* listener);
	virtual ~EditHandler();

	// serviceName+username identify the owning account; serviceMessageId is the edited message's network
	// id; newText is the new (HTML-escaped, like a normal incoming body) message text.
	MojErr handleEdit(const char* serviceName, const char* username, const char* serviceMessageId,
			const char* newText);

private:
	MojDbClient::Signal::Slot<EditHandler> m_findSlot;
	MojErr findResult(MojObject& result, MojErr err);

	MojDbClient::Signal::Slot<EditHandler> m_mergeSlot;
	MojErr mergeResult(MojObject& result, MojErr err);

	MojString m_serviceName;
	MojString m_username;
	MojString m_serviceMessageId;
	MojString m_newText;

	MojService* m_service;
	IMServiceApp::Listener* m_listener;
	MojDbServiceClient m_dbClient;
};

#endif /* EDITHANDLER_H_ */
