/*
 * sanitize.cpp
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
#include <glib.h>
#include <tidy.h>
#include <buffio.h>
#include <stdio.h>
#include <errno.h>
#include "entities.h"
#include "sanitize.h"

char *unsanitizeHtml(char *input) {
	char *result = (char*) malloc( sizeof(char) * (strlen(input) + 1) );
	decode_html_entities_utf8(result, input);
	return result;
}

// See sanitize.h for the rationale. Walks the UTF-8 string and rewrites any code point
// above U+FFFF as a decimal numeric HTML entity so it survives the device's JS runtimes,
// which mangle 4-byte UTF-8 to U+FFFD. Invalid bytes are passed through untouched.
char *encodeAstralEntities(const char *input) {
	if (!input)
		return g_strdup("");

	GString *out = g_string_sized_new(strlen(input) + 16);
	const char *p = input;
	while (*p) {
		gunichar cp = g_utf8_get_char_validated(p, -1);
		if (cp == (gunichar) -1 || cp == (gunichar) -2) {
			// invalid / incomplete UTF-8 - keep the raw byte and advance one
			g_string_append_c(out, *p);
			p++;
			continue;
		}
		const char *next = g_utf8_next_char(p);
		if (cp > 0xFFFF) {
			g_string_append_printf(out, "&#%u;", (unsigned) cp);
		} else {
			g_string_append_len(out, p, next - p);
		}
		p = next;
	}
	// free the GString wrapper but hand back the char* buffer (caller frees with free())
	return g_string_free(out, FALSE);
}

char *sanitizeHtml(const char *input, char **except, bool remove)
{
	TidyBuffer output = {0};
	TidyBuffer errbuf = {0};
	Bool ok = no;

	TidyDoc tdoc = tidyCreate(); // Initialize "document"
	int rc = tidySetCharEncoding( tdoc, "utf8");
	if ( rc >= 0 )
		ok = tidyOptSetInt( tdoc, TidyWrapLen, 0 ); // Convert to XHTML
	if (ok)
		ok = tidyOptSetValue( tdoc, TidyBodyOnly, "yes" ); // Convert to XHTML
	if (ok)
		ok = tidyOptSetBool( tdoc, TidyXhtmlOut, yes ); // Convert to XHTML
	if (ok)
		rc = tidySetErrorBuffer( tdoc, &errbuf ); // Capture diagnostics
	if ( rc >= 0 )
		rc = tidyParseString( tdoc, input ); // Parse the input
	if ( rc >= 0 )
		rc = tidyCleanAndRepair( tdoc ); // Tidy it up!
	if ( rc >= 0 )
		rc = tidyRunDiagnostics( tdoc ); // Kvetch
	if ( rc > 1 ) // If error, force output.
		rc = ( tidyOptSetBool(tdoc, TidyForceOutput, yes) ? rc : -1 );
	if ( rc >= 0 )
		rc = tidySaveBuffer( tdoc, &output ); // Pretty Print

	gchar *message_start;
	unsigned int message_size;
	if (rc >= 0) {
		message_size = output.size;
		message_start = g_strndup((const gchar *) output.bp, message_size);
	}
	else {
		message_start = g_strdup("A severe error occurred.");
		message_size = strlen(message_start);
	}
	tidyBufFree( &output );
	tidyBufFree( &errbuf );
	tidyRelease( tdoc );

	int count = 0;
	for (unsigned int i=0;i<message_size;i++)
		if ((message_start[i] == '<') || (message_start[i] == '>'))
			count++;

	// maximum possible resulted lenght
	// <a>a</a> => &lt;a&gt;a&lt;/a&gt;
	int result_size = message_size + count*3;
	gchar *result = g_strnfill(result_size, ' ');

	char *tag_start = strchr(message_start, '<');
	if (!tag_start) {
		g_free(result);
		return g_strchomp(message_start);
	}

	memcpy(result, message_start, tag_start-message_start);
	gchar *result_end = result + (tag_start-message_start);

	while (true) {
		tag_start++;
		gchar *tag_end = tag_start + 1;
		while ( (!isspace(*tag_end)) && (*tag_end != '/') && (*tag_end != '>') )
			tag_end++;
		gchar *tag=g_strndup(tag_start,tag_end-tag_start);
		bool is_allowed = false;
		int i=0;
		while (except[i]) {
			if ( (strlen(except[i]) == strlen(tag)) && (!strncmp(except[i], tag, strlen(except[i]))) ){
				is_allowed = true;
				break;
			}
			i++;
		}
		g_free(tag);
		tag_end = strchr(tag_end, '>');
		if (!tag_end) {
			// malformed markup: '<' with no closing '>'. Copy the remainder verbatim and stop
			// rather than doing pointer arithmetic on a NULL tag_end (crash / huge alloc).
			g_strlcpy(result_end, tag_start, message_size - (tag_start-message_start));
			break;
		}
		tag = g_strndup(tag_start, tag_end - tag_start);

		if (is_allowed) {
			result_end[0] = '<';
			result_end++;
			memcpy(result_end, tag, strlen(tag));
			result_end += strlen(tag);
			result_end[0] = '>';
			result_end++;
		}

		if ((!is_allowed) && (!remove)) {
			memcpy(result_end, "&lt;", 4);
			result_end += 4;
			memcpy(result_end, tag, strlen(tag));
			result_end += strlen(tag);
			memcpy(result_end, "&gt;", 4);
			result_end += 4;
		}

		g_free(tag);
		tag_start = tag_end + 1;
		tag_end = strchr(tag_end, '<');
		if (!tag_end) {
			g_strlcpy(result_end, tag_start, message_size - (tag_start-message_start));
			break;
		}
		memcpy(result_end, tag_start, tag_end-tag_start);
		result_end += tag_end-tag_start;
		tag_start = tag_end;
	}

	g_free(message_start);
	return g_strchomp(result);
}
