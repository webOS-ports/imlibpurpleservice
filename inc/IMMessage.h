/*
 * IMMessage.h
 *
 * Copyright 2010 Palm, Inc. All rights reserved.
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free
 * Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License,
 * Version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-
 * 1301, USA
 *
 * IMLibpurpleservice uses libpurple.so to implement a fully functional IM
 * Transport service for use on a mobile device.
 *
 * IMMessage class handles saving and retrieving an immessage object from the DB
 */

#ifndef INCOMINGIMMESSAGE_H_
#define INCOMINGIMMESSAGE_H_

#include "core/MojObject.h"
#include <time.h>

// Status is used to describe if the message is pending, has a failure or was successfully sent/received.
// Default is successful. When messages are moved to the outbox, we change the status to pending. The transports
// should set the status to successful or failed when the message is moved to the sent folder.
// The difference between failed and undeliverable is that the latter
// indicates that given another chance we know it will fail again because something is
// wrong with it
typedef enum {
	Successful = 0,
	Pending,
	Failed,
	Undeliverable,
	// used to indicate that the message can not be completed until a suitable data
	// connection can be made.
	WaitingForConnection,
} IMStatus;

typedef enum {
	Inbox = 0,
	Outbox,
	Drafts,
	System,
	Transient
} IMFolder;

// DB property names
#define MOJDB_FROM 					_T("from")
#define MOJDB_ADDRESS 				_T("addr")
#define MOJDB_FROM_ADDRESS 			_T("from.addr")
#define MOJDB_TO_ADDRESS 			_T("to.addr")
#define MOJDB_TO 					_T("to")
#define MOJDB_MSG_TEXT				_T("messageText")
#define MOJDB_DEVICE_TIMESTAMP		_T("localTimestamp")
#define MOJDB_SERVER_TIMESTAMP		_T("timestamp")
#define MOJDB_USERNAME				_T("username")
#define MOJDB_SERVICENAME           _T("serviceName") // type_gtalk, type_aim, etc
#define MOJDB_FOLDER				_T("folder")
#define MOJDB_STATUS       			_T("status")
#define MOJDB_ID					_T("_id")
#define MOJDB_IDS					_T("ids")
#define MOJDB_ERROR_CODE	        _T("errorCode")
#define MOJDB_ERROR_CATEGORY	    _T("errorCategory")

// webOS Servers/Rooms: multi-user-chat (MUC) properties. Only written for group-chat messages
// (Discord channels, IRC channels, ...); absent on 1:1 IMs. db8 is schemaless so these need no
// kind change; query indexes are added in Milestone 1.
#define MOJDB_CHAT_TYPE             _T("chatType")     // "groupchat" for MUC messages
#define MOJDB_CHANNEL_NAME          _T("channelName")  // stable room key (e.g. Discord channel id, Telegram chat-<id>)
#define MOJDB_CHANNEL_DISPLAY_NAME  _T("channelDisplayName")  // human room title (e.g. Telegram group title)
#define MOJDB_SERVER_ID             _T("serverId")     // parent server id (e.g. Discord guild id)
#define MOJDB_SERVER_NAME           _T("serverName")   // parent server display name (guild/network)

// libpurple transport property names
#define XPORT_SERVICE_TYPE          _T("serviceName") // gmail, aol etc
#define XPORT_FROM_ADDRESS 			_T("usernameFrom")
#define XPORT_TO_ADDRESS 			_T("usernameTo")
#define XPORT_USER 		        	_T("username")
#define XPORT_MSG_TEXT				_T("messageText")
#define XPORT_ERROR_TEXT	        _T("errorText")
#define XPORT_CAPABILITY_PROVIDERS  _T("capabilityProviders")

#define PALM_DB_IMMESSAGE_KIND 		"com.palm.immessage.libpurple:1"

class IMMessage : public MojRefCounted {

public:
	// IM messages type
	static const char* MSG_TYPE_IM;
	static const char* statusStrings[];
	static const char* folderStrings[];
	static const char* trustedTags[];

	IMMessage();
	virtual ~IMMessage();

	// webOS Servers/Rooms: channelName/serverId/serverName describe a multi-user-chat (MUC)
	// message's room + parent server; NULL for ordinary 1:1 IMs.
	MojErr initFromCallback(const char* serviceName, const char* username, const char* usernameFrom, const char* message, time_t timestamp = 0,
			const char* channelName = NULL, const char* channelDisplayName = NULL, const char* serverId = NULL, const char* serverName = NULL, bool muted = false,
			const char* usernameFromDisplay = NULL);
	MojErr createDBObject(MojObject& returnObject);
	MojErr unformatFromAddress(const MojString formattedScreenName, MojString& unformattedName);

private:
	MojString msgText;
	MojString fromAddress;
	MojString fromDisplayName;  // encoded sender name for display when it contains astral emoji (see sanitize.h); empty otherwise. Never used as a match key.
	MojString toAddress;

	// only meaningful for incoming messages - this is the time the message was received on the device.
	MojInt64 deviceTimestamp; // millisec.
	// time message was received on server - incoming timestamp
	MojInt64 serverTimestamp; // millisec.

	// successful, pending, failed etc.
	IMStatus status;

	// inbox, outbox etc.
	IMFolder folder;

	// gmail, aol etc
    MojString msgType;

	// webOS Servers/Rooms: multi-user-chat (MUC) metadata. isGroupChat gates whether the fields
	// below (and the MUC db8 properties) are written; all empty/false for ordinary 1:1 IMs.
	bool isGroupChat;
	MojString channelName;   // stable room key (e.g. Discord channel id/name, Telegram chat-<id>)
	MojString channelDisplayName;   // human room title (Telegram group name); channelName stays the match key
	MojString serverId;      // parent server id (Discord guild id) - mirrors serverName until M1
	MojString serverName;    // parent server display name (Discord guild / IRC network)

	// muted: the conversation is muted on the server side (e.g. a muted Telegram chat). When true
	// the message is stored with flags.noNotification so the Messaging app suppresses the banner
	// (the message still appears/counts as unread, matching native Telegram behaviour).
	bool muted;

};

#endif /* INCOMINGIMMESSAGE_H_ */
