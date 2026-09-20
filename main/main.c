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
#define NUM_GROUPS      15
#define LEDS_PER_GROUP  3        // WS2812s behind one lamp
#define NUM_LEDS        (NUM_GROUPS * LEDS_PER_GROUP)
#define BRIGHTNESS      60       // startup brightness, 0-255. Keep low unless you have a strong 5V supply.
#define NAME_LEN        24       // longest switch name, including the terminator
#define ANIM_TICK_MS    20       // 50 frames per second

static const char *AP_SSID = "ESP32-Lights";
static const char *AP_PASS = "12345678";   // min 8 characters

static const char *TAG = "lights";

static led_strip_handle_t s_strip;

static bool s_group_on[NUM_GROUPS] = { false };
static char s_names[NUM_GROUPS][NAME_LEN];
static uint8_t s_brightness = BRIGHTNESS;

// Colour of every LED, 0-255 per channel. Set from the web page as hex codes.
static uint8_t s_color[NUM_LEDS][3];

// ---------- transitions ----------
typedef enum {
    TRANSITION_NONE = 0,   // switch straight away
    TRANSITION_FADE,       // glide up and down
    TRANSITION_STAR,       // twinkle like a star, then settle
} transition_t;

static transition_t s_transition = TRANSITION_NONE;

static float s_level[NUM_GROUPS];       // 0..1, what is on the strip right now
static float s_target[NUM_GROUPS];      // 0 or 1, what was asked for
static int s_twinkle_ms[NUM_GROUPS];    // random star: time left in the twinkle
static int s_phase_ms[NUM_GROUPS];      // random star: time until the next blink
static volatile unsigned s_redraw;      // bumped when colours must be pushed again

static void render(void)
{
    for (int g = 0; g < NUM_GROUPS; g++) {
        float dim = (float)s_brightness * s_level[g] / 255.0f;   // 0..1

        for (int k = 0; k < LEDS_PER_GROUP; k++) {
            int led = g * LEDS_PER_GROUP + k;

            if (led >= NUM_LEDS) {
                break;
            }
            uint8_t r = (uint8_t)((float)s_color[led][0] * dim + 0.5f);
            uint8_t gg = (uint8_t)((float)s_color[led][1] * dim + 0.5f);
            uint8_t b = (uint8_t)((float)s_color[led][2] * dim + 0.5f);

            led_strip_set_pixel(s_strip, led, r, gg, b);
        }
    }
    led_strip_refresh(s_strip);
}

// One animation frame for every switch at once.
static void anim_task(void *arg)
{
    (void)arg;
    unsigned drawn = 0;

    while (true) {
        bool moved = false;

        for (int g = 0; g < NUM_GROUPS; g++) {
            float before = s_level[g];

            if (s_twinkle_ms[g] > 0) {
                s_twinkle_ms[g] -= ANIM_TICK_MS;
                s_phase_ms[g] -= ANIM_TICK_MS;
                if (s_phase_ms[g] <= 0) {
                    s_level[g] = (rand() % 100) < 55 ? 1.0f : 0.22f;
                    s_phase_ms[g] = 40 + rand() % 50;
                }
                if (s_twinkle_ms[g] <= 0) {
                    s_level[g] = s_target[g];   // settle where it belongs
                }
            } else if (s_level[g] != s_target[g]) {
                float step = (s_transition == TRANSITION_FADE) ? 0.10f : 1.0f;

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
        vTaskDelay(pdMS_TO_TICKS(ANIM_TICK_MS));
    }
}

static void set_group(int g, bool on)
{
    s_group_on[g] = on;
    s_target[g] = on ? 1.0f : 0.0f;

    if (s_transition == TRANSITION_STAR) {
        s_twinkle_ms[g] = 700;
        s_phase_ms[g] = 0;              // start blinking right away
        if (on) {
            s_level[g] = 1.0f;          // first flash
        }
    }
}

static void set_transition(int mode)
{
    s_transition = (transition_t)mode;
    for (int g = 0; g < NUM_GROUPS; g++) {
        s_twinkle_ms[g] = 0;
        s_level[g] = s_target[g];       // snap to whatever was asked for
    }
    s_redraw++;
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
    return send_plain(req, "200 OK", "ok");
}

static esp_err_t handle_all_off(httpd_req_t *req)
{
    for (int g = 0; g < NUM_GROUPS; g++) {
        set_group(g, false);
    }
    return send_plain(req, "200 OK", "ok");
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
    return send_plain(req, "200 OK", "ok");
}

static esp_err_t handle_transition(httpd_req_t *req)
{
    char arg_v[8];

    if (!query_arg(req, "v", arg_v, sizeof(arg_v))) {
        return send_plain(req, "400 Bad Request", "missing v");
    }

    int v = atoi(arg_v);
    if (v < TRANSITION_NONE || v > TRANSITION_STAR) {
        return send_plain(req, "400 Bad Request", "bad mode");
    }

    set_transition(v);
    return send_plain(req, "200 OK", "ok");
}

static esp_err_t handle_color(httpd_req_t *req)
{
    char arg_i[8];
    char arg_c[16];
    uint8_t rgb[3];

    if (!query_arg(req, "i", arg_i, sizeof(arg_i)) || !query_arg(req, "c", arg_c, sizeof(arg_c))) {
        return send_plain(req, "400 Bad Request", "missing i or c");
    }

    int i = atoi(arg_i);
    if (i < 0 || i >= NUM_LEDS) {
        return send_plain(req, "400 Bad Request", "bad index");
    }

    url_decode(arg_c);
    if (!parse_hex_color(arg_c, rgb)) {
        return send_plain(req, "400 Bad Request", "bad colour");
    }

    s_color[i][0] = rgb[0];
    s_color[i][1] = rgb[1];
    s_color[i][2] = rgb[2];
    s_redraw++;
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
    return send_plain(req, "200 OK", "ok");
}

static esp_err_t handle_state(httpd_req_t *req)
{
    static char json[NUM_LEDS * 9 + NUM_GROUPS * (2 * NAME_LEN + 8) + 96];
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
    for (int led = 0; led < NUM_LEDS; led++) {
        pos = json_addf(json, pos, sizeof(json), "%s%02X%02X%02X", led ? "," : "",
                        s_color[led][0], s_color[led][1], s_color[led][2]);
    }
    json_addf(json, pos, sizeof(json), "],\"brightness\":%u,\"transition\":%d}",
              (unsigned)s_brightness, (int)s_transition);

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

    memset(s_color, 0xFF, sizeof(s_color));   // every LED starts white
    render();                                 // ...and every LED starts dark
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
        ESP_LOGI(TAG, "AP SSID: %s", AP_SSID);
        ESP_LOGI(TAG, "AP IP: " IPSTR, IP2STR(&ip_info.ip));   // default 192.168.4.1
    }
}

static void start_webserver(void)
{
    static const httpd_uri_t routes[] = {
        { .uri = "/",           .method = HTTP_GET, .handler = handle_root },
        { .uri = "/set",        .method = HTTP_GET, .handler = handle_set },
        { .uri = "/alloff",     .method = HTTP_GET, .handler = handle_all_off },
        { .uri = "/brightness", .method = HTTP_GET, .handler = handle_brightness },
        { .uri = "/transition", .method = HTTP_GET, .handler = handle_transition },
        { .uri = "/color",      .method = HTTP_GET, .handler = handle_color },
        { .uri = "/name",       .method = HTTP_GET, .handler = handle_name },
        { .uri = "/state",      .method = HTTP_GET, .handler = handle_state },
    };

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    config.max_uri_handlers = 12;   // 8 routes registered below

    ESP_ERROR_CHECK(httpd_start(&server, &config));
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    }
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

    for (int g = 0; g < NUM_GROUPS; g++) {
        snprintf(s_names[g], sizeof(s_names[g]), "Switch %d", g + 1);
    }

    init_leds();
    init_softap();
    start_webserver();

    xTaskCreate(anim_task, "anim", 3584, NULL, 5, NULL);

    ESP_LOGI(TAG, "%d switches ready: join \"%s\" and open http://192.168.4.1/",
             NUM_GROUPS, AP_SSID);
}
