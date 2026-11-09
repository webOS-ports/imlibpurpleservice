/*
 * ActivityRearm.cpp
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport).
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free Software
 * Foundation.
 *
 * Re-creates the transport's DB-watch activities when it starts. See ActivityRearm.h.
 */

#include "ActivityRearm.h"
#include "ActivityDefinitions.h"
#include "IMServiceApp.h"

ActivityRearm::ActivityRearm(const std::string& name)
: m_createSlot(this, &ActivityRearm::createResult),
  m_name(name)
{
}

size_t ActivityRearm::rearmAll(MojService* service, const std::string& dir)
{
	size_t sent = 0;
	for (size_t i = 0; i < ActivityDefinitions::FILE_COUNT; ++i) {
		const char* file = ActivityDefinitions::FILES[i];
		MojObject payload;
		std::string name;
		if (!ActivityDefinitions::load(dir, file, payload, name)) {
			MojLogError(IMServiceApp::s_log, _T("ActivityRearm: no usable activity in %s/%s - not re-armed"), dir.c_str(), file);
			continue;
		}

		// Ref counted: the request's slot holds it until the answer is in.
		MojRefCountedPtr<ActivityRearm> rearm(new ActivityRearm(name));
		if (rearm->send(service, payload) == MojErrNone)
			++sent;
	}
	return sent;
}

MojErr ActivityRearm::send(MojService* service, const MojObject& payload)
{
	MojRefCountedPtr<MojServiceRequest> req;
	MojErr err = service->createRequest(req);
	if (err == MojErrNone) {
		MojLogInfo(IMServiceApp::s_log, _T("ActivityRearm: re-creating activity \"%s\""), m_name.c_str());
		// Not subscribed, so one answer: the id once it is created and started.
		err = req->send(m_createSlot, "com.palm.activitymanager", "create", payload, 1);
	}
	if (err != MojErrNone) {
		MojString error;
		MojErrToString(err, error);
		MojLogError(IMServiceApp::s_log, _T("ActivityRearm: create \"%s\" not sent: %d - %s"), m_name.c_str(), err, error.data());
	}
	return err;
}

MojErr ActivityRearm::createResult(MojObject& result, MojErr err)
{
	if (err != MojErrNone) {
		MojString error;
		bool found = false;
		result.get(_T("errorText"), error, found);
		if (!found)
			MojErrToString(err, error);
		MojLogError(IMServiceApp::s_log, _T("ActivityRearm: create \"%s\" failed: %d - %s"), m_name.c_str(), err, error.data());
		return MojErrNone;
	}

	MojInt64 activityId = -1;
	result.get(_T("activityId"), activityId);
	MojLogInfo(IMServiceApp::s_log, _T("ActivityRearm: activity \"%s\" armed as %lld"), m_name.c_str(), (long long) activityId);
	return MojErrNone;
}
