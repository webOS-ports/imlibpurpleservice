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
 * by, as an incoming group message's sender gets them, and the phone number the prpl keeps on the
 * chat buddy (attribute PHONE_ATTRIBUTE) when it has one: a WhatsApp group addressed by LID gives
 * its members as "<id>@lid", which says nothing of the number. This part tidies that list and
 * writes the reply:
 *
 *   { "returnValue": true, "inChat": <a conversation was found>,
 *     "members": [ { "address": "+31611111111", "name": "Anna" },
 *                  { "address": "123456789012345@lid", "name": "Marta", "phone": "+46701234567" },
 *                  ... ] }
 *
 * "phone" is there only when the number is known.
 *
 * Nothing here touches libpurple, so it can be tested on its own.
 */

#ifndef CHATMEMBERS_H_
#define CHATMEMBERS_H_

#include <string>
#include <vector>
#include "core/MojObject.h"

namespace ChatMembers {

/** The chat buddy attribute (purple_conv_chat_cb_get_attribute) a prpl keeps a member's phone
 *  number in, "+<digits>"; purple-combined's WhatsApp plugin sets it. */
extern const char* const PHONE_ATTRIBUTE;

struct Member {
	std::string address;
	std::string name;
	std::string phone; ///< "+<digits>", or empty when not known
};

/**
 * The members to answer with: each address once (the first name that is not empty, the first
 * phone), none without an address, not own (our own address on the service), names without the
 * "\x1e"/"\x1f" separators the app's records use and trimmed, and the address as the name when
 * there is none. A phone is kept only as "+" and digits (spaces around it trimmed); anything else
 * is no phone. In the order given.
 */
std::vector<Member> clean(const std::vector<Member>& members, const std::string& own);

/** The getChatMembers reply for members (already cleaned). */
MojErr toPayload(const std::vector<Member>& members, bool inChat, MojObject& out);

} // namespace ChatMembers

#endif /* CHATMEMBERS_H_ */
