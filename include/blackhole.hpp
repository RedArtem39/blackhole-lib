// blackhole-lib — C++ wrapper over blackhole.h (header only, C++17 or newer).
//
//   bh::Renderer renderer;
//   bh::Params p(bh::Preset::Frost);
//   p.camera_pitch = 0.25f;
//   renderer.renderPng(p, 3840, 2160, "frost.png");
#pragma once

#include "blackhole.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bh {

class Error : public std::runtime_error {
public:
    Error(bh_result code, const char* msg) : std::runtime_error(msg ? msg : "blackhole error"), code_(code) {}
    bh_result code() const noexcept { return code_; }

private:
    bh_result code_;
};

inline bh_result check(bh_result r) {
    if (r < 0) throw Error(r, bh_get_last_error());
    return r;
}

enum class Preset : int {
    Gargantua  = BH_PRESET_GARGANTUA,
    Frost      = BH_PRESET_FROST,
    Crimson    = BH_PRESET_CRIMSON,
    Monochrome = BH_PRESET_MONOCHROME,
    Void       = BH_PRESET_VOID,
    Quasar     = BH_PRESET_QUASAR,
};
inline constexpr int kPresetCount = BH_PRESET_COUNT;

inline const char* name(Preset p) { return bh_preset_name(static_cast<bh_preset>(p)); }

constexpr bh_color rgb(float r, float g, float b) { return bh_color{r, g, b}; }

// bh_params with defaults filled in. Fields are public: p.ring_color = bh::rgb(1, .5f, .2f);
struct Params : bh_params {
    Params() { bh_params_default(this); }
    explicit Params(Preset preset) { check(bh_params_preset(this, static_cast<bh_preset>(preset))); }

    static Params lerp(const Params& a, const Params& b, float t) {
        Params out;
        bh_params_lerp(&out, &a, &b, t);
        return out;
    }
};

class Renderer {
public:
    explicit Renderer(bool validation = false, int gpuIndex = -1) {
        bh_renderer_desc d;
        bh_renderer_desc_default(&d);
        d.enable_validation = validation ? 1 : 0;
        d.gpu_index = gpuIndex;
        check(bh_renderer_create(&d, &r_));
    }
    ~Renderer() { bh_renderer_destroy(r_); }
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&& o) noexcept : r_(std::exchange(o.r_, nullptr)) {}
    Renderer& operator=(Renderer&& o) noexcept {
        if (this != &o) {
            bh_renderer_destroy(r_);
            r_ = std::exchange(o.r_, nullptr);
        }
        return *this;
    }

    // RGBA8, sRGB, top row first.
    std::vector<std::uint8_t> render(const bh_params& p, std::uint32_t width, std::uint32_t height) {
        std::vector<std::uint8_t> px(std::size_t(width) * height * 4);
        check(bh_render_rgba8(r_, &p, width, height, px.data(), 0));
        return px;
    }
    void render(const bh_params& p, std::uint32_t width, std::uint32_t height, void* pixels, std::size_t stride = 0) {
        check(bh_render_rgba8(r_, &p, width, height, pixels, stride));
    }
    void renderPng(const bh_params& p, std::uint32_t width, std::uint32_t height, const std::string& pathUtf8) {
        check(bh_render_png(r_, &p, width, height, pathUtf8.c_str()));
    }

    std::string gpuName() const { return bh_renderer_gpu_name(r_); }
    bh_renderer* handle() const noexcept { return r_; }

private:
    bh_renderer* r_ = nullptr;
};

// Renders into a native window. Present once per frame.
class WindowTarget {
public:
    WindowTarget(Renderer& r, void* hwnd) { check(bh_target_create_win32(r.handle(), hwnd, &t_)); }
    ~WindowTarget() { bh_target_destroy(t_); }
    WindowTarget(const WindowTarget&) = delete;
    WindowTarget& operator=(const WindowTarget&) = delete;
    WindowTarget(WindowTarget&& o) noexcept : t_(std::exchange(o.t_, nullptr)) {}
    WindowTarget& operator=(WindowTarget&& o) noexcept {
        if (this != &o) {
            bh_target_destroy(t_);
            t_ = std::exchange(o.t_, nullptr);
        }
        return *this;
    }

    // false when nothing was drawn (window minimized / zero size)
    bool present(const bh_params& p) { return check(bh_target_present(t_, &p)) == BH_OK; }
    void setVsync(bool on) { bh_target_set_vsync(t_, on ? 1 : 0); }
    bh_target* handle() const noexcept { return t_; }

private:
    bh_target* t_ = nullptr;
};

}  // namespace bh
