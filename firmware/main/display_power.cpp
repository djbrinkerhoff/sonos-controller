#include "display_power.hpp"
#include "fast_flush.hpp"
#include "bsp/esp-bsp.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7123.h"
#include "esp_lcd_touch_st7123.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/mipi_dsi_host_ll.h"

namespace {
const char* TAG="display";
lv_display_t* display=nullptr;
esp_lcd_panel_io_handle_t io=nullptr;
esp_lcd_dsi_bus_handle_t bus=nullptr;
bool teardown=false;  // delete the DPI driver while asleep (ST7121 only)
bool asleep=false;

// The DPI driver's private struct starts with the public panel base and the
// DSI bus handle (ESP-IDF 5.5.3, esp_lcd_panel_dpi.c). The bus is needed to
// recreate the driver and is not otherwise exposed by the BSP.
struct DpiPanelPrefix { esp_lcd_panel_t base; esp_lcd_dsi_bus_handle_t bus; };

// The BSP's ST7121 settings (bsp_display.c).
const esp_lcd_dpi_panel_config_t dpi_config=[]{
    esp_lcd_dpi_panel_config_t c={};
    c.virtual_channel=0;
    c.dpi_clk_src=MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    c.dpi_clock_freq_mhz=70;
    c.in_color_format=LCD_COLOR_FMT_RGB565;
    c.num_fbs=CONFIG_BSP_LCD_DPI_BUFFER_NUMS;
    c.video_timing.h_size=BSP_LCD_H_RES; c.video_timing.v_size=BSP_LCD_V_RES;
    c.video_timing.hsync_back_porch=40; c.video_timing.hsync_pulse_width=2; c.video_timing.hsync_front_porch=40;
    c.video_timing.vsync_back_porch=24; c.video_timing.vsync_pulse_width=20; c.video_timing.vsync_front_porch=200;
#if CONFIG_BSP_LCD_USE_DMA2D
    c.flags.use_dma2d=true;
#endif
    return c;
}();
// The panel is only switched off (DISPOFF), never put in sleep mode: after
// SLPIN, SLPOUT and DISPON left the glass dark on hardware, and only the full
// reset and 21-command init (1.1 s) brought it back. Display-off keeps the
// panel's state, so waking takes one command (~76 ms including the redraw).
const st7123_lcd_init_cmd_t wake_cmds[]={{LCD_CMD_DISPON,nullptr,0,20}};

// Board version 3 (ST7121) reports touch firmware 1, as the BSP detects it.
bool is_st7121() {
    esp_lcd_panel_io_handle_t touch=nullptr;
    // ESP_LCD_TOUCH_IO_I2C_ST7123_CONFIG(), spelled out: its designator order is C-only.
    esp_lcd_panel_io_i2c_config_t config={};
    config.dev_addr=ESP_LCD_TOUCH_IO_I2C_ST7123_ADDRESS;
    config.scl_speed_hz=100000;
    config.control_phase_bytes=1;
    config.lcd_cmd_bits=16;
    config.flags.disable_control_phase=1;
    if(esp_lcd_new_panel_io_i2c(bsp_i2c_get_handle(),&config,&touch)!=ESP_OK) return false;
    uint8_t version=0;
    const bool read=esp_lcd_panel_io_rx_param(touch,0x0000,&version,1)==ESP_OK;
    esp_lcd_panel_io_del(touch);
    return read && version==1;
}
// With the DPI driver gone the host must leave video mode, or it waits for
// pixels that never come and cannot send the wake command. The lanes then
// idle in LP-11. ULPS would save ~5 mA more but did not come back out on
// hardware: the wake command never reached the panel.
void command_mode() {
    mipi_dsi_host_ll_enable_video_mode(&MIPI_DSI_HOST,false);
    mipi_dsi_host_ll_set_clock_lane_state(&MIPI_DSI_HOST,MIPI_DSI_LL_CLOCK_LANE_STATE_LP);
}
}

void display_power_init(lv_display_t* d) {
    display=d;
    io=fast_flush_panel_io();
    auto panel=fast_flush_panel();
    if(!io || !panel) { ESP_LOGW(TAG,"panel handles unavailable; sleep keeps the panel driver"); return; }
    bus=reinterpret_cast<DpiPanelPrefix*>(panel)->bus;
    teardown=bus && is_st7121();
    ESP_LOGI(TAG,"asleep, the panel %s",teardown?"driver is deleted":"stays driven");
}
bool display_asleep() { return asleep; }

void display_sleep() {
    auto panel=fast_flush_panel();
    if(asleep || !panel) return;
    fast_flush_suspend();
    esp_lcd_panel_disp_on_off(panel,false);
    if(teardown) {
        esp_lcd_panel_del(panel);
        command_mode();
    }
    asleep=true;
    ESP_LOGI(TAG,"asleep");
}

void display_wake() {
    if(!asleep) return;
    const int64_t started=esp_timer_get_time();
    esp_lcd_panel_handle_t panel=fast_flush_panel();
    if(teardown) {
        st7123_vendor_config_t vendor={};
        vendor.init_cmds=wake_cmds;
        vendor.init_cmds_size=sizeof(wake_cmds)/sizeof(wake_cmds[0]);
        vendor.mipi_config.dsi_bus=bus;
        vendor.mipi_config.dpi_config=&dpi_config;
        esp_lcd_panel_dev_config_t config={};
        config.reset_gpio_num=-1;
        config.rgb_ele_order=BSP_LCD_COLOR_SPACE;
        config.bits_per_pixel=BSP_LCD_BITS_PER_PIXEL;
        config.vendor_config=&vendor;
        panel=nullptr;
        esp_err_t result=esp_lcd_new_panel_st7123(io,&config,&panel);
        if(result==ESP_OK) result=esp_lcd_panel_init(panel);
        if(result!=ESP_OK || !fast_flush_resume(panel)) {
            // A black screen that never recovers is worse than a restart.
            ESP_LOGE(TAG,"panel recreate failed: %s; restarting",esp_err_to_name(result));
            esp_restart();
        }
    } else {
        esp_lcd_panel_disp_on_off(panel,true);
        fast_flush_resume(panel);
    }
    asleep=false;
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    lv_refr_now(display);
    ESP_LOGI(TAG,"awake in %lld ms",(esp_timer_get_time()-started)/1000);
}
