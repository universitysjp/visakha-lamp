/*
  ESP32 + WS2812 + Wi-Fi access point + web page.
  15 switches. Switch N controls LEDs 3N-2, 3N-1 and 3N.

  Switch transitions (None / Fade / Random star) are animated on the strip by
  anim_task(); the web page only ever sends the target state.

  The page itself lives in web/page.html and is turned into the PAGE_HTML
  string by tools/build_page.py (see main/page.h).

  Target: ESP32 (esp32). The strip hangs off the SPI2 (HSPI) native IOMUX MOSI pin,
  GPIO13, so the SPI backend never routes the signal through the GPIO matrix.
*/

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "nvs_flash.h"

#include "page.h"   // PAGE_HTML

// Data to the strip. The SPI backend only drives MOSI, and GPIO13 is SPI2's (HSPI)
// native IOMUX pin, so the signal skips the GPIO matrix entirely. SPI3's IOMUX MOSI
// is GPIO23 if you would rather use VSPI.
#define LED_PIN         13
#define NUM_GROUPS      22
#define LEDS_PER_GROUP  3        // WS2812s behind one lamp
#define NUM_LEDS        (NUM_GROUPS * LEDS_PER_GROUP)
#define BRIGHTNESS      255      // startup brightness, 0-255 (full on). Drop it if the 5V supply sags.
#define NAME_LEN        24       // longest switch name, including the terminator

// Which lamp sits where on the strip. The panel is labelled 1..NUM_GROUPS in the
// order you see it, but the strip was wired in a different order, so
// s_lamp_to_strip[lamp] is the strip group (0-based) that lamp lamp+1 is wired to.
// Everything else in this file works in label order - only render() and the
// colour lookups translate, which is why the running light follows the labels.
static const uint8_t s_lamp_to_strip[NUM_GROUPS] = {
    9, 10, 8, 11, 12, 6, 7, 5, 4, 3, 14, 13, 1, 2, 16, 15, 0, 17, 18, 19, 21, 20,
};
#define ANIM_TICK_MS    10       // 100 frames per second

// Wi-Fi access point credentials. The build reads these from .env (copy
// .env.example and edit it); the values below are only used when .env is absent.
#ifndef WIFI_SSID
#define WIFI_SSID "Visakha Lamp"
#endif
#ifndef WIFI_PASS
#define WIFI_PASS "12345678"   // min 8 characters
#endif

static const char *AP_SSID = WIFI_SSID;
static const char *AP_PASS = WIFI_PASS;

static const char *TAG = "lights";

static led_strip_handle_t s_strip;

static bool s_group_on[NUM_GROUPS] = { false };
static char s_names[NUM_GROUPS][NAME_LEN];
static uint8_t s_brightness = BRIGHTNESS;

// Colour of the three LEDs in a lamp. One triplet for the whole panel: LED slot k
// of every lamp uses s_slot_color[k], so all the lamps always match.
static uint8_t s_slot_color[LEDS_PER_GROUP][3];

// ---------- transitions: how one lamp switches ----------
typedef enum {
    TRANSITION_NONE = 0,   // switch straight away
    TRANSITION_FADE,       // glide up and down
} transition_t;

static transition_t s_transition = TRANSITION_FADE;   // the default

static float s_level[NUM_GROUPS];       // 0..1, what is on the strip right now
static float s_target[NUM_GROUPS];      // 0 or 1, what was asked for
static volatile unsigned s_redraw;      // bumped when colours must be pushed again
static volatile unsigned s_version;     // bumped on every change the page should see

static void bump(void)
{
    s_version++;
}

// ---------- animations: what the whole panel does ----------
typedef enum {
    ANIM_NONE = 0,
    ANIM_STAR,      // lamps twinkle at random, like stars
    ANIM_CHASE,     // a light runs up the panel, bottom row to top row
} animation_t;

// The panel's rows, bottom row first, so the chase climbs the way the panel hangs.
// Each row lists the lamps (0-based) that light together, ended by 0xFF.
#define ROW_END 0xFF
#define NUM_ROWS (sizeof(s_rows) / sizeof(s_rows[0]))

static const uint8_t s_rows[][6] = {
    { 16, 20, 21, ROW_END },              // row 6: lamps 17, 21, 22
    { 12, 13, 17, 18, 19, ROW_END },      // row 5: lamps 13, 14, 18, 19, 20
    { 7, 8, 9, 14, 15, ROW_END },         // row 4: lamps 8, 9, 10, 15, 16
    { 5, 6, 10, 11, ROW_END },            // row 3: lamps 6, 7, 11, 12
    { 2, 3, 4, ROW_END },                 // row 2: lamps 3, 4, 5
    { 0, 1, ROW_END },                    // row 1: lamps 1, 2
};

#define SEQ_STEP_MS     1000        // one more lamp every second while filling
#define HOLD_DEFAULT_S   900        // 15 minutes between the fill and the animation
#define FADE_MS          250        // how long a lamp takes to fade in or out
#define CHASE_ROWS_PER_S 1.5f       // how fast the chase climbs the panel
#define CHASE_WIDTH      1.3f       // how many rows wide its glow is

typedef enum {
    SEQ_IDLE = 0,
    SEQ_FILLING,    // switching on the lamps that were still off, one per second
    SEQ_WAITING,    // every lamp is on, sitting out the hold
    SEQ_RUNNING,    // the chosen animation is playing
} seq_state_t;

static seq_state_t s_seq = SEQ_IDLE;
static animation_t s_animation = ANIM_CHASE;
static int s_hold_s = HOLD_DEFAULT_S;   // seconds between the fill and the animation
static int s_seq_next;                  // lamp the fill is up to
static int s_seq_wait_ms;               // counts down a fill step, then the hold
static float s_chase_pos;               // where the chase is, in rows, moving smoothly
static int s_star_ms;
static float s_anim_level[NUM_GROUPS];  // what the animation adds on top

// defined below the animation loop, which the sequence uses
static void set_group(int g, bool on);

// ---------- settings that survive a power cut ----------
#define NVS_NAMESPACE    "lamp"
#define NVS_KEY          "settings"
#define SETTINGS_VERSION 3

typedef struct {
    uint16_t version;
    uint8_t brightness;
    uint8_t transition;
    uint8_t animation;
    uint32_t hold_s;
    uint8_t on[NUM_GROUPS];
    char names[NUM_GROUPS][NAME_LEN];
    uint8_t colors[LEDS_PER_GROUP][3];
} settings_blob_t;

static settings_blob_t s_blob;
static bool s_save_wanted;
static int s_save_in_ms;

// Call after anything worth remembering; the write happens once changes stop.
static void settings_touch(void)
{
    s_save_wanted = true;
    s_save_in_ms = 1000;
}

static void settings_defaults(void)
{
    s_brightness = BRIGHTNESS;
    s_transition = TRANSITION_FADE;
    s_animation = ANIM_CHASE;
    s_hold_s = HOLD_DEFAULT_S;
    memset(s_slot_color, 0xFF, sizeof(s_slot_color));   // every LED white

    for (int g = 0; g < NUM_GROUPS; g++) {
        s_group_on[g] = false;
        s_target[g] = 0.0f;
        s_level[g] = 0.0f;
        snprintf(s_names[g], sizeof(s_names[g]), "Lamp %d", g + 1);
    }
}

static void settings_save(void)
{
    nvs_handle_t h;

    s_blob.version = SETTINGS_VERSION;
    s_blob.brightness = s_brightness;
    s_blob.transition = (uint8_t)s_transition;
    s_blob.animation = (uint8_t)s_animation;
    s_blob.hold_s = (uint32_t)s_hold_s;
    for (int g = 0; g < NUM_GROUPS; g++) {
        s_blob.on[g] = s_group_on[g] ? 1 : 0;
        memcpy(s_blob.names[g], s_names[g], NAME_LEN);
    }
    memcpy(s_blob.colors, s_slot_color, sizeof(s_blob.colors));

    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_blob(h, NVS_KEY, &s_blob, sizeof(s_blob)) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

static void settings_load(void)
{
    size_t len = sizeof(s_blob);
    nvs_handle_t h;

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "no saved settings yet, using the defaults");
        return;
    }
    esp_err_t err = nvs_get_blob(h, NVS_KEY, &s_blob, &len);
    nvs_close(h);

    if (err != ESP_OK || len != sizeof(s_blob) || s_blob.version != SETTINGS_VERSION) {
        ESP_LOGW(TAG, "saved settings are missing or from another version, using the defaults");
        return;
    }

    s_brightness = s_blob.brightness;
    s_transition = (transition_t)(s_blob.transition <= TRANSITION_FADE ? s_blob.transition : TRANSITION_FADE);
    s_animation = (animation_t)(s_blob.animation <= ANIM_CHASE ? s_blob.animation : ANIM_CHASE);
    s_hold_s = (int)(s_blob.hold_s <= 86400u ? s_blob.hold_s : HOLD_DEFAULT_S);

    int on_count = 0;

    for (int g = 0; g < NUM_GROUPS; g++) {
        s_group_on[g] = s_blob.on[g] != 0;
        s_target[g] = s_group_on[g] ? 1.0f : 0.0f;
        s_level[g] = s_target[g];
        memcpy(s_names[g], s_blob.names[g], NAME_LEN);
        s_names[g][NAME_LEN - 1] = '\0';
        on_count += s_group_on[g] ? 1 : 0;
    }
    memcpy(s_slot_color, s_blob.colors, sizeof(s_blob.colors));
    bump();

    ESP_LOGW(TAG, "settings restored: brightness %u, transition %d, animation %d, hold %d s, %d lamps on",
             (unsigned)s_brightness, (int)s_transition, (int)s_animation, s_hold_s, on_count);
}

// While an animation runs it takes over the brightness: the lamps it is not
// touching drop back so the moving light (or the twinkle) actually shows. Without
// this the animation would be invisible on a panel that is already all lit.
static float animation_base(void)
{
    if (s_seq != SEQ_RUNNING) {
        return 1.0f;
    }
    return (s_animation == ANIM_CHASE) ? 0.12f : 0.35f;
}

static void render(void)
{
    for (int g = 0; g < NUM_GROUPS; g++) {
        int strip = s_lamp_to_strip[g];      // where this label lives on the strip
        float level = s_level[g] * animation_base();

        if (s_anim_level[g] > level) {
            level = s_anim_level[g];              // the animation rides on top
        }
        float dim = (float)s_brightness * level / 255.0f;   // 0..1

        for (int k = 0; k < LEDS_PER_GROUP; k++) {
            int led = strip * LEDS_PER_GROUP + k;

            if (led >= NUM_LEDS) {
                break;
            }
            uint8_t r = (uint8_t)((float)s_slot_color[k][0] * dim + 0.5f);
            uint8_t gg = (uint8_t)((float)s_slot_color[k][1] * dim + 0.5f);
            uint8_t b = (uint8_t)((float)s_slot_color[k][2] * dim + 0.5f);

            led_strip_set_pixel(s_strip, led, r, gg, b);
        }
    }
    led_strip_refresh(s_strip);
}

// Brightness of every row for the current chase position: a soft glow that fades
// in and out as it climbs, so the motion is smooth rather than one row per step.
static void chase_levels(void)
{
    for (int r = 0; r < (int)NUM_ROWS; r++) {
        float d = fabsf((float)r - s_chase_pos);

        if (d > (float)NUM_ROWS / 2.0f) {
            d = (float)NUM_ROWS - d;             // the glow loops round
        }
        float lvl = 1.0f - d / CHASE_WIDTH;

        if (lvl < 0.0f) {
            lvl = 0.0f;
        }
        lvl = lvl * lvl * (3.0f - 2.0f * lvl);   // smoothstep, softer edges

        for (int i = 0; s_rows[r][i] != ROW_END; i++) {
            s_anim_level[s_rows[r][i]] = lvl;
        }
    }
}

// Move the chosen animation on by one frame.
static void animation_tick(void)
{
    if (s_animation == ANIM_CHASE) {
        s_chase_pos += CHASE_ROWS_PER_S * (float)ANIM_TICK_MS / 1000.0f;
        if (s_chase_pos >= (float)NUM_ROWS) {
            s_chase_pos -= (float)NUM_ROWS;
        }
        chase_levels();
    } else if (s_animation == ANIM_STAR) {
        for (int g = 0; g < NUM_GROUPS; g++) {
            s_anim_level[g] *= 0.91f;           // let each flash die away
            if (s_anim_level[g] < 0.02f) {
                s_anim_level[g] = 0.0f;
            }
        }
        s_star_ms -= ANIM_TICK_MS;
        if (s_star_ms <= 0) {
            s_star_ms = 60 + (int)(rand() % 120);
            int flashes = 1 + (int)(rand() % 3);

            for (int i = 0; i < flashes; i++) {
                s_anim_level[rand() % NUM_GROUPS] = 0.6f + (float)(rand() % 40) / 100.0f;
            }
        }
    }
}

static void animation_reset(void)
{
    s_chase_pos = 0.0f;
    s_star_ms = 0;
    for (int g = 0; g < NUM_GROUPS; g++) {
        s_anim_level[g] = 0.0f;   // nothing on top of the lamps until the animation ticks
    }
    s_redraw++;
}

// One animation frame for every switch at once.
static void anim_task(void *arg)
{
    (void)arg;
    unsigned drawn = 0;

    while (true) {
        bool moved = false;

        // the "light the rest" sequence, one step at a time
        if (s_seq == SEQ_FILLING) {
            if (s_seq_wait_ms > 0) {
                s_seq_wait_ms -= ANIM_TICK_MS;
            } else {
                while (s_seq_next < NUM_GROUPS && s_group_on[s_seq_next]) {
                    s_seq_next++;               // skip the lamps that are already on
                }
                if (s_seq_next >= NUM_GROUPS) {
                    s_seq = SEQ_WAITING;        // everything is lit: sit out the hold
                    s_seq_wait_ms = s_hold_s * 1000;
                    bump();
                    ESP_LOGW(TAG, "sequence: all lamps lit, waiting %d s before the animation", s_hold_s);
                } else {
                    set_group(s_seq_next, true);
                    s_seq_next++;
                    s_seq_wait_ms = SEQ_STEP_MS;
                }
            }
        } else if (s_seq == SEQ_WAITING) {
            s_seq_wait_ms -= ANIM_TICK_MS;
            if (s_seq_wait_ms <= 0) {
                s_seq = SEQ_RUNNING;
                animation_reset();
                bump();
                ESP_LOGW(TAG, "sequence: starting animation %d", (int)s_animation);
            }
        }
        if (s_seq == SEQ_RUNNING) {
            animation_tick();
            moved = true;                       // the animation moves every frame
        }

        for (int g = 0; g < NUM_GROUPS; g++) {
            float before = s_level[g];

            if (s_level[g] != s_target[g]) {
                // fade over FADE_MS whatever the frame rate is
                float step = (s_transition == TRANSITION_FADE)
                             ? (float)ANIM_TICK_MS / (float)FADE_MS
                             : 1.0f;

                if (s_level[g] < s_target[g]) {
                    s_level[g] += step;
                    if (s_level[g] > s_target[g]) {
                        s_level[g] = s_target[g];
                    }
                } else {
                    s_level[g] -= step;
                    if (s_level[g] < s_target[g]) {
                        s_level[g] = s_target[g];
                    }
                }
            }

            if (s_level[g] != before) {
                moved = true;
            }
        }

        if (moved || s_redraw != drawn) {
            drawn = s_redraw;
            render();
        }

        // write the settings out once the changes have stopped coming
        if (s_save_wanted && (s_save_in_ms -= ANIM_TICK_MS) <= 0) {
            s_save_wanted = false;
            settings_save();
        }
        vTaskDelay(pdMS_TO_TICKS(ANIM_TICK_MS));
    }
}

static void set_group(int g, bool on)
{
    s_group_on[g] = on;
    s_target[g] = on ? 1.0f : 0.0f;
    settings_touch();
    bump();
}

static void set_transition(int mode)
{
    s_transition = (transition_t)mode;
    for (int g = 0; g < NUM_GROUPS; g++) {
        s_level[g] = s_target[g];       // snap to whatever was asked for
    }
    s_redraw++;
    settings_touch();
    bump();
}

static void set_animation(int mode)
{
    s_animation = (animation_t)mode;
    settings_touch();
    bump();
}

static void set_hold(int seconds)
{
    s_hold_s = seconds;
    settings_touch();
    bump();
}

// Start filling in the lamps that are still dark, one per second; the hold and
// then the chosen animation follow by themselves.
static void sequence_start(void)
{
    s_seq = SEQ_FILLING;
    s_seq_next = 0;
    s_seq_wait_ms = 0;
    bump();
    ESP_LOGW(TAG, "sequence: filling the dark lamps, then holding %d s, animation %d",
             s_hold_s, (int)s_animation);
}

// Give up the sequence and leave the lamps to settle on their own. The levels are
// deliberately not snapped here: a lamp you just tapped still has to fade in.
static void sequence_stop(void)
{
    s_seq = SEQ_IDLE;
    animation_reset();
    bump();
}

// ---------- helpers ----------
static esp_err_t send_plain(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// Parses "#RRGGBB" or "RRGGBB" into three channel values.
static bool parse_hex_color(const char *s, uint8_t *rgb)
{
    if (*s == '#') {
        s++;
    }
    if (strlen(s) != 6) {
        return false;
    }
    for (int k = 0; k < 6; k++) {
        if (!isxdigit((unsigned char)s[k])) {
            return false;
        }
    }
    for (int k = 0; k < 3; k++) {
        char pair[3] = { s[k * 2], s[k * 2 + 1], '\0' };

        rgb[k] = (uint8_t)strtol(pair, NULL, 16);
    }
    return true;
}

// Percent-decodes s in place ("a+b" -> "a b") and drops control characters.
static void url_decode(char *s)
{
    char *out = s;

    for (char *in = s; *in != '\0'; in++) {
        if (*in == '+') {
            *out++ = ' ';
        } else if (*in == '%' && isxdigit((unsigned char)in[1]) && isxdigit((unsigned char)in[2])) {
            char hex[3] = { in[1], in[2], '\0' };
            *out++ = (char)strtol(hex, NULL, 16);
            in += 2;
        } else if ((unsigned char)*in >= 0x20) {
            *out++ = *in;
        }
    }
    *out = '\0';
}

// Appends formatted text, keeping pos inside the buffer even if it truncates.
static size_t json_addf(char *dst, size_t pos, size_t dst_len, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(dst + pos, dst_len - pos, fmt, ap);
    va_end(ap);

    if (n > 0) {
        pos += (size_t)n;
    }
    return pos < dst_len ? pos : dst_len - 1;
}

// Appends s as a JSON string body, escaping what JSON requires.
static size_t json_append_escaped(char *dst, size_t pos, size_t dst_len, const char *s)
{
    for (const char *p = s; *p != '\0' && pos + 7 < dst_len; p++) {
        if (*p == '"' || *p == '\\') {
            dst[pos++] = '\\';
        }
        dst[pos++] = *p;
    }
    dst[pos] = '\0';
    return pos;
}

// Reads one query argument into out. Returns false if it is missing.
static bool query_arg(httpd_req_t *req, const char *key, char *out, size_t out_len)
{
    char query[192];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    return httpd_query_key_value(query, key, out, out_len) == ESP_OK;
}

// ---------- handlers ----------
static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_set(httpd_req_t *req)
{
    char arg_i[8];
    char arg_v[8];

    if (!query_arg(req, "i", arg_i, sizeof(arg_i)) || !query_arg(req, "v", arg_v, sizeof(arg_v))) {
        return send_plain(req, "400 Bad Request", "missing i or v");
    }

    int i = atoi(arg_i);
    int v = atoi(arg_v);
    if (i < 0 || i >= NUM_GROUPS) {
        return send_plain(req, "400 Bad Request", "bad index");
    }

    set_group(i, v == 1);
    sequence_stop();                    // a tap takes over from the sequence
    return send_plain(req, "200 OK", "ok");
}

static esp_err_t handle_all_off(httpd_req_t *req)
{
    sequence_stop();
    for (int g = 0; g < NUM_GROUPS; g++) {
        set_group(g, false);
    }
    return send_plain(req, "200 OK", "ok");
}

static esp_err_t handle_sequence(httpd_req_t *req)
{
    if (s_seq == SEQ_IDLE) {
        sequence_start();
        return send_plain(req, "200 OK", "started");
    }
    sequence_stop();
    return send_plain(req, "200 OK", "stopped");
}

static esp_err_t handle_brightness(httpd_req_t *req)
{
    char arg_v[8];

    if (!query_arg(req, "v", arg_v, sizeof(arg_v))) {
        return send_plain(req, "400 Bad Request", "missing v");
    }

    int v = atoi(arg_v);
    if (v < 0) {
        v = 0;
    }
    if (v > 255) {
        v = 255;
    }

    s_brightness = (uint8_t)v;
    s_redraw++;
    settings_touch();
    bump();
    return send_plain(req, "200 OK", "ok");
}

// A counter the page watches: it changes whenever anything it displays changes,
// so the page can stay in step without re-reading the whole state all the time.
static esp_err_t handle_version(httpd_req_t *req)
{
    char buf[12];

    snprintf(buf, sizeof(buf), "%u", (unsigned)s_version);
    return send_plain(req, "200 OK", buf);
}

static esp_err_t handle_animation(httpd_req_t *req)
{
    char arg_v[8];

    if (!query_arg(req, "v", arg_v, sizeof(arg_v))) {
        return send_plain(req, "400 Bad Request", "missing v");
    }

    int v = atoi(arg_v);
    if (v < ANIM_NONE || v > ANIM_CHASE) {
        return send_plain(req, "400 Bad Request", "bad animation");
    }

    set_animation(v);
    if (v == ANIM_NONE) {
        sequence_stop();                // stop whatever is playing
    } else {
        s_seq = SEQ_RUNNING;            // preview the choice straight away
        animation_reset();
        bump();
        ESP_LOGW(TAG, "animation %d started from the settings page", v);
    }
    return send_plain(req, "200 OK", "ok");
}

static esp_err_t handle_hold(httpd_req_t *req)
{
    char arg_v[12];

    if (!query_arg(req, "v", arg_v, sizeof(arg_v))) {
        return send_plain(req, "400 Bad Request", "missing v");
    }

    int v = atoi(arg_v);
    if (v < 0) {
        v = 0;
    }
    if (v > 86400) {
        v = 86400;                      // a day is plenty
    }

    set_hold(v);
    if (s_seq == SEQ_WAITING) {
        s_seq_wait_ms = v * 1000;       // a wait already under way follows the new value
    }
    return send_plain(req, "200 OK", "ok");
}

static esp_err_t handle_transition(httpd_req_t *req)
{
    char arg_v[8];

    if (!query_arg(req, "v", arg_v, sizeof(arg_v))) {
        return send_plain(req, "400 Bad Request", "missing v");
    }

    int v = atoi(arg_v);
    if (v < TRANSITION_NONE || v > TRANSITION_FADE) {
        return send_plain(req, "400 Bad Request", "bad mode");
    }

    set_transition(v);
    return send_plain(req, "200 OK", "ok");
}

static esp_err_t handle_color(httpd_req_t *req)
{
    char arg_k[8];
    char arg_c[16];
    uint8_t rgb[3];

    if (!query_arg(req, "k", arg_k, sizeof(arg_k)) || !query_arg(req, "c", arg_c, sizeof(arg_c))) {
        return send_plain(req, "400 Bad Request", "missing k or c");
    }

    int k = atoi(arg_k);
    if (k < 0 || k >= LEDS_PER_GROUP) {
        return send_plain(req, "400 Bad Request", "bad slot");
    }

    url_decode(arg_c);
    if (!parse_hex_color(arg_c, rgb)) {
        return send_plain(req, "400 Bad Request", "bad colour");
    }

    s_slot_color[k][0] = rgb[0];
    s_slot_color[k][1] = rgb[1];
    s_slot_color[k][2] = rgb[2];
    s_redraw++;
    settings_touch();
    bump();
    return send_plain(req, "200 OK", "ok");
}

// Wipe every saved setting and lamp state and go back to how it shipped.
static esp_err_t handle_reset(httpd_req_t *req)
{
    nvs_handle_t h;

    sequence_stop();
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY);
        nvs_commit(h);
        nvs_close(h);
    }
    settings_defaults();
    s_save_wanted = false;          // nothing to write until something changes again
    s_redraw++;
    bump();
    ESP_LOGW(TAG, "factory reset: settings and lamp states cleared");
    return send_plain(req, "200 OK", "ok");
}

static esp_err_t handle_name(httpd_req_t *req)
{
    char arg_i[8];
    char arg_n[160];

    if (!query_arg(req, "i", arg_i, sizeof(arg_i)) || !query_arg(req, "n", arg_n, sizeof(arg_n))) {
        return send_plain(req, "400 Bad Request", "missing i or n");
    }

    int i = atoi(arg_i);
    if (i < 0 || i >= NUM_GROUPS) {
        return send_plain(req, "400 Bad Request", "bad index");
    }

    url_decode(arg_n);
    strlcpy(s_names[i], arg_n, sizeof(s_names[i]));
    settings_touch();
    bump();
    return send_plain(req, "200 OK", "ok");
}

static esp_err_t handle_state(httpd_req_t *req)
{
    static char json[NUM_GROUPS * (2 * NAME_LEN + 8) + 256];
    size_t pos = 0;

    pos = json_addf(json, pos, sizeof(json), "{\"on\":[");
    for (int g = 0; g < NUM_GROUPS; g++) {
        pos = json_addf(json, pos, sizeof(json), "%s%d", g ? "," : "", s_group_on[g] ? 1 : 0);
    }
    pos = json_addf(json, pos, sizeof(json), "],\"names\":[");
    for (int g = 0; g < NUM_GROUPS; g++) {
        pos = json_addf(json, pos, sizeof(json), "%s\"", g ? "," : "");
        pos = json_append_escaped(json, pos, sizeof(json), s_names[g]);
        pos = json_addf(json, pos, sizeof(json), "\"");
    }
    pos = json_addf(json, pos, sizeof(json), "],\"colors\":[");
    for (int k = 0; k < LEDS_PER_GROUP; k++) {
        pos = json_addf(json, pos, sizeof(json), "%s%02X%02X%02X", k ? "," : "",
                        s_slot_color[k][0], s_slot_color[k][1], s_slot_color[k][2]);
    }
    json_addf(json, pos, sizeof(json),
              "],\"brightness\":%u,\"transition\":%d,\"animation\":%d,\"hold\":%d,"
              "\"wait\":%d,\"sequence\":%d}",
              (unsigned)s_brightness, (int)s_transition, (int)s_animation, s_hold_s,
              s_seq == SEQ_WAITING ? (s_seq_wait_ms + 999) / 1000 : 0, (int)s_seq);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

// ---------- setup ----------
static void init_leds(void)
{
    const led_strip_config_t strip_config = {
        .strip_gpio_num = LED_PIN,
        .max_leds = NUM_LEDS,
    };
    const led_strip_spi_config_t spi_config = {
        .spi_bus = SPI2_HOST,               // HSPI, the host whose IOMUX pin LED_PIN is
        .clk_src = SPI_CLK_SRC_DEFAULT,     // 2.5 MHz, the WS2812 bit timing
        .flags.with_dma = true,             // 405 bytes per refresh, far past the 64-byte FIFO
    };

    ESP_ERROR_CHECK(led_strip_new_spi_device(&strip_config, &spi_config, &s_strip));

    render();   // whatever the saved settings say
}

static void init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));

    wifi_config_t wifi_config = {
        .ap = {
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strlcpy((char *)wifi_config.ap.ssid, AP_SSID, sizeof(wifi_config.ap.ssid));
    strlcpy((char *)wifi_config.ap.password, AP_PASS, sizeof(wifi_config.ap.password));
    wifi_config.ap.ssid_len = strlen(AP_SSID);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_netif_ip_info_t ip_info;
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap != NULL && esp_netif_get_ip_info(ap, &ip_info) == ESP_OK) {
        // logged at WARN level so the message survives CONFIG_LOG_DEFAULT_LEVEL_WARN,
        // which is what keeps the IDF info strings out of the binary
        ESP_LOGW(TAG, "AP SSID: %s", AP_SSID);
        ESP_LOGW(TAG, "AP IP: " IPSTR, IP2STR(&ip_info.ip));   // default 192.168.4.1
    }
}

static void start_webserver(void)
{
    static const httpd_uri_t routes[] = {
        { .uri = "/",           .method = HTTP_GET, .handler = handle_root },
        { .uri = "/set",        .method = HTTP_GET, .handler = handle_set },
        { .uri = "/alloff",     .method = HTTP_GET, .handler = handle_all_off },
        { .uri = "/sequence",   .method = HTTP_GET, .handler = handle_sequence },
        { .uri = "/version",    .method = HTTP_GET, .handler = handle_version },
        { .uri = "/brightness", .method = HTTP_GET, .handler = handle_brightness },
        { .uri = "/transition", .method = HTTP_GET, .handler = handle_transition },
        { .uri = "/animation",  .method = HTTP_GET, .handler = handle_animation },
        { .uri = "/hold",       .method = HTTP_GET, .handler = handle_hold },
        { .uri = "/color",      .method = HTTP_GET, .handler = handle_color },
        { .uri = "/name",       .method = HTTP_GET, .handler = handle_name },
        { .uri = "/reset",      .method = HTTP_GET, .handler = handle_reset },
        { .uri = "/state",      .method = HTTP_GET, .handler = handle_state },
    };

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    config.max_uri_handlers = 14;   // 11 routes registered below

    ESP_ERROR_CHECK(httpd_start(&server, &config));
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    }
}

// A broken map silently lights the wrong lamps, so complain loudly at boot.
static void check_lamp_map(void)
{
    uint8_t seen[NUM_GROUPS] = { 0 };

    for (int g = 0; g < NUM_GROUPS; g++) {
        uint8_t strip = s_lamp_to_strip[g];

        if (strip >= NUM_GROUPS || seen[strip]) {
            ESP_LOGE(TAG, "lamp map is broken: lamp %d -> strip %d", g + 1, strip + 1);
            return;
        }
        seen[strip] = 1;
    }
    ESP_LOGW(TAG, "lamp map ok: labels 1-%d each mapped to one strip position", NUM_GROUPS);
}

void app_main(void)
{
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs);
    }

    settings_defaults();
    settings_load();

    check_lamp_map();
    init_leds();
    init_softap();
    start_webserver();

    xTaskCreate(anim_task, "anim", 3584, NULL, 5, NULL);

    ESP_LOGW(TAG, "%d lamps ready: join \"%s\" and open http://192.168.4.1/",
             NUM_GROUPS, AP_SSID);
}
