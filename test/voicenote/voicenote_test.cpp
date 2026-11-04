/*
 * voicenote_test.cpp
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport).
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free Software
 * Foundation.
 *
 * VoiceNote.cpp: which files count as a recording, and the M4A -> Ogg/Opus transcode. The M4A is
 * made here the way the device's recorder makes it (AAC, 44.1 kHz stereo, MP4 container), and the
 * resulting .ogg is checked by reading its Opus header and decoding it back.
 */

#include "VoiceNote.h"

#include <gtest/gtest.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <dirent.h>
#include <unistd.h>
#include <vector>

namespace {

std::string scratchDir()
{
	static std::string dir;
	if (dir.empty())
	{
		char tmpl[] = "voicenote-test-XXXXXX";
		const char* made = mkdtemp(tmpl);
		if (!made)
			abort();
		char* abs = realpath(made, NULL);
		dir = abs;
		free(abs);
	}
	return dir;
}

std::vector<unsigned char> readAll(const std::string& path)
{
	std::ifstream in(path, std::ios::binary);
	return std::vector<unsigned char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool fileExists(const std::string& path)
{
	return access(path.c_str(), F_OK) == 0;
}

// Runs a gst-launch style pipeline to its end; false on an error.
bool runPipeline(const std::string& description)
{
	GError* error = NULL;
	GstElement* pipeline = gst_parse_launch(description.c_str(), &error);
	if (!pipeline || error)
	{
		if (error)
			ADD_FAILURE() << error->message;
		g_clear_error(&error);
		if (pipeline)
			gst_object_unref(pipeline);
		return false;
	}
	gst_element_set_state(pipeline, GST_STATE_PLAYING);
	GstBus* bus = gst_element_get_bus(pipeline);
	GstMessage* msg = gst_bus_timed_pop_filtered(bus, 30 * GST_SECOND,
		(GstMessageType)(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
	bool ok = msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS;
	if (msg)
		gst_message_unref(msg);
	gst_object_unref(bus);
	gst_element_set_state(pipeline, GST_STATE_NULL);
	gst_object_unref(pipeline);
	return ok;
}

// A recorder-like M4A: `seconds` of a 440 Hz tone, AAC at 44.1 kHz stereo in an MP4 container.
std::string makeM4a(const char* name, double seconds)
{
	std::string path = scratchDir() + "/" + name;
	int buffers = (int)(seconds * 44100 / 1024);
	bool made = runPipeline("audiotestsrc wave=sine freq=440 volume=0.5 samplesperbuffer=1024 num-buffers="
		+ std::to_string(buffers) + " ! audio/x-raw,rate=44100,channels=2 ! audioconvert ! avenc_aac"
		" ! mp4mux ! filesink location=" + path);
	EXPECT_TRUE(made) << "could not make the test M4A (needs avenc_aac and mp4mux)";
	return path;
}

// Decodes an Ogg/Opus file back to mono 48 kHz samples.
std::vector<int16_t> decodeOgg(const std::string& path)
{
	std::vector<int16_t> out;
	GError* error = NULL;
	GstElement* pipeline = gst_parse_launch(("filesrc location=" + path + " ! oggdemux ! opusdec"
		" ! audioconvert ! audioresample ! audio/x-raw,format=S16LE,channels=1,rate=48000"
		" ! appsink name=sink sync=false").c_str(), &error);
	if (!pipeline || error)
	{
		g_clear_error(&error);
		if (pipeline)
			gst_object_unref(pipeline);
		return out;
	}
	GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
	gst_element_set_state(pipeline, GST_STATE_PLAYING);
	while (GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 5 * GST_SECOND))
	{
		GstMapInfo map;
		GstBuffer* buffer = gst_sample_get_buffer(sample);
		if (gst_buffer_map(buffer, &map, GST_MAP_READ))
		{
			const int16_t* s = reinterpret_cast<const int16_t*>(map.data);
			out.insert(out.end(), s, s + map.size / sizeof(int16_t));
			gst_buffer_unmap(buffer, &map);
		}
		gst_sample_unref(sample);
	}
	gst_element_set_state(pipeline, GST_STATE_NULL);
	gst_object_unref(sink);
	gst_object_unref(pipeline);
	return out;
}

// The OpusHead fields (RFC 7845) of an Ogg/Opus file, and the granule position of its last page.
struct OpusInfo { bool head = false; int channels = 0; unsigned inputRate = 0; long long lastGranule = -1; };

OpusInfo readOpusInfo(const std::vector<unsigned char>& ogg)
{
	OpusInfo info;
	for (size_t i = 0; i + 19 <= ogg.size(); i++)
	{
		if (memcmp(&ogg[i], "OpusHead", 8) == 0)
		{
			info.head = true;
			info.channels = ogg[i + 9];
			info.inputRate = ogg[i + 12] | (ogg[i + 13] << 8) | (ogg[i + 14] << 16) | ((unsigned)ogg[i + 15] << 24);
			break;
		}
	}
	for (size_t i = 0; i + 14 <= ogg.size(); i++)
	{
		if (memcmp(&ogg[i], "OggS", 4) == 0)
		{
			long long g = 0;
			for (int b = 7; b >= 0; b--)
				g = (g << 8) | ogg[i + 6 + b];
			info.lastGranule = g;
		}
	}
	return info;
}

double rms(const std::vector<int16_t>& pcm)
{
	if (pcm.empty())
		return 0;
	double sum = 0;
	for (int16_t s : pcm)
		sum += (double)s * s;
	return std::sqrt(sum / pcm.size());
}

} // namespace

TEST(IsRecordedVoicenote, TakesAnM4aStraightInTheRecordingFolder)
{
	EXPECT_TRUE(is_recorded_voicenote("/media/internal/.im-attachments/Audio04112026-07285605.m4a"));
	EXPECT_TRUE(is_recorded_voicenote("/media/internal/.im-attachments/Audio.M4A"));
}

TEST(IsRecordedVoicenote, LeavesEverythingElseAlone)
{
	// A music file picked to send, a file a transport saved, another type, a way out of the folder.
	EXPECT_FALSE(is_recorded_voicenote("/media/internal/music/song.m4a"));
	EXPECT_FALSE(is_recorded_voicenote("/media/internal/.im-attachments/whatsapp/in.m4a"));
	EXPECT_FALSE(is_recorded_voicenote("/media/internal/.im-attachments/../music/song.m4a"));
	EXPECT_FALSE(is_recorded_voicenote("/media/internal/.im-attachments/Audio.mp4"));
	EXPECT_FALSE(is_recorded_voicenote("/media/internal/.im-attachments/Audio.wav"));
	EXPECT_FALSE(is_recorded_voicenote("/media/internal/.im-attachments/.m4a"));
	EXPECT_FALSE(is_recorded_voicenote("/media/internal/.im-attachments/"));
	EXPECT_FALSE(is_recorded_voicenote("/media/internal/.im-attachments"));
	EXPECT_FALSE(is_recorded_voicenote(""));
	EXPECT_FALSE(is_recorded_voicenote(NULL));
}

TEST(M4aToOpus, MakesAMono48kOpusVoiceNoteOfTheSameLength)
{
	std::string m4a = makeM4a("tone.m4a", 2.0);
	std::string ogg = scratchDir() + "/tone.ogg";
	ASSERT_TRUE(m4a_to_opus_voicenote(m4a.c_str(), ogg.c_str(), 1.0f));

	std::vector<unsigned char> bytes = readAll(ogg);
	ASSERT_GE(bytes.size(), 4u);
	EXPECT_EQ(0, memcmp(bytes.data(), "OggS", 4));
	OpusInfo info = readOpusInfo(bytes);
	EXPECT_TRUE(info.head);
	EXPECT_EQ(1, info.channels);
	EXPECT_EQ(48000u, info.inputRate);
	// 2 s at 48 kHz, give or take the AAC priming and the last 20 ms frame.
	EXPECT_NEAR(96000, info.lastGranule, 48000 * 0.1);

	std::vector<int16_t> back = decodeOgg(ogg);
	EXPECT_NEAR(96000, (double)back.size(), 48000 * 0.1);
	// The tone is still there: a 0.5 sine is ~0.35 of full scale RMS; mono and Opus keep most of it.
	EXPECT_GT(rms(back), 32767 * 0.2);
}

TEST(M4aToOpus, FailsOnAFileThatIsNotAudioAndLeavesNoOgg)
{
	std::string junk = scratchDir() + "/junk.m4a";
	{
		std::ofstream out(junk, std::ios::binary);
		out << "this is not an mp4 file at all";
	}
	std::string ogg = scratchDir() + "/junk.ogg";
	EXPECT_FALSE(m4a_to_opus_voicenote(junk.c_str(), ogg.c_str(), 1.0f));
	EXPECT_FALSE(fileExists(ogg));
}

TEST(M4aToOpus, FailsOnAMissingFile)
{
	std::string ogg = scratchDir() + "/missing.ogg";
	EXPECT_FALSE(m4a_to_opus_voicenote((scratchDir() + "/missing.m4a").c_str(), ogg.c_str(), 1.0f));
	EXPECT_FALSE(fileExists(ogg));
	std::vector<int16_t> pcm(5, 1);
	EXPECT_FALSE(decode_m4a_pcm(NULL, 48000, pcm));
	EXPECT_TRUE(pcm.empty());
}

TEST(M4aToOpus, FailsOnARecordingWithNoSoundInIt)
{
	// A recording stopped before the first buffer: a valid MP4 with an empty audio track.
	std::string empty = scratchDir() + "/empty.m4a";
	ASSERT_TRUE(runPipeline("audiotestsrc num-buffers=0 ! audio/x-raw,rate=44100,channels=2 ! audioconvert"
		" ! avenc_aac ! mp4mux ! filesink location=" + empty));
	std::string ogg = scratchDir() + "/empty.ogg";
	EXPECT_FALSE(m4a_to_opus_voicenote(empty.c_str(), ogg.c_str(), 1.0f));
	EXPECT_FALSE(fileExists(ogg));
}

TEST(M4aToOpus, ReadsTheSameFileTwice)
{
	// The service decodes every voice note in one process: GStreamer is set up once and reused.
	std::string m4a = makeM4a("again.m4a", 0.5);
	std::vector<int16_t> first, second;
	ASSERT_TRUE(decode_m4a_pcm(m4a.c_str(), 48000, first));
	ASSERT_TRUE(decode_m4a_pcm(m4a.c_str(), 48000, second));
	EXPECT_EQ(first.size(), second.size());
}

int main(int argc, char** argv)
{
	gst_init(&argc, &argv);
	::testing::InitGoogleTest(&argc, argv);
	int rc = RUN_ALL_TESTS();
	std::string dir = scratchDir();
	if (DIR* d = opendir(dir.c_str()))
	{
		while (struct dirent* e = readdir(d))
			if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
				unlink((dir + "/" + e->d_name).c_str());
		closedir(d);
	}
	rmdir(dir.c_str());
	return rc;
}
