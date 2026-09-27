#pragma once
#include "esp_http_server.h"
// Development aids over Wi-Fi, since opening the USB console resets the
// device (which would roll back an unconfirmed OTA image):
//   GET /screenshot  current screen as a BMP
//   GET /tasks       FreeRTOS task states, stack headroom and CPU share
//   GET /log         the most recent ~32 KB of log output
void debug_log_init();  // call first thing in app_main
void debug_register(httpd_handle_t server);
