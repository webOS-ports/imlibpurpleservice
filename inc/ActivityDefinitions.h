/*
 * ActivityDefinitions.h
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport).
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free Software
 * Foundation.
 *
 * The transport's three DB-watch activities, as the configurator creates them at boot from
 * /etc/palm/activities/com.palm.imlibpurple/ (files/etc/palm/activities/... in this tree):
 *
 *   com.palm.imcommand.libpurple     "IM Service pending commands watch"  -> sendCommand
 *   com.palm.immessage.libpurple     "IM Service pending messages watch"  -> sendIM
 *   com.palm.imloginstate.libpurple  "Libpurple loginstate"               -> loginStateChanged
 *
 * Each file is {"start": true, "activity": {...}}, the activitymanager/create request without
 * the creator, which the configurator adds from the directory name. ActivityRearm sends the same
 * files again when the transport starts, with "replace": true; this part turns a file into that
 * request. The files stay the only definition of these activities: what the configurator
 * creates at boot and what the transport re-creates are the same.
 *
 * Nothing here talks to the bus, so it can be tested on its own.
 */

#ifndef ACTIVITYDEFINITIONS_H_
#define ACTIVITYDEFINITIONS_H_

#include <string>
#include "core/MojObject.h"

namespace ActivityDefinitions {

/** Where the definitions are installed. */
extern const char* const DIR;

/** The definition files in DIR, and how many there are. */
extern const char* const FILES[];
extern const size_t FILE_COUNT;

/** A definition is read only if it is at most this long. */
extern const size_t MAX_SIZE;

/**
 * The activitymanager/create request for one definition file's text: the file with "start" and
 * "replace" true, without what the transport may not or need not send (activity.creator, which
 * the activity manager takes only from the configurator and sets to the caller otherwise;
 * "subscribe" and "detailedEvents", which would make the transport the activity's parent; the
 * configurator's own "firstUseSafe").
 *
 * False, with payload and name left as they were, unless the text is an object holding an
 * "activity" object that has a non-empty "name", a "trigger" object and a callback method on
 * com.palm.imlibpurple: only this transport's own watches are re-created.
 */
bool toCreatePayload(const MojChar* text, MojObject& payload, std::string& name);

/**
 * Reads dir/file and makes its request (toCreatePayload). False when the file cannot be read,
 * is longer than MAX_SIZE or is not a definition.
 */
bool load(const std::string& dir, const char* file, MojObject& payload, std::string& name);

} // namespace ActivityDefinitions

#endif /* ACTIVITYDEFINITIONS_H_ */
