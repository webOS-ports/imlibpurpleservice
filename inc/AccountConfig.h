/*
 * AccountConfig.h
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport).
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free Software
 * Foundation.
 *
 * The settings a sign-in app returned for an account, as login() gets them.
 *
 * onCreate stores them as a com.palm.config.libpurple:1 row, {accountId, config: {...}}, and
 * OnEnabledHandler copies that whole row into the account's imloginstate record as "config". So
 * what reaches login() is the row, with the settings one level down. Old records, or a caller that
 * passes the settings themselves, have them at the top. Either way this returns the settings.
 */

#ifndef ACCOUNTCONFIG_H_
#define ACCOUNTCONFIG_H_

#include "core/MojObject.h"

inline MojObject accountSettings(const MojObject& stored)
{
	MojObject inner;
	if (stored.type() == MojObject::TypeObject && stored.get(_T("config"), inner) &&
	    inner.type() == MojObject::TypeObject)
		return inner;
	return stored;
}

#endif /* ACCOUNTCONFIG_H_ */
