/*
 * VoiceNote.h
 *
 * Part of imlibpurpleservice (webOS libpurple IM transport).
 *
 * This program is free software and licensed under the terms of the GNU
 * General Public License Version 2 as published by the Free Software
 * Foundation.
 *
 * Voice messages recorded by the Messaging app. com.webos.service.mediarecorder only writes M4A
 * (AAC in an MP4 container), which the services take for a video: WhatsApp and Signal sniff the
 * bytes and see video/mp4, and Telegram only sends an .ogg as a voice note. These turn such a
 * recording into the Ogg/Opus voice note that the WAV path (OpusEncoder.h) already produces.
 */

#ifndef VOICENOTE_H_
#define VOICENOTE_H_

#include <cstdint>
#include <vector>

// The folder the Messaging app has the recorder write voice messages to.
#define VOICENOTE_RECORDING_DIR "/media/internal/.im-attachments/"

// True for an .m4a straight in VOICENOTE_RECORDING_DIR: a recording, not a music file picked from
// elsewhere, which goes out as the file it is.
bool is_recorded_voicenote(const char* path);

// Decodes the audio of an M4A (AAC) file to interleaved 16-bit mono PCM at `rate` Hz, through
// GStreamer. False when it cannot be read or decoded, or runs longer than the length cap.
bool decode_m4a_pcm(const char* path, int rate, std::vector<int16_t>& pcm);

// decode_m4a_pcm, then pcm_to_opus_voicenote (48 kHz mono). On failure oggPath is not left as a
// valid file.
bool m4a_to_opus_voicenote(const char* m4aPath, const char* oggPath, float gain);

#endif /* VOICENOTE_H_ */
