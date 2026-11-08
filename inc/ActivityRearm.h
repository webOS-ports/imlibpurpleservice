/*
 * ActivityRearm.h
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport).
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free Software
 * Foundation.
 *
 * Re-creates the transport's DB-watch activities (ActivityDefinitions) when it starts.
 *
 * Only the configurator creates them, once, at boot. One that the activity manager no longer runs
 * stays so until the next boot, and with it the queue it drains: when the transport was restarted
 * while a watch fired, the callback found no service, the activity stayed "expired" with no one to
 * complete it, and no imcommand the Messaging app wrote was sent again.
 *
 * So the transport sends each definition again, with "replace": true, once it is on the bus. The
 * activity manager drops the old activity of that name and creator, however it stands, and arms
 * the new one. A db8 watch fires at once when its query already matches, so commands and messages
 * left pending are sent then, through the usual callback.
 */

#ifndef ACTIVITYREARM_H_
#define ACTIVITYREARM_H_

#include <string>
#include "core/MojService.h"
#include "core/MojServiceRequest.h"
#include "core/MojSignal.h"

class ActivityRearm : public MojSignalHandler
{
public:
	/**
	 * Sends activitymanager/create for every definition in dir that loads. The answers are only
	 * logged. Returns how many requests were sent.
	 */
	static size_t rearmAll(MojService* service, const std::string& dir);

private:
	explicit ActivityRearm(const std::string& name);

	MojErr send(MojService* service, const MojObject& payload);
	MojErr createResult(MojObject& result, MojErr err);

	MojServiceRequest::ReplySignal::Slot<ActivityRearm> m_createSlot;
	std::string m_name;
};

#endif /* ACTIVITYREARM_H_ */
