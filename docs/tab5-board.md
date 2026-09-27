# M5Stack Tab5 board setup

This project targets the Tab5's ESP32-P4. The P4 has no Wi-Fi radio; the onboard ESP32-C6 supplies Wi-Fi over the ESP-Hosted SDIO transport. The official Tab5 BSP is the clean way to bring up the display and touch, while Wi-Fi transport and connection setup remain application responsibilities.

## Component and toolchain baseline

Use the ESP-IDF Component Manager and pin the board component:

```yaml
# main/idf_component.yml
dependencies:
  idf: ">=5.5.3,<5.6"
  espressif/m5stack_tab5: "1.3.1"
  espressif/esp_lvgl_port: "2.6.2"
  lvgl/lvgl: "9.4.0"
  # Add when bringing up Wi-Fi through the onboard C6:
  espressif/esp_hosted: "1.4.0"
  espressif/esp_wifi_remote: "0.8.5"
```

`m5stack_tab5` 1.3.1 declares target `esp32p4`, requires IDF >=5.4, and brings in `esp_codec_dev ~1.5`, the display/touch drivers, and other optional peripherals. For this project, pin ESP-IDF 5.5.3, `esp_lvgl_port` 2.6.2, and LVGL 9.4.0: port 2.9 is incompatible with the IDF 5.5.3 DPI callback API. The app also uses the published M5Stack UserDemo pair `esp_hosted 1.4.0` and `esp_wifi_remote 0.8.5`. IDF 5.5.3 meets the BSP minimum.

Set the target before building:

```sh
idf.py set-target esp32p4
idf.py build
```

Use the legacy-compatible P4 defaults for older Tab5 silicon. IDF 5.5.3 otherwise defaults to P4 revision 3.1, which does not run on earlier Tab5 P4 revisions. The project's `sdkconfig.defaults` selects `<3.0` / revision 0; use its `sdkconfig.rev3` override only for a revision 3 board. The defaults also set 16 MB flash, PSRAM at 200 MHz, C++ exceptions, and hosted transport:

```ini
CONFIG_IDF_TARGET="esp32p4"
CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y
CONFIG_ESP32P4_REV_MIN_0=y
CONFIG_SLAVE_IDF_TARGET_ESP32C6=y
CONFIG_ESP_WIFI_REMOTE_LIBRARY_HOSTED=y
CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE=y
CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE=15
CONFIG_ESP_HOSTED_SDIO_PIN_CLK=12
CONFIG_ESP_HOSTED_SDIO_PIN_CMD=13
CONFIG_ESP_HOSTED_SDIO_PIN_D0=11
CONFIG_ESP_HOSTED_SDIO_PRIV_PIN_D1_4BIT_BUS=10
CONFIG_ESP_HOSTED_SDIO_PIN_D2=9
CONFIG_ESP_HOSTED_SDIO_PIN_D3=8
CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ=25000
```

These symbols and pins were confirmed in the project's generated ESP-IDF 5.5.3 `sdkconfig`; retain the `PRIV_PIN_D1_4BIT_BUS` spelling exactly. This project has not yet been flashed to a physical Tab5.

## Display and touch startup

Include `bsp/m5stack_tab5.h`; with the BSP's default graphics configuration it includes LVGL and the ESP LVGL port. This minimal startup initializes the panel, touch input, LVGL port/task, and turns on the backlight:

```c
#include "bsp/m5stack_tab5.h"

static void ui_start(void)
{
    lv_display_t *display = bsp_display_start();
    assert(display != NULL);
    ESP_ERROR_CHECK(bsp_display_backlight_on());

    if (bsp_display_lock(0)) {
        lv_obj_t *label = lv_label_create(lv_screen_active());
        lv_label_set_text(label, "Sonos");
        lv_obj_center(label);
        bsp_display_unlock();
    }
}
```

Call `bsp_display_start()` once. Every later LVGL call must be bracketed by `bsp_display_lock(0)` and `bsp_display_unlock()`. The screen is 720 × 1280 pixels in RGB565. Use `bsp_display_brightness_set(0..100)` after startup to adjust brightness.

The 1.3.1 BSP source probes the touchscreen controller over I2C and selects the matching panel data automatically:

| Detected touch controller/revision | Display panel path |
| --- | --- |
| ST712x, firmware byte `1` | ST7121 init data, ST7123 panel driver |
| ST712x, firmware byte `3` | ST7123 init data and driver |
| GT911 address | ILI9881C panel and GT911 touch |

So the same `bsp_display_start()` entry point covers ST7121, ST7123, and older ILI9881C/GT911 units. Registry metadata currently names ILI9881C and ST7123, but the v1.3.1 source contains the ST7121 probe and init path too.

## Wi-Fi C6 power and transport

The BSP's feature switch enables power to the C6 through the onboard PI4IOE5V6408 I/O expander:

```c
ESP_ERROR_CHECK(bsp_feature_enable(BSP_FEATURE_WIFI, true));
```

Disable it with `bsp_feature_enable(BSP_FEATURE_WIFI, false)` when shutting Wi-Fi down. The board header maps `BSP_WIFI_EN` to expander output 0. This is only power control: it does not start the ESP-Hosted SDIO link, install the remote Wi-Fi driver, initialize `esp_netif`, or connect to an AP. Add the `esp_hosted` and `esp_wifi_remote` components and follow their matching version's P4-host/C6-coprocessor configuration before calling the standard ESP-IDF Wi-Fi APIs.

Do not copy unrelated P4 EV-board pin defaults. On this project’s pinned ESP-Hosted 1.4.0 + IDF 5.5.3, the shown Kconfig entries produce the board pin map: CLK GPIO12, CMD GPIO13, D0 GPIO11, D1 GPIO10, D2 GPIO9, D3 GPIO8, C6 reset GPIO15. Recheck symbols if upgrading ESP-Hosted; its newer 2.x Kconfig uses per-slot private pin defaults on some targets.

## First hardware bring-up

Start with the official display example or the minimal display snippet above. A successful build does not require a serial port to be visible; flashing and monitoring do. Connect the Tab5's USB-C device/OTG port to the computer with a known data-capable cable. On macOS, verify that USB Serial/JTAG enumerates before choosing the `idf.py -p` device. If it does not appear, check cable/port and download mode (hold Reset until the green LED blinks rapidly, then release) using M5Stack's documented procedure. The C6's factory Wi-Fi firmware is separate from P4 application firmware and can be restored using M5Stack's C6 Wi-Fi SDIO recovery procedure if it was overwritten.

## Source references

- [ESP Component Registry: m5stack_tab5 1.3.1 README and dependencies](https://components.espressif.com/components/espressif/m5stack_tab5/versions/1.3.1/readme)
- [ESP Component Registry: m5stack_tab5 1.3.1 API](https://components.espressif.com/components/espressif/m5stack_tab5/versions/1.3.1/api?language=en)
- [Official BSP display implementation](https://github.com/espressif/esp-bsp/blob/master/bsp/m5stack_tab5/src/bsp_display.c)
- [Official BSP public board header](https://github.com/espressif/esp-bsp/blob/master/bsp/m5stack_tab5/include/bsp/m5stack_tab5.h)
- [Official BSP component manifest](https://github.com/espressif/esp-bsp/blob/master/bsp/m5stack_tab5/idf_component.yml)
- [M5Stack UserDemo app component manifest](https://github.com/m5stack/M5Tab5-UserDemo/blob/main/platforms/tab5/main/idf_component.yml)
- [M5Stack UserDemo sdkconfig defaults](https://github.com/m5stack/M5Tab5-UserDemo/blob/main/platforms/tab5/sdkconfig.defaults)
- [M5Stack UserDemo setup and flashing instructions](https://docs.m5stack.com/en/esp_idf/m5tab5/userdemo)
- [M5Stack C6 Wi-Fi module firmware recovery](https://docs.m5stack.com/en/guide/restore_factory/m5tab5_c6_wifi)
