// CydLauncher - lets several firmwares share one Cheap Yellow Display (4 MB flash, two OTA app slots, see partitions.csv).
//
// Each firmware embeds an ID string in its image with CYD_FW_ID("name", "version") at file scope. At boot the launcher
// reads every app partition in the table, checks that it holds a valid image and searches it for that ID, so the
// menu shows what is really flashed in each slot (name, version, build date), not a remembered name.
//   launcher_begin(pressed_fn)  after LVGL display + touch are up: splash with a touch window, menu on touch
//   launcher_switch(slot)       write the OTA boot record and restart (settings button / serial command)
//   launcher_other_name()       name of the other slot for a "Switch to ..." label (two-slot boards)
// Header-only; needs LVGL 9 and the Arduino ESP32 core (esp_ota_ops, esp_partition).
#pragma once
#include <Arduino.h>
#include <lvgl.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#define CYD_FW_ID(name, ver) extern "C" const char cyd_fw_id[] __attribute__((used)) = "CYD" "FW1:" name ":" ver ":" __DATE__ ";"
extern "C" const char cyd_fw_id[];   // referenced below so the linker keeps it in the image

struct LauncherSlot { const esp_partition_t *part; bool valid, running, identified; char name[32], ver[16], date[16]; uint32_t size; };
static LauncherSlot launcher_slots[4];
static int launcher_n = 0, launcher_running = -1;
static volatile int launcher_choice = -1;

// search the first 1 MB of the slot for the ID string (the read-only data segment comes first in an ESP32 image)
static bool launcher_find_id(LauncherSlot &s) {
  uint8_t *buf = (uint8_t *)malloc(4096 + 96);              // temporary: only needed while the slots are scanned at boot
  if (!buf) return false;
  char magic[8]; snprintf(magic, sizeof magic, "CYD%s", "FW1:");   // assembled at run time: the literal must not appear in this image
  const size_t magic_len = 7;
  size_t limit = min((size_t)s.part->size, (size_t)(1024 * 1024));
  memset(buf, 0, sizeof buf);
  for (size_t off = 0; off < limit; off += 4096) {
    memmove(buf, buf + 4096, 96);                                   // keep the tail of the previous chunk for overlaps
    if (esp_partition_read(s.part, off, buf + 96, 4096) != ESP_OK) { free(buf); return false; }
    for (size_t i = 0; i + magic_len < 4096 + 96; i++) {
      if (buf[i] != 'C' || memcmp(buf + i, magic, magic_len)) continue;
      char tmp[80]; strncpy(tmp, (const char *)buf + i + magic_len, 79); tmp[79] = 0;
      char *semi = strchr(tmp, ';'); if (!semi) continue; *semi = 0;
      char *a = strchr(tmp, ':'); if (!a) continue; *a++ = 0;
      char *b = strchr(a, ':'); if (b) *b++ = 0;
      strncpy(s.name, tmp, 31); strncpy(s.ver, a, 15); strncpy(s.date, b ? b : "", 15);
      free(buf); return true;
    }
  }
  free(buf);
  return false;
}
static void launcher_scan() {
  launcher_n = 0; launcher_running = -1;
  const esp_partition_t *run = esp_ota_get_running_partition();
  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
  uint32_t t0 = millis();
  for (; it && launcher_n < 4; it = esp_partition_next(it)) {
    LauncherSlot &s = launcher_slots[launcher_n];
    memset(&s, 0, sizeof s);
    s.part = esp_partition_get(it);
    s.running = run && s.part->address == run->address;
    esp_app_desc_t desc;
    s.valid = esp_ota_get_partition_description(s.part, &desc) == ESP_OK;
    if (s.valid) {
      s.identified = launcher_find_id(s);
      if (!s.identified) { strncpy(s.name, desc.project_name[0] ? desc.project_name : "unknown firmware", 31); strncpy(s.ver, desc.version, 15); strncpy(s.date, desc.date, 15); }
    } else strcpy(s.name, "(empty)");
    if (s.running) launcher_running = launcher_n;
    launcher_n++;
  }
  esp_partition_iterator_release(it);
  esp_ota_mark_app_valid_cancel_rollback();
  Serial.printf("launcher: %d app slots scanned in %lu ms, this image: %s\n", launcher_n, millis() - t0, cyd_fw_id);
  for (int i = 0; i < launcher_n; i++) {
    LauncherSlot &s = launcher_slots[i];
    Serial.printf("  %s @0x%06x %4u KB: %s %s %s%s%s\n", s.part->label, (unsigned)s.part->address, (unsigned)(s.part->size / 1024),
                  s.name, s.ver, s.date, s.running ? "  [running]" : "", s.valid && !s.identified ? "  (no CYD id)" : "");
  }
}
static int launcher_other_index() { for (int i = 0; i < launcher_n; i++) if (!launcher_slots[i].running && launcher_slots[i].valid) return i; return -1; }   // -1 when no other firmware is installed
static bool launcher_has_other() { return launcher_other_index() >= 0; }
static const char *launcher_other_name() { int i = launcher_other_index(); return i < 0 ? "?" : launcher_slots[i].name; }
// boot the given slot (default: the other one) and restart; false if that slot holds no valid app
static bool launcher_switch(int slot = -1) {
  if (slot < 0) slot = launcher_other_index();
  if (slot < 0 || slot >= launcher_n || !launcher_slots[slot].valid) { Serial.println("launcher: that slot holds no firmware"); return false; }
  esp_err_t e = esp_ota_set_boot_partition(launcher_slots[slot].part);
  Serial.printf("launcher: boot -> %s (%s %s): %s\n", launcher_slots[slot].part->label, launcher_slots[slot].name, launcher_slots[slot].ver, esp_err_to_name(e));
  if (e != ESP_OK) return false;
  delay(100); ESP.restart(); return true;
}

static void launcher_btn_cb(lv_event_t *e) { launcher_choice = (int)(intptr_t)lv_event_get_user_data(e); }
static lv_obj_t *launcher_button(lv_obj_t *scr, int y, lv_color_t bg, int id) {
  lv_obj_t *b = lv_button_create(scr); lv_obj_set_pos(b, 20, y); lv_obj_set_size(b, 280, 50);
  lv_obj_set_style_bg_color(b, bg, 0); lv_obj_set_style_radius(b, 6, 0); lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_bg_color(b, lv_color_white(), LV_STATE_PRESSED); lv_obj_set_style_text_color(b, lv_color_black(), LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(b, lv_color_hex(0x2A2C36), LV_STATE_DISABLED); lv_obj_set_style_text_color(b, lv_color_hex(0x5A5E6E), LV_STATE_DISABLED);
  lv_obj_add_event_cb(b, launcher_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)id);
  lv_obj_set_ext_click_area(b, 10);
  return b;
}
static lv_obj_t *launcher_text(lv_obj_t *parent, const char *txt, const lv_font_t *f, uint32_t color, lv_align_t align, int x, int y) {
  lv_obj_t *l = lv_label_create(parent); lv_label_set_text(l, txt); lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0); lv_obj_align(l, align, x, y); return l;
}
// blocking boot hook: splash with a touch window, then the slot menu if touched. pressed() reads the touch panel directly.
static void launcher_begin(bool (*pressed)(), void (*beep)() = NULL, uint32_t window_ms = 1500) {
  if (!launcher_n) launcher_scan();
  LauncherSlot *me = launcher_running >= 0 ? &launcher_slots[launcher_running] : NULL;
  lv_obj_t *prev = lv_screen_active();
  lv_obj_t *scr = lv_obj_create(NULL); lv_obj_set_style_bg_color(scr, lv_color_hex(0x101218), 0);
  char line[96];
  launcher_text(scr, me ? me->name : "?", &lv_font_montserrat_28, 0xFFFFFF, LV_ALIGN_CENTER, 0, -40);
  snprintf(line, sizeof line, "%s  -  built %s  -  slot %s", me ? me->ver : "", me ? me->date : "", me ? me->part->label : "?");
  launcher_text(scr, line, &lv_font_montserrat_12, 0x969BAA, LV_ALIGN_CENTER, 0, -12);
  int other = launcher_other_index();
  if (other >= 0) {
    snprintf(line, sizeof line, "Also on this device: %s %s", launcher_slots[other].name, launcher_slots[other].ver);
    launcher_text(scr, line, &lv_font_montserrat_12, 0x969BAA, LV_ALIGN_CENTER, 0, 10);
  }
  if (other >= 0) launcher_text(scr, "Touch the screen to choose a firmware", &lv_font_montserrat_14, 0x0096E6, LV_ALIGN_CENTER, 0, 36);
  lv_obj_t *bar = lv_obj_create(scr); lv_obj_set_size(bar, 260, 4); lv_obj_set_style_bg_color(bar, lv_color_hex(0x0096E6), 0);
  lv_obj_set_style_border_width(bar, 0, 0); lv_obj_set_style_radius(bar, 0, 0); lv_obj_align(bar, LV_ALIGN_CENTER, 0, 60);
  lv_screen_load(scr); lv_timer_handler();
  if (beep) beep();
  uint32_t t0 = millis(); bool touched = false;
  while (millis() - t0 < window_ms) {
    lv_obj_set_width(bar, 260 - 260 * (millis() - t0) / window_ms);
    lv_timer_handler();
    if (other >= 0 && pressed()) { touched = true; break; }   // no menu when only one firmware is installed
    delay(10);
  }
  if (touched) {
    Serial.println("launcher: menu");
    lv_obj_clean(scr);
    launcher_text(scr, "Firmware on this device", &lv_font_montserrat_20, 0xFFFFFF, LV_ALIGN_TOP_MID, 0, 12);
    int y = 46 - 58, shown = 0;
    for (int i = 0; i < launcher_n && shown < 3; i++) {
      LauncherSlot &s = launcher_slots[i];
      if (!s.valid) continue;                         // empty slots are not shown
      y += 58; shown++;
      lv_obj_t *b = launcher_button(scr, y, s.running ? lv_color_hex(0x0096E6) : lv_color_hex(0x343848), i);
      snprintf(line, sizeof line, "%s %s", s.name, s.ver);
      lv_obj_t *t = launcher_text(b, line, &lv_font_montserrat_16, 0xFFFFFF, LV_ALIGN_LEFT_MID, 8, -8);
      snprintf(line, sizeof line, "%s  %s  %u KB%s", s.part->label, s.valid ? s.date : "no image", (unsigned)(s.part->size / 1024), s.running ? "  -  running now" : "");
      launcher_text(b, line, &lv_font_montserrat_12, 0xC8CCD8, LV_ALIGN_LEFT_MID, 8, 12);
      if (!s.valid) lv_obj_add_state(b, LV_STATE_DISABLED);
      (void)t;
    }
    launcher_text(scr, "Continues with the running firmware in 20 s", &lv_font_montserrat_12, 0x5A5E6E, LV_ALIGN_BOTTOM_MID, 0, -6);
    launcher_choice = -1; t0 = millis();
    while (launcher_choice < 0 && millis() - t0 < 20000) { lv_timer_handler(); delay(5); }
    if (launcher_choice >= 0 && launcher_choice != launcher_running && launcher_switch(launcher_choice)) { while (true) delay(100); }
  }
  lv_screen_load(prev); lv_obj_delete(scr);
}
