// SleepCYD - firmware for the Cheap Yellow Display (ESP32-2432S028) that records a WatchPAT ONE sleep sensor over
// Bluetooth LE to the microSD card and turns the night into a report: on the screen, on a phone over Wi-Fi (web page +
// QR code) and, when configured, on the Nidra site.
//
// The code is one sketch plus a few headers:
//   watchpat.h   the WatchPAT ONE protocol (Nordic UART Service, packet framing, CRC, session commands)
//   analysis.h   the streaming analysis: pulse, SpO2, desaturations, PAT events (pAHI), breathing, position
//   nidra.h      the upload client of the Nidra ingest API (n8n webhook)
//   webpage.h    the report page served to the phone in night mode
//   records.h    the record the BLE task hands to the SD writer
// Three boot modes, chosen by flags in NVS: the recorder (Bluetooth on), night mode (analysis + web page, no Bluetooth)
// and Wi-Fi setup (network list + password keyboard, no Bluetooth). See docs/architecture.md.
// Serial 115200: `?` lists the commands (handle_line()).

#include <Arduino.h>
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <NimBLEDevice.h>
#include <esp_bt.h>
#include <esp_sleep.h>                            // hibernate: deep sleep, wake on the touch pen interrupt
#include <driver/gpio.h>                          // hold the backlight and amplifier pins while asleep
#include <time.h>
#include <sys/time.h>
#include "ff.h"                                   // f_mkfs for a real FAT format of the card
#include <CydLauncher.h>                          // boot splash, firmware ID in the image, slot switch (lib/CydLauncher)
#include "records.h"
#include "watchpat.h"
#include "analysis.h"

extern "C" bool btInUse() { return true; }     // keep the Bluetooth controller RAM: the core frees it otherwise

#define FW_VERSION "1.0"
CYD_FW_ID("SleepCYD", FW_VERSION);                // ID string the launcher finds in the flash image
#define SCREEN_W 320
#define SCREEN_H 240
#define TOP_H 34

// ---------------------------------------------------------------- pins
#define T_CLK 25
#define T_MISO 39
#define T_MOSI 32
#define T_CS 33
#define T_IRQ 36
#define SD_SCK 18
#define SD_MISO 19
#define SD_MOSI 23
#define SD_CS 5
#define BL_PIN 21                                  // display backlight (TFT_BL in config/TFT_eSPI_User_Setup.h): off while hibernating

// ---------------------------------------------------------------- colours
#define C_TOPBAR  lv_color_hex(0x12121A)
#define C_PANEL   lv_color_hex(0x1C1E28)
#define C_KEY     lv_color_hex(0x343848)
#define C_ACCENT  lv_color_hex(0x0096E6)
#define C_GREY    lv_color_hex(0x969BAA)
#define C_DIM     lv_color_hex(0x5A5E6E)
#define C_GREEN   lv_color_hex(0x3CC85A)
#define C_RED     lv_color_hex(0xE63C46)
#define C_AMBER   lv_color_hex(0xE6A020)

// ---------------------------------------------------------------- display
TFT_eSPI tft;
#define DRAW_BUF_SIZE (SCREEN_W * 12 * 2)   // 7.5 KB partial render buffer from the heap
static uint8_t *draw_buf;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  uint32_t w = lv_area_get_width(area), h = lv_area_get_height(area);
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)px_map, w * h, true);
  tft.endWrite();
  lv_display_flush_ready(disp);
}
static uint32_t tick_cb(void) { return millis(); }

// ---------------------------------------------------------------- touch (XPT2046, software SPI)
// The hardware VSPI bus is used by the SD card, HSPI by the display; the touch controller is slow, so bit-banging is fine.
// full-duplex bit-bang transfer, SPI mode 0 (data changes on the falling edge, sampled on the rising edge), MSB first
static uint16_t xpt_xfer(uint16_t out, int bits) {
  uint16_t in = 0;
  for (int i = bits - 1; i >= 0; i--) {
    digitalWrite(T_MOSI, (out >> i) & 1); delayMicroseconds(2);
    digitalWrite(T_CLK, HIGH); delayMicroseconds(2);
    in = (in << 1) | digitalRead(T_MISO);
    digitalWrite(T_CLK, LOW);
  }
  return in;
}
static void xpt_init() {
  pinMode(T_CLK, OUTPUT); pinMode(T_MOSI, OUTPUT); pinMode(T_MISO, INPUT); pinMode(T_CS, OUTPUT); pinMode(T_IRQ, INPUT);
  digitalWrite(T_CLK, LOW); digitalWrite(T_CS, HIGH);
}
// same command sequence as the XPT2046_Touchscreen library: each 16-bit transfer returns the previous conversion
static bool xpt_read(int &x, int &y, int &z) {
  digitalWrite(T_CS, LOW); delayMicroseconds(4);
  xpt_xfer(0xB1, 8);
  int z1 = xpt_xfer(0xC1, 16) >> 3;
  z = z1 + 4095;
  int z2 = xpt_xfer(0x91, 16) >> 3;
  z -= z2;
  bool down = z >= 120;
  if (down) {
    xpt_xfer(0x91, 16);                           // first X is noisy
    int d0 = xpt_xfer(0xD1, 16) >> 3, d1 = xpt_xfer(0x91, 16) >> 3, d2 = xpt_xfer(0xD1, 16) >> 3, d3 = xpt_xfer(0x91, 16) >> 3;
    auto med = [](int a, int b, int c) { return max(min(a, b), min(max(a, b), c)); };
    x = med(d0, d2, d2); y = med(d1, d3, d3);     // d0,d2 are Y samples; d1,d3 are X samples (library swaps the axes for rotation 1)
    int ysamp = med(d0, d2, d2), xsamp = med(d1, d3, d3);
    x = xsamp; y = ysamp;
  }
  int last = xpt_xfer(0xD0, 16) >> 3; (void)last;  // last Y with power down: re-arms the pen interrupt
  xpt_xfer(0, 16);
  digitalWrite(T_CS, HIGH);
  return down;
}
// speaker: amp input GPIO 26, amp enable GPIO 4 (LOW = on)
#define SPK_PIN 26
#define AMP_EN_PIN 4
static void spk_init() { pinMode(AMP_EN_PIN, OUTPUT); digitalWrite(AMP_EN_PIN, HIGH); ledcAttach(SPK_PIN, 1000, 10); ledcWrite(SPK_PIN, 0); }
static void spk_tone(uint16_t hz) {
  if (!hz) { ledcWrite(SPK_PIN, 0); digitalWrite(AMP_EN_PIN, HIGH); return; }
  digitalWrite(AMP_EN_PIN, LOW); ledcChangeFrequency(SPK_PIN, hz, 10); ledcWrite(SPK_PIN, 180);
}
static void boot_beep() { spk_tone(1200); delay(80); spk_tone(1800); delay(90); spk_tone(0); }
static void found_beep() { spk_tone(1300); delay(70); spk_tone(0); delay(40); spk_tone(1300); delay(70); spk_tone(0); delay(40); spk_tone(2000); delay(140); spk_tone(0); }
static bool touch_pressed_raw() { int x, y, z; return xpt_read(x, y, z) && z < 3800 && x > 150 && x < 3950 && y > 150 && y < 3950; }

// calibration: raw values that map to screen x=20/300 and y=20/220 (overridden from NVS)
static int cal_x0 = 419, cal_x1 = 3481, cal_y0 = 475, cal_y1 = 3225; static bool cal_swap = false, cal_have = false;
static bool calibrating = false; static volatile bool cal_pressed = false; static volatile int cal_raw_x = 0, cal_raw_y = 0;
static int fake_x = 0, fake_y = 0; static uint32_t fake_until = 0;   // serial tap injection "P x,y"

static uint32_t hib_last_touch = 0;                    // millis() of the last touch: the hibernate countdown (Settings > Device, hib_poll)

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data) {
  if (fake_until && (int32_t)(fake_until - millis()) > 0) { data->state = LV_INDEV_STATE_PRESSED; data->point.x = fake_x; data->point.y = fake_y; return; }
  fake_until = 0;
  static bool was_pressed = false;
  static int st_n = 0, st_x = 0, st_y = 0;              // consecutive consistent samples before a press counts
  int rx, ry, rz;
  if (millis() > 2000 && xpt_read(rx, ry, rz) && rz < 3800 && rx > 150 && rx < 3950 && ry > 150 && ry < 3950) {
    if (!was_pressed) {                                 // a single random sample (Wi-Fi glitch on the pin 39 input) is not a touch
      if (st_n && abs(rx - st_x) < 200 && abs(ry - st_y) < 200) st_n++; else st_n = 1;
      st_x = rx; st_y = ry;
      if (st_n < 3) { data->state = LV_INDEV_STATE_RELEASED; if (calibrating) cal_pressed = false; return; }
    }
    if (cal_swap) { int t = rx; rx = ry; ry = t; }
    int x = 20 + (rx - cal_x0) * 280 / (cal_x1 - cal_x0), y = 20 + (ry - cal_y0) * 200 / (cal_y1 - cal_y0);
    x = constrain(x, 0, SCREEN_W - 1); y = constrain(y, 0, SCREEN_H - 1);
    if (!was_pressed) { Serial.printf("touch raw %d,%d z %d -> %d,%d\n", rx, ry, rz, x, y); was_pressed = true; }
    if (calibrating) { cal_raw_x = cal_swap ? ry : rx; cal_raw_y = cal_swap ? rx : ry; cal_pressed = true; data->state = LV_INDEV_STATE_RELEASED; return; }
    hib_last_touch = millis();
    data->point.x = x; data->point.y = y; data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED; was_pressed = false; st_n = 0;
    if (calibrating) cal_pressed = false;
  }
}

// ---------------------------------------------------------------- settings (NVS)
Preferences prefs;
static String pend_path, pend_rec; static bool pend_skip = false, pend_sent = true;   // night marked for night mode (report / upload / web page at the next boot)
static String cfg_tz = "EST5EDT,M3.2.0,M11.1.0";   // POSIX time zone for the date in file names (serial: y<tz>)
static uint32_t cfg_serial = 0;           // WatchPAT serial of the last session: names re-uploads after a reboot
static String cfg_filter = "ITAMAR";     // substring of the advertised name: the WatchPAT ONE advertises as ITAMAR_<serial hex>N
static String cfg_last_addr;             // last device we connected to: reconnect to it first
static int cfg_session = 0;              // last session number used
static String cfg_dob;                    // date of birth YYYY-MM-DD
static bool cfg_demo = false;             // Settings > Device > Demo mode: a simulated night instead of a WatchPAT
static bool cfg_hib = false;              // Settings > Device > Hibernate: deep sleep after 30 s without a touch while idle; a touch wakes the board
static String cfg_ssid, cfg_wpass, cfg_api, cfg_tok, cfg_patient;   // Wi-Fi, the Nidra API (serial w, u, t), the patient (letters and spaces, 12 characters at most)
static void nv_put(const char *key, const String &v) { prefs.begin("sleep", false); prefs.putString(key, v); prefs.end(); }
static void nv_put(const char *key, bool v) { prefs.begin("sleep", false); prefs.putBool(key, v); prefs.end(); }
static void nv_put(const char *key, uint32_t v) { prefs.begin("sleep", false); prefs.putULong(key, v); prefs.end(); }
static void nv_remove(const char *key) { prefs.begin("sleep", false); prefs.remove(key); prefs.end(); }
static void cfg_load() {
  prefs.begin("sleep", true);
  cfg_filter = prefs.getString("filter", "ITAMAR"); cfg_last_addr = prefs.getString("last", ""); cfg_session = prefs.getInt("sess", 0);
  cfg_ssid = prefs.getString("ssid", ""); cfg_wpass = prefs.getString("wpass", ""); cfg_api = prefs.getString("api", ""); cfg_tok = prefs.getString("tok", ""); cfg_patient = prefs.getString("patient", ""); cfg_dob = prefs.isKey("dob") ? prefs.getString("dob", "") : ""; cfg_demo = prefs.getBool("demo", false); cfg_hib = prefs.getBool("hib", false); cfg_serial = prefs.getULong("serial", 0); if (prefs.isKey("tz")) cfg_tz = prefs.getString("tz", cfg_tz); pend_path = prefs.isKey("pend") ? prefs.getString("pend", "") : ""; pend_skip = prefs.getBool("pskip", false); pend_sent = prefs.getBool("psent", true); pend_rec = prefs.isKey("prec") ? prefs.getString("prec", "") : "";
  cal_have = prefs.isKey("cx0"); cal_swap = prefs.getBool("cswap", false);
  cal_x0 = prefs.getInt("cx0", cal_x0); cal_x1 = prefs.getInt("cx1", cal_x1); cal_y0 = prefs.getInt("cy0", cal_y0); cal_y1 = prefs.getInt("cy1", cal_y1);
  prefs.end();
  if (!cfg_ssid.length()) {                           // reuse the Wi-Fi saved by the display firmware on this board, if any
    prefs.begin("cfg", true); cfg_ssid = prefs.getString("ssid", ""); cfg_wpass = prefs.getString("pass", ""); prefs.end();
  }
}
static void cfg_save() {
  prefs.begin("sleep", false);
  prefs.putString("filter", cfg_filter); prefs.putString("last", cfg_last_addr); prefs.putInt("sess", cfg_session);
  prefs.putString("ssid", cfg_ssid); prefs.putString("wpass", cfg_wpass); prefs.putString("api", cfg_api); prefs.putString("tok", cfg_tok); prefs.putString("patient", cfg_patient); prefs.putString("dob", cfg_dob);
  prefs.putBool("cswap", cal_swap); prefs.putInt("cx0", cal_x0); prefs.putInt("cx1", cal_x1); prefs.putInt("cy0", cal_y0); prefs.putInt("cy1", cal_y1);
  prefs.end();
}

// ---------------------------------------------------------------- UI helpers
static void flat(lv_obj_t *o, lv_color_t bg) {
  lv_obj_set_style_bg_color(o, bg, 0); lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(o, 0, 0); lv_obj_set_style_radius(o, 0, 0);
  lv_obj_set_style_pad_all(o, 0, 0); lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}
static lv_obj_t *label(lv_obj_t *parent, const char *txt, const lv_font_t *font, lv_color_t color, int x, int y) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, txt); lv_obj_set_style_text_font(l, font, 0); lv_obj_set_style_text_color(l, color, 0);
  lv_obj_set_pos(l, x, y);
  return l;
}
static lv_obj_t *button(lv_obj_t *parent, const char *txt, int x, int y, int w, int h, lv_color_t bg, lv_event_cb_t cb, void *ud) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_set_pos(b, x, y); lv_obj_set_size(b, w, h);
  lv_obj_set_style_bg_color(b, bg, 0); lv_obj_set_style_radius(b, 4, 0); lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_bg_color(b, lv_color_white(), LV_STATE_PRESSED); lv_obj_set_style_text_color(b, lv_color_black(), LV_STATE_PRESSED);
  lv_obj_set_style_border_width(b, 2, LV_STATE_PRESSED); lv_obj_set_style_border_color(b, C_ACCENT, LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(b, C_DIM, LV_STATE_DISABLED); lv_obj_set_style_text_color(b, C_GREY, LV_STATE_DISABLED);
  lv_obj_t *l = lv_label_create(b); lv_label_set_text(l, txt); lv_obj_center(l);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
  lv_obj_set_ext_click_area(b, 10);
  return b;
}

static lv_obj_t *bigbutton(lv_obj_t *parent, const char *txt, int x, int y, int w, int h, lv_color_t bg, lv_event_cb_t cb, void *ud) {
  lv_obj_t *b = button(parent, txt, x, y, w, h, bg, cb, ud);
  lv_obj_set_style_radius(b, 8, 0); lv_obj_set_style_text_font(lv_obj_get_child(b, 0), &lv_font_montserrat_20, 0);
  return b;
}
static void title_tap_cb(lv_event_t *e);
static lv_obj_t *scr_home, *scr_settings = NULL;
static lv_obj_t *scr_welcome = NULL, *scr_turnon = NULL, *lbl_to_state = NULL;   // patient choice at boot; "turn on the WatchPAT" (scanning)
static lv_obj_t *lbl_hib = NULL; static void hib_text();   // the hibernate countdown in the top bar of the patient screen
static void settings_cb(lv_event_t *e); static void welcome_show(); static void welcome_leave(); static void turnon_show();
static void end_prompt(const char *why, int auto_s); static void gear(lv_obj_t *scr); static void drop_screen(lv_obj_t *s); static void sim_start(); static void demo_start();
#define DEMO_PATH "/DEMO.DAT"                     // a recorded night at the card root (outside /SLEEP: not listed, not reset); see README "Demo mode"
static lv_obj_t *lbl_state, *lbl_stats, *lbl_sd, *lbl_log, *list_devs, *btn_scan, *btn_disc, *lbl_top_right, *btn_up, *btn_down;
static void scroll_cb(lv_event_t *e) { lv_obj_scroll_by(list_devs, 0, -80 * (int)(intptr_t)lv_event_get_user_data(e), LV_ANIM_ON); }
static lv_obj_t *lbl_set_info, *btn_format, *lbl_format;
static uint32_t format_armed_until = 0;

// on-screen log: last 7 lines
static char loglines[7][48]; static int log_n = 0; static bool log_dirty = false;
static void ulog(const char *fmt, ...) {
  char buf[200]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  Serial.println(buf);
  strncpy(loglines[log_n % 7], buf, 47); loglines[log_n % 7][47] = 0; log_n++; log_dirty = true;
}
static void log_refresh() {
  if (!lbl_log || !log_dirty) return;
  char all[7 * 49 + 1]; all[0] = 0;
  for (int i = (log_n > 7 ? log_n - 7 : 0); i < log_n; i++) { strcat(all, loglines[i % 7]); strcat(all, "\n"); }
  lv_label_set_text(lbl_log, all); log_dirty = false;
}

// ---------------------------------------------------------------- SD card
SPIClass sdSpi(VSPI);
static bool sd_ok = false;
static uint64_t sd_total = 0, sd_used = 0;
static void sd_stats() { if (sd_ok) { sd_total = SD.totalBytes(); sd_used = SD.usedBytes(); } }
static bool sd_begin() {
  sd_ok = SD.begin(SD_CS, sdSpi, 20000000, "/sd", 8, true);   // formats the card if it carries no file system
  if (!sd_ok) { ulog("sd: no card / mount failed"); return false; }
  SD.mkdir("/SLEEP");
  sd_stats();
  ulog("sd: %s %.1f GB, %.1f MB used", SD.cardType() == CARD_SDHC ? "SDHC" : SD.cardType() == CARD_SD ? "SD" : "card",
       sd_total / 1e9, sd_used / 1e6);
  return true;
}
static bool sd_format() {
  // f_mkfs rewrites the FAT on the mounted drive (drive 0 = the only FatFs volume); then remount
  ulog("sd: formatting...");
  if (sd_ok) { } else if (!SD.begin(SD_CS, sdSpi, 20000000, "/sd", 8, false)) { ulog("sd: no card to format"); return false; }
  BYTE *work = (BYTE *)malloc(FF_MAX_SS);
  if (!work) return false;
  const MKFS_PARM opt = {(BYTE)FM_ANY, 0, 0, 0, 0};
  FRESULT r = f_mkfs("0:", &opt, work, FF_MAX_SS);
  free(work);
  SD.end(); sd_ok = false;
  if (r != FR_OK) { ulog("sd: f_mkfs failed (%d)", (int)r); sd_begin(); return false; }
  bool ok = sd_begin();
  if (ok) { ulog("sd: formatted"); cfg_session = 0; cfg_save(); }
  return ok;
}

// RECORD FORMAT (sNNNN.dat): header "SLPC" u8 ver=1, u8 pad[3], u32 epoch (0 if clock unknown), u32 boot ms at open.
// Then records: u32 ms since boot, u16 attribute handle, u16 len, u8 kind (1 notify, 2 indicate, 3 read), u8 pad, data[len].
static QueueHandle_t rec_q;
static File sess_dat, sess_txt;
static int sess_no = 0; static uint32_t sess_pkts = 0, sess_bytes = 0, sess_last_flush = 0;
static volatile uint32_t rx_pkts = 0, rx_bytes = 0, rx_dropped = 0;

static bool session_open(const char *devname, const char *addr) {
  if (!sd_ok) return false;
  sess_no = ++cfg_session; cfg_save();
  char path[32];
  snprintf(path, sizeof path, "/SLEEP/s%04d.dat", sess_no);
  sess_dat = SD.open(path, FILE_WRITE);
  snprintf(path, sizeof path, "/SLEEP/s%04d.txt", sess_no);
  sess_txt = SD.open(path, FILE_WRITE);
  if (!sess_dat || !sess_txt) { ulog("sd: cannot create session files"); return false; }
  time_t now = time(nullptr); uint32_t epoch = now > 1600000000 ? (uint32_t)now : 0;
  uint8_t hdr[16] = {'S', 'L', 'P', 'C', 1, 0, 0, 0};
  uint32_t ms = millis(); memcpy(hdr + 8, &epoch, 4); memcpy(hdr + 12, &ms, 4);
  sess_dat.write(hdr, 16);
  sess_txt.printf("SleepCYD session %d\ndevice %s\naddress %s\nboot_ms %lu\nepoch %lu\n", sess_no, devname, addr, (unsigned long)ms, (unsigned long)epoch);
  File idx = SD.open("/SLEEP/index.txt", FILE_APPEND);
  if (idx) { idx.printf("s%04d %s %s epoch %lu\n", sess_no, devname, addr, (unsigned long)epoch); idx.close(); }
  sess_pkts = sess_bytes = 0; sess_last_flush = millis();
  ulog("session s%04d started", sess_no);
  return true;
}
static void session_close() {
  if (sess_dat) { sess_dat.close(); }
  if (sess_txt) { sess_txt.printf("end_ms %lu packets %lu bytes %lu\n", (unsigned long)millis(), (unsigned long)sess_pkts, (unsigned long)sess_bytes); sess_txt.close(); }
  if (sess_no) ulog("session s%04d closed: %lu pkts %lu B", sess_no, (unsigned long)sess_pkts, (unsigned long)sess_bytes);
  sess_no = 0; sd_stats();
}
static void session_write(const Rec &r) {
  if (!sess_dat) return;
  sess_dat.write((const uint8_t *)&r, 10 + r.len);
  sess_pkts++; sess_bytes += r.len;
  if (millis() - sess_last_flush > 1000) { sess_dat.flush(); sess_last_flush = millis(); }
}
static void rec_push(uint16_t handle, uint8_t kind, const uint8_t *d, size_t n) {
  static Rec r;                                  // only used from the NimBLE host task
  r.ms = millis(); r.handle = handle; r.kind = kind; r.pad = 0; r.len = n > 512 ? 512 : n;
  memcpy(r.data, d, r.len);
  if (xQueueSend(rec_q, &r, 0) != pdTRUE) rx_dropped = rx_dropped + 1;
  rx_pkts = rx_pkts + 1; rx_bytes += r.len;
}

// ---------------------------------------------------------------- Bluetooth
struct BleDev { char name[28]; char addr[18]; int rssi; NimBLEAddress address; };
static BleDev ble_devs[24]; static volatile int ble_n = 0;
enum State { ST_IDLE, ST_SCANNING, ST_CONNECTING, ST_CONNECTED, ST_WAIT, ST_FOUND };   // ST_FOUND: "WatchPAT found" dialog is up
static volatile State state = ST_IDLE;
static volatile bool scan_done = false, disc_flag = false;
static volatile int auto_target = -1; static int want_connect = -1;
static uint32_t state_t0 = 0; static bool auto_mode = true;    // auto: keep scanning for the filter name and reconnect
static NimBLEClient *client = NULL; static int conn_idx = -1;
static char conn_name[28] = "", conn_addr[18] = "";
static int n_subs = 0;

// WatchPAT ONE names are ITAMAR_<serial in hex>N; returns the decimal serial or "" if the name has no hex block
static String watchpat_serial(const char *name) {
  const char *u = strchr(name, '_'); if (!u) return "";
  char *end; unsigned long v = strtoul(u + 1, &end, 16);
  if (end - (u + 1) < 6) return "";
  return String(v);
}
static bool name_matches(const char *name) {
  if (!cfg_filter.length() || !name[0]) return false;
  String a = name, b = cfg_filter; a.toLowerCase(); b.toLowerCase();
  return a.indexOf(b) >= 0;
}
class ScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *dev) override {
    std::string addr = dev->getAddress().toString();
    int i;
    for (i = 0; i < ble_n; i++) if (!strcmp(ble_devs[i].addr, addr.c_str())) break;
    if (i == ble_n) {
      if (ble_n >= 24) return;
      BleDev &d = ble_devs[ble_n];
      strncpy(d.addr, addr.c_str(), 17); d.addr[17] = 0; d.address = dev->getAddress(); d.name[0] = 0; ble_n = ble_n + 1;
    }
    BleDev &d = ble_devs[i];
    d.rssi = dev->getRSSI();
    if (dev->haveName() && !d.name[0]) { strncpy(d.name, dev->getName().c_str(), 27); d.name[27] = 0; Serial.printf("ble: %s '%s' %d dBm\n", d.addr, d.name, d.rssi); }
    if (auto_mode && auto_target < 0 && (name_matches(d.name) || (cfg_last_addr.length() && cfg_last_addr == d.addr))) {
      auto_target = i; NimBLEDevice::getScan()->stop();
    }
  }
  void onScanEnd(const NimBLEScanResults &results, int reason) override { scan_done = true; }
};
static ScanCB scan_cb;
class ClientCB : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient *c, int reason) override { disc_flag = true; Serial.printf("ble: disconnected, reason %d\n", reason); }
  void onPassKeyEntry(NimBLEConnInfo &info) override { Serial.println("ble: device asks for a passkey, sending 000000"); NimBLEDevice::injectPassKey(info, 0); }
  void onConfirmPasskey(NimBLEConnInfo &info, uint32_t pin) override { Serial.printf("ble: confirm passkey %06lu\n", (unsigned long)pin); NimBLEDevice::injectConfirmPasskey(info, true); }
  void onAuthenticationComplete(NimBLEConnInfo &info) override {
    Serial.printf("ble: auth complete, encrypted %d bonded %d\n", info.isEncrypted(), info.isBonded());
    if (sess_txt) sess_txt.printf("auth encrypted %d bonded %d\n", info.isEncrypted(), info.isBonded());
  }
  void onMTUChange(NimBLEClient *c, uint16_t mtu) override { Serial.printf("ble: mtu %u\n", mtu); }
};
static ClientCB client_cb;

static void ble_init() {
  NimBLEDevice::init("SleepCYD");
  NimBLEDevice::setSecurityAuth(true, false, true);              // bond, no MITM, LE secure connections ("just works")
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::setMTU(517);
  NimBLEScan *s = NimBLEDevice::getScan();
  s->setScanCallbacks(&scan_cb, false); s->setActiveScan(true); s->setInterval(100); s->setWindow(99);
  ulog("ble: ready, filter '%s'", cfg_filter.c_str());
}
static void ui_state(const char *txt, lv_color_t c);
static void ui_devlist();
static lv_obj_t *dlg = NULL, *lbl_dlg_timer; static int found_idx = -1; static uint32_t found_t0 = 0;
static void dlg_close() { if (dlg) { lv_obj_delete(dlg); dlg = NULL; } }
static void dlg_connect_cb(lv_event_t *e) { dlg_close(); want_connect = found_idx; }
static void dlg_cancel_cb(lv_event_t *e) {
  dlg_close(); auto_mode = false; state = ST_IDLE; ulog("found dialog: cancel, manual list");
  if (scr_home) lv_screen_load(scr_home); ui_devlist(); ui_state("Tap a device to connect (list scrolls)", C_GREY);
}
static void show_found(int idx) {
  found_idx = idx; found_t0 = millis(); state = ST_FOUND;
  BleDev &d = ble_devs[idx];
  String serial = watchpat_serial(d.name);
  ulog("found %s serial %s", d.name, serial.length() ? serial.c_str() : "?");
  dlg_close();
  dlg = lv_obj_create(lv_screen_active()); lv_obj_set_size(dlg, 290, 160); lv_obj_center(dlg);
  lv_obj_set_style_bg_color(dlg, C_PANEL, 0); lv_obj_set_style_border_color(dlg, C_ACCENT, 0); lv_obj_set_style_border_width(dlg, 2, 0);
  lv_obj_set_style_radius(dlg, 6, 0); lv_obj_set_style_pad_all(dlg, 10, 0); lv_obj_remove_flag(dlg, LV_OBJ_FLAG_SCROLLABLE);
  label(dlg, "WatchPAT found", &lv_font_montserrat_20, C_GREEN, 0, 0);
  lv_obj_t *l = label(dlg, "", &lv_font_montserrat_14, lv_color_white(), 0, 30);
  if (serial.length()) lv_label_set_text_fmt(l, "Serial %s", serial.c_str()); else lv_label_set_text(l, d.name);
  lv_obj_t *l2 = label(dlg, "", &lv_font_montserrat_12, C_GREY, 0, 50);
  lv_label_set_text_fmt(l2, "%s   %s   %d dBm", d.name, d.addr, d.rssi);
  lbl_dlg_timer = label(dlg, "Connecting automatically in 30 s", &lv_font_montserrat_12, C_DIM, 0, 68);
  button(dlg, "Connect", 0, 92, 130, 42, C_ACCENT, dlg_connect_cb, NULL);
  button(dlg, "Cancel", 140, 92, 128, 42, C_KEY, dlg_cancel_cb, NULL);
  ui_state(d.name, C_GREEN);
  found_beep();
}
static bool sim_active = false;
static void scan_start() {
  if (sim_active) return;                                     // demo mode: no Bluetooth activity
  if (state == ST_SCANNING || state == ST_CONNECTED || state == ST_CONNECTING) return;
  ble_n = 0; scan_done = false; auto_target = -1; state = ST_SCANNING; state_t0 = millis();
  if (!NimBLEDevice::getScan()->start(6000, false, true)) { ulog("ble: scan failed to start"); state = ST_WAIT; state_t0 = millis(); return; }
  ui_state(auto_mode ? "Scanning for WatchPAT..." : "Scanning 6 s...", C_ACCENT);
  if (!auto_mode) { lv_obj_clean(list_devs); lv_obj_t *l = lv_list_add_text(list_devs, "Scanning..."); lv_obj_set_style_text_color(l, C_DIM, 0); lv_obj_remove_flag(list_devs, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(lbl_log, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(btn_up, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(btn_down, LV_OBJ_FLAG_HIDDEN); }
}

static void notify_cb(NimBLERemoteCharacteristic *c, uint8_t *d, size_t n, bool is_notify) { rec_push(c->getHandle(), is_notify ? 1 : 2, d, n); }

static void ble_disconnect() {
  wp_end(true);
  if (client) { if (client->isConnected()) client->disconnect(); NimBLEDevice::deleteClient(client); client = NULL; }
  conn_idx = -1; disc_flag = false; n_subs = 0;
  session_close();
}
// connect, write the attribute table to the session, read every readable characteristic once, subscribe to every notify/indicate
static void ble_connect(int idx) {
  if (idx < 0 || idx >= ble_n) return;
  BleDev d = ble_devs[idx];
  state = ST_CONNECTING;
  ui_state(d.name[0] ? d.name : d.addr, C_AMBER); lv_label_set_text_fmt(lbl_stats, "Connecting..."); lv_refr_now(NULL);
  ulog("ble: connecting %s '%s'", d.addr, d.name);
  client = NimBLEDevice::createClient();
  client->setClientCallbacks(&client_cb, false);
  client->setConnectTimeout(8000);
  disc_flag = false;
  if (!client->connect(d.address, true, false, false)) {   // no client MTU exchange: the WatchPAT initiates it
    ulog("ble: connect failed"); NimBLEDevice::deleteClient(client); client = NULL;
    state = ST_WAIT; state_t0 = millis(); return;
  }
  conn_idx = idx; strcpy(conn_name, d.name); strcpy(conn_addr, d.addr);
  if (name_matches(d.name)) { cfg_last_addr = d.addr; cfg_save(); }   // only remember WatchPAT devices
  ulog("ble: connected, rssi %d mtu %u", client->getRssi(), client->getMTU());
  state = ST_CONNECTED; ui_state(d.name[0] ? d.name : d.addr, C_GREEN);
  session_open(d.name[0] ? d.name : "(no name)", d.addr);
  const std::vector<NimBLERemoteService *> &svcs = client->getServices(true);
  int n_chars = 0; n_subs = 0;
  for (NimBLERemoteService *svc : svcs) {
    std::string su = svc->getUUID().toString();
    if (sess_txt) sess_txt.printf("service %s\n", su.c_str());
    Serial.printf("  service %s\n", su.c_str());
    for (NimBLERemoteCharacteristic *ch : svc->getCharacteristics(true)) {
      std::string cu = ch->getUUID().toString();
      char props[16] = ""; if (ch->canRead()) strcat(props, "R"); if (ch->canWrite()) strcat(props, "W"); if (ch->canNotify()) strcat(props, "N"); if (ch->canIndicate()) strcat(props, "I");
      if (sess_txt) sess_txt.printf("  char %04x %s %s\n", ch->getHandle(), cu.c_str(), props);
      Serial.printf("    char %04x %s %s\n", ch->getHandle(), cu.c_str(), props);
      n_chars++;
      if (ch->canRead()) {
        NimBLEAttValue v = ch->readValue();
        if (v.size()) {
          rec_push(ch->getHandle(), 3, v.data(), v.size());
          if (sess_txt) { sess_txt.printf("    value "); for (size_t i = 0; i < v.size() && i < 64; i++) sess_txt.printf("%02x", v[i]); sess_txt.printf("\n"); }
        }
      }
      if (ch->canNotify() || ch->canIndicate()) {
        if (ch->subscribe(ch->canNotify(), notify_cb, true)) n_subs++;
        else Serial.printf("    subscribe %04x failed\n", ch->getHandle());
      }
      if (disc_flag) break;
    }
    if (disc_flag) break;
  }
  if (sess_txt) { sess_txt.printf("characteristics %d subscribed %d\n", n_chars, n_subs); sess_txt.flush(); }
  ulog("ble: %d services, %d chars, %d subscribed", (int)svcs.size(), n_chars, n_subs);
  if (!n_subs) ulog("nothing to subscribe to: device may need its app to start streaming");
  NimBLERemoteService *nus = client->getService(WP_NUS_SVC);
  if (nus) wp_begin(nus);
}

// ---------------------------------------------------------------- ready, recording (dashboard) and report screens
static lv_obj_t *scr_ready = NULL, *scr_dash = NULL, *scr_report = NULL;
static lv_obj_t *lbl_ready_dev, *lbl_ready_msg, *btn_start;
static lv_obj_t *lbl_d_saved, *lbl_d_time, *lbl_d_pos, *lbl_d_hint, *btn_d_stop, *lbl_report;
static uint32_t dot_t[6] = {0};
static char report_text[800] = "";
static void disc_btn_cb(lv_event_t *e);
static void sim_finish();
static void wp_user_start();
static void wp_user_end();
static int wp_get_state(); static uint32_t wp_get_last(); static uint32_t wp_get_bytes();   // driver state lives below
static void to_home_cb(lv_event_t *e) { if (scr_home) lv_screen_load(scr_home); else ESP.restart(); }
static void topbar(lv_obj_t *scr, const char *title);
static lv_obj_t *scr_date = NULL, *lbl_dt[5], *lbl_dt_note; static int dt_v[5]; static int dt_mode = 0;   // 0 clock (Settings), 1 clock (before Start), 2 date of birth
static void (*dt_done)(bool) = NULL;                 // where to go afterwards; NULL: Settings, or Start sleep in mode 1
static const int DT_MAX[5] = {2099, 12, 31, 23, 59};
static int dt_min(int i) { return i == 0 ? (dt_mode == 2 ? 1900 : 2025) : i < 3 ? 1 : 0; }
static int dt_cols() { return dt_mode == 2 ? 3 : 5; }
static bool have_date() { return time(NULL) > 1700000000; }
static void dt_show() { for (int i = 0; i < dt_cols(); i++) lv_label_set_text_fmt(lbl_dt[i], i == 0 ? "%04d" : "%02d", dt_v[i]); }
static void dt_adj_cb(lv_event_t *e) {
  int k = (int)(intptr_t)lv_event_get_user_data(e), i = k / 2, d = (k & 1) ? -1 : 1;
  dt_v[i] += d; if (dt_v[i] > DT_MAX[i]) dt_v[i] = dt_min(i); if (dt_v[i] < dt_min(i)) dt_v[i] = DT_MAX[i]; dt_show();
}
static void dt_leave(bool ok) {
  lv_obj_t *s = scr_date; scr_date = NULL; void (*f)(bool) = dt_done; dt_done = NULL;
  if (f) f(ok); else lv_screen_load(dt_mode == 1 && scr_ready ? scr_ready : scr_settings);
  if (s) drop_screen(s);
}
static void build_date();
static void dt_set_cb(lv_event_t *e) {
  if (dt_mode == 2) {
    char b[12]; snprintf(b, sizeof b, "%04d-%02d-%02d", dt_v[0], dt_v[1], dt_v[2]); cfg_dob = b; nv_put("dob", cfg_dob);
    Serial.printf("patient born %s\n", b); dt_leave(true); return;
  }
  struct tm tm = {}; tm.tm_year = dt_v[0] - 1900; tm.tm_mon = dt_v[1] - 1; tm.tm_mday = dt_v[2]; tm.tm_hour = dt_v[3]; tm.tm_min = dt_v[4]; tm.tm_isdst = -1;
  time_t t = mktime(&tm); struct timeval tv = {t, 0}; settimeofday(&tv, NULL);
  Serial.printf("time: set by hand to %s", ctime(&t));
  dt_leave(true);
}
static void dt_cancel_cb(lv_event_t *e) { dt_leave(false); }
static void date_open(int mode, void (*done)(bool)) {
  dt_mode = mode; dt_done = done; if (scr_date) { lv_obj_t *o = scr_date; scr_date = NULL; drop_screen(o); } build_date();
  time_t now = time(NULL); struct tm tm = {};
  if (mode == 2) { int y = 1980, m = 1, d = 1; sscanf(cfg_dob.c_str(), "%d-%d-%d", &y, &m, &d); tm.tm_year = y - 1900; tm.tm_mon = m - 1; tm.tm_mday = d; }
  else if (have_date()) localtime_r(&now, &tm);
  else { char mon[4]; int d, y; sscanf(__DATE__, "%3s %d %d", mon, &d, &y); const char *M = "JanFebMarAprMayJunJulAugSepOctNovDec"; tm.tm_year = y - 1900; tm.tm_mon = (strstr(M, mon) - M) / 3; tm.tm_mday = d; tm.tm_hour = 21; tm.tm_min = 0; }   // the build date as a starting point
  dt_v[0] = tm.tm_year + 1900; dt_v[1] = tm.tm_mon + 1; dt_v[2] = tm.tm_mday; dt_v[3] = tm.tm_hour; dt_v[4] = tm.tm_min;
  lv_label_set_text(lbl_dt_note, mode == 2 ? "When was the patient born?" : mode == 1 ? "The date is not known. Set today's date and time, then press Start again." : (have_date() ? "Date and time, local" : "Date and time not set yet"));
  lv_obj_set_style_text_color(lbl_dt_note, mode == 1 || (mode == 0 && !have_date()) ? C_AMBER : C_GREY, 0);
  dt_show(); lv_screen_load(scr_date);
}
static void build_date() {
  scr_date = lv_obj_create(NULL); lv_obj_set_style_bg_color(scr_date, C_PANEL, 0); topbar(scr_date, dt_mode == 2 ? "Date of birth" : "Date and time");
  lbl_dt_note = label(scr_date, "", &lv_font_montserrat_12, C_GREY, 10, TOP_H + 4); lv_obj_set_width(lbl_dt_note, 300);
  const char *cap[5] = {"Year", "Month", "Day", "Hour", "Min"}; int n = dt_cols();
  for (int i = 0; i < n; i++) {
    int x, w; if (n == 5) { x = 8 + i * 62; w = i == 0 ? 68 : 56; if (i) x += 12; } else { x = 24 + i * 96; w = 80; }
    lv_obj_t *c = label(scr_date, cap[i], &lv_font_montserrat_12, C_DIM, x, TOP_H + 34); lv_obj_set_width(c, w); lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_CENTER, 0);
    button(scr_date, LV_SYMBOL_UP, x, TOP_H + 50, w, 34, C_KEY, dt_adj_cb, (void *)(intptr_t)(i * 2));
    lbl_dt[i] = label(scr_date, "", &lv_font_montserrat_20, lv_color_white(), x, TOP_H + 92); lv_obj_set_width(lbl_dt[i], w); lv_obj_set_style_text_align(lbl_dt[i], LV_TEXT_ALIGN_CENTER, 0);
    button(scr_date, LV_SYMBOL_DOWN, x, TOP_H + 120, w, 34, C_KEY, dt_adj_cb, (void *)(intptr_t)(i * 2 + 1));
  }
  button(scr_date, "Cancel", 8, 196, 120, 38, C_KEY, dt_cancel_cb, NULL);
  bigbutton(scr_date, "Set", 206, 196, 106, 38, C_GREEN, dt_set_cb, NULL);
}
// the checklist before the night: WatchPAT ONE wording (chest sensor, wrist unit, finger probe) from the Itamar patient guide
static lv_obj_t *chk[4];
static const char *CHK_TXT[4] = {"Chest sensor stuck on below the neck notch", "WatchPAT on the wrist of the other hand", "Finger probe pushed on (not the thumb)", "SleepCYD plugged into power"};
static void chk_cb(lv_event_t *e) {
  int n = 0; for (int i = 0; i < 4; i++) if (lv_obj_has_state(chk[i], LV_STATE_CHECKED)) n++;
  if (n == 4) lv_obj_remove_state(btn_start, LV_STATE_DISABLED); else lv_obj_add_state(btn_start, LV_STATE_DISABLED);
}
static void start_cb(lv_event_t *e) { if (lv_obj_has_state(btn_start, LV_STATE_DISABLED)) return; if (cfg_demo && !wp_get_state()) { demo_start(); return; } if (!have_date()) { date_open(1, NULL); return; } wp_user_start(); }   // the file name needs the date
static void ready_show() {                           // the WatchPAT is connected: the checklist, then Start
  if (!scr_ready) return;
  for (int i = 0; i < 4; i++) lv_obj_remove_state(chk[i], LV_STATE_CHECKED);
  lv_obj_add_state(btn_start, LV_STATE_DISABLED);
  lv_label_set_text(lbl_ready_msg, "Tap each line when it is done, then Start."); lv_obj_set_style_text_color(lbl_ready_msg, C_GREY, 0);
  lv_obj_t *a = lv_screen_active();
  if (a == scr_home || a == scr_turnon || a == scr_welcome) { lv_screen_load(scr_ready); welcome_leave(); }
}
static void dash_stop_cb(lv_event_t *e) { end_prompt("Stop the recording now?", 0); }
static void topbar2(lv_obj_t *scr, const char *title, lv_obj_t **right) {
  lv_obj_t *bar = lv_obj_create(scr); lv_obj_set_pos(bar, 0, 0); lv_obj_set_size(bar, SCREEN_W, TOP_H); flat(bar, C_TOPBAR);
  label(bar, title, &lv_font_montserrat_16, lv_color_white(), 10, 9);
  if (right) { *right = label(bar, "", &lv_font_montserrat_16, C_GREEN, 0, 0); lv_obj_align(*right, LV_ALIGN_RIGHT_MID, -44, 0); }
}
static void build_ready() {
  scr_ready = lv_obj_create(NULL); lv_obj_set_style_bg_color(scr_ready, C_PANEL, 0);
  topbar2(scr_ready, "Start sleep", NULL); gear(scr_ready);
  lbl_ready_dev = label(scr_ready, "", &lv_font_montserrat_14, C_GREEN, 12, TOP_H + 5);
  for (int i = 0; i < 4; i++) {
    lv_obj_t *c = chk[i] = lv_checkbox_create(scr_ready); lv_checkbox_set_text_static(c, CHK_TXT[i]); lv_obj_set_pos(c, 12, TOP_H + 25 + i * 27);
    lv_obj_set_style_text_font(c, &lv_font_montserrat_14, 0); lv_obj_set_style_text_color(c, lv_color_white(), 0); lv_obj_set_style_pad_column(c, 10, 0);
    lv_obj_set_style_bg_color(c, C_KEY, LV_PART_INDICATOR); lv_obj_set_style_border_color(c, C_GREY, LV_PART_INDICATOR); lv_obj_set_style_border_width(c, 2, LV_PART_INDICATOR); lv_obj_set_style_radius(c, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(c, C_GREEN, (lv_style_selector_t)LV_PART_INDICATOR | LV_STATE_CHECKED); lv_obj_set_style_border_color(c, C_GREEN, (lv_style_selector_t)LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(c, C_GREEN, LV_STATE_CHECKED);
    lv_obj_add_event_cb(c, chk_cb, LV_EVENT_VALUE_CHANGED, NULL); lv_obj_set_ext_click_area(c, 5);
  }
  lbl_ready_msg = label(scr_ready, "", &lv_font_montserrat_12, C_GREY, 12, TOP_H + 134); lv_obj_set_width(lbl_ready_msg, 296);
  btn_start = bigbutton(scr_ready, "Start sleep", 12, 186, 296, 48, C_GREEN, start_cb, NULL);
  lv_obj_set_style_text_color(lv_obj_get_child(btn_start, 0), lv_color_black(), 0);
  lv_obj_add_state(btn_start, LV_STATE_DISABLED);
}
static void build_dash() {                            // while recording: the time, a status line, Stop. No live numbers.
  scr_dash = lv_obj_create(NULL); lv_obj_set_style_bg_color(scr_dash, C_PANEL, 0);
  topbar2(scr_dash, "Receiving data", &lbl_d_saved); gear(scr_dash);
  lbl_d_hint = label(scr_dash, "", &lv_font_montserrat_14, C_AMBER, 0, TOP_H + 10); lv_obj_set_width(lbl_d_hint, SCREEN_W); lv_obj_set_style_text_align(lbl_d_hint, LV_TEXT_ALIGN_CENTER, 0);
  lbl_d_time = label(scr_dash, "00:00:00", &lv_font_montserrat_48, lv_color_white(), 0, TOP_H + 34); lv_obj_set_width(lbl_d_time, SCREEN_W); lv_obj_set_style_text_align(lbl_d_time, LV_TEXT_ALIGN_CENTER, 0);
  lbl_d_pos = label(scr_dash, "", &lv_font_montserrat_16, C_GREY, 0, TOP_H + 92); lv_obj_set_width(lbl_d_pos, SCREEN_W); lv_obj_set_style_text_align(lbl_d_pos, LV_TEXT_ALIGN_CENTER, 0);
  btn_d_stop = bigbutton(scr_dash, "Stop", 60, 150, 200, 54, C_RED, dash_stop_cb, NULL);
  button(scr_dash, "Status", 8, 212, 70, 26, C_KEY, to_home_cb, NULL);
}
static void dash_tick() {                             // call every ~100 ms
  if (!scr_dash || lv_screen_active() != scr_dash) return;   // (deleted before an upload)
  uint32_t now = millis();
  static uint32_t last = 0; if (now - last < 1000) return; last = now;
  lv_label_set_text_fmt(lbl_d_time, "%02lu:%02lu:%02lu", (unsigned long)(an.seconds / 3600), (unsigned long)(an.seconds / 60 % 60), (unsigned long)(an.seconds % 60));
  if (!sim_active) lv_label_set_text_fmt(lbl_d_saved, "%.1f MB", wp_get_bytes() / 1048576.0f); else lv_label_set_text(lbl_d_saved, "");
  lv_label_set_text(lbl_d_pos, cfg_patient.length() ? cfg_patient.c_str() : "Sleep well");
  lv_label_set_text(lbl_d_hint, sim_active ? "SIMULATED DATA (demo)" : wp_get_state() == 3 ? "The WatchPAT ended the test" : (an.packets > 10 && !an.finger) ? "No pulse signal: check the finger probe" : (now - wp_get_last() > 5000 ? "No data from the device" : "Receiving data"));
}
static void report_build() {
  uint32_t secs = an.seconds, tot = 0; for (int i = 0; i < 6; i++) tot += an.pos_secs[i];
  float hours = secs / 3600.0f;
  int k = 0; if (cfg_patient.length()) k += snprintf(report_text, sizeof report_text, "Patient: %s\n", cfg_patient.c_str());
  k += snprintf(report_text + k, sizeof report_text - k, "Recorded %02lu:%02lu:%02lu   (%.1f MB on the card)\n", (unsigned long)(secs / 3600), (unsigned long)(secs / 60 % 60), (unsigned long)(secs % 60), wp_get_bytes() / 1048576.0f);
  if (an.spo2_n) k += snprintf(report_text + k, sizeof report_text - k, "SpO2 average %.1f %%   lowest %.0f %%\n", an.spo2_sum / an.spo2_n, an.spo2_min);
  else k += snprintf(report_text + k, sizeof report_text - k, "SpO2 not available (no pulse signal)\n");
  if (an.hr_n) k += snprintf(report_text + k, sizeof report_text - k, "Pulse average %.0f   min %.0f   max %.0f bpm\n", an.hr_sum / an.hr_n, an.hr_min, an.hr_max);
  k += snprintf(report_text + k, sizeof report_text - k, "Oxygen drops (3%% for 10 s or more): %lu   ODI %.1f per hour\n", (unsigned long)an.desat_count, hours > 0.01f ? an.desat_count / hours : 0.0f);
  { float pahi = hours > 0.01f ? (an.apnea_count + an.hypopnea_count) / hours : 0.0f;
    k += snprintf(report_text + k, sizeof report_text - k, "pAHI %.1f per hour (%s)   pRDI %.1f   apneas %lu  hypopneas %lu  RERA %lu\n", pahi, an_severity(pahi),
                  hours > 0.01f ? (an.apnea_count + an.hypopnea_count + an.rera_count) / hours : 0.0f, (unsigned long)an.apnea_count, (unsigned long)an.hypopnea_count, (unsigned long)an.rera_count); }
  if (tot) k += snprintf(report_text + k, sizeof report_text - k, "Position: back %lu%%  left %lu%%  right %lu%%  front %lu%%  upright %lu%%\n",
                         (unsigned long)(an.pos_secs[4] * 100 / tot), (unsigned long)(an.pos_secs[2] * 100 / tot), (unsigned long)(an.pos_secs[3] * 100 / tot), (unsigned long)(an.pos_secs[5] * 100 / tot), (unsigned long)((an.pos_secs[0] + an.pos_secs[1]) * 100 / tot));
  if (an.resp > 0) k += snprintf(report_text + k, sizeof report_text - k, "Breathing %.0f per minute (last reading)\n", an.resp);
  k += snprintf(report_text + k, sizeof report_text - k, "Estimates from the raw signals, not a clinical score.");
}
static uint32_t wp_get_serial();
static int report_sess = 0;                    // session shown in the report (sess_no is 0 by then: the files are closed)
static lv_obj_t *btn_close_rep = NULL;
#include "nidra.h"                                  // the Nidra site API (n8n webhook): start, chunks, complete
static bool is_nidra() { return cfg_api.indexOf("/webhook/nidra") >= 0; }
static void pend_set(const char *path, bool upload) { pend_path = path; pend_skip = false; pend_sent = !upload; pend_rec = ""; nv_put("pend", pend_path); nv_put("pskip", false); nv_put("psent", !upload); nv_remove("prec"); }
static void pend_clear() { pend_path = ""; pend_rec = ""; pend_sent = true; nv_remove("pend"); nv_remove("prec"); nv_put("pskip", false); nv_put("psent", true); }
static void report_close_cb(lv_event_t *e) { if (!scr_home) { delay(200); ESP.restart(); } welcome_show(); }
static void report_show(int sess) {
  report_sess = sess;
  if (scr_report) { lv_obj_delete(scr_report); scr_report = NULL; }
  scr_report = lv_obj_create(NULL); lv_obj_set_style_bg_color(scr_report, C_PANEL, 0);
  lv_obj_t *t; topbar2(scr_report, "Sleep report", &t); if (sess) lv_label_set_text_fmt(t, "s%04d", sess); else lv_label_set_text(t, "DEMO"); lv_obj_set_style_text_color(t, sess ? C_GREY : C_AMBER, 0);
  lbl_report = label(scr_report, report_text, &lv_font_montserrat_14, lv_color_white(), 12, TOP_H + 8); lv_obj_set_width(lbl_report, 300);
  lv_obj_set_style_text_line_space(lbl_report, 3, 0);
  btn_close_rep = bigbutton(scr_report, "Close", 206, 196, 106, 40, C_ACCENT, report_close_cb, NULL);
  lv_screen_load(scr_report);
}

// ---------------------------------------------------------------- patient name (letters and spaces, 12 characters)
static lv_obj_t *scr_name = NULL, *name_ta = NULL, *name_bm = NULL;
static void (*name_done)(bool) = NULL; static bool name_blank = false;   // what to do after OK/Cancel (NULL: back to Settings); start with an empty field
static bool name_shift = true;
static const char *NM_UP[] = {"Q","W","E","R","T","Y","U","I","O","P","\n","A","S","D","F","G","H","J","K","L","\n","Z","X","C","V","B","N","M",LV_SYMBOL_BACKSPACE,"\n","Cancel","Space","OK",""};
static const char *NM_LO[] = {"q","w","e","r","t","y","u","i","o","p","\n","a","s","d","f","g","h","j","k","l","\n","z","x","c","v","b","n","m",LV_SYMBOL_BACKSPACE,"\n","Cancel","Space","OK",""};
static void name_apply_map() {
  lv_buttonmatrix_set_map(name_bm, name_shift ? NM_UP : NM_LO);
  lv_buttonmatrix_set_button_width(name_bm, 26, 2); lv_buttonmatrix_set_button_width(name_bm, 27, 2); lv_buttonmatrix_set_button_width(name_bm, 28, 4); lv_buttonmatrix_set_button_width(name_bm, 29, 2);
}
static String clean_name(const String &in) {
  String o; for (unsigned i = 0; i < in.length() && o.length() < 12; i++) { char c = in[i]; if (isalpha((unsigned char)c) || (c == ' ' && o.length() && o[o.length() - 1] != ' ')) o += c; }
  o.trim(); return o;
}
static void patient_btn_label();
static void name_close(bool ok) {
  lv_obj_t *s = scr_name; scr_name = NULL; name_ta = name_bm = NULL; void (*f)(bool) = name_done; name_done = NULL; name_blank = false;
  if (f) f(ok); else lv_screen_load(scr_settings);
  if (s) drop_screen(s);
}
static void name_key_cb(lv_event_t *e) {
  uint32_t id = lv_buttonmatrix_get_selected_button(name_bm); if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
  const char *t = lv_buttonmatrix_get_button_text(name_bm, id); if (!t) return;
  String cur = lv_textarea_get_text(name_ta);
  if (!strcmp(t, "Cancel")) { name_close(false); return; }
  if (!strcmp(t, "OK")) { cfg_patient = clean_name(cur); cfg_save(); patient_btn_label(); Serial.printf("patient name set: '%s'\n", cfg_patient.c_str()); name_close(true); return; }
  if (!strcmp(t, LV_SYMBOL_BACKSPACE)) { lv_textarea_delete_char(name_ta); }
  else if (!strcmp(t, "Space")) { if (cur.length() && cur.length() < 12 && cur[cur.length() - 1] != ' ') lv_textarea_add_char(name_ta, ' '); }
  else if (cur.length() < 12) lv_textarea_add_text(name_ta, t);
  String now = lv_textarea_get_text(name_ta);
  bool want = !now.length() || now[now.length() - 1] == ' ';           // capital letter at the start of each word
  if (want != name_shift) { name_shift = want; name_apply_map(); }
}
static void patient_cb(lv_event_t *e) {
  scr_name = lv_obj_create(NULL); lv_obj_set_style_bg_color(scr_name, lv_color_black(), 0);
  name_ta = lv_textarea_create(scr_name); lv_obj_set_pos(name_ta, 4, 4); lv_obj_set_size(name_ta, 312, 44);
  lv_textarea_set_one_line(name_ta, true); lv_textarea_set_max_length(name_ta, 12); lv_textarea_set_accepted_chars(name_ta, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz ");
  lv_textarea_set_placeholder_text(name_ta, "Patient name"); lv_textarea_set_text(name_ta, name_blank ? "" : cfg_patient.c_str());
  lv_obj_set_style_text_font(name_ta, &lv_font_montserrat_20, 0); lv_obj_set_style_bg_color(name_ta, C_PANEL, 0); lv_obj_set_style_text_color(name_ta, lv_color_white(), 0);
  lv_obj_set_style_border_color(name_ta, C_ACCENT, 0); lv_obj_remove_flag(name_ta, LV_OBJ_FLAG_CLICKABLE);       // typing only through the keys
  name_bm = lv_buttonmatrix_create(scr_name); lv_obj_set_pos(name_bm, 2, 52); lv_obj_set_size(name_bm, 316, 186);
  lv_obj_set_style_bg_color(name_bm, lv_color_black(), 0); lv_obj_set_style_border_width(name_bm, 0, 0); lv_obj_set_style_pad_all(name_bm, 2, 0); lv_obj_set_style_pad_gap(name_bm, 4, 0);
  lv_obj_set_style_bg_color(name_bm, C_KEY, LV_PART_ITEMS); lv_obj_set_style_text_color(name_bm, lv_color_white(), LV_PART_ITEMS); lv_obj_set_style_text_font(name_bm, &lv_font_montserrat_20, LV_PART_ITEMS);
  lv_obj_set_style_radius(name_bm, 6, LV_PART_ITEMS); lv_obj_set_style_shadow_width(name_bm, 0, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(name_bm, lv_color_white(), (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_PRESSED); lv_obj_set_style_text_color(name_bm, lv_color_black(), (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_PRESSED);
  String cur = name_blank ? "" : cfg_patient; name_shift = !cur.length() || cur[cur.length() - 1] == ' ';
  name_apply_map();
  lv_obj_add_event_cb(name_bm, name_key_cb, LV_EVENT_VALUE_CHANGED, NULL);
  lv_screen_load(scr_name);
}

// ---------------------------------------------------------------- the night's flow
// Boot: "New patient | Same patient". New: remove the card's recordings?, name, date of birth. Then "Turn on the WatchPAT" (scanning),
// the found dialog, the checklist, Start, "Receiving data", and the End-session prompt (Stop, end of test, or a lost link).
static void gear(lv_obj_t *scr) { lv_obj_t *b = button(scr, LV_SYMBOL_SETTINGS, 284, 4, 28, 26, C_KEY, settings_cb, NULL); lv_obj_set_style_radius(b, 13, 0); lv_obj_set_ext_click_area(b, 6); }
static void drop_screen(lv_obj_t *s) { if (dlg && lv_obj_get_screen(dlg) == s) dlg = NULL; lv_obj_delete(s); }   // a dialog on that screen dies with it
static lv_obj_t *lbl_to_who = NULL, *scr_wipe = NULL;
static void welcome_leave() { if (scr_welcome && lv_screen_active() != scr_welcome) { lv_obj_t *s = scr_welcome; scr_welcome = NULL; lbl_hib = NULL; drop_screen(s); } }
static void wipe_leave() { lv_obj_t *s = scr_wipe; scr_wipe = NULL; if (s) drop_screen(s); }
static int sd_wipe() {                               // every file in /SLEEP, a few at a time (deleting while listing skips entries)
  int n = 0; if (!sd_ok) return 0;
  for (int round = 0; round < 60; round++) {
    char names[16][40]; int k = 0;
    File d = SD.open("/SLEEP"); if (!d) break;
    for (File f = d.openNextFile(); f && k < 16; f = d.openNextFile()) snprintf(names[k++], 40, "/SLEEP/%s", f.name());
    d.close();
    if (!k) break;
    for (int i = 0; i < k; i++) if (SD.remove(names[i])) n++;
  }
  cfg_session = 0; cfg_save(); pend_clear(); sd_stats(); ulog("sd: %d files removed", n); return n;
}
static void new_dob_done(bool ok) { turnon_show(); }
static void new_name_done(bool ok) { if (!ok) { welcome_show(); return; } cfg_dob = ""; nv_remove("dob"); date_open(2, new_dob_done); }
static void wipe_next() { name_done = new_name_done; name_blank = true; patient_cb(NULL); wipe_leave(); }
static void wipe_yes_cb(lv_event_t *e) {
  lv_obj_t *l = lv_obj_get_child(lv_event_get_target_obj(e), 0); lv_label_set_text(l, "Removing..."); lv_refr_now(NULL);
  sd_wipe(); wipe_next();
}
static void wipe_no_cb(lv_event_t *e) { wipe_next(); }
static void wipe_back_cb(lv_event_t *e) { welcome_show(); wipe_leave(); }
static void wipe_show() {
  wipe_leave(); scr_wipe = lv_obj_create(NULL); lv_obj_set_style_bg_color(scr_wipe, C_PANEL, 0); topbar(scr_wipe, "New patient");
  label(scr_wipe, "Remove all recordings from the card?", &lv_font_montserrat_16, lv_color_white(), 12, TOP_H + 12);
  int files = 0; uint64_t bytes = 0;
  File d = sd_ok ? SD.open("/SLEEP") : File();
  if (d) { for (File f = d.openNextFile(); f; f = d.openNextFile()) { files++; bytes += f.size(); } d.close(); }
  lv_obj_t *l = label(scr_wipe, "", &lv_font_montserrat_14, C_GREY, 12, TOP_H + 40); lv_obj_set_width(l, 296); lv_obj_set_style_text_line_space(l, 3, 0);
  if (!sd_ok) lv_label_set_text(l, "No SD card found."); else lv_label_set_text_fmt(l, "%d files, %.1f MB on the card.\nThe previous patient's nights would be gone\nfrom this device (the site keeps its copies).", files, bytes / 1048576.0);
  bigbutton(scr_wipe, "Remove all", 12, TOP_H + 104, 140, 46, C_RED, wipe_yes_cb, NULL);
  bigbutton(scr_wipe, "Keep them", 168, TOP_H + 104, 140, 46, C_KEY, wipe_no_cb, NULL);
  button(scr_wipe, "Back", 216, 200, 96, 34, C_ACCENT, wipe_back_cb, NULL);
  lv_screen_load(scr_wipe);
}
static void welcome_new_cb(lv_event_t *e) { wipe_show(); welcome_leave(); }
static void welcome_old_cb(lv_event_t *e) { turnon_show(); }
static void welcome_show() {
  if (!scr_welcome) {
    scr_welcome = lv_obj_create(NULL); lv_obj_set_style_bg_color(scr_welcome, C_PANEL, 0); topbar(scr_welcome, "SleepCYD"); gear(scr_welcome);
    if (cfg_hib) { lbl_hib = label(scr_welcome, "", &lv_font_montserrat_12, C_AMBER, 96, 3); lv_obj_set_width(lbl_hib, 182); lv_obj_set_style_text_align(lbl_hib, LV_TEXT_ALIGN_RIGHT, 0); hib_text(); }
    label(scr_welcome, "Who is sleeping tonight?", &lv_font_montserrat_16, lv_color_white(), 12, TOP_H + 10);
    bigbutton(scr_welcome, "New patient", 12, TOP_H + 38, 296, 50, C_ACCENT, welcome_new_cb, NULL);
    if (cfg_patient.length()) {
      char t[40]; snprintf(t, sizeof t, "Same patient:  %s", cfg_patient.c_str());
      lv_obj_t *b = bigbutton(scr_welcome, t, 12, TOP_H + 98, 296, 50, C_GREEN, welcome_old_cb, NULL); lv_obj_set_style_text_color(lv_obj_get_child(b, 0), lv_color_black(), 0);
    }
    lv_obj_t *l = label(scr_welcome, "", &lv_font_montserrat_12, C_DIM, 12, 200); lv_obj_set_width(l, 296);
    if (!sd_ok) { lv_label_set_text(l, "No SD card: insert one and restart."); lv_obj_set_style_text_color(l, C_RED, 0); }
    else if (!cfg_ssid.length()) { lv_label_set_text(l, "No Wi-Fi set: reports stay on the card.\nTap the gear to set it up."); lv_obj_set_style_text_color(l, C_AMBER, 0); }
    else lv_label_set_text_fmt(l, "Wi-Fi %s  -  %s", cfg_ssid.c_str(), is_nidra() ? "nights go to the site" : "report page on the phone");
    if (cfg_demo) { lv_label_set_text(l, "Demo mode is on: a recorded night is replayed, nothing\nis sent. Settings > Device turns it off."); lv_obj_set_style_text_color(l, C_AMBER, 0); }
  }
  lv_screen_load(scr_welcome);
}
static void to_patient_cb(lv_event_t *e) { welcome_show(); }
static void bullet(lv_obj_t *scr, int n, const char *txt, int y) {   // a numbered round bullet and its line
  lv_obj_t *c = lv_obj_create(scr); lv_obj_set_size(c, 24, 24); lv_obj_set_pos(c, 12, y); flat(c, C_ACCENT); lv_obj_set_style_radius(c, 12, 0);
  lv_obj_t *l = lv_label_create(c); lv_label_set_text_fmt(l, "%d", n); lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0); lv_obj_set_style_text_color(l, lv_color_white(), 0); lv_obj_center(l);
  label(scr, txt, &lv_font_montserrat_16, lv_color_white(), 46, y + 3);
}
static void build_turnon() {
  scr_turnon = lv_obj_create(NULL); lv_obj_set_style_bg_color(scr_turnon, C_PANEL, 0); topbar(scr_turnon, "SleepCYD"); gear(scr_turnon);
  lbl_to_who = label(scr_turnon, "", &lv_font_montserrat_14, C_GREEN, 0, 0); lv_obj_align(lbl_to_who, LV_ALIGN_TOP_RIGHT, -46, 10);   // the patient, in the top bar
  label(scr_turnon, "Turn on the WatchPAT", &lv_font_montserrat_20, lv_color_white(), 12, TOP_H + 8);
  bullet(scr_turnon, 1, "Battery in, or pull its tab", TOP_H + 42);
  bullet(scr_turnon, 2, "Wait for its light to blink", TOP_H + 74);
  bullet(scr_turnon, 3, "Keep it near this screen", TOP_H + 106);
  lbl_to_state = label(scr_turnon, "", &lv_font_montserrat_14, C_ACCENT, 12, TOP_H + 140); lv_obj_set_width(lbl_to_state, 296); lv_label_set_long_mode(lbl_to_state, LV_LABEL_LONG_DOT);
  button(scr_turnon, "Change patient", 8, 196, 150, 38, C_KEY, to_patient_cb, NULL);
  button(scr_turnon, "Status", 216, 196, 96, 38, C_KEY, to_home_cb, NULL);
}
static void turnon_show() {
  int ws = wp_get_state();
  if (ws) { lv_screen_load(ws == 2 || ws == 5 ? scr_dash : scr_ready); welcome_leave(); return; }   // already connected
  if (cfg_demo) {                                    // demo: no device, a simulated night from the checklist
    lv_label_set_text(lbl_ready_dev, "DEMO MODE: a simulated night, no WatchPAT"); lv_obj_set_style_text_color(lbl_ready_dev, C_AMBER, 0);
    ready_show(); lv_screen_load(scr_ready); welcome_leave();
    if (!(sd_ok && SD.exists(DEMO_PATH))) { lv_label_set_text(lbl_ready_msg, "No DEMO.DAT on the card: Start plays a synthetic hour instead."); lv_obj_set_style_text_color(lbl_ready_msg, C_AMBER, 0); }
    return;
  }
  if (lbl_to_who) lv_label_set_text(lbl_to_who, cfg_patient.length() ? cfg_patient.c_str() : "");
  lv_screen_load(scr_turnon); welcome_leave();
  if (!sd_ok) { ui_state("Insert an SD card and restart", C_RED); return; }
  if (state == ST_IDLE || state == ST_WAIT) { auto_mode = true; scan_start(); }
}
// end of the session: a prompt (Stop, end of test, lost link), then night mode (report, upload, QR) or the on-screen report without Wi-Fi
static char end_path[40] = ""; static int end_sess = 0;   // the recording that ended (set by wp_end)
static lv_obj_t *end_dlg = NULL, *lbl_end_timer = NULL; static uint32_t end_auto_at = 0;
static void end_dlg_close() { if (end_dlg) { lv_obj_delete(end_dlg); end_dlg = NULL; } lbl_end_timer = NULL; end_auto_at = 0; }
static void end_now_cb(lv_event_t *e) { end_dlg_close(); wp_user_end(); }
static void end_keep_cb(lv_event_t *e) { end_dlg_close(); }
static void end_prompt(const char *why, int auto_s) {
  if (end_dlg) return;
  if (scr_dash && lv_screen_active() != scr_dash) lv_screen_load(scr_dash);
  end_dlg = lv_obj_create(lv_screen_active()); lv_obj_set_size(end_dlg, 296, 156); lv_obj_center(end_dlg);
  lv_obj_set_style_bg_color(end_dlg, C_PANEL, 0); lv_obj_set_style_border_color(end_dlg, C_AMBER, 0); lv_obj_set_style_border_width(end_dlg, 2, 0);
  lv_obj_set_style_radius(end_dlg, 6, 0); lv_obj_set_style_pad_all(end_dlg, 10, 0); lv_obj_remove_flag(end_dlg, LV_OBJ_FLAG_SCROLLABLE);
  label(end_dlg, "End the session?", &lv_font_montserrat_20, C_AMBER, 0, 0);
  lv_obj_t *l = label(end_dlg, why, &lv_font_montserrat_14, lv_color_white(), 0, 30); lv_obj_set_width(l, 272);
  if (auto_s) { lbl_end_timer = label(end_dlg, "", &lv_font_montserrat_12, C_DIM, 0, 70); end_auto_at = millis() + auto_s * 1000UL; }
  button(end_dlg, "End session", 0, 90, 140, 42, C_RED, end_now_cb, NULL);
  button(end_dlg, "Continue", 150, 90, 124, 42, C_KEY, end_keep_cb, NULL);
}
static void demo_start() {                            // Demo mode: replay the recorded demo night through night mode (no upload)
  if (!(sd_ok && SD.exists(DEMO_PATH))) { sim_start(); return; }
  lv_obj_t *s = lv_obj_create(NULL); lv_obj_set_style_bg_color(s, C_PANEL, 0);
  label(s, "Demo night", &lv_font_montserrat_28, C_AMBER, 12, 40);
  label(s, "A night recorded earlier, replayed from the card.", &lv_font_montserrat_14, C_GREY, 12, 84);
  label(s, "Preparing the report...", &lv_font_montserrat_16, lv_color_white(), 12, 130);
  lv_screen_load(s); lv_refr_now(NULL);
  pend_set(DEMO_PATH, false); delay(1500); ESP.restart();
}
static void finish_night() {
  end_dlg_close();
  if (!end_path[0]) { turnon_show(); return; }        // nothing was recorded
  if (cfg_ssid.length()) {                            // night mode: the report, the upload and the QR codes, without Bluetooth
    lv_obj_t *s = lv_obj_create(NULL); lv_obj_set_style_bg_color(s, C_PANEL, 0);
    label(s, "Session ended", &lv_font_montserrat_28, C_GREEN, 12, 40);
    label(s, cfg_patient.c_str(), &lv_font_montserrat_16, C_GREY, 12, 84);
    label(s, "Preparing the report...", &lv_font_montserrat_16, lv_color_white(), 12, 130);
    lv_screen_load(s); lv_refr_now(NULL);
    pend_set(end_path, is_nidra()); delay(1500); ESP.restart();
  }
  int sess = end_sess; end_path[0] = 0; report_show(sess);
}

// ---------------------------------------------------------------- demo mode: simulated night (5 taps on the title)
// Synthetic oximetry/PAT/chest/motion samples go through the same analysis, dashboard and report as real data.
#define SIM_LEN 3600                                  // simulated seconds (1 hour, plays at about 60x)
static uint32_t sim_t = 0, sim_rng = 12345; static float sim_ph = 0, sim_ch = 0;
static uint32_t sim_ev_start[40], sim_ev_len[40]; static int sim_ev_n = 0;
static uint32_t sim_rand() { sim_rng = sim_rng * 1664525u + 1013904223u; return sim_rng >> 8; }
static float sim_frand() { return (sim_rand() & 0xFFFF) / 65535.0f; }
static void sim_start() {
  if (sim_active || wp_get_state() != 0) return;
  an_reset(); memset(dot_t, 0, sizeof dot_t); sim_active = true; sim_t = 0; sim_ph = sim_ch = 0; sim_rng = 12345; sim_ev_n = 0;
  uint32_t t = 150;                                    // breathing events: one every 3 to 6 minutes, 18 to 35 s long, two clusters
  while (t < SIM_LEN - 60 && sim_ev_n < 40) { sim_ev_start[sim_ev_n] = t; sim_ev_len[sim_ev_n] = 18 + sim_rand() % 18; sim_ev_n++; t += 180 + sim_rand() % 180; if (sim_ev_n == 8) t -= 100; }
  Serial.println("demo: simulated night started");
  found_beep();
  lv_screen_load(scr_dash);
}
static float sim_event_level(uint32_t t) {            // 0 normal .. 1 in an event (airflow stopped), with a lagging oxygen response
  float lvl = 0;
  for (int i = 0; i < sim_ev_n; i++) { uint32_t a = sim_ev_start[i], b = a + sim_ev_len[i]; if (t >= a && t < b) lvl = 1; }
  return lvl;
}
static float sim_spo2(uint32_t t) {                   // oxygen follows the events 8 s late and recovers over 12 s
  float depth = 0;
  for (int i = 0; i < sim_ev_n; i++) {
    float a = sim_ev_start[i] + 8.0f, b = sim_ev_start[i] + sim_ev_len[i] + 8.0f, d = 5.0f + (i % 4);
    float x = 0; if (t >= a && t < b) x = d; else if (t >= b && t < b + 12) x = d * (1 - (t - b) / 12.0f);
    if (x > depth) depth = x;
  }
  return 97.0f - depth;
}
static void sim_second() {
  uint32_t t = sim_t;
  float ev = sim_event_level(t), S = sim_spo2(t), R = (116.0f - S) / 25.0f;
  float f = 1.15f + 0.08f * sinf(t / 300.0f) + (ev > 0 ? 0.12f : 0);           // pulse about 69 to 80 bpm, up in events
  int seg = (t / 1200) % 5; static const int posseq[5] = {4, 2, 4, 3, 4};      // supine, left, supine, right, supine
  float patA = ev > 0 ? 180 : 420;
  for (int i = 0; i < 100; i++) {
    sim_ph += 2 * M_PI * f / 100; if (sim_ph > 2 * M_PI) sim_ph -= 2 * M_PI;
    float shape = powf((1 + sinf(sim_ph)) / 2, 6);                              // peaky pulse wave
    float noise = (sim_frand() - 0.5f) * 4;
    ring_push(an_pat, (int16_t)(shape * patA + noise));
    ring_push(an_oxb, (int16_t)(3000 + shape * 60 + noise));                    // channel B: DC 3000, AC/DC 2 %
    ring_push(an_oxa, (int16_t)(2000 + shape * 40 * R + noise));                // channel A: AC/DC = R x that of B -> SpO2 = 116 - 25 R
    sim_ch += 2 * M_PI * 0.23f / 100; if (sim_ch > 2 * M_PI) sim_ch -= 2 * M_PI;
    ring_push(an_chest, (int16_t)(sinf(sim_ch) * (ev > 0 ? 40 : 320) + (sim_frand() - 0.5f) * 10));
  }
  bool burst = (t % 420) < 12;                                                  // body movement for 12 s every 7 minutes
  int ax[6] = {0}; ax[posseq[seg] == 4 ? 4 : posseq[seg]] = 1000;               // dominant axis: z+ supine, y+ left, y- right
  int x = 0, y = 0, z = 0; if (posseq[seg] == 4) z = 1000; else if (posseq[seg] == 2) y = 1000; else if (posseq[seg] == 3) y = -1000;
  (void)ax;
  for (int i = 0; i < 5; i++) { int j = burst ? 300 : 10; ring_push(an_ax, x + (int)((sim_frand() - 0.5f) * j)); ring_push(an_ay, y + (int)((sim_frand() - 0.5f) * j)); ring_push(an_az, z + (int)((sim_frand() - 0.5f) * j)); }
  an_step(true, x, y, z, t * 1000);
  an.metric = (int)(S * 10);
  uint32_t now = millis(); for (int i = 0; i < 6; i++) dot_t[i] = now;
}
static void report_show(int sess);
static void report_build();
static void sim_finish() {
  if (!sim_active) return;
  sim_active = false; report_build(); Serial.printf("demo report:\n%s\n", report_text); report_show(0);
}
static void sim_poll() {
  if (!sim_active) return;
  for (int k = 0; k < 6 && sim_t < SIM_LEN; k++) { sim_second(); sim_t++; }
  if (sim_t >= SIM_LEN) sim_finish();
}
static uint8_t title_taps = 0; static uint32_t title_t0 = 0;
static void title_tap_cb(lv_event_t *e) {
  if (millis() - title_t0 > 3000) title_taps = 0;
  if (!title_taps) title_t0 = millis();
  if (++title_taps >= 5) { title_taps = 0; sim_start(); }
}

// ---------------------------------------------------------------- WatchPAT session (NUS protocol, see watchpat.h)
static bool wp_active = false; static int wp_state = 0;   // 0 off, 1 session start sent, 4 ready (waiting for Start), 5 start sent, 2 acquiring, 3 device reported end of test
static bool wp_resume = false;                              // next session start uses mode 4 (resume) after a lost link
static NimBLERemoteCharacteristic *wp_rx = NULL; static uint16_t wp_tx_handle = 0;
static WpReasm wp_r; static uint32_t wp_seq = 0, wp_t_sent = 0, wp_t_last = 0, wp_retries = 0;
static uint32_t wp_pkts = 0, wp_bytes = 0, wp_crc_bad = 0, wp_acks = 0, wp_serial = 0;
static WpCounts wp_c = {};
static File wp_dat; static char wp_path[40] = "";
static uint8_t wp_pkt[WP_MAX], wp_out[64];

static void wp_send(const uint8_t *pkt, size_t n) {        // 20-byte chunks, write without response, like the app
  if (!wp_rx) return;
  for (size_t i = 0; i < n; i += 20) { wp_rx->writeValue(pkt + i, min((size_t)20, n - i), false); delay(10); }
}
static void wp_cmd(uint16_t opcode) { size_t n = wp_build(wp_out, opcode, ++wp_seq, NULL, 0); wp_send(wp_out, n); Serial.printf("wp: tx opcode %04x id %lu\n", opcode, (unsigned long)wp_seq); }
static void wp_ack(uint16_t opcode, uint32_t pid) { size_t n = wp_build_ack(wp_out, opcode, pid); wp_send(wp_out, n); wp_acks++; }
static void wp_session_start() {
  size_t n = wp_build_session_start(wp_out, ++wp_seq, wp_resume ? 4 : 1); wp_send(wp_out, n); wp_state = 1; wp_t_sent = millis();
  ulog("wp: session start %s(try %lu)", wp_resume ? "resume " : "", (unsigned long)wp_retries + 1);
}
static void wp_begin(NimBLERemoteService *nus) {
  wp_rx = nus->getCharacteristic(WP_NUS_RX);
  NimBLERemoteCharacteristic *tx = nus->getCharacteristic(WP_NUS_TX);
  if (!wp_rx || !tx) { ulog("wp: NUS characteristics missing"); return; }
  wp_tx_handle = tx->getHandle(); wp_r.n = 0; wp_seq = 0; wp_pkts = wp_bytes = wp_crc_bad = wp_acks = 0; wp_retries = 0; wp_serial = 0; memset(&wp_c, 0, sizeof wp_c);
  time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm); char day[8];   // YYMMDD_<serial>_NNN.DAT, NNN counts the day's recordings; 000000 when the date is unknown
  if (now > 1700000000) snprintf(day, sizeof day, "%02u%02u%02u", (unsigned)tm.tm_year % 100, (unsigned)(tm.tm_mon + 1) % 100, (unsigned)tm.tm_mday % 100); else strcpy(day, "000000");   // not strftime: it lives in IRAM (3 KB) and the deep sleep code of Hibernate needs that room
  for (int seq = 1; seq < 1000; seq++) { snprintf(wp_path, sizeof wp_path, "/SLEEP/%s_%lu_%03d.DAT", day, (unsigned long)wp_get_serial(), seq); if (!SD.exists(wp_path)) break; }
  wp_dat = SD.open(wp_path, FILE_WRITE);
  if (!wp_dat) { ulog("wp: cannot create %s", wp_path); return; }
  if (sess_txt) sess_txt.printf("watchpat file %s\n", wp_path);
  wp_active = true; wp_t_last = millis(); an_reset(); memset(dot_t, 0, sizeof dot_t);
  wp_session_start();
}
static void wp_end(bool send_stop) {
  if (!wp_active) return;
  if (send_stop && wp_state == 2 && client && client->isConnected()) { wp_cmd(WP_OP_STOP_ACQ); delay(300); }
  if (wp_dat) { wp_dat.close(); }
  if (sess_txt) sess_txt.printf("watchpat packets %lu bytes %lu crc_bad %lu oxiA %lu oxiB %lu pat %lu chest %lu metric %lu motion %lu events %lu\n",
                                (unsigned long)wp_pkts, (unsigned long)wp_bytes, (unsigned long)wp_crc_bad, (unsigned long)wp_c.oxia, (unsigned long)wp_c.oxib,
                                (unsigned long)wp_c.pat, (unsigned long)wp_c.chest, (unsigned long)wp_c.metric, (unsigned long)wp_c.motion, (unsigned long)wp_c.event);
  ulog("wp: %lu packets %lu KB saved in %s", (unsigned long)wp_pkts, (unsigned long)(wp_bytes / 1024), wp_path);
  if (wp_pkts) {
    report_build();
    char rp[48]; snprintf(rp, sizeof rp, "%.*s_report.txt", (int)strlen(wp_path) - 4, wp_path);
    File rf = SD.open(rp, FILE_WRITE); if (rf) { rf.printf("SleepCYD report, serial %lu, session s%04d\n%s\n", (unsigned long)wp_serial, sess_no, report_text); rf.close(); }
    if (sess_txt) sess_txt.printf("report:\n%s\n", report_text);
    Serial.printf("report:\n%s\n", report_text);
    strncpy(end_path, wp_path, sizeof end_path - 1); end_sess = sess_no;   // finish_night() takes it from here
  }
  wp_active = false; wp_state = 0; wp_rx = NULL;
}
static void wp_handle(const uint8_t *pkt, size_t n) {
  uint16_t op = wp_opcode(pkt); uint32_t pid = wp_pid(pkt); const uint8_t *pl = pkt + WP_HDR; size_t pn = n - WP_HDR;
  bool crc = wp_crc_ok(pkt, n); if (!crc) wp_crc_bad++;
  switch (op) {
    case WP_OP_ACK: {
      uint16_t orig = pn >= 2 ? (pl[0] << 8) | pl[1] : 0; uint8_t st = pn > 2 ? pl[2] : 0;
      Serial.printf("wp: ack for %04x status %u (id %lu)\n", orig, st, (unsigned long)pid);
      if (orig == WP_OP_START_ACQ && st == 0) { wp_state = 2; wp_t_last = millis(); an_reset(); memset(dot_t, 0, sizeof dot_t); ulog("wp: recording started"); if (lv_screen_active() != scr_dash) lv_screen_load(scr_dash); }
      if (st) ulog("wp: device refused %04x status %u", orig, st);
      return;                                           // ACKs are not acknowledged
    }
    case WP_OP_SESSION_CONFIRM: case WP_OP_CONFIG: {
      if (pn >= 58) { wp_serial = pl[54] | (pl[55] << 8) | (pl[56] << 16) | ((uint32_t)pl[57] << 24); if (wp_serial && wp_serial != cfg_serial) { cfg_serial = wp_serial; nv_put("serial", cfg_serial); } }
      ulog("wp: session confirmed, serial %lu, hw %u.%u fw %u", (unsigned long)wp_serial, pl[0], pl[1], pn >= 4 ? pl[2] | (pl[3] << 8) : 0);
      if (sess_txt) sess_txt.printf("watchpat serial %lu hw %u.%u\n", (unsigned long)wp_serial, pl[0], pl[1]);
      wp_ack(op, pid);
      if (wp_state == 1) {
        if (wp_resume) { delay(50); wp_cmd(WP_OP_START_ACQ); wp_state = 5; wp_t_sent = millis(); ulog("wp: resuming recording"); end_dlg_close(); }
        else {
          wp_state = 4; found_beep();
          lv_label_set_text_fmt(lbl_ready_dev, "WatchPAT %lu connected", (unsigned long)wp_serial); lv_obj_set_style_text_color(lbl_ready_dev, C_GREEN, 0);
          ready_show();
          ulog("wp: ready, waiting for Start");
        }
      }
      return;
    }
    case WP_OP_DATA: {
      if (wp_dat) { uint8_t lp[4] = {(uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24)}; wp_dat.write(lp, 4); wp_dat.write(pkt, n); }
      Serial.printf("wp: pkt id %lu %uB at %lu ms\n", (unsigned long)pid, (unsigned)n, (unsigned long)millis());
      wp_pkts++; wp_bytes += n; wp_t_last = millis(); wp_state = 2;
      wp_count_records(pl, pn, wp_c);
      { uint8_t seen = an_feed(pl, pn, millis()); for (int i = 0; i < 6; i++) if (seen & (1 << (i + 1))) dot_t[i] = millis(); }
      wp_ack(op, pid);
      if (wp_pkts <= 3 || wp_pkts % 20 == 0) {
        ulog("wp: #%lu %uB crc %s oxi %lu/%lu pat %lu chest %lu mot %lu", (unsigned long)wp_pkts, (unsigned)n, crc ? "ok" : "BAD",
             (unsigned long)wp_c.oxia, (unsigned long)wp_c.oxib, (unsigned long)wp_c.pat, (unsigned long)wp_c.chest, (unsigned long)wp_c.motion);
        if (wp_dat) wp_dat.flush();
      }
      return;
    }
    case WP_OP_END_OF_TEST: { uint16_t reason = pkt[18] | (pkt[19] << 8); ulog("wp: END OF TEST, reason %u", reason); if (sess_txt) sess_txt.printf("end of test reason %u\n", reason); wp_ack(op, pid); wp_state = 3; end_prompt("The WatchPAT reported the end of the test.", 300); return; }
    case WP_OP_ERROR: { char h[41]; int k = 0; for (size_t i = 0; i < pn && i < 20; i++) k += snprintf(h + k, sizeof h - k, "%02x", pl[i]); ulog("wp: ERROR status %s", h); wp_ack(op, pid); return; }
    case WP_OP_TECH: { char h[61]; int k = 0; for (size_t i = 0; i < pn && i < 30; i++) k += snprintf(h + k, sizeof h - k, "%02x", pl[i]); ulog("wp: tech status %s", h); if (sess_txt) sess_txt.printf("tech status %s\n", h); wp_ack(op, pid); return; }
    default: Serial.printf("wp: rx opcode %04x id %lu len %u\n", op, (unsigned long)pid, (unsigned)n); wp_ack(op, pid); return;
  }
}
static int wp_get_state() { return wp_state; }
static uint32_t wp_get_serial() { return wp_serial ? wp_serial : cfg_serial; }
static uint32_t wp_get_bytes() { return wp_bytes + wp_pkts * 4; }
static uint32_t wp_get_last() { return wp_t_last; }
#define BYTES_PER_HOUR 2060000UL                      // measured: 552 packets = 315,766 bytes -> about 2.06 MB per hour, 16.5 MB for 8 h
static void wp_user_start() {
  if (!wp_active || wp_state != 4) return;
  sd_stats();
  if (sd_ok && sd_total - sd_used < 12ULL * BYTES_PER_HOUR) { lv_label_set_text(lbl_ready_msg, "The SD card is nearly full.\nFree space for a 12 hour night is needed.\nFormat it in Settings, then try again."); lv_obj_set_style_text_color(lbl_ready_msg, C_RED, 0); return; }
  wp_cmd(WP_OP_START_ACQ); wp_state = 5; wp_t_sent = millis(); ulog("wp: start requested by user");
}
static void wp_user_end() { if (sim_active) { sim_finish(); return; } auto_mode = false; wp_resume = false; ulog("wp: end requested by user"); ble_disconnect(); state = ST_IDLE; ui_state("Recording ended", C_GREY); finish_night(); }
static void wp_poll() {
  if (!wp_active) return;
  size_t n;
  while ((n = wp_next(wp_r, wp_pkt)) > 0) wp_handle(wp_pkt, n);
  if (wp_state == 5 && millis() - wp_t_sent > 10000) { ulog("wp: no answer to start, retrying"); wp_state = 4; wp_user_start(); }
  if (wp_state == 1 && millis() - wp_t_sent > 10000) {
    if (++wp_retries < 3) wp_session_start(); else { ulog("wp: no session confirm, giving up"); wp_state = 0; }
  }
  static uint32_t warned = 0;
  if (wp_state == 2 && millis() - wp_t_last > 60000 && millis() - warned > 60000) { warned = millis(); ulog("wp: no data for %lu s", (unsigned long)((millis() - wp_t_last) / 1000)); }
}

// ---------------------------------------------------------------- UI
static void ui_state(const char *txt, lv_color_t c) {
  if (lbl_state) { lv_label_set_text(lbl_state, txt); lv_obj_set_style_text_color(lbl_state, c, 0); }
  if (lbl_to_state) { lv_label_set_text(lbl_to_state, txt); lv_obj_set_style_text_color(lbl_to_state, c, 0); }
}
static void dev_item_cb(lv_event_t *e) { want_connect = (int)(intptr_t)lv_event_get_user_data(e); }
static void ui_devlist() {
  if (!list_devs) return;
  lv_obj_clean(list_devs);
  int order[24], n = ble_n;
  for (int i = 0; i < n; i++) order[i] = i;
  auto rank = [](const BleDev &d) { return (name_matches(d.name) ? 2000 : d.name[0] ? 1000 : 0) + d.rssi + 200; };
  for (int a = 1; a < n; a++) { int v = order[a], b = a; while (b > 0 && rank(ble_devs[order[b - 1]]) < rank(ble_devs[v])) { order[b] = order[b - 1]; b--; } order[b] = v; }
  for (int k = 0; k < n; k++) {
    int i = order[k];
    char txt[64]; String serial = name_matches(ble_devs[i].name) ? watchpat_serial(ble_devs[i].name) : String("");
    if (serial.length()) snprintf(txt, sizeof txt, "WatchPAT %s  %d", serial.c_str(), ble_devs[i].rssi);
    else snprintf(txt, sizeof txt, "%s  %d", ble_devs[i].name[0] ? ble_devs[i].name : ble_devs[i].addr, ble_devs[i].rssi);
    lv_obj_t *b = lv_list_add_button(list_devs, LV_SYMBOL_BLUETOOTH, txt);
    lv_obj_set_style_pad_ver(b, 6, 0);
    lv_obj_set_style_bg_color(b, name_matches(ble_devs[i].name) ? lv_color_hex(0x0A3A5A) : lv_color_black(), 0);
    lv_obj_set_style_text_color(b, ble_devs[i].name[0] ? lv_color_white() : C_GREY, 0);
    lv_obj_add_event_cb(b, dev_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }
  if (!ble_n) { lv_obj_t *l = lv_list_add_text(list_devs, "No devices found"); lv_obj_set_style_text_color(l, C_DIM, 0); }
  lv_obj_remove_flag(list_devs, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(lbl_log, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(btn_up, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(btn_down, LV_OBJ_FLAG_HIDDEN);
}
static void ui_showlog() {
  if (!list_devs) return;
  lv_obj_clean(list_devs);                        // free the list buttons: the scan list is rebuilt on the next scan
  lv_obj_add_flag(list_devs, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(lbl_log, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(btn_up, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(btn_down, LV_OBJ_FLAG_HIDDEN);
}
static void ui_stats() {
  if (!lbl_stats) return;
  if (state == ST_CONNECTED && wp_active)
    lv_label_set_text_fmt(lbl_stats, "WatchPAT %lu  %s  %lu pkts  %lu KB  %lus ago%s", (unsigned long)wp_serial,
                          wp_state == 2 ? "recording" : wp_state == 4 ? "ready" : wp_state == 5 ? "starting" : wp_state == 1 ? "connecting" : wp_state == 3 ? "END OF TEST" : "idle", (unsigned long)wp_pkts, (unsigned long)(wp_bytes / 1024),
                          (unsigned long)((millis() - wp_t_last) / 1000), wp_crc_bad ? "  CRC!" : "");
  else if (state == ST_CONNECTED)
    lv_label_set_text_fmt(lbl_stats, "s%04d   %lu packets   %lu KB   %d subs%s", sess_no, (unsigned long)sess_pkts, (unsigned long)(sess_bytes / 1024), n_subs, rx_dropped ? "  DROPS" : "");
  else lv_label_set_text_fmt(lbl_stats, "%d sessions on card   last s%04d", cfg_session, cfg_session);
  if (lbl_sd) {
    if (sd_ok) lv_label_set_text_fmt(lbl_sd, "SD %.1f GB, %.1f MB used", sd_total / 1e9, sd_used / 1e6); else lv_label_set_text(lbl_sd, "No SD card");
    lv_obj_set_style_text_color(lbl_sd, sd_ok ? C_GREY : C_RED, 0);
  }
  if (lbl_top_right) lv_label_set_text_fmt(lbl_top_right, "%s  %u KB", state == ST_CONNECTED ? LV_SYMBOL_BLUETOOTH : "", (unsigned)ESP.getFreeHeap() / 1024);
  if (btn_disc) { if (state == ST_CONNECTED || state == ST_CONNECTING) lv_obj_remove_state(btn_disc, LV_STATE_DISABLED); else lv_obj_add_state(btn_disc, LV_STATE_DISABLED); }
  if (btn_scan) {
    lv_label_set_text(lv_obj_get_child(btn_scan, 0), wp_active ? (wp_state == 2 || wp_state == 5 ? "Recording" : "Ready") : "Scan");
    if (wp_active) lv_obj_remove_state(btn_scan, LV_STATE_DISABLED);
    else if (state == ST_SCANNING || state == ST_CONNECTED || state == ST_CONNECTING) lv_obj_add_state(btn_scan, LV_STATE_DISABLED); else lv_obj_remove_state(btn_scan, LV_STATE_DISABLED);
  }
}
static void scan_btn_cb(lv_event_t *e) { if (wp_active) { lv_screen_load(wp_state == 2 || wp_state == 5 ? scr_dash : scr_ready); return; } auto_mode = false; if (state == ST_IDLE || state == ST_WAIT) scan_start(); }
static void disc_btn_cb(lv_event_t *e) { auto_mode = false; wp_resume = false; ble_disconnect(); state = ST_IDLE; ulog("disconnected by user"); scan_start(); }
static void home_back_cb(lv_event_t *e) { turnon_show(); }

static void topbar(lv_obj_t *scr, const char *title) {
  lv_obj_t *bar = lv_obj_create(scr); lv_obj_set_pos(bar, 0, 0); lv_obj_set_size(bar, SCREEN_W, TOP_H); flat(bar, C_TOPBAR);
  label(bar, title, &lv_font_montserrat_16, lv_color_white(), 10, 9);
  if (scr == scr_home) { lbl_top_right = label(bar, "", &lv_font_montserrat_12, C_GREY, 0, 0); lv_obj_align(lbl_top_right, LV_ALIGN_RIGHT_MID, -44, 0); }
}
static void build_home() {
  scr_home = lv_obj_create(NULL); lv_obj_set_style_bg_color(scr_home, C_PANEL, 0);
  topbar(scr_home, "Status"); gear(scr_home);
  lv_obj_t *hit = lv_obj_create(scr_home); lv_obj_set_size(hit, 130, TOP_H); lv_obj_set_pos(hit, 0, 0); lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(hit, 0, 0);
  lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(hit, title_tap_cb, LV_EVENT_CLICKED, NULL);
  lbl_state = label(scr_home, "Starting...", &lv_font_montserrat_16, C_GREY, 10, TOP_H + 6);
  lbl_stats = label(scr_home, "", &lv_font_montserrat_12, C_GREY, 10, TOP_H + 28);
  lbl_sd = label(scr_home, "", &lv_font_montserrat_12, C_GREY, 10, TOP_H + 44);
  lbl_log = label(scr_home, "", &lv_font_montserrat_12, lv_color_white(), 10, TOP_H + 64); lv_obj_set_width(lbl_log, 300);
  list_devs = lv_list_create(scr_home); lv_obj_set_pos(list_devs, 8, TOP_H + 62); lv_obj_set_size(list_devs, 268, 98);
  lv_obj_set_style_bg_color(list_devs, lv_color_black(), 0); lv_obj_set_style_border_width(list_devs, 0, 0); lv_obj_set_style_pad_all(list_devs, 2, 0);
  lv_obj_set_scrollbar_mode(list_devs, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_style_width(list_devs, 8, LV_PART_SCROLLBAR); lv_obj_set_style_bg_color(list_devs, C_ACCENT, LV_PART_SCROLLBAR); lv_obj_set_style_bg_opa(list_devs, LV_OPA_COVER, LV_PART_SCROLLBAR);
  btn_up = button(scr_home, LV_SYMBOL_UP, 282, TOP_H + 62, 30, 46, C_KEY, scroll_cb, (void *)(intptr_t)-1);
  btn_down = button(scr_home, LV_SYMBOL_DOWN, 282, TOP_H + 114, 30, 46, C_KEY, scroll_cb, (void *)(intptr_t)1);
  lv_obj_add_flag(btn_up, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(btn_down, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(list_devs, LV_OBJ_FLAG_HIDDEN);
  btn_scan = button(scr_home, "Scan", 8, 196, 96, 38, C_ACCENT, scan_btn_cb, NULL);
  btn_disc = button(scr_home, "Disconnect", 112, 196, 96, 38, C_RED, disc_btn_cb, NULL);
  button(scr_home, "Back", 216, 196, 96, 38, C_KEY, home_back_cb, NULL);
}

// settings: format, calibrate, forget bonds, restart
static void format_cb(lv_event_t *e) {
  if (!lbl_format) return;
  if (state == ST_CONNECTED) { lv_label_set_text(lbl_format, "Disconnect first"); return; }
  if (millis() > format_armed_until) { format_armed_until = millis() + 6000; lv_label_set_text(lbl_format, "Removes every recording on the card. Tap again to confirm."); lv_obj_set_style_text_color(lbl_format, C_AMBER, 0); return; }
  format_armed_until = 0; lv_label_set_text(lbl_format, "Removing..."); lv_refr_now(NULL);
  int n = sd_wipe();
  lv_label_set_text_fmt(lbl_format, "%d files removed", n); lv_obj_set_style_text_color(lbl_format, C_GREEN, 0);
}
static void date_cb(lv_event_t *e) { date_open(0, NULL); }
static void bonds_cb(lv_event_t *e) { NimBLEDevice::deleteAllBonds(); cfg_last_addr = ""; cfg_save(); if (lbl_format) { lv_label_set_text(lbl_format, "Pairing info cleared"); lv_obj_set_style_text_color(lbl_format, C_GREY, 0); } }
static void restart_cb(lv_event_t *e) { session_close(); delay(100); ESP.restart(); }
static void switch_fw_cb(lv_event_t *e) { session_close(); if (!launcher_switch() && lbl_format) lv_label_set_text(lbl_format, "Other firmware slot is empty"); }

// touch calibration screen (4 targets)
static bool cal_auto = false;
static lv_obj_t *scr_cal, *cal_h, *cal_v, *lbl_cal; static int cal_step; static int cal_rx[4], cal_ry[4]; static bool cal_wait; static lv_timer_t *cal_timer;
static const int CAL_PTS[4][2] = {{20, 20}, {300, 20}, {300, 220}, {20, 220}};
static void cal_place() {
  lv_obj_set_pos(cal_h, CAL_PTS[cal_step][0] - 12, CAL_PTS[cal_step][1] - 1); lv_obj_set_pos(cal_v, CAL_PTS[cal_step][0] - 1, CAL_PTS[cal_step][1] - 12);
  lv_label_set_text_fmt(lbl_cal, "Touch calibration\nTap the centre of the cross  (%d of 4)", cal_step + 1);
}
static void cal_finish(bool ok) {
  calibrating = false; if (cal_timer) { lv_timer_delete(cal_timer); cal_timer = NULL; }
  if (ok) {
    bool sw = abs(cal_ry[1] - cal_ry[0]) > abs(cal_rx[1] - cal_rx[0]);
    if (sw) for (int i = 0; i < 4; i++) { int t = cal_rx[i]; cal_rx[i] = cal_ry[i]; cal_ry[i] = t; }
    int x0 = (cal_rx[0] + cal_rx[3]) / 2, x1 = (cal_rx[1] + cal_rx[2]) / 2, y0 = (cal_ry[0] + cal_ry[1]) / 2, y1 = (cal_ry[2] + cal_ry[3]) / 2;
    if (abs(x1 - x0) < 500 || abs(y1 - y0) < 500) ok = false; else { cal_x0 = x0; cal_x1 = x1; cal_y0 = y0; cal_y1 = y1; cal_swap = sw; cal_have = true; cfg_save(); }
    Serial.printf("cal: %s x %d..%d y %d..%d\n", ok ? "saved" : "rejected", x0, x1, y0, y1);
  }
  if (cal_auto || !scr_settings) welcome_show(); else lv_screen_load(scr_settings);
  lv_obj_delete(scr_cal); scr_cal = NULL;
  if (lbl_format) lv_label_set_text(lbl_format, ok ? "Touch calibration saved" : "Calibration cancelled"); cal_auto = false;
}
static void cal_timer_cb(lv_timer_t *t) {
  if (cal_wait) { if (!cal_pressed) cal_wait = false; return; }
  if (!cal_pressed) return;
  cal_rx[cal_step] = cal_raw_x; cal_ry[cal_step] = cal_raw_y; cal_wait = true; cal_step++;
  if (cal_step >= 4) cal_finish(true); else cal_place();
}
static void cal_cb(lv_event_t *e) {
  scr_cal = lv_obj_create(NULL); lv_obj_set_style_bg_color(scr_cal, lv_color_black(), 0);
  lbl_cal = lv_label_create(scr_cal); lv_obj_set_style_text_font(lbl_cal, &lv_font_montserrat_14, 0); lv_obj_set_style_text_color(lbl_cal, lv_color_white(), 0);
  lv_obj_set_style_text_align(lbl_cal, LV_TEXT_ALIGN_CENTER, 0); lv_obj_center(lbl_cal);
  cal_h = lv_obj_create(scr_cal); lv_obj_set_size(cal_h, 24, 2); flat(cal_h, C_ACCENT);
  cal_v = lv_obj_create(scr_cal); lv_obj_set_size(cal_v, 2, 24); flat(cal_v, C_ACCENT);
  cal_swap = false; cal_step = 0; cal_wait = true; cal_pressed = false; calibrating = true; cal_place(); lv_screen_load(scr_cal);
  cal_timer = lv_timer_create(cal_timer_cb, 30, NULL);
}
// ---------------------------------------------------------------- nights on the card: pick one to see its report (night mode)
static char night_paths[8][40];
static void night_pick_cb(lv_event_t *e) { int i = (int)(intptr_t)lv_event_get_user_data(e); session_close(); pend_set(night_paths[i], false); delay(100); ESP.restart(); }
static void night_send_cb(lv_event_t *e) { session_close(); nv_put("pskip", false); delay(100); ESP.restart(); }
static void nights_fill(lv_obj_t *pg) {              // the Nights tab: up to five recordings, newest first
  struct N { char name[32]; uint32_t size; } ns[5]; int n = 0;
  File d = sd_ok ? SD.open("/SLEEP") : File();
  if (d) {
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
      const char *nm = f.name(); size_t L = strlen(nm);
      if (L < 5 || L >= sizeof ns[0].name || f.size() < 20000 || strcasecmp(nm + L - 4, ".dat") != 0 || strstr(nm, "_report")) continue;
      int i = n < 5 ? n++ : -1;
      if (i < 0) { int w = 0; for (int j = 1; j < 5; j++) if (strcmp(ns[j].name, ns[w].name) < 0) w = j; if (strcmp(ns[w].name, nm) < 0) i = w; }
      if (i >= 0) { strcpy(ns[i].name, nm); ns[i].size = f.size(); }
    }
    d.close();
  }
  for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (strcmp(ns[j].name, ns[i].name) > 0) { N t = ns[i]; ns[i] = ns[j]; ns[j] = t; }
  for (int i = 0; i < n; i++) {
    snprintf(night_paths[i], sizeof night_paths[i], "/SLEEP/%.25s", ns[i].name);
    char t[48]; snprintf(t, sizeof t, "%.*s    %.1f h", (int)strlen(ns[i].name) - 4, ns[i].name, ns[i].size / (float)BYTES_PER_HOUR);
    button(pg, t, 0, i * 30, 296, 27, C_KEY, night_pick_cb, (void *)(intptr_t)i);
  }
  if (!n) label(pg, "No nights recorded yet", &lv_font_montserrat_16, C_GREY, 0, 10);
  int y = n * 30 + 4;
  if (pend_path.length() && !pend_sent && is_nidra()) { button(pg, "Send the last night to the site", 0, y, 296, 30, C_GREEN, night_send_cb, NULL); y += 36; }
  label(pg, n ? "Tap a night: the device restarts, builds its report\nand shows a QR code that opens it on the phone." : "", &lv_font_montserrat_12, C_DIM, 0, y);
}
// ---------------------------------------------------------------- hibernate (Settings > Device)
// With the switch on, the board goes to ESP32 deep sleep after HIB_IDLE_S seconds without a touch while it has nothing to
// do: on the patient screen, in Settings or on the Status screen, with no sensor connected and no scan running. Asleep,
// only the pen interrupt of the touch controller (GPIO 36, an RTC pin) stays armed: a touch on the screen wakes the board,
// which boots again from scratch (the settings are in NVS, the recordings on the card, the clock is kept by the RTC). A
// night that is being recorded is never interrupted: the recorder is not idle then.
#define HIB_IDLE_S 30
static bool hib_idle() {
  if (!cfg_hib || calibrating || sim_active || wp_active || dlg) return false;
  if (state != ST_IDLE && state != ST_WAIT) return false;        // scanning, connecting or connected: never
  lv_obj_t *a = lv_screen_active();
  return a == scr_welcome || a == scr_settings || a == scr_home;
}
static int hib_left() { int left = HIB_IDLE_S - (int)((millis() - hib_last_touch) / 1000); return left < 0 ? 0 : left; }
static void hib_text() {                               // the notice in the top bar of the patient screen
  if (!lbl_hib) return;
  if (hib_idle()) lv_label_set_text_fmt(lbl_hib, "Device turns off in %d s\nSettings > Device turns it off", hib_left());
  else lv_label_set_text(lbl_hib, "Hibernate is on\nSettings > Device turns it off");
}
static void hib_enter() {
  Serial.printf("hibernate: %d s without a touch, deep sleep; a touch on the screen wakes the device\n", HIB_IDLE_S); Serial.flush();
  if (NimBLEDevice::isInitialized()) NimBLEDevice::deinit(true);   // the controller must be off before deep sleep (ESP-IDF sleep modes)
  if (sd_ok) SD.end();
  spk_tone(0);
  tft.writecommand(0x28); tft.writecommand(0x10); delay(120);    // ILI9341: display off, sleep in
  digitalWrite(BL_PIN, LOW);
  gpio_hold_en((gpio_num_t)BL_PIN); gpio_hold_en((gpio_num_t)AMP_EN_PIN); gpio_deep_sleep_hold_en();   // the backlight and the amplifier stay off while asleep
  int x, y, z; xpt_read(x, y, z);                       // leaves the touch controller powered down with its pen interrupt armed
  esp_sleep_enable_ext0_wakeup((gpio_num_t)T_IRQ, 0);  // PENIRQ goes low on a touch
  esp_deep_sleep_start();
}
static void hib_poll() {                                // from loop(), twice a second
  if (!cfg_hib) return;
  hib_text();
  if (hib_idle() && hib_left() == 0) hib_enter();
}
static void hib_wake_release() {                        // at boot: after a hibernate wake the held pins would stay frozen
  gpio_deep_sleep_hold_dis(); gpio_hold_dis((gpio_num_t)BL_PIN); gpio_hold_dis((gpio_num_t)AMP_EN_PIN);
}

// settings: tabs Patient | Wi-Fi | Nights | Device, built when opened and freed on Back (the recording needs the memory)
static int settings_from = 0;                        // 0 patient screen, 1 turn-on, 2 start sleep, 3 recording, 4 status
static lv_obj_t *lbl_pt_name = NULL, *lbl_pt_dob = NULL;
static void settings_leave() { lv_obj_t *s = scr_settings; scr_settings = NULL; lbl_format = lbl_set_info = lbl_pt_name = lbl_pt_dob = btn_format = NULL; if (s) drop_screen(s); }
static void back_cb(lv_event_t *e) {
  if (settings_from == 1 && scr_turnon) turnon_show(); else if (settings_from == 2 && scr_ready) lv_screen_load(scr_ready);
  else if (settings_from == 3 && scr_dash) lv_screen_load(scr_dash); else if (settings_from == 4 && scr_home) lv_screen_load(scr_home); else welcome_show();
  settings_leave();
}
static void patient_btn_label() {                    // also refreshes the name on the turn-on screen and rebuilds the patient screen
  if (lbl_to_who) lv_label_set_text(lbl_to_who, cfg_patient.length() ? cfg_patient.c_str() : "");
  welcome_leave();
  if (lbl_pt_name) lv_label_set_text(lbl_pt_name, cfg_patient.length() ? (String("Name:  ") + cfg_patient).c_str() : "Name:  tap to set");
  if (lbl_pt_dob) lv_label_set_text(lbl_pt_dob, cfg_dob.length() ? (String("Born:  ") + cfg_dob).c_str() : "Born:  tap to set");
}
static void pt_done(bool ok) { if (scr_settings) lv_screen_load(scr_settings); else welcome_show(); patient_btn_label(); }
static void pt_name_cb(lv_event_t *e) { name_done = pt_done; patient_cb(NULL); }
static void pt_dob_cb(lv_event_t *e) { date_open(2, pt_done); }
static void pt_new_cb(lv_event_t *e) { if (state == ST_CONNECTED) { if (lbl_format) lv_label_set_text(lbl_format, "Disconnect first"); return; } wipe_show(); settings_leave(); }
static void wifi_change_cb(lv_event_t *e) { session_close(); nv_put("wifiset", true); delay(100); ESP.restart(); }   // Wi-Fi setup is a separate boot without Bluetooth
static void status_cb(lv_event_t *e) { lv_screen_load(scr_home); settings_leave(); }
static void demo_sw_cb(lv_event_t *e) { cfg_demo = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED); nv_put("demo", cfg_demo); welcome_leave(); Serial.printf("demo mode %s\n", cfg_demo ? "on" : "off"); }
static void hib_sw_cb(lv_event_t *e) { cfg_hib = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED); nv_put("hib", cfg_hib); hib_last_touch = millis(); welcome_leave(); Serial.printf("hibernate %s\n", cfg_hib ? "on" : "off"); }
static void toggle_row(lv_obj_t *pg, const char *txt, int lx, int sx, int y, bool on, lv_event_cb_t cb) {   // a labelled switch in the Device tab: label at lx, switch at sx
  label(pg, txt, &lv_font_montserrat_14, lv_color_white(), lx, y + 8);
  lv_obj_t *sw = lv_switch_create(pg); lv_obj_set_size(sw, 54, 28); lv_obj_set_pos(sw, sx, y);
  lv_obj_set_style_bg_color(sw, C_KEY, 0); lv_obj_set_style_bg_color(sw, C_GREEN, (lv_style_selector_t)LV_PART_INDICATOR | LV_STATE_CHECKED); lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
  if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
}
static lv_obj_t *tab_btn(lv_obj_t *pg, const char *txt, int x, int y, int w, lv_color_t bg, lv_event_cb_t cb) { return button(pg, txt, x, y, w, 36, bg, cb, NULL); }
static void build_settings() {
  scr_settings = lv_obj_create(NULL); lv_obj_set_style_bg_color(scr_settings, C_PANEL, 0); topbar(scr_settings, "Settings");
  button(scr_settings, "Back", 246, 4, 66, 26, C_ACCENT, back_cb, NULL);
  lv_obj_t *tv = lv_tabview_create(scr_settings); lv_obj_set_pos(tv, 0, TOP_H); lv_obj_set_size(tv, SCREEN_W, SCREEN_H - TOP_H);
  lv_tabview_set_tab_bar_size(tv, 34); lv_obj_set_style_bg_color(tv, C_PANEL, 0); lv_obj_set_style_border_width(tv, 0, 0);
  lv_obj_t *pg[4]; const char *names[4] = {"Patient", "Wi-Fi", "Nights", "Device"};
  for (int i = 0; i < 4; i++) { pg[i] = lv_tabview_add_tab(tv, names[i]); lv_obj_set_style_pad_all(pg[i], 8, 0); lv_obj_set_style_bg_color(pg[i], C_PANEL, 0); lv_obj_set_style_bg_opa(pg[i], LV_OPA_COVER, 0); }
  lv_obj_t *bar = lv_tabview_get_tab_bar(tv); lv_obj_set_style_bg_color(bar, C_TOPBAR, 0); lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0); lv_obj_set_style_border_width(bar, 0, 0);
  for (uint32_t i = 0; i < lv_obj_get_child_count(bar); i++) {
    lv_obj_t *b = lv_obj_get_child(bar, i);
    lv_obj_set_style_bg_color(b, C_TOPBAR, 0); lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0); lv_obj_set_style_radius(b, 0, 0); lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_text_color(b, C_GREY, 0); lv_obj_set_style_text_font(b, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(b, lv_color_white(), LV_STATE_CHECKED); lv_obj_set_style_border_color(b, C_ACCENT, LV_STATE_CHECKED);
    lv_obj_set_style_border_side(b, LV_BORDER_SIDE_BOTTOM, LV_STATE_CHECKED); lv_obj_set_style_border_width(b, 3, LV_STATE_CHECKED);
  }
  lv_obj_set_style_bg_color(lv_tabview_get_content(tv), C_PANEL, 0); lv_obj_set_style_bg_opa(lv_tabview_get_content(tv), LV_OPA_COVER, 0);
  // Patient
  lv_obj_t *b = tab_btn(pg[0], "", 0, 0, 296, C_KEY, pt_name_cb); lbl_pt_name = lv_obj_get_child(b, 0); lv_obj_align(lbl_pt_name, LV_ALIGN_LEFT_MID, 8, 0);
  b = tab_btn(pg[0], "", 0, 44, 296, C_KEY, pt_dob_cb); lbl_pt_dob = lv_obj_get_child(b, 0); lv_obj_align(lbl_pt_dob, LV_ALIGN_LEFT_MID, 8, 0);
  patient_btn_label();
  tab_btn(pg[0], "New patient  (removes the recordings)", 0, 100, 296, C_RED, pt_new_cb);
  // Wi-Fi
  lv_obj_t *l = label(pg[1], "", &lv_font_montserrat_14, lv_color_white(), 0, 0); lv_obj_set_width(l, 296); lv_obj_set_style_text_line_space(l, 4, 0);
  lv_label_set_text_fmt(l, "Network:  %s\nSite:  %s", cfg_ssid.length() ? cfg_ssid.c_str() : "not set", is_nidra() ? "Nidra (set)" : cfg_api.length() ? "custom server" : "not set (serial command u)");
  tab_btn(pg[1], cfg_ssid.length() ? "Change the Wi-Fi network" : "Set up Wi-Fi", 0, 52, 296, C_ACCENT, wifi_change_cb);
  label(pg[1], "Wi-Fi gives the date, the report page on your phone\nand the upload to the site. Changing it restarts the device.", &lv_font_montserrat_12, C_DIM, 0, 98);
  // Nights
  nights_fill(pg[2]);
  // Device
  tab_btn(pg[3], "Date & time", 0, 0, 144, C_KEY, date_cb); tab_btn(pg[3], "Calibrate touch", 152, 0, 144, C_KEY, cal_cb);
  btn_format = tab_btn(pg[3], "Reset data", 0, 44, 144, C_RED, format_cb); tab_btn(pg[3], "Restart", 152, 44, 144, C_KEY, restart_cb);
  tab_btn(pg[3], "Status & log", 0, 88, 144, C_KEY, status_cb);
  if (launcher_has_other()) { char sw[48]; snprintf(sw, sizeof sw, "Switch to %s", launcher_other_name()); tab_btn(pg[3], sw, 152, 88, 144, C_KEY, switch_fw_cb); }
  toggle_row(pg[3], "Demo mode", 0, 86, 132, cfg_demo, demo_sw_cb);      // both switches on one row: the tab shows 164 px without scrolling
  toggle_row(pg[3], "Hibernate", 152, 242, 132, cfg_hib, hib_sw_cb);
  lbl_format = label(pg[3], "", &lv_font_montserrat_12, C_GREY, 0, 172); lv_obj_set_width(lbl_format, 296);
  lbl_set_info = label(pg[3], "", &lv_font_montserrat_12, C_DIM, 0, 190); lv_obj_set_width(lbl_set_info, 296);
}
static void settings_cb(lv_event_t *e) {
  lv_obj_t *a = lv_screen_active();
  settings_from = a == scr_turnon ? 1 : a == scr_ready ? 2 : a == scr_dash ? 3 : a == scr_home ? 4 : 0;
  if (scr_settings) settings_leave();
  build_settings(); format_armed_until = 0;
  lv_label_set_text_fmt(lbl_set_info, "SleepCYD %s  -  LVGL %d.%d  -  heap %u KB\n%s%s", FW_VERSION, lv_version_major(), lv_version_minor(), (unsigned)ESP.getFreeHeap() / 1024,
                        sd_ok ? "SD card mounted" : "No SD card", pend_path.length() && !pend_sent ? "  -  one night waits for the site" : "");
  lv_screen_load(scr_settings);
}

// ---------------------------------------------------------------- serial commands
static void list_files() {
  if (!sd_ok) { Serial.println("no sd"); return; }
  File d = SD.open("/SLEEP"); if (!d) return;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) Serial.printf("  %s %lu\n", f.name(), (unsigned long)f.size());
  d.close();
}
static void dump_file(const char *name) {
  String p = String("/SLEEP/") + name; File f = SD.open(p);
  if (!f) { Serial.println("no such file"); return; }
  Serial.printf("--- %s %lu bytes\n", name, (unsigned long)f.size());
  bool text = p.endsWith(".txt");
  uint8_t buf[32]; size_t off = 0;
  while (true) {
    int n = f.read(buf, sizeof buf); if (n <= 0) break;
    if (text) { Serial.write(buf, n); }
    else { Serial.printf("%06x ", (unsigned)off); for (int i = 0; i < n; i++) Serial.printf("%02x", buf[i]); Serial.println(); }
    off += n;
  }
  f.close(); Serial.println("--- end");
}
static void demo_fetch(const char *url);
static void handle_line(char *line) {
  if (line[0] == 'P') { int x, y; if (sscanf(line + 1, "%d,%d", &x, &y) == 2) { fake_x = x; fake_y = y; fake_until = millis() + 120; } }
  else if (line[0] == 'M') {
    Serial.printf("state %d auto %d devs %d conn %s '%s' subs %d | rx %lu pkts %lu B dropped %lu | session s%04d %lu pkts | sd %d %.1f/%.1f MB | heap %u\n",
                  (int)state, auto_mode, ble_n, conn_addr, conn_name, n_subs, (unsigned long)rx_pkts, (unsigned long)rx_bytes, (unsigned long)rx_dropped,
                  sess_no, (unsigned long)sess_pkts, sd_ok, sd_used / 1e6, sd_total / 1e6, (unsigned)ESP.getFreeHeap());
    Serial.printf("heap: free %u, largest block %u, lowest since boot %u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(), (unsigned)ESP.getMinFreeHeap()); Serial.println();
    if (wp_active) Serial.printf("watchpat: state %d serial %lu pkts %lu bytes %lu crc_bad %lu acks %lu | oxiA %lu oxiB %lu pat %lu chest %lu metric %lu motion %lu events %lu other %lu bad %lu | last %lu s ago | %s\n",
                                 wp_state, (unsigned long)wp_serial, (unsigned long)wp_pkts, (unsigned long)wp_bytes, (unsigned long)wp_crc_bad, (unsigned long)wp_acks,
                                 (unsigned long)wp_c.oxia, (unsigned long)wp_c.oxib, (unsigned long)wp_c.pat, (unsigned long)wp_c.chest, (unsigned long)wp_c.metric, (unsigned long)wp_c.motion,
                                 (unsigned long)wp_c.event, (unsigned long)wp_c.other, (unsigned long)wp_c.bad, (unsigned long)((millis() - wp_t_last) / 1000), wp_path);
  }
  else if (line[0] == 'S') { scan_btn_cb(NULL); }
  else if (line[0] == 'A') { auto_mode = true; if (state == ST_IDLE) scan_start(); }
  else if (line[0] == 'C') { int i = atoi(line + 1); if (i >= 0 && i < ble_n) { auto_mode = false; want_connect = i; } else Serial.println("bad index (see scan list)"); }
  else if (line[0] == 'D') { disc_btn_cb(NULL); }
  else if (line[0] == 'Z') {                       // raw touch probe for 10 s: pressure and position every 200 ms
    for (int i = 0; i < 50; i++) {
      int x = 0, y = 0, z = 0; bool d = xpt_read(x, y, z);
      Serial.printf("touch probe: irq %d down %d z %d x %d y %d\n", digitalRead(T_IRQ), d, z, x, y);
      delay(200);
    }
  }
  else if (line[0] == 'H') { cal_auto = false; cal_cb(NULL); }
  else if (line[0] == 'J') { sim_start(); }
  else if (line[0] == 'k') { demo_start(); }
  else if (line[0] == 'w') { String a = line + 1; int c = a.indexOf(','); cfg_ssid = c < 0 ? a : a.substring(0, c); cfg_wpass = c < 0 ? "" : a.substring(c + 1); cfg_save(); Serial.printf("wifi set: '%s'\n", cfg_ssid.c_str()); }
  else if (line[0] == 'u') { cfg_api = line + 1; cfg_api.trim(); cfg_save(); Serial.printf("api url: '%s'\n", cfg_api.c_str()); }
  else if (line[0] == 't') { cfg_tok = line + 1; cfg_tok.trim(); cfg_save(); Serial.printf("api token %s\n", cfg_tok.length() ? "set" : "cleared"); }
  else if (line[0] == 'q') {                         // q <file on the card>: send that recording to the Nidra site (night mode with upload)
    String path = String("/SLEEP/") + (line[1] == ' ' ? line + 2 : line + 1);
    if (!cfg_api.length() || !cfg_ssid.length() || !is_nidra()) Serial.println("set wifi (w) and the Nidra url (u) first");
    else { pend_set(path.c_str(), true); Serial.println("marked for the Nidra site, restarting into night mode"); delay(200); ESP.restart(); }
  }
  else if (line[0] == 'n') { cfg_patient = clean_name(String(line + 1)); cfg_save(); patient_btn_label(); Serial.printf("patient name set: '%s'\n", cfg_patient.c_str()); }
  else if (line[0] == 'o') { cfg_dob = line + 1; cfg_dob.trim(); nv_put("dob", cfg_dob); patient_btn_label(); Serial.printf("date of birth '%s'\n", cfg_dob.c_str()); }
  else if (line[0] == 'i') { nv_put("wifiset", true); Serial.println("restarting into Wi-Fi setup"); delay(200); ESP.restart(); }
  else if (line[0] == 'g') { String u = line + 1; u.trim(); demo_fetch(u.c_str()); }
  else if (line[0] == 'd') { String f = String("/SLEEP/") + (line[1] == ' ' ? line + 2 : line + 1); Serial.printf("delete %s: %d\n", f.c_str(), SD.remove(f)); sd_stats(); }
  else if (line[0] == 'x') { cfg_last_addr = ""; cfg_save(); Serial.println("remembered device cleared"); }
  else if (line[0] == 'G') { wp_user_start(); }
  else if (line[0] == 'E') { wp_user_end(); }
  else if (line[0] == 'K') { if (wp_active) wp_cmd(WP_OP_TECH_REQ); }
  else if (line[0] == 'Y') { if (state == ST_FOUND) dlg_connect_cb(NULL); }
  else if (line[0] == 'N') { if (state == ST_FOUND) dlg_cancel_cb(NULL); }
  else if (line[0] == 'F') { if (state == ST_CONNECTED) Serial.println("disconnect first"); else sd_format(); }
  else if (line[0] == 'L') { list_files(); }
  else if (line[0] == 'b') { bonds_cb(NULL); Serial.println("pairing info cleared"); }
  else if (line[0] == 'y') { cfg_tz = line + 1; cfg_tz.trim(); nv_put("tz", cfg_tz); setenv("TZ", cfg_tz.c_str(), 1); tzset(); time_t t = time(NULL); Serial.printf("time zone '%s', local time now %s", cfg_tz.c_str(), ctime(&t)); }
  else if (line[0] == 'v') { String f = String("/SLEEP/") + (line[1] == ' ' ? line + 2 : line + 1); pend_set(f.c_str(), false); Serial.println("restarting into night mode (report + web page, no upload)"); delay(200); ESP.restart(); }
  else if (line[0] == 'z') { cfg_serial = strtoul(line + 1, NULL, 10); nv_put("serial", cfg_serial); Serial.printf("watchpat serial for re-uploads: %lu\n", (unsigned long)cfg_serial); }
  else if (line[0] == 'R') { dump_file(line + 1 + (line[1] == ' ')); }
  else if (line[0] == 'W') { cfg_filter = line + 1; cfg_filter.trim(); cfg_save(); ulog("filter '%s'", cfg_filter.c_str()); }
  else if (line[0] == 'T') {
    int Y, Mo, D, h, m, s;
    if (sscanf(line + 1, "%d,%d,%d,%d,%d,%d", &Y, &Mo, &D, &h, &m, &s) == 6) {
      struct tm t = {}; t.tm_year = Y - 1900; t.tm_mon = Mo - 1; t.tm_mday = D; t.tm_hour = h; t.tm_min = m; t.tm_sec = s;
      struct timeval tv = {mktime(&t), 0}; settimeofday(&tv, NULL); Serial.println("time set");
    }
  }
  else if (line[0] == 'O') { session_close(); launcher_switch(); }
  else if (line[0] == '?') Serial.println("? help | M status | S manual scan | A auto scan | Y/N found dialog | Cn connect to item n | D disconnect | G start | E end | K tech status\n"
                                          "P x,y tap | H calibrate | Z touch probe | nName | oYYYY-MM-DD birth | wSSID,pass | uURL | tTOKEN | y<tz> | Ty,m,d,h,m,s | i wifi setup\n"
                                          "k demo night | J synthetic night | g <url> fetch DEMO.DAT | v <file> report | q <file> send | L list | R <file> dump | d <file> delete | F format\n"
                                          "Wname filter | x forget device | b forget pairing | z<serial> | O other firmware");
}
static void poll_serial() {
  static char buf[96]; static int n = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') { if (n) { buf[n] = 0; handle_line(buf); n = 0; } }
    else if (n < (int)sizeof buf - 1) buf[n++] = c;
  }
}

// ---------------------------------------------------------------- night mode: a separate boot without Bluetooth
// A finished night is marked in flash (pend). The next boot analyses the file on the card into watchpat_sNNNN_report.json,
// joins Wi-Fi, sends the night to the Nidra site when configured and not yet sent, then serves the report on the home
// network for about three minutes with a QR code of the address. Settings > Nights restarts into this mode for any night.
#include <WebServer.h>
#include <ESPmDNS.h>
#include "webpage.h"
static bool nm_demo = false;                     // night mode on /DEMO.DAT
static bool report_pass(const char *dat, const char *jsonpath, void (*pct)(int)) {   // feed the whole file through the analysis, write the JSON
  File f = SD.open(dat, FILE_READ); if (!f) return false;
  size_t total = f.size(), pos = 0; uint8_t *pkt = (uint8_t *)malloc(WP_MAX); if (!pkt) { f.close(); return false; }
  an_reset(); uint32_t idx = 0; int last = -1; uint32_t t0 = millis();
  while (pos + 4 <= total) {
    uint32_t len = 0; if (f.read((uint8_t *)&len, 4) != 4) break;
    if (len < WP_HDR || len > WP_MAX || pos + 4 + len > total) break;
    if (f.read(pkt, len) != (int)len) break;
    pos += 4 + len;
    if (wp_opcode(pkt) == WP_OP_DATA) { idx++; an_feed(pkt + WP_HDR, len - WP_HDR, idx * 1000); }
    int p = 100 * pos / total; if (p != last) { last = p; if (pct) pct(p); }
  }
  f.close(); free(pkt);
  Serial.printf("report: %lu packets analysed in %lu s\n", (unsigned long)idx, (unsigned long)((millis() - t0) / 1000));
  if (!idx) return false;
  File j = SD.open(jsonpath, FILE_WRITE); if (!j) return false;
  uint32_t secs = an.seconds, tot = 0; for (int i = 0; i < 6; i++) tot += an.pos_secs[i]; float hours = secs / 3600.0f;
  const char *fn = strrchr(dat, '/'); fn = fn ? fn + 1 : dat;
  j.printf("{\"device\":\"SleepCYD\",\"version\":\"%s\",\"patient_name\":\"%s\",\"patient_dob\":\"%s\",\"sensor_serial\":%lu,\"file\":\"%s\",\"bytes\":%lu,\"duration_s\":%lu,\"packets\":%lu",
           FW_VERSION, nm_demo ? "Demo" : cfg_patient.c_str(), nm_demo ? "" : cfg_dob.c_str(), (unsigned long)wp_get_serial(), fn, (unsigned long)total, (unsigned long)secs, (unsigned long)an.packets);
  if (an.spo2_n) j.printf(",\"spo2\":{\"mean\":%.1f,\"min\":%.0f}", an.spo2_sum / an.spo2_n, an.spo2_min); else j.print(",\"spo2\":{\"mean\":null,\"min\":null}");
  if (an.hr_n) j.printf(",\"pulse\":{\"mean\":%.0f,\"min\":%.0f,\"max\":%.0f}", an.hr_sum / an.hr_n, an.hr_min, an.hr_max); else j.print(",\"pulse\":{\"mean\":null,\"min\":null,\"max\":null}");
  j.printf(",\"desaturations\":%lu,\"odi3\":%.1f,\"breathing\":%.0f", (unsigned long)an.desat_count, hours > 0.01f ? an.desat_count / hours : 0.0f, an.resp > 0 ? an.resp : 0.0f);
  { float pahi = hours > 0.01f ? (an.apnea_count + an.hypopnea_count) / hours : 0.0f;
    j.printf(",\"pahi\":%.1f,\"prdi\":%.1f,\"severity\":\"%s\",\"apneas\":%lu,\"hypopneas\":%lu,\"reras\":%lu,\"central\":%lu", pahi, hours > 0.01f ? (an.apnea_count + an.hypopnea_count + an.rera_count) / hours : 0.0f,
             an_severity(pahi), (unsigned long)an.apnea_count, (unsigned long)an.hypopnea_count, (unsigned long)an.rera_count, (unsigned long)an.central_count);
    Serial.printf("report: pAHI %.1f (%s) pRDI %.1f  apneas %lu hypopneas %lu RERA %lu  desats %lu central %lu\n", pahi, an_severity(pahi), hours > 0.01f ? (an.apnea_count + an.hypopnea_count + an.rera_count) / hours : 0.0f,
                  (unsigned long)an.apnea_count, (unsigned long)an.hypopnea_count, (unsigned long)an.rera_count, (unsigned long)an.desat_count, (unsigned long)an.central_count); }
  j.printf(",\"position_pct\":{\"supine\":%lu,\"left\":%lu,\"right\":%lu,\"prone\":%lu,\"upright\":%lu}", (unsigned long)(tot ? an.pos_secs[4] * 100 / tot : 0), (unsigned long)(tot ? an.pos_secs[2] * 100 / tot : 0),
           (unsigned long)(tot ? an.pos_secs[3] * 100 / tot : 0), (unsigned long)(tot ? an.pos_secs[5] * 100 / tot : 0), (unsigned long)(tot ? (an.pos_secs[0] + an.pos_secs[1]) * 100 / tot : 0));
  j.print(",\"position_codes\":[\"Upright\",\"Inverted\",\"Left\",\"Right\",\"Supine\",\"Prone\"],\"per_minute\":{\"spo2\":[");
  for (int i = 0; i < an.trend_n; i++) j.printf(i ? ",%u" : "%u", an.trend[i]);
  j.print("],\"pulse\":["); for (int i = 0; i < an.trend_n; i++) j.printf(i ? ",%u" : "%u", an.hr_trend[i]);
  j.print("],\"position\":["); for (int i = 0; i < an.trend_n; i++) j.printf(i ? ",%u" : "%u", an.pos_trend[i]);
  j.print("]}}"); j.close();
  return true;
}
static WebServer *web = NULL; static uint32_t web_last = 0; static bool web_seen = false; static String nm_dat, nm_json, nm_site; static bool nm_keep_pending = false;
static lv_obj_t *scr_nm, *lbl_nm_status, *lbl_nm_sub, *nm_fill, *lbl_nm_count;
static void nm_status(const char *t) { lv_label_set_text(lbl_nm_status, t); lv_refr_now(NULL); }
static void nm_pct(int p, const char *what) { lv_obj_set_width(nm_fill, 1 + 303 * p / 100); char m[40]; snprintf(m, sizeof m, "%s %d%%", what, p); lv_label_set_text(lbl_nm_status, m); lv_timer_handler(); }
static void nm_pct_up(int p) { nm_pct(p, "Sending to the site"); if (p % 10 == 0) Serial.printf("nidra: %d%%\n", p); }
static void nm_pct_an(int p) { nm_pct(p, "Analysing the night"); if (p % 10 == 0) Serial.printf("report: %d%%\n", p); }
static void nm_finish() {
  if (web) web->stop();
  WiFi.mode(WIFI_OFF);
  if (nm_keep_pending) nv_put("pskip", true); else pend_clear();   // a failed upload stays marked: Settings > Nights > Send
  delay(150); ESP.restart();
}
static void nm_close_cb(lv_event_t *e) { nm_finish(); }
static void nm_retry_cb(lv_event_t *e) { nv_put("pskip", false); delay(150); ESP.restart(); }
static void web_hit(const char *what) { web_last = millis(); web_seen = true; Serial.printf("web: %s from %s (heap %u)\n", what, web->client().remoteIP().toString().c_str(), (unsigned)ESP.getFreeHeap()); }
static void web_root() { web_hit("/"); web->send_P(200, "text/html", WEB_PAGE); }
static void web_json() { web_hit("/report.json"); File f = SD.open(nm_json, FILE_READ); if (!f) { web->send(404, "text/plain", "no report"); return; } web->streamFile(f, "application/json"); f.close(); }
static void web_dat() {
  web_hit("/data.dat"); File f = SD.open(nm_dat, FILE_READ); if (!f) { web->send(404, "text/plain", "no file"); return; }
  const char *n = strrchr(nm_dat.c_str(), '/'); web->sendHeader("Content-Disposition", String("attachment; filename=\"") + (n ? n + 1 : "night.dat") + "\"");
  web->streamFile(f, "application/octet-stream"); f.close();
}
static void web_site() { web_hit("/site"); web->send(200, "text/plain", nm_site); }
static float jnum(const String &j, const char *key, int from = 0) {   // number after "key": in the report JSON, NAN if missing or null
  int k = j.indexOf(key, from); if (k < 0) return NAN; String v = j.substring(k + strlen(key), k + strlen(key) + 12); v.trim();
  return v.startsWith("null") ? NAN : v.toFloat();
}
static void nm_summary(int x) {                          // the night in a few lines beside the QR code (no site link to show there)
  File f = SD.open(nm_json, FILE_READ); if (!f) return;
  String j; char buf[256];
  while (f.available() && j.length() < 3500 && j.indexOf("per_minute") < 0) { int n = f.read((uint8_t *)buf, sizeof buf); j.concat(buf, n); }
  f.close();
  float secs = jnum(j, "\"duration_s\":"); int ks = j.indexOf("\"spo2\":{"), kp = j.indexOf("\"pulse\":{");
  float s_mean = jnum(j, "\"mean\":", ks), s_min = jnum(j, "\"min\":", ks), p_mean = jnum(j, "\"mean\":", kp), p_min = jnum(j, "\"min\":", kp), p_max = jnum(j, "\"max\":", kp);
  float drops = jnum(j, "\"desaturations\":"), odi = jnum(j, "\"odi3\":"), back = jnum(j, "\"supine\":"), pahi = jnum(j, "\"pahi\":");
  char t[220]; int k = snprintf(t, sizeof t, "%.1f h recorded\n", secs / 3600.0f);
  if (!isnan(pahi)) k += snprintf(t + k, sizeof t - k, "pAHI %.1f/h (%s)\n", pahi, an_severity(pahi));
  if (!isnan(s_mean)) k += snprintf(t + k, sizeof t - k, "SpO2 %.1f %%, low %.0f %%\n", s_mean, s_min); else k += snprintf(t + k, sizeof t - k, "SpO2: no signal\n");
  if (!isnan(p_mean)) k += snprintf(t + k, sizeof t - k, "Pulse %.0f bpm\n(%.0f to %.0f)\n", p_mean, p_min, p_max);
  if (!isnan(drops)) k += snprintf(t + k, sizeof t - k, "Drops %.0f, ODI %.1f/h\n", drops, odi);
  if (!isnan(back)) k += snprintf(t + k, sizeof t - k, "On the back %.0f %%", back);
  label(scr_nm, nm_demo ? "Demo night" : "This night", &lv_font_montserrat_14, nm_demo ? C_AMBER : lv_color_white(), x, 32);
  lv_obj_t *l = label(scr_nm, t, &lv_font_montserrat_12, C_GREY, x, 54); lv_obj_set_width(l, 128); lv_obj_set_style_text_line_space(l, 3, 0);
}
static lv_obj_t *nm_qr(const String &url, int x, const char *cap1, const char *cap2) {
  lv_obj_t *q = lv_qrcode_create(scr_nm); lv_qrcode_set_size(q, 116); lv_qrcode_set_dark_color(q, lv_color_black()); lv_qrcode_set_light_color(q, lv_color_white());
  lv_qrcode_update(q, url.c_str(), url.length()); lv_obj_set_pos(q, x, 32); lv_obj_set_style_border_width(q, 4, 0); lv_obj_set_style_border_color(q, lv_color_white(), 0);
  lv_obj_t *c = label(scr_nm, cap1, &lv_font_montserrat_12, lv_color_white(), x - 10, 158); lv_obj_set_width(c, 144); lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_CENTER, 0);
  c = label(scr_nm, cap2, &lv_font_montserrat_12, C_DIM, x - 10, 174); lv_obj_set_width(c, 144); lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_CENTER, 0);
  return q;
}
static void night_mode() {
  if (!pend_path.length()) return;
  if (pend_skip) { nv_put("pskip", false); return; }              // skipped once (no Wi-Fi, "Later"): a normal boot now
  esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);                  // Bluetooth is not used this boot: its memory goes to TLS and the web server
  nm_dat = pend_path; nm_json = pend_path.substring(0, pend_path.length() - 4) + "_report.json"; nm_demo = pend_path == DEMO_PATH;
  scr_nm = lv_obj_create(NULL); lv_obj_set_style_bg_color(scr_nm, C_PANEL, 0);
  label(scr_nm, nm_demo ? "Preparing the demo report" : "Preparing the night report", &lv_font_montserrat_20, nm_demo ? C_AMBER : lv_color_white(), 12, 16);
  lbl_nm_sub = label(scr_nm, nm_demo ? "A night recorded earlier, nothing is sent" : cfg_patient.length() ? cfg_patient.c_str() : "", &lv_font_montserrat_16, C_GREY, 12, 50);
  lbl_nm_status = label(scr_nm, "Reading the card...", &lv_font_montserrat_16, C_AMBER, 12, 96);
  lv_obj_t *track = lv_obj_create(scr_nm); lv_obj_set_pos(track, 8, 130); lv_obj_set_size(track, 304, 10); flat(track, C_KEY); lv_obj_set_style_radius(track, 5, 0);
  nm_fill = lv_obj_create(scr_nm); lv_obj_set_pos(nm_fill, 8, 130); lv_obj_set_size(nm_fill, 1, 10); flat(nm_fill, C_GREEN); lv_obj_set_style_radius(nm_fill, 5, 0);
  label(scr_nm, "Keep the device on. This takes a few minutes.", &lv_font_montserrat_12, C_DIM, 12, 150);
  lv_screen_load(scr_nm); lv_timer_handler();
  sdSpi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS); sd_begin();
  Serial.printf("night mode: %s, heap %u largest %u\n", nm_dat.c_str(), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  bool have_json = false;                                           // a cached report is reused only if it has the apnea numbers (older builds lacked them)
  { File jf = SD.open(nm_json, FILE_READ); if (jf) { char head[700]; int hn = jf.read((uint8_t *)head, sizeof head - 1); head[hn > 0 ? hn : 0] = 0; have_json = strstr(head, "\"pahi\"") != NULL; jf.close(); } }
  if (!have_json) have_json = report_pass(nm_dat.c_str(), nm_json.c_str(), nm_pct_an);
  Serial.printf("night mode: report %s (%s)\n", have_json ? "ready" : "FAILED", nm_json.c_str());
  bool wifi = false;
  if (cfg_ssid.length()) {
    nm_status("Connecting to Wi-Fi..."); WiFi.mode(WIFI_STA); WiFi.begin(cfg_ssid.c_str(), cfg_wpass.c_str());
    uint32_t t0 = millis(); while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) { delay(100); lv_timer_handler(); }
    wifi = WiFi.status() == WL_CONNECTED;
    if (wifi) { WiFi.setSleep(false); Serial.printf("night mode: wifi '%s' ip %s gw %s rssi %d\n", cfg_ssid.c_str(), WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(), WiFi.RSSI()); }
    else Serial.printf("night mode: wifi '%s' not joined (status %d)\n", cfg_ssid.c_str(), (int)WiFi.status());
  }
  bool sent = pend_sent; String rec = pend_rec;
  if (wifi && is_nidra() && !sent) {
    nm_status("Sending to the site 0%");
    int code = nidra_upload(cfg_api, cfg_tok, nm_dat.c_str(), cfg_patient, wp_get_serial(), "SleepCYD " FW_VERSION, nm_status, nm_pct_up, rec);
    sent = code == 200;
    if (sent) { nv_put("psent", true); nv_put("prec", rec); found_beep(); } else Serial.printf("night mode: upload failed (%d)\n", code);
  }
  nm_keep_pending = is_nidra() && !sent;
  if (sent && rec.length()) { int w = cfg_api.indexOf("/webhook/"); if (w > 0) nm_site = cfg_api.substring(0, w) + "/webhook/nidra-signals?recording=" + rec; }
  uint32_t t_start = millis();
  if (!wifi) {                                                      // no network: say so, keep the night marked when an upload is still due
    lv_obj_clean(scr_nm);
    label(scr_nm, cfg_ssid.length() ? "No Wi-Fi found" : "No Wi-Fi configured", &lv_font_montserrat_20, C_AMBER, 12, 16);
    lv_obj_t *l = label(scr_nm, have_json ? "The report is saved on the card.\nConnect the device to Wi-Fi and open\nSettings > Nights to see it on your phone." : "The report could not be built.", &lv_font_montserrat_14, lv_color_white(), 12, 56); lv_obj_set_width(l, 300);
    if (nm_keep_pending) bigbutton(scr_nm, "Retry", 8, 196, 150, 40, C_GREEN, nm_retry_cb, NULL);
    bigbutton(scr_nm, "Close", 206, 196, 106, 40, C_ACCENT, nm_close_cb, NULL);
    while (true) { lv_timer_handler(); poll_serial(); delay(5); if (millis() - t_start > 600000UL) nm_finish(); }
  }
  web = new WebServer(80);
  web->on("/", web_root); web->on("/report.json", web_json); web->on("/data.dat", web_dat); web->on("/site", web_site); web->on("/favicon.ico", []() { web_last = millis(); web->send(204, "text/plain", ""); });
  web->onNotFound([]() { web_hit(web->uri().c_str()); web->send(404, "text/plain", "not found"); });
  web->begin(); MDNS.begin("sleepcyd");
  String url1 = "http://" + WiFi.localIP().toString() + "/";
  lv_obj_clean(scr_nm);
  label(scr_nm, "Scan to open the report", &lv_font_montserrat_16, lv_color_white(), 12, 8);
  nm_qr(url1, 14, url1.c_str(), "phone on the same Wi-Fi");
  if (nm_site.length()) nm_qr(nm_site, 190, "Full report on the site", "from anywhere");
  else if (nm_keep_pending) { lv_obj_t *l = label(scr_nm, "Not sent to the site.\nSettings > Nights > Send\nto try again.", &lv_font_montserrat_12, C_AMBER, 190, 60); lv_obj_set_width(l, 124); }
  else nm_summary(186);
  lbl_nm_count = label(scr_nm, "", &lv_font_montserrat_14, C_AMBER, 12, 208);
  bigbutton(scr_nm, "Close", 206, 196, 106, 40, C_ACCENT, nm_close_cb, NULL);
  Serial.printf("night mode: report at %s  site: %s  heap %u\n", url1.c_str(), nm_site.length() ? nm_site.c_str() : "-", (unsigned)ESP.getFreeHeap());
  web_last = millis(); uint32_t lastc = 0;
  while (true) {                                                    // about three minutes, each request restarts the clock
    web->handleClient(); lv_timer_handler(); poll_serial(); delay(2);
    if (millis() - lastc >= 500) {
      lastc = millis(); int left = (web_seen ? 180 : 300) - (int)((millis() - web_last) / 1000);   // 5 min for the first scan, 3 min idle after a request
      if (left <= 0 || millis() - t_start > 1800000UL) nm_finish();
      lv_label_set_text_fmt(lbl_nm_count, "Closes in %d:%02d", left / 60, left % 60);
    }
  }
}

// ---------------------------------------------------------------- Wi-Fi setup: a separate boot without Bluetooth (Settings > Wi-Fi)
static lv_obj_t *scr_ws = NULL, *ws_ta = NULL, *lbl_ws = NULL; static String ws_ssid, ws_pass; static volatile int ws_step = 0; static bool ws_open = false;
static void ws_pick_cb(lv_event_t *e) { int i = (int)(intptr_t)lv_event_get_user_data(e); ws_ssid = WiFi.SSID(i); ws_open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN; ws_pass = ""; ws_step = ws_open ? 2 : 1; }
static void ws_cancel_cb(lv_event_t *e) { ws_step = 9; }
static void ws_rescan_cb(lv_event_t *e) { ws_step = 0; }
static void ws_retry_cb(lv_event_t *e) { ws_step = 1; }
static void ws_kb_cb(lv_event_t *e) { lv_event_code_t c = lv_event_get_code(e); if (c == LV_EVENT_READY) { ws_pass = lv_textarea_get_text(ws_ta); ws_step = 2; } else if (c == LV_EVENT_CANCEL) ws_step = 0; }
static void ws_screen(const char *title) { lv_obj_clean(scr_ws); ws_ta = lbl_ws = NULL; if (title) topbar(scr_ws, title); }
static void ws_list() {
  ws_screen("Wi-Fi networks"); lbl_ws = label(scr_ws, "Looking for networks...", &lv_font_montserrat_14, C_GREY, 12, TOP_H + 8); lv_refr_now(NULL);
  WiFi.disconnect(); int n = WiFi.scanNetworks(); int order[32], m = 0;
  for (int i = 0; i < n; i++) {
    if (!WiFi.SSID(i).length()) continue;
    bool dup = false; for (int k = 0; k < m; k++) if (WiFi.SSID(order[k]) == WiFi.SSID(i)) { dup = true; if (WiFi.RSSI(i) > WiFi.RSSI(order[k])) order[k] = i; }
    if (dup || m >= 32) continue;
    int k = m++; while (k > 0 && WiFi.RSSI(order[k - 1]) < WiFi.RSSI(i)) { order[k] = order[k - 1]; k--; } order[k] = i;
  }
  Serial.printf("wifi setup: %d networks\n", m);
  lv_label_set_text(lbl_ws, m ? "Tap your network" : "No networks found. Tap Scan again.");
  for (int k = 0; k < m && k < 5; k++) {
    int i = order[k]; char t[56]; snprintf(t, sizeof t, "%s   %d dBm%s", WiFi.SSID(i).c_str(), (int)WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "   open" : "");
    button(scr_ws, t, 8, TOP_H + 30 + k * 27, 304, 25, C_KEY, ws_pick_cb, (void *)(intptr_t)i);
  }
  button(scr_ws, "Scan again", 8, 204, 120, 32, C_KEY, ws_rescan_cb, NULL); button(scr_ws, "Cancel", 216, 204, 96, 32, C_ACCENT, ws_cancel_cb, NULL);
}
// password keyboard: ten keys per row and a row of its own for the mode switch (abc / ABC / 1#), space and OK.
// The key texts "abc", "ABC", "1#", backspace and OK are the ones lv_keyboard's own handler acts on.
#define KB_CTL LV_KEYBOARD_CTRL_BUTTON_FLAGS
static const char *KB_LC[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
                              "a", "s", "d", "f", "g", "h", "j", "k", "l", "-", "\n", "z", "x", "c", "v", "b", "n", "m", ".", "_", LV_SYMBOL_BACKSPACE, "\n",
                              "ABC", "1#", " ", LV_SYMBOL_OK, ""};
static const char *KB_UC[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
                              "A", "S", "D", "F", "G", "H", "J", "K", "L", "-", "\n", "Z", "X", "C", "V", "B", "N", "M", ".", "_", LV_SYMBOL_BACKSPACE, "\n",
                              "abc", "1#", " ", LV_SYMBOL_OK, ""};
static const char *KB_SP[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n", "!", "@", "#", "$", "%", "^", "&", "*", "(", ")", "\n",
                              "-", "_", "+", "=", "[", "]", "{", "}", ";", ":", "\n", "'", "\"", ",", ".", "<", ">", "/", "?", "~", LV_SYMBOL_BACKSPACE, "\n",
                              "abc", "`", " ", LV_SYMBOL_OK, ""};
static const lv_buttonmatrix_ctrl_t KB_CTRL[] = {(lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)(KB_CTL | 1), (lv_buttonmatrix_ctrl_t)(KB_CTL | 3), (lv_buttonmatrix_ctrl_t)(KB_CTL | 2), (lv_buttonmatrix_ctrl_t)7, (lv_buttonmatrix_ctrl_t)(KB_CTL | 3)};
static void ws_password() {
  ws_screen(NULL);
  ws_ta = lv_textarea_create(scr_ws); lv_obj_set_pos(ws_ta, 2, 2); lv_obj_set_size(ws_ta, 316, 32); lv_textarea_set_one_line(ws_ta, true); lv_textarea_set_max_length(ws_ta, 63);
  char ph[48]; snprintf(ph, sizeof ph, "Password for %s", ws_ssid.c_str()); lv_textarea_set_placeholder_text(ws_ta, ph); lv_textarea_set_text(ws_ta, ws_pass.c_str());
  lv_obj_set_style_text_font(ws_ta, &lv_font_montserrat_16, 0); lv_obj_set_style_bg_color(ws_ta, C_PANEL, 0); lv_obj_set_style_text_color(ws_ta, lv_color_white(), 0);
  lv_obj_set_style_pad_ver(ws_ta, 5, 0); lv_obj_set_style_border_color(ws_ta, C_ACCENT, 0); lv_obj_remove_flag(ws_ta, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *kb = lv_keyboard_create(scr_ws); lv_obj_set_size(kb, SCREEN_W, SCREEN_H - 36); lv_obj_align(kb, LV_ALIGN_TOP_LEFT, 0, 36); lv_keyboard_set_textarea(kb, ws_ta);   // the widget comes bottom-aligned: set_pos alone would push it off the screen
  lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, KB_LC, KB_CTRL); lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, KB_UC, KB_CTRL); lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_SPECIAL, KB_SP, KB_CTRL);
  lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_obj_set_style_bg_color(kb, lv_color_black(), 0); lv_obj_set_style_border_width(kb, 0, 0); lv_obj_set_style_pad_all(kb, 1, 0); lv_obj_set_style_pad_gap(kb, 2, 0); lv_obj_set_style_radius(kb, 0, 0);
  lv_obj_set_style_bg_color(kb, C_KEY, LV_PART_ITEMS); lv_obj_set_style_text_color(kb, lv_color_white(), LV_PART_ITEMS); lv_obj_set_style_text_font(kb, &lv_font_montserrat_20, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(kb, C_ACCENT, (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_radius(kb, 4, LV_PART_ITEMS); lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(kb, lv_color_white(), (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_PRESSED); lv_obj_set_style_text_color(kb, lv_color_black(), (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_add_event_cb(kb, ws_kb_cb, LV_EVENT_READY, NULL); lv_obj_add_event_cb(kb, ws_kb_cb, LV_EVENT_CANCEL, NULL);
}
static void ws_connect() {
  ws_screen("Wi-Fi"); lbl_ws = label(scr_ws, "", &lv_font_montserrat_16, C_AMBER, 12, TOP_H + 20); lv_obj_set_width(lbl_ws, 296);
  lv_label_set_text_fmt(lbl_ws, "Connecting to %s...", ws_ssid.c_str()); lv_refr_now(NULL);
  WiFi.disconnect(); delay(100); WiFi.begin(ws_ssid.c_str(), ws_pass.c_str());
  uint32_t t0 = millis(); while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) { delay(100); lv_timer_handler(); }
  if (WiFi.status() == WL_CONNECTED) {
    cfg_ssid = ws_ssid; cfg_wpass = ws_pass; cfg_save();
    configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com"); t0 = millis(); while (!have_date() && millis() - t0 < 5000) { delay(100); lv_timer_handler(); }
    lv_label_set_text_fmt(lbl_ws, "Connected to %s", ws_ssid.c_str()); lv_obj_set_style_text_color(lbl_ws, C_GREEN, 0);
    lv_obj_t *l = label(scr_ws, "", &lv_font_montserrat_14, lv_color_white(), 12, TOP_H + 56); lv_obj_set_width(l, 296);
    time_t t = time(NULL); lv_label_set_text_fmt(l, "Address %s\n%s\nSaved. Restarting...", WiFi.localIP().toString().c_str(), have_date() ? ctime(&t) : "(no date from the network)");
    lv_refr_now(NULL); Serial.printf("wifi setup: '%s' saved, ip %s\n", cfg_ssid.c_str(), WiFi.localIP().toString().c_str());
    delay(2500); ESP.restart();
  }
  lv_label_set_text_fmt(lbl_ws, "Could not connect to %s", ws_ssid.c_str()); lv_obj_set_style_text_color(lbl_ws, C_RED, 0);
  label(scr_ws, ws_open ? "The network did not answer." : "Check the password and try again.", &lv_font_montserrat_14, lv_color_white(), 12, TOP_H + 56);
  button(scr_ws, "Try again", 8, 204, 110, 32, C_GREEN, ws_retry_cb, NULL); button(scr_ws, "Other network", 126, 204, 120, 32, C_KEY, ws_rescan_cb, NULL); button(scr_ws, "Cancel", 254, 204, 58, 32, C_ACCENT, ws_cancel_cb, NULL);
  ws_step = 3;
}
static bool ws_mode = false;
static void demo_fetch(const char *url) {               // g <url>: download a recording to /DEMO.DAT (only in the Wi-Fi setup boot: memory, no Bluetooth)
  if (!ws_mode) { Serial.println("g: first restart into the Wi-Fi setup boot (serial i), then g <url>"); return; }
  if (!sd_ok) { sdSpi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS); if (!sd_begin()) { Serial.println("g: no card"); return; } }
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.begin(cfg_ssid.c_str(), cfg_wpass.c_str()); uint32_t t0 = millis(); while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(100);
    if (WiFi.status() != WL_CONNECTED) { Serial.println("g: no Wi-Fi"); return; }
  }
  HTTPClient http; http.begin(url); http.setTimeout(30000); int code = http.GET();
  if (code != 200) { Serial.printf("g: GET %s -> %d\n", url, code); http.end(); return; }
  int total = http.getSize(); WiFiClient *st = http.getStreamPtr();
  File f = SD.open(DEMO_PATH, FILE_WRITE); if (!f) { Serial.println("g: cannot create the file"); http.end(); return; }
  uint8_t buf[2048]; size_t got = 0; uint32_t t0 = millis(), last = millis(), rep = 0;
  while (http.connected() && (total < 0 || got < (size_t)total)) {
    size_t n = st->available();
    if (!n) { if (millis() - last > 20000) break; delay(1); continue; }
    n = st->readBytes(buf, n > sizeof buf ? sizeof buf : n); f.write(buf, n); got += n; last = millis();
    if (got - rep >= 1048576) { rep = got; Serial.printf("g: %u KB\n", (unsigned)(got / 1024)); }
  }
  f.close(); http.end(); SD.remove(String(DEMO_PATH).substring(0, 5) + "_report.json");
  Serial.printf("g: %u of %d bytes to %s in %lu s\n", (unsigned)got, total, DEMO_PATH, (unsigned long)((millis() - t0) / 1000));
}
static void wifi_setup_mode() {
  prefs.begin("sleep", true); bool go = prefs.getBool("wifiset", false); prefs.end();
  if (!go) return;
  ws_mode = true;
  nv_put("wifiset", false);
  esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
  scr_ws = lv_obj_create(NULL); lv_obj_set_style_bg_color(scr_ws, C_PANEL, 0); lv_obj_remove_flag(scr_ws, LV_OBJ_FLAG_SCROLLABLE); lv_screen_load(scr_ws);
  WiFi.mode(WIFI_STA);
  Serial.printf("wifi setup mode, heap %u\n", (unsigned)ESP.getFreeHeap());
  int shown = -1;
  while (true) {                                     // the buttons only set ws_step; the screens are rebuilt here, outside their callbacks
    if (ws_step != shown) {
      shown = ws_step;
      if (ws_step == 0) ws_list();
      else if (ws_step == 1) { if (ws_open) { ws_step = 2; shown = -1; continue; } ws_password(); }
      else if (ws_step == 2) ws_connect();
      else if (ws_step == 9) { delay(100); ESP.restart(); }
    }
    lv_timer_handler(); poll_serial(); delay(5);
  }
}

RTC_DATA_ATTR static uint32_t boot_sync_tried;   // survives the restart below, not a power-up
static void boot_time_sync() {                   // a few seconds of Wi-Fi at power-up, for the date in the file names, then a clean restart
  if (!cfg_ssid.length() || have_date() || boot_sync_tried == 0xC0FFEE) return;
  boot_sync_tried = 0xC0FFEE;
  ui_state("Getting the date...", C_GREY); lv_timer_handler();
  WiFi.mode(WIFI_STA); WiFi.begin(cfg_ssid.c_str(), cfg_wpass.c_str());
  uint32_t t0 = millis(); while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) { delay(100); lv_timer_handler(); }
  if (WiFi.status() == WL_CONNECTED) { configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com"); while (time(NULL) < 1700000000 && millis() - t0 < 15000) { delay(100); lv_timer_handler(); } }
  time_t t = time(NULL); Serial.printf("time: %s after %lu ms, %s", t > 1700000000 ? "set" : "unknown (no Wi-Fi or NTP)", (unsigned long)(millis() - t0), t > 1700000000 ? ctime(&t) : "\n");
  delay(100); ESP.restart();                     // Wi-Fi leaves about 34 KB less heap even when switched off: boot again without it, the clock is kept
}

// ---------------------------------------------------------------- setup / loop
void setup() {
  Serial.begin(115200);
  hib_wake_release();
  bool woke = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0;   // a touch on the screen ended a hibernate
  if (woke) Serial.println("woke from hibernate by a touch");
  cfg_load();
  setenv("TZ", cfg_tz.c_str(), 1); tzset();
  spk_init();
  xpt_init();
  tft.init(); tft.setRotation(1); tft.fillScreen(TFT_BLACK);
  lv_init(); lv_tick_set_cb(tick_cb);
  lv_display_t *disp = lv_display_create(SCREEN_W, SCREEN_H);
  lv_display_set_flush_cb(disp, flush_cb);
  draw_buf = (uint8_t *)heap_caps_malloc(DRAW_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  lv_display_set_buffers(disp, draw_buf, NULL, DRAW_BUF_SIZE, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_indev_t *indev = lv_indev_create(); lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER); lv_indev_set_read_cb(indev, touch_read);
  night_mode();                                    // a marked night: report, upload, web page, without Bluetooth; returns only when there is none
  wifi_setup_mode();                               // Settings > Wi-Fi: network list and password, without Bluetooth; restarts afterwards
  launcher_scan();                                 // so the settings screen can name the other slot
  build_home(); build_turnon(); build_ready(); build_dash(); lv_screen_load(scr_home);
  if (!cal_have) { cal_auto = true; cal_cb(NULL); }
  launcher_begin(touch_pressed_raw, woke ? NULL : boot_beep);   // boot chirp (not after a hibernate wake), then touch during the splash to pick the other firmware slot
  lv_timer_handler();

  rec_q = xQueueCreate(6, sizeof(Rec));            // 3 KB; the WatchPAT sends ~3 chunks per second
  sdSpi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sd_begin();
  boot_time_sync();
  ble_init();
  if (!sd_ok) ui_state("Insert an SD card and restart", C_RED);
  if (!calibrating) welcome_show();                // New patient | Same patient; scanning starts on the "turn on" screen
  hib_last_touch = millis();
  Serial.printf("ready: SleepCYD %s, free heap %u\n", FW_VERSION, (unsigned)ESP.getFreeHeap());
}

void loop() {
  static uint32_t last_ui = 0;
  lv_timer_handler();
  poll_serial();
  // drain packets from the BLE task to the card
  static Rec r; int drained = 0;
  while (rec_q && drained < 8 && xQueueReceive(rec_q, &r, 0) == pdTRUE) {
    drained++;
    if (wp_active && r.handle == wp_tx_handle && r.kind != 3) { wp_feed(wp_r, r.data, r.len); continue; }
    session_write(r);
    if (sess_pkts <= 20 || sess_pkts % 500 == 0) {
      char hex[25]; int k = 0; for (int i = 0; i < r.len && i < 12; i++) k += snprintf(hex + k, sizeof hex - k, "%02x", r.data[i]);
      ulog("#%lu h%04x %s %dB %s", (unsigned long)sess_pkts, r.handle, r.kind == 3 ? "read" : r.kind == 2 ? "ind" : "ntf", r.len, hex);
    }
  }
  wp_poll(); sim_poll();
  { static uint32_t dt = 0; if (millis() - dt >= 100) { dt = millis(); dash_tick(); } }
  // connection state machine
  if (state == ST_SCANNING && scan_done) {
    scan_done = false;
    if (auto_target >= 0) { int t = auto_target; auto_target = -1; show_found(t); }
    else {
      if (!auto_mode || lv_screen_active() == scr_home) ui_devlist();   // the list costs memory; in auto mode it is hidden anyway
      if (auto_mode) { ui_state(ble_n ? "No WatchPAT yet - tap a device or wait" : "No devices nearby", C_GREY); state = ST_WAIT; state_t0 = millis(); }
      else { ui_state(ble_n ? "Tap a device to connect (list scrolls)" : "No devices found - tap Scan", C_GREY); state = ST_IDLE; }
      ulog("scan: %d devices, no '%s'", ble_n, cfg_filter.c_str());
      for (int i = 0; i < ble_n; i++) Serial.printf("  C%d  %s  '%s'  %d dBm\n", i, ble_devs[i].addr, ble_devs[i].name, ble_devs[i].rssi);
    }
  }
  if (want_connect >= 0) { int i = want_connect; want_connect = -1; if (state != ST_CONNECTED && state != ST_CONNECTING) { if (state == ST_SCANNING) NimBLEDevice::getScan()->stop(); dlg_close(); ui_showlog(); ble_connect(i); } }
  if (state == ST_CONNECTED && disc_flag) {
    ulog("ble: link lost"); bool rec = wp_active && wp_state == 2; if (rec) wp_resume = true; wp_end(false); ble_disconnect(); state = ST_WAIT; state_t0 = millis(); ui_state("Link lost, reconnecting...", C_AMBER); ui_showlog();
    if (rec && end_path[0]) end_prompt("The WatchPAT disconnected. Waiting for it to come back; End keeps what was recorded.", 600);
    else if (lv_screen_active() == scr_ready) turnon_show();                       // it went away before the night started
  }
  if (state == ST_WAIT && auto_mode && millis() - state_t0 > 4000) { ui_showlog(); scan_start(); }
  if (state == ST_FOUND) {
    int left = 30 - (int)((millis() - found_t0) / 1000);
    if (left <= 0) { dlg_close(); want_connect = found_idx; }
    else if (lbl_dlg_timer && dlg) lv_label_set_text_fmt(lbl_dlg_timer, "Connecting automatically in %d s", left);
  }
  if (state == ST_SCANNING && millis() - state_t0 > 9000) { NimBLEDevice::getScan()->stop(); scan_done = true; }
  if (millis() - last_ui > 500) {
    last_ui = millis(); ui_stats(); log_refresh(); hib_poll();
    if (end_auto_at) { int left = (int32_t)(end_auto_at - millis()) / 1000; if (left <= 0) end_now_cb(NULL); else if (lbl_end_timer) lv_label_set_text_fmt(lbl_end_timer, "Ends by itself in %d:%02d", left / 60, left % 60); }
  }
  delay(5);
}
