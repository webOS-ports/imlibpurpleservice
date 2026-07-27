/*
 * LibpurpleAdapter.cpp
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

/*
 * This file includes code from pidgin  (nullclient.c)
 *
 * pidgin
 *
 * Pidgin is the legal property of its developers, whose names are too numerous
 * to list here.  Please refer to the COPYRIGHT file distributed with this
 * source distribution.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301  USA
 *
 */

#include "purple.h"

#include <glib.h>

#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <unordered_map>
#include <vector>
#include <set>

#include "Util.h"
#include "LibpurpleAdapter.h"
#include "PalmImCommon.h"
#include "AuthChannel.h"

//#include <cjson/json.h>
//#include <lunaservice.h>
//#include <json_utils.h>
#include "IMServiceApp.h"
#include "entities.h"       // decode_html_entities_utf8 - decode the app's &#NNNNN; emoji to UTF-8 for sendReaction
#include "OpusEncoder.h"    // wav_to_opus_voicenote - transcode a recorded WAV to an Ogg/Opus voice note


static const guint PURPLE_GLIB_READ_COND  = (G_IO_IN | G_IO_HUP | G_IO_ERR);
static const guint PURPLE_GLIB_WRITE_COND = (G_IO_OUT | G_IO_HUP | G_IO_ERR | G_IO_NVAL);
static const guint CONNECT_TIMEOUT_SECONDS = 45;
/* Discord's QR / remote-auth login waits for the user to scan the code and approve
 * sign-in on a second device (their phone), which routinely takes well over 45s. If
 * the normal connect timeout fires during that window it disconnects the account and
 * kills the in-flight ticket->token exchange HTTP request ("no json node"), so the
 * login silently fails and loops back to a fresh QR. Give interactive QR logins a much
 * longer grace period. */
static const guint QR_CONNECT_TIMEOUT_SECONDS = 300;

static LoginCallbackInterface* s_loginState = NULL;
static IMServiceCallbackInterface* s_imServiceHandler = NULL;
// Interactive-login (Discord QR) channel + the set of account keys whose current
// login is a disposable QR-preview (create-after-confirm). Preview logins route
// their connect callbacks to the AuthChannel, NOT to the webOS login-state machine.
static AuthChannel* s_authChannel = NULL;
static std::set<std::string> s_qrPreviewKeys;

// Pending Discord remote-auth captcha requests. The prpl raised purple_request_fields
// with read-only sitekey/rqdata/rqtoken + an editable "captcha_key" field and an OK
// callback. We hold the request keyed by account so submitCaptcha() can fill captcha_key
// with the UI-solved token and invoke the callback (which re-POSTs remote-auth/login).
typedef void (*PurpleRequestFieldsCbT)(void*, PurpleRequestFields*);
struct PendingCaptcha {
	PurpleRequestFields*   fields;
	PurpleRequestFieldsCbT okCb;
	void*                  userData;
};
static std::unordered_map<std::string, PendingCaptcha> s_pendingCaptcha;

std::hash<std::string> hash;

/**
 * List of accounts that are online
 */
static std::unordered_map<std::string, PurpleAccount*> s_onlineAccountData;
/**
 * List of accounts that are in the process of logging in
 */
static std::unordered_map<std::string, PurpleAccount*> s_pendingAccountData;
static std::unordered_map<std::string, PurpleAccount*> s_offlineAccountData;
static std::unordered_map<std::string, guint> s_accountLoginTimers;

// TODO - this does not seem to be used anymore...
static std::unordered_map<std::string, std::string> s_connectionTypeData;

static std::unordered_map<std::string, std::string> s_AccountIdsData;
// Perf (#3): last-seen avatar path per "accountKey\x1fbuddyName", so a presence tick with an
// unchanged avatar skips the per-buddy com.palm.contact find in updateBuddyStatus.
static std::unordered_map<std::string, std::string> s_lastBuddyAvatar;

/*
 * list of pending authorization requests
 */
static GHashTable* s_AuthorizeRequests = NULL;

static bool s_libpurpleInitialized = FALSE;
static bool s_registeredForAccountSignals = FALSE;
static bool s_registeredForPresenceUpdateSignals = FALSE;
/**
 * Keeps track of the local IP address that we bound to when logging in to individual accounts
 * key: accountKey, value: IP address
 */
static std::unordered_map<std::string, std::string> s_ipAddressesBoundTo;

typedef struct _IOClosure
{
	guint result;
	gpointer data;
	PurpleInputFunction function;
} IOClosure;

// used for processing buddy invite requests
typedef struct _auth_and_add
{
	PurpleAccountRequestAuthorizationCb auth_cb;
	PurpleAccountRequestAuthorizationCb deny_cb;
	void *data;
	char *remote_user;
	char *alias;
	PurpleAccount *account;
} AuthRequest;

struct AccountMetaData
{
    std::string account_key;
    std::string servicename;
};

static void incoming_message_cb(PurpleConversation *conv, const char *who, const char *alias, const char *message,	PurpleMessageFlags flags, time_t mtime);
// webOS reactions: handler for the "webos-im-reaction" signal a prpl emits; routes to the DB reaction merge.
static void im_reaction_cb(PurpleAccount* account, const char* targetServiceMessageId, const char* emoji, const char* sender, void* data);
// webOS: handler for "webos-im-outbox-id" - attaches a network id to the user's own app-sent message row.
static void im_outbox_id_cb(PurpleAccount* account, const char* serviceMessageId, const char* text, void* data);
// webOS delivery/read receipts: a prpl reports the recipient delivered/read our outgoing message.
// by-id (WhatsApp/Signal): (account, serviceMessageId, status). watermark (Telegram/Facebook/Teams):
// (account, scope, watermark, status). status is "delivered" or "read".
static void im_receipt_cb(PurpleAccount* account, const char* serviceMessageId, const char* status, void* data);
static void im_receipt_hwm_cb(PurpleAccount* account, const char* scope, const char* watermark, const char* status, void* data);
// webOS: register+connect all cross-prpl reaction signals ONCE, early (from initializeLibpurple), before
// any prpl logs in - so instant-reconnect prpls (whatsmeow) don't race the registration.
static void registerWebosReactionSignals();
static void im_reaction_set_cb(PurpleAccount* account, const char* targetServiceMessageId, const char* serialized, const char* unused, void* data);
static std::string getServiceNameFromPurpleAccount(PurpleAccount* account);
// human-friendly WhatsApp display name (push-name, else "+<phone>"); defined lower, used in incoming_message_cb
static std::string whatsAppDisplayName(const char* alias, const char* username);
// true if s is a bare Signal ACI UUID (8-4-4-4-12 hex); defined lower, used in incoming_message_cb
static bool isSignalUuid(const char* s);
static void adapterUIInit(void);
static GHashTable* getClientInfo(void);
static gboolean adapterInvokeIO(GIOChannel *source, GIOCondition condition, gpointer data);
static guint adapterIOAdd(gint fd, PurpleInputCondition condition, PurpleInputFunction function, gpointer data);

//Prompt for authorization when someone adds this account to their buddy list.
//To authorize them to see this account's presence, call authorize_cb (user_data); otherwise call deny_cb (user_data);
//Returns:
//    a UI-specific handle, as passed to close_account_request.
//    void(* _PurpleAccountUiOps::close_account_request)(void *ui_handle)
//    Close a pending request for authorization.
//    ui_handle is a handle as returned by request_authorize.
//Parameters:
//    	account 	The account that was added
//    	remote_user 	The name of the user that added this account.
//    	id 	The optional ID of the local account. Rarely used.
//    	alias 	The optional alias of the remote user.
//    	message 	The optional message sent by the user wanting to add you.
//    	on_list 	Is the remote user already on the buddy list?
//    	auth_cb 	The callback called when the local user accepts
//    	deny_cb 	The callback called when the local user rejects
//    	user_data 	Data to be passed back to the above callbacks
static void *request_authorize_cb (PurpleAccount *account,
	const char *remote_user,
	const char *id,
	const char *alias,
	const char *message,
	gboolean on_list,
	PurpleAccountRequestAuthorizationCb authorize_cb,
	PurpleAccountRequestAuthorizationCb deny_cb,
	void *user_data);

void request_add_cb (PurpleAccount *account, const char *remote_user, const char *id, const char *alias, const char *message);

// AccountUIOps:
//    /** A buddy who is already on this account's buddy list added this account
//-	   *  to their buddy list.
//     */
//-	void (*notify_added)(PurpleAccount *account,
//-	                     const char *remote_user,
//-	                     const char *id,
//-	                     const char *alias,
//-	                     const char *message);
//-
//-	/** This account's status changed. */
//-	void (*status_changed)(PurpleAccount *account,
//-	                       PurpleStatus *status);
//-
//-	/** Someone we don't have on our list added us; prompt to add them. */
//-	void (*request_add)(PurpleAccount *account,
//-	                    const char *remote_user,
//-	                    const char *id,
//-	                    const char *alias,
//-	                    const char *message);
//-
//-	/** Prompt for authorization when someone adds this account to their buddy
//-	 * list.  To authorize them to see this account's presence, call \a
//-	 * authorize_cb (\a user_data); otherwise call \a deny_cb (\a user_data);
//-	 * @return a UI-specific handle, as passed to #close_account_request.
//-	 */
//-	void *(*request_authorize)(PurpleAccount *account,
//-	                           const char *remote_user,
//-	                           const char *id,
//-	                           const char *alias,
//-	                           const char *message,
//-	                           gboolean on_list,
//-	                           PurpleAccountRequestAuthorizationCb authorize_cb,
//-	                           PurpleAccountRequestAuthorizationCb deny_cb,
//-	                           void *user_data);
//-
//-	/** Close a pending request for authorization.  \a ui_handle is a handle
//-	 *  as returned by #request_authorize.
//-	 */
//-	void (*close_account_request)(void *ui_handle);
static PurpleAccountUiOps adapterAccountUIOps =
{
	NULL,                  //  notify_added
	NULL,                  //  status_changed
	request_add_cb,        //  request_add
	request_authorize_cb,  //  request_authorize
	NULL,                  //  close_account_request,

	// padding
	NULL,
	NULL,
	NULL,
	NULL,
};

static PurpleCoreUiOps adapterCoreUIOps =
{
	NULL, NULL, adapterUIInit, NULL, getClientInfo, NULL, NULL, NULL
};

static PurpleEventLoopUiOps adapterEventLoopUIOps =
{
	g_timeout_add, g_source_remove, adapterIOAdd, g_source_remove, NULL, g_timeout_add_seconds, NULL, NULL, NULL
};

static PurpleConversationUiOps adapterConversationUIOps  =
{
	NULL, NULL, NULL, NULL, incoming_message_cb, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	NULL, NULL
};

/* webOS: Discord's remote-auth (QR) login raises the QR through purple_request_fields
 * (an image field "qr_image" + a string "qr_string"). The stock transport installed no
 * request ui-ops, so the prpl fell back to dumping the QR into a chat conversation.
 * Install a request_fields op that forwards the QR image to the AuthChannel, which
 * surfaces it to the accounts auth UI for inline QR sign-in. Only request_fields is set;
 * request_input is deliberately left NULL so Telegram's login-code / 2FA capture keeps
 * its existing sendMessage-routing path untouched. */
static void* adapter_request_fields(const char *title, const char *primary, const char *secondary,
        PurpleRequestFields *fields, const char *ok_text, GCallback ok_cb,
        const char *cancel_text, GCallback cancel_cb, PurpleAccount *account,
        const char *who, PurpleConversation *conv, void *user_data)
{
	// Resolve the account: Discord passes account=NULL, who=username. Fall back to a
	// pending account whose username matches `who`.
	PurpleAccount* acct = account;
	if (acct == NULL && who != NULL)
	{
		for (std::unordered_map<std::string, PurpleAccount*>::iterator it = s_pendingAccountData.begin();
		     it != s_pendingAccountData.end(); ++it)
		{
			PurpleAccount* a = it->second;
			if (a && a->username && strcmp(a->username, who) == 0) { acct = a; break; }
		}
	}
	if (acct == NULL || acct->ui_data == NULL)
	{
		MojLogError(IMServiceApp::s_log, _T("adapter_request_fields: could not resolve account (who=%s)"), who ? who : "");
		return NULL;
	}

	const guchar* imgData = NULL;
	gsize imgLen = 0;
	const char* qrString = NULL;
	// WhatsApp (purple-gowhatsapp) offers device-linking as a QR image AND an 8-char
	// pairing code in the same request; it names the QR-payload string "qr_data" (Discord
	// uses "qr_string") and the code "pairing_code". Surface the pairing code as urlString
	// (it's the more reliable path on a small screen), falling back to the QR payload.
	const char* pairingCode = NULL;
	// Captcha fields (Discord remote-auth hCaptcha). Presence of captcha_sitekey marks
	// this request as a captcha challenge rather than the QR image.
	const char* capService = NULL;
	const char* capSitekey = NULL;
	const char* capRqData = NULL;
	const char* capRqToken = NULL;

	GList* groups = purple_request_fields_get_groups(fields);
	for (; groups != NULL; groups = groups->next)
	{
		PurpleRequestFieldGroup* group = (PurpleRequestFieldGroup*)groups->data;
		GList* flds = purple_request_field_group_get_fields(group);
		for (; flds != NULL; flds = flds->next)
		{
			PurpleRequestField* f = (PurpleRequestField*)flds->data;
			const char* id = purple_request_field_get_id(f);
			PurpleRequestFieldType t = purple_request_field_get_type(f);
			if (t == PURPLE_REQUEST_FIELD_IMAGE && id && strcmp(id, "qr_image") == 0)
			{
				imgData = (const guchar*)purple_request_field_image_get_buffer(f);
				imgLen = purple_request_field_image_get_size(f);
			}
			else if (t == PURPLE_REQUEST_FIELD_STRING && id &&
			         (strcmp(id, "qr_string") == 0 || strcmp(id, "qr_data") == 0))
			{
				qrString = purple_request_field_string_get_value(f);
			}
			else if (t == PURPLE_REQUEST_FIELD_STRING && id && strcmp(id, "pairing_code") == 0)
			{
				pairingCode = purple_request_field_string_get_value(f);
			}
			else if (t == PURPLE_REQUEST_FIELD_STRING && id && strcmp(id, "captcha_sitekey") == 0)
				capSitekey = purple_request_field_string_get_value(f);
			else if (t == PURPLE_REQUEST_FIELD_STRING && id && strcmp(id, "captcha_service") == 0)
				capService = purple_request_field_string_get_value(f);
			else if (t == PURPLE_REQUEST_FIELD_STRING && id && strcmp(id, "captcha_rqdata") == 0)
				capRqData = purple_request_field_string_get_value(f);
			else if (t == PURPLE_REQUEST_FIELD_STRING && id && strcmp(id, "captcha_rqtoken") == 0)
				capRqToken = purple_request_field_string_get_value(f);
		}
	}

	std::string const& serviceName = getServiceNameFromPurpleAccount(acct);
	// acct->ui_data (checked non-NULL above) is our AccountMetaData carrying the account key.
	std::string const  accountKey  = ((AccountMetaData*)acct->ui_data)->account_key;

	// Captcha challenge: hold the request (fields + ok_cb + user_data) so submitCaptcha
	// can complete it once the UI solves the hCaptcha, then surface it to the UI.
	if (capSitekey != NULL)
	{
		PendingCaptcha pc;
		pc.fields   = fields;
		pc.okCb     = (PurpleRequestFieldsCbT)ok_cb;
		pc.userData = user_data;
		s_pendingCaptcha[accountKey] = pc;

		MojLogInfo(IMServiceApp::s_log, _T("adapter_request_fields: CAPTCHA for service=%s user=%s (sitekey=%s)"),
		           serviceName.c_str(), acct->username ? acct->username : "", capSitekey);

		if (s_authChannel)
			s_authChannel->publishCaptchaChallenge(serviceName.c_str(), acct->username,
			                                       capService, capSitekey, capRqData, capRqToken);

		// Return a non-NULL handle so the prpl treats the request as accepted. We keep
		// ownership of `fields` and free it in submitCaptcha after invoking the callback.
		return (void*)fields;
	}

	// Prefer the pairing code as the surfaced urlString when the prpl provided one
	// (WhatsApp); otherwise the raw QR payload (Discord's qr_string / gowhatsapp's qr_data).
	const char* urlString = (pairingCode && *pairingCode) ? pairingCode : qrString;

	MojLogInfo(IMServiceApp::s_log, _T("adapter_request_fields: QR for service=%s user=%s (%u img bytes, pairing=%s)"),
	           serviceName.c_str(), acct->username ? acct->username : "", (unsigned)imgLen,
	           (pairingCode && *pairingCode) ? "yes" : "no");

	if (s_authChannel)
		s_authChannel->publishQRChallenge(serviceName.c_str(), acct->username, imgData, imgLen, "image/png", urlString);

	return NULL;   // no ui handle to track
}

static PurpleRequestUiOps adapterRequestUIOps =
{
	NULL,                    // request_input  (left NULL: Telegram keeps its sendMessage path)
	NULL,                    // request_choice
	NULL,                    // request_action
	adapter_request_fields,  // request_fields (Discord QR)
	NULL,                    // request_file
	NULL,                    // close_request
	NULL,                    // request_folder
	NULL,                    // request_action_with_icon
	NULL,                    // _purple_reserved1
	NULL                     // _purple_reserved2
};

// useful for debugging
static void authRequest_log_func(gpointer key, gpointer value, gpointer ud)
{
	AuthRequest *aa = (AuthRequest *)value;
	MojLogInfo(IMServiceApp::s_log, _T("  AuthRequest: key = %s, requester = %s"), (char*)key, aa->remote_user);
}
static void logAuthRequestTableValues()
{
	MojLogInfo(IMServiceApp::s_log, _T("Authorize Request Table:"));
	g_hash_table_foreach(s_AuthorizeRequests, authRequest_log_func, NULL);
}


void adapterUIInit(void)
{
	purple_conversations_set_ui_ops(&adapterConversationUIOps);
	purple_accounts_set_ui_ops(&adapterAccountUIOps);
	// Surface interactive-login challenges (Discord QR) to the accounts UI instead of chat.
	purple_request_set_ui_ops(&adapterRequestUIOps);
}

void destroyNotify(gpointer dataToFree)
{
	g_free(dataToFree);
}

gboolean adapterInvokeIO(GIOChannel* ioChannel, GIOCondition ioCondition, gpointer data)
{
	IOClosure* ioClosure = (IOClosure*)data;
	int purpleCondition = 0;

	if (PURPLE_GLIB_READ_COND & ioCondition)
	{
		purpleCondition = purpleCondition | PURPLE_INPUT_READ;
	}

	if (PURPLE_GLIB_WRITE_COND & ioCondition)
	{
		purpleCondition = purpleCondition | PURPLE_INPUT_WRITE;
	}

	ioClosure->function(ioClosure->data, g_io_channel_unix_get_fd(ioChannel), (PurpleInputCondition)purpleCondition);

	return TRUE;
}

guint adapterIOAdd(gint fd, PurpleInputCondition purpleCondition, PurpleInputFunction inputFunction, gpointer data)
{
	GIOChannel* ioChannel;
	unsigned int ioCondition = 0;
	IOClosure* ioClosure = g_new0(IOClosure, 1);

	ioClosure->data = data;
	ioClosure->function = inputFunction;

	if (PURPLE_INPUT_READ & purpleCondition)
	{
		ioCondition = ioCondition | PURPLE_GLIB_READ_COND;
	}

	if (PURPLE_INPUT_WRITE & purpleCondition)
	{
		ioCondition = ioCondition | PURPLE_GLIB_WRITE_COND;
	}

	ioChannel = g_io_channel_unix_new(fd);
	ioClosure->result = g_io_add_watch_full(ioChannel, G_PRIORITY_DEFAULT, (GIOCondition)ioCondition, adapterInvokeIO, ioClosure,
			destroyNotify);

	g_io_channel_unref(ioChannel);
	return ioClosure->result;
}

/*
 * Helper methods
 */

static std::string stripResourceFromJabberUsername(std::string const& username, std::string const& serviceName)
{
	if (serviceName != "type_jabber")
	{
		return username;
	}

	return username.substr(0,username.find_last_of("/"));
}

/*
 * Given mojo-friendly serviceName, it will return prpl-specific protocol_id (e.g. given "type_aim", it will return "prpl-aim")
 * Free the returned string when you're done with it
 */
static std::string getPrplProtocolIdFromServiceName(std::string const& serviceName)
{
	// webOS Teams port: purple-teams (EionRobb) registers its OWN upstream protocol id, which the
	// generic "prpl-" + <type> transform below cannot derive from the "type_teams" service name. Prefer
	// the plugin's NATIVE personal-build id ("prpl-eionrobb-msteams-personal") so the plugin can be
	// built STOCK from upstream with no webOS-specific id patch (-DTEAMS_PERSONAL_PLUGIN_ID); fall back
	// to the legacy "prpl-teams-personal" that older webOS builds forced, so either plugin build works.
	// The service name "type_teams" (baked into db8 kinds / capability ids) stays decoupled either way.
	if (serviceName == "type_teams")
	{
		if (purple_find_prpl("prpl-eionrobb-msteams-personal") != NULL)
			return "prpl-eionrobb-msteams-personal";
		return "prpl-teams-personal";
	}
	// Signal maps to hoehermann/purple-presage (prpl-hehoe-presage) — the native Rust
	// backend (no JVM). This replaces the old JVM-based purple-signal (prpl-hehoe-signal),
	// which ran but was too slow (interpreter-only OpenJDK Zero) for Signal's provisioning
	// handshake. libpresage.so is a single cross-built armv7 .so (presage + libsignal-rs +
	// SQLCipher, rustls/ring TLS); see messaging/signal/build-presage.sh.
	if (serviceName == "type_signal")
	{
		return "prpl-hehoe-presage";
	}
	// hoehermann/purple-gowhatsapp (whatsmeow branch) registers as "prpl-hehoe-whatsmeow";
	// keep the db8/capability service name "type_whatsapp" decoupled from the plugin id.
	if (serviceName == "type_whatsapp")
	{
		return "prpl-hehoe-whatsmeow";
	}
	std::string prplProtocolIdToReturn = "prpl-" + serviceName.substr(strlen("type_"), std::string::npos);
	return prplProtocolIdToReturn;
}

// Inverse of getPrplProtocolIdFromServiceName(): map a loaded prpl's protocol_id back to the
// webOS db8/capability service name ("type_..."). MUST mirror the special-cases above, or an
// account whose plugin id does not follow the generic "prpl-<type>" pattern gets registered under
// the wrong service key. That breaks the auto-login path (account_logged_in_cb repairs ui_data from
// the account itself): the account comes online under e.g. "type_hehoe-whatsmeow" while the webOS
// login() for "type_whatsapp" can't find/adopt it, so its connect timer never disarms and
// connectTimeoutCallback force-disconnects the healthy session (WhatsApp/Signal never go online).
static std::string getServiceNameFromPrplProtocolId(const char* prplProtocolId)
{
	std::string prpl = prplProtocolId ? prplProtocolId : "";
	// Accept BOTH the native upstream id and the legacy webOS-forced id (see the forward map above).
	if (prpl == "prpl-teams-personal" || prpl == "prpl-eionrobb-msteams-personal")
		return "type_teams";
	if (prpl == "prpl-hehoe-presage")
		return "type_signal";
	if (prpl == "prpl-hehoe-whatsmeow")
		return "type_whatsapp";
	if (prpl.compare(0, strlen("prpl-"), "prpl-") == 0)
		return "type_" + prpl.substr(strlen("prpl-"));
	return prpl;
}

static const char* getMojoFriendlyErrorCode(PurpleConnectionError type)
{
	const char* mojoFriendlyErrorCode;
	if (type == PURPLE_CONNECTION_ERROR_INVALID_USERNAME)
	{
		mojoFriendlyErrorCode = ERROR_BAD_USERNAME;
	}
	else if (type == PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED)
	{
		mojoFriendlyErrorCode = ERROR_AUTHENTICATION_FAILED;
	}
	else if (type == PURPLE_CONNECTION_ERROR_NETWORK_ERROR)
	{
		mojoFriendlyErrorCode = ERROR_NETWORK_ERROR;
	}
	else if (type == PURPLE_CONNECTION_ERROR_NAME_IN_USE)
	{
		mojoFriendlyErrorCode = ERROR_NAME_IN_USE;
	}
	else
	{
		MojLogInfo(IMServiceApp::s_log, _T("PurpleConnectionError was %i"), type);
		mojoFriendlyErrorCode = ERROR_GENERIC_ERROR;
	}
	return mojoFriendlyErrorCode;
}

static std::string getAccountKey(std::string const& username, std::string const& serviceName)
{
	// WhatsApp identity arrives in three interchangeable forms: the +E.164 the webOS account layer
	// now stores for display ("+31652044684"), whatsmeow's device-ID JID ("31652044684@s.whatsapp.net")
	// that the prpl renames the account to once pairing completes, and (older paths) the bare digits.
	// Normalize all three to the bare digits so a WhatsApp account maps to ONE stable transport key --
	// otherwise sendMessage, preview adoption, and post-restart auto-login would key the same account
	// differently and "lose" the logged-in session.
	std::string key = username;
	static const std::string waSuffix = "@s.whatsapp.net";
	if (key.size() > waSuffix.size() &&
	    key.compare(key.size() - waSuffix.size(), waSuffix.size(), waSuffix) == 0)
		key.erase(key.size() - waSuffix.size());
	// Strip the display '+' only for WhatsApp: Signal/Telegram usernames are legitimately +E.164 and
	// must keep it (their key must stay distinct from any bare-digit form).
	if (serviceName == "type_whatsapp" && !key.empty() && key[0] == '+')
		key.erase(0, 1);
	return key + "_" + serviceName;
}

// Map a webOS-side WhatsApp username (+E.164 for display, or bare digits) to the JID whatsmeow
// requires ("<digits>@s.whatsapp.net"): gowhatsapp compares the purple account username against its
// device ID and errors ("username does not match the main device's ID") on anything else. Idempotent
// when already a JID; a no-op for every other service (their username reaches the prpl verbatim).
static std::string getPurpleUsername(std::string const& username, std::string const& serviceName)
{
	if (serviceName != "type_whatsapp")
		return username;
	// Anything already carrying an '@' is a real whatsmeow id -- the phone JID
	// ("<digits>@s.whatsapp.net"), an opaque linked id ("<id>@lid"), or a group ("<id>@g.us") -- and
	// must pass through untouched. Only the display +E.164 form ("+31638307067") or bare digits, which
	// have no '@', get mapped to the device-JID the prpl needs. This makes the helper safe for buddy
	// addresses (which can be @lid/@g.us), not just the account's own phone username.
	if (username.find('@') != std::string::npos)
		return username;
	std::string digits;
	for (std::string::size_type i = 0; i < username.size(); ++i)
		if (username[i] >= '0' && username[i] <= '9')
			digits += username[i];
	if (digits.empty())
		return username;
	return digits + "@s.whatsapp.net";
}

// Inverse of getPurpleUsername: map a prpl account's username BACK to the webOS-side username the
// account layer keys everything on. For WhatsApp the purple account is the device JID
// ("31652044684@s.whatsapp.net") but the webOS account + imloginstate + immessage records use +E.164
// ("+31652044684"). Any callback that reports an account owner to the webOS layer (login state, buddy
// re-sync, incoming message) MUST translate, or it targets a username no webOS record is keyed on --
// e.g. requestBuddyResync's imloginstate bump silently matches nothing and the buddy sync never runs.
// No-op for every other service and for an already-+E.164/foreign form.
static std::string getWebosUsername(const char* purpleUsername, std::string const& serviceName, PurpleAccount* account = NULL)
{
	std::string u(purpleUsername ? purpleUsername : "");
	if (serviceName == "type_whatsapp")
	{
		static const std::string waSuffix = "@s.whatsapp.net";
		if (u.size() > waSuffix.size() &&
		    u.compare(u.size() - waSuffix.size(), waSuffix.size(), waSuffix) == 0)
		{
			std::string digits = u.substr(0, u.size() - waSuffix.size());
			if (!digits.empty() && digits[0] != '+')
				return "+" + digits;
			return digits;
		}
		return u;
	}
	// webOS Signal: contacts are keyed by their ACI UUID, which never matches a phone-based person
	// record, so a Signal buddy/message would not merge with the contact (unlike WhatsApp). presage
	// stashes the contact's phone as a "phone_number" buddy attribute when Signal shares it; use that as
	// the webOS ims.value (+E.164) so the Signal identity merges with the same phone contact. presage's
	// send accepts +E.164 (classify_recipient -> resolve_phone_to_uuid), so the reverse path just passes
	// it through. Falls back to the UUID when no phone is known (phone-number sharing off).
	if (serviceName == "type_signal" && account != NULL && isSignalUuid(u.c_str()))
	{
		PurpleBuddy* b = purple_find_buddy(account, u.c_str());
		if (b != NULL)
		{
			const char* phone = purple_blist_node_get_string(&b->node, "phone_number");
			if (phone && *phone)
			{
				std::string p(phone);
				if (p[0] != '+')
					p = "+" + p;
				return p;
			}
		}
	}
	return u;
}

static char* getAuthRequestKey(const char* username, const char* serviceName, const char* remoteUsername)
{
	if (!username)
	{
		MojLogError(IMServiceApp::s_log, _T("getAuthRequestKey - empty username"));
		return strdup("");
	}
	if (!serviceName)
	{
		MojLogError(IMServiceApp::s_log, _T("getAuthRequestKey - empty serviceName"));
		return strdup("");
	}
	if (!remoteUsername)
	{
		MojLogError(IMServiceApp::s_log, _T("getAuthRequestKey - empty remoteUsername"));
		return strdup("");
	}
	char *authRequestKey = NULL;
	// asprintf allocates appropriate-sized buffer
	asprintf(&authRequestKey, "%s_%s_%s", username, serviceName, remoteUsername);
	MojLogInfo(IMServiceApp::s_log, _T("getAuthRequestKey - username: %s, serviceName: %s, remoteUser: %s key: %s"), username, serviceName, remoteUsername, authRequestKey);
	return authRequestKey;
}

static std::string const& getAccountKeyFromPurpleAccount(PurpleAccount* account)
{
    static const std::string empty = "";
	if (!account || !account->ui_data)
	{
		MojLogError(IMServiceApp::s_log, _T("getAccountKeyFromPurpleAccount called with empty account"));
		return empty;
	}
	return ((AccountMetaData*)account->ui_data)->account_key;
}

static std::string getServiceNameFromPurpleAccount(PurpleAccount* account)
{
	if (account == NULL)
	{
		MojLogError(IMServiceApp::s_log, _T("getServiceNameFromPurpleAccount called with NULL account"));
		return "";
	}
	// Prefer the servicename stashed in ui_data at login/adoption.
	if (account->ui_data)
		return ((AccountMetaData*)account->ui_data)->servicename;
	// ui_data isn't set yet when libpurple auto-logged the account in before the transport tracked it
	// (see the adoption note in login()). Derive the service from the prpl protocol id so callbacks
	// (incoming messages, buddy status) still resolve it -- otherwise an empty serviceName makes the
	// WhatsApp +E.164 translation (and other per-service logic) silently no-op and the JID leaks through.
	const char* prpl = account->protocol_id;
	if (prpl != NULL && *prpl != '\0')
		return getServiceNameFromPrplProtocolId(prpl);
	MojLogError(IMServiceApp::s_log, _T("getServiceNameFromPurpleAccount: account has neither ui_data nor protocol_id"));
	return "";
}

/**
 * Returns a GString containing the special stanza to enable server-side presence update queue
 * Clean up after yourself using g_string_free when you're done with the return value
 */
static GString* getEnableQueueStanza(PurpleAccount* account)
{
	GString* stanza = NULL;
	if (account != NULL)
	{
		if (strcmp(account->protocol_id, "prpl-jabber") == 0)
		{
			stanza = g_string_new("");
			PurpleConnection* pc = purple_account_get_connection(account);
			if (pc == NULL)
			{
				return NULL;
			}
			const char* displayName = purple_connection_get_display_name(pc);
			if (displayName == NULL)
			{
				return NULL;
			}
			g_string_append(stanza, "<iq from='");
			g_string_append(stanza, displayName);
			g_string_append(stanza, "' type='set'><query xmlns='google:queue'><enable/></query></iq>");
		}
		else if (strcmp(account->protocol_id, "prpl-aim") == 0)
		{
			MojLogInfo(IMServiceApp::s_log, _T("getEnableQueueStanza for AIM"));
			stanza = g_string_new("true");
		}
	}
	return stanza;
}

/**
 * Returns a GString containing the special stanza to disable and flush the server-side presence update queue
 * Clean up after yourself using g_string_free when you're done with the return value
 */
static GString* getDisableQueueStanza(PurpleAccount* account)
{
	GString* stanza = NULL;
	if (account != NULL)
	{
		if (strcmp(account->protocol_id, "prpl-jabber") == 0)
		{
			stanza = g_string_new("");
			PurpleConnection* pc = purple_account_get_connection(account);
			if (pc == NULL)
			{
				return NULL;
			}
			const char* displayName = purple_connection_get_display_name(pc);
			if (displayName == NULL)
			{
				return NULL;
			}
			g_string_append(stanza, "<iq from='");
			g_string_append(stanza, displayName);
			g_string_append(stanza, "' type='set'><query xmlns='google:queue'><disable/><flush/></query></iq>");
		}
		else if (strcmp(account->protocol_id, "prpl-aim") == 0)
		{
			MojLogInfo(IMServiceApp::s_log, _T("getDisableQueueStanza for AIM"));
			stanza = g_string_new("false");
		}
	}
	return stanza;
}

static void enableServerQueueForAccount(PurpleAccount* account)
{
	if (!account)
	{
		MojLogError(IMServiceApp::s_log, _T("enableServerQueueForAccount called with empty account"));
		return;
	}

	PurplePluginProtocolInfo* prpl_info = NULL;
	PurpleConnection* gc = purple_account_get_connection(account);
	PurplePlugin* prpl = NULL;

	if (gc != NULL)
	{
		prpl = purple_connection_get_prpl(gc);
	}

	if (prpl != NULL)
	{
		prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(prpl);
	}

	if (prpl_info && prpl_info->send_raw)
	{
		GString* enableQueueStanza = getEnableQueueStanza(account);
		if (enableQueueStanza != NULL)
		{
			MojLogInfo(IMServiceApp::s_log, _T("Enabling server queue for %s"), account->protocol_id);
			prpl_info->send_raw(gc, enableQueueStanza->str, enableQueueStanza->len);
			g_string_free(enableQueueStanza, TRUE);
		}
	}
}

static void disableServerQueueForAccount(PurpleAccount* account)
{
	if (!account)
	{
		MojLogError(IMServiceApp::s_log, _T("disableServerQueueForAccount called with empty account"));
		return;
	}
	PurplePluginProtocolInfo* prpl_info = NULL;
	PurpleConnection* gc = purple_account_get_connection(account);
	PurplePlugin* prpl = NULL;

	if (gc != NULL)
	{
		prpl = purple_connection_get_prpl(gc);
	}

	if (prpl != NULL)
	{
		prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(prpl);
	}

	if (prpl_info && prpl_info->send_raw)
	{
		GString* disableQueueStanza = getDisableQueueStanza(account);
		if (disableQueueStanza != NULL)
		{
			MojLogInfo(IMServiceApp::s_log, _T("Disabling server queue"));
			prpl_info->send_raw(gc, disableQueueStanza->str, disableQueueStanza->len);
			g_string_free(disableQueueStanza, TRUE);
		}
	}
}

/**
 * Asking the gtalk server to enable/disable queueing of presence updates
 * This is called when the screen is turned off (enable:true) or turned on (enable:false)
 */
bool LibpurpleAdapter::queuePresenceUpdates(bool enable)
{
	typedef std::unordered_map<std::string, PurpleAccount*>::const_iterator
		iter_type;

	for (iter_type i = s_onlineAccountData.begin(); i != s_onlineAccountData.end(); ++i)
	{
		PurpleAccount* account = i->second;
		if (account)
		{
			/*
			 * enabling/disabling server queue is supported by gtalk (jabber) or aim
			 */
 			if ((strcmp(account->protocol_id, "prpl-jabber") == 0) ||
			    (strcmp(account->protocol_id, "prpl-aim") == 0))
 			{
				if (enable)
				{
					enableServerQueueForAccount(account);
				}
				else
				{
					disableServerQueueForAccount(account);
				}
 			}
		}
	}
	return TRUE;
}


static int getPalmAvailabilityFromPurpleAvailability(int prplAvailability)
{
	switch (prplAvailability)
	{
	case PURPLE_STATUS_UNSET:
		return PalmAvailability::NO_PRESENCE;
	case PURPLE_STATUS_OFFLINE:
		return PalmAvailability::OFFLINE;
	case PURPLE_STATUS_AVAILABLE:
		return PalmAvailability::ONLINE;
	case PURPLE_STATUS_UNAVAILABLE:
		return PalmAvailability::IDLE;
	case PURPLE_STATUS_INVISIBLE:
		return PalmAvailability::INVISIBLE;
	case PURPLE_STATUS_AWAY:
		return PalmAvailability::IDLE;
	case PURPLE_STATUS_EXTENDED_AWAY:
		return PalmAvailability::IDLE;
	case PURPLE_STATUS_MOBILE:
		return PalmAvailability::MOBILE;
	case PURPLE_STATUS_TUNE:
		return PalmAvailability::ONLINE;
	default:
		return PalmAvailability::OFFLINE;
	}
}


static PurpleStatusPrimitive getPurpleAvailabilityFromPalmAvailability(int palmAvailability)
{
	switch (palmAvailability)
	{
	case PalmAvailability::ONLINE:
		return PURPLE_STATUS_AVAILABLE;
	case PalmAvailability::MOBILE:
		return PURPLE_STATUS_MOBILE;
	case PalmAvailability::IDLE:
		return PURPLE_STATUS_AWAY;
	case PalmAvailability::INVISIBLE:
		return PURPLE_STATUS_INVISIBLE;
	case PalmAvailability::OFFLINE:
		return PURPLE_STATUS_OFFLINE;
	default:
		return PURPLE_STATUS_OFFLINE;
	}
}

/*
 * End of helper methods
 */

/*
 * Callbacks
 */

// forward decl (defined below): batched presence coalescer, shared with buddy_status_changed_cb.
static void queuePresenceUpdate(const char* accountId, const char* serviceName, const char* username, int availability, const char* customMessage, const char* groupName);

static void buddy_signed_on_off_cb(PurpleBuddy* buddy, gpointer data)
{
//	LSError lserror;
//	LSErrorInit(&lserror);

	PurpleAccount* account = purple_buddy_get_account(buddy);

	std::string const& accountKey = getAccountKeyFromPurpleAccount(account);

	if (s_AccountIdsData.count(accountKey) == 0)
	{
		MojLogError(IMServiceApp::s_log, _T("buddy_signed_on_off_cb: accountId not found in table."));
		return;
	}

	std::string const& accountId = s_AccountIdsData[accountKey];

	std::string const& serviceName = getServiceNameFromPurpleAccount(account);
	PurpleStatus* activeStatus = purple_presence_get_active_status(purple_buddy_get_presence(buddy));
	/*
	 * Getting the new availability
	 */
	int newStatusPrimitive = purple_status_type_get_primitive(purple_status_get_type(activeStatus));
	int newAvailabilityValue = getPalmAvailabilityFromPurpleAvailability(newStatusPrimitive);
	PurpleBuddyIcon* icon = purple_buddy_get_icon(buddy);
	const char* customMessage = "";
	char* buddyAvatarLocation = NULL;

	if (icon != NULL)
	{
		buddyAvatarLocation = purple_buddy_icon_get_full_path(icon);
	}

	if (buddy->alias == NULL)
	{
		buddy->alias = (char*)"";
	}

	customMessage = purple_status_get_attr_string(activeStatus, "message");
	if (customMessage == NULL)
	{
		customMessage = "";
	}

	PurpleGroup* group = purple_buddy_get_group(buddy);
	const char* groupName = purple_group_get_name(group);
	if (groupName == NULL)
	{
		groupName = "";
	}

	// call into the imlibpurpletransport
	// buddy->name is stored in the imbuddyStatus DB kind in the libpurple format - ie. for AIM without the "@aol.com" so that is how we need to search for it
	// WhatsApp: report under the +E.164 address so the status keys the same as the contact's ims.value
	// (otherwise the JID-keyed status never matches the "+<phone>"-keyed contact -> buddy shows offline).
	std::string const buddyWebosName = getWebosUsername(buddy->name, serviceName, purple_buddy_get_account(buddy));
	// Perf (#2/#3): mirror buddy_status_changed_cb. A login/relogin/roam signs EVERY buddy on at once,
	// so a per-buddy find+merge here was THE buddy-sync bottleneck (hundreds of serial db8 round-trips
	// ~467ms each). Gate the avatar (skip the contact find when it hasn't changed) and route
	// presence-only ticks through the batched queuePresenceUpdate (one find + one batched merge/put
	// per account); only a genuinely changed/first-seen avatar takes the immediate per-buddy path.
	const char* avatarToForward = buddyAvatarLocation;
	{
		std::string avatarKey = accountKey;
		avatarKey.push_back('\x1f');
		avatarKey.append(buddy->name ? buddy->name : "");
		std::string currentAvatar = buddyAvatarLocation ? buddyAvatarLocation : "";
		std::unordered_map<std::string, std::string>::iterator la = s_lastBuddyAvatar.find(avatarKey);
		if (la != s_lastBuddyAvatar.end() && la->second == currentAvatar)
		{
			avatarToForward = NULL; // unchanged -> skip the contact find/update
		}
		else
		{
			s_lastBuddyAvatar[avatarKey] = currentAvatar;
		}
	}
	if (avatarToForward != NULL)
	{
		s_imServiceHandler->updateBuddyStatus(accountId.c_str(), serviceName.c_str(), buddyWebosName.c_str(), newAvailabilityValue, customMessage, groupName, avatarToForward);
	}
	else
	{
		queuePresenceUpdate(accountId.c_str(), serviceName.c_str(), buddyWebosName.c_str(), newAvailabilityValue, customMessage, groupName);
	}

	g_message(
			"%s says: %s's presence: availability: '%i', custom message: '%s', avatar location: '%s', display name: '%s', group name: '%s'",
			__FUNCTION__, buddy->name, newAvailabilityValue, customMessage, buddyAvatarLocation, buddy->alias, groupName);

	if (buddyAvatarLocation)
	{
		g_free(buddyAvatarLocation);
	}
}

// ---- Perf (#2): coalesce per-buddy presence ticks and flush them as ONE batched db8 write. -------
// libpurple emits buddy-status-changed one buddy at a time; during a login/contact-sync burst that
// meant a find+merge round-trip PER buddy (measured ~467ms/op under load). We buffer presence-only
// ticks per account and, after a short quiet window, hand the whole set to updateBuddyStatusBatch,
// which does a single find + one batched merge/put (~14ms/op amortized).
struct PendingPresence
{
	std::string serviceName;
	int availability;
	std::string customMessage;
	std::string groupName;
};
static std::unordered_map<std::string, std::unordered_map<std::string, PendingPresence> > s_pendingPresence; // accountId -> username -> latest
static guint s_presenceFlushTimer = 0;
#define PRESENCE_FLUSH_DEBOUNCE_SECONDS 3

static gboolean presenceFlushCallback(gpointer /*data*/)
{
	s_presenceFlushTimer = 0;
	if (s_imServiceHandler == NULL)
	{
		s_pendingPresence.clear();
		return FALSE;
	}
	for (std::unordered_map<std::string, std::unordered_map<std::string, PendingPresence> >::iterator ai = s_pendingPresence.begin(); ai != s_pendingPresence.end(); ++ai)
	{
		if (ai->second.empty())
			continue;
		std::string serviceName = ai->second.begin()->second.serviceName;
		MojObject updates; // array of { username, availability, status, group }
		for (std::unordered_map<std::string, PendingPresence>::iterator ui = ai->second.begin(); ui != ai->second.end(); ++ui)
		{
			MojObject o;
			o.putString(_T("username"), ui->first.c_str());
			o.putInt(_T("availability"), ui->second.availability);
			o.putString(_T("status"), ui->second.customMessage.c_str());
			o.putString(_T("group"), ui->second.groupName.c_str());
			updates.push(o);
		}
		s_imServiceHandler->updateBuddyStatusBatch(ai->first.c_str(), serviceName.c_str(), updates);
	}
	s_pendingPresence.clear();
	return FALSE; // one-shot
}

static void queuePresenceUpdate(const char* accountId, const char* serviceName, const char* username, int availability, const char* customMessage, const char* groupName)
{
	if (accountId == NULL || username == NULL)
		return;
	PendingPresence& p = s_pendingPresence[accountId][username]; // keep only the LATEST tick per buddy
	p.serviceName = serviceName ? serviceName : "";
	p.availability = availability;
	p.customMessage = customMessage ? customMessage : "";
	p.groupName = groupName ? groupName : "";
	if (s_presenceFlushTimer != 0)
		purple_timeout_remove(s_presenceFlushTimer);
	s_presenceFlushTimer = purple_timeout_add_seconds(PRESENCE_FLUSH_DEBOUNCE_SECONDS, presenceFlushCallback, NULL);
}

static void buddy_status_changed_cb(PurpleBuddy* buddy, PurpleStatus* old_status, PurpleStatus* new_status,
		gpointer unused)
{
	/*
	 * Getting the new availability
	 */
	int newStatusPrimitive = purple_status_type_get_primitive(purple_status_get_type(new_status));
	int newAvailabilityValue = getPalmAvailabilityFromPurpleAvailability(newStatusPrimitive);

	/*
	 * Getting the new custom message
	 */
	const char* customMessage = purple_status_get_attr_string(new_status, "message");
	if (customMessage == NULL)
	{
		customMessage = "";
	}

//	LSError lserror;
//	LSErrorInit(&lserror);

	PurpleAccount* account = purple_buddy_get_account(buddy);
	std::string const& accountKey = getAccountKeyFromPurpleAccount(account);

	if (s_AccountIdsData.count(accountKey) == 0)
	{
		MojLogError(IMServiceApp::s_log, _T("buddy_status_changed_cb: accountId not found in table."));
		return;
	}

	std::string const& accountId = s_AccountIdsData[accountKey];

	std::string const& serviceName = getServiceNameFromPurpleAccount(account);

	PurpleBuddyIcon* icon = purple_buddy_get_icon(buddy);
	char* buddyAvatarLocation = NULL;
	if (icon != NULL)
	{
		buddyAvatarLocation = purple_buddy_icon_get_full_path(icon);
	}

	PurpleGroup* group = purple_buddy_get_group(buddy);
	const char* groupName = purple_group_get_name(group);
	if (groupName == NULL)
	{
		groupName = "";
	}

	// Perf (#3): this fires on every presence tick (online/idle/away), but a buddy's avatar almost
	// never changes between ticks. Forwarding the (unchanged) avatar path made updateBuddyStatus do
	// a wasted com.palm.contact find PER presence change for every avatar'd buddy - a big chunk of
	// the db8 churn during a login/contact-sync burst. Track the last path per buddy and only forward
	// the avatar when it actually changed (or is first seen); avatar-only updates still come through
	// here via buddy_avatar_changed_cb, which will see a differing path and forward it.
	const char* avatarToForward = buddyAvatarLocation;
	{
		std::string avatarKey = accountKey;
		avatarKey.push_back('\x1f');
		avatarKey.append(buddy->name ? buddy->name : "");
		std::string currentAvatar = buddyAvatarLocation ? buddyAvatarLocation : "";
		std::unordered_map<std::string, std::string>::iterator la = s_lastBuddyAvatar.find(avatarKey);
		if (la != s_lastBuddyAvatar.end() && la->second == currentAvatar)
		{
			avatarToForward = NULL; // unchanged -> skip the contact find/update in updateBuddyStatus
		}
		else
		{
			s_lastBuddyAvatar[avatarKey] = currentAvatar;
		}
	}

	// call into the imlibpurpletransport
	// buddy->name is stored in the imbuddyStatus DB kind in the libpurple format - ie. for AIM without the "@aol.com" so that is how we need to search for it
	// Perf (#2): presence-only ticks are coalesced + flushed as a batch. Only avatar-changed ticks
	// (avatarToForward != NULL, gated by #3) take the immediate per-buddy path, which also does the
	// contact avatar update - those are rare, so per-buddy is fine for them.
	// Report the buddy under its webOS-facing address (+E.164 for WhatsApp) so the imbuddystatus record
	// is keyed the same as the contact's ims.value; otherwise a JID-keyed status would never match the
	// "+<phone>"-keyed contact. No-op for @lid/group ids and other services.
	std::string const buddyWebosName = getWebosUsername(buddy->name, serviceName, purple_buddy_get_account(buddy));
	if (avatarToForward != NULL)
	{
		s_imServiceHandler->updateBuddyStatus(accountId.c_str(), serviceName.c_str(), buddyWebosName.c_str(), newAvailabilityValue, customMessage, groupName, avatarToForward);
	}
	else
	{
		queuePresenceUpdate(accountId.c_str(), serviceName.c_str(), buddyWebosName.c_str(), newAvailabilityValue, customMessage, groupName);
	}

	g_message(
			"%s says: %s's presence: availability: '%i', custom message: '%s', avatar location: '%s', display name: '%s', group name: '%s'",
			__FUNCTION__, buddy->name, newAvailabilityValue, customMessage, buddyAvatarLocation, buddy->alias, groupName);

	if (buddyAvatarLocation)
	{
		g_free(buddyAvatarLocation);
	}
}

static void buddy_avatar_changed_cb(PurpleBuddy* buddy)
{
	PurpleStatus* activeStatus = purple_presence_get_active_status(purple_buddy_get_presence(buddy));
	MojLogInfo(IMServiceApp::s_log, _T("buddy avatar changed for %s"), buddy->name);
	buddy_status_changed_cb(buddy, activeStatus, activeStatus, NULL);
}

/*
 * Called after we remove a buddy from our list
 */
static void buddy_removed_cb(PurpleBuddy* buddy)
{
	// nothing to do...
	MojLogInfo(IMServiceApp::s_log, _T("buddy removed %s"), buddy->name);
}

/*
 * Called after we block a buddy from our list
 */
static void buddy_blocked_cb(PurpleBuddy* buddy)
{
	// nothing to do...
	MojLogInfo(IMServiceApp::s_log, _T("buddy blocked %s"), buddy->name);
}

/*
 * Called both after we add a buddy to our list and when we accept a remote users' invitation to add us to their list
 * buddy is the new buddy
 */
/*
 * webOS Telegram port: coalesce a burst of buddy-added signals into a single buddy-list re-sync.
 * The login-time buddy snapshot (getFullBuddyList) runs once, right after login. Protocols like
 * tdlib-purple load their chat/contact list asynchronously AFTER login, so those buddies arrive
 * via buddy-added past the snapshot and used to be dropped (buddy_added_cb was a no-op, and the
 * incremental buddyListResult(fullList=false) path does nothing). We debounce the burst and ask
 * the login-state layer to re-run the full sync so these become db8 contacts.
 */
#define BUDDY_RESYNC_DEBOUNCE_SECONDS 8
// webOS: hard cap on how often a single account's post-change buddy re-sync may ACTUALLY fire.
// The async prpls (tdlib/whatsmeow/presage) load their contact+chat lists in waves spread over
// minutes; the 8s debounce only coalesces a *continuous* burst, so each wave >8s apart used to
// fire its own re-sync. Each re-sync bumps imloginstate -> handleLoginStateChange -> getBuddyLists
// AND re-runs enumerateServersChannels (a delete+recreate of imchannel rows) -- a churn storm that
// broke imchannel.chatThreadId links and regenerated duplicate chatthreads (e.g. multiple PinePhone
// Telegram rooms). Coalesce the waves: at most one re-sync per account per this interval. The
// debounce still delivers a prompt final sync once the list settles (nothing fired for a while).
#define BUDDY_RESYNC_MIN_INTERVAL_SECONDS 45

struct BuddyResyncCtx
{
	std::string serviceName;
	std::string username;
	std::string accountKey;
	guint timerId;
};
static std::unordered_map<std::string, BuddyResyncCtx*> s_buddyResyncCtx;
// wall-clock time a re-sync last actually FIRED per account, for the min-interval rate limit above.
static std::unordered_map<std::string, time_t> s_lastResyncFire;

static gboolean buddyResyncTimeoutCallback(gpointer data)
{
	BuddyResyncCtx* ctx = (BuddyResyncCtx*)data;

	// Rate limit: if a re-sync fired for this account within the last MIN_INTERVAL, defer this one
	// to the end of that window instead of firing now (coalesces later buddy-load waves into it).
	// Keep ctx in s_buddyResyncCtx so a further buddy change just resets the debounce as usual.
	time_t now = time(NULL);
	std::unordered_map<std::string, time_t>::iterator lt = s_lastResyncFire.find(ctx->accountKey);
	if (lt != s_lastResyncFire.end() && now >= lt->second && (now - lt->second) < BUDDY_RESYNC_MIN_INTERVAL_SECONDS)
	{
		guint wait = (guint)(BUDDY_RESYNC_MIN_INTERVAL_SECONDS - (now - lt->second));
		ctx->timerId = purple_timeout_add_seconds(wait, buddyResyncTimeoutCallback, ctx);
		return FALSE; // this occurrence ends; ctx lives on with the new (deferred) timer
	}

	s_lastResyncFire[ctx->accountKey] = now;
	s_buddyResyncCtx.erase(ctx->accountKey);
	if (s_loginState != NULL)
	{
		MojLogInfo(IMServiceApp::s_log, _T("buddyResyncTimeoutCallback: requesting buddy re-sync for %s"), ctx->accountKey.c_str());
		s_loginState->buddyListChanged(ctx->serviceName.c_str(), ctx->username.c_str());
	}
	delete ctx;
	return FALSE; // one-shot
}

// Schedule (debounced) the post-change re-sync for a live account: buddy-list re-sync + the M3
// server/channel enumeration. Fires once ~BUDDY_RESYNC_DEBOUNCE_SECONDS after the last change in a
// burst. Shared by buddy_added_cb (buddies) and blist_node_added_cb (chats/channels).
static void scheduleAccountResync(PurpleAccount* account)
{
	if (account == NULL)
		return;
	// Only re-sync for a live, logged-in account. Nodes added while still connecting (or loaded from
	// the blist at startup) are covered by the normal login-time snapshot / the debounce that follows.
	if (!purple_account_is_connected(account))
		return;

	std::string const& serviceName = getServiceNameFromPurpleAccount(account);
	std::string const& accountKey = getAccountKeyFromPurpleAccount(account);
	// Report the webOS-side owner username (imloginstate/db8 are keyed on it), not the prpl JID.
	std::string const usernameStr = getWebosUsername(account->username, serviceName);
	const char* username = usernameStr.c_str();
	if (serviceName.empty() || *username == '\0')
		return;

	// Reset any pending debounce timer for this account so the re-sync fires once, after the LAST
	// change in the burst (covers the post-login buddy+channel load and later single additions).
	BuddyResyncCtx* ctx = NULL;
	std::unordered_map<std::string, BuddyResyncCtx*>::iterator it = s_buddyResyncCtx.find(accountKey);
	if (it != s_buddyResyncCtx.end())
	{
		ctx = it->second;
		purple_timeout_remove(ctx->timerId);
	}
	else
	{
		ctx = new BuddyResyncCtx;
		ctx->accountKey = accountKey;
		s_buddyResyncCtx[accountKey] = ctx;
	}
	ctx->serviceName = serviceName;
	ctx->username = username;
	ctx->timerId = purple_timeout_add_seconds(BUDDY_RESYNC_DEBOUNCE_SECONDS, buddyResyncTimeoutCallback, ctx);
}

static void buddy_added_cb(PurpleBuddy* buddy)
{
	MojLogInfo(IMServiceApp::s_log, _T("buddy added %s"), buddy->name);
	scheduleAccountResync(purple_buddy_get_account(buddy));
}

// webOS Servers/Rooms M3: a CHAT (e.g. a Discord guild channel) added to the blist AFTER the
// initial buddy burst must also (re)trigger the post-login re-sync, so enumerateServersChannels
// picks up channels that populate late (channels are chats, not buddies, so buddy-added misses
// them). Fires from the generic "blist-node-added" signal; ignores non-chat nodes.
static void blist_node_added_cb(PurpleBlistNode* node)
{
	if (node == NULL || !PURPLE_BLIST_NODE_IS_CHAT(node))
		return;
	scheduleAccountResync(purple_chat_get_account((PurpleChat*)node));
}

// Delete a disposable QR-preview account OFF the signal-callback stack. Calling
// purple_accounts_delete() directly from account_logged_in_cb (the account's own "signed-on"
// handler) deletes the connection/account while libpurple is still using it up the stack; with a
// large synced buddy list (WhatsApp) the re-entrant mass blist teardown crashes the transport.
// Deferring via a 0-timeout runs the delete after the signal has finished dispatching.
// Context for the deferred preview-account delete: the account plus a retry counter, so the
// delete can wait for an in-flight (async, Go-backed) disconnect to finish before freeing.
struct PreviewDeleteCtx
{
	PurpleAccount* acct;
	int tries;
};

static gboolean deferredDeletePreviewAccount(gpointer data)
{
	PreviewDeleteCtx* ctx = (PreviewDeleteCtx*)data;
	if (ctx == NULL)
		return FALSE;
	PurpleAccount* acct = ctx->acct;
	if (acct == NULL)
	{
		delete ctx;
		return FALSE;
	}
	// Do not free the account until its connection is fully torn down. Session-based, Go-backed
	// prpls (whatsmeow/WhatsApp) run the disconnect as an ASYNC client shutdown; calling
	// purple_accounts_delete() while that is still in flight frees the account out from under the
	// goroutine still using it -> SIGSEGV a few hundred ms later (the deferral-to-next-tick was not
	// enough). Poll until the connection object is gone (or give up after ~10s and delete anyway).
	if ((purple_account_is_connecting(acct) || purple_account_is_connected(acct) ||
	     purple_account_get_connection(acct) != NULL) && ctx->tries < 40)
	{
		ctx->tries++;
		return TRUE; // reschedule on the next 250ms tick
	}
	purple_accounts_delete(acct);
	delete ctx;
	return FALSE; // done
}

// Disconnect + disable a disposable QR-preview account, then delete it once its connection has
// fully torn down (see deferredDeletePreviewAccount). Shared by both confirm paths -- the
// "signed-on" handler and the token-poll fallback -- so neither frees a still-connecting account.
static void schedulePreviewAccountDelete(PurpleAccount* acct)
{
	if (acct == NULL)
		return;
	if (purple_account_is_connected(acct) || purple_account_is_connecting(acct))
		purple_account_disconnect(acct);
	purple_account_set_enabled(acct, UI_ID, FALSE);
	PreviewDeleteCtx* ctx = new PreviewDeleteCtx();
	ctx->acct = acct;
	ctx->tries = 0;
	purple_timeout_add(250, deferredDeletePreviewAccount, ctx);
}

// Cancel + free any pending buddy-resync debounce timer for an account about to be torn down,
// so buddyResyncTimeoutCallback can't fire against a deleted account.
static void cancelBuddyResync(const std::string& accountKey)
{
	std::unordered_map<std::string, BuddyResyncCtx*>::iterator it = s_buddyResyncCtx.find(accountKey);
	if (it != s_buddyResyncCtx.end())
	{
		purple_timeout_remove(it->second->timerId);
		delete it->second;
		s_buddyResyncCtx.erase(it);
	}
}

static void account_logged_in_cb(PurpleConnection* gc, gpointer loginState)
{
	void* blist_handle = purple_blist_get_handle();
	static int handle;

	PurpleAccount* loggedInAccount = purple_connection_get_account(gc);
	g_return_if_fail(loggedInAccount != NULL);

	/* webOS: an account can reach "signed-on" WITHOUT going through
	 * LibpurpleAdapter::login() -- e.g. purple auto-login of an account persisted in
	 * accounts.xml when the transport (re)starts. In that case ui_data (which carries
	 * the account_key + serviceName) was never set, so the account would be registered
	 * under an empty key and every sendMessage would fail with "not logged in". Repair
	 * ui_data here from the account itself so registration is always keyed correctly. */
	if (loggedInAccount->ui_data == NULL)
	{
		const char* prpl = loggedInAccount->protocol_id ? loggedInAccount->protocol_id : "";
		// Use the special-case-aware inverse map, NOT a bare "prpl-"->"type_" strip: whatsmeow
		// (prpl-hehoe-whatsmeow -> type_whatsapp) and presage (prpl-hehoe-presage -> type_signal)
		// would otherwise register under a bogus service key and never adopt into the webOS login.
		std::string svc = getServiceNameFromPrplProtocolId(prpl);
		std::string uname = loggedInAccount->username ? loggedInAccount->username : "";

		AccountMetaData* amd = new AccountMetaData;
		amd->servicename = svc;
		amd->account_key = getAccountKey(uname, svc);
		loggedInAccount->ui_data = (void*)amd;
		MojLogInfo(IMServiceApp::s_log, _T("account_logged_in_cb: repaired missing ui_data (auto-login); accountKey %s"), amd->account_key.c_str());
	}

	std::string const& serviceName = getServiceNameFromPurpleAccount(loggedInAccount);
	std::string const& accountKey = getAccountKeyFromPurpleAccount(loggedInAccount);

	/* webOS create-after-confirm: this was a disposable QR-preview login. Remote-auth
	 * succeeded, so the prpl has persisted the Discord token on the account. Hand the
	 * token to the AuthChannel as the confirmed credential (the UI creates the real
	 * account with it) and tear the preview down. Do NOT run the normal login-state
	 * path -- there is no webOS account for this yet. */
	if (s_qrPreviewKeys.count(accountKey))
	{
		/* The confirmed credential the UI must store on the real account. Discord's
		 * remote-auth persists it as the "token" account string; session-based prpls
		 * (gowhatsapp) instead set it as the account PASSWORD (deviceJID|registrationId
		 * from purple_set_credentials). Prefer the token, fall back to the password, so
		 * the created account carries whatever lets it reconnect without re-pairing. */
		const char* token = purple_account_get_string(loggedInAccount, "token", NULL);
		if (token == NULL || *token == '\0')
			token = purple_account_get_password(loggedInAccount);
		MojLogInfo(IMServiceApp::s_log, _T("account_logged_in_cb: QR-preview confirmed for %s (credential %s)"),
		           accountKey.c_str(), (token && *token) ? "present" : "MISSING");
		if (s_authChannel)
			s_authChannel->setConfirmed(serviceName.c_str(), loggedInAccount->username, token ? token : "");

		if (s_accountLoginTimers.count(accountKey))
		{
			purple_timeout_remove(s_accountLoginTimers[accountKey]);
			s_accountLoginTimers.erase(accountKey);
		}
		s_qrPreviewKeys.erase(accountKey);
		s_pendingAccountData.erase(accountKey);
		cancelBuddyResync(accountKey);
		// ADOPTION (do NOT delete the paired preview). Earlier this disconnected + deleted the
		// disposable account here; for session prpls (whatsmeow/WhatsApp) disconnecting a
		// freshly-paired-and-synced client SIGSEGVs, and deleting orphans it in accounts.xml. So
		// instead keep the live, paired connection and register it as an online account that has
		// no webOS accountId yet. The UI creates the real webOS account from the confirmed token;
		// when its onEnabled -> LibpurpleAdapter::login() arrives it finds THIS account already
		// online without a webosAccountId and adopts it (stamps the id + marks the login state),
		// reusing the paired session with no disconnect and no second login. If the user abandons
		// the flow instead, the fail/timeout/cancel paths still tear the preview down.
		s_onlineAccountData[accountKey] = loggedInAccount;
		s_ipAddressesBoundTo[accountKey] = s_ipAddressesBoundTo.count(accountKey) ? s_ipAddressesBoundTo[accountKey] : "";
		MojLogInfo(IMServiceApp::s_log, _T("account_logged_in_cb: keeping paired preview %s alive for adoption"), accountKey.c_str());
		return;
	}

	if (s_onlineAccountData.count(accountKey))
	{
		// we were online. why are we getting notified that we're connected again? we were never disconnected.
		// mark the account online just to be sure?
		MojLogError(IMServiceApp::s_log, _T("account_logged_in_cb: account already online. why are we getting notified?"));
		return;
	}

	/*
	 * cancel the connect timeout for this account
	 */
	if (s_accountLoginTimers.count(accountKey))
	{
		guint timerHandle = s_accountLoginTimers[accountKey];
		purple_timeout_remove(timerHandle);
		s_accountLoginTimers.erase(accountKey);
	}

	MojLogInfo(IMServiceApp::s_log, _T("account_logged_in_cb: inserting account into onlineAccountData hash table. accountKey %s"), accountKey.c_str());
	/* Use the actually-connected account. For a normal login() this equals
	 * s_pendingAccountData[accountKey]; for an auto-login (not in pending) that lookup
	 * would insert a NULL, so key off loggedInAccount directly. */
	s_onlineAccountData[accountKey] = loggedInAccount;
	s_pendingAccountData.erase(accountKey);

	MojLogInfo(IMServiceApp::s_log, _T("Account connected..."));

	// reply with login success
	if (loginState)
	{
		((LoginCallbackInterface*)loginState)->loginResult(serviceName.c_str(), getWebosUsername(loggedInAccount->username, serviceName).c_str(), LoginCallbackInterface::LOGIN_SUCCESS, false, ERROR_NO_ERROR, true);
	}
	else
	{
		MojLogError(IMServiceApp::s_log, _T("ERROR: account_logged_in_cb called with loginState=NULL"));
	}

	if (s_registeredForPresenceUpdateSignals == FALSE)
	{
		purple_signal_connect(blist_handle, "buddy-status-changed", &handle, PURPLE_CALLBACK(buddy_status_changed_cb),
				loginState);
		purple_signal_connect(blist_handle, "buddy-signed-on", &handle, PURPLE_CALLBACK(buddy_signed_on_off_cb),
				GINT_TO_POINTER(TRUE));
		purple_signal_connect(blist_handle, "buddy-signed-off", &handle, PURPLE_CALLBACK(buddy_signed_on_off_cb),
				GINT_TO_POINTER(FALSE));
		purple_signal_connect(blist_handle, "buddy-icon-changed", &handle, PURPLE_CALLBACK(buddy_avatar_changed_cb),
				GINT_TO_POINTER(FALSE));
		purple_signal_connect(blist_handle, "buddy-removed", &handle, PURPLE_CALLBACK(buddy_removed_cb),
				GINT_TO_POINTER(FALSE));
		purple_signal_connect(blist_handle, "buddy-added", &handle, PURPLE_CALLBACK(buddy_added_cb),
				GINT_TO_POINTER(FALSE));
		// webOS Servers/Rooms M3: also catch chats (Discord guild channels) added to the blist, so a
		// late-arriving channel re-triggers the server/channel enumeration (see blist_node_added_cb).
		purple_signal_connect(blist_handle, "blist-node-added", &handle, PURPLE_CALLBACK(blist_node_added_cb),
				GINT_TO_POINTER(FALSE));
		purple_signal_connect(blist_handle, "buddy-privacy-changed", &handle, PURPLE_CALLBACK(buddy_blocked_cb),
				GINT_TO_POINTER(FALSE));

		// testing. Doesn't work: error - "Signal data for sent-im-msg not found". Need to figure out the right handle
//		purple_signal_connect(purple_connections_get_handle(), "sent-im-msg", &handle, PURPLE_CALLBACK(sent_message_cb),
//						GINT_TO_POINTER(FALSE));
		s_registeredForPresenceUpdateSignals = TRUE;
	}
}

static void account_signed_off_cb(PurpleConnection* gc, gpointer loginState)
{
	PurpleAccount* account = purple_connection_get_account(gc);
	g_return_if_fail(account != NULL);

	std::string const& accountKey = getAccountKeyFromPurpleAccount(account);
	if (s_onlineAccountData.count(accountKey))
	{
		MojLogInfo(IMServiceApp::s_log, _T("account_signed_off_cb: removing account from onlineAccountData hash table. accountKey %s"), accountKey.c_str());
		s_onlineAccountData.erase(accountKey);
	}
	else if (s_pendingAccountData.count(accountKey))
	{
		s_pendingAccountData.erase(accountKey);
	}
	else
	{
		// Already signed off this account (or never signed in) so just return
		return;
	}

	s_ipAddressesBoundTo.erase(accountKey);
	//g_hash_table_remove(connectionTypeData, accountKey);

	MojLogInfo(IMServiceApp::s_log, _T("Account disconnected..."));

	if (s_offlineAccountData.count(accountKey))
	{
		/*
		 * Keep the PurpleAccount struct to reuse in future logins
		 */
		s_offlineAccountData[accountKey] = account;
	}

	// reply with signed off
	if (loginState)
	{
		std::string const& serviceName = getServiceNameFromPurpleAccount(account);
		((LoginCallbackInterface*)loginState)->loginResult(serviceName.c_str(), getWebosUsername(account->username, serviceName).c_str(), LoginCallbackInterface::LOGIN_SIGNED_OFF, false, ERROR_NO_ERROR, true);
	}
	else
	{
		MojLogError(IMServiceApp::s_log, _T("ERROR: account_logged_in_cb called with loginState=NULL"));
	}
}

/*
 * This callback is called if a) the login attempt failed, or b) login was successful but the session was closed
 * (e.g. connection problems, etc).
 */
static void account_login_failed_cb(PurpleConnection* gc, PurpleConnectionError type, const gchar* description,
		gpointer loginState)
{
	MojLogError(IMServiceApp::s_log, _T("account_login_failed is called with type %d, description: %s"), type, description);

	PurpleAccount* account = purple_connection_get_account(gc);
	g_return_if_fail(account != NULL);

	gboolean loggedOut = FALSE;
	bool noRetry = true;
	std::string const& accountKey = getAccountKeyFromPurpleAccount(account);

	/* webOS create-after-confirm: a disposable QR-preview login failed (bad network,
	 * QR expired before approval, etc.). Report it on the AuthChannel so the UI can
	 * offer a refresh, and tear the preview down -- do NOT touch the login-state machine. */
	if (s_qrPreviewKeys.count(accountKey))
	{
		std::string const& svc = getServiceNameFromPurpleAccount(account);
		MojLogInfo(IMServiceApp::s_log, _T("account_login_failed_cb: QR-preview failed for %s: %s"), accountKey.c_str(), description ? description : "");
		if (s_authChannel)
			s_authChannel->setChallengeState(svc.c_str(), account->username,
			    (type == PURPLE_CONNECTION_ERROR_NETWORK_ERROR) ? AuthChannel::StateExpired : AuthChannel::StateFailed,
			    description);
		if (s_accountLoginTimers.count(accountKey))
		{
			purple_timeout_remove(s_accountLoginTimers[accountKey]);
			s_accountLoginTimers.erase(accountKey);
		}
		s_qrPreviewKeys.erase(accountKey);
		s_pendingAccountData.erase(accountKey);
		s_onlineAccountData.erase(accountKey);
		cancelBuddyResync(accountKey);
		// CRITICAL: tear the failed preview down. Previously this path only erased tracking maps and
		// returned, leaving the disposable account PERSISTED + ENABLED in accounts.xml. It then
		// auto-logged-in on every transport (re)start and hammered the server -- for WhatsApp that is
		// a permanent 429 "rate-overlimit" loop even though no real account was ever created. Delete
		// it (once its connection is fully torn down) exactly like the confirm path does.
		schedulePreviewAccountDelete(account);
		return;
	}

	if (s_onlineAccountData.count(accountKey))
	{
		/*
		 * We were online on this account and are now disconnected because either a) the data connection is dropped,
		 * b) the server is down, or c) the user has logged in from a different location and forced this session to
		 * get closed.
		 */
		if (type != PURPLE_CONNECTION_ERROR_NETWORK_ERROR)
		{
			// TODO - this does not seem to really be used...
			loggedOut = TRUE;
			MojLogError(IMServiceApp::s_log, _T("We were logged out. Reason: %s, prpl error code: %i"), description, type);
		}
		MojLogInfo(IMServiceApp::s_log, _T("account_login_failed_cb: removing account from onlineAccountData hash table. accountKey %s"), accountKey.c_str());
		s_onlineAccountData.erase(accountKey);
	}
	else
	{
		/*
		 * cancel the connect timeout for this account
		 */
		if (s_accountLoginTimers.count(accountKey))
		{
			guint timerHandle = s_accountLoginTimers[accountKey];
			purple_timeout_remove(timerHandle);
			s_accountLoginTimers.erase(accountKey);
		}

		if (s_pendingAccountData.count(accountKey) == 0)
		{
			/*
			 * This account was in neither of the account data lists (online or pending). This can happen if the connection goes down
			 * while we are in the process of logging in and the account was in the pending list (gets removed in deviceConnectionClosed).
			 */
			MojLogWarning(IMServiceApp::s_log, _T("account_login_failed_cb: account in neither online or pending list. Can happen if we lose connection while in pending state."));
			// still need to let MojoDb know about the failure
//			free(serviceName);
//			free(username);
//			free(accountKey);
//			return;
		}
		else
		{
			s_pendingAccountData.erase(accountKey);
		}
	}

	// Retry on ANY transient/connection failure; only a genuine credential/config error should park
	// the account offline (retrying with bad credentials just hammers the server). Previously ONLY
	// PURPLE_CONNECTION_ERROR_NETWORK_ERROR was retryable, so presage (Signal) reporting a dropped
	// connection as OTHER_ERROR (16) -- e.g. its async runtime ending on a transient "Invalid
	// response" -- set noRetry=true and parked Signal at availability=OFFLINE *forever*, requiring a
	// manual re-toggle in the availability menu. Park only on the real auth/settings errors.
	switch (type)
	{
		case PURPLE_CONNECTION_ERROR_INVALID_USERNAME:
		case PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED:
		case PURPLE_CONNECTION_ERROR_AUTHENTICATION_IMPOSSIBLE:
		case PURPLE_CONNECTION_ERROR_INVALID_SETTINGS:
			noRetry = true; // credential/config problem -> do not auto-retry (would hammer)
			break;
		default:
			MojLogInfo(IMServiceApp::s_log, _T("account_login_failed_cb: transient error (type %i: %s) -> retry, not park"),
			           type, description ? description : "");
			noRetry = false; // network / other / encryption / cert / name-in-use / ... -> retry
			break;
	}

	const char* mojoFriendlyErrorCode = getMojoFriendlyErrorCode(type);
	s_ipAddressesBoundTo.erase(accountKey);
	s_connectionTypeData.erase(accountKey);

	/*
	 * Keep the PurpleAccount struct to reuse in future logins
	 */
	s_offlineAccountData[accountKey] = account;

	// reply with login failed
	if (loginState != NULL)
	{
		std::string const& serviceName = getServiceNameFromPurpleAccount(account);
		//TODO: determine if there are cases where noRetry should be false
		//TODO: include "description" parameter because it had useful details?
		((LoginCallbackInterface*)loginState)->loginResult(serviceName.c_str(), getWebosUsername(account->username, serviceName).c_str(), LoginCallbackInterface::LOGIN_FAILED, loggedOut, mojoFriendlyErrorCode, noRetry);
	}
	else
	{
		MojLogError(IMServiceApp::s_log, _T("ERROR: account_login_failed_cb called with loginState=NULL"));
	}
}

static void account_status_changed(PurpleAccount* account, PurpleStatus* oldStatus, PurpleStatus* newStatus, gpointer loginState)
{
	printf("\n\n ACCOUNT STATUS CHANGED \n\n");
}

/*
 *  This gets called then we decline a remote user's buddy invite
 *  What signal gets emitted when a remote user declines our invitation???
 */
static void account_auth_deny_cb(PurpleAccount* account, const char* remote_user)
{
	MojLogInfo(IMServiceApp::s_log, _T("account_auth_deny_cb called. account: %s, remote_user: %s"), account->username, remote_user);

	// TODO this needs to happen when remote user declines our invite, not here...
//	char* serviceName = getServiceNameFromPrplProtocolId(account->protocol_id);
//	char* usernameFromStripped = stripResourceFromJabberUsername(remote_user, serviceName);
//
//	// tell transport to delete the buddy and contacts
//	s_imServiceHandler->buddyInviteDeclined(serviceName, account->username, usernameFromStripped);
//
//	// clean up
//	free(serviceName);
//	free(username);
//	free(usernameFromStripped);


}

/*
 * This gets called then we accept a remote user's buddy invite
 *      log: account_auth_accept_cb called. account: palm@gmail.com/Home, buddy: palm3@gmail.com
 */
static void account_auth_accept_cb(PurpleAccount* account, const char* remote_user)
{
	// nothing to do
	MojLogInfo(IMServiceApp::s_log, _T("account_auth_accept_cb called. account: %s, remote_user: %s"), account->username, remote_user);
}

/*
 * Find a buddy-list chat for this account whose "id" component equals `id`.
 *
 * webOS Servers/Rooms: purple-discord names a channel *conversation* by the raw channel snowflake
 * (e.g. "1410895923755880"), but the blist PurpleChat is keyed by its human name ("general") with
 * the snowflake stored in the "id" component. So purple_blist_find_chat(account, <snowflake>) - which
 * matches on the chat's display name - misses for Discord, and we never recover the human channel
 * name or the parent guild. Walk the blist and match the "id" component instead. Other prpls whose
 * conversation name is already the human/keyed name resolve via purple_blist_find_chat and never
 * reach this fallback.
 */
static PurpleChat* findChatByIdComponent(PurpleAccount* account, const char* id)
{
	if (account == NULL || id == NULL || *id == '\0')
		return NULL;

	for (PurpleBlistNode* node = purple_blist_get_root(); node != NULL; node = node->next)
	{
		if (!PURPLE_BLIST_NODE_IS_GROUP(node))
			continue;
		for (PurpleBlistNode* child = node->child; child != NULL; child = child->next)
		{
			if (!PURPLE_BLIST_NODE_IS_CHAT(child))
				continue;
			PurpleChat* chat = (PurpleChat*)child;
			if (purple_chat_get_account(chat) != account)
				continue;
			GHashTable* comps = purple_chat_get_components(chat);
			if (comps == NULL)
				continue;
			const char* compId = (const char*)g_hash_table_lookup(comps, "id");
			if (compId != NULL && strcmp(compId, id) == 0)
				return chat;
		}
	}
	return NULL;
}

/*
 * webOS Servers/Rooms: derive the server (guild / team / network) identity for a group chat, used
 * IDENTICALLY by incoming_message_cb (message-driven) and enumerateServersChannels (proactive) so the
 * two paths resolve to the SAME imserver (dedup key = serviceName+serverName) instead of duplicating.
 *  - Telegram (flat): every room lands under one synthetic server = the network name; the blist group
 *    (tdlib's generic "Chats"/default) is meaningless as a server, so it's ignored.
 *  - Discord / Teams (hierarchical): the blist group is "Guild: Category" (Discord) or the team name;
 *    the server is the part before the first ": " (the guild/team). The remainder (Discord category)
 *    is returned via outCategory for imchannel.parentId.
 */
// webOS Servers/Rooms: display-name cleanup for server/channel names read from the prpl blist. Some
// prpls hand back HTML-escaped names (Teams "LuneOS &amp; webOS-OSE"), which the app shows literally;
// decode the common entities so the user sees "LuneOS & webOS-OSE".
static std::string htmlUnescape(const std::string& in)
{
	std::string out;
	out.reserve(in.size());
	for (size_t i = 0; i < in.size(); )
	{
		if (in[i] == '&')
		{
			if      (in.compare(i, 5, "&amp;")  == 0) { out += '&';  i += 5; continue; }
			else if (in.compare(i, 4, "&lt;")   == 0) { out += '<';  i += 4; continue; }
			else if (in.compare(i, 4, "&gt;")   == 0) { out += '>';  i += 4; continue; }
			else if (in.compare(i, 6, "&quot;") == 0) { out += '"';  i += 6; continue; }
			else if (in.compare(i, 6, "&apos;") == 0) { out += '\''; i += 6; continue; }
			else if (in.compare(i, 5, "&#39;")  == 0) { out += '\''; i += 5; continue; }
		}
		out += in[i++];
	}
	return out;
}

// True if `name` is a raw Teams/Skype thread id like "19:<hex>@thread.skype" / "@thread.v2" - i.e. a
// group chat with no topic set. purple-teams uses that id as the chat title, so it would otherwise be
// shown to the user verbatim.
static bool isRawThreadId(const std::string& name)
{
	return name.compare(0, 3, "19:") == 0 && name.find("@thread.") != std::string::npos;
}

// Clean a channel display name for storage/UI: decode HTML entities, and replace an un-named Teams
// group chat's raw thread id with a readable placeholder. (Proper participant-derived names - "Alice,
// Bob, ..." like the real client - need the resolved member list and are a purple-teams follow-up.)
static std::string cleanChannelDisplayName(const char* rawName)
{
	if (rawName == NULL || *rawName == '\0')
		return std::string();
	std::string n = htmlUnescape(rawName);
	if (isRawThreadId(n))
		return std::string("Group chat");
	return n;
}

// webOS WhatsApp Channels: true if `name` is a WhatsApp Channel/newsletter JID ("<id>@newsletter").
// whatsmeow leaves IsGroup=false for the newsletter server, so these otherwise arrive as 1:1 IMs.
static bool isWhatsAppNewsletter(const char* name)
{
	if (name == NULL)
		return false;
	size_t len = strlen(name);
	static const char* suffix = "@newsletter";
	size_t slen = strlen(suffix);
	return len > slen && strcmp(name + len - slen, suffix) == 0;
}

static std::string deriveServerName(PurpleAccount* account, const char* groupName, std::string* outCategory)
{
	if (outCategory)
		outCategory->clear();
	const char* protoId = account ? purple_account_get_protocol_id(account) : NULL;
	if (protoId != NULL && strstr(protoId, "telegram") != NULL)
	{
		const char* net = purple_account_get_protocol_name(account);
		return (net != NULL && *net != '\0') ? std::string(net) : std::string("Telegram");
	}
	if (groupName == NULL || *groupName == '\0')
		return std::string();
	std::string g = groupName;
	// webOS Teams: purple-teams files ALL Teams chats under one blist group named "Teams - <tenantId>"
	// (teams_get_blist_group), and for a personal account the tenant is the consumer GUID - so the
	// Servers tab showed a "server" literally called "Teams - 9188040d-6c67-...". Collapse it to "Teams".
	if (g == "Teams" || g.compare(0, 8, "Teams - ") == 0)
		return std::string("Teams");
	std::string::size_type sep = g.find(": ");
	if (sep != std::string::npos)
	{
		if (outCategory)
			*outCategory = htmlUnescape(g.substr(sep + 2));
		return htmlUnescape(g.substr(0, sep));
	}
	return htmlUnescape(g);
}

/*
 * webOS Servers/Rooms M3: join a group channel so the prpl fetches + delivers its recent history and
 * accepts sends into it (purple-discord fetches ~100 messages on join). Robust to an unstable buddy
 * list: if the channel's chat isn't in the blist, build the join components straight from the channel
 * key (Discord/Telegram store it under "id", Teams under "chatname"). Returns the (existing or newly
 * joined) chat conversation, or NULL. Joining is idempotent - an already-open chat is returned as-is.
 */
static PurpleConversation* joinChannelChat(PurpleAccount* account, const char* channel)
{
	if (account == NULL || channel == NULL || *channel == '\0')
		return NULL;
	PurpleConnection* gc = purple_account_get_connection(account);
	if (gc == NULL)
		return NULL;

	PurpleConversation* conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, channel, account);
	if (conv != NULL)
		return conv;   // already joined

	// Prefer the blist chat's own components; fall back to constructing them from the key.
	PurpleChat* chat = findChatByIdComponent(account, channel);
	if (chat == NULL)
		chat = purple_blist_find_chat(account, channel);

	GHashTable* built = NULL;
	GHashTable* components = (chat != NULL) ? purple_chat_get_components(chat) : NULL;
	if (components == NULL)
	{
		const char* protoId = purple_account_get_protocol_id(account);
		const char* key = (protoId != NULL && strstr(protoId, "teams") != NULL) ? "chatname" : "id";
		built = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
		g_hash_table_insert(built, g_strdup(key), g_strdup(channel));
		components = built;
	}

	serv_join_chat(gc, components);
	if (built != NULL)
		g_hash_table_destroy(built);

	return purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, channel, account);
}


// webOS receive attachments: true if `message` is nothing but a libpurple imgstore image reference
// (e.g. <img id="7">), optionally surrounded by whitespace. presage emits such a message as an inline
// copy of a received image IN ADDITION to the file:// URL message we rely on (the auto-download path
// template writes both), so we drop the redundant inline one to avoid a duplicate/broken image bubble.
// A real image delivered as a URL (Discord/Telegram) arrives as text, not an <img> tag, so is unaffected.
static bool isPureImgstoreMessage(const char* message)
{
	if (message == NULL)
		return false;
	const char* p = message;
	while (*p && isspace((unsigned char)*p)) p++;
	if (strncasecmp(p, "<img", 4) != 0)
		return false;
	const char* end = strchr(p, '>');
	if (end == NULL)
		return false;
	// must be an imgstore-id reference ("<img id=...>"), not a src=URL <img> - check within the tag
	std::string tag(p, end - p);
	for (size_t i = 0; i + 2 < tag.size(); i++)
		if ((tag[i]=='i'||tag[i]=='I') && (tag[i+1]=='d'||tag[i+1]=='D') && tag[i+2]=='=')
		{
			end++;
			while (*end && isspace((unsigned char)*end)) end++;
			return *end == '\0';   // nothing follows the single <img id=...> tag
		}
	return false;
}

void incoming_message_cb(PurpleConversation* conv, const char* who, const char* alias, const char* message,
		PurpleMessageFlags flags, time_t mtime)
{
	/*
	 * snippet taken from nullclient
	 */
	const char* usernameFrom;
	if (who && *who)
		usernameFrom = who;
	else if (alias && *alias)
		usernameFrom = alias;
	else
		usernameFrom = "";

	// webOS Servers/Rooms: tdlib-purple smuggles a group message sender as "id<userId>\x1f<Display Name>"
	// through the single `who` slot. Split it so usernameFrom becomes the routable id (-> from.addr, so
	// the app can open a 1:1 with the sender) and the display name is forwarded separately (-> from.name).
	// Non-group messages and other prpls have no \x1f and are unaffected.
	std::string usernameFromBuf;
	std::string usernameFromDisplayBuf;
	{
		const char* sep = (usernameFrom && *usernameFrom) ? strchr(usernameFrom, '\x1f') : NULL;
		if (sep != NULL)
		{
			usernameFromBuf.assign(usernameFrom, sep - usernameFrom); // "id<userId>"
			usernameFromDisplayBuf.assign(sep + 1);                   // "Display Name"
			usernameFrom = usernameFromBuf.c_str();
		}
	}
	const char* usernameFromDisplay = usernameFromDisplayBuf.empty() ? NULL : usernameFromDisplayBuf.c_str();

	if ((flags & PURPLE_MESSAGE_RECV) != PURPLE_MESSAGE_RECV)
	{
		// A message WE sent. A plain local echo (app-initiated send) is already persisted by
		// OutgoingIMHandler, so ignore it. But a carbon of a message we sent from ANOTHER client
		// (PURPLE_MESSAGE_REMOTE_SEND - e.g. the Telegram/Signal/WhatsApp/Discord phone app) has no
		// local row, so store it as an Outbox message so it shows on the sent side of the thread (and a
		// reaction can attach to it). Both 1:1 IMs and guild CHANNEL carbons are handled below.
		if ((flags & PURPLE_MESSAGE_REMOTE_SEND) && s_imServiceHandler != NULL)
		{
			PurpleAccount* sentAccount = purple_conversation_get_account(conv);
			if (sentAccount != NULL)
			{
				std::string const& sentService = getServiceNameFromPurpleAccount(sentAccount);
				std::string ownerWebos = getWebosUsername(sentAccount->username, sentService, sentAccount);
				// serviceMessageId the prpl stashed on the conv right before this write (its own id).
				char* svcMsgId = (char*) purple_conversation_get_data(conv, "webos-msg-id");
				// webOS replies: a carbon of a reply we sent from another client carries the quoted-original
				// too. Read it (same stash as the RECV path) so the Outbox row renders the inline quote card.
				char* qMsgId = (char*) purple_conversation_get_data(conv, "webos-quoted-id");
				char* qText  = (char*) purple_conversation_get_data(conv, "webos-quoted-text");
				char* qFrom  = (char*) purple_conversation_get_data(conv, "webos-quoted-from");

				if (purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_IM)
				{
					const char* peer = purple_conversation_get_name(conv); // the recipient (1:1 peer)
					if (peer != NULL && *peer != '\0')
					{
						std::string peerStripped = stripResourceFromJabberUsername(peer, sentService);
						std::string peerWebos = getWebosUsername(peerStripped.c_str(), sentService, sentAccount);
						s_imServiceHandler->incomingIM(sentService.c_str(), ownerWebos.c_str(), peerWebos.c_str(),
								message, mtime, NULL, NULL, NULL, NULL, false, NULL,
								(svcMsgId && *svcMsgId) ? svcMsgId : NULL,
								(qMsgId && *qMsgId) ? qMsgId : NULL, (qText && *qText) ? qText : NULL,
								(qFrom && *qFrom) ? qFrom : NULL, /* outgoing */ true);
					}
				}
				else if (purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_CHAT)
				{
					// webOS #2: a message WE sent in a guild CHANNEL from another device (e.g. the Discord
					// phone app). Store it as an OUTGOING row in that channel (folder=outbox), resolving
					// channel + server the same way the RECV channel path does so it groups under the same
					// imserver. usernameFrom = us (the sender).
					const char* channelName = purple_conversation_get_name(conv);
					if (channelName != NULL && *channelName != '\0')
					{
						const char* channelDisplayName = purple_conversation_get_title(conv);
						std::string parentGroupName;
						PurpleChat* chat = purple_blist_find_chat(sentAccount, channelName);
						if (chat == NULL)
							chat = findChatByIdComponent(sentAccount, channelName);
						if (chat != NULL)
						{
							const char* humanName = purple_chat_get_name(chat);
							if (humanName != NULL && *humanName != '\0')
								channelDisplayName = humanName;
							PurpleBlistNode* parent = ((PurpleBlistNode*)chat)->parent;
							if (parent != NULL && PURPLE_BLIST_NODE_IS_GROUP(parent))
							{
								const char* groupName = purple_group_get_name((PurpleGroup*)parent);
								if (groupName != NULL)
									parentGroupName = groupName;
							}
						}
						// webOS: don't stamp the raw match key/JID as a display name when no human room
						// title resolved (unaliased WhatsApp group during backfill) -- see the incoming path.
						if (channelDisplayName != NULL && strcmp(channelDisplayName, channelName) == 0)
							channelDisplayName = NULL;
						std::string serverNameStr = deriveServerName(sentAccount, parentGroupName.empty() ? NULL : parentGroupName.c_str(), NULL);
						const char* serverName = serverNameStr.empty() ? NULL : serverNameStr.c_str();
						s_imServiceHandler->incomingIM(sentService.c_str(), ownerWebos.c_str(), ownerWebos.c_str(),
								message, mtime, channelName, channelDisplayName, serverName, serverName, false, NULL,
								(svcMsgId && *svcMsgId) ? svcMsgId : NULL,
								(qMsgId && *qMsgId) ? qMsgId : NULL, (qText && *qText) ? qText : NULL,
								(qFrom && *qFrom) ? qFrom : NULL, /* outgoing */ true);
					}
				}

				if (svcMsgId != NULL)
				{
					g_free(svcMsgId);
					purple_conversation_set_data(conv, "webos-msg-id", NULL);
				}
				if (qMsgId != NULL) { g_free(qMsgId); purple_conversation_set_data(conv, "webos-quoted-id", NULL); }
				if (qText  != NULL) { g_free(qText);  purple_conversation_set_data(conv, "webos-quoted-text", NULL); }
				if (qFrom  != NULL) { g_free(qFrom);  purple_conversation_set_data(conv, "webos-quoted-from", NULL); }
			}
		}
		return;
	}

	// webOS receive attachments: drop a redundant inline-imgstore copy of a received image (presage
	// emits one alongside the file:// URL message we actually render). See isPureImgstoreMessage.
	if (isPureImgstoreMessage(message))
	{
		MojLogInfo(IMServiceApp::s_log, _T("incoming_message_cb: dropping redundant inline-image (imgstore) message"));
		return;
	}

	PurpleAccount* account = purple_conversation_get_account(conv);

	// these never return null...
	std::string const& serviceName = getServiceNameFromPurpleAccount(account);

	if (account->username == usernameFrom) // TODO: should this compare use account->username?
	{
		/* We get notified even though we sent the message. Just ignore it */
		return;
	}

	std::string usernameFromStripped = stripResourceFromJabberUsername(usernameFrom, serviceName);

	// webOS Servers/Rooms (Milestone 0): detect multi-user chat (MUC) conversations - e.g.
	// Discord guild channels, IRC channels, Teams channels. libpurple delivers these through the
	// same write_conv slot as 1:1 IMs, so branch on the conversation type. For a chat we resolve
	// the parent "server" (Discord guild / IRC network) from the room's buddy-list group, which
	// purple-discord sets to the guild name. channelName/serverName are forwarded onto the
	// immessage so the ChatThreader/UI can group channels under their server. 1:1 IMs are
	// unaffected: both pointers stay NULL and the stored record is identical to before.
	const char* channelName = NULL;
	const char* channelDisplayName = NULL;   // human room title (Telegram group name); channelName stays the key
	std::string serverNameStr;   // resolved guild/team/network server (via deriveServerName)
	std::string parentGroupName; // raw blist group of the chat, fed to deriveServerName
	// Muted-conversation support: the prpl (e.g. tdlib-purple) records a chat's server-side mute
	// state as a "muted" bool on the buddy (1:1) / chat (group) blist node. Read it here and forward
	// it so the message is stored with flags.noNotification (banner suppressed, still unread). This
	// is protocol-agnostic - any prpl that sets the "muted" node bool participates.
	bool muted = false;
	if (purple_conversation_get_type(conv) == PURPLE_CONV_TYPE_CHAT)
	{
		channelName = purple_conversation_get_name(conv);
		// Human room title for display. tdlib-purple names the conversation "chat-<id>" (a stable
		// key, kept as channelName) but exposes the real room title separately - forward it so the
		// UI shows "Gubbins Calls" instead of "chat-1001609073900". purple-discord's name is already
		// human, so title==name there and this is harmless.
		channelDisplayName = purple_conversation_get_title(conv);
		if (channelName && *channelName)
		{
			PurpleChat* chat = purple_blist_find_chat(account, channelName);
			// purple-discord names the conversation by the raw channel snowflake, so the lookup
			// above (which matches the chat's human display name) misses - fall back to matching
			// the "id" component. See findChatByIdComponent.
			if (chat == NULL)
				chat = findChatByIdComponent(account, channelName);
			if (chat != NULL)
			{
				// Human channel name for display: purple_chat_get_name() returns the prpl's
				// get_chat_name (Discord -> components["name"], e.g. "general"). Prefer it over the
				// conversation title, which for Discord is just the snowflake. Falls through to the
				// title/name for prpls that don't provide a distinct human name.
				const char* humanName = purple_chat_get_name(chat);
				if (humanName != NULL && *humanName != '\0')
					channelDisplayName = humanName;
				// The chat's parent blist node is its group; for purple-discord that group
				// is the guild (server). Use direct field access (public struct member) so we
				// don't depend on any particular libpurple accessor version.
				PurpleBlistNode* parent = ((PurpleBlistNode*)chat)->parent;
				if (parent != NULL && PURPLE_BLIST_NODE_IS_GROUP(parent))
				{
					const char* groupName = purple_group_get_name((PurpleGroup*)parent);
					if (groupName != NULL)
						parentGroupName = groupName;
				}
				// webOS: an archived chat is silenced like a muted one (no notification banner). The
				// prpl (tdlib-purple) sets both bools on the chat blist node.
				muted = purple_blist_node_get_bool((PurpleBlistNode*)chat, "muted")
				        || purple_blist_node_get_bool((PurpleBlistNode*)chat, "archived");
			}
		}
		// webOS: if we couldn't resolve a HUMAN room title, channelDisplayName has fallen back to the
		// raw match key (for a WhatsApp group that's the "<digits>-<digits>@g.us" JID, before its name
		// has been fetched during post-connect backfill). Don't stamp that -- leave it NULL so the
		// ChatThreader keeps the thread's existing (good) name instead of downgrading it to the JID.
		// (Pairs with the chatthreader JID guard; a later live message re-supplies the real name.)
		if (channelDisplayName != NULL && channelName != NULL && strcmp(channelDisplayName, channelName) == 0)
			channelDisplayName = NULL;
		// Resolve the server identity consistently with enumerateServersChannels: the guild/team (the
		// part before ": " in the blist group) for Discord/Teams, or the synthetic network server for
		// flat Telegram (whose blist group is meaningless). Sharing deriveServerName makes a channel's
		// message-driven and enumerated records dedup to the same imserver instead of duplicating.
		serverNameStr = deriveServerName(account, parentGroupName.empty() ? NULL : parentGroupName.c_str(), NULL);
		MojLogInfo(IMServiceApp::s_log,
			_T("incoming_message_cb: group-chat message. channel: %s title: %s server(guild): %s sender: %s muted: %d"),
			channelName ? channelName : "", channelDisplayName ? channelDisplayName : "", serverNameStr.c_str(), usernameFromStripped.c_str(), muted);
	}
	else
	{
		// 1:1 IM: the buddy node carries the per-chat mute/archived flags. (An archived 1:1 whose
		// buddy was pruned from the list has no node here - that rarer case isn't silenced yet.)
		PurpleBuddy* buddy = purple_find_buddy(account, usernameFrom);
		if (buddy != NULL)
			muted = purple_blist_node_get_bool((PurpleBlistNode*)buddy, "muted")
			        || purple_blist_node_get_bool((PurpleBlistNode*)buddy, "archived");
	}

	// webOS WhatsApp Channels: whatsmeow delivers a followed Channel (newsletter) as a 1:1 IM whose
	// peer JID is "<id>@newsletter" (it leaves IsGroup=false for the newsletter server), so it lands in
	// the else-branch above with channelName==NULL and would be stored as an ordinary chatthread. Route
	// it into the Server/Channel tab instead by tagging it like a MUC: channelName = the JID (the stable
	// match key, == purple_conversation_get_name so it dedups with the enumerated record), server = a
	// synthetic "WhatsApp Channels". Setting channelName flips IMMessage isGroupChat true. The channel's
	// human title comes from the buddy alias (best effort; the JID is the fallback).
	if (channelName == NULL && serviceName == "type_whatsapp")
	{
		const char* imName = purple_conversation_get_name(conv);
		if (isWhatsAppNewsletter(imName))
		{
			channelName = imName;
			serverNameStr = "WhatsApp Channels";
			PurpleBuddy* nlBuddy = purple_find_buddy(account, imName);
			if (nlBuddy != NULL)
			{
				const char* alias = purple_buddy_get_alias(nlBuddy);
				if (alias != NULL && *alias != '\0' && !isWhatsAppNewsletter(alias))
					channelDisplayName = alias;
			}
		}
	}

	// webOS WhatsApp: the sender id is the raw JID "<digits>@s.whatsapp.net" (or opaque "<id>@lid"),
	// so a message would otherwise show "31611745571@s.whatsapp.net" as the sender. Give from.name a
	// human value -- the push-name if the sender is a known buddy, else the formatted "+<phone>" --
	// mirroring getFullBuddyList's whatsAppDisplayName. from.addr keeps the routable JID. Only set
	// when the prpl didn't already smuggle a display name (usernameFromDisplay via the \x1f split).
	if (usernameFromDisplay == NULL && serviceName == "type_whatsapp")
	{
		PurpleBuddy* senderBuddy = purple_find_buddy(account, usernameFromStripped.c_str());
		const char* senderAlias = senderBuddy ? purple_buddy_get_alias_only(senderBuddy) : NULL;
		std::string waDisp = whatsAppDisplayName(senderAlias, usernameFromStripped.c_str());
		if (!waDisp.empty() && waDisp != usernameFromStripped)
		{
			usernameFromDisplayBuf = waDisp;
			usernameFromDisplay = usernameFromDisplayBuf.c_str();
		}
	}

	// webOS Signal: contacts are opaque ACI UUIDs. presage sets a profile-name alias when it has
	// one; when it doesn't, never surface the raw UUID -- show the alias if human, else a generic
	// label. from.addr keeps the routable UUID.
	if (usernameFromDisplay == NULL && serviceName == "type_signal")
	{
		PurpleBuddy* senderBuddy = purple_find_buddy(account, usernameFrom);
		const char* senderAlias = senderBuddy ? purple_buddy_get_alias_only(senderBuddy) : NULL;
		if (senderAlias && *senderAlias && !isSignalUuid(senderAlias))
		{
			usernameFromDisplayBuf = senderAlias;
			usernameFromDisplay = usernameFromDisplayBuf.c_str();
		}
		else if (isSignalUuid(usernameFrom))
		{
			usernameFromDisplayBuf = "Signal user";
			usernameFromDisplay = usernameFromDisplayBuf.c_str();
		}
	}

	// call the transport service incoming message handler
	// webOS Teams port: forward the libpurple message time (mtime, secs) so history/
	// offline messages are stored with their original send time, not the arrival time.
	// webOS Servers/Rooms: forward channel + server (both NULL for 1:1 IMs). serverId has no
	// stable value from the blist group alone, so mirror serverName for now - Milestone 1 will
	// pull the real guild id from the chat's components.
	const char* serverName = serverNameStr.empty() ? NULL : serverNameStr.c_str();
	// webOS Servers/Rooms: decode HTML entities and replace an un-named Teams group chat's raw thread id
	// with a readable placeholder, so the stored/displayed room title matches what deriveServerName
	// already does for the server. channelName (the match key) stays raw.
	std::string channelDisplayBuf;
	if (channelDisplayName != NULL && *channelDisplayName != '\0')
	{
		channelDisplayBuf = cleanChannelDisplayName(channelDisplayName);
		if (!channelDisplayBuf.empty())
			channelDisplayName = channelDisplayBuf.c_str();
	}
	// Store the account owner AND the sender in the webOS +E.164 form (getWebosUsername) so the
	// message threads to the same buddy/contact whose ims.value we now write as "+<phone>". The human
	// from.name was already computed above from the raw JID, and the buddy lookups above used the raw
	// JID, so only the STORED addresses change here. Held in locals so the c_str()s outlive the call.
	std::string ownerWebos = getWebosUsername(account->username, serviceName, account);
	std::string senderWebos = getWebosUsername(usernameFromStripped.c_str(), serviceName, account);

	// webOS reactions: the prpl stashes its own id for THIS message on the conversation right before
	// serv_got_im (which synchronously drives us here). Read it so incomingIM can persist it as
	// serviceMessageId, then free + clear it (the prpl g_strdup'd it; this handoff owns the free).
	char* svcMsgId = (char*) purple_conversation_get_data(conv, "webos-msg-id");
	// webOS replies: the prpl also stashes the quoted-original (id/text/sender) this message replies to,
	// the same way it stashes webos-msg-id. Read alongside so incomingIM can persist a proper inline
	// quote instead of the raw "> "/HTML folded into the body. Freed + cleared below (the prpl g_strdup'd).
	char* qMsgId = (char*) purple_conversation_get_data(conv, "webos-quoted-id");
	char* qText  = (char*) purple_conversation_get_data(conv, "webos-quoted-text");
	char* qFrom  = (char*) purple_conversation_get_data(conv, "webos-quoted-from");

	s_imServiceHandler->incomingIM(serviceName.c_str(), ownerWebos.c_str(), senderWebos.c_str(),
			message, mtime, channelName, channelDisplayName, serverName, serverName, muted, usernameFromDisplay, svcMsgId,
			qMsgId, qText, qFrom);

	if (svcMsgId != NULL) {
		g_free(svcMsgId);
		purple_conversation_set_data(conv, "webos-msg-id", NULL);
	}
	if (qMsgId != NULL) { g_free(qMsgId); purple_conversation_set_data(conv, "webos-quoted-id", NULL); }
	if (qText  != NULL) { g_free(qText);  purple_conversation_set_data(conv, "webos-quoted-text", NULL); }
	if (qFrom  != NULL) { g_free(qFrom);  purple_conversation_set_data(conv, "webos-quoted-from", NULL); }
}

/*
 * webOS reactions (cross-prpl): a prpl emitted "webos-im-reaction" for a message it identifies by
 * targetServiceMessageId. Resolve the owning account + reacting sender to webOS usernames and hand
 * off to IMServiceHandler, which merges the reaction onto the target message row (ReactionHandler).
 * emoji=="" (or NULL) means the sender removed their reaction. sender may be NULL/empty for a 1:1
 * chat (the reactor is the peer); a group reaction carries the member id.
 */
static void im_reaction_cb(PurpleAccount* account, const char* targetServiceMessageId, const char* emoji, const char* sender, void* data)
{
	if (account == NULL || targetServiceMessageId == NULL || *targetServiceMessageId == '\0' || s_imServiceHandler == NULL)
		return;

	std::string const& serviceName = getServiceNameFromPurpleAccount(account);
	std::string ownerWebos  = getWebosUsername(account->username, serviceName, account);
	std::string senderWebos = (sender != NULL && *sender != '\0') ? getWebosUsername(sender, serviceName, account) : std::string();

	s_imServiceHandler->handleReaction(serviceName.c_str(), ownerWebos.c_str(), targetServiceMessageId,
			emoji ? emoji : "", senderWebos.c_str());
}

/*
 * webOS: a prpl emitted "webos-im-outbox-id" carrying the network id (serviceMessageId) it assigned to
 * a message the user sent FROM THE APP, plus the sent text as a correlation hint. Resolve the owning
 * account and hand off to IMServiceHandler, which finds the matching Outbox row and stores the id so a
 * reaction can later attach to the user's own sent message.
 */
static void im_outbox_id_cb(PurpleAccount* account, const char* serviceMessageId, const char* text, void* data)
{
	if (account == NULL || serviceMessageId == NULL || *serviceMessageId == '\0' || s_imServiceHandler == NULL)
		return;

	std::string const& serviceName = getServiceNameFromPurpleAccount(account);
	std::string ownerWebos = getWebosUsername(account->username, serviceName);

	s_imServiceHandler->handleOutboxId(serviceName.c_str(), ownerWebos.c_str(), serviceMessageId,
			text ? text : "");
}

/*
 * webOS delivery/read receipts (by-id): a prpl (WhatsApp/Signal) reported that the recipient DELIVERED
 * or READ the outgoing message whose network id is serviceMessageId. Resolve the owning account and
 * hand off to IMServiceHandler, which upgrades the Outbox row's deliveryStatus (single/double tick).
 */
static void im_receipt_cb(PurpleAccount* account, const char* serviceMessageId, const char* status, void* data)
{
	if (account == NULL || serviceMessageId == NULL || *serviceMessageId == '\0' || status == NULL || s_imServiceHandler == NULL)
		return;
	std::string const& serviceName = getServiceNameFromPurpleAccount(account);
	std::string ownerWebos = getWebosUsername(account->username, serviceName, account);
	s_imServiceHandler->handleReceiptById(serviceName.c_str(), ownerWebos.c_str(), serviceMessageId, status);
}

/*
 * webOS delivery/read receipts (watermark): a prpl (Telegram/Facebook/Teams) reported that everything
 * up to a boundary was delivered/read. scope names the conversation + match field (see ReceiptHandler);
 * watermark is the numeric boundary. Upgrades every Outbox row at/under it.
 */
static void im_receipt_hwm_cb(PurpleAccount* account, const char* scope, const char* watermark, const char* status, void* data)
{
	if (account == NULL || scope == NULL || *scope == '\0' || watermark == NULL || *watermark == '\0' || status == NULL || s_imServiceHandler == NULL)
		return;
	std::string const& serviceName = getServiceNameFromPurpleAccount(account);
	std::string ownerWebos = getWebosUsername(account->username, serviceName, account);
	s_imServiceHandler->handleReceiptWatermark(serviceName.c_str(), ownerWebos.c_str(), scope, watermark, status);
}

/*
 * webOS reactions (aggregated/REPLACE): a prpl emitted "webos-im-reaction-set" carrying the whole
 * reaction summary for one message (serialized as "count<SP>emoji" records separated by '\n'). Used
 * by prpls that only expose aggregated counts (Telegram). Resolve the owning account and REPLACE the
 * target message's reactions with this set (an empty `serialized` clears them all).
 */
static void im_reaction_set_cb(PurpleAccount* account, const char* targetServiceMessageId, const char* serialized, const char* unused, void* data)
{
	if (account == NULL || targetServiceMessageId == NULL || *targetServiceMessageId == '\0' || s_imServiceHandler == NULL)
		return;

	std::string const& serviceName = getServiceNameFromPurpleAccount(account);
	std::string ownerWebos = getWebosUsername(account->username, serviceName);

	s_imServiceHandler->handleReactionSet(serviceName.c_str(), ownerWebos.c_str(), targetServiceMessageId,
			serialized ? serialized : "");
}

/*
 * Called when a remote user requests authorization to be our buddy
 *
 * Note: this method will get called every time user logs in if there is a pending invitation.
 */
static void *request_authorize_cb (PurpleAccount *account, const char *remote_user, const char *id,	const char *alias, const char *message,
	gboolean on_list, PurpleAccountRequestAuthorizationCb authorize_cb,	PurpleAccountRequestAuthorizationCb deny_cb,
	void *user_data)
{

	MojLogInfo(IMServiceApp::s_log, _T("request_authorize_cb called. remote user: %s, id: %s, message: %s"), remote_user, id, message);

	// these never return null...
	std::string const& serviceName = getServiceNameFromPurpleAccount(account);
	std::string usernameFromStripped = stripResourceFromJabberUsername(remote_user, serviceName);

	// Save off the authorize/deny callbacks to use later
	AuthRequest *aa = g_new0(AuthRequest, 1);
	aa->auth_cb = authorize_cb;
	aa->deny_cb = deny_cb;
	aa->data = user_data;
	aa->remote_user = g_strdup(remote_user);
	aa->alias = g_strdup(alias);
	aa->account = account;

	char *authRequestKey = getAuthRequestKey(account->username, serviceName.c_str(), usernameFromStripped.c_str());
	// if there is already an entry for this, we need to replace it since callback function pointers will change on login
	// old object gets deleted by our destroy functions specified in the hash table construction
	g_hash_table_replace(s_AuthorizeRequests, authRequestKey, aa);
	// log the table
	logAuthRequestTableValues();

	// call back into IMServiceHandler to create a receivedBuddyInvite imCommand.
	s_imServiceHandler->receivedBuddyInvite(serviceName.c_str(), getWebosUsername(account->username, serviceName).c_str(), usernameFromStripped.c_str(), message);

	// don't free the authRequestKey - it is not copied, but held onto for the life of the hash table once inserted
	return NULL;
}

/*
 * Not used
 */
void request_add_cb(PurpleAccount *account, const char *remote_user, const char *id, const char *alias, const char *message) {

	MojLogInfo(IMServiceApp::s_log, _T("request_add_cb called. remote user: %s"), remote_user);
}

gboolean connectTimeoutCallback(gpointer data)
{

	MojLogError(IMServiceApp::s_log, _T("connectTimeoutCallback called - we not neither success nor failure callback from the last login attempt."));
	// A connect timeout is transient -- a slow or interrupted login (e.g. the login-state machine
	// re-logging-in an account that was already online, then racing its own re-evaluation). Retry
	// rather than parking the account at availability=OFFLINE forever, which previously knocked ALL
	// accounts offline in a burst and required a manual availability re-toggle. A genuinely
	// unreachable account just times out again on the next (backed-off) retry.
	bool noRetry = false;
	std::string* data_ptr = reinterpret_cast<std::string*> (data);
	std::string accountKey = *data_ptr;
	delete data_ptr;

	PurpleAccount* account = NULL;
	if (s_pendingAccountData.count(accountKey) == 0)
	{
		/*
		 * We can get here if we abandoned a pending login because a better connection (wifi) came up
		 * In this case, this is the only way that we can tell MojoDb that the login did not complete and to reset it's watch
		 */
		MojLogWarning(IMServiceApp::s_log,
				_T("WARNING: got to connectTimeoutCallback without an account in the pending list. Must have abandoned login earlier."));
		noRetry = false;
	}
	else {
		account = s_pendingAccountData[accountKey];
		/*
		 * abort logging in since our connect timeout has hit before login either failed or succeeded
		 */
		purple_account_disconnect(account);
	}

	s_accountLoginTimers.erase(accountKey);
	s_pendingAccountData.erase(accountKey);
	s_ipAddressesBoundTo.erase(accountKey);

	// webOS create-after-confirm: a disposable QR-preview login timed out (the user
	// never scanned / approved). Report expiry on the AuthChannel so the UI offers a
	// refresh; do NOT route through the login-state machine (no webOS account exists).
	if (s_qrPreviewKeys.count(accountKey))
	{
		s_qrPreviewKeys.erase(accountKey);
		if (account && s_authChannel)
		{
			std::string const& svc = getServiceNameFromPurpleAccount(account);
			s_authChannel->setChallengeState(svc.c_str(), account->username, AuthChannel::StateExpired, "QR code expired");
		}
		// Tear the timed-out preview down so it is not left persisted + enabled in accounts.xml
		// (it would auto-log-in on every boot otherwise -- see account_login_failed_cb).
		if (account)
		{
			cancelBuddyResync(accountKey);
			schedulePreviewAccountDelete(account);
		}
		return FALSE;
	}

	if (s_loginState)
	{
		std::string const& serviceName = getServiceNameFromPurpleAccount(account);

		// TODO - should noRetry be false here in other cases?
		// Can't really tell - we will get here if the proper sa security certificate is not installed, which is a permanent failure.
		// libpurple just does not reliably call the login failed callback in all cases...this is not the same as a connection timeout.
		s_loginState->loginResult(serviceName.c_str(), getWebosUsername(account->username, serviceName).c_str(), LoginCallbackInterface::LOGIN_TIMEOUT, false, ERROR_NETWORK_ERROR, noRetry);
	}
	else
	{
		MojLogError(IMServiceApp::s_log, _T("ERROR: connectTimeoutCallback called with s_loginState=NULL."));
	}

	return FALSE;
}

/*
 * End of callbacks
 */

/*
 * libpurple initialization methods
 */

static GHashTable* getClientInfo(void)
{
	GHashTable* clientInfo = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);
	g_hash_table_insert(clientInfo, (void*)"name", (void*)"Palm Messaging");
	g_hash_table_insert(clientInfo, (void*)"version", (void*)"");

	return clientInfo;
}

static void initializeLibpurple()
{
	if (signal(SIGCHLD, SIG_IGN) == SIG_ERR) {
		MojLogError(IMServiceApp::s_log, _T("initializeLibpurple: signal(SIGCHLD, SIG_IGN) failed"));
	}

	/* Set a custom user directory (optional) */
	purple_util_set_user_dir(CUSTOM_USER_DIRECTORY);

	/* libpurple prpl debug (prpl_debug_misc: tdlib "Displaying message", HTTP request tracing,
	 * "Incoming update", ...) is a huge, continuous drain on this RESIDENT daemon - it fills
	 * /media/internal/imstdout.log at tens of MB/hour. It is NOT gated by the PmLog level, so
	 * gate it here: OFF by default, ON only when IM_PURPLE_DEBUG is set in the environment
	 * (see /var/imdaemon.sh). Flip it on when actively debugging a specific connector. */
	purple_debug_set_enabled(getenv("IM_PURPLE_DEBUG") != NULL);

	/* Set the core-uiops, which is used to
	 * 	- initialize the ui specific preferences.
	 * 	- initialize the debug ui.
	 * 	- initialize the ui components for all the modules.
	 * 	- uninitialize the ui components for all the modules when the core terminates.
	 */
	purple_core_set_ui_ops(&adapterCoreUIOps);

	purple_eventloop_set_ui_ops(&adapterEventLoopUIOps);

	// TODO - there is a memory leak in here...at least on desktop
	if (!purple_core_init(UI_ID))
	{
		MojLogInfo(IMServiceApp::s_log, _T("libpurple initialization failed."));
		abort();
	}

	// Register the webOS cross-prpl reaction signals NOW, before any account connects, so a prpl that
	// reconnects instantly from a saved session (whatsmeow) can connect to "webos-im-send-reaction" in
	// its login handler without racing the (previously per-first-login) registration.
	registerWebosReactionSignals();

	/* webOS: /var is a small partition (~62MB). libpurple's per-conversation logging duplicates
	 * what db8 already stores and grows unbounded under /var/preferences/com.palm.purple/transport/logs.
	 * Disable it (and system logging) so nothing accumulates there. Must be set after purple_core_init
	 * so the logging subsystem's prefs exist. */
	purple_prefs_set_bool("/purple/logging/log_ims", FALSE);
	purple_prefs_set_bool("/purple/logging/log_chats", FALSE);
	purple_prefs_set_bool("/purple/logging/log_system", FALSE);

	/* Create and load the buddylist. */
	purple_set_blist(purple_blist_new());
	purple_blist_load();

	purple_buddy_icons_set_cache_dir("/var/luna/data/im-avatars");

	s_libpurpleInitialized = TRUE;
	MojLogInfo(IMServiceApp::s_log, _T("libpurple initialized.\n"));
}
/*
 * End of libpurple initialization methods
 */

/*
 * webOS Teams port: called from IMServiceHandler::onDelete when a webOS account is
 * removed. Finds the persisted PurpleAccount tagged with this webOS accountId and
 * deletes it, so accounts.xml, the buddy list (blist.xml) and the stored OAuth
 * refresh_token are all removed for a genuinely clean re-add. Returns true if an
 * account was found and deleted.
 */
bool LibpurpleAdapter::deleteAccountByWebosId(const char* accountId, std::string* outUsername, std::string* outServiceName)
{
	if (accountId == NULL || *accountId == '\0')
		return false;

	/* Ensure accounts.xml is loaded so the persisted account is enumerable even if
	 * the transport was just activated for this delete (idempotent — guarded init). */
	if (!s_libpurpleInitialized)
	{
		initializeLibpurple();
	}

	for (GList* l = purple_accounts_get_all(); l != NULL; l = l->next)
	{
		PurpleAccount* account = (PurpleAccount*)l->data;
		const char* aid = purple_account_get_string(account, "webosAccountId", NULL);
		if (aid != NULL && strcmp(aid, accountId) == 0)
		{
			/* Capture username + serviceName BEFORE deleting so the caller can purge
			 * this account's db8 chat records (keyed by username/serviceName). */
			if (outUsername != NULL && account->username != NULL)
				outUsername->assign(account->username);
			if (outServiceName != NULL)
			{
				// special-case-aware inverse map so the db8 purge (keyed by serviceName)
				// finds WhatsApp/Signal chat records too (type_whatsapp / type_signal),
				// not the bogus type_hehoe-* a bare "prpl-"->"type_" strip would produce.
				outServiceName->assign(getServiceNameFromPrplProtocolId(account->protocol_id));
			}
			MojLogInfo(IMServiceApp::s_log, _T("LibpurpleAdapter::deleteAccountByWebosId removing persisted account for %s"), accountId);
			purple_accounts_delete(account);
			return true;
		}
	}
	MojLogInfo(IMServiceApp::s_log, _T("LibpurpleAdapter::deleteAccountByWebosId no persisted account for %s"), accountId);
	return false;
}

/*
 * Service methods
 */
LibpurpleAdapter::LoginResult LibpurpleAdapter::login(LoginParams const& params, LoginCallbackInterface* loginState)
{
	LoginResult result = OK;

	PurpleAccount* account;
	bool accountIsAlreadyOnline = FALSE;
	bool accountIsAlreadyPending = FALSE;

	MojLogInfo(IMServiceApp::s_log, _T("%s called."), __FUNCTION__);

	PurpleAccount* alreadyActiveAccount = NULL;

	if (params.serviceName.empty() || params.username.empty())
	{
		MojLogError(IMServiceApp::s_log, _T("LibpurpleAdapter::login with empty username or serviceName"));
		return INVALID_CREDENTIALS;
	}

	MojLogInfo(IMServiceApp::s_log, _T("Parameters: accountId %s, servicename %s, connectionType %s"), params.accountId.data(), params.serviceName.data(), params.connectionType.data());

	if (s_libpurpleInitialized == FALSE)
	{
		initializeLibpurple();
	}

	/* libpurple variables */
	std::string const& accountKey = getAccountKey(params.username.data(), params.serviceName.data());

	// If this account id isn't yet stored, then keep track of it now.
	s_AccountIdsData[accountKey] = params.accountId.data();

	/*
	 * Let's check to see if we're already logged in to this account or that we're already in the process of logging in
	 * to this account. This can happen when mojo goes down and comes back up.
	 */
	if (s_onlineAccountData.count(accountKey))
	{
		accountIsAlreadyOnline = TRUE;
		alreadyActiveAccount = s_onlineAccountData[accountKey];
	}
	else
	{
		if (s_pendingAccountData.count(accountKey))
		{
			alreadyActiveAccount = s_pendingAccountData[accountKey];
			accountIsAlreadyPending = TRUE;
		}
	}

	if (alreadyActiveAccount != NULL)
	{
		/*
		 * We're either already logged in to this account or we're already in the process of logging in to this account
		 * (i.e. it's pending; waiting for server response)
		 */
		/* ADOPTION (must run BEFORE the interface check below, which would otherwise
		 * disconnect + rebind a mismatched-IP account -> crash whatsmeow): the account may
		 * already be online because it was paired via a QR-preview login we kept alive (see
		 * account_logged_in_cb). If it has no webOS accountId yet, THIS login() call is the
		 * account's creation -- adopt the live paired session: stamp the webosAccountId (so
		 * onDelete can find it + it is no longer an untagged orphan), bind it to the current
		 * interface, and report login success. Reuses the paired connection with no disconnect
		 * and no second login. */
		if (accountIsAlreadyOnline)
		{
			const char* existingWebosId = purple_account_get_string(alreadyActiveAccount, "webosAccountId", NULL);
			if ((existingWebosId == NULL || *existingWebosId == '\0') && !params.accountId.empty())
			{
				purple_account_set_string(alreadyActiveAccount, "webosAccountId", params.accountId.data());
				if (alreadyActiveAccount->ui_data == NULL)
				{
					AccountMetaData* amd = new AccountMetaData;
					amd->account_key = accountKey;
					amd->servicename = params.serviceName.data();
					alreadyActiveAccount->ui_data = (void*)amd;
				}
				s_ipAddressesBoundTo[accountKey] = params.localIpAddress.data();
				MojLogInfo(IMServiceApp::s_log, _T("LibpurpleAdapter::login: adopted already-paired preview account %s"), accountKey.c_str());
				/* Report success under the webOS-side username (params.username, e.g. the bare
				 * WhatsApp digits) -- NOT alreadyActiveAccount->username, which whatsmeow has
				 * rewritten to the JID form; the imloginstate record is keyed on the former. */
				if (loginState)
					((LoginCallbackInterface*)loginState)->loginResult(params.serviceName.data(), params.username.data(),
					    LoginCallbackInterface::LOGIN_SUCCESS, false, ERROR_NO_ERROR, true);
				return OK;
			}
		}
		std::string const& accountBoundToIpAddress = s_ipAddressesBoundTo[accountKey];
		// Only force a logout+relogin when we have a NEW, non-empty local IP that GENUINELY differs from
		// the one this account is bound to. A wake-from-sleep or connection-manager blip can deliver an
		// empty/stale localIpAddress; treating that as an interface change tore every account down and
		// parked them all offline (availability OFFLINE), and the repeated churn never cleanly reconnected.
		// If the incoming IP is unknown (empty) or the account was never bound to one, keep the existing
		// connection - a genuinely dead socket is caught by libpurple's own SIGNED_OFF handling, which
		// re-drives login through IMLoginState.
		bool ipReallyChanged = !params.localIpAddress.empty()
				&& !accountBoundToIpAddress.empty()
				&& params.localIpAddress.data() != accountBoundToIpAddress;
		if (!ipReallyChanged)
		{
			/*
			 * We're using the right interface for this account (or the local IP is unknown - don't churn)
			 */
			if (accountIsAlreadyPending)
			{
				MojLogError(IMServiceApp::s_log, _T("LibpurpleAdapter::login: We were already in the process of logging in."));
				return OK;
			}
			else if (accountIsAlreadyOnline)
			{
				MojLogError(IMServiceApp::s_log, _T("LibpurpleAdapter::login: We were already logged in to the requested account."));
				return ALREADY_LOGGED_IN;
			}
		}
		else
		{
			/*
			 * We're not using the right interface. Close the current connection for this account and create a new one
			 */
			MojLogError(IMServiceApp::s_log,
					_T("LibpurpleAdapter::login: We have to logout and login again since the local IP address has changed (bound=%s, new=%s). Logging out from account."),
					accountBoundToIpAddress.c_str(), params.localIpAddress.data());
			/*
			 * Once the current connection is closed we don't want to let mojo know that the account was disconnected.
			 * Since mojo went down and came back up it didn't know that the account was connected anyways.
			 * So let's take the account out of the account data hash and then disconnect it.
			 */
			if (s_onlineAccountData.count(accountKey))
			{
				MojLogInfo(IMServiceApp::s_log, _T("LibpurpleAdapter::login: removing account from onlineAccountData hash table. accountKey %s"), accountKey.c_str());
				s_onlineAccountData.erase(accountKey);
			}
			if (s_pendingAccountData.count(accountKey))
				s_pendingAccountData.erase(accountKey);

			purple_account_disconnect(alreadyActiveAccount);
		}
	}

	/*
	 * Let's go through our usual login process
	 */

	// TODO this currently ignores authentication token, but should check it as well when support for auth token is added
	if (params.password.empty())
	{
		MojLogError(IMServiceApp::s_log, _T("Error: null or empty password trying to log in to servicename %s"), params.serviceName.data());
	    return INVALID_CREDENTIALS;
	}
	else
	{
#ifdef DEVICE
		/* save the local IP address that we need to use */
		// If you are running imlibpurpletransport on desktop, but tethered to device, params->localIpAddress needs to be set to
		// NULL otherwise login will fail...
		if (!params.localIpAddress.empty())
		{
			purple_prefs_remove("/purple/network/preferred_local_ip_address");
			purple_prefs_add_string("/purple/network/preferred_local_ip_address", params.localIpAddress.data());
		}
		else
		{

			/*
			 * If we're on device you should not accept an empty ipAddress; it's mandatory to be provided
			 */
			MojLogError(IMServiceApp::s_log, _T("LibpurpleAdapter::login with missing localIpAddress"));
			return FAILED;

		}
#endif

		/* save the local IP address that we need to use */
		s_connectionTypeData[accountKey] = params.connectionType;

		/*
		 * If we've already logged in to this account before then re-use the old PurpleAccount struct
		 */
		if (s_offlineAccountData.count(accountKey))
        {
			account = s_offlineAccountData[accountKey];
        }
		else
		{
			/* Create the account */
    		std::string prplProtocolId = getPrplProtocolIdFromServiceName(params.serviceName.data());

			/* WhatsApp's webOS username is +E.164 (for display); whatsmeow requires the JID as the
			 * purple account username (getPurpleUsername). Every other service passes through verbatim. */
			std::string const purpleUsername = getPurpleUsername(params.username.data(), params.serviceName.data());

			/* webOS Teams port: persist the PurpleAccount in accounts.xml so the rotated
			 * OAuth refresh_token (the prpl stores it as the account password) and the
			 * buddy list survive transport restarts natively — no out-of-band token files.
			 * Reuse the persisted account across restarts; only create+add a fresh one. */
			account = purple_accounts_find(purpleUsername.c_str(), prplProtocolId.c_str());
			if (!account)
			{
				/* A prpl that can't be resolved (plugin missing / failed g_module_open, or dlclosed for
				 * lacking g_module_make_resident) makes createPurpleAccount->getProtocolInfo THROW. If we
				 * let that propagate it hits std::terminate and CRASH-LOOPS the WHOLE transport (every
				 * account, not just this one). Catch it and fail only THIS account's login. */
				try
				{
					account = Util::createPurpleAccount(purpleUsername.c_str(), prplProtocolId.c_str(), params.config);
				}
				catch (const Util::MojoException& e)
				{
					MojLogError(IMServiceApp::s_log, _T("LibpurpleAdapter::login: createPurpleAccount threw for prpl '%s' (service %s): %s - failing this account's login only"),
						prplProtocolId.c_str(), params.serviceName.data(), e.what().c_str());
					return FAILED;
				}
				if (!account)
				{
					MojLogError(IMServiceApp::s_log, _T("LibpurpleAdapter::login failed to create new Purple account"));
					return FAILED;
				}
				purple_accounts_add(account);
			}

            // TODO: If the account did exist before we get a leak here,
            //       look into this (when do we have to delete
            //       account->ui_data? Is there a destructor for
            //       purple_accounts?)
			AccountMetaData* amd = new AccountMetaData;
			amd->account_key = accountKey;
			amd->servicename = params.serviceName.data();

			account->ui_data = (void*)amd;

			/* Record the webOS accountId as a persisted account setting so onDelete
			 * can find and remove exactly this account later (accounts.xml survives
			 * restarts; the in-memory ui_data does not). */
			if (!params.accountId.empty())
				purple_account_set_string(account, "webosAccountId", params.accountId.data());
		}

		// webOS receive attachments: make image-capable prpls auto-download incoming files to a
		// persistent, WebKit-readable location (/media/internal) and surface them as a file:// URL in
		// the conversation body, which the Messaging app renders inline (same path as remote image
		// URLs). These are plain prpl account options - no plugin rebuild. gowhatsapp's "xfer" image
		// mode emits ONLY the file:// URL (its inline imgstore copy is suppressed); presage emits the
		// URL plus a redundant inline imgstore copy that incoming_message_cb drops. Set every login so
		// accounts persisted before this feature also pick it up.
		{
			std::string svc = params.serviceName.data();
			std::string attachDir;
			if (svc == "type_whatsapp")
				attachDir = "/media/internal/.im-attachments/whatsapp";
			else if (svc == "type_signal")
				attachDir = "/media/internal/.im-attachments/signal";
			if (!attachDir.empty())
			{
				purple_build_dir(attachDir.c_str(), 0755);
				// $hash is a per-image content hash, $extension includes the leading dot -> unique,
				// stable filenames; re-receiving the same image just overwrites in place.
				std::string tmpl = attachDir + "/$hash$extension";
				purple_account_set_string(account, "attachment-path-template", tmpl.c_str());
				if (svc == "type_whatsapp")
					purple_account_set_string(account, "handle-images", "xfer");
			}
		}

		MojLogInfo(IMServiceApp::s_log, _T("Logging in..."));

		/* Don't clobber a persisted refresh_token (stored as the password by the prpl
		 * on a previous successful login) with the initial webOS credential. Set the
		 * password from the webOS credential only when the account has none yet — first
		 * login, or after an invalid_grant cleared it — so steady-state logins take the
		 * silent-refresh path instead of restarting the device-code flow every time. */
		{
			const char* existingPw = purple_account_get_password(account);
			if (existingPw == NULL || *existingPw == '\0')
			{
				purple_account_set_password(account, params.password.data());
			}
		}

		/* webOS: let credential-caching prpls skip re-auth on reconnect. purple-facebook only takes
		 * its saved-token path (fb_login: fb_data_load && remember_password) when this flag is set;
		 * without it, every login re-runs email+password auth and re-triggers the 2FA challenge. The
		 * token/cid/mid are already persisted in accounts.xml (fb_data_save after a successful login),
		 * so honoring remember_password here makes subsequent reconnects silent. Benign for prpls that
		 * ignore the flag. */
		purple_account_set_remember_password(account, TRUE);
	}

	// webOS: the prpl account may ALREADY be connected. libpurple auto-logs-in accounts persisted in
	// accounts.xml (auto-login=1) at startup, frequently BEFORE the transport registered its "signed-on"
	// handler (assignIMLoginState), so account_logged_in_cb never fired: the account is online at the
	// prpl level but untracked here. When login() then runs (needsToLogin, after the user goes
	// available), re-enabling an already-connected account does NOT re-emit signed-on -- so without this
	// it would stay untracked forever: availability never resets to ONLINE, getFullBuddyList never runs,
	// and callbacks see an empty serviceName (the JID then leaks into contacts + incoming messages).
	// Adopt the live connection instead: ensure ui_data, register it online, and report LOGIN_SUCCESS so
	// the login-state machine advances to GETTING_BUDDIES. A freshly-created account is NOT yet connected
	// here, so it correctly falls through to the normal connect path below.
	if (result == OK && account != NULL && purple_account_is_connected(account) &&
	    s_onlineAccountData.count(accountKey) == 0)
	{
		if (account->ui_data == NULL)
		{
			AccountMetaData* amd = new AccountMetaData;
			amd->account_key = accountKey;
			amd->servicename = params.serviceName.data();
			account->ui_data = (void*)amd;
		}
		if (!params.accountId.empty())
			purple_account_set_string(account, "webosAccountId", params.accountId.data());
		s_onlineAccountData[accountKey] = account;
		s_pendingAccountData.erase(accountKey);
		s_ipAddressesBoundTo[accountKey] = params.localIpAddress.data();
		MojLogInfo(IMServiceApp::s_log, _T("LibpurpleAdapter::login: adopting already-connected account %s (auto-login raced the signed-on handler)"), accountKey.c_str());
		if (loginState)
			((LoginCallbackInterface*)loginState)->loginResult(params.serviceName.data(), params.username.data(),
			    LoginCallbackInterface::LOGIN_SUCCESS, false, ERROR_NO_ERROR, true);
		return OK;
	}

	if (result == OK)
	{
		/* mark the account as pending */
        s_pendingAccountData[accountKey] = account;

		if (!params.localIpAddress.empty())
		{
			/* keep track of the local IP address that we bound to when logging in to this account */
            s_ipAddressesBoundTo[accountKey] = params.localIpAddress;
		}

		/* It's necessary to enable the account first. */
		purple_account_set_enabled(account, UI_ID, TRUE);

		/* Now, to connect the account, create a status and activate it. */

		/*
		 * Create a timer for this account's login so it can fail the login after a timeout.
         *
         * BUG: This is actually a memory leak, as we are currently not able to
         *      delete the string-ptr on removal. Need to implement our own
         *      EventUiOps that handle this.
         */
        /* Discord logs in via an interactive QR / remote-auth flow that waits on the
         * user's phone; use the longer grace period so we don't tear the account down
         * mid-handshake. Telegram is likewise interactive: the user must type the login
         * code AND (if enabled) a 2FA password into the "Telegram" auth chat, which
         * easily exceeds the normal 45s. Facebook (E2EE, prpl-gometa) is likewise
         * interactive when the account has two-factor enabled: messagix selects the
         * "Notification on another device" (approve-from-another-device) method and then
         * polls, waiting for the user to approve the login on their phone — which easily
         * exceeds 45s. Give all these the longer grace period; otherwise the 45s connect
         * timeout fires mid-approval, force-disconnects the healthy login (the in-flight
         * poll then fails with "context canceled") and the account manager records it as
         * AcctMgr_Bad_Authentication — i.e. the user sees "wrong username and password"
         * even though the credentials were fine. Other protocols keep the normal timeout.
         * (Note: the retired plain purple-facebook was "prpl-facebook"; the current E2EE
         * plugin registers as "prpl-gometa".) */
        const char* protoId = purple_account_get_protocol_id(account);
        bool interactiveAuth = (protoId != NULL &&
                                (strcmp(protoId, "prpl-discord") == 0 ||
                                 strcmp(protoId, "prpl-telegram") == 0 ||
                                 strcmp(protoId, "prpl-gometa") == 0 ||
                                 strcmp(protoId, "prpl-hehoe-presage") == 0));
        guint connectTimeout = interactiveAuth ? QR_CONNECT_TIMEOUT_SECONDS : CONNECT_TIMEOUT_SECONDS;
        guint timerHandle = purple_timeout_add_seconds(connectTimeout, connectTimeoutCallback, new std::string(accountKey));
        s_accountLoginTimers[accountKey] = timerHandle;

		PurpleStatusPrimitive prim = getPurpleAvailabilityFromPalmAvailability(params.availability);
		PurpleSavedStatus* savedStatus = purple_savedstatus_new(NULL, prim);
        if (!params.customMessage.empty())
		{
			purple_savedstatus_set_message(savedStatus, params.customMessage);
		}
		purple_savedstatus_activate_for_account(savedStatus, account);
	}

	return result;
}

bool LibpurpleAdapter::logout(const char* serviceName, const char* username, LoginCallbackInterface* loginState)
{
	MojLogInfo(IMServiceApp::s_log, _T("%s called."), __FUNCTION__);

	if (!username || !serviceName)
	{
		MojLogError(IMServiceApp::s_log, _T("Invalid logout parameter. Please double check the passed parameters."));
		return FALSE;
	}

	bool success = TRUE;

	MojLogInfo(IMServiceApp::s_log, _T("Parameters: servicename %s"), serviceName);

	std::string const& accountKey = getAccountKey(username, serviceName);

	// Remove the accountId since a logout could be from the user removing the account
	s_AccountIdsData.erase(accountKey);

	PurpleAccount* accountTologoutFrom = 0;

	if (s_onlineAccountData.count(accountKey))
		accountTologoutFrom = s_onlineAccountData[accountKey];
	else if (s_pendingAccountData.count(accountKey))
		accountTologoutFrom = s_pendingAccountData[accountKey];
	else
	{
		MojLogError(IMServiceApp::s_log, _T("Trying to logout from an account that is not logged in. service name %s"), serviceName);
		success = FALSE;
	}

	if (accountTologoutFrom != NULL)
	{
		purple_account_disconnect(accountTologoutFrom);
	}

	return success;
}

bool LibpurpleAdapter::setMyAvailability(const char* serviceName, const char* username, int availability)
{
	MojLogInfo(IMServiceApp::s_log, _T("%s called."), __FUNCTION__);

	if (!serviceName || !username)
	{
		MojLogError(IMServiceApp::s_log, _T("setMyAvailability: Invalid parameter. Please double check the passed parameters."));
		return FALSE;
	}

	MojLogInfo(IMServiceApp::s_log, _T("Parameters: serviceName %s, availability %i"), serviceName, availability);

	bool retVal = FALSE;
	std::string accountKey = getAccountKey(username, serviceName);
	if (s_onlineAccountData.count(accountKey) == 0)
	{
		//this should never happen based on MessagingService's logic
		MojLogError(IMServiceApp::s_log,
				_T("setMyAvailability was called on an account that wasn't logged in. serviceName: %s, availability: %i"),
				serviceName, availability);
		retVal = FALSE;
	}
	else
	{
    	PurpleAccount* account = s_onlineAccountData[accountKey];
		retVal = TRUE;

		/*
		 * Let's get the current custom message and set it as well so that we don't overwrite it with ""
		 */
		PurplePresence* presence = purple_account_get_presence(account);
		const PurpleStatus* status = purple_presence_get_active_status(presence);
		const PurpleValue* value = purple_status_get_attr_value(status, "message");
		const char* customMessage = NULL;
		if (value != NULL)
		{
			customMessage = purple_value_get_string(value);
		}
		if (customMessage == NULL)
		{
			customMessage = "";
		}

		PurpleStatusPrimitive prim = getPurpleAvailabilityFromPalmAvailability(availability);
		PurpleStatusType* type = purple_account_get_status_type_with_primitive(account, prim);
		GList* attrs = NULL;
		attrs = g_list_append(attrs, (void*)"message");
		attrs = g_list_append(attrs, (char*)customMessage);
		purple_account_set_status_list(account, purple_status_type_get_id(type), TRUE, attrs);
	}

	return retVal;
}

bool LibpurpleAdapter::setMyCustomMessage(const char* serviceName, const char* username, const char* customMessage)
{
	MojLogInfo(IMServiceApp::s_log, _T("%s called."), __FUNCTION__);

	if (!serviceName || !username || !customMessage)
	{
		MojLogError(IMServiceApp::s_log, _T("setMyCustomMessage: Invalid parameter. Please double check the passed parameters."));
		return FALSE;
	}

	MojLogInfo(IMServiceApp::s_log, _T("Parameters: serviceName %s"), serviceName);

	bool retVal = FALSE;
	std::string accountKey = getAccountKey(username, serviceName);

	if (s_onlineAccountData.count(accountKey) == 0)
	{
		//this should never happen based on MessagingService's logic
		MojLogError(IMServiceApp::s_log,
				_T("setMyCustomMessage was called on an account that wasn't logged in. serviceName: %s"),
				serviceName);
		retVal = FALSE;
	}
	else
	{
    	PurpleAccount* account = s_onlineAccountData[accountKey];
		retVal = TRUE;

		// get the account's current status type
		PurpleStatusType* type = purple_status_get_type(purple_account_get_active_status(account));
		GList* attrs = NULL;
		attrs = g_list_append(attrs, (void*)"message");
		attrs = g_list_append(attrs, (char*)customMessage);
		purple_account_set_status_list(account, purple_status_type_get_id(type), TRUE, attrs);
	}

	return retVal;
}

/*
 * Block this user from sending us messages
 */
LibpurpleAdapter::SendResult LibpurpleAdapter::blockBuddy(const char* serviceName, const char* username, const char* buddyUsername, bool block)
{
	bool success = FALSE;
	MojLogInfo(IMServiceApp::s_log, _T("%s called."), __FUNCTION__);

	if (!serviceName)
	{
		MojLogError(IMServiceApp::s_log, _T("blockBuddy: null serviceNname"));
		return INVALID_PARAMS;
	}
	if (!username)
	{
		MojLogError(IMServiceApp::s_log, _T("blockBuddy: null username"));
		return INVALID_PARAMS;
	}
	if (!buddyUsername)
	{
		MojLogError(IMServiceApp::s_log, _T("blockBuddy: null buddyUsername"));
		return INVALID_PARAMS;
	}

	std::string accountKey = getAccountKey(username, serviceName);

	if (s_onlineAccountData.count(accountKey) == 0)
	{
		MojLogError(IMServiceApp::s_log, "blockBuddy: Trying to send from an account that is not logged in. service name %s", serviceName);
	}
    else
	{
    	PurpleAccount* account = s_onlineAccountData[accountKey];
        success = TRUE;
		if (block)
		{
			MojLogInfo(IMServiceApp::s_log, _T("blockBuddy: deny %s. account perm_deny %d"), buddyUsername, account->perm_deny);
			purple_privacy_deny(account, buddyUsername, false, true);
			//bool success = purple_privacy_deny_add(account, transportFriendlyUserName, false);
			//if (!success) {
			//	MojLogError(IMServiceApp::s_log, "blockBuddy: purple_privacy_deny_add - returned false");
			//	GSList *l;
			//	for (l = account->deny; l != NULL; l = l->next) {
			//		MojLogError(IMServiceApp::s_log, "account deny list: %s", purple_normalize(account, (char *)l->data));
			//	}
			//}
		}
		else
		{
			MojLogInfo(IMServiceApp::s_log, _T("blockBuddy: allow %s"), buddyUsername);
			purple_privacy_allow(account, buddyUsername, false, true);
		}
	}

	if (success)
		return SENT;
	else
        return USER_NOT_LOGGED_IN;
}

/*
 * Remove a buddy from our account
 */
LibpurpleAdapter::SendResult LibpurpleAdapter::removeBuddy(const char* serviceName, const char* username, const char* buddyUsername)
{
    LibpurpleAdapter::SendResult res = USER_NOT_LOGGED_IN;
	MojLogInfo(IMServiceApp::s_log, _T("%s called."), __FUNCTION__);

	if (!serviceName)
	{
		MojLogError(IMServiceApp::s_log, _T("removeBuddy: null serviceNname"));
		return INVALID_PARAMS;
	}
	if (!username)
	{
		MojLogError(IMServiceApp::s_log, _T("removeBuddy: null username"));
		return INVALID_PARAMS;
	}
	if (!buddyUsername)
	{
		MojLogError(IMServiceApp::s_log, _T("removeBuddy: null buddyUsername"));
		return INVALID_PARAMS;
	}

	std::string accountKey = getAccountKey(username, serviceName);

	if (s_onlineAccountData.count(accountKey) == 0)
	{
		MojLogError(IMServiceApp::s_log, "removeBuddy: Trying to send from an account that is not logged in. service name %s", serviceName);
	}
    else
	{
    	PurpleAccount* account = s_onlineAccountData[accountKey];

		MojLogInfo(IMServiceApp::s_log, _T("removeBuddy: %s"), buddyUsername);

		PurpleBuddy* buddy = purple_find_buddy(account, buddyUsername);
		if (NULL == buddy) {
            res = INVALID_PARAMS;

			MojLogError(IMServiceApp::s_log, _T("could not find buddy in list - cannot remove"));
		}
		else {
			PurpleGroup* group = purple_buddy_get_group(buddy);
			// remove from server list
			purple_account_remove_buddy(account, buddy, group);

			// remove from buddy list - generates a "buddy-removed" signal
			purple_blist_remove_buddy(buddy);

            res = SENT;
		}
	}

	return res;
}

/*
 * Add a buddy to our buddy list. Some services (GTalk) will require the buddy to authorize us to add them
 */
LibpurpleAdapter::SendResult LibpurpleAdapter::addBuddy(const char* serviceName, const char* username, const char* buddyUsername, const char* groupName)
{
	bool success = FALSE;
	MojLogInfo(IMServiceApp::s_log, _T("%s called."), __FUNCTION__);

	if (!serviceName)
	{
		MojLogError(IMServiceApp::s_log, _T("addBuddy: null serviceNname"));
		return INVALID_PARAMS;
	}
	if (!username)
	{
		MojLogError(IMServiceApp::s_log, _T("addBuddy: null username"));
		return INVALID_PARAMS;
	}
	if (!buddyUsername)
	{
		MojLogError(IMServiceApp::s_log, _T("addBuddy: null buddyUsername"));
		return INVALID_PARAMS;
	}

	std::string accountKey = getAccountKey(username, serviceName);

	if (s_onlineAccountData.count(accountKey) == 0)
	{
		MojLogError(IMServiceApp::s_log, "addBuddy: Trying to send from an account that is not logged in. service name %s", serviceName);
	}
	else
	{
    	PurpleAccount* account = s_onlineAccountData[accountKey];

        success = TRUE;
		MojLogInfo(IMServiceApp::s_log, _T("addBuddy: %s"), buddyUsername);

		PurpleBuddy* buddy = purple_buddy_new(account, buddyUsername, /*alias*/ NULL);

		// add buddy to list
		/*
		void purple_blist_add_buddy  	(  	PurpleBuddy *   	 buddy,
				PurpleContact *  	contact,
				PurpleGroup *  	group,
				PurpleBlistNode *  	node
			)

		Adds a new buddy to the buddy list.
		The buddy will be inserted right after node or prepended to the group if node is NULL. If both are NULL, the buddy will be added to the "Buddies" group.
		*/
		PurpleGroup *group = NULL;
		if (NULL != groupName && *groupName != '\0') {
			group = purple_find_group(groupName);
			if (NULL == group){
				// group not there - add it
				MojLogInfo(IMServiceApp::s_log, _T("addBuddy: adding new group %s"), groupName);
				group = purple_group_new(groupName);
			}
		}
		purple_blist_add_buddy(buddy, NULL, group, NULL);

		// add to server - note: this has to be called AFTER the purple_blist_add_buddy() call otherwise it seg faults...
		purple_account_add_buddy(account, buddy);

		// note - there seems to be an inconsistency in libpurple where AIM buddies added via purple appear offline until the account is logged off and on...
		// see http://pidgin.im/pipermail/devel/2007-June/001517.html:
		/*	Such is not the case on AIM, however.  The behavior I see here is that the
			buddies appear in the Pidgin blist but always have an offline status, even
			though I know at least four of these people are online.  Looking at blist.xml
			after the next shown flush in the debug window shows that nothing has been added
			to the local list.  The alias and buddy notes are not added, either.  The status
			remains incorrect until I restart Pidgin, disable and enable the account, or
			switch to the offline status and then back to an online status.  At this point,
			the buddies are finally shown in blist.xml and statuses are correct; however,
			the alias and notes string that were set at import are missing.  Behavior is
			identical on ICQ when importing a list of AIM buddies (which to my limited
			knowledge does not require authorization).
		*/
	}

	if (success)
		return SENT;
	else
        return USER_NOT_LOGGED_IN;
}

/*
 * Authorize the remote user to be our buddy
 */
LibpurpleAdapter::SendResult LibpurpleAdapter::authorizeBuddy(const char* serviceName, const char* username, const char* fromUsername)
{
	MojLogInfo(IMServiceApp::s_log, _T("authorizeBuddy: username: %s, serviceName: %s, buddyUsername: %s"), username, serviceName, fromUsername);

	if (!serviceName || !username || !fromUsername)
	{
		MojLogError(IMServiceApp::s_log, _T("authorizeBuddy: null parameter. cannot process command"));
		return INVALID_PARAMS;
	}

	// if we got here, we need to be online...
	std::string const& accountKey = getAccountKey(username, serviceName);

	if (s_onlineAccountData.count(accountKey) == 0)
	{
		MojLogError(IMServiceApp::s_log, _T("authorizeBuddy: account not online"));
		return USER_NOT_LOGGED_IN;
	}

	// create the key and find the auth_and_add object in the s_AuthorizeRequests table
	char *authRequestKey = getAuthRequestKey(username, serviceName, fromUsername);
	AuthRequest *aa = (AuthRequest *)g_hash_table_lookup(s_AuthorizeRequests, authRequestKey);
	if (NULL == aa)
	{
		MojLogError(IMServiceApp::s_log, "authorizeBuddy: cannot find auth request object");
		// log the table
		logAuthRequestTableValues();
		free (authRequestKey);
		return SEND_FAILED;
	}

	// authorize account
	aa->auth_cb(aa->data);

	// TODO - do we need to add this user to our buddy list? Libpurple seems to add it automatically - appears at next login.

	// we are done with this request - remove it from list
	// object and key held by table get deleted by our destroy functions specified in the hash table construction
	g_hash_table_remove(s_AuthorizeRequests, authRequestKey);

	// free key used for look-up
	free (authRequestKey);

	return SENT;
}

/*
 * Decline the request from the remote user to be our buddy
 */
LibpurpleAdapter::SendResult LibpurpleAdapter::declineBuddy(const char* serviceName, const char* username, const char* fromUsername)
{
	MojLogInfo(IMServiceApp::s_log, _T("declineBuddy: username: %s, serviceName: %s, buddyUsername: %s"), username, serviceName, fromUsername);
	if (!serviceName)
	{
		MojLogError(IMServiceApp::s_log, _T("declineBuddy: null serviceNname"));
		return INVALID_PARAMS;
	}
	if (!username)
	{
		MojLogError(IMServiceApp::s_log, _T("declineBuddy: null username"));
		return INVALID_PARAMS;
	}
	if (!fromUsername)
	{
		MojLogError(IMServiceApp::s_log, _T("declineBuddy: null fromUsername"));
		return INVALID_PARAMS;
	}

	// if we got here, we need to be online...
	std::string accountKey = getAccountKey(username, serviceName);
	if (s_onlineAccountData.count(accountKey) == 0)
	{
		MojLogError(IMServiceApp::s_log, _T("declineBuddy: account not online"));
		return USER_NOT_LOGGED_IN;
	}

	// create the key and find the auth_and_add object in the s_AuthorizeRequests table
	char *authRequestKey = getAuthRequestKey(username, serviceName, fromUsername);
	AuthRequest *aa = (AuthRequest *)g_hash_table_lookup(s_AuthorizeRequests, authRequestKey);
	if (NULL == aa)
	{
		MojLogError(IMServiceApp::s_log, "declineBuddy: cannot find auth request object");
		// log the table
		logAuthRequestTableValues();
		free (authRequestKey);
		return SEND_FAILED;
	}

	aa->deny_cb(aa->data);

	// we are done with this request - remove it from list
	// object gets deleted by our destroy functions specified in the hash table construction
	g_hash_table_remove(s_AuthorizeRequests, authRequestKey);

	// free key used for look-up
	free (authRequestKey);
	return SENT;
}

// webOS Telegram port: reduce a display name to plain printable ASCII (0x20-0x7E), collapsing runs of
// whitespace and trimming. Old webOS (enyo, no emoji/CJK/Thai fonts) renders anything else as tofu
// boxes, and astral-plane chars (emoji) can break the messaging JS. May return empty if the input had
// no ASCII content. Non-ASCII bytes are dropped without emitting a space, so "a<emoji>b" -> "ab" while
// "a <emoji> b" -> "a b".
static std::string reduceToAscii(const char* in)
{
	std::string out;
	if (in == NULL)
		return out;
	bool prevSpace = false;
	for (const unsigned char* p = (const unsigned char*)in; *p; ++p)
	{
		unsigned char c = *p;
		if (c < 0x20 || c > 0x7E)
			continue; // drop non-printable / non-ASCII
		bool isSpace = (c == ' ' || c == '\t');
		if (isSpace)
		{
			if (!out.empty() && !prevSpace)
				out.push_back(' ');
			prevSpace = true;
		}
		else
		{
			out.push_back((char)c);
			prevSpace = false;
		}
	}
	while (!out.empty() && out[out.size() - 1] == ' ')
		out.erase(out.size() - 1);
	return out;
}

// webOS Telegram port: drop astral-plane characters (Unicode > U+FFFF: emoji, flags, rare CJK-ext) from
// a display name, keeping ALL Basic-Multilingual-Plane text - Latin, Cyrillic, Greek, Thai, CJK, BMP
// symbols - which the webOS WebKit renders fine via the fallback-font slots. WebKit's font fallback is
// UTF-16/BMP-oriented and shows astral codepoints as the replacement glyph (U+FFFD), no matter what
// emoji font is installed, so we strip those rather than leave a row of "?" glyphs. Whitespace left
// where an emoji was removed is collapsed, and the result is trimmed.
static std::string stripAstral(const char* in)
{
	std::string out;
	if (in == NULL)
		return out;
	bool prevSpace = false;
	const unsigned char* p = (const unsigned char*)in;
	while (*p)
	{
		unsigned char c = *p;
		int len = 1;
		if (c >= 0xF0)      len = 4;   // 4-byte UTF-8 == U+10000.. (astral) -> drop
		else if (c >= 0xE0) len = 3;   // 3-byte BMP (Thai, CJK, symbols like U+26A1)
		else if (c >= 0xC0) len = 2;   // 2-byte BMP (Latin-ext, Cyrillic, Greek, Arabic, Hebrew)
		for (int i = 1; i < len; ++i)  // guard against a truncated trailing sequence
			if ((p[i] & 0xC0) != 0x80) { len = 1; break; }

		if (len == 4)
		{
			p += 4;
			continue;
		}
		bool isSpace = (len == 1 && (c == ' ' || c == '\t'));
		if (isSpace)
		{
			if (!out.empty() && !prevSpace)
				out.push_back(' ');
			prevSpace = true;
		}
		else
		{
			out.append((const char*)p, len);
			prevSpace = false;
		}
		p += len;
	}
	while (!out.empty() && out[out.size() - 1] == ' ')
		out.erase(out.size() - 1);
	return out;
}

// webOS Telegram port: true if a buddy display name (alias) is effectively empty (NULL/""/whitespace).
// tdlib gives deleted Telegram accounts no name, so they arrive as nameless buddies whose contact then
// shows the raw "id<number>" - skip those entirely.
static bool isBlankName(const char* s)
{
	if (s == NULL)
		return true;
	for (const char* p = s; *p; ++p)
		if (*p != ' ' && *p != '\t')
			return false;
	return true;
}

// True if a WhatsApp buddy string is just a raw id (no human push-name): the part before '@' is
// only digits/phone punctuation. A real push-name ("Alan", "Vladushka") has a letter there.
// true if s is a bare Signal ACI UUID like "1594a976-5256-4fc6-b855-d23232cc5579" (8-4-4-4-12 hex).
static bool isSignalUuid(const char* s)
{
	if (s == NULL)
		return false;
	std::string u(s);
	if (u.size() != 36)
		return false;
	for (size_t i = 0; i < 36; ++i)
	{
		char c = u[i];
		if (i == 8 || i == 13 || i == 18 || i == 23)
		{
			if (c != '-') return false;
		}
		else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
			return false;
	}
	return true;
}

static bool isWhatsAppRawId(const char* s)
{
	if (s == NULL || *s == '\0')
		return true;
	std::string u(s);
	size_t at = u.find('@');
	std::string local = (at == std::string::npos) ? u : u.substr(0, at);
	if (local.empty())
		return true;
	for (size_t i = 0; i < local.size(); ++i)
	{
		char c = local[i];
		if (!((c >= '0' && c <= '9') || c == '+' || c == '.' || c == ':' || c == '-'))
			return false; // a letter/other -> real push-name
	}
	return true;
}

// Human-friendly display name for a WhatsApp buddy: prefer a real push-name; otherwise format the
// phone JID "<digits>@s.whatsapp.net" as "+<digits>". Never emit the raw "<id>@s.whatsapp.net" /
// "<id>@lid" (a "@lid" is an opaque LinkedID with no phone -> fall back to its bare id).
static std::string whatsAppDisplayName(const char* alias, const char* username)
{
	if (!isWhatsAppRawId(alias))
		return alias;
	std::string u = username ? username : "";
	size_t at = u.find('@');
	std::string local = (at == std::string::npos) ? u : u.substr(0, at);
	std::string suffix = (at == std::string::npos) ? std::string() : u.substr(at);
	if (!local.empty() && local[0] == '+')
		local.erase(0, 1);
	if (suffix == "@s.whatsapp.net" && !local.empty())
		return "+" + local;
	// "@lid" (LinkedID) or any other non-phone JID: no phone number is available and the local part
	// is an opaque identifier. Never surface that raw id to the user -- WhatsApp normally supplies a
	// push-name (used above when alias is non-raw); when even that is missing, show a generic label.
	return "WhatsApp user";
}

// If a WhatsApp buddy id is a phone-number JID ("<digits>@s.whatsapp.net"), return the bare digits so
// the contact record can carry a +E.164 phoneNumber (BuddyListConsolidator prepends the "+"). That
// lets the contacts linker MERGE the buddy into the device contact that already has that number,
// instead of creating a duplicate JID-only contact -- and the linked contact then shows its real name.
// Returns "" for opaque "@lid" ids, group ids, or any id whose local part isn't purely digits (e.g. a
// ":NN" device suffix), where no phone number can be trusted.
static std::string whatsAppPhoneFromJid(const char* username)
{
	std::string u = username ? username : "";
	static const std::string waSuffix = "@s.whatsapp.net";
	if (u.size() <= waSuffix.size() ||
	    u.compare(u.size() - waSuffix.size(), waSuffix.size(), waSuffix) != 0)
		return "";
	std::string local = u.substr(0, u.size() - waSuffix.size());
	if (local.empty())
		return "";
	for (std::string::size_type i = 0; i < local.size(); ++i)
		if (local[i] < '0' || local[i] > '9')
			return "";
	return local;
}

bool LibpurpleAdapter::getFullBuddyList(const char* serviceName, const char* username)
{
	MojLogInfo(IMServiceApp::s_log, "%s called.", __FUNCTION__);

	if (!serviceName || !username)
	{
		MojLogError(IMServiceApp::s_log, _T("getBuddyList: Invalid parameter. Please double check the passed parameters."));
		return FALSE;
	}

	if (s_loginState == NULL)
	{
		MojLogError(IMServiceApp::s_log, _T("getBuddyList: s_loginState still null."));
		return FALSE;
	}

	MojLogInfo(IMServiceApp::s_log, _T("getFullBuddyList: Parameters: serviceName %s, username %s"), serviceName, username);

	/*
	 * Send over the full buddy list if the account is already logged in
	 */
	bool success = FALSE;
	std::string accountKey = getAccountKey(username, serviceName);
	if (s_onlineAccountData.count(accountKey) == 0)
	{
		MojLogError(IMServiceApp::s_log, _T("getFullBuddyList: ERROR: No account for user on %s"), serviceName);
	}
	else
	{
    	PurpleAccount* account = s_onlineAccountData[accountKey];
		success = TRUE;
		GSList* buddyList = purple_find_buddies(account, NULL);
		MojObject buddyListObj;
		if (!buddyList)
		{
			// webOS resilience: an ONLINE account with ZERO buddies almost always means the protocol
			// (tdlib) has not finished loading its contact/chat list yet - or couldn't (e.g. /var full).
			// Reporting an empty full list here would make the BuddyListConsolidator DELETE every
			// existing contact (this wiped all Telegram contacts when tdlib couldn't load). Treat it as
			// "not ready": return false so getBuddyLists cleans up WITHOUT deleting anything. The
			// debounced buddy-added resync runs the real sync once buddies actually load.
			MojLogWarning(IMServiceApp::s_log, _T("getFullBuddyList: 0 buddies for online account %s - skipping sync to avoid wiping contacts"), serviceName);
			return FALSE;
		}

		GSList* buddyIterator = NULL;
		PurpleBuddy* buddyToBeAdded = NULL;
		PurpleGroup* group = NULL;
		char* buddyAvatarLocation = NULL;

		//printf("\n\n\n\n\n\n ---------------- BUDDY LIST SIZE: %d----------------- \n\n\n\n\n\n", g_slist_length(buddyList));
		for (buddyIterator = buddyList; buddyIterator != NULL; buddyIterator = buddyIterator->next)
		{
			MojObject buddyObj;
			buddyObj.clear(MojObject::TypeArray);
			buddyToBeAdded = (PurpleBuddy*)buddyIterator->data;

			// webOS: resolve the buddy's display name as local alias, else SERVER alias. tdlib-purple
			// (Telegram) writes the local ->alias, but purple-facebook only ever calls
			// purple_buddy_set_server_alias(), so reading ->alias directly left every Facebook buddy
			// nameless -> skipped here -> absent from Contacts. purple_buddy_get_alias_only() returns
			// the local alias, else the server alias, or NULL if neither is set (it does NOT fall back
			// to the numeric username), which is exactly the "is this buddy nameless?" test we want.
			const char* resolvedAlias = purple_buddy_get_alias_only(buddyToBeAdded);

			// WhatsApp buddies always get a readable name (push-name, else a formatted "+<phone>")
			// -- never skipped and never shown as the raw "<id>@s.whatsapp.net" / "<id>@lid".
			bool isWhatsApp = (serviceName != NULL && strcmp(serviceName, "type_whatsapp") == 0);
			bool isSignal = (serviceName != NULL && strcmp(serviceName, "type_signal") == 0);
			bool isGometa = (serviceName != NULL && strcmp(serviceName, "type_gometa") == 0);
			std::string waName;
			if (isWhatsApp)
			{
				waName = whatsAppDisplayName(resolvedAlias, buddyToBeAdded->name);
				// Carry the buddy's phone number (bare digits; BuddyListConsolidator adds "+") so the
				// contacts linker merges this buddy into the existing device contact with that number
				// instead of creating a JID-only duplicate. Empty for "@lid" / group ids -> no merge.
				std::string waPhone = whatsAppPhoneFromJid(buddyToBeAdded->name);
				if (!waPhone.empty())
					buddyObj.putString("phoneNumber", waPhone.c_str());
			}
			// webOS Signal: never DROP a Signal contact for being nameless. On a linked (secondary)
			// device purple-presage often has no synced address-book name, so the buddy arrives with
			// an empty alias; skipping it here (like the tdlib path below) hid real contacts that were
			// actively messaging (e.g. +31611745571 / Alan). Keep it and fall back to the username
			// (a "+<phone>" or UUID) in the displayName block below. presage's profile-name sync
			// upgrades this to a real name when one becomes available.
			else if (isSignal)
			{
				// intentionally not skipped; displayName falls back to the username below
			}
			// webOS Telegram port: skip deleted/nameless users. tdlib gives them no name, so the
			// contact would otherwise show a raw "id<number>". Not reporting them here also makes the
			// BuddyListConsolidator delete any such contacts left from a previous (pre-filter) sync.
			else if (isBlankName(resolvedAlias) && !isGometa)
			{
				// tdlib gives DELETED Telegram users no name -> skip them (they'd show as "id<number>").
				// But a nameless FACEBOOK (gometa) buddy is NOT deleted -- purple-facebook only sets the
				// SERVER alias, which lags on reconnect, so the buddy legitimately arrives nameless. Skipping
				// it drops it from the roster, which then makes BuddyListConsolidator DELETE its contact ->
				// it comes back unlinked (744870190 no longer merged into the Alan Morford person). So keep
				// gometa buddies: they fall through with no displayName (formatForDB keeps remoteId+ims; the
				// merge preserves any existing good name, see hasChanges), and the name fills in when the
				// server alias syncs.
				MojLogInfo(IMServiceApp::s_log, _T("getFullBuddyList: skipping nameless buddy %s (deleted user?)"), buddyToBeAdded->name);
				continue;
			}

			// Store the buddy's webOS-facing address: for WhatsApp the raw whatsmeow phone JID
			// ("<digits>@s.whatsapp.net") becomes "+<digits>", so the contact's ims.value + remoteId (and
			// the conversation "to") read as "+31638307067" instead of the JID, matching the +E.164 the
			// incoming-message from.addr now uses. Opaque "@lid"/group ids pass through unchanged. The
			// display name (waName, above) and phone-merge were already derived from the raw JID.
			std::string const buddyWebosName = getWebosUsername(buddyToBeAdded->name, serviceName, purple_buddy_get_account(buddyToBeAdded));
			buddyObj.putString("username", buddyWebosName.c_str());
			buddyObj.putString("serviceName", serviceName);

			group = purple_buddy_get_group(buddyToBeAdded);
			const char* groupName = purple_group_get_name(group);
			buddyObj.putString("group", groupName);

			/*
			 * Getting the availability
			 */
			PurpleStatus* status = purple_presence_get_active_status(purple_buddy_get_presence(buddyToBeAdded));
			int newStatusPrimitive = purple_status_type_get_primitive(purple_status_get_type(status));
			int availability = getPalmAvailabilityFromPurpleAvailability(newStatusPrimitive);
			buddyObj.putInt("availability", availability);

			if (isWhatsApp)
			{
				std::string cleanName = stripAstral(waName.c_str());
				buddyObj.putString("displayName", cleanName.empty() ? waName.c_str() : cleanName.c_str());
			}
			else if (resolvedAlias != NULL)
			{
				// webOS Telegram port: strip only astral emoji/flags (unrenderable on this WebKit), keep
				// all BMP text (Thai/Cyrillic/CJK/Latin) which renders via the fallback-font slots. If the
				// name was entirely astral it strips to empty - keep the original then, so we never fall
				// back to a raw "id<number>".
				std::string cleanName = stripAstral(resolvedAlias);
				buddyObj.putString("displayName", cleanName.empty() ? resolvedAlias : cleanName.c_str());
			}
			else if (isSignal)
			{
				// nameless Signal buddy (kept above): show its username -- a "+<phone>" or UUID -- so
				// the contact still appears (by number) instead of vanishing from Contacts.
				const char* u = buddyToBeAdded->name ? buddyToBeAdded->name : "";
				std::string cleanName = stripAstral(u);
				buddyObj.putString("displayName", cleanName.empty() ? u : cleanName.c_str());
			}

			PurpleBuddyIcon* icon = purple_buddy_get_icon(buddyToBeAdded);
			if (icon != NULL)
			{
				buddyAvatarLocation = purple_buddy_icon_get_full_path(icon);
				buddyObj.putString("avatar", buddyAvatarLocation);
				MojLogInfo(IMServiceApp::s_log, _T("getFullBuddyList: buddy %s avatar %s."), buddyToBeAdded->name, buddyAvatarLocation);
			}
			else
			{
				buddyObj.putString("avatar", "");
				MojLogInfo(IMServiceApp::s_log, _T("getFullBuddyList: buddy %s has no avatar."), buddyToBeAdded->name);
			}

			const char* customMessage = purple_status_get_attr_string(status, "message");
			if (customMessage != NULL)
			{
				buddyObj.putString("status", customMessage);
			}

			// webOS Telegram port: extra profile info the prpl (tdlib-purple) stashed on the buddy node
			// (keys must match BuddyOptions in purple-info.h). Forwarded so BuddyListConsolidator can
			// enrich the db8 contact with phone / @username / structured name instead of only an id.
			{
				PurpleBlistNode* bnode = (PurpleBlistNode*)buddyToBeAdded;
				const char* bPhone = purple_blist_node_get_string(bnode, "tdlib-phone");
				const char* bUser  = purple_blist_node_get_string(bnode, "tdlib-username");
				const char* bFirst = purple_blist_node_get_string(bnode, "tdlib-first-name");
				const char* bLast  = purple_blist_node_get_string(bnode, "tdlib-last-name");
				if (bPhone && *bPhone) buddyObj.putString("phoneNumber", bPhone);
				if (bUser  && *bUser)  buddyObj.putString("handle", bUser);   // @username
				// Strip astral emoji from the structured name (renders as the header) - keep BMP text.
				if (bFirst && *bFirst) { std::string s = stripAstral(bFirst); if (!s.empty()) buddyObj.putString("firstName", s.c_str()); }
				if (bLast  && *bLast)  { std::string s = stripAstral(bLast);  if (!s.empty()) buddyObj.putString("lastName", s.c_str()); }
			}

			g_message("%s says: %s's presence: availability: '%d', custom message: '%s', avatar location: '%s', display name: '%s', group name:'%s'",
					__FUNCTION__, buddyToBeAdded->name, availability, customMessage, buddyAvatarLocation, buddyToBeAdded->alias, groupName);

			if (buddyAvatarLocation)
			{
				g_free(buddyAvatarLocation);
				buddyAvatarLocation = NULL;
			}

			buddyListObj.push(buddyObj);
		}

		s_loginState->buddyListResult(serviceName, username, buddyListObj, true);

		//TODO free the buddyList object???
	}

	return success;
}

/*
 * webOS Servers/Rooms M3: enumerate a room account's full server->channel roster from the in-memory
 * buddy list and hand it to the service handler to upsert into db8, so every guild/team/network and
 * its visible channels appear in the Servers tab immediately, independent of any incoming message.
 * Works for Discord (guild->channel), Teams (team->channel) and Telegram (flat, one synthetic server);
 * see deriveServerName. The roster is signature-compared to the previous run per account: an unchanged
 * roster is skipped, so the frequent blist-changed triggers don't churn the (delete+recreate) db8 sync.
 */
// last-enumerated roster signature per account, to skip a no-op delete+recreate (churn guard).
static std::unordered_map<std::string, std::string> s_lastServerChannelSig;

bool LibpurpleAdapter::enumerateServersChannels(const char* serviceName, const char* username)
{
	if (!serviceName || !username || s_imServiceHandler == NULL)
		return false;

	std::string accountKey = getAccountKey(username, serviceName);
	if (s_onlineAccountData.count(accountKey) == 0)
	{
		MojLogInfo(IMServiceApp::s_log, _T("enumerateServersChannels: no online account for %s/%s"), serviceName, username);
		return false;
	}
	PurpleAccount* account = s_onlineAccountData[accountKey];
	if (account == NULL)
		return false;

	// Enumerate for any room protocol (Discord, Teams, Telegram). deriveServerName maps each chat to
	// its server - guild/team for Discord/Teams, one synthetic network server for flat Telegram - the
	// same mapping incoming_message_cb uses. Non-room protocols simply have no group chats in the
	// blist, so this yields nothing and (with the empty-roster guard) is a harmless no-op.
	// server name -> server MojObject (carrying its "channels" array). std::map keeps a stable
	// (alphabetical) server order in the output.
	std::map<std::string, MojObject> serverByGuild;
	std::set<std::string> sigSet;   // order-independent roster signature ("server\x1f channelId")

	for (PurpleBlistNode* node = purple_blist_get_root(); node != NULL; node = node->next)
	{
		if (!PURPLE_BLIST_NODE_IS_GROUP(node))
			continue;
		const char* groupName = purple_group_get_name((PurpleGroup*)node);
		if (groupName == NULL || *groupName == '\0')
			continue;

		// Server + category via the shared deriveServerName (identical to incoming_message_cb):
		// Discord/Teams -> guild/team (before ": "), category -> the remainder; Telegram -> synthetic
		// network server (the group is ignored). Empty -> unrelated group, skip its chats.
		std::string categoryName;
		std::string guildName = deriveServerName(account, groupName, &categoryName);
		if (guildName.empty())
			continue;

		int position = 0;
		for (PurpleBlistNode* child = node->child; child != NULL; child = child->next)
		{
			if (!PURPLE_BLIST_NODE_IS_CHAT(child))
				continue;
			PurpleChat* chat = (PurpleChat*)child;
			if (purple_chat_get_account(chat) != account)
				continue;
			GHashTable* comps = purple_chat_get_components(chat);
			if (comps == NULL)
				continue;
			// Channel key (must equal purple_conversation_get_name so this dedups with the
			// message-driven record): Discord/Telegram store it as "id", Teams as "chatname".
			const char* chanId = (const char*)g_hash_table_lookup(comps, "id");
			if (chanId == NULL || *chanId == '\0')
				chanId = (const char*)g_hash_table_lookup(comps, "chatname");
			// Human name: Discord exposes it as the "name" component; Teams/Telegram set it as the
			// chat alias (returned by purple_chat_get_name).
			const char* chanName = (const char*)g_hash_table_lookup(comps, "name");
			if (chanName == NULL || *chanName == '\0')
				chanName = purple_chat_get_name(chat);
			if (chanId == NULL || *chanId == '\0')
				continue;   // no stable key -> skip

			// churn-guard signature: server + channel key (the set makes it order-independent).
			sigSet.insert(guildName + std::string("\x1f") + chanId);

			if (serverByGuild.find(guildName) == serverByGuild.end())
			{
				MojObject server;
				server.putString(_T("remoteId"), guildName.c_str());
				server.putString(_T("name"), guildName.c_str());
				MojObject emptyChannels(MojObject::TypeArray);
				server.put(_T("channels"), emptyChannels);
				serverByGuild[guildName] = server;
			}

			MojObject channel;
			channel.putString(_T("remoteId"), chanId);
			std::string chanDisplay = cleanChannelDisplayName(chanName ? chanName : chanId);
			if (chanDisplay.empty())
				chanDisplay = chanId;
			channel.putString(_T("name"), chanDisplay.c_str());
			if (!categoryName.empty())
				channel.putString(_T("parentId"), categoryName.c_str());
			channel.putInt(_T("position"), position++);

			MojObject& server = serverByGuild[guildName];
			MojObject channels;
			server.get(_T("channels"), channels);
			channels.push(channel);
			server.put(_T("channels"), channels);
		}
	}

	// webOS WhatsApp Channels: followed Channels (newsletters) are stored as BUDDIES ("<id>@newsletter")
	// under the "Whatsapp" blist group, NOT as CHAT nodes, so the group/chat walk above misses them.
	// Emit each as a channel under a synthetic "WhatsApp Channels" server so all followed channels show
	// on login (not only after their next post). remoteId = the newsletter JID (== purple_conversation_
	// get_name for its IM) so it dedups with the message-driven record from incoming_message_cb.
	if (strcmp(serviceName, "type_whatsapp") == 0)
	{
		static const char* kWaChannelsServer = "WhatsApp Channels";
		std::string waServer = kWaChannelsServer;
		int nlPosition = 0;
		GSList* buddies = purple_find_buddies(account, NULL);
		for (GSList* b = buddies; b != NULL; b = b->next)
		{
			PurpleBuddy* buddy = (PurpleBuddy*)b->data;
			const char* bname = buddy ? purple_buddy_get_name(buddy) : NULL;
			if (!isWhatsAppNewsletter(bname))
				continue;
			sigSet.insert(waServer + std::string("\x1f") + bname);
			if (serverByGuild.find(waServer) == serverByGuild.end())
			{
				MojObject server;
				server.putString(_T("remoteId"), waServer.c_str());
				server.putString(_T("name"), waServer.c_str());
				MojObject emptyChannels(MojObject::TypeArray);
				server.put(_T("channels"), emptyChannels);
				serverByGuild[waServer] = server;
			}
			const char* alias = purple_buddy_get_alias(buddy);
			std::string chanDisplay = cleanChannelDisplayName((alias && *alias && !isWhatsAppNewsletter(alias)) ? alias : bname);
			if (chanDisplay.empty())
				chanDisplay = bname;
			MojObject channel;
			channel.putString(_T("remoteId"), bname);
			channel.putString(_T("name"), chanDisplay.c_str());
			channel.putInt(_T("position"), nlPosition++);
			MojObject& server = serverByGuild[waServer];
			MojObject channels;
			server.get(_T("channels"), channels);
			channels.push(channel);
			server.put(_T("channels"), channels);
		}
		if (buddies != NULL)
			g_slist_free(buddies);
	}

	MojObject serversObj(MojObject::TypeArray);
	int totalChannels = 0;
	for (std::map<std::string, MojObject>::iterator it = serverByGuild.begin(); it != serverByGuild.end(); ++it)
	{
		MojObject channels;
		it->second.get(_T("channels"), channels);
		totalChannels += (int)channels.size();
		serversObj.push(it->second);
	}

	// SAFETY: if the blist yielded no channels (e.g. Discord guild trees not synced yet / out of sync),
	// do NOT sync - syncServersChannels would DELETE this account's existing server/channel records and
	// recreate nothing, wiping a working (message-driven) Servers tab. Only sync when we found a roster.
	if (serverByGuild.empty())
	{
		MojLogInfo(IMServiceApp::s_log, _T("enumerateServersChannels: %s has no channels in the blist; leaving existing records untouched"), serviceName);
		return false;
	}

	// Churn guard: if the roster is identical to the last one synced for this account, skip the
	// (destructive delete+recreate) db8 sync. The blist-changed trigger fires often (tdlib re-adds
	// Telegram chats etc.); without this the same roster would be deleted and rebuilt every few
	// seconds, flickering the Servers tab.
	std::string sig;
	for (std::set<std::string>::iterator sit = sigSet.begin(); sit != sigSet.end(); ++sit)
		sig += *sit + "\n";
	if (s_lastServerChannelSig[accountKey] == sig)
	{
		MojLogInfo(IMServiceApp::s_log, _T("enumerateServersChannels: %s roster unchanged (%d channels); skipping sync"), serviceName, totalChannels);
		return true;
	}
	s_lastServerChannelSig[accountKey] = sig;

	MojLogInfo(IMServiceApp::s_log, _T("enumerateServersChannels: %s -> %d servers, %d channels"),
		serviceName, (int)serverByGuild.size(), totalChannels);

	s_imServiceHandler->syncServersChannels(serviceName, username, serversObj);
	return true;
}

/*
 * webOS Servers/Rooms M3: join a channel on demand (called when the user opens it in the Servers tab)
 * so the prpl fetches + delivers its history. username may be NULL - the account is then resolved by
 * serviceName (first online account of that service). Joining also lands the channel in the buddy
 * list, so a subsequent send finds it instead of falling back to a 1:1 IM ("<snowflake> is offline").
 */
bool LibpurpleAdapter::openChannel(const char* serviceName, const char* username, const char* channel)
{
	if (!serviceName || !channel || !*channel)
		return false;

	PurpleAccount* account = NULL;
	if (username && *username)
	{
		std::string accountKey = getAccountKey(username, serviceName);
		if (s_onlineAccountData.count(accountKey))
			account = s_onlineAccountData[accountKey];
	}
	if (account == NULL)
	{
		// resolve by serviceName: first online account of this service
		for (std::unordered_map<std::string, PurpleAccount*>::iterator it = s_onlineAccountData.begin();
		     it != s_onlineAccountData.end(); ++it)
		{
			if (getServiceNameFromPurpleAccount(it->second) == serviceName)
			{
				account = it->second;
				break;
			}
		}
	}
	if (account == NULL)
	{
		MojLogInfo(IMServiceApp::s_log, _T("openChannel: no online account for %s"), serviceName);
		return false;
	}

	MojLogInfo(IMServiceApp::s_log, _T("openChannel: joining %s on %s"), channel, serviceName);
	return joinChannelChat(account, channel) != NULL;
}

LibpurpleAdapter::SendResult LibpurpleAdapter::sendMessage(const char* serviceName, const char* username, const char* usernameTo, const char* messageText, const char* quotedMessageId)
{
	if (!serviceName || !username || !usernameTo || !messageText)
	{
		MojLogError(IMServiceApp::s_log, _T("sendMessage: Invalid parameter. Please double check the passed parameters."));
		return LibpurpleAdapter::INVALID_PARAMS;
	}

	LibpurpleAdapter::SendResult retVal = LibpurpleAdapter::SENT;
	MojLogInfo(IMServiceApp::s_log, _T("%s called."), __FUNCTION__);

	std::string accountKey = getAccountKey(username, serviceName);

	// The Messaging app addresses WhatsApp buddies by the +E.164 ims.value we now store
	// ("+31638307067"); whatsmeow needs the device JID. Translate back here so channel lookup and the
	// 1:1 conversation both target the id the prpl knows. Group ("@g.us")/opaque ("@lid") ids and every
	// other service pass through unchanged. Repoint the local so all downstream uses see the JID.
	std::string const usernameToBuf = getPurpleUsername(usernameTo, serviceName);
	usernameTo = usernameToBuf.c_str();

	PurpleAccount* accountToSendFrom = NULL;
	if (s_onlineAccountData.count(accountKey))
	{
		accountToSendFrom = s_onlineAccountData[accountKey];
	}
	else if (s_pendingAccountData.count(accountKey))
	{
		// webOS Telegram port: the account is still authenticating (e.g. tdlib is in
		// authorizationStateWaitCode / WaitPassword). Route the outgoing message to the
		// prpl anyway so the plugin can capture the user's reply as the login code / 2FA
		// password (tdlib-purple's promptAuthInputViaChat -> tgprpl_send_im mechanism).
		// Without this the code reply is rejected here and login never completes.
		accountToSendFrom = s_pendingAccountData[accountKey];
		MojLogInfo(IMServiceApp::s_log, _T("sendMessage: account %s still authenticating; routing message to prpl for auth-input capture"), serviceName);
	}

	if (accountToSendFrom == NULL)
	{
		retVal = LibpurpleAdapter::USER_NOT_LOGGED_IN;
		MojLogError(IMServiceApp::s_log, _T("sendMessage: Trying to send from an account that is not logged in. service name %s"), serviceName);
	}
	else
	{
		// webOS Servers/Rooms M3 (outbound to channels): if the target resolves to a group channel -
		// a blist chat, matched by its "id" component (Discord channel snowflake) or by name (IRC
		// "#channel") - send into the CHAT conversation via serv_chat_send instead of opening a 1:1 IM.
		// Join the chat first if it isn't already open (purple-discord's join creates the conversation
		// synchronously via purple_serv_got_joined_chat, so the chat id is available immediately after).
		PurpleChat* channelChat = findChatByIdComponent(accountToSendFrom, usernameTo);
		if (channelChat == NULL)
			channelChat = purple_blist_find_chat(accountToSendFrom, usernameTo);
		if (channelChat != NULL)
		{
			PurpleConnection* gc = purple_account_get_connection(accountToSendFrom);
			PurpleConversation* chatConv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, usernameTo, accountToSendFrom);
			if (chatConv == NULL && gc != NULL)
			{
				GHashTable* components = purple_chat_get_components(channelChat);
				if (components != NULL)
					serv_join_chat(gc, components);
				chatConv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, usernameTo, accountToSendFrom);
			}
			if (chatConv != NULL && gc != NULL)
			{
				// webOS native reply: stash the reply target on the conv so the prpl chat-send reads it and
				// sets a real reply_to (cleared by the prpl after use). Mirror of the incoming stash.
				if (quotedMessageId && *quotedMessageId)
					purple_conversation_set_data(chatConv, "webos-reply-to", g_strdup(quotedMessageId));
				char* chatMsg = g_strcompress(messageText);
				int cerr = serv_chat_send(gc, purple_conv_chat_get_id(purple_conversation_get_chat_data(chatConv)),
						chatMsg, (PurpleMessageFlags)0);
				free(chatMsg);
				if (cerr < 0)
				{
					retVal = LibpurpleAdapter::SEND_FAILED;
					MojLogError(IMServiceApp::s_log, _T("sendMessage: serv_chat_send returned err %d for channel %s"), cerr, usernameTo);
				}
				else
					MojLogInfo(IMServiceApp::s_log, _T("sendMessage: sent to channel %s"), usernameTo);
			}
			else
			{
				retVal = LibpurpleAdapter::SEND_FAILED;
				MojLogError(IMServiceApp::s_log, _T("sendMessage: could not open chat conversation for channel %s"), usernameTo);
			}
			return retVal;
		}

		PurpleConversation* purpleConversation = purple_conversation_new(PURPLE_CONV_TYPE_IM, accountToSendFrom, usernameTo);
		char* messageTextUnescaped = g_strcompress(messageText);

		// replace this with the lower level call so we can try to get an error code back...
		// calls common_send which calls serv_send_im (conversation.c)
//		purple_conv_im_send(purple_conversation_get_im_data(purpleConversation), messageTextUnescaped);
//				common_send(PurpleConversation *conv, const char *message, PurpleMessageFlags msgflags)
//					gc = purple_conversation_get_gc(conv);
//					err = serv_send_im(gc, purple_conversation_get_name(conv), sent, msgflags);
		// we still don't seem to get an error value back there...returns 1, even for an invalid recipient
		// webOS native reply: stash the reply target on the conv so tgprpl_send_im reads it and sets a
		// real reply_to on the tdlib sendMessage (the prpl clears it after use). No-op for non-replies.
		if (quotedMessageId && *quotedMessageId)
			purple_conversation_set_data(purpleConversation, "webos-reply-to", g_strdup(quotedMessageId));
		int err = serv_send_im(purple_conversation_get_gc(purpleConversation), purple_conversation_get_name(purpleConversation), messageTextUnescaped, (PurpleMessageFlags)0);
		if (err < 0) {
			retVal = LibpurpleAdapter::SEND_FAILED;
			MojLogError(IMServiceApp::s_log, _T("sendMessage: serv_send_im returned err %d"), err);
		}

		free(messageTextUnescaped);
	}

	return retVal;
}

/*
 * webOS reactions (SEND): the user reacted (or removed a reaction) to a message from the TouchPad.
 * Resolve the owning account and emit "webos-im-send-reaction" so the owning prpl transmits it over
 * its backend. targetServiceMessageId is the prpl's own id for the reacted-to message (the same id we
 * stored as serviceMessageId when it arrived); remove=true removes my `emoji` reaction, else adds it
 * (emoji is always supplied so backends can remove a specific reaction); usernameTo is the peer/chat.
 */
LibpurpleAdapter::SendResult LibpurpleAdapter::sendReaction(const char* serviceName, const char* username, const char* usernameTo, const char* targetServiceMessageId, const char* emoji, bool remove, const char* targetSender)
{
	if (!serviceName || !username || !usernameTo || !targetServiceMessageId || *targetServiceMessageId == '\0')
	{
		MojLogError(IMServiceApp::s_log, _T("sendReaction: Invalid parameter."));
		return LibpurpleAdapter::INVALID_PARAMS;
	}

	std::string accountKey = getAccountKey(username, serviceName);
	std::string const usernameToBuf = getPurpleUsername(usernameTo, serviceName);

	PurpleAccount* account = NULL;
	if (s_onlineAccountData.count(accountKey))
		account = s_onlineAccountData[accountKey];

	if (account == NULL)
	{
		MojLogError(IMServiceApp::s_log, _T("sendReaction: account not logged in. service %s"), serviceName);
		return LibpurpleAdapter::USER_NOT_LOGGED_IN;
	}

	MojLogInfo(IMServiceApp::s_log, _T("sendReaction: %s emoji '%s' on message %s (%s)"),
			remove ? _T("remove") : _T("add"), emoji ? emoji : "", targetServiceMessageId, serviceName);

	// The app stores/sends the emoji as &#NNNNN; entities (raw astral emoji get mangled through db8);
	// decode to real UTF-8 in-process so the prpl backend gets a usable emoji (supplied for removes too).
	char *decodedEmoji = NULL;
	if (emoji && *emoji) {
		decodedEmoji = (char*) malloc(strlen(emoji) + 1);
		if (decodedEmoji) { decode_html_entities_utf8(decodedEmoji, emoji); }
	}

	// webOS reactions (SEND) db8 fallback: stash the reacted-to message's original sender on the account
	// right before the (synchronous) emit, so a backend that needs it - whatsmeow, to set FromMe /
	// Participant on BuildReaction - can read it back in its signal handler even when its in-memory
	// message cache has no entry for the target (transport restart/crash, or a message older than the
	// cache). Delivered out-of-band (not a new signal param) so the shared 5-arg signal - and the 5
	// other prpls connected to it - stay untouched. The handler clears it after reading.
	purple_account_set_string(account, "webos-reaction-target-sender", targetSender ? targetSender : "");

	purple_signal_emit(purple_conversations_get_handle(), "webos-im-send-reaction",
			account, targetServiceMessageId, decodedEmoji ? decodedEmoji : "", usernameToBuf.c_str(),
			remove ? "1" : "0");

	if (decodedEmoji) { free(decodedEmoji); }
	return LibpurpleAdapter::SENT;
}

/*
 * webOS attachment send. Mirrors sendMessage's account resolution + channel detection, but instead of
 * serv_send_im / serv_chat_send it hands the local file to libpurple's file-transfer path:
 *   - group channel target -> serv_chat_send_file(gc, chatId, path)  (gated by chat_can_receive_file)
 *   - 1:1 IM target        -> serv_send_file(gc, who, path)
 * Every prpl in this build implements send_file with the headless-friendly contract: a non-NULL
 * filename means the xfer is already accepted (purple_xfer_request_accepted), so no UI dialog is
 * needed. filePath must be an absolute path that EXISTS and is READABLE in the transport process
 * (e.g. /media/internal/...); a URL or a path only valid in the app sandbox will fail.
 */
LibpurpleAdapter::SendResult LibpurpleAdapter::sendFile(const char* serviceName, const char* username, const char* usernameTo, const char* filePath)
{
	if (!serviceName || !username || !usernameTo || !filePath || !filePath[0])
	{
		MojLogError(IMServiceApp::s_log, _T("sendFile: Invalid parameter. Please double check the passed parameters."));
		return LibpurpleAdapter::INVALID_PARAMS;
	}

	MojLogInfo(IMServiceApp::s_log, _T("%s called."), __FUNCTION__);

	// The prpl back-end reads the file from disk itself, so the path has to resolve in THIS process.
	if (!g_file_test(filePath, G_FILE_TEST_EXISTS) || !g_file_test(filePath, G_FILE_TEST_IS_REGULAR))
	{
		MojLogError(IMServiceApp::s_log, _T("sendFile: file does not exist or is not a regular file: %s"), filePath);
		return LibpurpleAdapter::SEND_FAILED;
	}

	// webOS voice messages: a recorded voice note arrives as a WAV (the native MediaCaptureV3 output;
	// the file picker is image-only, so a .wav here is always our recorder). Transcode it to Ogg/Opus
	// so the prpl sends it as a proper voice note (PTT) instead of a raw WAV document - Opus is the
	// voice-note format for WhatsApp/Telegram/Discord/FB-E2EE. The .ogg is written next to the WAV and
	// used for the rest of sendFile; `transcoded` must outlive the function so filePath stays valid.
	std::string transcoded;
	{
		size_t plen = strlen(filePath);
		if (plen > 4 && g_ascii_strcasecmp(filePath + plen - 4, ".wav") == 0)
		{
			transcoded.assign(filePath, plen - 4);
			transcoded += ".ogg";
			if (wav_to_opus_voicenote(filePath, transcoded.c_str(), 6.0f))
			{
				MojLogInfo(IMServiceApp::s_log, _T("sendFile: voice note transcoded %s -> %s"), filePath, transcoded.c_str());
				filePath = transcoded.c_str();
			}
			else
			{
				MojLogError(IMServiceApp::s_log, _T("sendFile: voice-note transcode failed for %s; sending as-is"), filePath);
			}
		}
	}

	std::string accountKey = getAccountKey(username, serviceName);

	// Same +E.164 -> device-JID translation as sendMessage (see there); WhatsApp attachments address
	// the buddy by "+<phone>". No-op for group/@lid ids and other services.
	std::string const usernameToBuf = getPurpleUsername(usernameTo, serviceName);
	usernameTo = usernameToBuf.c_str();

	PurpleAccount* accountToSendFrom = NULL;
	if (s_onlineAccountData.count(accountKey))
	{
		accountToSendFrom = s_onlineAccountData[accountKey];
	}
	else if (s_pendingAccountData.count(accountKey))
	{
		accountToSendFrom = s_pendingAccountData[accountKey];
	}

	if (accountToSendFrom == NULL)
	{
		MojLogError(IMServiceApp::s_log, _T("sendFile: Trying to send from an account that is not logged in. service name %s"), serviceName);
		return LibpurpleAdapter::USER_NOT_LOGGED_IN;
	}

	PurpleConnection* gc = purple_account_get_connection(accountToSendFrom);
	if (gc == NULL)
	{
		MojLogError(IMServiceApp::s_log, _T("sendFile: no active connection for service %s"), serviceName);
		return LibpurpleAdapter::USER_NOT_LOGGED_IN;
	}

	LibpurpleAdapter::SendResult retVal = LibpurpleAdapter::SENT;

	// Servers/Rooms: if the target resolves to a group channel (blist chat, matched by its id
	// component or by name), route the file into the CHAT via serv_chat_send_file. Join first if the
	// conversation isn't open yet (same pattern as sendMessage's channel branch).
	PurpleChat* channelChat = findChatByIdComponent(accountToSendFrom, usernameTo);
	if (channelChat == NULL)
		channelChat = purple_blist_find_chat(accountToSendFrom, usernameTo);
	if (channelChat != NULL)
	{
		PurpleConversation* chatConv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, usernameTo, accountToSendFrom);
		if (chatConv == NULL)
		{
			GHashTable* components = purple_chat_get_components(channelChat);
			if (components != NULL)
				serv_join_chat(gc, components);
			chatConv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, usernameTo, accountToSendFrom);
		}

		if (chatConv == NULL)
		{
			MojLogError(IMServiceApp::s_log, _T("sendFile: could not open chat conversation for channel %s"), usernameTo);
			return LibpurpleAdapter::SEND_FAILED;
		}

		int chatId = purple_conv_chat_get_id(purple_conversation_get_chat_data(chatConv));

		// Only attempt the chat file-send if the prpl advertises it (Discord/Telegram/Teams do;
		// a prpl without chat_send_file would otherwise no-op or crash).
		PurplePlugin* prpl = purple_connection_get_prpl(gc);
		PurplePluginProtocolInfo* prpl_info = prpl ? PURPLE_PLUGIN_PROTOCOL_INFO(prpl) : NULL;
		if (prpl_info == NULL || prpl_info->chat_send_file == NULL)
		{
			MojLogError(IMServiceApp::s_log, _T("sendFile: prpl for %s does not support chat file transfer"), serviceName);
			return LibpurpleAdapter::SEND_FAILED;
		}
		if (prpl_info->chat_can_receive_file != NULL && !prpl_info->chat_can_receive_file(gc, chatId))
		{
			MojLogError(IMServiceApp::s_log, _T("sendFile: chat %s cannot receive files"), usernameTo);
			return LibpurpleAdapter::SEND_FAILED;
		}

		serv_chat_send_file(gc, chatId, filePath);
		MojLogInfo(IMServiceApp::s_log, _T("sendFile: initiated chat file transfer to channel %s: %s"), usernameTo, filePath);
		return retVal;
	}

	// 1:1 IM file transfer. serv_send_file dispatches to prpl->send_file; with a non-NULL path our
	// prpls accept the xfer immediately without a UI prompt. Guard on send_file so a prpl without it
	// fails cleanly here instead of falling into libpurple's generic (UI-driven) xfer path.
	{
		PurplePlugin* prpl = purple_connection_get_prpl(gc);
		PurplePluginProtocolInfo* prpl_info = prpl ? PURPLE_PLUGIN_PROTOCOL_INFO(prpl) : NULL;
		if (prpl_info == NULL || prpl_info->send_file == NULL)
		{
			MojLogError(IMServiceApp::s_log, _T("sendFile: prpl for %s does not support file transfer"), serviceName);
			return LibpurpleAdapter::SEND_FAILED;
		}
	}
	serv_send_file(gc, usernameTo, filePath);
	MojLogInfo(IMServiceApp::s_log, _T("sendFile: initiated file transfer to %s: %s"), usernameTo, filePath);

	return retVal;
}


// Called by IMLoginState whenever a connection interface goes down.
// If all==true, then all interfaces went down.
bool LibpurpleAdapter::deviceConnectionClosed(bool all, const char* ipAddress)
{
	MojLogInfo(IMServiceApp::s_log, _T("%s called. ipAddress %s"), __FUNCTION__, ipAddress);

	if (!ipAddress && !all)
	{
		// not much we can do here
		MojLogError(IMServiceApp::s_log, _T("deviceConnectionClosed called with all=false and no ipAddress. Nothing to do"));
		return FALSE;
	}

	std::vector<std::string> accountToLogoutList;

	typedef std::unordered_map<std::string, std::string>::const_iterator iter;

	// s_ipAdressesBoundTo is abused as a joined store for online and pending
	// accounts
	for (iter i = s_ipAddressesBoundTo.begin(); i != s_ipAddressesBoundTo.end(); ++i)
	{
		std::string const& accountKey = i->first;

		std::string const& accountBoundToIpAddress = i->second;

		if (all == true || (accountBoundToIpAddress != "" && ipAddress == accountBoundToIpAddress))
		{
			bool accountWasLoggedIn = FALSE;

			PurpleAccount* account;

			if (s_onlineAccountData.count(accountKey) == 0)
			{
				if (s_pendingAccountData.count(accountKey) == 0)
				{
					MojLogInfo(IMServiceApp::s_log, _T("account was not found in the hash"));
					continue;
				}

				account = s_pendingAccountData[accountKey];
				// Note: in this case our login timer is still active, so that is the way we will let MojoDb know we are abandoning login and to reset watch.
				MojLogWarning(IMServiceApp::s_log, _T("deviceConnectionClosed: Abandoning pending login"));
			}
			else
			{
				account = s_onlineAccountData[accountKey];
				accountWasLoggedIn = TRUE;
				MojLogInfo(IMServiceApp::s_log, _T("Logging out"));
			}

			MojLogInfo(IMServiceApp::s_log, _T("deviceConnectionClosed: removing account from onlineAccountData hash table. accountKey %s"), accountKey.c_str());
			s_onlineAccountData.erase(accountKey);
			s_pendingAccountData.erase(accountKey);
			/*
			 * Keep the PurpleAccount struct to reuse in future logins
			 */
			s_offlineAccountData[accountKey] = account;

			purple_account_disconnect(account);

			accountToLogoutList.push_back(accountKey);

			// We can't remove this guy since we're iterating through its keys. We'll remove it after the break
			// g_hash_table_remove(ipAddressesBoundTo, accountKey);
		}
	}

	if (accountToLogoutList.empty())
	{
		MojLogInfo(IMServiceApp::s_log, _T("No accounts were connected on the requested ip address"));
	}
	else
	{
		for (std::vector<std::string>::iterator i = accountToLogoutList.begin();
				i != accountToLogoutList.end();
				++i)
		{
			s_ipAddressesBoundTo.erase(*i);
		}
	}

	return TRUE;
}

// Register + connect the webOS cross-prpl reaction signals ONCE. Called from initializeLibpurple
// (right after purple_core_init, BEFORE any prpl logs in) so a prpl that reconnects instantly from a
// saved session (e.g. whatsmeow) can connect to "webos-im-send-reaction" in its login handler without
// racing this registration. Previously this lived in assignIMLoginState, which runs per-account and
// fired ~40s AFTER the combined WhatsApp/Facebook plugin had already tried (and failed) to connect,
// silently dropping every WhatsApp/Facebook reaction. Idempotent via s_reactionSignalRegistered.
static void registerWebosReactionSignals()
{
	static bool s_reactionSignalRegistered = false;
	if (s_reactionSignalRegistered)
		return;
	s_reactionSignalRegistered = true;

	static int webosHandle = 0x1AD6;
	void* convHandle = purple_conversations_get_handle();

	// RECV per-sender merge (WhatsApp/Facebook/Signal): (account, targetServiceMessageId, emoji, sender).
	purple_signal_register(convHandle, "webos-im-reaction",
			purple_marshal_VOID__POINTER_POINTER_POINTER_POINTER, NULL, 4,
			purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_STRING));
	purple_signal_connect(convHandle, "webos-im-reaction", &webosHandle,
			PURPLE_CALLBACK(im_reaction_cb), NULL);

	// RECV aggregated REPLACE (Telegram): (account, targetServiceMessageId, serialized, NULL).
	purple_signal_register(convHandle, "webos-im-reaction-set",
			purple_marshal_VOID__POINTER_POINTER_POINTER_POINTER, NULL, 4,
			purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_STRING));
	purple_signal_connect(convHandle, "webos-im-reaction-set", &webosHandle,
			PURPLE_CALLBACK(im_reaction_set_cb), NULL);

	// react-to-own-sent: a prpl emits (account, serviceMessageId, text) once it learns an app-sent
	// message's network id; OutboxIdHandler attaches it to the Outbox row.
	purple_signal_register(convHandle, "webos-im-outbox-id",
			purple_marshal_VOID__POINTER_POINTER_POINTER, NULL, 3,
			purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_STRING));
	purple_signal_connect(convHandle, "webos-im-outbox-id", &webosHandle,
			PURPLE_CALLBACK(im_outbox_id_cb), NULL);

	// delivery/read receipts BY-ID (WhatsApp/Signal): (account, serviceMessageId, status).
	purple_signal_register(convHandle, "webos-im-receipt",
			purple_marshal_VOID__POINTER_POINTER_POINTER, NULL, 3,
			purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_STRING));
	purple_signal_connect(convHandle, "webos-im-receipt", &webosHandle,
			PURPLE_CALLBACK(im_receipt_cb), NULL);

	// delivery/read receipts WATERMARK (Telegram/Facebook/Teams): (account, scope, watermark, status).
	purple_signal_register(convHandle, "webos-im-receipt-hwm",
			purple_marshal_VOID__POINTER_POINTER_POINTER_POINTER, NULL, 4,
			purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_STRING));
	purple_signal_connect(convHandle, "webos-im-receipt-hwm", &webosHandle,
			PURPLE_CALLBACK(im_receipt_hwm_cb), NULL);

	// SEND: prpls CONNECT to this to transmit a reaction the user placed (account, targetServiceMessageId,
	// emoji, peer, removeFlag "1"=remove). Emitted by LibpurpleAdapter::sendReaction; owning prpl handles it.
	purple_signal_register(convHandle, "webos-im-send-reaction",
			purple_marshal_VOID__POINTER_POINTER_POINTER_POINTER_POINTER, NULL, 5,
			purple_value_new(PURPLE_TYPE_SUBTYPE, PURPLE_SUBTYPE_ACCOUNT),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_STRING),
			purple_value_new(PURPLE_TYPE_STRING));
	MojLogInfo(IMServiceApp::s_log, _T("registered webos-im-reaction + webos-im-reaction-set + webos-im-outbox-id + webos-im-send-reaction signals (early)"));
}

void LibpurpleAdapter::assignIMLoginState(LoginCallbackInterface* loginState)
{
	MojLogInfo(IMServiceApp::s_log, _T("%s called."), __FUNCTION__);

	static int handle = 0x1AD;
	s_loginState = loginState;

	if (s_registeredForAccountSignals == TRUE)
	{
		MojLogInfo(IMServiceApp::s_log, _T("Disconnecting old signals."));
		s_registeredForAccountSignals = FALSE;

		purple_signal_disconnect(purple_connections_get_handle(), "signed-on", &handle,
				PURPLE_CALLBACK(account_logged_in_cb));
		purple_signal_disconnect(purple_connections_get_handle(), "signed-off", &handle,
				PURPLE_CALLBACK(account_signed_off_cb));
		purple_signal_disconnect(purple_connections_get_handle(), "connection-error", &handle,
				PURPLE_CALLBACK(account_login_failed_cb));

		purple_signal_disconnect(purple_accounts_get_handle(), "account-status-changed", &handle,
				PURPLE_CALLBACK(account_status_changed));
		purple_signal_disconnect(purple_accounts_get_handle(), "account-authorization-denied", &handle,
				PURPLE_CALLBACK(account_auth_deny_cb));
		purple_signal_disconnect(purple_accounts_get_handle(), "account-authorization-granted", &handle,
				PURPLE_CALLBACK(account_auth_accept_cb));
	}

	if (loginState != NULL)
	{
		MojLogInfo(IMServiceApp::s_log, _T("Connecting new signals."));
		s_registeredForAccountSignals = TRUE;

		// webOS cross-prpl reaction signals are registered early in initializeLibpurple (before any prpl
		// logs in) via registerWebosReactionSignals(). This call is an idempotent fallback so the signals
		// still exist even on a path where initializeLibpurple's registration didn't run.
		registerWebosReactionSignals();

		/*
		 * Listen for a number of different signals:
		 */
		purple_signal_connect(purple_connections_get_handle(), "signed-on", &handle,
				PURPLE_CALLBACK(account_logged_in_cb), loginState);
		purple_signal_connect(purple_connections_get_handle(), "signed-off", &handle,
				PURPLE_CALLBACK(account_signed_off_cb), loginState);
		purple_signal_connect(purple_connections_get_handle(), "connection-error", &handle,
				PURPLE_CALLBACK(account_login_failed_cb), loginState);

		// accounts signals
		purple_signal_connect(purple_accounts_get_handle(), "account-status-changed", &handle,
				PURPLE_CALLBACK(account_status_changed), loginState);
		purple_signal_connect(purple_accounts_get_handle(), "account-authorization-denied", &handle,
				PURPLE_CALLBACK(account_auth_deny_cb), loginState);
		purple_signal_connect(purple_accounts_get_handle(), "account-authorization-granted", &handle,
			   PURPLE_CALLBACK(account_auth_accept_cb), loginState);
	}
}

void LibpurpleAdapter::assignIMServiceHandler(IMServiceCallbackInterface* imServiceHandler)
{
	MojLogInfo(IMServiceApp::s_log, _T("%s called."), __FUNCTION__);

	s_imServiceHandler = imServiceHandler;
}

void LibpurpleAdapter::assignAuthChannel(AuthChannel* authChannel)
{
	MojLogInfo(IMServiceApp::s_log, _T("%s called."), __FUNCTION__);
	s_authChannel = authChannel;
}

/* QR-preview token poll. prpl-discord persists the obtained token on the account
 * (purple_account_set_string "token") the instant remote-auth completes -- BEFORE it
 * opens the main gateway and starts syncing. We poll for that token rather than wait on
 * the "signed-on" signal (which did not fire reliably for the disposable preview account,
 * and which only fires AFTER a full gateway sync -> a flood of messages into a webOS
 * account that doesn't exist yet). On finding it: hand it to the UI as the confirmed
 * credential and tear the preview down (disconnect) so nothing syncs. */
struct QRPollCtx {
	std::string accountKey;
	std::string serviceName;
	std::string username;
	PurpleAccount* account;
	int elapsed;
};

static gboolean qrTokenPollCallback(gpointer data)
{
	QRPollCtx* ctx = (QRPollCtx*)data;

	// Preview cancelled/torn down elsewhere -> stop polling.
	if (s_qrPreviewKeys.count(ctx->accountKey) == 0)
	{
		delete ctx;
		return FALSE;
	}

	// prpl-discord persists the obtained credential as the "token" account string.
	const char* token = purple_account_get_string(ctx->account, "token", NULL);
	// Session prpls (gowhatsapp/whatsmeow) never set "token"; they store the reconnect
	// credential (deviceJID|registrationId) as BOTH the account password and the "credentials"
	// account string the instant pairing completes (gowhatsapp_store_credentials) -- which is
	// well BEFORE they signal PURPLE_CONNECTED. gowhatsapp only goes "online" (-> signed-on ->
	// account_logged_in_cb) after a full contact sync finishes, and on a fresh QR pair that
	// sync is huge and its end-marker often never arrives in time, so signed-on never fires and
	// the account is never confirmed -> connectTimeout, stuck on the QR page. Polling the stored
	// "credentials" instead confirms as soon as pairing is done, independent of that sync.
	// "credentials" == gowhatsapp's GOWHATSAPP_CREDENTIALS_KEY (its constants.h; inlined to avoid a
	// prpl header dependency in the transport). Non-session prpls never set it, so this is a no-op there.
	const char* sessionCred = purple_account_get_string(ctx->account, "credentials", NULL);
	bool haveToken = (token && *token);
	bool haveSession = (!haveToken && sessionCred && *sessionCred);
	if (haveToken || haveSession)
	{
		const char* cred = haveToken ? token : sessionCred;
		MojLogInfo(IMServiceApp::s_log, _T("qrTokenPoll: %s credential obtained for %s -> confirm"),
		           haveToken ? "token" : "session", ctx->accountKey.c_str());
		if (s_authChannel)
			s_authChannel->setConfirmed(ctx->serviceName.c_str(), ctx->username.c_str(), cred);

		s_qrPreviewKeys.erase(ctx->accountKey);
		s_pendingAccountData.erase(ctx->accountKey);
		if (s_accountLoginTimers.count(ctx->accountKey))
		{
			purple_timeout_remove(s_accountLoginTimers[ctx->accountKey]);
			s_accountLoginTimers.erase(ctx->accountKey);
		}

		if (haveToken)
		{
			// Discord (token-based): delete the disposable preview entirely -- disconnect it (so it
			// never syncs) AND remove it from accounts.xml. Keeping it persisted leaves an UNTAGGED
			// orphan (no webosAccountId) that auto-logs-in on every transport restart and floods, and
			// that onDelete (matched on webosAccountId) can NEVER clean up. The token was handed to the
			// UI above; the real account is created by the UI and logs in directly with it (no 2nd QR).
			s_onlineAccountData.erase(ctx->accountKey);
			schedulePreviewAccountDelete(ctx->account);
		}
		else
		{
			// Session prpl (whatsmeow): do NOT tear down. Disconnecting a freshly-paired whatsmeow
			// client SIGSEGVs, and the live paired session is exactly what we want to reuse. Keep it
			// alive and register it as an online account with no webOS accountId yet; when the UI
			// creates the real account, its onEnabled -> LibpurpleAdapter::login() finds THIS session
			// already online and adopts it (stamps the webosAccountId, reuses the connection -- see the
			// adoption block in login()). Mirrors account_logged_in_cb's adoption path but fires on
			// credential-stored rather than the unreliable signed-on signal. The preview already holds
			// the "credentials" string in accounts.xml, so post-restart auto-login reconnects it too.
			s_onlineAccountData[ctx->accountKey] = ctx->account;
			if (s_ipAddressesBoundTo.count(ctx->accountKey) == 0)
				s_ipAddressesBoundTo[ctx->accountKey] = "";
			MojLogInfo(IMServiceApp::s_log, _T("qrTokenPoll: keeping paired preview %s alive for adoption"), ctx->accountKey.c_str());
		}

		delete ctx;
		return FALSE;
	}

	ctx->elapsed += 1;
	if (ctx->elapsed > (int)QR_CONNECT_TIMEOUT_SECONDS)
	{
		// overall timeout; connectTimeoutCallback (if still armed) reports expiry.
		delete ctx;
		return FALSE;
	}
	return TRUE;   // keep polling (~1s)
}

/*
 * Start a disposable "QR-preview" login (create-after-confirm). This spins up a prpl
 * account with the QRLOGIN sentinel purely so prpl-discord runs its remote-auth flow and
 * emits the QR (surfaced via the request_fields ui-op -> AuthChannel). On sign-on the prpl
 * has persisted the obtained Discord token on the account; account_logged_in_cb detects the
 * QR-preview key, hands the token to the AuthChannel as the confirmed credential, and tears
 * the preview down. The real account is then created by the UI with that token and logs in
 * directly (no second QR). This does NOT touch the webOS login-state machine.
 */
LibpurpleAdapter::LoginResult LibpurpleAdapter::startQRLogin(const char* serviceName, const char* username)
{
	if (!serviceName || !*serviceName || !username || !*username)
	{
		MojLogError(IMServiceApp::s_log, _T("startQRLogin: empty serviceName/username"));
		return FAILED;
	}

	std::string const accountKey = getAccountKey(username, serviceName);

	std::string prplProtocolId = getPrplProtocolIdFromServiceName(serviceName);

	/* WhatsApp's webOS username is +E.164 (for display); whatsmeow requires the JID as the purple
	 * account username. Translate here so find/create hit the same account the prpl will pair. */
	std::string const purpleUsername = getPurpleUsername(username, serviceName);

	/* A previously-saved Discord account auto-logs-in on transport start with its stored
	 * "token", so by the time the user opens Add-Account it is already CONNECTED (and
	 * quietly flooding messages). prpl-discord's discord_login does a DIRECT login whenever
	 * the "token" string is non-empty -- and re-enabling an already-connected account never
	 * re-runs discord_login at all -- so merely clearing the token + enabling emits no QR.
	 * The only robust way to force remote-auth is to tear any such account down completely
	 * and start from a brand-new, tokenless account. purple_accounts_delete disconnects it
	 * (if connected) and removes it from accounts.xml, which also kills the auto-login flood
	 * source. The real account is (re)created by the UI after confirm with the fresh token. */
	PurpleAccount* account = purple_accounts_find(purpleUsername.c_str(), prplProtocolId.c_str());
	if (account)
	{
		if (purple_account_is_connected(account) || purple_account_is_connecting(account))
			purple_account_disconnect(account);
		purple_account_set_enabled(account, UI_ID, FALSE);
		purple_accounts_delete(account);
		account = NULL;
	}
	// Drop any stale in-memory session tracking so the fresh login is not mistaken for active.
	s_onlineAccountData.erase(accountKey);
	s_pendingAccountData.erase(accountKey);
	s_offlineAccountData.erase(accountKey);

	MojObject emptyConfig;
	/* Same guard as login(): an unresolved prpl throws here - catch it so a QR-add of a broken/
	 * missing plugin fails just this attempt instead of crash-looping the whole transport. */
	try
	{
		account = Util::createPurpleAccount(purpleUsername.c_str(), prplProtocolId.c_str(), emptyConfig);
	}
	catch (const Util::MojoException& e)
	{
		MojLogError(IMServiceApp::s_log, _T("startQRLogin: createPurpleAccount threw for prpl '%s': %s - failing this login only"),
			prplProtocolId.c_str(), e.what().c_str());
		return FAILED;
	}
	if (!account)
	{
		MojLogError(IMServiceApp::s_log, _T("startQRLogin: failed to create Purple account"));
		return FAILED;
	}
	purple_accounts_add(account);

	AccountMetaData* amd = new AccountMetaData;
	amd->account_key = accountKey;
	amd->servicename = serviceName;
	account->ui_data = (void*)amd;

	/* Fresh account: no "token" string + the QRLOGIN sentinel password -> discord_login
	 * takes the remote-auth (QR) path (see discord_login: token empty AND password=="QRLOGIN"). */
	purple_account_set_password(account, "QRLOGIN");

	s_pendingAccountData[accountKey] = account;
	s_qrPreviewKeys.insert(accountKey);

	purple_account_set_enabled(account, UI_ID, TRUE);

	// Poll for the token the moment remote-auth completes (robust; independent of the
	// signed-on signal) and tear the preview down before it syncs. See qrTokenPollCallback.
	QRPollCtx* pollCtx = new QRPollCtx;
	pollCtx->accountKey  = accountKey;
	pollCtx->serviceName = serviceName;
	pollCtx->username    = username;
	pollCtx->account     = account;
	pollCtx->elapsed     = 0;
	purple_timeout_add_seconds(1, qrTokenPollCallback, pollCtx);

	// QR grace-period timeout; connectTimeoutCallback special-cases QR-preview keys.
	guint timerHandle = purple_timeout_add_seconds(QR_CONNECT_TIMEOUT_SECONDS, connectTimeoutCallback, new std::string(accountKey));
	s_accountLoginTimers[accountKey] = timerHandle;

	PurpleSavedStatus* savedStatus = purple_savedstatus_new(NULL, PURPLE_STATUS_AVAILABLE);
	purple_savedstatus_activate_for_account(savedStatus, account);

	MojLogInfo(IMServiceApp::s_log, _T("startQRLogin: pending QR login started for %s"), accountKey.c_str());
	return OK;
}

void LibpurpleAdapter::cancelQRLogin(const char* serviceName, const char* username)
{
	if (!serviceName || !username)
		return;
	std::string const accountKey = getAccountKey(username, serviceName);
	MojLogInfo(IMServiceApp::s_log, _T("cancelQRLogin: %s"), accountKey.c_str());

	s_qrPreviewKeys.erase(accountKey);

	// Discard any pending captcha request for this account (destroy the held fields).
	if (s_pendingCaptcha.count(accountKey))
	{
		if (s_pendingCaptcha[accountKey].fields)
			purple_request_fields_destroy(s_pendingCaptcha[accountKey].fields);
		s_pendingCaptcha.erase(accountKey);
	}

	if (s_accountLoginTimers.count(accountKey))
	{
		purple_timeout_remove(s_accountLoginTimers[accountKey]);
		s_accountLoginTimers.erase(accountKey);
	}

	PurpleAccount* account = NULL;
	if (s_pendingAccountData.count(accountKey))
		account = s_pendingAccountData[accountKey];
	else if (s_onlineAccountData.count(accountKey))
		account = s_onlineAccountData[accountKey];

	s_pendingAccountData.erase(accountKey);
	s_onlineAccountData.erase(accountKey);

	if (account)
		purple_account_set_enabled(account, UI_ID, FALSE);

	if (s_authChannel)
		s_authChannel->clearChallenge(serviceName, username);
}

/*
 * Feed a UI-solved captcha response token back into the prpl's pending request_fields
 * callback. adapter_request_fields stored the PurpleRequestFields + ok callback keyed by
 * account when Discord raised the hCaptcha; here we set the editable "captcha_key" field
 * to the solved token and invoke that callback, which re-POSTs remote-auth/login. The
 * held fields are then destroyed (we own them; the prpl uses no close_request ui-op).
 */
bool LibpurpleAdapter::submitCaptcha(const char* serviceName, const char* username, const char* captchaKey)
{
	if (!serviceName || !username)
		return false;
	std::string const accountKey = getAccountKey(username, serviceName);

	std::unordered_map<std::string, PendingCaptcha>::iterator it = s_pendingCaptcha.find(accountKey);
	if (it == s_pendingCaptcha.end())
	{
		MojLogError(IMServiceApp::s_log, _T("submitCaptcha: no pending captcha request for %s"), accountKey.c_str());
		return false;
	}

	PendingCaptcha pc = it->second;
	s_pendingCaptcha.erase(it);   // erase before invoking (callback may raise a new captcha)

	MojLogInfo(IMServiceApp::s_log, _T("submitCaptcha: completing captcha for %s (key %s)"),
	           accountKey.c_str(), (captchaKey && *captchaKey) ? "present" : "EMPTY");

	if (pc.fields)
	{
		PurpleRequestField* keyField = purple_request_fields_get_field(pc.fields, "captcha_key");
		if (keyField)
			purple_request_field_string_set_value(keyField, captchaKey ? captchaKey : "");

		if (pc.okCb)
			pc.okCb(pc.userData, pc.fields);

		purple_request_fields_destroy(pc.fields);
	}
	return true;
}

/*
 * Value destroy function for s_AuthorizeRequests
 */
static void deleteAuthRequest(void* obj)
{
	AuthRequest *aa = (AuthRequest *)obj;
	MojLogInfo(IMServiceApp::s_log, _T("deleteAuthRequest: deleting auth request object. account: %s, remote_user: %s"), aa->account->username, aa->remote_user);
 	g_free(aa->remote_user);
	g_free(aa->alias);
	g_free(aa);
}

/*
 * Check if all the accounts are offline so we can shut down
 */
bool LibpurpleAdapter::allAccountsOffline()
{
	if (s_onlineAccountData.empty())
	{
		if (s_pendingAccountData.empty()) {
			return true;
		}
		else {
			MojLogInfo(IMServiceApp::s_log, _T("allAccountsOffline - %d accounts still pending"), s_pendingAccountData.size());
		}
	}
	else {
		MojLogInfo(IMServiceApp::s_log, _T("allAccountsOffline - %d accounts still online"), s_onlineAccountData.size());
	}

	return false;
}

void LibpurpleAdapter::init()
{
	initializeLibpurple();
}
