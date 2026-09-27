#pragma once
#include "esp_http_server.h"

// Registers POST /ota on the given server. The body is a signed
// sonos_controller.bin; on success the device reboots into it after ~1 s.
void ota_register(httpd_handle_t server);

// Logs the running partition, its OTA state and the app version.
void ota_log_boot();

// Confirms a freshly updated app so the bootloader keeps it. Idempotent;
// a no-op once the image is no longer pending verification.
void ota_mark_healthy();
