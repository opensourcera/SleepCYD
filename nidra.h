// Upload a recording to the Nidra ingest API (the n8n webhook the phone app uses), straight from the SD card.
//
//   POST <url>  header X-Nidra-Token, JSON body, one message at a time:
//     start     {type, recording, participant_id, patient_name, device_name, device_serial, timezone, source, started_at, session_type}
//     chunk     {type, recording, seq, packets, data_base64, sha256}   300 packets (about 170 KB raw) per chunk; the server
//               answers with the SHA-256 it stored and the chunk only counts when it matches
//     complete  {type, recording, packets, chunks, capture_bytes, capture_sha256, ended_at, duration_seconds, end_reason}
//   The chunk body is produced on the fly (base64 of the bytes read from the card, hash computed while streaming), so the
//   whole night never needs to be in memory. Reference clients: the Nidra project's push_recording.py and the phone app's Uploader.java.
#pragma once
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SD.h>
#include <time.h>
#include "mbedtls/sha256.h"

#define NIDRA_PKTS_PER_CHUNK 300

static const char NIDRA_B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static size_t nidra_b64(const uint8_t *in, size_t n, uint8_t *out) {
  size_t o = 0, i = 0;
  for (; i + 2 < n; i += 3) {
    uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
    out[o++] = NIDRA_B64[v >> 18]; out[o++] = NIDRA_B64[(v >> 12) & 63]; out[o++] = NIDRA_B64[(v >> 6) & 63]; out[o++] = NIDRA_B64[v & 63];
  }
  if (i < n) {
    uint32_t v = ((uint32_t)in[i] << 16) | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0);
    out[o++] = NIDRA_B64[v >> 18]; out[o++] = NIDRA_B64[(v >> 12) & 63]; out[o++] = i + 1 < n ? NIDRA_B64[(v >> 6) & 63] : '='; out[o++] = '=';
  }
  return o;
}
static void nidra_hex(const uint8_t *d, char *out) { for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", d[i]); out[64] = 0; }
static String nidra_iso(time_t t) { struct tm tm; gmtime_r(&t, &tm); char b[40]; strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%S.000+0000", &tm); return String(b); }
static String nidra_json_str(const String &s) { String o; for (size_t i = 0; i < s.length(); i++) { char c = s[i]; if (c == '"' || c == '\\') o += '\\'; if ((uint8_t)c >= 32) o += c; } return o; }
static bool nidra_have_time() { return time(NULL) > 1700000000; }
static bool nidra_sync_time() {                       // the board has no clock: take it from NTP once Wi-Fi is up
  if (nidra_have_time()) return true;
  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  for (int i = 0; i < 50 && !nidra_have_time(); i++) delay(200);
  return nidra_have_time();
}

// progress: bytes of the file handed to the network so far
static size_t nidra_total = 0, nidra_done = 0; static void (*nidra_pct_cb)(int) = NULL; static int nidra_last_pct = -1;
static void nidra_tick(size_t n) {
  nidra_done += n; int pct = nidra_total ? (int)(100ULL * nidra_done / nidra_total) : 100;
  if (pct != nidra_last_pct) { nidra_last_pct = pct; if (nidra_pct_cb) nidra_pct_cb(pct); }
}

// One chunk message as a Stream: head JSON, base64 of `bytes` bytes read from the file, then ,"sha256":"..."}
class NidraChunk : public Stream {
 public:
  File &f; size_t raw_left; String head; mbedtls_sha256_context sha; mbedtls_sha256_context *whole;
  static uint8_t out[2048], raw[1536];               // static: the loop task's stack is small
  size_t out_n = 0, out_pos = 0; int stage = 0; char sha_hex[65] = {0};
  NidraChunk(File &file, size_t bytes, const String &h, mbedtls_sha256_context *wholectx) : f(file), raw_left(bytes), head(h), whole(wholectx) {
    mbedtls_sha256_init(&sha); mbedtls_sha256_starts(&sha, 0);
  }
  ~NidraChunk() { mbedtls_sha256_free(&sha); }
  size_t total() const { return head.length() + 4 * ((raw_left + 2) / 3) + 12 + 64 + 2; }   // "  ,"sha256":"  hex  "}
  void fill() {
    if (out_pos < out_n) return;
    out_pos = out_n = 0;
    if (stage == 0) { memcpy(out, head.c_str(), head.length()); out_n = head.length(); stage = 1; return; }
    if (stage == 1) {
      if (raw_left) {
        size_t want = raw_left < sizeof raw ? raw_left : sizeof raw, n = 0;
        while (n < want) { int r = f.read(raw + n, want - n); if (r <= 0) break; n += r; }
        raw_left -= n; if (n < want) raw_left = 0;    // short file: finish the chunk (the checksum will not match and the chunk is retried)
        mbedtls_sha256_update(&sha, raw, n); if (whole) mbedtls_sha256_update(whole, raw, n);
        nidra_tick(n); out_n = nidra_b64(raw, n, out); return;
      }
      uint8_t dig[32]; mbedtls_sha256_finish(&sha, dig); nidra_hex(dig, sha_hex);
      out_n = snprintf((char *)out, sizeof out, "\",\"sha256\":\"%s\"}", sha_hex); stage = 2;
    }
  }
  int available() override { fill(); return out_n - out_pos; }
  int read() override { fill(); return out_pos < out_n ? out[out_pos++] : -1; }
  size_t readBytes(uint8_t *buf, size_t len) override {
    size_t got = 0;
    while (got < len) { fill(); if (out_pos >= out_n) break; size_t n = len - got < out_n - out_pos ? len - got : out_n - out_pos; memcpy(buf + got, out + out_pos, n); out_pos += n; got += n; }
    return got;
  }
  size_t readBytes(char *buf, size_t len) override { return readBytes((uint8_t *)buf, len); }
  int peek() override { fill(); return out_pos < out_n ? out[out_pos] : -1; }
  size_t write(uint8_t) override { return 0; }
};
uint8_t NidraChunk::out[2048]; uint8_t NidraChunk::raw[1536];

// how many bytes the next NIDRA_PKTS_PER_CHUNK packets take, starting at `pos` of a [u32 len][packet]... file
static size_t nidra_scan(File &f, size_t pos, int &packets, size_t total) {
  size_t p = pos; packets = 0;
  while (packets < NIDRA_PKTS_PER_CHUNK && p + 4 <= total) {
    f.seek(p); uint32_t len = 0; if (f.read((uint8_t *)&len, 4) != 4) break;
    if (len == 0 || len > 65536 || p + 4 + len > total) break;
    p += 4 + len; packets++;
  }
  return p - pos;
}

static int nidra_post(HTTPClient &http, NetworkClient &cl, const String &url, const String &token, const String *body, Stream *stream, size_t size, String &reply) {
  http.setReuse(true);
  if (!http.begin(cl, url)) return -100;
  http.addHeader("Content-Type", "application/json"); http.addHeader("X-Nidra-Token", token); http.setTimeout(60000);
  int code = body ? http.POST(*body) : http.sendRequest("POST", stream, size);
  reply = code > 0 ? http.getString() : http.errorToString(code);
  http.end();
  return code;
}
static bool nidra_ok(const String &reply) { return reply.indexOf("\"ok\"") >= 0 && reply.indexOf("true") > reply.indexOf("\"ok\""); }

// Returns 200 when the whole recording is on the server, else the HTTP code (or negative) of the message that failed.
// `recording` receives the session name the server knows the night by.
static int nidra_upload(const String &url, const String &token, const char *path, const String &patient, uint32_t serial, const char *source,
                        void (*status)(const char *), void (*pct)(int), String &recording) {
  File f = SD.open(path, FILE_READ);
  if (!f || !f.size()) { Serial.printf("nidra: no file %s\n", path); return -101; }
  size_t total = f.size();
  status("Getting the time..."); bool have_time = nidra_sync_time();
  int packets_total = 0; { size_t p = 0; while (p < total) { int n; size_t b = nidra_scan(f, p, n, total); if (!n) break; p += b; packets_total += n; } }
  time_t now = time(NULL), start = now - packets_total;   // one packet per second
  char rec[48]; const char *fn0 = strrchr(path, '/'); fn0 = fn0 ? fn0 + 1 : path;
  if (isdigit((unsigned char)fn0[0])) { strncpy(rec, fn0, sizeof rec - 1); rec[sizeof rec - 1] = 0; char *dot = strrchr(rec, '.'); if (dot) *dot = 0; }   // YYMMDD_<serial>_NNN: the file's own name
  else if (have_time) { struct tm tm; gmtime_r(&start, &tm); snprintf(rec, sizeof rec, "CYD%lu_%04d%02d%02d_%02d%02d%02d", (unsigned long)serial, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec); }
  else snprintf(rec, sizeof rec, "CYD%lu_x%08lx", (unsigned long)serial, (unsigned long)esp_random());
  recording = rec;
  String id = WiFi.macAddress(); id.replace(":", ""); id = "CYD-" + id.substring(6);
  Serial.printf("nidra: %s -> %s, %d packets, %u bytes, time %s, heap %u largest %u\n", path, rec, packets_total, (unsigned)total, have_time ? "ok" : "unknown", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  WiFiClientSecure sc; sc.setInsecure(); WiFiClient plain; HTTPClient http;   // TLS without certificate check (see README)
  bool https = url.startsWith("https"); NetworkClient &cl = https ? (NetworkClient &)sc : (NetworkClient &)plain;
  String reply;
  String body = String("{\"type\":\"start\",\"recording\":\"") + rec + "\",\"participant_id\":\"" + id + "\""
    + (patient.length() ? ",\"patient_name\":\"" + nidra_json_str(patient) + "\"" : "")
    + ",\"device_name\":\"WatchPAT ONE\",\"device_serial\":\"" + String((unsigned long)serial) + "\",\"timezone\":\"UTC\",\"source\":\"" + source + "\",\"session_type\":\"sleep\""
    + (have_time ? ",\"started_at\":\"" + nidra_iso(start) + "\"" : "") + "}";
  status("Starting session...");
  int code = nidra_post(http, cl, url, token, &body, NULL, 0, reply);
  Serial.printf("nidra: start -> %d %s (heap %u)\n", code, reply.substring(0, 120).c_str(), (unsigned)ESP.getFreeHeap());
  if (code != 200 || !nidra_ok(reply)) { f.close(); return code > 0 ? code : -1; }

  mbedtls_sha256_context whole, snap; mbedtls_sha256_init(&whole); mbedtls_sha256_init(&snap); mbedtls_sha256_starts(&whole, 0);
  nidra_total = total; nidra_done = 0; nidra_pct_cb = pct; nidra_last_pct = -1;
  size_t pos = 0; int seq = 0, pk_sent = 0; uint32_t t0 = millis();
  while (pos < total) {
    int pk; size_t bytes = nidra_scan(f, pos, pk, total);
    if (!pk) break;
    String head = String("{\"type\":\"chunk\",\"recording\":\"") + rec + "\",\"seq\":" + seq + ",\"packets\":" + pk + ",\"data_base64\":\"";
    bool sent = false;
    for (int attempt = 0; attempt < 3 && !sent; attempt++) {
      mbedtls_sha256_clone(&snap, &whole);              // the whole-file hash is rewound if this try fails
      size_t done0 = nidra_done; f.seek(pos);
      NidraChunk cs(f, bytes, head, &whole);
      code = nidra_post(http, cl, url, token, NULL, &cs, cs.total(), reply);
      sent = code == 200 && nidra_ok(reply) && reply.indexOf(cs.sha_hex) >= 0;
      if (!sent) {
        Serial.printf("nidra: chunk %d try %d -> %d %s\n", seq, attempt + 1, code, reply.substring(0, 100).c_str());
        mbedtls_sha256_clone(&whole, &snap); nidra_done = done0; status("Retrying..."); delay(1500);
      }
    }
    if (!sent) { mbedtls_sha256_free(&whole); mbedtls_sha256_free(&snap); f.close(); return code > 0 ? code : -2; }
    if (seq % 10 == 0) Serial.printf("nidra: chunk %d ok, %u of %u bytes after %lu s, heap %u\n", seq, (unsigned)(pos + bytes), (unsigned)total, (unsigned long)((millis() - t0) / 1000), (unsigned)ESP.getFreeHeap());
    pos += bytes; seq++; pk_sent += pk;
  }
  uint8_t dig[32]; mbedtls_sha256_finish(&whole, dig); char hex[65]; nidra_hex(dig, hex); mbedtls_sha256_free(&whole); mbedtls_sha256_free(&snap);
  time_t end = time(NULL);
  body = String("{\"type\":\"complete\",\"recording\":\"") + rec + "\",\"packets\":" + pk_sent + ",\"chunks\":" + seq + ",\"capture_bytes\":" + (unsigned long)pos
    + ",\"capture_sha256\":\"" + hex + "\"" + (have_time ? ",\"ended_at\":\"" + nidra_iso(end) + "\"" : "") + ",\"duration_seconds\":" + pk_sent + ",\"end_reason\":\"user\"}";
  status("Finishing...");
  code = nidra_post(http, cl, url, token, &body, NULL, 0, reply);
  Serial.printf("nidra: complete -> %d %s; %d chunks, %u bytes in %lu s\n", code, reply.substring(0, 120).c_str(), seq, (unsigned)pos, (unsigned long)((millis() - t0) / 1000));
  f.close();
  return code == 200 && nidra_ok(reply) ? 200 : (code > 0 ? code : -3);
}
