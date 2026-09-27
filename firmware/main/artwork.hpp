#pragma once
#include <cstdint>
#include <string>

// A decoded album-art image. The pixel buffer lives in PSRAM and is freed with
// heap_caps_free, so it can be wrapped in an lv_image_dsc_t directly:
// data = pixels, data_size = stride * height, LV_COLOR_FORMAT_RGB565.
struct Artwork {
    uint8_t* pixels = nullptr;                  // RGB565 little-endian
    uint32_t width = 0, height = 0, stride = 0; // stride in bytes
    Artwork() = default;
    ~Artwork();
    Artwork(Artwork&& other) noexcept;
    Artwork& operator=(Artwork&& other) noexcept;
    Artwork(const Artwork&) = delete;
    Artwork& operator=(const Artwork&) = delete;
};

// Downloads the image and returns exactly side x side RGB565 pixels: centre
// cropped, resampled, and with corners of corner_radius already rounded
// against background (0xRRGGBB). The UI can then draw it as a plain
// unscaled rectangle, which is far cheaper than scaling or clipping per frame.
// Throws std::runtime_error on any failure: bad URI, network, oversized body,
// unsupported format, or decode error.
Artwork fetch_artwork(const std::string& speaker_ip, const std::string& art_uri, uint32_t side,
                      uint32_t corner_radius = 0, uint32_t background = 0);

// Resolves a Sonos albumArtURI to an absolute URL. Exposed for testing.
std::string artwork_url(const std::string& speaker_ip, const std::string& art_uri);
