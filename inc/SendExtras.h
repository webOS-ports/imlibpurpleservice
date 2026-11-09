/*
 * SendExtras.h
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport).
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free Software
 * Foundation.
 *
 * What an outgoing message carries besides its text, for a prpl that can send it natively: a
 * location, a contact card, and who the text mentions. The Messaging app puts them on the
 * immessage next to messageText, which is their plain form:
 *
 *   "location":    { "latitude": 52.37, "longitude": 4.89, "name": "Dam" }
 *   "contactCard": { "vcard": "BEGIN:VCARD\r\n...END:VCARD\r\n" }
 *   "mentions":    [ { "id": "<buddy name on the service>", "name": "Anna" } ]   (text has "@Anna")
 *
 * LibpurpleAdapter::sendMessage sets each on the conversation as a string right before the prpl's
 * send and takes it off again right after, so a prpl reads it in its send_im/chat_send and never
 * frees it, and one that knows nothing of it sends the text:
 *
 *   "webos-send-location"  "<lat>\x1f<lon>\x1f<name>"
 *   "webos-send-contact"   the vCard
 *   "webos-send-mentions"  "<id>\x1f<name>" records joined by "\x1e"
 *
 * Nothing here touches libpurple, so it can be tested on its own.
 */

#ifndef SENDEXTRAS_H_
#define SENDEXTRAS_H_

#include <string>
#include <vector>
#include <glib.h>
#include "core/MojObject.h"

namespace SendExtras {

extern const char* const LocationKey;
extern const char* const ContactKey;
extern const char* const MentionsKey;

struct Mention {
	std::string id;
	std::string name;
};

struct Extras {
	std::string location;   // "" when the message has none, else the conversation-data string
	std::string contact;
	std::string mentions;

	bool empty() const { return location.empty() && contact.empty() && mentions.empty(); }
};

/** "<lat>\x1f<lon>\x1f<name>", or "" for a position that is not on the earth. */
std::string location(double latitude, double longitude, const std::string& name);

/** The card when it is a vCard (BEGIN:VCARD ... END:VCARD), else "". */
std::string contact(const std::string& vcard);

/** The records of the mentions with both an id and a name, or "" when there are none. */
std::string mentions(const std::vector<Mention>& list);

/** Read what an immessage carries besides its text. Fields that are absent or unusable are left empty. */
Extras fromMessage(const MojObject& imMsg);

/**
 * Split a "\x1f"-joined list of buddy names (an imcommand's members), dropping empty ones.
 */
std::vector<std::string> splitMembers(const std::string& joined);
std::string joinMembers(const std::vector<std::string>& members);

/**
 * Put extras on a conversation's data for a send, and take them off after it. Data has
 * gpointer get(const char*) and void set(const char*, gpointer). The strings are the glib
 * allocator's: the prpl reads them and never frees them; clear frees what is there.
 */
template <class Data>
void clear(Data& data);

template <class Data>
void set(Data& data, const Extras* extras)
{
	clear(data);   // nothing left from before, to be sent along or leaked
	if (extras == NULL)
		return;
	if (!extras->location.empty())
		data.set(LocationKey, g_strdup(extras->location.c_str()));
	if (!extras->contact.empty())
		data.set(ContactKey, g_strdup(extras->contact.c_str()));
	if (!extras->mentions.empty())
		data.set(MentionsKey, g_strdup(extras->mentions.c_str()));
}

template <class Data>
void clear(Data& data)
{
	const char* const keys[] = { LocationKey, ContactKey, MentionsKey };
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
		gpointer value = data.get(keys[i]);
		if (value != NULL) {
			data.set(keys[i], NULL);
			g_free(value);
		}
	}
}

} // namespace SendExtras

#endif /* SENDEXTRAS_H_ */
