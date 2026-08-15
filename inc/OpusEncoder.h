/*
 * OpusEncoder.h
 *
 * webOS voice messages: transcode a 16-bit PCM WAV file (as produced by the native mediaserver
 * MediaCaptureV3 "audio:" capture - clean 8 kHz mono, but with a broken/streaming data-length header)
 * into an Ogg/Opus voice note that every Opus-native network (WhatsApp/Telegram/Discord/FB-E2EE)
 * accepts. Uses libopus (reference encoder, VOIP mode) + libogg muxing - no gst-launch, no external
 * process (the gst-0.10 opusenc backport hangs at non-48 kHz, and shelling out from a resident service
 * is fragile). The WAV's declared data length is ignored in favour of the actual file size.
 */
#ifndef OPUSENCODER_H_
#define OPUSENCODER_H_

// Transcode wavPath -> oggPath (Ogg/Opus, VOIP). gain is a linear amplitude multiplier applied to the
// PCM before encoding (the native capture is quiet ~23% peak, so ~3.0 lifts it; samples are clamped to
// int16). Returns true on success. On failure oggPath is not left as a valid file.
bool wav_to_opus_voicenote(const char* wavPath, const char* oggPath, float gain);

#endif /* OPUSENCODER_H_ */
