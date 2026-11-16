/*
 * InstallFirmware - firmware installer for ESP32-P4 boards with an ESP32-C6 radio.
 *
 * On two panels it recognizes, it shows what it is doing on the screen:
 *   - Guition JC8012P4A1* (10.1", recognized by its RX8025T clock at I2C 0x32)
 *   - Waveshare ESP32-P4-86-Panel-ETH-2RO / ESP32-P4-WIFI6-Touch-LCD-4B
 *     (recognized by the GT911 touch at 0x5D/0x14)
 * Anything else - including one of those whose I2C didn't answer - gets the
 * same C6 step over Serial0 only, without touching a display.
 *
 *  1. Touch firmware (JC8012 only).  Its touch controller (a GSL3680) needs
 *     ~18 KB of firmware sent at every power-up.  The display libraries read
 *     it from NVS; this puts it there, automatically, when it's missing.
 *
 *  2. C6 firmware (every board).  The ESP32-C6 runs ESP-Hosted firmware.  The
 *     default is 2.12.13, which matches the Arduino core:
 *       older on the C6 (or no version reported)  -> offered as an update
 *       2.12.13 on the C6                          -> up to date
 *       3.1.0 on the C6 (see Advanced)             -> "Restore 2.12.13 for Arduino"
 *       anything else newer                        -> offered only with a warning:
 *           this installer has no copy of that version to put back
 *     Advanced offers ESP-Hosted 3.1.0 (Wi-Fi + Bluetooth), for ESP-IDF projects
 *     built with ESP-Hosted 3.  Arduino sketches can't use it; this installer
 *     can put 2.12.13 back (tested).
 *     The C6 is expected on SDIO the way both panels wire it (CLK 18, CMD 19,
 *     D0-D3 14-17, reset 54: the core's defaults).
 *
 * Over Serial0 (115200): 'u' takes the offer, 's' skips it, 'a' opens Advanced
 * ('y' installs, 'n' goes back), 'e' erases the touch firmware and restarts
 * (to test step 1).
 *
 * Board settings (sketch.yaml): ESP32P4 Dev Module, PSRAM enabled, Flash Size
 * 16 MB, Partition Scheme Custom (the partitions.csv here: the same layout as
 * the display libraries' examples, so NVS is never moved).  Needs LVGL 9 and
 * chipguy_P486_or_JC8012_display.
 *
 * Licenses: the touch firmware (gsl3680_firmware_blob.h) is Silead's, GPL v2;
 * the C6 firmwares (c6_firmware.h, c6_firmware_3x.h) are Espressif's
 * ESP-Hosted, Apache 2.0.  So a build of this sketch is GPL - see the README.
 * Your own sketches only read the touch firmware from NVS and don't contain it.
 */

#include "lv_conf.h"
#include "lvgl.h"
#include "lv_setup.hpp"
#include <ESP32P4C6_FirmwareInstaller.h>
#include "esp32-hal-hosted.h"
#include "gsl3680_firmware_blob.h"
#include "c6_firmware.h"        // C6_*:  2.12.13, the default
#include "c6_firmware_3x.h"     // C6X_*: 3.1.0, Advanced

// Above the first function: arduino-cli puts its generated prototypes there.
struct C6Image { const uint8_t *data; uint32_t len; const uint8_t *sha; const char *version; uint32_t ver; };
enum Step : uint8_t { STEP_TOUCH, STEP_C6, STEP_WAIT, STEP_UPDATE };
enum Answer : int8_t { ANSWER_NONE, MAIN_YES, MAIN_NO, ADV_OPEN, ADV_YES, ADV_NO };

static Step step = STEP_TOUCH;
static volatile Answer answer = ANSWER_NONE;
static P4Board board = P4Board::Unknown;   // Unknown: serial only, no display
static bool ui = false;
static bool c6Ok = false;                  // the C6 answered and reported in
static uint32_t have = 0;                  // the C6's version, (major<<16)|(minor<<8)|patch
static char haveText[24] = "?";
static const C6Image *mainOffer = nullptr; // what the main buttons install, if anything
static const C6Image *target = nullptr;    // what STEP_UPDATE installs
static bool advOpen = false;

static uint32_t ver(uint32_t a, uint32_t b, uint32_t c) { return (a << 16) | (b << 8) | c; }
static const C6Image IMG_2X = {C6_FIRMWARE, sizeof(C6_FIRMWARE), C6_FW_SHA256, C6_FW_VERSION,
                               (C6_FW_MAJOR << 16) | (C6_FW_MINOR << 8) | C6_FW_PATCH};
static const C6Image IMG_3X = {C6X_FIRMWARE, sizeof(C6X_FIRMWARE), C6X_FW_SHA256, C6X_FW_VERSION,
                               (C6X_FW_MAJOR << 16) | (C6X_FW_MINOR << 8) | C6X_FW_PATCH};

// ── Which board, by I2C only ───────────────────────────────────────────────
// The library's detection may answer from NVS or from the flash size; the
// screen is only started when the I2C bus itself says which panel this is.

// A touch controller caught mid-transfer by a warm reset can hold SDA low,
// and then nothing answers.  Nine clocks with SDA released, then a STOP,
// lets it finish its byte and let go.
static void i2cBusClear()
{
    const int SDA = P4PanelCommonPins::SDA, SCL = P4PanelCommonPins::SCL;
    pinMode(SDA, INPUT_PULLUP);
    pinMode(SCL, OUTPUT_OPEN_DRAIN);
    digitalWrite(SCL, HIGH);
    delayMicroseconds(10);
    for (int i = 0; i < 9; i++) {
        digitalWrite(SCL, LOW);  delayMicroseconds(10);
        digitalWrite(SCL, HIGH); delayMicroseconds(10);
    }
    pinMode(SDA, OUTPUT_OPEN_DRAIN);                 // STOP: SDA low -> high while SCL high
    digitalWrite(SDA, LOW);  delayMicroseconds(10);
    digitalWrite(SDA, HIGH); delayMicroseconds(10);
    pinMode(SDA, INPUT);
    pinMode(SCL, INPUT);
}

static P4Board detectByI2c()
{
    P4Board lib = chipguy_p4panel_detect();
    ThreadsafeWire &wire = p4panel_wire();
    bool rtc = false, gt911 = false;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) {                           // nothing answered: unstick the bus, try again
            wire.end();
            i2cBusClear();
            delay(20);
        }
        // Detection skips the bus when NVS already has its answer: start it here.
        if (!wire.begin()) continue;
        wire.setClock(400000);
        rtc   = wire.probe(JC8012Pins::RX8025T_ADDR);
        gt911 = wire.probe(gt911_touch::ADDR_PRIMARY) || wire.probe(gt911_touch::ADDR_SECONDARY);
        if (rtc || gt911) break;
    }
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
static lv_obj_t *logLabel, *logBox, *bar, *barLabel, *btnRow, *btnYes, *btnNo, *btnAdv;
static lv_obj_t *advPanel, *advText, *advYes;
static String logText;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    char buf[400];
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

static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_color_t color, Answer a,
                        int w = 200, int h = 64, const lv_font_t *font = &lv_font_montserrat_20)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, color, 0);
    lv_obj_add_event_cb(b, onButton, LV_EVENT_CLICKED, (void *)(intptr_t)a);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_center(l);
    return b;
}

static void setButton(lv_obj_t *b, const char *text, lv_color_t color)
{
    lv_label_set_text(lv_obj_get_child(b, 0), text);
    lv_obj_set_style_bg_color(b, color, 0);
}

static lv_color_t GREEN()  { return lv_palette_main(LV_PALETTE_GREEN); }
static lv_color_t ORANGE() { return lv_palette_main(LV_PALETTE_DEEP_ORANGE); }
static lv_color_t GREY()   { return lv_palette_main(LV_PALETTE_GREY); }

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

    btnAdv = button(scr, "Advanced", GREY(), ADV_OPEN, 96, 30, &lv_font_montserrat_16);
    lv_obj_align(btnAdv, LV_ALIGN_TOP_RIGHT, -6, 4);
    lv_obj_add_flag(btnAdv, LV_OBJ_FLAG_HIDDEN);

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
    btnYes = button(btnRow, "Update C6", GREEN(), MAIN_YES);
    btnNo  = button(btnRow, "Skip", GREY(), MAIN_NO);
    lv_obj_add_flag(btnRow, LV_OBJ_FLAG_HIDDEN);

    // The Advanced page: a panel over everything, hidden until asked for.
    advPanel = lv_obj_create(scr);
    lv_obj_set_size(advPanel, 480, 480);
    lv_obj_center(advPanel);
    lv_obj_set_style_bg_color(advPanel, lv_color_hex(0x181018), 0);
    lv_obj_set_style_border_width(advPanel, 0, 0);
    lv_obj_t *advTitle = lv_label_create(advPanel);
    lv_label_set_text(advTitle, "Advanced: ESP-Hosted " C6X_FW_VERSION);
    lv_obj_set_style_text_font(advTitle, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(advTitle, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_align(advTitle, LV_ALIGN_TOP_MID, 0, 4);
    advText = lv_label_create(advPanel);
    lv_label_set_long_mode(advText, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(advText, 440);
    lv_obj_set_style_text_font(advText, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(advText, lv_color_white(), 0);
    lv_obj_align(advText, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_t *advRow = lv_obj_create(advPanel);
    lv_obj_set_size(advRow, 456, 84);
    lv_obj_align(advRow, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(advRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(advRow, 0, 0);
    lv_obj_set_flex_flow(advRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(advRow, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    advYes = button(advRow, "Install " C6X_FW_VERSION, ORANGE(), ADV_YES);
    button(advRow, "Back", GREY(), ADV_NO);
    lv_obj_add_flag(advPanel, LV_OBJ_FLAG_HIDDEN);
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

// The "no way back" warning: installing `img` replaces a newer version this
// installer carries no copy of.
static String noWayBack(const C6Image &img)
{
    String s = "   The C6 has ";
    s += haveText;
    s += ", newer than ";
    s += img.version;
    s += ". Installing ";
    s += img.version;
    s += " replaces it, and this installer has no copy of ";
    s += haveText;
    s += " to put back.";
    if ((have >> 16) >= 3 && img.ver < ver(3, 0, 0))
        s += " Going back from ESP-Hosted 3 to 2 was tested only from " C6X_FW_VERSION ".";
    s += " Accept the risk, or leave it and use an installer that carries your version.\n";
    return s;
}

static void offerMain(const char *yes, lv_color_t color, const char *no)
{
    if (ui) {
        setButton(btnYes, yes, color);
        setButton(btnNo, no, GREY());
        lv_obj_clear_flag(btnRow, LV_OBJ_FLAG_HIDDEN);
    }
    Serial0.printf("   Type 'u' (%s) or 's' (%s); 'a' for Advanced.\n", yes, no);
}

// Works out what to offer.  Returns false if the C6 can't be reached.
static bool c6Step()
{
    say(board == P4Board::JC8012 ? "\n2. Wi-Fi chip (ESP32-C6) firmware\n" : "Wi-Fi chip (ESP32-C6) firmware\n");
    if (!hostedInitWiFi()) {
        say("   The C6 did not answer on SDIO.\n");
        return false;
    }
    c6Ok = true;
    uint32_t ma = 0, mi = 0, pa = 0, hma = 0, hmi = 0, hpa = 0;
    hostedGetSlaveVersion(&ma, &mi, &pa);
    hostedGetHostVersion(&hma, &hmi, &hpa);
    have = ver(ma, mi, pa);
    snprintf(haveText, sizeof haveText, "%lu.%lu.%lu", (unsigned long)ma, (unsigned long)mi, (unsigned long)pa);

    if (have == 0) say("   On the C6: too old to report its version\n");
    else           say("   On the C6: %s\n", haveText);
    say("   In this installer: " C6_FW_VERSION " (and " C6X_FW_VERSION " under Advanced)\n");
    say("   This Arduino core expects: %lu.%lu.%lu\n",
        (unsigned long)hma, (unsigned long)hmi, (unsigned long)hpa);

    mainOffer = nullptr;
    if (have < IMG_2X.ver) {
        say("   An update is available.\n");
        mainOffer = &IMG_2X;
        offerMain("Update C6", GREEN(), "Skip");
    } else if (have == IMG_2X.ver) {
        say("   Up to date.\n");
    } else if (have == IMG_3X.ver) {
        say("   This is the ESP-Hosted 3 firmware from Advanced. Arduino sketches can't use it:"
            " Wi-Fi and Bluetooth won't work in them. Restore " C6_FW_VERSION " for Arduino?\n");
        mainOffer = &IMG_2X;
        offerMain("Restore " C6_FW_VERSION, GREEN(), "Leave it");
    } else {
        if (ma >= 3) say("   Arduino sketches can't use ESP-Hosted 3: Wi-Fi and Bluetooth won't work in them.\n");
        say("%s", noWayBack(IMG_2X).c_str());
        mainOffer = &IMG_2X;
        offerMain("Accept risk", ORANGE(), "Leave it");
    }
    if (ui) lv_obj_clear_flag(btnAdv, LV_OBJ_FLAG_HIDDEN);
    if (!mainOffer) {
        Serial0.println("   Type 'a' for Advanced.");
        say("\nAll done. You can upload your own sketch now.\n");
    }
    return true;
}

static void openAdvanced()
{
    if (!c6Ok) return;
    String s;
    bool canInstall = true;
    if (have == IMG_3X.ver) {
        s = "ESP-Hosted " C6X_FW_VERSION " is already on the C6.";
        canInstall = false;
    } else {
        s = "For ESP-IDF projects built with ESP-Hosted 3. This build has Wi-Fi and Bluetooth.\n\n"
            "Arduino sketches can't use it: with it on the C6, Wi-Fi and Bluetooth stop working "
            "in every Arduino sketch, until " C6_FW_VERSION " is put back. This installer can do "
            "that (tested): run it again and choose Restore.\n";
        if (have > IMG_3X.ver) { s += "\n"; s += noWayBack(IMG_3X); s.trim(); }
    }
    advOpen = true;
    if (ui) {
        lv_label_set_text(advText, s.c_str());
        setButton(advYes, have > IMG_3X.ver ? "Accept risk" : "Install " C6X_FW_VERSION, ORANGE());
        if (canInstall) lv_obj_clear_flag(advYes, LV_OBJ_FLAG_HIDDEN);
        else            lv_obj_add_flag(advYes, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(advPanel, LV_OBJ_FLAG_HIDDEN);
    }
    Serial0.printf("\nAdvanced: ESP-Hosted " C6X_FW_VERSION "\n%s\n", s.c_str());
    Serial0.println(canInstall ? "Type 'y' to install, 'n' to go back." : "Type 'n' to go back.");
    if (!canInstall) advYes = advYes;   // nothing to confirm
}

static void closeAdvanced()
{
    advOpen = false;
    if (ui) lv_obj_add_flag(advPanel, LV_OBJ_FLAG_HIDDEN);
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

static bool updateC6(const C6Image &img)
{
    uint8_t sha[32];
    mbedtls_sha256(img.data, img.len, sha, 0);
    if (memcmp(sha, img.sha, 32) != 0) {
        say("   The C6 firmware in this sketch is damaged. Rebuild it.\n");
        return false;
    }
    say("   Installing %s. Keep the board powered.\n", img.version);
    // An ESP-Hosted 3 C6 sends traffic this (2.x) host doesn't expect while the
    // update runs: hundreds of harmless "Dropping packet(s)" errors.  Mute them.
    esp_log_level_set("H_SDIO_DRV", ESP_LOG_NONE);
    if (!hostedBeginUpdate()) { say("   The C6 refused the update.\n"); return false; }

    if (ui) lv_obj_clear_flag(bar, LV_OBJ_FLAG_HIDDEN);
    static uint8_t chunk[4096];                      // the write call wants RAM, not flash
    int lastPct = -1;
    for (uint32_t off = 0; off < img.len; off += sizeof(chunk)) {
        uint32_t n = img.len - off < sizeof(chunk) ? img.len - off : sizeof(chunk);
        memcpy(chunk, img.data + off, n);
        if (!hostedWriteUpdate(chunk, n)) {
            say("   Writing failed at %lu bytes.\n", (unsigned long)off);
            return false;
        }
        int pct = (int)((uint64_t)(off + n) * 100 / img.len);
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
    if (c == 'u') answer = advOpen ? ANSWER_NONE : MAIN_YES;
    if (c == 's') answer = advOpen ? ANSWER_NONE : MAIN_NO;
    if (c == 'a') answer = ADV_OPEN;
    if (c == 'y') answer = advOpen ? ADV_YES : ANSWER_NONE;
    if (c == 'n') answer = advOpen ? ADV_NO : ANSWER_NONE;
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
            c6Step();
            answer = ANSWER_NONE;
            step = STEP_WAIT;
            break;
        case STEP_WAIT: {
            Answer a = answer;
            answer = ANSWER_NONE;
            if (a == ANSWER_NONE) break;
            if (a == ADV_OPEN && !advOpen) { openAdvanced(); break; }
            if (a == ADV_NO && advOpen)    { closeAdvanced(); Serial0.println("Back."); break; }
            if (a == ADV_YES && advOpen && have != IMG_3X.ver) {
                closeAdvanced();
                if (ui) lv_obj_add_flag(btnRow, LV_OBJ_FLAG_HIDDEN);
                target = &IMG_3X;
                step = STEP_UPDATE;
                break;
            }
            if (a == MAIN_YES && mainOffer && !advOpen) {
                if (ui) lv_obj_add_flag(btnRow, LV_OBJ_FLAG_HIDDEN);
                target = mainOffer;
                step = STEP_UPDATE;
                break;
            }
            if (a == MAIN_NO && mainOffer && !advOpen) {
                if (ui) lv_obj_add_flag(btnRow, LV_OBJ_FLAG_HIDDEN);
                mainOffer = nullptr;
                say("   Left as it is.\n\nAll done. You can upload your own sketch now.\n");
            }
            break;
        }
        case STEP_UPDATE:
            if (ui) lv_obj_add_flag(btnAdv, LV_OBJ_FLAG_HIDDEN);
            if (updateC6(*target)) {
                say("   Done. Restarting in 5 s to start it...\n");
                uint32_t t0 = millis();
                while (millis() - t0 < 5000) { if (ui) lv_timer_handler(); delay(20); }
                ESP.restart();
            }
            say("\nThe C6 was not changed. You can try again, or upload your own sketch.\n");
            step = STEP_WAIT;
            break;
    }
    if (ui) lv_timer_handler();
    delay(5);
}
