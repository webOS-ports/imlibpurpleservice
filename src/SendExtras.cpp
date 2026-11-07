/*
 * SendExtras.cpp
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport).
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free Software
 * Foundation.
 *
 * See SendExtras.h.
 */

#include "SendExtras.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <strings.h>

namespace SendExtras {

const char* const LocationKey = "webos-send-location";
const char* const ContactKey = "webos-send-contact";
const char* const MentionsKey = "webos-send-mentions";

namespace {

const char Unit = '\x1f';
const char Record = '\x1e';

// A field of a record: the separators taken out, so a name cannot split it.
std::string field(const std::string& value)
{
	std::string out;
	out.reserve(value.size());
	for (std::string::const_iterator it = value.begin(); it != value.end(); ++it) {
		if (*it != Unit && *it != Record)
			out += *it;
	}
	return out;
}

std::string str(const MojObject& obj, const MojChar* key)
{
	MojString value;
	bool found = false;
	if (obj.get(key, value, found) != MojErrNone || !found)
		return std::string();
	return std::string(value.data());
}

// A number as JSON has it, integer or not.
bool number(const MojObject& obj, const MojChar* key, double& out)
{
	MojObject value;
	if (!obj.get(key, value))
		return false;
	if (value.type() == MojObject::TypeDecimal) {
		out = value.decimalValue().floatValue();
		return true;
	}
	if (value.type() == MojObject::TypeInt) {
		out = static_cast<double>(value.intValue());
		return true;
	}
	return false;
}

bool startsWithNoCase(const std::string& s, const std::string& prefix)
{
	return s.size() >= prefix.size() && strncasecmp(s.data(), prefix.data(), prefix.size()) == 0;
}

} // namespace

std::string location(double latitude, double longitude, const std::string& name)
{
	if (!std::isfinite(latitude) || !std::isfinite(longitude) ||
	    latitude < -90.0 || latitude > 90.0 || longitude < -180.0 || longitude > 180.0)
		return std::string();
	char buf[64];
	int const n = snprintf(buf, sizeof(buf), "%.7f%c%.7f%c", latitude, Unit, longitude, Unit);
	if (n <= 0 || static_cast<size_t>(n) >= sizeof(buf))
		return std::string();
	return std::string(buf, static_cast<size_t>(n)) + field(name);
}

std::string contact(const std::string& vcard)
{
	// Trailing line ends and blanks do not count when looking for the end of the card.
	size_t end = vcard.find_last_not_of(" \t\r\n");
	if (end == std::string::npos || !startsWithNoCase(vcard, "BEGIN:VCARD"))
		return std::string();
	static const char endTag[] = "END:VCARD";
	size_t const tagLen = sizeof(endTag) - 1;
	if (end + 1 < tagLen || strncasecmp(vcard.data() + end + 1 - tagLen, endTag, tagLen) != 0)
		return std::string();
	return vcard;
}

std::string mentions(const std::vector<Mention>& list)
{
	std::string out;
	for (std::vector<Mention>::const_iterator it = list.begin(); it != list.end(); ++it) {
		std::string const id = field(it->id);
		std::string const name = field(it->name);
		if (id.empty() || name.empty())
			continue;
		if (!out.empty())
			out += Record;
		out += id;
		out += Unit;
		out += name;
	}
	return out;
}

Extras fromMessage(const MojObject& imMsg)
{
	Extras extras;

	MojObject loc;
	double lat = 0, lon = 0;
	if (imMsg.get(_T("location"), loc) && loc.type() == MojObject::TypeObject &&
	    number(loc, _T("latitude"), lat) && number(loc, _T("longitude"), lon))
		extras.location = location(lat, lon, str(loc, _T("name")));

	MojObject card;
	if (imMsg.get(_T("contactCard"), card) && card.type() == MojObject::TypeObject)
		extras.contact = contact(str(card, _T("vcard")));

	MojObject list;
	if (imMsg.get(_T("mentions"), list) && list.type() == MojObject::TypeArray) {
		std::vector<Mention> found;
		MojObject entry;
		for (MojSize i = 0; i < list.size(); ++i) {
			if (!list.at(i, entry) || entry.type() != MojObject::TypeObject)
				continue;
			Mention m;
			m.id = str(entry, _T("id"));
			m.name = str(entry, _T("name"));
			found.push_back(m);
		}
		extras.mentions = mentions(found);
	}
	return extras;
}

std::vector<std::string> splitMembers(const std::string& joined)
{
	std::vector<std::string> out;
	size_t start = 0;
	while (start <= joined.size()) {
		size_t const end = joined.find(Unit, start);
		std::string const one = joined.substr(start, end == std::string::npos ? std::string::npos : end - start);
		if (!one.empty())
			out.push_back(one);
		if (end == std::string::npos)
			break;
		start = end + 1;
	}
	return out;
}

std::string joinMembers(const std::vector<std::string>& members)
{
	std::string out;
	for (std::vector<std::string>::const_iterator it = members.begin(); it != members.end(); ++it) {
		std::string const one = field(*it);
		if (one.empty())
			continue;
		if (!out.empty())
			out += Unit;
		out += one;
	}
	return out;
}

} // namespace SendExtras
