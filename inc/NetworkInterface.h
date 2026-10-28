/*
 * NetworkInterface.h
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport).
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free Software
 * Foundation.
 *
 * Reading one network interface out of a com.palm.connectionmanager status. Kept apart from
 * ConnectionStateHandler so test/connstate can check it without the rest of the service.
 */

#ifndef NETWORKINTERFACE_H_
#define NETWORKINTERFACE_H_

#include "core/MojObject.h"
#include "core/MojString.h"

bool readNetworkInterface(const MojObject& internet, const MojChar* name, MojString& ipAddress);

#endif /* NETWORKINTERFACE_H_ */
