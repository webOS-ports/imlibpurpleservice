/*
 * ActivityDefinitions.cpp
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport).
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free Software
 * Foundation.
 *
 * The transport's DB-watch activity definitions, turned into activitymanager/create requests.
 * See ActivityDefinitions.h.
 */

#include "ActivityDefinitions.h"

#include <cstring>
#include <fstream>
#include <iterator>

namespace ActivityDefinitions {

const char* const DIR = "/etc/palm/activities/com.palm.imlibpurple";

const char* const FILES[] = {
	"com.palm.imcommand.libpurple",
	"com.palm.immessage.libpurple",
	"com.palm.imloginstate.libpurple"
};
const size_t FILE_COUNT = sizeof(FILES) / sizeof(FILES[0]);

const size_t MAX_SIZE = static_cast<size_t>(64) * 1024;

namespace {

/* Our own methods only: a definition that calls back elsewhere is not one of ours. */
bool isOurCallback(const MojString& method)
{
	return method.startsWith(_T("palm://com.palm.imlibpurple/"))
		|| method.startsWith(_T("luna://com.palm.imlibpurple/"));
}

bool drop(MojObject& obj, const MojChar* key)
{
	bool found = false;
	return obj.del(key, found) == MojErrNone;
}

} // namespace

bool toCreatePayload(const MojChar* text, MojObject& payload, std::string& name)
{
	if (text == NULL)
		return false;

	// Only an object has keys: anything else in place of the definition, the activity or the
	// callback fails the get() that follows. The trigger is only passed on, so it is checked.
	MojObject def;
	// With its length: given MojSizeMax, db8's parser read past the end of some of the texts
	// test/rearm refuses (valgrind: uninitialised reads in MojJsonParser::parseChunk).
	if (def.fromJson(text, strlen(text)) != MojErrNone)
		return false;

	MojObject activity;
	if (!def.get(_T("activity"), activity))
		return false;

	MojString activityName;
	bool found = false;
	if (activity.get(_T("name"), activityName, found) != MojErrNone || !found || activityName.empty())
		return false;

	MojObject trigger;
	if (!activity.get(_T("trigger"), trigger) || trigger.type() != MojObject::TypeObject)
		return false;

	MojObject callback;
	if (!activity.get(_T("callback"), callback))
		return false;
	MojString method;
	found = false;
	if (callback.get(_T("method"), method, found) != MojErrNone || !found || !isOurCallback(method))
		return false;

	// The activity manager refuses a creator from anyone but the configurator, and sets it to the
	// caller: serviceId com.palm.imlibpurple, which is what the configurator gives these too, so
	// "replace" finds the one it made.
	if (!drop(activity, _T("creator")))
		return false;

	// Not the activity's parent: the configurator does not subscribe either, so the activity lives
	// on when the transport exits, and the callback is what starts each run.
	if (!drop(def, _T("subscribe")) || !drop(def, _T("detailedEvents")) || !drop(def, _T("firstUseSafe")))
		return false;

	if (def.put(_T("activity"), activity) != MojErrNone
		|| def.putBool(_T("start"), true) != MojErrNone
		|| def.putBool(_T("replace"), true) != MojErrNone)
		return false;

	payload = def;
	name.assign(activityName.data(), activityName.length());
	return true;
}

bool load(const std::string& dir, const char* file, MojObject& payload, std::string& name)
{
	if (file == NULL)
		return false;

	std::ifstream in((dir + "/" + file).c_str(), std::ios::in | std::ios::binary);
	if (!in)
		return false;

	std::string text;
	std::istreambuf_iterator<char> it(in), end;
	for (; it != end; ++it) {
		if (text.size() >= MAX_SIZE)
			return false;
		text.push_back(*it);
	}
	if (in.bad())
		return false;

	return toCreatePayload(text.c_str(), payload, name);
}

} // namespace ActivityDefinitions
