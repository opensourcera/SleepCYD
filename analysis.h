// Live analysis of WatchPAT ONE data packets on the ESP32, ported from C:\code\sleep\watchpat\watchpat_analysis.py.
// Feed each DATA_PACKET payload with an_feed(); read the derived values from the `an` struct once per packet.
// Pulse from PAT peaks (fallback oximetry), SpO2 from the red/IR AC-DC ratio at the pulse frequency (both channel
// orderings scored, trimmed median + EMA), respiration from chest zero crossings, body position from the dominant
// accelerometer axis, desaturation events (drop >= 3 % for >= 10 s) and a running session summary.
// These are the same heuristics as the PC tool: estimates for monitoring, not a clinical score.
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

#include <algorithm>
#define AN_RATE 100
#define AN_WAVE 500          // 5 s of 100 Hz waveform per channel
#define AN_CHEST 600         // 6 s of chest (respiration window)
#define AN_MOTION 50         // 10 s of 5 Hz motion
#define AN_TREND 600         // per-minute SpO2 trend (10 h; a night is about 480 minutes)
#define AN_PAT_BASE 300      // PAT amplitude baseline window (3 s envelopes, outside events)
#define AN_SPO2_HIST 128     // SpO2 values kept for the oxygen drop of a PAT event (duration + 5 s, at most 125)
#define AN_PAT_EVT 16

struct Ring { int16_t *buf; int cap, n, head; };
static void ring_push(Ring &r, int16_t v) { r.buf[r.head] = v; r.head = (r.head + 1) % r.cap; if (r.n < r.cap) r.n++; }
// copy the last count samples (oldest first) into out
static int ring_tail(const Ring &r, int16_t *out, int count) {
  if (count > r.n) count = r.n;
  int start = (r.head - count + r.cap) % r.cap;
  for (int i = 0; i < count; i++) out[i] = r.buf[(start + i) % r.cap];
  return count;
}

struct Analysis {
  // live values (-1 = unknown)
  float hr = -1, spo2 = -1, resp = -1, motion = -1;
  int metric = 0; char position[8] = "?"; const char *position_label = "Unknown";
  uint32_t seconds = 0, packets = 0;
  bool finger = false;                       // oximetry DC present
  // session summary
  float spo2_sum = 0; uint32_t spo2_n = 0; float spo2_min = 101; float hr_sum = 0; uint32_t hr_n = 0; float hr_min = 999, hr_max = 0;
  uint32_t desat_count = 0, central_count = 0, pos_secs[6] = {0};   // x+ x- y+ y- z+ z-
  uint8_t trend[AN_TREND]; int trend_n = 0; float trend_acc = 0; int trend_cnt = 0; uint32_t trend_last_min = 0;   // SpO2 per minute (0 = unknown)
  uint8_t hr_trend[AN_TREND]; float hr_acc = 0; int hr_cnt = 0;                                                     // pulse per minute (0 = unknown)
  uint8_t pos_trend[AN_TREND]; uint16_t pos_cnt[6] = {0};                                                            // dominant position per minute (0 unknown, 1..6 = x+ x- y+ y- z+ z-)
  // state
  float spo2_score_ab = 0, spo2_score_ba = 0, spo2_ema = -1; float spo2_raw[10]; int spo2_raw_n = 0, spo2_raw_i = 0;
  float desat_base[30]; int desat_base_n = 0, desat_base_i = 0; bool desat_in = false; uint32_t desat_start = 0; float desat_nadir = 100;
  uint32_t resp_last = 0;
  // PAT events: the 3 s peak-to-peak PAT amplitude against the 75th percentile of the last 300 amplitudes seen outside events
  float pat_base[AN_PAT_BASE]; int pat_base_n = 0, pat_base_i = 0; bool pat_in = false; uint32_t pat_start = 0; float pat_hr_base = -1;
  float spo2_hist[AN_SPO2_HIST]; int spo2_hist_n = 0, spo2_hist_i = 0;      // the last valid SpO2 values, one per second with a signal
  uint32_t apnea_count = 0, hypopnea_count = 0, rera_count = 0, pat_only_count = 0;
  uint32_t pat_evt_t[AN_PAT_EVT]; int pat_evt_i = 0;                        // start times of the last PAT events (for the central check)
  float pat_ratio = -1;                                                     // live: amplitude as % of the baseline (-1 = unknown)
};

static int16_t an_pat_buf[AN_WAVE], an_oxa_buf[AN_WAVE], an_oxb_buf[AN_WAVE], an_chest_buf[AN_CHEST];
static int16_t an_ax_buf[AN_MOTION], an_ay_buf[AN_MOTION], an_az_buf[AN_MOTION];
static Ring an_pat = {an_pat_buf, AN_WAVE}, an_oxa = {an_oxa_buf, AN_WAVE}, an_oxb = {an_oxb_buf, AN_WAVE}, an_chest = {an_chest_buf, AN_CHEST};
static Ring an_ax = {an_ax_buf, AN_MOTION}, an_ay = {an_ay_buf, AN_MOTION}, an_az = {an_az_buf, AN_MOTION};
static Analysis an;
static float an_work[AN_CHEST], an_work2[AN_CHEST];   // scratch
static uint8_t an_seen;                                // bit per record id seen in the last packet (1 oxiA .. 6 motion)

static const char *an_severity(float pahi) { return pahi < 5 ? "normal" : pahi < 15 ? "mild" : pahi < 30 ? "moderate" : "severe"; }
static void an_reset() { an = Analysis(); an_pat.n = an_oxa.n = an_oxb.n = an_chest.n = an_ax.n = an_ay.n = an_az.n = 0; }

// ---- waveform decoders (watchpat_protocol.py) ----
static void an_decode_bytedelta(const uint8_t *p, size_t n, Ring &r) {
  if (n < 2) return;
  int acc = (int16_t)(p[0] | (p[1] << 8)); ring_push(r, acc);
  for (size_t i = 2; i < n; i++) { uint8_t b = p[i]; acc += (b >> 1) ^ -(int)(b & 1); ring_push(r, (int16_t)acc); }
}
static void an_decode_nibble(const uint8_t *p, size_t n, Ring &r) {
  if (n < 3) return;
  int acc = (int16_t)(p[0] | (p[1] << 8)); ring_push(r, acc);
  for (size_t i = 3; i < n; i++) {
    int lo = p[i] & 15, hi = p[i] >> 4; acc += lo >= 8 ? lo - 16 : lo; ring_push(r, (int16_t)acc);
    if (hi == 0 || hi == 7) ring_push(r, (int16_t)acc);
  }
}

// ---- helpers ----
static void an_boxmean(const float *a, float *out, int n, int win) {   // same output as np.convolve(a, ones/win, 'same'), running sum
  int pl = (win - 1) / 2, lo = -pl, hi = -pl + win - 1; double s = 0;
  for (int k = 0; k <= hi && k < n; k++) s += a[k];
  for (int i = 0; i < n; i++) {
    out[i] = s / win;
    if (lo >= 0 && lo < n) s -= a[lo];
    lo++; hi++;
    if (hi >= 0 && hi < n) s += a[hi];
  }
}
static float an_median(float *a, int n) {          // in place; small arrays end up sorted, large ones only partitioned
  if (!n) return 0;
  if (n <= 64) { for (int i = 1; i < n; i++) { float v = a[i]; int j = i; while (j > 0 && a[j - 1] > v) { a[j] = a[j - 1]; j--; } a[j] = v; } return a[n / 2]; }
  std::nth_element(a, a + n / 2, a + n); return a[n / 2];
}
static float an_percentile(float *a, int n, float pct) {
  if (!n) return 0; std::sort(a, a + n);
  float idx = pct / 100.0f * (n - 1); int lo = (int)idx; float f = idx - lo;
  return lo + 1 < n ? a[lo] + (a[lo + 1] - a[lo]) * f : a[lo];
}

// ---- pulse: peak detection on a 5 s window ----
static float an_heart_rate(const Ring &r) {
  static int16_t s[AN_WAVE]; int n = ring_tail(r, s, AN_WAVE);
  if (n < AN_RATE * 3) return -1;
  float *x = an_work, *base = an_work2;
  for (int i = 0; i < n; i++) x[i] = s[i];
  an_boxmean(x, base, n, AN_RATE * 3 / 4);
  for (int i = 0; i < n; i++) x[i] -= base[i];           // detrended
  for (int i = 0; i < n; i++) base[i] = fabsf(x[i]);
  float thr = an_median(base, n) * 1.5f; if (thr < 20) thr = 20;
  int refr = AN_RATE * 35 / 100, pw = AN_RATE * 15 / 100; float prom = thr * 1.2f;
  int peaks[64], np = 0, last = -refr;
  for (int i = 1; i < n - 1 && np < 64; i++) {
    if (!(x[i] > thr && x[i] > x[i - 1] && x[i] >= x[i + 1])) continue;
    if (i - last < refr) continue;
    float mn = x[i]; for (int k = (i - pw < 0 ? 0 : i - pw); k < i + pw + 1 && k < n; k++) if (x[k] < mn) mn = x[k];
    if (x[i] - mn >= prom) { peaks[np++] = i; last = i; }
  }
  if (np < 3) return -1;
  float bpms[64]; int nb = 0; float minI = AN_RATE * 60.0f / 140, maxI = AN_RATE * 60.0f / 40;
  for (int i = 1; i < np; i++) { int d = peaks[i] - peaks[i - 1]; if (d >= minI && d <= maxI) bpms[nb++] = 60.0f * AN_RATE / d; }
  if (nb < 2) return -1;
  return an_median(bpms, nb);
}

// ---- SpO2 from the AC/DC ratio of both oximetry channels at the pulse frequency (4 s window) ----
static void an_ac_pair(const float *a, const float *b, int n, float a_dc, float b_dc, float bpm, float &a_ac, float &b_ac) {
  float w = 2 * M_PI * (bpm / 60) / AN_RATE, cw = cosf(w), sw = sinf(w), c = 1, s = 0;   // one sinusoid, rotated by recurrence
  float ac = 0, as = 0, bc = 0, bs = 0;
  for (int i = 0; i < n; i++) {
    float da = a[i] - a_dc, db = b[i] - b_dc; ac += da * c; as += da * s; bc += db * c; bs += db * s;
    float c2 = c * cw - s * sw; s = s * cw + c * sw; c = c2;
  }
  a_ac = 2.0f / n * sqrtf(ac * ac + as * as); b_ac = 2.0f / n * sqrtf(bc * bc + bs * bs);
}
static void an_spo2_from(float a_ac, float a_dc, float b_ac, float b_dc, float &spo2, float &ratio) {
  spo2 = -1; ratio = -1;
  if (a_ac <= 0 || b_ac <= 0) return;
  if (a_ac / fabsf(a_dc) > 0.5f || b_ac / fabsf(b_dc) > 0.5f) return;
  ratio = fabsf((a_ac / a_dc) / (b_ac / b_dc));
  float v = 116.0f - 25.0f * ratio;
  if (v >= 60 && v <= 100) spo2 = v;
}
static float an_spo2(float hr) {
  static int16_t sa[AN_RATE * 4], sb[AN_RATE * 4];
  int n = AN_RATE * 4;
  if (an_oxa.n < n || an_oxb.n < n || hr <= 0) return -1;
  ring_tail(an_oxa, sa, n); ring_tail(an_oxb, sb, n);
  float *a = an_work, *b = an_work2; int32_t da = 0, db = 0;
  for (int i = 0; i < n; i++) { a[i] = sa[i]; b[i] = sb[i]; da += sa[i]; db += sb[i]; }
  float a_dc = da / (float)n, b_dc = db / (float)n;
  an.finger = fabsf(a_dc) > 10 && fabsf(b_dc) > 10;
  if (!an.finger) return -1;
  float dc_ratio = a_dc / b_dc;
  if (fabsf(dc_ratio - 1.0f) <= 0.08f) return -1;      // channels not distinct: no valid red/IR pair
  float s_ab, r_ab, s_ba, r_ba, a_ac, b_ac;
  an_ac_pair(a, b, n, a_dc, b_dc, hr, a_ac, b_ac);          // the AC amplitudes are the same for both channel orders
  an_spo2_from(a_ac, a_dc, b_ac, b_dc, s_ab, r_ab);
  an_spo2_from(b_ac, b_dc, a_ac, a_dc, s_ba, r_ba);
  an.spo2_score_ab *= 0.9f; an.spo2_score_ba *= 0.9f;
  if (s_ab > 0 && r_ab >= 0.4f && r_ab <= 1.3f) an.spo2_score_ab += 1.0f - fabsf(r_ab - 0.7f);
  if (s_ba > 0 && r_ba >= 0.4f && r_ba <= 1.3f) an.spo2_score_ba += 1.0f - fabsf(r_ba - 0.7f);
  if (an.spo2_score_ab >= an.spo2_score_ba) return s_ab > 0 ? s_ab : s_ba;
  return s_ba > 0 ? s_ba : s_ab;
}

// ---- respiration from chest zero crossings over 10 s ----
static float an_resp() {
  static int16_t s[AN_CHEST]; int n = ring_tail(an_chest, s, AN_CHEST);
  if (n < AN_RATE * 4) return -1;
  float *x = an_work, *base = an_work2;
  for (int i = 0; i < n; i++) x[i] = s[i];
  an_boxmean(x, base, n, AN_RATE / 2);
  for (int i = 0; i < n; i++) x[i] -= base[i];
  int cross = 0; for (int i = 1; i < n; i++) if ((x[i] >= 0) != (x[i - 1] >= 0)) cross++;
  float bpm = (cross / 2.0f) * 60.0f / (n / (float)AN_RATE);
  return (bpm < 4 || bpm > 40) ? -1 : bpm;
}

static const char *AN_POS_LABEL[6] = {"Upright", "Inverted", "Left", "Right", "Supine", "Prone"};
static const char *AN_POS_CODE[6] = {"x+", "x-", "y+", "y-", "z+", "z-"};

// per-second analysis step (rings already hold the new samples); an_feed() calls it after decoding a packet, the demo mode calls it directly
static uint8_t an_step(bool have_motion, int last_x, int last_y, int last_z, uint32_t now_ms);

// feed one DATA_PACKET payload (after the 24-byte header); returns the bit mask of record ids seen
static uint8_t an_feed(const uint8_t *pl, size_t n, uint32_t now_ms) {
  an_seen = 0;
  size_t pos = 3;
  int last_x = 0, last_y = 0, last_z = 0; bool have_motion = false;
  while (pos + 12 <= n) {
    if (pl[pos] != 0xAA || pl[pos + 1] != 0xAA) break;
    uint8_t id = pl[pos + 2]; uint16_t len = pl[pos + 4] | (pl[pos + 5] << 8);
    const uint8_t *p = pl + pos + 12; if (pos + 12 + len > n) break;
    if (id >= 1 && id <= 6) an_seen |= 1 << id;
    switch (id) {
      case 1: an_decode_bytedelta(p, len, an_oxa); break;
      case 2: an_decode_bytedelta(p, len, an_oxb); break;
      case 3: an_decode_bytedelta(p, len, an_pat); break;
      case 4: an_decode_nibble(p, len, an_chest); break;
      case 5: if (len >= 4) an.metric = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); break;
      case 6:
        for (size_t o = 0; o + 16 <= len; o += 16) {
          if (!(p[o] == 0xDD && p[o + 1] == 0xDD && p[o + 2] == 0xA3 && p[o + 3] == 0x57)) continue;
          last_x = (int16_t)(p[o + 8] | (p[o + 9] << 8)); last_y = (int16_t)(p[o + 10] | (p[o + 11] << 8)); last_z = (int16_t)(p[o + 12] | (p[o + 13] << 8));
          ring_push(an_ax, last_x); ring_push(an_ay, last_y); ring_push(an_az, last_z); have_motion = true;
        }
        break;
    }
    pos += 12 + len;
  }
  return an_step(have_motion, last_x, last_y, last_z, now_ms);
}

static uint8_t an_step(bool have_motion, int last_x, int last_y, int last_z, uint32_t now_ms) {
  an.packets++; an.seconds = an.packets;      // one packet = one second of data

  // pulse (PAT, then oximetry B, then A)
  float hr = an_heart_rate(an_pat); if (hr < 0) hr = an_heart_rate(an_oxb); if (hr < 0) hr = an_heart_rate(an_oxa);
  an.hr = hr;
  if (hr > 0) { an.hr_sum += hr; an.hr_n++; if (hr < an.hr_min) an.hr_min = hr; if (hr > an.hr_max) an.hr_max = hr; an.hr_acc += hr; an.hr_cnt++; }

  // SpO2: raw estimate -> trimmed median of the last 10 -> EMA
  float raw = an_spo2(hr);
  an.spo2_raw[an.spo2_raw_i] = raw; an.spo2_raw_i = (an.spo2_raw_i + 1) % 10; if (an.spo2_raw_n < 10) an.spo2_raw_n++;
  float valid[10]; int nv = 0; for (int i = 0; i < an.spo2_raw_n; i++) if (an.spo2_raw[i] > 0) valid[nv++] = an.spo2_raw[i];
  if (nv) {
    an_median(valid, nv); int trim = nv / 5; if (trim < 1) trim = 1;
    float med = nv > 4 ? valid[trim + (nv - 2 * trim) / 2] : valid[nv / 2];
    an.spo2_ema = an.spo2_ema < 0 ? med : an.spo2_ema + 0.15f * (med - an.spo2_ema);
    an.spo2 = an.spo2_ema;
    an.spo2_sum += an.spo2; an.spo2_n++; if (an.spo2 < an.spo2_min) an.spo2_min = an.spo2;
    // per-minute trend
    an.trend_acc += an.spo2; an.trend_cnt++;
  } else an.spo2 = -1;
  uint32_t minute = an.seconds / 60;
  if (minute != an.trend_last_min) {                 // one entry per minute in each trend, 0 where nothing was measured
    uint8_t v = an.trend_cnt ? (uint8_t)(an.trend_acc / an.trend_cnt + 0.5f) : 0;
    uint8_t h = an.hr_cnt ? (uint8_t)(an.hr_acc / an.hr_cnt + 0.5f) : 0;
    int pb = 0; for (int i = 1; i < 6; i++) if (an.pos_cnt[i] > an.pos_cnt[pb]) pb = i; uint8_t pv = an.pos_cnt[pb] ? pb + 1 : 0;
    if (an.trend_n < AN_TREND) { an.trend[an.trend_n] = v; an.hr_trend[an.trend_n] = h; an.pos_trend[an.trend_n] = pv; an.trend_n++; }
    else { memmove(an.trend, an.trend + 1, AN_TREND - 1); an.trend[AN_TREND - 1] = v; memmove(an.hr_trend, an.hr_trend + 1, AN_TREND - 1); an.hr_trend[AN_TREND - 1] = h; memmove(an.pos_trend, an.pos_trend + 1, AN_TREND - 1); an.pos_trend[AN_TREND - 1] = pv; }
    an.trend_acc = 0; an.trend_cnt = 0; an.hr_acc = 0; an.hr_cnt = 0; memset(an.pos_cnt, 0, sizeof an.pos_cnt); an.trend_last_min = minute;
  }

  // desaturation events: baseline = 90th percentile of the last 30 valid SpO2 values outside events
  if (an.spo2 > 0) {
    if (!an.desat_in) { an.desat_base[an.desat_base_i] = an.spo2; an.desat_base_i = (an.desat_base_i + 1) % 30; if (an.desat_base_n < 30) an.desat_base_n++; }
    if (an.desat_base_n >= 30) {
      float tmp[30]; memcpy(tmp, an.desat_base, sizeof tmp);
      float baseline = an_percentile(tmp, 30, 90), drop = baseline - an.spo2;
      if (!an.desat_in) { if (drop >= 3) { an.desat_in = true; an.desat_start = an.seconds; an.desat_nadir = an.spo2; } }
      else {
        if (an.spo2 < an.desat_nadir) an.desat_nadir = an.spo2;
        if (drop < 1) {
          if (an.seconds - an.desat_start >= 10) {
            an.desat_count++;
            bool near = an.pat_in; for (int i = 0; i < AN_PAT_EVT && !near; i++) { uint32_t t = an.pat_evt_t[i]; if (t && (t > an.desat_start ? t - an.desat_start : an.desat_start - t) <= 60) near = true; }
            if (!near) an.central_count++;
          }
          an.desat_in = false;
        }
      }
    }
  }

  if (an.spo2 > 0) { an.spo2_hist[an.spo2_hist_i] = an.spo2; an.spo2_hist_i = (an.spo2_hist_i + 1) % AN_SPO2_HIST; if (an.spo2_hist_n < AN_SPO2_HIST) an.spo2_hist_n++; }

  // PAT events (same rules as the PC analysis): amplitude down to 70 % of the baseline starts one, back above 80 % or 120 s ends it;
  // 10..120 s long -> apnea (SpO2 fell >= 4 %), hypopnea (>= 3 %), RERA (pulse rose >= 6 bpm), otherwise a PAT-only event
  if (an_pat.n >= AN_RATE * 3) {
    static int16_t pw[AN_RATE * 3]; int m = ring_tail(an_pat, pw, AN_RATE * 3); int16_t lo = pw[0], hi = pw[0];
    for (int i = 1; i < m; i++) { if (pw[i] < lo) lo = pw[i]; if (pw[i] > hi) hi = pw[i]; }
    float env = (float)hi - lo;
    if (env > 0) {
      if (!an.pat_in) { an.pat_base[an.pat_base_i] = env; an.pat_base_i = (an.pat_base_i + 1) % AN_PAT_BASE; if (an.pat_base_n < AN_PAT_BASE) an.pat_base_n++; }
      if (an.pat_base_n >= 30) {
        static float tmp[AN_PAT_BASE]; memcpy(tmp, an.pat_base, an.pat_base_n * sizeof(float));
        float base = an_percentile(tmp, an.pat_base_n, 75), ratio = base > 0 ? env / base : 1.0f; an.pat_ratio = ratio * 100;
        if (!an.pat_in) { if (ratio <= 0.70f) { an.pat_in = true; an.pat_start = an.seconds; an.pat_hr_base = an.hr; } }
        else if (ratio > 0.80f || an.seconds - an.pat_start > 120) {
          uint32_t dur = an.seconds - an.pat_start;
          if (dur >= 10 && dur <= 120) {
            float hr_rise = (an.pat_hr_base > 0 && an.hr > 0) ? an.hr - an.pat_hr_base : 0;
            int look = (int)dur + 5; if (look > an.spo2_hist_n) look = an.spo2_hist_n; float drop = 0;
            if (look >= 2) {
              float mx = -1, mn = 1000;
              for (int i = 0; i < look; i++) { float v = an.spo2_hist[(an.spo2_hist_i - 1 - i + AN_SPO2_HIST) % AN_SPO2_HIST]; if (v > mx) mx = v; if (v < mn) mn = v; }
              drop = mx - mn; if (drop < 0) drop = 0;
            }
            if (drop >= 4) an.apnea_count++; else if (drop >= 3) an.hypopnea_count++; else if (hr_rise >= 6) an.rera_count++; else an.pat_only_count++;
            an.pat_evt_t[an.pat_evt_i] = an.pat_start; an.pat_evt_i = (an.pat_evt_i + 1) % AN_PAT_EVT;
          }
          an.pat_in = false;
        }
      }
    }
  }

  // respiration every 5 s
  if (now_ms - an.resp_last >= 5000) { an.resp = an_resp(); an.resp_last = now_ms; }

  // motion level and body position
  if (have_motion) {
    static int16_t x[AN_MOTION], y[AN_MOTION], z[AN_MOTION]; int m = ring_tail(an_ax, x, AN_MOTION); ring_tail(an_ay, y, AN_MOTION); ring_tail(an_az, z, AN_MOTION);
    if (m >= 4) { double s = 0; for (int i = 1; i < m; i++) { float dx = x[i] - x[i - 1], dy = y[i] - y[i - 1], dz = z[i] - z[i - 1]; s += sqrtf(dx * dx + dy * dy + dz * dz); } an.motion = s / (m - 1); }
    int axes[6] = {last_x, -last_x, last_y, -last_y, last_z, -last_z}; int best = 0;
    for (int i = 1; i < 6; i++) if (axes[i] > axes[best]) best = i;
    strcpy(an.position, AN_POS_CODE[best]); an.position_label = AN_POS_LABEL[best]; an.pos_secs[best]++; an.pos_cnt[best]++;
  }
  return an_seen;
}
