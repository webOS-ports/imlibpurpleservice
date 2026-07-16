/*
 * sanitize.h
 *
 * Copyright (c) 2015 Nikolay Nizov <nizovn@gmail.com>
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
 */

#ifndef SANITIZE_H_
#define SANITIZE_H_

char *sanitizeHtml(const char *input, char **except, bool remove);
char *unsanitizeHtml(char *input);

/*
 * Encode non-BMP (astral, > U+FFFF) code points as decimal numeric HTML entities
 * (e.g. U+1F62D "😭" -> "&#128557;"), returning a newly allocated string (free with free()).
 * BMP text is copied through unchanged.
 *
 * Why: the device's JS runtimes - both the WebKit app service bridge AND the node.js
 * services (e.g. the chatthreader) - corrupt 4-byte UTF-8 sequences to U+FFFD when they
 * receive luna-service / db8 payloads. Emoji stored as raw UTF-8 are therefore destroyed
 * before any JavaScript can read them. Native code here sees the bytes intact, so encoding
 * astral code points to plain ASCII entities lets them survive the round-trip; the Messaging
 * app then renders the entities as inline emoji images.
 */
char *encodeAstralEntities(const char *input);

#endif
