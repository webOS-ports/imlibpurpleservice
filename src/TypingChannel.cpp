/*
 * TypingChannel.cpp
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport). See TypingChannel.h.
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free Software
 * Foundation.
 */

#include "TypingChannel.h"
#include "IMServiceApp.h"

#include <cstring>

// ---- TypingState ---------------------------------------------------------------------------

std::string TypingState::key(const std::string& serviceName, const std::string& username,
                             const std::string& from)
{
	// '\x1f' cannot be in any of the three, so the key cannot be read two ways.
	return serviceName + '\x1f' + username + '\x1f' + from;
}

bool TypingState::set(const std::string& serviceName, const std::string& username,
                      const std::string& from, State state)
{
	if (serviceName.empty() || username.empty() || from.empty())
		return false;
	std::string const k = key(serviceName, username, from);
	Table::iterator it = m_table.find(k);
	if (state == None) {
		if (it == m_table.end())
			return false;
		m_table.erase(it);
		return true;
	}
	if (it != m_table.end() && it->second.state == state)
		return false;
	Entry& e = m_table[k];
	e.serviceName = serviceName;
	e.username = username;
	e.from = from;
	e.state = state;
	return true;
}

std::vector<TypingState::Entry> TypingState::clearAccount(const std::string& serviceName,
                                                          const std::string& username)
{
	std::vector<Entry> dropped;
	for (Table::iterator it = m_table.begin(); it != m_table.end();) {
		if (it->second.serviceName == serviceName && it->second.username == username) {
			dropped.push_back(it->second);
			dropped.back().state = None;
			m_table.erase(it++);
		} else {
			++it;
		}
	}
	return dropped;
}

std::vector<TypingState::Entry> TypingState::snapshot() const
{
	std::vector<Entry> out;
	for (Table::const_iterator it = m_table.begin(); it != m_table.end(); ++it)
		out.push_back(it->second);
	return out;
}

const char* TypingState::name(State state)
{
	switch (state) {
	case Typing: return "typing";
	case Paused: return "paused";
	default:     return "none";
	}
}

TypingState::State TypingState::parse(const char* name)
{
	if (name && strcmp(name, "typing") == 0)
		return Typing;
	if (name && strcmp(name, "paused") == 0)
		return Paused;
	return None;
}

// ---- TypingChannel -------------------------------------------------------------------------

TypingChannel::Subscription::Subscription(TypingChannel* owner, MojServiceMessage* msg)
: m_owner(owner),
  m_msg(msg),
  m_cancelSlot(this, &TypingChannel::Subscription::handleCancel)
{
	msg->notifyCancel(m_cancelSlot);
}

MojErr TypingChannel::Subscription::handleCancel(MojServiceMessage* msg)
{
	(void)msg;
	if (m_owner)
		m_owner->removeSubscription(this);
	return MojErrNone;
}

TypingChannel::TypingChannel(MojService* service)
{
	(void)service;
}

TypingChannel::~TypingChannel()
{
	// A message's cancel slot keeps its Subscription alive after this list lets go of it, and the
	// service cancels what is left of its subscriptions when it closes, which can be after this
	// channel is gone: detach them, so a late cancel does not call into a freed channel.
	for (std::list<MojRefCountedPtr<Subscription> >::iterator it = m_subscribers.begin();
	     it != m_subscribers.end(); ++it) {
		if (it->get() != NULL)
			(*it)->m_owner = NULL;
	}
	m_subscribers.clear();
}

void TypingChannel::removeSubscription(const Subscription* sub)
{
	for (std::list<MojRefCountedPtr<Subscription> >::iterator it = m_subscribers.begin();
	     it != m_subscribers.end(); ++it) {
		if (it->get() == sub) {
			m_subscribers.erase(it);
			return;
		}
	}
}

MojErr TypingChannel::eventPayload(const TypingState::Entry& entry, MojObject& out)
{
	MojErr err = out.putBool(_T("returnValue"), true);
	MojErrCheck(err);
	err = out.putBool(_T("subscribed"), true);
	MojErrCheck(err);
	err = out.putString(_T("serviceName"), entry.serviceName.c_str());
	MojErrCheck(err);
	err = out.putString(_T("username"), entry.username.c_str());
	MojErrCheck(err);
	err = out.putString(_T("from"), entry.from.c_str());
	MojErrCheck(err);
	err = out.putString(_T("state"), TypingState::name(entry.state));
	MojErrCheck(err);
	return MojErrNone;
}

MojErr TypingChannel::snapshotPayload(const std::vector<TypingState::Entry>& entries, MojObject& out)
{
	MojErr err = out.putBool(_T("returnValue"), true);
	MojErrCheck(err);
	err = out.putBool(_T("subscribed"), true);
	MojErrCheck(err);
	MojObject list(MojObject::TypeArray);
	for (size_t i = 0; i < entries.size(); i++) {
		MojObject one;
		err = one.putString(_T("serviceName"), entries[i].serviceName.c_str());
		MojErrCheck(err);
		err = one.putString(_T("username"), entries[i].username.c_str());
		MojErrCheck(err);
		err = one.putString(_T("from"), entries[i].from.c_str());
		MojErrCheck(err);
		err = one.putString(_T("state"), TypingState::name(entries[i].state));
		MojErrCheck(err);
		err = list.push(one);
		MojErrCheck(err);
	}
	err = out.put(_T("typing"), list);
	MojErrCheck(err);
	return MojErrNone;
}

void TypingChannel::send(const TypingState::Entry& entry)
{
	MojObject payload;
	if (eventPayload(entry, payload) != MojErrNone)
		return;
	for (std::list<MojRefCountedPtr<Subscription> >::iterator it = m_subscribers.begin();
	     it != m_subscribers.end(); ++it) {
		Subscription* sub = it->get();
		if (sub == NULL || sub->m_msg.get() == NULL)
			continue;
		MojErr err = sub->m_msg->reply(payload);
		if (err != MojErrNone)
			MojLogError(IMServiceApp::s_log, _T("TypingChannel: reply err %d"), (int)err);
	}
}

void TypingChannel::publish(const char* serviceName, const char* username, const char* from,
                            TypingState::State state)
{
	if (!serviceName || !username || !from)
		return;
	if (!m_state.set(serviceName, username, from, state))
		return;
	TypingState::Entry entry;
	entry.serviceName = serviceName;
	entry.username = username;
	entry.from = from;
	entry.state = state;
	send(entry);
}

void TypingChannel::clearAccount(const char* serviceName, const char* username)
{
	if (!serviceName || !username)
		return;
	std::vector<TypingState::Entry> dropped = m_state.clearAccount(serviceName, username);
	for (size_t i = 0; i < dropped.size(); i++)
		send(dropped[i]);
}

MojErr TypingChannel::subscribe(MojServiceMessage* msg)
{
	MojRefCountedPtr<Subscription> sub(new Subscription(this, msg));
	MojAllocCheck(sub.get());
	m_subscribers.push_back(sub);

	MojObject payload;
	MojErr err = snapshotPayload(m_state.snapshot(), payload);
	MojErrCheck(err);
	return msg->reply(payload);
}
