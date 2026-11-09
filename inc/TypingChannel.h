/*
 * TypingChannel.h
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport).
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free Software
 * Foundation.
 *
 * Who is typing to us, for the Messaging app. libpurple raises "buddy-typing", "buddy-typed"
 * and "buddy-typing-stopped" for a 1:1 conversation when a prpl calls serv_got_typing; nothing
 * of it is kept in db8, as it changes every few seconds. The app subscribes with watchTyping and
 * gets the current typers first, then one event per change:
 *
 *   { "serviceName": "type_whatsapp", "username": "<our account>", "from": "<buddy>",
 *     "state": "typing" | "paused" | "none" }
 *
 * "paused" is libpurple's PURPLE_TYPED: text was typed and the typing stopped. A prpl that never
 * sends the stop leaves a buddy "typing", so the app lets the state lapse on its own as well.
 *
 * TypingState is the table, with nothing of the bus in it, so it can be tested on its own.
 */

#ifndef TYPINGCHANNEL_H_
#define TYPINGCHANNEL_H_

#include <list>
#include <map>
#include <string>
#include <vector>
#include "core/MojObject.h"
#include "core/MojRefCount.h"
#include "core/MojService.h"
#include "core/MojServiceMessage.h"
#include "core/MojSignal.h"

class TypingState
{
public:
	enum State { None = 0, Typing, Paused };

	struct Entry {
		std::string serviceName;
		std::string username;
		std::string from;
		State state;
	};

	/** Record a change. False when it changes nothing, so nothing need be sent. */
	bool set(const std::string& serviceName, const std::string& username,
	         const std::string& from, State state);

	/** Forget every typer of one account, as when it signs off. The entries that were dropped. */
	std::vector<Entry> clearAccount(const std::string& serviceName, const std::string& username);

	/** Everyone typing or paused now, in a stable order. */
	std::vector<Entry> snapshot() const;

	static const char* name(State state);
	/** "typing", "paused" or "none" (anything else) as a State. */
	static State parse(const char* name);

private:
	typedef std::map<std::string, Entry> Table;
	static std::string key(const std::string& serviceName, const std::string& username,
	                       const std::string& from);
	Table m_table;
};

class TypingChannel
{
public:
	explicit TypingChannel(MojService* service);
	~TypingChannel();

	/** From LibpurpleAdapter's typing signals, on the glib main loop. */
	void publish(const char* serviceName, const char* username, const char* from,
	             TypingState::State state);
	/** An account signed off: whoever was typing on it is not any more. */
	void clearAccount(const char* serviceName, const char* username);

	/** watchTyping: hold the subscription and reply with the typers there are now. */
	MojErr subscribe(MojServiceMessage* msg);

	/** The payloads, exposed for the tests. */
	static MojErr eventPayload(const TypingState::Entry& entry, MojObject& out);
	static MojErr snapshotPayload(const std::vector<TypingState::Entry>& entries, MojObject& out);

private:
	class Subscription : public MojSignalHandler {
	public:
		Subscription(TypingChannel* owner, MojServiceMessage* msg);
		using MojSignalHandler::handleCancel;   // the base's own, which this overload would hide
		MojErr handleCancel(MojServiceMessage* msg);

		TypingChannel* m_owner;
		MojRefCountedPtr<MojServiceMessage> m_msg;
		MojServiceMessage::CancelSignal::Slot<Subscription> m_cancelSlot;
	};
	friend class Subscription;

	void send(const TypingState::Entry& entry);
	void removeSubscription(const Subscription* sub);

	TypingState m_state;
	std::list<MojRefCountedPtr<Subscription> > m_subscribers;
};

#endif /* TYPINGCHANNEL_H_ */
