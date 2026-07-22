/*
 * LibpurpleAdapter.h
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
 * The LibpurpleAdapter is a simple adapter between libpurple.so and the
 * IMLibpurpleService transport
 */

#ifndef LIBPURPLEADAPTER_H_
#define LIBPURPLEADAPTER_H_

#include <purple.h>
//#include <lunaservice.h> //TODO remove this once LS is removed from the adapter
#include <syslog.h>
#include <string>
#include "PalmImCommon.h"
#include "core/MojService.h"
#include "db/MojDb.h"

#define CUSTOM_USER_DIRECTORY  "/var/preferences/com.palm.purple/transport"
#define CUSTOM_PLUGIN_PATH     APP_PATH "/plugins"
#define PLUGIN_SAVE_PREF       "/purple/nullclient/plugins/saved"
#define UI_ID                  "adapter"

class AuthChannel;


/*
 * The adapter callbacks interface
 */
class LoginCallbackInterface
{
public:
	typedef enum {
		LOGIN_SUCCESS, // successful login
		LOGIN_FAILED, //
		LOGIN_TIMEOUT, //
		LOGIN_SIGNED_OFF
	} LoginResult;

	virtual void loginResult(const char* serviceName, const char* username, LoginResult type, bool loggedOut, const char* errCode, bool noRetry) = 0;
	virtual void buddyListResult(const char* serviceName, const char* username, MojObject& buddyList, bool fullList) = 0;
	// webOS Telegram port: the buddy list changed AFTER the login-time getFullBuddyList() one-shot
	// snapshot (e.g. tdlib-purple loads its chats/contacts asynchronously post-login, so those
	// buddies never made it into a db8 contact). Ask the login-state layer to re-run the full
	// buddy-list sync (getBuddyLists) for this account. The adapter debounces the post-login burst.
	virtual void buddyListChanged(const char* serviceName, const char* username) = 0;
};

/*
 * Incoming IM message callback
 */
class IMServiceCallbackInterface
{
public:
	typedef enum {
		RECEIVE_SUCCESS, // successful login
		RECEIVE_FAILED, //
	} ReceiveResult;

	// webOS Teams port: timestamp is the libpurple message time (write_conv mtime, secs);
	// 0 => use current time. Preserves original send time for history/offline messages.
	// webOS Servers/Rooms: channelName/serverId/serverName tag a multi-user-chat (MUC) message
	// with its room + parent server (Discord guild, IRC network). All NULL for ordinary 1:1 IMs.
	// muted: source conversation is muted server-side (e.g. a muted Telegram chat) -> the message
	// is stored with flags.noNotification so the Messaging app suppresses the banner. Default false.
	// usernameFromDisplay: the sender's human display name when it differs from usernameFrom (which is the
	// routable id). Set for group messages on flat protocols (Telegram) so from.name shows the name while
	// from.addr stays the id; NULL for 1:1 IMs and where usernameFrom is already the display name (Discord).
	virtual bool incomingIM(const char* serviceName, const char* username, const char* usernameFrom, const char* message, time_t timestamp = 0,
				const char* channelName = NULL, const char* channelDisplayName = NULL, const char* serverId = NULL, const char* serverName = NULL, bool muted = false,
				const char* usernameFromDisplay = NULL, const char* serviceMessageId = NULL) = 0;
	// webOS reactions: `sender` reacted (emoji, or "" to remove) to the message the prpl identifies by
	// `targetServiceMessageId`. Attaches to that message's `reactions` array (see ReactionHandler)
	// rather than storing a new message. Cross-prpl: driven by the "webos-im-reaction" signal.
	virtual bool handleReaction(const char* serviceName, const char* username, const char* targetServiceMessageId,
				const char* emoji, const char* sender) = 0;
	virtual bool updateBuddyStatus(const char* accountId, const char* serviceName, const char* username, int availability,
				const char* customMessage, const char* groupName, const char* buddyAvatarLoc) = 0;
	// Perf (#2): batched presence for one account - `updates` is an array of { username, availability,
	// status, group }. The adapter coalesces per-buddy presence ticks over a short window and flushes
	// them here as ONE batch, which the db8 impl turns into a single find + batched merge/put instead
	// of a find+merge per buddy. Default no-op so non-db8 implementors need not override.
	virtual bool updateBuddyStatusBatch(const char* accountId, const char* serviceName, MojObject& updates) { return true; }
	virtual bool receivedBuddyInvite(const char* serviceName, const char* username, const char* usernameFrom, const char* message) = 0;
	virtual bool buddyInviteDeclined(const char* serviceName, const char* username, const char* usernameFrom) = 0;
	// webOS Servers/Rooms M3: full server->channel roster enumerated from the buddy list at login,
	// so every guild + all its (visible) channels appear in the Servers tab without waiting for a
	// message. serversObj is an array of { remoteId, name, channels:[{ remoteId, name, parentId,
	// position }] }. Default no-op so non-db8 implementors need not override.
	virtual bool syncServersChannels(const char* serviceName, const char* username, MojObject& serversObj) { return true; }
};

class LibpurpleAdapter
{
public:
	typedef enum {
		OK,
		ALREADY_LOGGED_IN,
		FAILED,
		INVALID_CREDENTIALS
	} LoginResult;

	// return values for sending commands to libpurple
	typedef enum {
		SENT,
		USER_NOT_LOGGED_IN,
		INVALID_PARAMS,
		SEND_FAILED
	} SendResult;

	static void init();
	static void assignIMLoginState(LoginCallbackInterface* loginState);
	static void assignIMServiceHandler(IMServiceCallbackInterface* incomingIMHandler);
	// Interactive-login (Discord QR) challenge channel. See AuthChannel.
	static void assignAuthChannel(AuthChannel* authChannel);
	// Start a disposable, not-yet-persisted remote-auth login purely to obtain a QR /
	// token (create-after-confirm). The QR is surfaced through the AuthChannel; on
	// remote-auth success the obtained token is pushed as the confirmed credential.
	static LoginResult startQRLogin(const char* serviceName, const char* username);
	// Tear down a pending QR-preview login (user cancelled / refreshing the code).
	static void cancelQRLogin(const char* serviceName, const char* username);
	// Feed a solved captcha response token back to the prpl's pending request_fields
	// callback (Discord remote-auth hCaptcha). Returns true if a pending captcha request
	// was found and its callback invoked.
	static bool submitCaptcha(const char* serviceName, const char* username, const char* captchaKey);
	static LoginResult login(LoginParams const& params, LoginCallbackInterface* loginState);
	// return false if already logged out
	static bool logout(const char* serviceName, const char* username, LoginCallbackInterface* loginState);
	// webOS Teams port: remove the persisted PurpleAccount (accounts.xml + blist +
	// stored refresh_token) tagged with this webOS accountId, on account deletion.
	// If outUsername/outServiceName are non-NULL they receive the account's username and
	// serviceName ("type_<suffix>") BEFORE it is deleted, so the caller can purge that
	// account's db8 chat data (which is keyed by username/serviceName, not webOS accountId).
	static bool deleteAccountByWebosId(const char* accountId, std::string* outUsername = NULL, std::string* outServiceName = NULL);
	static bool getFullBuddyList(const char* serviceName, const char* username);
	// webOS Servers/Rooms M3: walk the (in-memory) buddy list for a hierarchical account (Discord),
	// build the guild->channel roster and hand it to the service handler (syncServersChannels) to
	// upsert into db8. Called post-login once the blist is populated.
	static bool enumerateServersChannels(const char* serviceName, const char* username);
	// webOS Servers/Rooms M3: join a channel on demand (Servers-tab open) so the prpl fetches its
	// history and a later send routes to the chat. username may be NULL (resolve by serviceName).
	static bool openChannel(const char* serviceName, const char* username, const char* channel);
	static bool setMyAvailability(const char* serviceName, const char* username, int availability);
	static bool setMyCustomMessage(const char* serviceName, const char* username, const char* customMessage);
	static SendResult blockBuddy(const char* serviceName, const char* username, const char* buddyUsername, bool block);
	static SendResult removeBuddy(const char* serviceName, const char* username, const char* buddyUsername);
	static SendResult addBuddy(const char* serviceName, const char* username, const char* buddyUsername, const char* groupname);
	static SendResult authorizeBuddy(const char* serviceName, const char* username, const char* buddyUsername);
	static SendResult declineBuddy(const char* serviceName, const char* username, const char* buddyUsername);
	static SendResult sendMessage(const char *serviceName, const char *username, const char *usernameTo, const char *messageText);
	// webOS attachment send: transmit a local file (absolute path, must exist and be readable in the
	// transport process) to usernameTo. Mirrors sendMessage's account resolution + channel detection:
	// a group-channel target routes through serv_chat_send_file, a 1:1 IM through serv_send_file. All
	// of this build's prpls treat a non-NULL filename as an already-accepted xfer (no UI dialog).
	static SendResult sendFile(const char *serviceName, const char *username, const char *usernameTo, const char *filePath);
	static bool queuePresenceUpdates(bool enable);
	static bool deviceConnectionClosed(bool all, const char* ipAddress);
	static bool allAccountsOffline();
};

#endif /* LIBPURPLEADAPTER_H_ */
