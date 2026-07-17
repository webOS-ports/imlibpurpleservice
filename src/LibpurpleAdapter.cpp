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
static std::string const& getServiceNameFromPurpleAccount(PurpleAccount* account);
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
	// webOS Teams port: the personal (Teams-for-Life) libpurple plugin registers
	// as "prpl-teams-personal", which the generic "prpl-" + <type> transform below
	// cannot derive from the "type_teams" service name. Map it explicitly so
	// purple_account_new() finds the loaded prpl. Keep the service name "type_teams"
	// (baked into db8 kinds / capability ids) decoupled from the plugin id.
	if (serviceName == "type_teams")
	{
		return "prpl-teams-personal";
	}
	// hoehermann/purple-signal registers as "prpl-hehoe-signal" (the "hehoe" infix
	// cannot be derived from the "type_signal" service name), so map it explicitly.
	// NOTE: the Signal prpl is scaffolding only and not currently runnable on this
	// device (needs an embedded JVM + an ARMv7 Rust libsignal); see
	// messaging/signal/BUILD-LOG.md. The mapping is inert unless a type_signal
	// account exists and the plugin is actually loaded.
	if (serviceName == "type_signal")
	{
		return "prpl-hehoe-signal";
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
	return username + "_" + serviceName;
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

static std::string const& getServiceNameFromPurpleAccount(PurpleAccount* account)
{
    static const std::string empty = "";
	if (!account || !account->ui_data)
	{
		MojLogError(IMServiceApp::s_log, _T("getAccountKeyFromPurpleAccount called with empty account"));
		return empty;
	}

	return ((AccountMetaData*)account->ui_data)->servicename;
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
	s_imServiceHandler->updateBuddyStatus(accountId.c_str(), serviceName.c_str(), buddy->name, newAvailabilityValue, customMessage, groupName, buddyAvatarLocation);

	g_message(
			"%s says: %s's presence: availability: '%i', custom message: '%s', avatar location: '%s', display name: '%s', group name: '%s'",
			__FUNCTION__, buddy->name, newAvailabilityValue, customMessage, buddyAvatarLocation, buddy->alias, groupName);

	if (buddyAvatarLocation)
	{
		g_free(buddyAvatarLocation);
	}
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

	// call into the imlibpurpletransport
	// buddy->name is stored in the imbuddyStatus DB kind in the libpurple format - ie. for AIM without the "@aol.com" so that is how we need to search for it
	s_imServiceHandler->updateBuddyStatus(accountId.c_str(), serviceName.c_str(), buddy->name, newAvailabilityValue, customMessage, groupName, buddyAvatarLocation);

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

struct BuddyResyncCtx
{
	std::string serviceName;
	std::string username;
	std::string accountKey;
	guint timerId;
};
static std::unordered_map<std::string, BuddyResyncCtx*> s_buddyResyncCtx;

static gboolean buddyResyncTimeoutCallback(gpointer data)
{
	BuddyResyncCtx* ctx = (BuddyResyncCtx*)data;
	s_buddyResyncCtx.erase(ctx->accountKey);
	if (s_loginState != NULL)
	{
		MojLogInfo(IMServiceApp::s_log, _T("buddyResyncTimeoutCallback: requesting buddy re-sync for %s"), ctx->accountKey.c_str());
		s_loginState->buddyListChanged(ctx->serviceName.c_str(), ctx->username.c_str());
	}
	delete ctx;
	return FALSE; // one-shot
}

static void buddy_added_cb(PurpleBuddy* buddy)
{
	MojLogInfo(IMServiceApp::s_log, _T("buddy added %s"), buddy->name);

	PurpleAccount* account = purple_buddy_get_account(buddy);
	if (account == NULL)
		return;

	// Only re-sync for a live, logged-in account. Buddies added while the account is still
	// connecting (or loaded from blist at startup) are covered by the normal login-time snapshot.
	if (!purple_account_is_connected(account))
		return;

	std::string const& serviceName = getServiceNameFromPurpleAccount(account);
	std::string const& accountKey = getAccountKeyFromPurpleAccount(account);
	const char* username = account->username;
	if (serviceName.empty() || username == NULL || *username == '\0')
		return;

	// Reset any pending debounce timer for this account so the re-sync fires once, ~8s after the
	// LAST buddy in the burst is added (covers both the post-login load and later single additions).
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
		std::string svc;
		if (strncmp(prpl, "prpl-", 5) == 0)
			svc = std::string("type_") + (prpl + 5);
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
		s_onlineAccountData.erase(accountKey);
		// Delete the disposable preview account entirely (see qrTokenPollCallback): keeping it
		// persisted would leave an untagged orphan (no webosAccountId) that auto-logs-in +
		// floods and that onDelete can never remove. The token is already handed to the UI;
		// the real account is created by the UI and logs in directly with that token.
		if (purple_account_is_connected(loggedInAccount) || purple_account_is_connecting(loggedInAccount))
			purple_account_disconnect(loggedInAccount);
		purple_account_set_enabled(loggedInAccount, UI_ID, FALSE);
		purple_accounts_delete(loggedInAccount);
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
		((LoginCallbackInterface*)loginState)->loginResult(serviceName.c_str(), loggedInAccount->username, LoginCallbackInterface::LOGIN_SUCCESS, false, ERROR_NO_ERROR, true);
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
		((LoginCallbackInterface*)loginState)->loginResult(serviceName.c_str(), account->username, LoginCallbackInterface::LOGIN_SIGNED_OFF, false, ERROR_NO_ERROR, true);
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
		MojLogInfo(IMServiceApp::s_log, _T("account_login_failed_cb: removing account from onlineAccountData hash table. accountKey %s"), accountKey);
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

	// Special handling for broken network connection errors (due to bad coverage or flight mode)
	// We need to set noRetry to false if there was a network type error regardless of if we were pending or online.
	if (type == PURPLE_CONNECTION_ERROR_NETWORK_ERROR)
	{
		MojLogError(IMServiceApp::s_log, _T("We had a network error. Reason: %s, prpl error code: %i. Need to retry"), description, type);
		noRetry = false;
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
		((LoginCallbackInterface*)loginState)->loginResult(serviceName.c_str(), account->username, LoginCallbackInterface::LOGIN_FAILED, loggedOut, mojoFriendlyErrorCode, noRetry);
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
		/* this is a sent message. ignore it. */
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
	std::string serverNameStr;   // guild / network - the room's blist group
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
			if (chat != NULL)
			{
				// The chat's parent blist node is its group; for purple-discord that group
				// is the guild (server). Use direct field access (public struct member) so we
				// don't depend on any particular libpurple accessor version.
				PurpleBlistNode* parent = ((PurpleBlistNode*)chat)->parent;
				if (parent != NULL && PURPLE_BLIST_NODE_IS_GROUP(parent))
				{
					const char* groupName = purple_group_get_name((PurpleGroup*)parent);
					if (groupName != NULL)
						serverNameStr = groupName;
				}
				// webOS: an archived chat is silenced like a muted one (no notification banner). The
				// prpl (tdlib-purple) sets both bools on the chat blist node.
				muted = purple_blist_node_get_bool((PurpleBlistNode*)chat, "muted")
				        || purple_blist_node_get_bool((PurpleBlistNode*)chat, "archived");
			}
		}
		// Flat-hierarchy protocols (Telegram: groups/supergroups/channels have no parent "server" -
		// they land under tdlib-purple's generic "Chats" blist group, or none at all when the chat
		// isn't in the blist, giving an inconsistent/meaningless server). Route them under one
		// stable synthetic server = the network name, so every one of the account's Telegram rooms
		// groups under a single "Telegram" server in the Servers tab. Discord/IRC keep their real guild.
		const char* protoId = purple_account_get_protocol_id(account);
		if (protoId != NULL && strstr(protoId, "telegram") != NULL)
		{
			const char* net = purple_account_get_protocol_name(account);
			serverNameStr = (net != NULL && *net != '\0') ? net : "Telegram";
		}
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

	// call the transport service incoming message handler
	// webOS Teams port: forward the libpurple message time (mtime, secs) so history/
	// offline messages are stored with their original send time, not the arrival time.
	// webOS Servers/Rooms: forward channel + server (both NULL for 1:1 IMs). serverId has no
	// stable value from the blist group alone, so mirror serverName for now - Milestone 1 will
	// pull the real guild id from the chat's components.
	const char* serverName = serverNameStr.empty() ? NULL : serverNameStr.c_str();
	s_imServiceHandler->incomingIM(serviceName.c_str(), account->username, usernameFromStripped.c_str(),
			message, mtime, channelName, channelDisplayName, serverName, serverName, muted, usernameFromDisplay);
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
	s_imServiceHandler->receivedBuddyInvite(serviceName.c_str(), account->username, usernameFromStripped.c_str(), message);

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
	bool noRetry = true;
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
		return FALSE;
	}

	if (s_loginState)
	{
		std::string const& serviceName = getServiceNameFromPurpleAccount(account);

		// TODO - should noRetry be false here in other cases?
		// Can't really tell - we will get here if the proper sa security certificate is not installed, which is a permanent failure.
		// libpurple just does not reliably call the login failed callback in all cases...this is not the same as a connection timeout.
		s_loginState->loginResult(serviceName.c_str(), account->username, LoginCallbackInterface::LOGIN_TIMEOUT, false, ERROR_NETWORK_ERROR, noRetry);
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
	signal(SIGCHLD, SIG_IGN);

	/* Set a custom user directory (optional) */
	purple_util_set_user_dir(CUSTOM_USER_DIRECTORY);

	/* We do not want any debugging for now to keep the noise to a minimum. */
	purple_debug_set_enabled(TRUE);

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
				const char* prpl = account->protocol_id ? account->protocol_id : "";
				if (strncmp(prpl, "prpl-", 5) == 0)
					outServiceName->assign(std::string("type_") + (prpl + 5));
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
		std::string const& accountBoundToIpAddress = s_ipAddressesBoundTo[accountKey];
		if (params.localIpAddress.data() == accountBoundToIpAddress)
		{
			/*
			 * We're using the right interface for this account
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
					_T("LibpurpleAdapter::login: We have to logout and login again since the local IP address has changed. Logging out from account."));
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

			/* webOS Teams port: persist the PurpleAccount in accounts.xml so the rotated
			 * OAuth refresh_token (the prpl stores it as the account password) and the
			 * buddy list survive transport restarts natively — no out-of-band token files.
			 * Reuse the persisted account across restarts; only create+add a fresh one. */
			account = purple_accounts_find(params.username.data(), prplProtocolId.c_str());
			if (!account)
			{
				account = Util::createPurpleAccount(params.username.data(), prplProtocolId.c_str(), params.config);
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
         * easily exceeds the normal 45s. Facebook is likewise interactive when the
         * account has two-factor enabled: the prpl raises a login-code challenge and
         * the user types the code into the "facebook" auth chat. Give all three the
         * longer grace period; otherwise the 45s connect timeout fires mid-challenge
         * and drives a retry loop that re-issues auth.login (a fresh machine_id each
         * time) before the code can be entered. Other protocols keep the normal
         * timeout. */
        const char* protoId = purple_account_get_protocol_id(account);
        bool interactiveAuth = (protoId != NULL &&
                                (strcmp(protoId, "prpl-discord") == 0 ||
                                 strcmp(protoId, "prpl-telegram") == 0 ||
                                 strcmp(protoId, "prpl-facebook") == 0));
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

			// webOS Telegram port: skip deleted/nameless users. tdlib gives them no name, so the
			// contact would otherwise show a raw "id<number>". Not reporting them here also makes the
			// BuddyListConsolidator delete any such contacts left from a previous (pre-filter) sync.
			if (isBlankName(resolvedAlias))
			{
				MojLogInfo(IMServiceApp::s_log, _T("getFullBuddyList: skipping nameless buddy %s (deleted user?)"), buddyToBeAdded->name);
				continue;
			}

			buddyObj.putString("username", buddyToBeAdded->name);
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

			if (resolvedAlias != NULL)
			{
				// webOS Telegram port: strip only astral emoji/flags (unrenderable on this WebKit), keep
				// all BMP text (Thai/Cyrillic/CJK/Latin) which renders via the fallback-font slots. If the
				// name was entirely astral it strips to empty - keep the original then, so we never fall
				// back to a raw "id<number>".
				std::string cleanName = stripAstral(resolvedAlias);
				buddyObj.putString("displayName", cleanName.empty() ? resolvedAlias : cleanName.c_str());
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

LibpurpleAdapter::SendResult LibpurpleAdapter::sendMessage(const char* serviceName, const char* username, const char* usernameTo, const char* messageText)
{
	if (!serviceName || !username || !usernameTo || !messageText)
	{
		MojLogError(IMServiceApp::s_log, _T("sendMessage: Invalid parameter. Please double check the passed parameters."));
		return LibpurpleAdapter::INVALID_PARAMS;
	}

	LibpurpleAdapter::SendResult retVal = LibpurpleAdapter::SENT;
	MojLogInfo(IMServiceApp::s_log, _T("%s called."), __FUNCTION__);

	std::string accountKey = getAccountKey(username, serviceName);

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
		PurpleConversation* purpleConversation = purple_conversation_new(PURPLE_CONV_TYPE_IM, accountToSendFrom, usernameTo);
		char* messageTextUnescaped = g_strcompress(messageText);

		// replace this with the lower level call so we can try to get an error code back...
		// calls common_send which calls serv_send_im (conversation.c)
//		purple_conv_im_send(purple_conversation_get_im_data(purpleConversation), messageTextUnescaped);
//				common_send(PurpleConversation *conv, const char *message, PurpleMessageFlags msgflags)
//					gc = purple_conversation_get_gc(conv);
//					err = serv_send_im(gc, purple_conversation_get_name(conv), sent, msgflags);
		// we still don't seem to get an error value back there...returns 1, even for an invalid recipient
		int err = serv_send_im(purple_conversation_get_gc(purpleConversation), purple_conversation_get_name(purpleConversation), messageTextUnescaped, (PurpleMessageFlags)0);
		if (err < 0) {
			retVal = LibpurpleAdapter::SEND_FAILED;
			MojLogError(IMServiceApp::s_log, _T("sendMessage: serv_send_im returned err %d"), err);
		}

		free(messageTextUnescaped);
	}

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

	const char* token = purple_account_get_string(ctx->account, "token", NULL);
	if (token && *token)
	{
		MojLogInfo(IMServiceApp::s_log, _T("qrTokenPoll: token obtained for %s -> confirm + tear down preview"), ctx->accountKey.c_str());
		if (s_authChannel)
			s_authChannel->setConfirmed(ctx->serviceName.c_str(), ctx->username.c_str(), token);

		s_qrPreviewKeys.erase(ctx->accountKey);
		s_pendingAccountData.erase(ctx->accountKey);
		s_onlineAccountData.erase(ctx->accountKey);
		if (s_accountLoginTimers.count(ctx->accountKey))
		{
			purple_timeout_remove(s_accountLoginTimers[ctx->accountKey]);
			s_accountLoginTimers.erase(ctx->accountKey);
		}
		// Delete the disposable preview account entirely -- disconnect it (so it never syncs)
		// AND remove it from accounts.xml. Keeping it persisted leaves an UNTAGGED orphan (no
		// webosAccountId), which auto-logs-in on every transport restart and floods, and which
		// onDelete (deleteAccountByWebosId, matched on webosAccountId) can NEVER clean up --
		// exactly why deleting the account from the UI did not clear it. The token was already
		// handed to the UI above; the real account is created by the UI and logs in directly
		// with that token (passed as its credential -> discord_login uses it, no second QR).
		if (purple_account_is_connected(ctx->account) || purple_account_is_connecting(ctx->account))
			purple_account_disconnect(ctx->account);
		purple_account_set_enabled(ctx->account, UI_ID, FALSE);
		purple_accounts_delete(ctx->account);

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

	/* A previously-saved Discord account auto-logs-in on transport start with its stored
	 * "token", so by the time the user opens Add-Account it is already CONNECTED (and
	 * quietly flooding messages). prpl-discord's discord_login does a DIRECT login whenever
	 * the "token" string is non-empty -- and re-enabling an already-connected account never
	 * re-runs discord_login at all -- so merely clearing the token + enabling emits no QR.
	 * The only robust way to force remote-auth is to tear any such account down completely
	 * and start from a brand-new, tokenless account. purple_accounts_delete disconnects it
	 * (if connected) and removes it from accounts.xml, which also kills the auto-login flood
	 * source. The real account is (re)created by the UI after confirm with the fresh token. */
	PurpleAccount* account = purple_accounts_find(username, prplProtocolId.c_str());
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
	account = Util::createPurpleAccount(username, prplProtocolId.c_str(), emptyConfig);
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
