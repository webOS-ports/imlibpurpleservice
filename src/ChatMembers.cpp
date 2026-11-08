/*
 * ChatMembers.cpp
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport).
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free Software
 * Foundation.
 *
 * See ChatMembers.h.
 */

#include "ChatMembers.h"

#include <set>

namespace ChatMembers {

namespace {

std::string trim(const std::string& s)
{
	static const char* const space = " \t\r\n";
	size_t const first = s.find_first_not_of(space);
	if (first == std::string::npos)
		return std::string();
	size_t const last = s.find_last_not_of(space);
	return s.substr(first, last - first + 1);
}

std::string tidy(const std::string& s)
{
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); ++i) {
		if (s[i] != '\x1e' && s[i] != '\x1f')
			out += s[i];
	}
	return trim(out);
}

// "+<digits>" (trimmed), else empty.
std::string phoneNumber(const std::string& s)
{
	std::string p = trim(s);
	if (p.size() < 2 || p[0] != '+' || p.find_first_not_of("0123456789", 1) != std::string::npos)
		return std::string();
	return p;
}

} // namespace

const char* const PHONE_ATTRIBUTE = "phone";

std::vector<Member> clean(const std::vector<Member>& members, const std::string& own)
{
	std::vector<Member> out;
	std::set<std::string> seen;
	for (size_t i = 0; i < members.size(); ++i) {
		std::string const address = tidy(members[i].address);
		if (address.empty() || (!own.empty() && address == own))
			continue;
		std::string const name = tidy(members[i].name);
		std::string const phone = phoneNumber(members[i].phone);
		if (seen.count(address)) {
			// Seen without a name or a phone: take this one's.
			for (size_t j = 0; j < out.size(); ++j) {
				if (out[j].address != address)
					continue;
				if (out[j].name == address && !name.empty())
					out[j].name = name;
				if (out[j].phone.empty())
					out[j].phone = phone;
			}
			continue;
		}
		seen.insert(address);
		Member m;
		m.address = address;
		m.name = name.empty() ? address : name;
		m.phone = phone;
		out.push_back(m);
	}
	return out;
}

bool joinOpensMembers(const std::string& protocolId, const std::string& chat)
{
	if (chat.empty())
		return false;
	if (protocolId == "prpl-telegram")
		return true;
	if (protocolId == "prpl-hehoe-whatsmeow") {
		static const std::string newsletter = "@newsletter";
		if (chat.size() >= newsletter.size()
				&& chat.compare(chat.size() - newsletter.size(), newsletter.size(), newsletter) == 0)
			return false;
		return chat != "status@broadcast";
	}
	return false;
}

OpenWay openWay(const std::string& protocolId, const std::string& chat, bool inBuddyList, bool connected)
{
	if (!connected || chat.empty())
		return DontOpen;
	if (joinOpensMembers(protocolId, chat))
		return inBuddyList ? OpenByJoin : DontOpen;
	return OpenBySignal;
}

bool stillJoining(bool joinAsked, bool inChat, size_t memberCount)
{
	return joinAsked && (!inChat || memberCount == 0);
}

MojErr toPayload(const std::vector<Member>& members, bool inChat, MojObject& out)
{
	return toPayload(members, inChat, false, out);
}

MojErr toPayload(const std::vector<Member>& members, bool inChat, bool joining, MojObject& out)
{
	MojErr err = out.putBool(_T("returnValue"), true);
	MojErrCheck(err);
	err = out.putBool(_T("inChat"), inChat);
	MojErrCheck(err);
	if (joining) {
		err = out.putBool(_T("joining"), true);
		MojErrCheck(err);
	}
	MojObject list(MojObject::TypeArray);
	for (size_t i = 0; i < members.size(); ++i) {
		MojObject one;
		err = one.putString(_T("address"), members[i].address.c_str());
		MojErrCheck(err);
		err = one.putString(_T("name"), members[i].name.c_str());
		MojErrCheck(err);
		if (!members[i].phone.empty()) {
			err = one.putString(_T("phone"), members[i].phone.c_str());
			MojErrCheck(err);
		}
		err = list.push(one);
		MojErrCheck(err);
	}
	err = out.put(_T("members"), list);
	MojErrCheck(err);
	return MojErrNone;
}

} // namespace ChatMembers
