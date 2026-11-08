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

std::string tidy(const std::string& s)
{
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); ++i) {
		if (s[i] != '\x1e' && s[i] != '\x1f')
			out += s[i];
	}
	static const char* const space = " \t\r\n";
	size_t const first = out.find_first_not_of(space);
	if (first == std::string::npos)
		return std::string();
	size_t const last = out.find_last_not_of(space);
	return out.substr(first, last - first + 1);
}

} // namespace

std::vector<Member> clean(const std::vector<Member>& members, const std::string& own)
{
	std::vector<Member> out;
	std::set<std::string> seen;
	for (size_t i = 0; i < members.size(); ++i) {
		std::string const address = tidy(members[i].address);
		if (address.empty() || (!own.empty() && address == own))
			continue;
		std::string const name = tidy(members[i].name);
		if (seen.count(address)) {
			// Seen without a name: take this one's.
			for (size_t j = 0; j < out.size(); ++j) {
				if (out[j].address == address && out[j].name == address && !name.empty())
					out[j].name = name;
			}
			continue;
		}
		seen.insert(address);
		Member m;
		m.address = address;
		m.name = name.empty() ? address : name;
		out.push_back(m);
	}
	return out;
}

MojErr toPayload(const std::vector<Member>& members, bool inChat, MojObject& out)
{
	MojErr err = out.putBool(_T("returnValue"), true);
	MojErrCheck(err);
	err = out.putBool(_T("inChat"), inChat);
	MojErrCheck(err);
	MojObject list(MojObject::TypeArray);
	for (size_t i = 0; i < members.size(); ++i) {
		MojObject one;
		err = one.putString(_T("address"), members[i].address.c_str());
		MojErrCheck(err);
		err = one.putString(_T("name"), members[i].name.c_str());
		MojErrCheck(err);
		err = list.push(one);
		MojErrCheck(err);
	}
	err = out.put(_T("members"), list);
	MojErrCheck(err);
	return MojErrNone;
}

} // namespace ChatMembers
