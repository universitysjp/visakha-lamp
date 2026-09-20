# Visakha-Lamp

A 15-lamp light panel driven by an ESP32 and controlled from any phone. The ESP32
runs its own Wi-Fi access point and serves the control page itself, so there is no
app, no router and no internet connection involved.

![The control page](docs/screenshot.png)

## Hardware

| | |
|---|---|
| Board | ESP32 Dev Module (developed on an ESP32-D0WD-V3, 4 MB flash) |
| LEDs | WS2812 / NeoPixel strip, 45 pixels — three per lamp |
| Data | GPIO 4 → strip DIN, through a 330 Ω resistor |
| Power | separate 5 V supply for the strip, ground shared with the ESP32 |

Change `NUM_GROUPS`, `LEDS_PER_GROUP` or `NUM_LEDS` at the top of `main/main.c` if
your panel is wired differently.

## Using it

1. Power the panel. It starts a Wi-Fi access point: **ESP32-Lights**, password `12345678`.
2. Join it from a phone and open <http://192.168.4.1/>.
3. Tap a lamp to switch it. The `Settings` button at the bottom of the page holds the
   master brightness, the transitions, and each switch's name and three hex colours.

The fade and random-star transitions animate the real LEDs — the animation runs on the
ESP32, not in the browser.

## HTTP API

| Request | Effect |
|---|---|
| `GET /` | the control page |
| `GET /set?i=<lamp>&v=0\|1` | switch lamp `i` (0–14) off or on |
| `GET /alloff` | switch every lamp off |
| `GET /brightness?v=<0-255>` | master brightness |
| `GET /transition?v=0\|1\|2` | `0` none, `1` fade, `2` random star |
| `GET /color?i=<led>&c=RRGGBB` | colour of a single LED (0–44) |
| `GET /name?i=<lamp>&n=<text>` | rename a switch (URL-encoded) |
| `GET /state` | JSON: `on`, `names`, `colors`, `brightness`, `transition` |

## Building

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) v6.1 (installed here with `eim install`):

```sh
source ~/.espressif/tools/activate_idf_v6.1.sh
idf.py set-target esp32
idf.py build
idf.py -p <PORT> flash monitor      # ls /dev/cu.* or /dev/ttyUSB* to find PORT
```

## Layout

```
main/main.c            firmware: soft AP, HTTP server, WS2812 output, transitions
main/page.h            generated: the control page as a C string
web/page.html          the control page itself, hand-editable (SVGs inlined)
tools/build_page.py    web/page.html -> main/page.h
```

To change the page, edit `web/page.html`, run `python3 tools/build_page.py`, then flash.

Switch names, colours and brightness are held in RAM and reset when the panel is
unplugged; the switch states themselves start off.
