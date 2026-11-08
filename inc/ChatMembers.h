/*
 * ChatMembers.h
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport).
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free Software
 * Foundation.
 *
 * Who is in a group chat, for the Messaging app's Group Info (getChatMembers). libpurple keeps a
 * chat's people on its conversation (purple_conv_chat_get_users) once the prpl has put them there;
 * LibpurpleAdapter::chatMembers reads them and gives each the address and name the app knows them
 * by, as an incoming group message's sender gets them. This part tidies that list and writes the
 * reply:
 *
 *   { "returnValue": true, "inChat": <a conversation was found>,
 *     "members": [ { "address": "+31611111111", "name": "Anna" }, ... ] }
 *
 * Nothing here touches libpurple, so it can be tested on its own.
 */

#ifndef CHATMEMBERS_H_
#define CHATMEMBERS_H_

#include <string>
#include <vector>
#include "core/MojObject.h"

namespace ChatMembers {

struct Member {
	std::string address;
	std::string name;
};

/**
 * The members to answer with: each address once (the first name that is not empty), none without
 * an address, not own (our own address on the service), names without the "\x1e"/"\x1f"
 * separators the app's records use and trimmed, and the address as the name when there is none.
 * In the order given.
 */
std::vector<Member> clean(const std::vector<Member>& members, const std::string& own);

/** The getChatMembers reply for members (already cleaned). */
MojErr toPayload(const std::vector<Member>& members, bool inChat, MojObject& out);

} // namespace ChatMembers

#endif /* CHATMEMBERS_H_ */
