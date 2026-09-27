#include "artwork.hpp"
#include "network.hpp"
#include "sonos.hpp"
#include "driver/jpeg_decode.h"
#include "lvgl.h"
// LVGL's lodepng.h declares its C++ overloads inside extern "C", which C++
// cannot include; these are the two C entry points used here.
extern "C" unsigned lodepng_decode32(unsigned char** out, unsigned* w, unsigned* h, const unsigned char* in, size_t insize);
extern "C" const char* lodepng_error_text(unsigned code);
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <algorithm>
#include <cmath>
#include <utility>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <stdexcept>

Artwork::~Artwork() { heap_caps_free(pixels); }
Artwork::Artwork(Artwork&& other) noexcept
    : pixels(other.pixels), width(other.width), height(other.height), stride(other.stride) {
    other.pixels = nullptr; other.width = other.height = other.stride = 0;
}
Artwork& Artwork::operator=(Artwork&& other) noexcept {
    if (this != &other) {
        heap_caps_free(pixels);
        pixels = other.pixels; width = other.width; height = other.height; stride = other.stride;
        other.pixels = nullptr; other.width = other.height = other.stride = 0;
    }
    return *this;
}

namespace {
const char* TAG = "artwork";
constexpr size_t MAX_BODY = 1024 * 1024;

struct Buffer {
    uint8_t* data = nullptr;
    size_t size = 0, capacity = 0;
    ~Buffer() { heap_caps_free(data); }
    Buffer() = default;
    Buffer(Buffer&& o) noexcept : data(o.data), size(o.size), capacity(o.capacity) {
        o.data = nullptr; o.size = o.capacity = 0;
    }
    Buffer& operator=(Buffer&& o) noexcept {
        if (this != &o) {
            heap_caps_free(data);
            data = o.data; size = o.size; capacity = o.capacity;
            o.data = nullptr; o.size = o.capacity = 0;
        }
        return *this;
    }
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    void allocate(size_t bytes) {
        if (bytes <= capacity) { size = 0; return; }
        auto* grown = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
        if (!grown) throw std::runtime_error("Out of PSRAM");
        heap_caps_free(data);
        data = grown; capacity = bytes; size = 0;
    }
};
struct Body : Buffer {
    // Set from the HTTP event callback, which runs inside esp_http_client's C
    // stack, so failures are flagged rather than thrown.
    bool too_large = false, no_memory = false;
    bool push(const void* chunk, size_t len) {
        if (size + len > MAX_BODY) { too_large = true; return false; }
        if (size + len > capacity) {
            size_t next = std::max<size_t>(capacity * 2, 64 * 1024);
            while (next < size + len) next *= 2;
            auto* grown = static_cast<uint8_t*>(heap_caps_malloc(next, MALLOC_CAP_SPIRAM));
            if (!grown) { no_memory = true; return false; }
            if (size) std::memcpy(grown, data, size);
            heap_caps_free(data);
            data = grown; capacity = next;
        }
        std::memcpy(data + size, chunk, len);
        size += len;
        return true;
    }
};
esp_err_t on_data(esp_http_client_event_t* e) {
    if (e->event_id == HTTP_EVENT_ON_DATA && e->data_len > 0)
        if (!static_cast<Body*>(e->user_data)->push(e->data, e->data_len)) return ESP_FAIL;
    return ESP_OK;
}
void check(esp_err_t result, const char* context) {
    if (result != ESP_OK) throw std::runtime_error(std::string(context) + ": " + esp_err_to_name(result));
}
void download(const std::string& url, Body& body) {
    if (!network_online()) throw std::runtime_error("Wi-Fi is disconnected");
    esp_http_client_config_t config{};
    config.url = url.c_str();
    config.timeout_ms = 5000;      // applies to each connect/read, not the whole transfer
    config.event_handler = on_data;
    config.user_data = &body;
    config.max_redirection_count = 3;
    config.crt_bundle_attach = esp_crt_bundle_attach;  // for https:// artwork URIs
    config.buffer_size = 4096;
    auto* client = esp_http_client_init(&config);
    if (!client) throw std::runtime_error("HTTP allocation failed");
    struct Cleanup { esp_http_client_handle_t handle; ~Cleanup() { esp_http_client_cleanup(handle); } } cleanup{client};
    auto result = esp_http_client_perform(client);
    if (body.too_large) throw std::runtime_error("Artwork exceeds 1 MiB limit");
    if (body.no_memory) throw std::runtime_error("Out of PSRAM for artwork download");
    check(result, "Artwork download failed");
    int status = esp_http_client_get_status_code(client);
    if (status != 200) throw std::runtime_error("Artwork HTTP error " + std::to_string(status));
}

// The hardware decoder engine is created lazily and kept for the process's
// lifetime; creation and use are serialized even though the caller is a
// single worker task.
std::mutex engine_lock;
jpeg_decoder_handle_t engine_handle;
jpeg_decoder_handle_t engine() {  // call with engine_lock held
    if (!engine_handle) {
        jpeg_decode_engine_cfg_t config{};
        config.timeout_ms = 1000;  // a corrupt bitstream must not hang the worker
        if (jpeg_new_decoder_engine(&config, &engine_handle) != ESP_OK) engine_handle = nullptr;
    }
    return engine_handle;
}
uint32_t align_up(uint32_t value, uint32_t to) { return (value + to - 1) / to * to; }

struct Image {
    Buffer buffer;
    uint32_t w = 0, h = 0;
    uint16_t* pixels() { return reinterpret_cast<uint16_t*>(buffer.data); }
};

// The decoder pads output rows and columns up to the JPEG MCU block (8 px, or
// 16 px for subsampled chroma), so the real picture is cropped out of the
// padded result. Grayscale scans only decode to GRAY, which is expanded to
// RGB565 by hand.
Image decode_jpeg(const uint8_t* data, size_t size) {
    jpeg_decode_picture_info_t info{};
    check(jpeg_decoder_get_info(data, size, &info), "JPEG header");
    if (!info.width || !info.height) throw std::runtime_error("JPEG has zero dimensions");
    if (info.width > 4096 || info.height > 4096) throw std::runtime_error("Artwork dimensions too large");
    const bool gray = info.sample_method == JPEG_DOWN_SAMPLING_GRAY;
    const uint32_t bpp = gray ? 1 : 2;
    jpeg_decode_memory_alloc_cfg_t mem_cfg{};
    mem_cfg.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;
    size_t raw_size = 0;
    auto* raw = static_cast<uint8_t*>(jpeg_alloc_decoder_mem(align_up(info.width, 16) * align_up(info.height, 16) * bpp, &mem_cfg, &raw_size));
    if (!raw) throw std::runtime_error("Out of PSRAM for decoded artwork");
    struct Free { uint8_t* p; ~Free() { heap_caps_free(p); } } free_raw{raw};
    jpeg_decode_cfg_t config{};
    config.output_format = gray ? JPEG_DECODE_OUT_FORMAT_GRAY : JPEG_DECODE_OUT_FORMAT_RGB565;
    config.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;  // little-endian, as LVGL RGB565 expects
    config.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
    uint32_t out_size = 0;
    {
        std::lock_guard<std::mutex> lock(engine_lock);
        if (!engine()) throw std::runtime_error("JPEG decoder engine unavailable");
        check(jpeg_decoder_process(engine_handle, &config, data, size, raw, raw_size, &out_size), "JPEG decode");
    }
    // Padded decode dimensions. For single-component scans the MCU factor is
    // not reported, so it is matched against out_size instead.
    uint32_t pw = 0, pv = 0;
    if (!gray) {
        uint32_t mx = 8, my = 8;
        if (info.sample_method == JPEG_DOWN_SAMPLING_YUV422 || info.sample_method == JPEG_DOWN_SAMPLING_YUV420) mx = 16;
        if (info.sample_method == JPEG_DOWN_SAMPLING_YUV420) my = 16;
        pw = align_up(info.width, mx); pv = align_up(info.height, my);
        if (out_size != pw * pv * bpp) throw std::runtime_error("JPEG decode size mismatch");
    } else {
        for (uint32_t mx : {8u, 16u})
            for (uint32_t my : {8u, 16u})
                if (!pw && out_size == align_up(info.width, mx) * align_up(info.height, my)) {
                    pw = align_up(info.width, mx); pv = align_up(info.height, my);
                }
        if (!pw) throw std::runtime_error("JPEG decode size mismatch");
    }
    Image img;
    img.w = info.width; img.h = info.height;
    img.buffer.allocate(img.w * img.h * 2);
    img.buffer.size = img.w * img.h * 2;
    if (gray) {
        auto* dst = img.pixels();
        for (uint32_t y = 0; y < img.h; ++y)
            for (uint32_t x = 0; x < img.w; ++x) {
                uint8_t v = raw[y * pw + x];
                dst[y * img.w + x] = ((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3);
            }
    } else {
        for (uint32_t y = 0; y < img.h; ++y)
            std::memcpy(img.buffer.data + y * img.w * 2, raw + y * pw * bpp, img.w * 2);
    }
    return img;
}

// Sonos hands back whatever the music service supplies; Apple Music art is PNG.
// LVGL's copy of lodepng is modified: the "out" pointer it returns is an
// lv_draw_buf_t* whose data holds R,G,B,A bytes, freed with
// lv_draw_buf_destroy. Transparency is flattened onto the panel background.
Image decode_png(const uint8_t* data, size_t size) {
    unsigned char* out=nullptr; unsigned w=0, h=0;
    const unsigned error=lodepng_decode32(&out,&w,&h,data,size);
    auto* decoded=reinterpret_cast<lv_draw_buf_t*>(out);
    struct Free { lv_draw_buf_t* p; ~Free() { if(p) lv_draw_buf_destroy(p); } } free_decoded{decoded};
    if(error) throw std::runtime_error(std::string("PNG decode: ")+lodepng_error_text(error));
    if(!decoded || !w || !h || w>4096 || h>4096) throw std::runtime_error("Artwork dimensions too large");
    Image img;
    img.w=w; img.h=h;
    img.buffer.allocate(w*h*2);
    img.buffer.size=w*h*2;
    constexpr uint32_t bg_r=0x1e, bg_g=0x23, bg_b=0x28;
    auto* dst=img.pixels();
    for(uint32_t y=0;y<h;++y) {
        const unsigned char* row=decoded->data+y*decoded->header.stride;
        for(uint32_t x=0;x<w;++x) {
            const unsigned char* p=row+x*4;
            const uint32_t a=p[3];
            const uint32_t r=(p[0]*a+bg_r*(255-a))/255, g=(p[1]*a+bg_g*(255-a))/255, b=(p[2]*a+bg_b*(255-a))/255;
            dst[y*w+x]=((r>>3)<<11)|((g>>2)<<5)|(b>>3);
        }
    }
    return img;
}

// 2x2 box filter, applied iteratively: cheap and visibly better than nearest
// neighbour for large reductions.
void halve(Image& img) {
    const uint32_t dw = img.w / 2, dh = img.h / 2;
    Buffer out;
    out.allocate(dw * dh * 2);
    const auto* src = img.pixels();
    auto* dst = reinterpret_cast<uint16_t*>(out.data);
    for (uint32_t y = 0; y < dh; ++y)
        for (uint32_t x = 0; x < dw; ++x) {
            const uint16_t a = src[(2 * y) * img.w + 2 * x], b = src[(2 * y) * img.w + 2 * x + 1];
            const uint16_t c = src[(2 * y + 1) * img.w + 2 * x], d = src[(2 * y + 1) * img.w + 2 * x + 1];
            uint32_t r = ((a >> 11) + (b >> 11) + (c >> 11) + (d >> 11) + 2) / 4;
            uint32_t g = (((a >> 5) & 63) + ((b >> 5) & 63) + ((c >> 5) & 63) + ((d >> 5) & 63) + 2) / 4;
            uint32_t bl = ((a & 31) + (b & 31) + (c & 31) + (d & 31) + 2) / 4;
            dst[y * dw + x] = (r << 11) | (g << 5) | bl;
        }
    img.buffer = std::move(out);
    img.w = dw; img.h = dh;
}
// Centre square of the image, so covers fill their frame without letterboxing.
void crop_square(Image& img) {
    const uint32_t side = std::min(img.w, img.h);
    if (img.w == side && img.h == side) return;
    Buffer out;
    out.allocate(side * side * 2);
    const uint32_t x0 = (img.w - side) / 2, y0 = (img.h - side) / 2;
    for (uint32_t y = 0; y < side; ++y)
        std::memcpy(out.data + y * side * 2, img.buffer.data + ((y0 + y) * img.w + x0) * 2, side * 2);
    img.buffer = std::move(out);
    img.w = img.h = side;
}
// Bilinear resample to exactly side x side (up or down; large reductions are
// box-filtered by halve() first).
void resample(Image& img, uint32_t side) {
    if (img.w == side && img.h == side) return;
    Buffer out;
    out.allocate(side * side * 2);
    const auto* src = img.pixels();
    auto* dst = reinterpret_cast<uint16_t*>(out.data);
    const uint32_t scale_x = ((img.w - 1) << 16) / std::max<uint32_t>(side - 1, 1);
    const uint32_t scale_y = ((img.h - 1) << 16) / std::max<uint32_t>(side - 1, 1);
    for (uint32_t y = 0; y < side; ++y) {
        const uint32_t fy = y * scale_y, sy = fy >> 16, wy = (fy >> 8) & 255, sy1 = std::min(sy + 1, img.h - 1);
        for (uint32_t x = 0; x < side; ++x) {
            const uint32_t fx = x * scale_x, sx = fx >> 16, wx = (fx >> 8) & 255, sx1 = std::min(sx + 1, img.w - 1);
            const uint16_t p[4] = {src[sy * img.w + sx], src[sy * img.w + sx1], src[sy1 * img.w + sx], src[sy1 * img.w + sx1]};
            const uint32_t w[4] = {(256 - wx) * (256 - wy), wx * (256 - wy), (256 - wx) * wy, wx * wy};
            uint32_t r = 0, g = 0, b = 0;
            for (int i = 0; i < 4; ++i) { r += (p[i] >> 11) * w[i]; g += ((p[i] >> 5) & 63) * w[i]; b += (p[i] & 31) * w[i]; }
            dst[y * side + x] = static_cast<uint16_t>(((r >> 16) << 11) | ((g >> 16) << 5) | (b >> 16));
        }
    }
    img.buffer = std::move(out);
    img.w = img.h = side;
}
// Anti-aliased rounded corners blended into the background colour, so the UI
// can blit the image as a plain rectangle instead of clipping it every frame.
void round_corners(Image& img, uint32_t radius, uint32_t background) {
    if (!radius) return;
    const uint32_t br = (background >> 16) & 255, bg = (background >> 8) & 255, bb = background & 255;
    auto* px = img.pixels();
    for (uint32_t y = 0; y < radius; ++y)
        for (uint32_t x = 0; x < radius; ++x) {
            const float dx = radius - x - 0.5f, dy = radius - y - 0.5f;
            const float coverage = std::clamp(radius - std::sqrt(dx * dx + dy * dy) + 0.5f, 0.0f, 1.0f);
            if (coverage >= 1.0f) continue;
            const uint32_t a = static_cast<uint32_t>(coverage * 256);
            for (auto [cx, cy] : {std::pair{x, y}, {img.w - 1 - x, y}, {x, img.h - 1 - y}, {img.w - 1 - x, img.h - 1 - y}}) {
                uint16_t& p = px[cy * img.w + cx];
                const uint32_t r = ((p >> 11) << 3), g = (((p >> 5) & 63) << 2), b = ((p & 31) << 3);
                const uint32_t nr = (r * a + br * (256 - a)) >> 8, ng = (g * a + bg * (256 - a)) >> 8, nb = (b * a + bb * (256 - a)) >> 8;
                p = static_cast<uint16_t>(((nr >> 3) << 11) | ((ng >> 2) << 5) | (nb >> 3));
            }
        }
}
}
std::string artwork_url(const std::string& speaker_ip, const std::string& art_uri) {
    if (!sonos::valid_ipv4(speaker_ip)) throw std::runtime_error("Invalid speaker address");
    if (!art_uri.empty() && art_uri[0] == '/') return "http://" + speaker_ip + ":1400" + art_uri;
    if (art_uri.compare(0, 7, "http://") == 0 || art_uri.compare(0, 8, "https://") == 0) return art_uri;
    throw std::runtime_error("Unsupported artwork URI");
}
Artwork fetch_artwork(const std::string& speaker_ip, const std::string& art_uri, uint32_t side,
                      uint32_t corner_radius, uint32_t background) {
    if (!side) throw std::runtime_error("side must be positive");
    const int64_t started = esp_timer_get_time();
    Body body;
    download(artwork_url(speaker_ip, art_uri), body);
    static const uint8_t png_magic[] = {0x89, 'P', 'N', 'G'};
    Image img;
    if (body.size >= 2 && body.data[0] == 0xFF && body.data[1] == 0xD8) img = decode_jpeg(body.data, body.size);
    else if (body.size >= 4 && std::memcmp(body.data, png_magic, 4) == 0) img = decode_png(body.data, body.size);
    else throw std::runtime_error("Unsupported artwork format");
    const uint32_t decoded_w = img.w, decoded_h = img.h;
    crop_square(img);
    while (img.w >= 2 * side) halve(img);
    resample(img, side);
    round_corners(img, corner_radius, background);
    ESP_LOGI(TAG, "artwork: %u B -> %ux%u -> %ux%u in %lld ms", static_cast<unsigned>(body.size),
             static_cast<unsigned>(decoded_w), static_cast<unsigned>(decoded_h), static_cast<unsigned>(img.w),
             static_cast<unsigned>(img.h), (esp_timer_get_time() - started) / 1000);
    Artwork art;
    art.width = img.w; art.height = img.h; art.stride = img.w * 2;
    art.pixels = img.buffer.data;
    img.buffer.data = nullptr;
    return art;
}
