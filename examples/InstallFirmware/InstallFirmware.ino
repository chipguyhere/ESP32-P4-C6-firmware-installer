/*
 * InstallFirmware - firmware installer for ESP32-P4 boards with an ESP32-C6 radio.
 *
 * On two panels it recognizes, it shows what it is doing on the screen:
 *   - Guition JC8012P4A1C (10.1", recognized by its RX8025T clock at I2C 0x32)
 *   - Waveshare ESP32-P4-86-Panel "P486" (recognized by its GT911 touch at 0x5D/0x14)
 * Anything else - including one of those whose I2C didn't answer - gets the
 * same C6 step over Serial0 only, without touching a display.
 *
 *  1. Touch firmware (JC8012 only).  Its touch controller (a GSL3680) needs
 *     ~18 KB of firmware sent at every power-up.  The display libraries read
 *     it from NVS; this puts it there, automatically, when it's missing.
 *
 *  2. C6 firmware (every board).  The ESP32-C6 runs ESP-Hosted firmware that
 *     must match the Arduino core.  If the C6's is older than the one this
 *     sketch carries, it offers to update it: a button on the screen, or 'u'
 *     ('s' skips) over Serial0.  It never installs an older version over a
 *     newer one.  The C6 is expected on SDIO the way both panels wire it
 *     (CLK 18, CMD 19, D0-D3 14-17, reset 54: the core's defaults).
 *
 * Over Serial0, 'e' erases the touch firmware and restarts (to test step 1).
 *
 * Upload it, follow the screen or Serial0 at 115200, then upload your own
 * sketch: both firmwares stay put through later uploads.  Only erasing the
 * whole flash removes the touch firmware; then run this again.
 *
 * Board settings (sketch.yaml): ESP32P4 Dev Module, PSRAM enabled, Flash Size
 * 16 MB, Partition Scheme Custom (the partitions.csv here: the same layout as
 * the display libraries' examples, so NVS is never moved).  Needs LVGL 9 and
 * chipguy_P486_or_JC8012_display.
 *
 * Licenses: the touch firmware (gsl3680_firmware_blob.h) is Silead's, GPL v2;
 * the C6 firmware (c6_firmware.h) is Espressif's ESP-Hosted, Apache 2.0.  So
 * a build of this sketch is GPL - see the README.  Your own sketches only read
 * the touch firmware from NVS and don't contain it.
 */

#include "lv_conf.h"
#include "lvgl.h"
#include "lv_setup.hpp"
#include <ESP32P4C6_FirmwareInstaller.h>
#include "esp32-hal-hosted.h"
#include "gsl3680_firmware_blob.h"
#include "c6_firmware.h"

// Above the first function: arduino-cli puts its generated prototypes there.
enum Step : uint8_t { STEP_TOUCH, STEP_C6, STEP_ASK, STEP_UPDATE, STEP_DONE, STEP_IDLE };
static Step step = STEP_TOUCH;
enum Answer : int8_t { ANSWER_NONE, ANSWER_UPDATE, ANSWER_SKIP };
static volatile Answer answer = ANSWER_NONE;

static P4Board board = P4Board::Unknown;   // Unknown: serial only, no display
static bool ui = false;

// ── Which board, by I2C only ───────────────────────────────────────────────
// The library's detection may answer from NVS or from the flash size; the
// screen is only started when the I2C bus itself says which panel this is.
static P4Board detectByI2c()
{
    P4Board lib = chipguy_p4panel_detect();
    ThreadsafeWire &wire = p4panel_wire();
    // Detection skips the bus when NVS already has its answer: start it here.
    if (!wire.begin()) { Serial0.println("I2C bus on GPIO7/8 did not start"); return P4Board::Unknown; }
    wire.setClock(400000);
    bool rtc   = wire.probe(JC8012Pins::RX8025T_ADDR);
    bool gt911 = wire.probe(gt911_touch::ADDR_PRIMARY) || wire.probe(gt911_touch::ADDR_SECONDARY);
    P4Board byI2c = (rtc && !gt911) ? P4Board::JC8012
                  : (gt911 && !rtc) ? P4Board::P486
                  : P4Board::Unknown;
    Serial0.printf("I2C: RTC@0x32 %s, GT911 %s; library says %s (by %s)\n",
                   rtc ? "yes" : "no", gt911 ? "yes" : "no", chipguy_p4panel_name(lib),
                   chipguy_p4panel_source_name(chipguy_p4panel_detect_source()));
    return (byI2c == lib) ? byI2c : P4Board::Unknown;
}

// The names shown for each board.
static const char *boardTitle(P4Board b)
{
    switch (b) {
        case P4Board::JC8012: return "JC8012P4A1*";
        case P4Board::P486:   return "ESP32-P4-86-Panel-ETH-2RO or ESP32-P4-WIFI6-Touch-LCD-4B";
        default:              return "unrecognized board";
    }
}

// ── Output: Serial0 always, the screen when there is one ───────────────────
static lv_obj_t *logLabel, *logBox, *bar, *barLabel, *btnRow;
static String logText;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    Serial0.print(buf);
    if (!ui) return;
    logText += buf;
    lv_label_set_text(logLabel, logText.c_str());
    lv_obj_scroll_to_y(logBox, LV_COORD_MAX, LV_ANIM_OFF);
    lv_timer_handler();
}

static void onButton(lv_event_t *e)
{
    answer = (Answer)(intptr_t)lv_event_get_user_data(e);
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_color_t color, Answer a)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 200, 64);
    lv_obj_set_style_bg_color(b, color, 0);
    lv_obj_add_event_cb(b, onButton, LV_EVENT_CLICKED, (void *)(intptr_t)a);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_center(l);
    return b;
}

// A 480x480 canvas, scaled up to fill either panel.
static void buildScreen()
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_text_color(scr, lv_color_white(), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Firmware installer");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_palette_main(LV_PALETTE_CYAN), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *name = lv_label_create(scr);           // the P486's two names take two lines
    lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(name, 456);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(name, boardTitle(board));
    lv_obj_set_style_text_font(name, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(name, lv_palette_main(LV_PALETTE_CYAN), 0);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 34);

    logBox = lv_obj_create(scr);
    lv_obj_set_size(logBox, 456, 262);
    lv_obj_align(logBox, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_style_bg_color(logBox, lv_color_hex(0x101418), 0);
    lv_obj_set_style_border_width(logBox, 0, 0);
    logLabel = lv_label_create(logBox);
    lv_label_set_long_mode(logLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(logLabel, lv_pct(100));
    lv_obj_set_style_text_font(logLabel, &lv_font_montserrat_16, 0);
    // Explicit: the default theme's text color is dark, unreadable on this box.
    lv_obj_set_style_text_color(logLabel, lv_color_white(), 0);
    lv_label_set_text(logLabel, "");

    bar = lv_bar_create(scr);
    lv_obj_set_size(bar, 456, 22);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 352);
    lv_bar_set_range(bar, 0, 100);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
    barLabel = lv_label_create(scr);
    lv_obj_align(barLabel, LV_ALIGN_TOP_MID, 0, 378);
    lv_obj_set_style_text_color(barLabel, lv_color_white(), 0);
    lv_label_set_text(barLabel, "");

    btnRow = lv_obj_create(scr);
    lv_obj_set_size(btnRow, 456, 84);
    lv_obj_align(btnRow, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_opa(btnRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btnRow, 0, 0);
    lv_obj_set_flex_flow(btnRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btnRow, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    button(btnRow, "Update C6", lv_palette_main(LV_PALETTE_GREEN), ANSWER_UPDATE);
    button(btnRow, "Skip", lv_palette_main(LV_PALETTE_GREY), ANSWER_SKIP);
    lv_obj_add_flag(btnRow, LV_OBJ_FLAG_HIDDEN);
}

// ── 1. Touch firmware (JC8012 only) ────────────────────────────────────────
static void touchStep()
{
    if (board != P4Board::JC8012) return;      // the P486's GT911 needs none; unknown boards: hands off
    say("1. Touch firmware\n");
    gsl3680_touch::Status st = display.getGsl3680().status();
    if (st == gsl3680_touch::OK) {
        say("   Installed. Touch is working.\n");
        return;
    }
    if (st != gsl3680_touch::NO_FIRMWARE && st != gsl3680_touch::BAD_FIRMWARE) {
        // The firmware is there and checks out; the controller is the problem.
        say("   Firmware is installed, but: %s.\n", gsl3680_touch::statusText(st));
        return;
    }
    say("   Not installed yet. Installing...\n");
    p4c6fw::Result r = p4c6fw::storeBlob(gsl3680_touch::NVS_NAMESPACE, gsl3680_touch::NVS_KEY,
                                         GSL3680_BLOB, sizeof(GSL3680_BLOB),
                                         GSL3680_IMAGE_BYTES, GSL3680_IMAGE_SHA256, Serial0);
    say("   %s.\n", p4c6fw::resultText(r));
    if (r != p4c6fw::INSTALLED && r != p4c6fw::ALREADY_INSTALLED) return;
    if (display.restartTouch()) say("   Touch is working.\n");
    else say("   Touch did not start: %s.\n", gsl3680_touch::statusText(display.getGsl3680().status()));
}

// ── 2. C6 firmware (every board) ───────────────────────────────────────────
static uint32_t ver(uint32_t a, uint32_t b, uint32_t c) { return (a << 16) | (b << 8) | c; }

// Returns true if an update should be offered.
static bool c6Step()
{
    say(board == P4Board::JC8012 ? "\n2. Wi-Fi chip (ESP32-C6) firmware\n" : "Wi-Fi chip (ESP32-C6) firmware\n");
    if (!hostedInitWiFi()) {
        say("   The C6 did not answer on SDIO.\n");
        return false;
    }
    uint32_t ma = 0, mi = 0, pa = 0, hma = 0, hmi = 0, hpa = 0;
    hostedGetSlaveVersion(&ma, &mi, &pa);
    hostedGetHostVersion(&hma, &hmi, &hpa);
    uint32_t have = ver(ma, mi, pa), ours = ver(C6_FW_MAJOR, C6_FW_MINOR, C6_FW_PATCH);

    if (have == 0) say("   On the C6: too old to report its version\n");
    else           say("   On the C6: %lu.%lu.%lu\n", (unsigned long)ma, (unsigned long)mi, (unsigned long)pa);
    say("   In this installer: " C6_FW_VERSION "\n");
    say("   This Arduino core expects: %lu.%lu.%lu\n",
        (unsigned long)hma, (unsigned long)hmi, (unsigned long)hpa);

    if (have == ours) { say("   Up to date.\n"); return false; }
    if (have > ours) {
        say("   The C6 has newer firmware; leaving it alone.\n");
        if (ma >= 3) say("   Note: Arduino core 3.3 can't use ESP-Hosted 3.x; Wi-Fi won't work in Arduino sketches.\n");
        return false;
    }
    say("   An update is available.\n");
    Serial0.println("   Type 'u' to update, 's' to skip.");   // serial only: the screen has buttons
    return true;
}

static void setProgress(int pct)
{
    if (ui) {
        lv_bar_set_value(bar, pct, LV_ANIM_OFF);
        lv_label_set_text_fmt(barLabel, "%d%%", pct);
        lv_timer_handler();
    }
    if (pct % 10 == 0) Serial0.printf("   %d%%\n", pct);
}

static bool updateC6()
{
    uint8_t sha[32];
    mbedtls_sha256(C6_FIRMWARE, sizeof(C6_FIRMWARE), sha, 0);
    if (memcmp(sha, C6_FW_SHA256, 32) != 0) {
        say("   The C6 firmware in this sketch is damaged. Rebuild it.\n");
        return false;
    }
    say("   Updating. Keep the board powered.\n");
    if (!hostedBeginUpdate()) { say("   The C6 refused the update.\n"); return false; }

    if (ui) lv_obj_clear_flag(bar, LV_OBJ_FLAG_HIDDEN);
    static uint8_t chunk[4096];                      // the write call wants RAM, not flash
    const uint32_t total = sizeof(C6_FIRMWARE);
    int lastPct = -1;
    for (uint32_t off = 0; off < total; off += sizeof(chunk)) {
        uint32_t n = total - off < sizeof(chunk) ? total - off : sizeof(chunk);
        memcpy(chunk, C6_FIRMWARE + off, n);
        if (!hostedWriteUpdate(chunk, n)) {
            say("   Writing failed at %lu bytes.\n", (unsigned long)off);
            return false;
        }
        int pct = (int)((uint64_t)(off + n) * 100 / total);
        if (pct != lastPct) { setProgress(pct); lastPct = pct; }
    }
    if (!hostedEndUpdate()) { say("   The C6 rejected the image.\n"); return false; }
    hostedActivateUpdate();     // older C6 firmware has no "activate"; the core allows for that
    return true;
}

void setup()
{
    Serial0.begin(115200);
    delay(300);
    Serial0.println("\nESP32-P4 C6 firmware installer");
    board = detectByI2c();
    if (board != P4Board::Unknown) {
        lv_setup.begin({0, P4Res::Square480},     // JC8012: 480x480, scaled to 780x780
                       {0, P4Res::Square480});    // P486:   480x480, scaled to 720x720
        buildScreen();
        ui = true;
        lv_timer_handler();
        Serial0.printf("Panel: %s, using its screen\n", boardTitle(board));
    } else {
        Serial0.println("No supported panel recognized by I2C: serial only, no display.");
    }
}

void loop()
{
    int c = Serial0.read();
    if (c == 'u') answer = ANSWER_UPDATE;
    if (c == 's') answer = ANSWER_SKIP;
    if (c == 'e' && board == P4Board::JC8012) {   // for testing: remove the touch firmware, then restart
        bool ok = p4c6fw::eraseBlob(gsl3680_touch::NVS_NAMESPACE, gsl3680_touch::NVS_KEY);
        Serial0.println(ok ? "Touch firmware erased. Restarting." : "Erase failed.");
        delay(200);
        if (ok) ESP.restart();
    }

    switch (step) {
        case STEP_TOUCH:
            touchStep();
            step = STEP_C6;
            break;
        case STEP_C6:
            if (c6Step()) {
                if (ui) lv_obj_clear_flag(btnRow, LV_OBJ_FLAG_HIDDEN);
                answer = ANSWER_NONE;
                step = STEP_ASK;
            } else {
                step = STEP_DONE;
            }
            break;
        case STEP_ASK:
            if (answer == ANSWER_NONE) break;
            if (ui) lv_obj_add_flag(btnRow, LV_OBJ_FLAG_HIDDEN);
            if (answer == ANSWER_SKIP) { say("   Skipped.\n"); step = STEP_DONE; break; }
            step = STEP_UPDATE;
            break;
        case STEP_UPDATE:
            if (updateC6()) {
                say("   Done. Restarting in 5 s to start it...\n");
                uint32_t t0 = millis();
                while (millis() - t0 < 5000) { if (ui) lv_timer_handler(); delay(20); }
                ESP.restart();
            }
            step = STEP_DONE;
            break;
        case STEP_DONE:
            say("\nAll done. You can upload your own sketch now.\n");
            step = STEP_IDLE;
            break;
        case STEP_IDLE:
            break;
    }
    if (ui) lv_timer_handler();
    delay(5);
}
