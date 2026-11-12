// blackhole-lib example: interactive window (C++ API + window target).
//
//   drag            orbit            wheel   zoom
//   1..6            presets (smooth blend)
//   F11 / dbl-click fullscreen       Space   pause
//   + / -           exposure         [ / ]   render scale
//   V               vsync            F12     save PNG of the current view
//   Esc             quit
//
//   viewer.exe --test   off-screen, click-through run of 60 frames, prints timing
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>

#include "blackhole.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>

namespace {

struct State {
    HWND hwnd = nullptr;
    bool test = false, quit = false, paused = false, fullscreen = false, vsync = true, vsyncDirty = false;
    bool drag = false, shot = false;
    POINT last{};
    float yawOff = 0, pitchOff = 0, dist = 24.0f, exposure = 1.0f, scale = 1.0f;
    int preset = 0;
    bh::Params from{bh::Preset::Gargantua}, to{bh::Preset::Gargantua};
    double blendStart = -10.0;
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
} S;

void toggleFullscreen() {
    if (S.test) return;
    DWORD style = (DWORD)GetWindowLongW(S.hwnd, GWL_STYLE);
    if (!S.fullscreen) {
        MONITORINFO mi{sizeof(mi)};
        if (GetWindowPlacement(S.hwnd, &S.placement) &&
            GetMonitorInfoW(MonitorFromWindow(S.hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLongW(S.hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(S.hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
    } else {
        SetWindowLongW(S.hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(S.hwnd, &S.placement);
        SetWindowPos(S.hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    S.fullscreen = !S.fullscreen;
}

LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NCHITTEST:
        if (S.test) return HTTRANSPARENT;
        break;
    case WM_MOUSEACTIVATE:
        if (S.test) return MA_NOACTIVATE;
        break;
    case WM_CLOSE: S.quit = true; return 0;
    case WM_LBUTTONDOWN:
        S.drag = true;
        S.last = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        SetCapture(h);
        return 0;
    case WM_LBUTTONUP:
        S.drag = false;
        ReleaseCapture();
        return 0;
    case WM_LBUTTONDBLCLK: toggleFullscreen(); return 0;
    case WM_MOUSEMOVE:
        if (S.drag) {
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            S.yawOff += (p.x - S.last.x) * 0.004f;
            S.pitchOff += (p.y - S.last.y) * 0.003f;
            S.last = p;
        }
        return 0;
    case WM_MOUSEWHEEL:
        S.dist = std::clamp(S.dist * std::pow(0.92f, GET_WHEEL_DELTA_WPARAM(w) / 120.0f), 4.0f, 90.0f);
        return 0;
    case WM_KEYDOWN:
        if (w >= '1' && w < '1' + bh::kPresetCount) S.preset = int(w - '1') + 100;   // +100 = "pending"
        switch (w) {
        case VK_ESCAPE: S.quit = true; break;
        case VK_F11: toggleFullscreen(); break;
        case VK_SPACE: S.paused = !S.paused; break;
        case VK_F12: S.shot = true; break;
        case 'V': S.vsync = !S.vsync; S.vsyncDirty = true; break;
        case VK_OEM_PLUS: case VK_ADD: S.exposure *= 1.15f; break;
        case VK_OEM_MINUS: case VK_SUBTRACT: S.exposure /= 1.15f; break;
        case VK_OEM_4: S.scale = std::max(0.375f, S.scale - 0.125f); break;
        case VK_OEM_6: S.scale = std::min(2.0f, S.scale + 0.125f); break;
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

float smooth(float t) { return t * t * (3.0f - 2.0f * t); }

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--test")) S.test = true;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HINSTANCE hinst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"BlackHoleViewer";
    RegisterClassExW(&wc);

    DWORD style = S.test ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    DWORD ex = S.test ? (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT) : 0;
    RECT r{0, 0, 1600, 900};
    AdjustWindowRectEx(&r, style, FALSE, ex);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    int x = S.test ? -20000 : std::max(0, (GetSystemMetrics(SM_CXSCREEN) - ww) / 2);
    int y = S.test ? -20000 : std::max(0, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2);
    S.hwnd = CreateWindowExW(ex, wc.lpszClassName, L"Black Hole", style, x, y, ww, wh, nullptr, nullptr, hinst, nullptr);
    ShowWindow(S.hwnd, S.test ? SW_SHOWNOACTIVATE : SW_SHOW);

    try {
        bh::Renderer renderer(std::getenv("BH_VALIDATE") != nullptr);
        bh::WindowTarget target(renderer, S.hwnd);
        if (S.test) {
            target.setVsync(false);
            S.vsync = false;
        }

        auto t0 = std::chrono::steady_clock::now();
        auto prev = t0;
        double anim = 0, fpsTimer = 0, testMs = 0;
        int fpsFrames = 0, frames = 0;

        while (!S.quit) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (S.quit) break;

            auto now = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(now - prev).count();
            prev = now;
            double wall = std::chrono::duration<double>(now - t0).count();
            if (!S.paused) anim += std::min(dt, 0.1);

            if (S.vsyncDirty) {
                target.setVsync(S.vsync);
                S.vsyncDirty = false;
            }

            // preset change: blend from whatever is on screen right now
            float bt = std::clamp(float((wall - S.blendStart) / 1.5), 0.0f, 1.0f);
            bh::Params look = bh::Params::lerp(S.from, S.to, smooth(bt));
            if (S.preset >= 100) {
                S.from = look;
                S.to = bh::Params(static_cast<bh::Preset>(S.preset - 100));
                S.preset -= 100;
                S.blendStart = wall;
                look = S.from;
            }

            float t = (float)anim;
            bh::Params p = look;
            p.time = t;
            p.camera_yaw = 0.55f + t * 0.012f + S.yawOff;
            float basePitch = 0.11f + 0.045f * std::sin(t * 0.041f);
            float pitch = std::clamp(basePitch + S.pitchOff, -1.45f, 1.45f);
            S.pitchOff = pitch - basePitch;
            p.camera_pitch = pitch;
            p.camera_distance = S.dist;
            p.exposure *= S.exposure;
            p.render_scale = S.scale;

            auto f0 = std::chrono::steady_clock::now();
            target.present(p);
            frames++;

            if (S.shot) {
                S.shot = false;
                RECT rc;
                GetClientRect(S.hwnd, &rc);
                SYSTEMTIME st;
                GetLocalTime(&st);
                std::string name = std::format("blackhole_{:04}{:02}{:02}_{:02}{:02}{:02}.png", st.wYear, st.wMonth,
                                               st.wDay, st.wHour, st.wMinute, st.wSecond);
                renderer.renderPng(p, (uint32_t)std::max<LONG>(rc.right, 1), (uint32_t)std::max<LONG>(rc.bottom, 1),
                                   name);
            }

            if (S.test) {
                if (frames == 30) {   // switch preset mid-run to exercise blending
                    S.preset = 101;
                }
                if (frames > 5)
                    testMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - f0).count();
                if (frames >= 60) S.quit = true;
            }

            fpsTimer += dt;
            fpsFrames++;
            if (fpsTimer > 0.5 && !S.test) {
                std::wstring title = std::format(L"Black Hole  |  {:.0f} fps  |  {}  |  scale {:.0f}%  |  {}",
                                                 fpsFrames / fpsTimer, std::wstring(bh::name((bh::Preset)S.preset),
                                                 bh::name((bh::Preset)S.preset) + strlen(bh::name((bh::Preset)S.preset))),
                                                 S.scale * 100.0f, S.vsync ? L"vsync" : L"uncapped");
                SetWindowTextW(S.hwnd, title.c_str());
                fpsTimer = 0;
                fpsFrames = 0;
            }
        }
        if (S.test)
            printf("viewer test: %d frames, avg present %.2f ms, gpu %s\n", frames, testMs / std::max(1, frames - 5),
                   renderer.gpuName().c_str());
    } catch (const bh::Error& e) {
        fprintf(stderr, "blackhole: %s\n", e.what());
        if (!S.test) MessageBoxA(nullptr, e.what(), "Black Hole", MB_ICONERROR);
        return 1;
    }
    DestroyWindow(S.hwnd);
    return 0;
}
