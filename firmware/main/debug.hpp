#pragma once
#include "esp_http_server.h"
// Development aid: GET /screenshot returns the current screen as a BMP.
void debug_register(httpd_handle_t server);
