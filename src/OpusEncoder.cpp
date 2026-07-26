/*
 * OpusEncoder.cpp - see OpusEncoder.h.
 *
 * Minimal, self-contained WAV(PCM16) -> Ogg/Opus voice-note encoder. libopus reference encoder (VOIP)
 * + libogg muxing per RFC 7845. No gst-launch / external process.
 */
#include "OpusEncoder.h"

#include <opus/opus.h>
#include <ogg/ogg.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>

namespace {

// ---- little-endian readers over a byte buffer ------------------------------------------------
static uint16_t rd_u16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd_u32(const uint8_t* p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }

static void put_u16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF); }
static void put_u32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; i++) v.push_back((x >> (8 * i)) & 0xFF); }

// Parse a RIFF/WAVE PCM16 file. Ignores the (possibly bogus/streaming) declared data length and uses
// the actual bytes present. Returns the PCM as interleaved int16, plus rate/channels. false on error.
static bool parse_wav(const uint8_t* buf, size_t len, std::vector<int16_t>& pcm, int& rate, int& channels)
{
	if (len < 44 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0)
		return false;
	int bits = 0; rate = 0; channels = 0;
	size_t pcmOff = 0, pcmBytes = 0;
	size_t off = 12;
	while (off + 8 <= len) {
		const uint8_t* id = buf + off;
		uint32_t sz = rd_u32(buf + off + 4);
		size_t body = off + 8;
		if (memcmp(id, "fmt ", 4) == 0 && body + 16 <= len) {
			channels = rd_u16(buf + body + 2);
			rate     = (int)rd_u32(buf + body + 4);
			bits     = rd_u16(buf + body + 14);
		} else if (memcmp(id, "data", 4) == 0) {
			pcmOff = body;
			// The native capture writes a bogus ~2GB data length; clamp to what's actually in the file.
			size_t avail = (body <= len) ? (len - body) : 0;
			pcmBytes = ((size_t)sz <= avail) ? (size_t)sz : avail;
			break; // audio starts here
		}
		// chunks are word-aligned
		off = body + sz + (sz & 1);
		if (sz == 0xFFFFFFFFu || sz > len) break; // guard against the bogus length looping
	}
	if (channels < 1 || channels > 2 || rate <= 0 || bits != 16 || pcmOff == 0 || pcmBytes < 2)
		return false;
	size_t n = pcmBytes / 2;
	pcm.resize(n);
	for (size_t i = 0; i < n; i++)
		pcm[i] = (int16_t)rd_u16(buf + pcmOff + i * 2);
	return true;
}

static bool read_file(const char* path, std::vector<uint8_t>& out)
{
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	if (sz <= 0) { fclose(f); return false; }
	out.resize((size_t)sz);
	size_t rd = fread(out.data(), 1, (size_t)sz, f);
	fclose(f);
	return rd == (size_t)sz;
}

static bool write_page(FILE* f, ogg_page* pg)
{
	return fwrite(pg->header, 1, pg->header_len, f) == (size_t)pg->header_len &&
	       fwrite(pg->body,   1, pg->body_len,   f) == (size_t)pg->body_len;
}

} // namespace

bool wav_to_opus_voicenote(const char* wavPath, const char* oggPath, float gain)
{
	std::vector<uint8_t> raw;
	if (!read_file(wavPath, raw)) return false;

	std::vector<int16_t> pcm; int rate = 0, channels = 0;
	if (!parse_wav(raw.data(), raw.size(), pcm, rate, channels)) return false;

	// libopus only accepts 8/12/16/24/48 kHz; the native capture is 8 kHz which is fine.
	if (rate != 8000 && rate != 12000 && rate != 16000 && rate != 24000 && rate != 48000) return false;

	// Level: the native capture is quiet, so NORMALIZE to ~90% full-scale rather than a fixed boost
	// (which clips louder clips and under-boosts quiet ones). Cap the applied gain at `gain`x so a
	// near-silent recording isn't amplified into loud noise; never attenuate.
	if (gain > 1.0f && !pcm.empty()) {
		int peak = 1;
		for (size_t i = 0; i < pcm.size(); i++) { int m = pcm[i] < 0 ? -pcm[i] : pcm[i]; if (m > peak) peak = m; }
		float g = 29490.0f / (float)peak;   // -> ~90% of int16 full scale
		if (g > gain) g = gain;
		if (g > 1.0f) {
			for (size_t i = 0; i < pcm.size(); i++) {
				int v = (int)(pcm[i] * g);
				if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
				pcm[i] = (int16_t)v;
			}
		}
	}

	int err = 0;
	OpusEncoder* enc = opus_encoder_create(rate, channels, OPUS_APPLICATION_VOIP, &err);
	if (!enc || err != OPUS_OK) { if (enc) opus_encoder_destroy(enc); return false; }
	opus_encoder_ctl(enc, OPUS_SET_BITRATE(24000));
	opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
	int lookahead = 0;
	opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&lookahead)); // in input-rate samples
	uint16_t pre_skip = (uint16_t)((int64_t)lookahead * 48000 / rate);

	FILE* out = fopen(oggPath, "wb");
	if (!out) { opus_encoder_destroy(enc); return false; }

	ogg_stream_state os;
	ogg_stream_init(&os, 0x564f4943 /*'VOIC'*/);

	// --- OpusHead (BOS) ---
	{
		std::vector<uint8_t> h;
		h.insert(h.end(), {'O','p','u','s','H','e','a','d'});
		h.push_back(1);                    // version
		h.push_back((uint8_t)channels);    // channel count
		put_u16(h, pre_skip);              // pre-skip (48 kHz samples)
		put_u32(h, (uint32_t)rate);        // original input sample rate
		put_u16(h, 0);                     // output gain
		h.push_back(0);                    // channel mapping family 0
		ogg_packet op; memset(&op, 0, sizeof(op));
		op.packet = h.data(); op.bytes = (long)h.size(); op.b_o_s = 1; op.granulepos = 0; op.packetno = 0;
		ogg_stream_packetin(&os, &op);
		ogg_page pg; while (ogg_stream_flush(&os, &pg)) if (!write_page(out, &pg)) goto fail;
	}
	// --- OpusTags ---
	{
		std::vector<uint8_t> t;
		t.insert(t.end(), {'O','p','u','s','T','a','g','s'});
		const char* vendor = "webos-synergy libopus";
		put_u32(t, (uint32_t)strlen(vendor));
		t.insert(t.end(), vendor, vendor + strlen(vendor));
		put_u32(t, 0);                     // 0 user comments
		ogg_packet op; memset(&op, 0, sizeof(op));
		op.packet = t.data(); op.bytes = (long)t.size(); op.packetno = 1;
		ogg_stream_packetin(&os, &op);
		ogg_page pg; while (ogg_stream_flush(&os, &pg)) if (!write_page(out, &pg)) goto fail;
	}

	// --- audio: 20 ms frames ---
	{
		const int frame = rate / 50;                 // samples per channel, 20 ms
		const int64_t gran_per_frame = (int64_t)frame * 48000 / rate; // 48 kHz granules per frame
		std::vector<int16_t> in(frame * channels);
		unsigned char pkt[4000];
		size_t pos = 0; int64_t granule = 0; ogg_int64_t packetno = 2;
		size_t total = pcm.size();
		while (pos < total) {
			size_t got = total - pos; if (got > (size_t)(frame * channels)) got = frame * channels;
			memset(in.data(), 0, in.size() * sizeof(int16_t)); // zero-pad the final short frame
			memcpy(in.data(), pcm.data() + pos, got * sizeof(int16_t));
			pos += got;
			int nb = opus_encode(enc, in.data(), frame, pkt, sizeof(pkt));
			if (nb < 0) goto fail;
			granule += gran_per_frame;
			ogg_packet op; memset(&op, 0, sizeof(op));
			op.packet = pkt; op.bytes = nb; op.granulepos = granule; op.packetno = packetno++;
			op.e_o_s = (pos >= total) ? 1 : 0;
			ogg_stream_packetin(&os, &op);
			ogg_page pg;
			while (ogg_stream_pageout(&os, &pg)) if (!write_page(out, &pg)) goto fail;
		}
		ogg_page pg; while (ogg_stream_flush(&os, &pg)) if (!write_page(out, &pg)) goto fail;
	}

	ogg_stream_clear(&os);
	fclose(out);
	opus_encoder_destroy(enc);
	return true;

fail:
	ogg_stream_clear(&os);
	fclose(out);
	opus_encoder_destroy(enc);
	remove(oggPath);
	return false;
}
