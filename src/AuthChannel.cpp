/*
 * AuthChannel.cpp
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport). See AuthChannel.h.
 *
 * Server-side luna subscription that surfaces interactive login challenges
 * (Discord remote-auth QR) to the accounts auth UI. Mirrors the db8
 * MojDbServiceHandler "Watcher" idiom: retain each subscribed MojServiceMessage,
 * notifyCancel() to reap it when the client goes away, and reply() repeatedly
 * (payload carrying "subscribed":true) to push state transitions.
 */

#include <glib.h>
#include "AuthChannel.h"
#include "IMServiceApp.h"

AuthChannel::Subscription::Subscription(AuthChannel* owner, MojServiceMessage* msg, const std::string& key)
: m_owner(owner),
  m_msg(msg),
  m_key(key),
  m_cancelSlot(this, &AuthChannel::Subscription::handleCancel)
{
	msg->notifyCancel(m_cancelSlot);
}

MojErr AuthChannel::Subscription::handleCancel(MojServiceMessage* msg)
{
	(void)msg;
	// The framework holds a ref across this dispatch, so it is safe for the owner
	// to drop our list ref here.
	if (m_owner)
		m_owner->removeSubscription(this);
	return MojErrNone;
}

AuthChannel::AuthChannel(MojService* service)
: m_service(service)
{
	MojLogInfo(IMServiceApp::s_log, _T("AuthChannel constructor"));
}

AuthChannel::~AuthChannel()
{
	m_subscribers.clear();   // MojRefCountedPtr entries release their handlers
}

void AuthChannel::removeSubscription(Subscription* sub)
{
	for (std::list<MojRefCountedPtr<Subscription> >::iterator it = m_subscribers.begin();
	     it != m_subscribers.end(); ++it) {
		if (it->get() == sub) {
			MojLogInfo(IMServiceApp::s_log, _T("AuthChannel: dropping subscriber key=%s"), sub->m_key.c_str());
			m_subscribers.erase(it);
			return;
		}
	}
}

std::string AuthChannel::accountKey(const char* serviceName, const char* username) const
{
	std::string s(serviceName ? serviceName : "");
	std::string u(username ? username : "");
	return u + "_" + s;   // matches LibpurpleAdapter getAccountKey(username, serviceName)
}

// ---- producers (called from LibpurpleAdapter, glib main loop) ----------------

void AuthChannel::publishQRChallenge(const char* serviceName, const char* username,
                                     const unsigned char* imageData, size_t imageLen,
                                     const char* imageMimeType, const char* urlString)
{
	std::string key = accountKey(serviceName, username);
	Challenge& c = m_challenges[key];
	c.kind  = ChallengeQRCode;
	c.state = StateWaiting;
	c.urlString = urlString ? urlString : "";
	c.token.clear();
	c.message.clear();

	if (imageData && imageLen > 0) {
		gchar* b64 = g_base64_encode(imageData, imageLen);
		c.imageDataUri  = "data:";
		c.imageDataUri += (imageMimeType && *imageMimeType) ? imageMimeType : "image/png";
		c.imageDataUri += ";base64,";
		c.imageDataUri += b64;
		g_free(b64);
	}

	MojLogInfo(IMServiceApp::s_log, _T("AuthChannel::publishQRChallenge key=%s (%u image bytes)"),
	           key.c_str(), (unsigned)imageLen);
	notifySubscribers(key);
}

void AuthChannel::publishCaptchaChallenge(const char* serviceName, const char* username,
                                          const char* service, const char* sitekey,
                                          const char* rqdata, const char* rqtoken)
{
	std::string key = accountKey(serviceName, username);
	Challenge& c = m_challenges[key];
	c.kind  = ChallengeCaptcha;
	c.state = StateWaiting;
	c.imageDataUri.clear();
	c.urlString.clear();
	c.token.clear();
	c.message.clear();
	c.captchaService = (service && *service) ? service : "hcaptcha";
	c.captchaSitekey = sitekey ? sitekey : "";
	c.captchaRqData  = rqdata ? rqdata : "";
	c.captchaRqToken = rqtoken ? rqtoken : "";

	MojLogInfo(IMServiceApp::s_log, _T("AuthChannel::publishCaptchaChallenge key=%s (service=%s sitekey=%s)"),
	           key.c_str(), c.captchaService.c_str(), c.captchaSitekey.c_str());
	notifySubscribers(key);
}

void AuthChannel::setChallengeState(const char* serviceName, const char* username,
                                    ChallengeState state, const char* message)
{
	std::string key = accountKey(serviceName, username);
	std::map<std::string, Challenge>::iterator it = m_challenges.find(key);
	if (it == m_challenges.end()) {
		// No challenge to advance (e.g. a normal, non-QR login). Ignore.
		return;
	}
	it->second.state = state;
	if (message)
		it->second.message = message;
	MojLogInfo(IMServiceApp::s_log, _T("AuthChannel::setChallengeState key=%s state=%d"), key.c_str(), (int)state);
	notifySubscribers(key);
}

void AuthChannel::setConfirmed(const char* serviceName, const char* username, const char* token)
{
	std::string key = accountKey(serviceName, username);
	std::map<std::string, Challenge>::iterator it = m_challenges.find(key);
	if (it == m_challenges.end())
		return;
	it->second.state = StateConfirmed;
	it->second.token = token ? token : "";
	MojLogInfo(IMServiceApp::s_log, _T("AuthChannel::setConfirmed key=%s (token %s)"),
	           key.c_str(), (token && *token) ? "present" : "absent");
	notifySubscribers(key);
}

void AuthChannel::clearChallenge(const char* serviceName, const char* username)
{
	std::string key = accountKey(serviceName, username);
	m_challenges.erase(key);
	MojLogInfo(IMServiceApp::s_log, _T("AuthChannel::clearChallenge key=%s"), key.c_str());
}

// ---- snapshot + push ---------------------------------------------------------

MojErr AuthChannel::buildSnapshot(const std::string& key, MojObject& out) const
{
	MojErr err = out.putBool(_T("returnValue"), true);
	MojErrCheck(err);
	err = out.putBool(_T("subscribed"), true);
	MojErrCheck(err);

	std::map<std::string, Challenge>::const_iterator it = m_challenges.find(key);
	if (it == m_challenges.end()) {
		// No active challenge yet -> report "none" so the UI keeps polling.
		err = out.putString(_T("state"), _T("none"));
		MojErrCheck(err);
		return MojErrNone;
	}

	const Challenge& c = it->second;
	const char* stateStr = "waiting";
	switch (c.state) {
		case StateWaiting:   stateStr = "waiting";   break;
		case StateScanned:   stateStr = "scanned";   break;
		case StateConfirmed: stateStr = "confirmed"; break;
		case StateExpired:   stateStr = "expired";   break;
		case StateFailed:    stateStr = "failed";    break;
	}
	err = out.putString(_T("state"), stateStr);
	MojErrCheck(err);

	// "kind" lets the validator pick its renderer (QR <img> vs hCaptcha WebView).
	const char* kindStr = "none";
	switch (c.kind) {
		case ChallengeQRCode:  kindStr = "qrcode";  break;
		case ChallengeCaptcha: kindStr = "captcha"; break;
		case ChallengeInput:   kindStr = "input";   break;
		default:               kindStr = "none";    break;
	}
	err = out.putString(_T("kind"), kindStr);
	MojErrCheck(err);

	if (c.kind == ChallengeCaptcha) {
		err = out.putString(_T("captchaService"), c.captchaService.c_str());
		MojErrCheck(err);
		err = out.putString(_T("captchaSitekey"), c.captchaSitekey.c_str());
		MojErrCheck(err);
		err = out.putString(_T("captchaRqData"), c.captchaRqData.c_str());
		MojErrCheck(err);
		err = out.putString(_T("captchaRqToken"), c.captchaRqToken.c_str());
		MojErrCheck(err);
	}

	if (!c.imageDataUri.empty()) {
		err = out.putString(_T("qrImage"), c.imageDataUri.c_str());
		MojErrCheck(err);
	}
	if (!c.urlString.empty()) {
		err = out.putString(_T("urlString"), c.urlString.c_str());
		MojErrCheck(err);
	}
	if (!c.message.empty()) {
		err = out.putString(_T("message"), c.message.c_str());
		MojErrCheck(err);
	}
	if (c.state == StateConfirmed && !c.token.empty()) {
		err = out.putString(_T("token"), c.token.c_str());
		MojErrCheck(err);
	}
	return MojErrNone;
}

void AuthChannel::notifySubscribers(const std::string& key)
{
	MojObject snapshot;
	if (buildSnapshot(key, snapshot) != MojErrNone) {
		MojLogError(IMServiceApp::s_log, _T("AuthChannel::notifySubscribers buildSnapshot failed"));
		return;
	}
	for (std::list<MojRefCountedPtr<Subscription> >::iterator it = m_subscribers.begin();
	     it != m_subscribers.end(); ++it) {
		Subscription* sub = it->get();
		if (sub == NULL || sub->m_key != key || sub->m_msg.get() == NULL)
			continue;
		MojErr err = sub->m_msg->reply(snapshot);
		if (err != MojErrNone)
			MojLogError(IMServiceApp::s_log, _T("AuthChannel::notifySubscribers reply err %d"), (int)err);
	}
}

// ---- luna method backends (called from IMServiceHandler) ---------------------

MojErr AuthChannel::subscribe(MojServiceMessage* msg, const char* serviceName, const char* username)
{
	std::string key = accountKey(serviceName, username);

	// Register the held-open subscription (ref-counted; reaps itself on cancel).
	MojRefCountedPtr<Subscription> sub(new Subscription(this, msg, key));
	MojAllocCheck(sub.get());
	m_subscribers.push_back(sub);

	MojLogInfo(IMServiceApp::s_log, _T("AuthChannel::subscribe key=%s (%u subscribers)"),
	           key.c_str(), (unsigned)m_subscribers.size());

	// Immediately reply with the current snapshot (the UI polls, but a fresh
	// subscribe should also get the current state right away).
	MojObject snapshot;
	MojErr err = buildSnapshot(key, snapshot);
	MojErrCheck(err);
	return msg->reply(snapshot);
}

MojErr AuthChannel::submitInput(MojServiceMessage* msg, const char* serviceName,
                                const char* username, const char* action, const char* value)
{
	// The actual refresh/cancel work (restart or tear down the pending prpl login)
	// is done by IMServiceHandler via LibpurpleAdapter; here we just acknowledge and,
	// for cancel, drop any challenge snapshot so a later subscribe reports "none".
	(void)value;
	std::string key = accountKey(serviceName, username);
	if (action && strcmp(action, "cancel") == 0)
		m_challenges.erase(key);

	MojObject reply;
	MojErr err = reply.putBool(_T("returnValue"), true);
	MojErrCheck(err);
	return msg->replySuccess(reply);
}
