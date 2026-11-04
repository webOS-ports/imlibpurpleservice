/*
 * VoiceNote.cpp
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport). See VoiceNote.h.
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free Software
 * Foundation.
 */

#include "VoiceNote.h"
#include "OpusEncoder.h"

// HAVE_GSTREAMER comes from CMakeLists.txt. The legacy webOS 3.0.5 build has no GStreamer 1.0: there
// an M4A cannot be decoded and goes out as the file it is, as before.
#ifdef HAVE_GSTREAMER
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#else
#include <glib.h>
#endif

#include <cstring>
#include <utility>

namespace {

#ifdef HAVE_GSTREAMER
// Ten minutes of 48 kHz mono is 55 MB of PCM; anything longer is sent as the file it is.
const int MAX_SECONDS = 600;

// Waits for the next decoded buffer this long before checking the bus again, and gives up after
// STALL_LIMIT of those in a row with neither data nor end of stream.
const GstClockTime PULL_TIMEOUT = 100 * GST_MSECOND;
const int STALL_LIMIT = 100;
#endif

bool ends_with_ci(const char* s, const char* suffix)
{
	size_t n = strlen(s), m = strlen(suffix);
	return n >= m && g_ascii_strcasecmp(s + n - m, suffix) == 0;
}

} // namespace

bool is_recorded_voicenote(const char* path)
{
	static const char dir[] = VOICENOTE_RECORDING_DIR;
	const size_t dirLen = sizeof(dir) - 1;
	if (!path || strncmp(path, dir, dirLen) != 0)
		return false;
	const char* name = path + dirLen;
	// Straight in the folder: no subfolder (the transports keep received files in their own) and
	// no way back out of it.
	if (strchr(name, '/') != NULL)
		return false;
	return strlen(name) > 4 && ends_with_ci(name, ".m4a");
}

bool decode_m4a_pcm(const char* path, int rate, std::vector<int16_t>& pcm)
{
	pcm.clear();
	if (!path || rate <= 0)
		return false;
#ifndef HAVE_GSTREAMER
	return false;
#else

	GError* error = NULL;
	if (!gst_init_check(NULL, NULL, &error))
	{
		g_clear_error(&error);
		return false;
	}

	// The decoder is named rather than left to decodebin: on a Halium device decodebin may pick the
	// Android codec, which is meant for playback and not for a quick decode inside a service.
	GstElement* pipeline = gst_parse_launch(
		"filesrc name=src ! qtdemux ! aacparse ! avdec_aac ! audioconvert ! audioresample"
		" ! capsfilter name=caps ! appsink name=sink sync=false", &error);
	if (!pipeline || error)
	{
		g_clear_error(&error);
		if (pipeline)
			gst_object_unref(pipeline);
		return false;
	}

	GstElement* src = gst_bin_get_by_name(GST_BIN(pipeline), "src");
	GstElement* caps = gst_bin_get_by_name(GST_BIN(pipeline), "caps");
	GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
	GstBus* bus = gst_element_get_bus(pipeline);
	bool ok = false;

	if (src && caps && sink && bus)
	{
		g_object_set(src, "location", path, NULL);
		// Native-endian int16, which is what pcm holds.
		GstCaps* want = gst_caps_new_simple("audio/x-raw",
			"format", G_TYPE_STRING, G_BYTE_ORDER == G_LITTLE_ENDIAN ? "S16LE" : "S16BE",
			"layout", G_TYPE_STRING, "interleaved",
			"channels", G_TYPE_INT, 1,
			"rate", G_TYPE_INT, rate, NULL);
		g_object_set(caps, "caps", want, NULL);
		gst_caps_unref(want);

		const size_t maxSamples = (size_t)rate * MAX_SECONDS;
		bool failed = gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE;
		int stalls = 0;
		while (!failed)
		{
			GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), PULL_TIMEOUT);
			if (sample)
			{
				stalls = 0;
				GstBuffer* buffer = gst_sample_get_buffer(sample);
				GstMapInfo map;
				if (buffer && gst_buffer_map(buffer, &map, GST_MAP_READ))
				{
					size_t count = map.size / sizeof(int16_t);
					if (pcm.size() + count > maxSamples)
						failed = true;
					else
					{
						size_t at = pcm.size();
						pcm.resize(at + count);
						memcpy(pcm.data() + at, map.data, count * sizeof(int16_t));
					}
					gst_buffer_unmap(buffer, &map);
				}
				gst_sample_unref(sample);
				continue;
			}
			if (gst_app_sink_is_eos(GST_APP_SINK(sink)))
			{
				ok = !pcm.empty();
				break;
			}
			GstMessage* msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
			if (msg)
			{
				gst_message_unref(msg);
				failed = true;
			}
			else if (++stalls >= STALL_LIMIT)
				failed = true;
		}
		gst_element_set_state(pipeline, GST_STATE_NULL);
	}

	if (bus)
		gst_object_unref(bus);
	if (sink)
		gst_object_unref(sink);
	if (caps)
		gst_object_unref(caps);
	if (src)
		gst_object_unref(src);
	gst_object_unref(pipeline);
	if (!ok)
		pcm.clear();
	return ok;
#endif
}

bool m4a_to_opus_voicenote(const char* m4aPath, const char* oggPath, float gain)
{
	const int rate = 48000;
	std::vector<int16_t> pcm;
	if (!oggPath || !decode_m4a_pcm(m4aPath, rate, pcm))
		return false;
	return pcm_to_opus_voicenote(std::move(pcm), rate, 1, oggPath, gain);
}
