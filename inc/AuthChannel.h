/*
 * AuthChannel.h
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport).
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free Software
 * Foundation.
 *
 * AuthChannel surfaces interactive login challenges that a libpurple prpl
 * raises during connect - today Discord's "scan this QR code" remote-auth
 * flow - to a webOS UI (the shared accounts auth app) instead of dumping
 * them into a chat conversation.
 *
 * Design (see also the startQRLogin / getAuthChallenge / submitAuthInput luna
 * methods on IMServiceHandler):
 *
 *   Discord validator (accounts customUI)        prpl-discord (request_fields)
 *        |  startQRLogin(serviceName,username)          |
 *        v                                              v
 *   IMServiceHandler --> LibpurpleAdapter: PENDING login (reuses the Telegram
 *        |                pending-account infra) so the prpl runs remote-auth
 *        |                and raises the QR via the request_fields ui-op
 *        v                                              |
 *   AuthChannel <-- publishQRChallenge(image) ---------+
 *        |  getAuthChallenge (subscribe): push {state,qrImage,urlString,token}
 *        v
 *   validator renders the QR <img> inline, polls, and on state==confirmed
 *   calls sendResult({credentials.password = token}) -> the account is created
 *   ONLY THEN, already holding the real Discord token (no QR on later logins).
 *
 * "Create-after-confirm": the account does not exist while the QR is shown; the
 * pending prpl login exists only to obtain the token. submitAuthInput(cancel)
 * tears the pending login down; (refresh) restarts it for a fresh QR.
 *
 * The channel is keyed by the transport's account key (serviceName + "_" +
 * username, the same key LibpurpleAdapter already uses for its account maps).
 * A challenge carries a state the UI polls via a persistent subscription:
 *
 *   waiting   - challenge published, awaiting the user / second device
 *   scanned   - (Discord) phone scanned the QR, awaiting approval [optional]
 *   confirmed - remote-auth succeeded; snapshot carries the token for the UI
 *   expired   - challenge timed out / connection dropped; UI may refresh
 *   failed    - login failed with an error message
 *
 * Only the request_fields ui-op is installed for now, so this handles the
 * Discord QR path. Telegram's code/2FA capture keeps its existing
 * sendMessage routing (LibpurpleAdapter) untouched; it can migrate onto this
 * channel later by also installing request_input.
 */

#ifndef AUTHCHANNEL_H_
#define AUTHCHANNEL_H_

#include <map>
#include <list>
#include <string>
#include "core/MojService.h"
#include "core/MojServiceMessage.h"
#include "core/MojSignal.h"
#include "core/MojObject.h"
#include "core/MojRefCount.h"

class AuthChannel
{
public:
	enum ChallengeKind {
		ChallengeNone = 0,
		ChallengeQRCode,   // Discord remote-auth: image + url string
		ChallengeCaptcha,  // Discord remote-auth: hCaptcha (sitekey + rqdata + rqtoken)
		ChallengeInput     // single-value prompt (reserved for Telegram migration)
	};

	enum ChallengeState {
		StateWaiting = 0,
		StateScanned,
		StateConfirmed,
		StateExpired,
		StateFailed
	};

	AuthChannel(MojService* service);
	~AuthChannel();

	// ---- called from LibpurpleAdapter (glib main loop / prpl callbacks) ----

	// Publish (or replace) the QR challenge for an account. imageData is the
	// raw TGA/PNG bytes the prpl produced; AuthChannel base64-encodes it into
	// a data URI for the UI. urlString is the raw QR payload (fallback text).
	void publishQRChallenge(const char* serviceName, const char* username,
	                        const unsigned char* imageData, size_t imageLen,
	                        const char* imageMimeType, const char* urlString);

	// Publish (or replace) an hCaptcha challenge for an account. The UI renders the
	// hCaptcha widget (in an embedded browser) from sitekey + rqdata, then feeds the
	// solved response token back via submitAuthInput(action="captcha", value=key).
	void publishCaptchaChallenge(const char* serviceName, const char* username,
	                             const char* service, const char* sitekey,
	                             const char* rqdata, const char* rqtoken);

	// Advance the state of an existing challenge and push it to subscribers.
	// message is optional (used for StateFailed).
	void setChallengeState(const char* serviceName, const char* username,
	                       ChallengeState state, const char* message = NULL);

	// Terminal success: record the credential the pending remote-auth produced
	// (Discord token) and push StateConfirmed so the validator can create the
	// account with it. The token rides the snapshot to the subscribed UI only.
	void setConfirmed(const char* serviceName, const char* username, const char* token);

	// Drop a challenge (account gone / login fully done). Notifies subscribers
	// with a final reply if a terminal state was not already sent.
	void clearChallenge(const char* serviceName, const char* username);

	// ---- called from IMServiceHandler (luna method dispatch) ----

	// getAuthChallenge: register a persistent subscription for an account key
	// and immediately reply with the current challenge snapshot (or an empty
	// "none" snapshot if there is no active challenge yet).
	MojErr subscribe(MojServiceMessage* msg, const char* serviceName, const char* username);

	// submitAuthInput: the UI hands back the user's answer (e.g. the user
	// dismissed/refreshed the QR). Returns the stored purple callback context
	// so LibpurpleAdapter can invoke it. value may be NULL for a plain refresh.
	// (Full input plumbing lands with the Telegram migration; QR only needs
	// refresh/cancel today.)
	MojErr submitInput(MojServiceMessage* msg, const char* serviceName,
	                   const char* username, const char* action, const char* value);

private:
	struct Challenge {
		ChallengeKind  kind;
		ChallengeState state;
		std::string    imageDataUri;   // "data:image/tga;base64,...."
		std::string    urlString;
		std::string    message;
		std::string    token;          // credential from remote-auth (confirmed only)
		// Captcha (ChallengeCaptcha) parameters for the UI's hCaptcha widget.
		std::string    captchaService; // "hcaptcha"
		std::string    captchaSitekey;
		std::string    captchaRqData;
		std::string    captchaRqToken;
	};

	// One held-open subscription = one ref-counted MojSignalHandler carrying the
	// service message + a CancelSignal slot (mirrors db8 MojDbServiceHandler::Watcher).
	// On cancel it deregisters itself from the owning AuthChannel.
	class Subscription : public MojSignalHandler {
	public:
		Subscription(AuthChannel* owner, MojServiceMessage* msg, const std::string& key);
		MojErr handleCancel(MojServiceMessage* msg);

		AuthChannel*                        m_owner;
		MojRefCountedPtr<MojServiceMessage> m_msg;
		std::string                         m_key;
		MojServiceMessage::CancelSignal::Slot<Subscription> m_cancelSlot;
	};
	friend class Subscription;

	std::string accountKey(const char* serviceName, const char* username) const;
	MojErr buildSnapshot(const std::string& key, MojObject& out) const;
	void   notifySubscribers(const std::string& key);
	void   removeSubscription(Subscription* sub);

	MojService* m_service;
	std::map<std::string, Challenge>              m_challenges;   // key -> current challenge
	std::list<MojRefCountedPtr<Subscription> >    m_subscribers;  // all live subscriptions
};

#endif /* AUTHCHANNEL_H_ */
