// Black Hole — realtime bent-ray black hole with a volumetric accretion disk.
// C++20 + Vulkan, Win32. Two passes: HDR scene -> mip chain (bloom) -> composite/tonemap.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <windowsx.h>
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#define VK_CHECK(x)                                                                              \
    do {                                                                                         \
        VkResult r_ = (x);                                                                       \
        if (r_ != VK_SUCCESS)                                                                    \
            throw std::runtime_error(std::format("{} failed: VkResult {} (line {})", #x, (int)r_, \
                                                 __LINE__));                                     \
    } while (0)

static FILE* g_log = nullptr;
static int g_validationErrors = 0;

static void logf(const std::string& s) {
    fprintf(stderr, "%s\n", s.c_str());
    fflush(stderr);
    if (g_log) {
        fprintf(g_log, "%s\n", s.c_str());
        fflush(g_log);
    }
    OutputDebugStringA((s + "\n").c_str());
}

static fs::path exeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

static std::vector<char> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("cannot open " + p.string());
    std::vector<char> data((size_t)f.tellg());
    f.seekg(0);
    f.read(data.data(), (std::streamsize)data.size());
    return data;
}

// ------------------------------------------------------------------------------ PNG (stored deflate)
static uint32_t crc32(const uint8_t* d, size_t n, uint32_t c = 0) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t k = i;
            for (int j = 0; j < 8; j++) k = (k & 1) ? 0xEDB88320u ^ (k >> 1) : k >> 1;
            table[i] = k;
        }
        init = true;
    }
    c = ~c;
    for (size_t i = 0; i < n; i++) c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

static void putBE(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24));
    v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x));
}

static bool writePNG(const fs::path& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgb) {
    std::vector<uint8_t> raw;
    raw.reserve((size_t(w) * 3 + 1) * h);
    for (uint32_t y = 0; y < h; y++) {
        raw.push_back(0);
        auto row = rgb.begin() + size_t(y) * w * 3;
        raw.insert(raw.end(), row, row + size_t(w) * 3);
    }
    std::vector<uint8_t> z{0x78, 0x01};
    size_t pos = 0;
    do {
        size_t n = std::min<size_t>(65535, raw.size() - pos);
        bool last = pos + n == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(n & 0xFF));
        z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n & 0xFF));
        z.push_back(uint8_t((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    } while (pos < raw.size());
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    putBE(z, (b << 16) | a);

    std::vector<uint8_t> out{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        putBE(out, (uint32_t)data.size());
        size_t start = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), data.begin(), data.end());
        putBE(out, crc32(out.data() + start, out.size() - start));
    };
    std::vector<uint8_t> ihdr;
    putBE(ihdr, w);
    putBE(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write((const char*)out.data(), (std::streamsize)out.size());
    return true;
}

// ------------------------------------------------------------------------------ app
struct PushScene {
    float resX, resY, time, fov;
    float yaw, pitch, dist, roll;
};
struct PushComp {
    float resX, resY, time, exposure;
};

struct Options {
    bool validate = false;
    std::string shot;       // non-empty: offscreen capture mode
    float shotTime = 20.0f;
    uint32_t w = 1600, h = 900;
    int frames = 3;
    bool hasYaw = false, hasPitch = false;
    float yaw = 0, pitch = 0, dist = -1, scale = 1.0f;
};

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
                                                    VkDebugUtilsMessageTypeFlagsEXT,
                                                    const VkDebugUtilsMessengerCallbackDataEXT* d, void*) {
    if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) g_validationErrors++;
    logf(std::string("[vk] ") + d->pMessage);
    return VK_FALSE;
}

class App {
public:
    explicit App(const Options& o) : opt(o) {
        scale = o.scale;
        if (o.dist > 0) dist = o.dist;
    }
    ~App() { destroy(); }
    int run();

private:
    static constexpr int FRAMES = 2;
    static constexpr VkFormat HDR_FMT = VK_FORMAT_R16G16B16A16_SFLOAT;

    Options opt;
    bool testMode() const { return !opt.shot.empty(); }

    // window / input
    HWND hwnd = nullptr;
    HINSTANCE hinst = nullptr;
    bool quit = false, minimized = false, paused = false, fullscreen = false, vsync = true;
    bool swapDirty = false, hdrDirty = false, shotRequested = false;
    bool drag = false;
    POINT last{};
    float yawOff = 0, pitchOff = 0, dist = 24.0f, exposure = 1.0f, scale = 1.0f;
    WINDOWPLACEMENT prevPlacement{sizeof(WINDOWPLACEMENT)};

    // vulkan core
    VkInstance inst = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT dbg = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    std::string devName;
    VkDevice dev = VK_NULL_HANDLE;
    uint32_t qfam = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd[FRAMES]{};
    VkSemaphore imgAvail[FRAMES]{};
    VkFence fence[FRAMES]{};
    int frame = 0;

    // swapchain
    VkSwapchainKHR swap = VK_NULL_HANDLE;
    VkFormat swapFmt = VK_FORMAT_UNDEFINED;
    VkExtent2D swapExt{};
    bool swapTransferSrc = false;
    std::vector<VkImage> swapImgs;
    std::vector<VkImageView> swapViews;
    std::vector<VkFramebuffer> swapFbs;
    std::vector<VkSemaphore> renderDone;

    // pipelines
    VkShaderModule vs = VK_NULL_HANDLE, fsScene = VK_NULL_HANDLE, fsComp = VK_NULL_HANDLE;
    VkRenderPass scenePass = VK_NULL_HANDLE, compPass = VK_NULL_HANDLE;
    VkPipelineLayout sceneLayout = VK_NULL_HANDLE, compLayout = VK_NULL_HANDLE;
    VkPipeline scenePipe = VK_NULL_HANDLE, compPipe = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkDescriptorSet dset = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;

    // HDR target
    VkImage hdrImg = VK_NULL_HANDLE;
    VkDeviceMemory hdrMem = VK_NULL_HANDLE;
    VkImageView hdrView0 = VK_NULL_HANDLE, hdrViewAll = VK_NULL_HANDLE;
    VkFramebuffer hdrFb = VK_NULL_HANDLE;
    VkExtent2D hdrExt{};
    uint32_t hdrMips = 1;

    // readback
    VkBuffer rbBuf = VK_NULL_HANDLE;
    VkDeviceMemory rbMem = VK_NULL_HANDLE;
    VkDeviceSize rbSize = 0;

    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);

    void createWindow();
    void toggleFullscreen();
    void createInstance();
    void pickDevice();
    void createDevice();
    void createStatic();
    void createSwapchain();
    void destroySwapViews();
    void createHDR();
    void destroyHDR();
    void ensureReadback();
    void saveShot(const fs::path& path);
    void record(VkCommandBuffer cb, uint32_t img, const PushScene& ps, const PushComp& pcmp, bool shot);
    bool drawFrame(const PushScene& ps, const PushComp& pcmp, bool shot);
    uint32_t memType(uint32_t bits, VkMemoryPropertyFlags props);
    VkShaderModule loadShader(const char* name);
    VkRenderPass makePass(VkFormat fmt, VkImageLayout finalLayout, VkPipelineStageFlags srcStage);
    VkPipeline makePipe(VkRenderPass rp, VkPipelineLayout layout, VkShaderModule frag);
    void destroy();
};

// ------------------------------------------------------------------------------ window
LRESULT CALLBACK App::wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    }
    auto app = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (app && app->hwnd == h) return app->handle(m, w, l);
    return DefWindowProcW(h, m, w, l);
}

LRESULT App::handle(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NCHITTEST:
        if (testMode()) return HTTRANSPARENT;
        break;
    case WM_MOUSEACTIVATE:
        if (testMode()) return MA_NOACTIVATE;
        break;
    case WM_CLOSE:
        quit = true;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        if (w == SIZE_MINIMIZED) {
            minimized = true;
        } else {
            minimized = false;
            swapDirty = true;
        }
        return 0;
    case WM_LBUTTONDOWN:
        drag = true;
        last = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        SetCapture(hwnd);
        return 0;
    case WM_LBUTTONUP:
        drag = false;
        ReleaseCapture();
        return 0;
    case WM_LBUTTONDBLCLK:
        toggleFullscreen();
        return 0;
    case WM_MOUSEMOVE:
        if (drag) {
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            yawOff += (p.x - last.x) * 0.004f;
            pitchOff += (p.y - last.y) * 0.003f;
            last = p;
        }
        return 0;
    case WM_MOUSEWHEEL: {
        float d = GET_WHEEL_DELTA_WPARAM(w) / 120.0f;
        dist = std::clamp(dist * std::pow(0.92f, d), 4.0f, 90.0f);
        return 0;
    }
    case WM_KEYDOWN:
        switch (w) {
        case VK_ESCAPE: quit = true; break;
        case VK_F11: toggleFullscreen(); break;
        case VK_SPACE: paused = !paused; break;
        case VK_F12: shotRequested = true; break;
        case 'V': vsync = !vsync; swapDirty = true; break;
        case 'R': yawOff = pitchOff = 0; dist = 24.0f; exposure = 1.0f; break;
        case VK_OEM_PLUS: case VK_ADD: exposure *= 1.15f; break;
        case VK_OEM_MINUS: case VK_SUBTRACT: exposure /= 1.15f; break;
        case VK_OEM_4: scale = std::max(0.375f, scale - 0.125f); hdrDirty = true; break;   // [
        case VK_OEM_6: scale = std::min(2.0f, scale + 0.125f); hdrDirty = true; break;     // ]
        }
        return 0;
    }
    return DefWindowProcW(hwnd, m, w, l);
}

void App::createWindow() {
    hinst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));   // IDC_ARROW
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"BlackHoleWnd";
    RegisterClassExW(&wc);

    DWORD style = testMode() ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    DWORD ex = testMode() ? (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT) : 0;
    RECT r{0, 0, (LONG)opt.w, (LONG)opt.h};
    AdjustWindowRectEx(&r, style, FALSE, ex);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    int x, y;
    if (testMode()) {
        x = -20000;
        y = -20000;
    } else {
        x = std::max(0, (GetSystemMetrics(SM_CXSCREEN) - ww) / 2);
        y = std::max(0, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2);
    }
    // hwnd must be known inside wndProc before the first sized message
    hwnd = CreateWindowExW(ex, wc.lpszClassName, L"Black Hole", style, x, y, ww, wh, nullptr, nullptr, hinst,
                           this);
    if (!hwnd) throw std::runtime_error("CreateWindowEx failed");
    ShowWindow(hwnd, testMode() ? SW_SHOWNOACTIVATE : SW_SHOW);
}

void App::toggleFullscreen() {
    if (testMode()) return;
    DWORD style = (DWORD)GetWindowLongW(hwnd, GWL_STYLE);
    if (!fullscreen) {
        MONITORINFO mi{sizeof(mi)};
        if (GetWindowPlacement(hwnd, &prevPlacement) &&
            GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLongW(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            ShowCursor(FALSE);
        }
    } else {
        SetWindowLongW(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &prevPlacement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        ShowCursor(TRUE);
    }
    fullscreen = !fullscreen;
    swapDirty = true;
}

// ------------------------------------------------------------------------------ vulkan setup
void App::createInstance() {
    bool haveVal = false;
    if (opt.validate) {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> layers(n);
        vkEnumerateInstanceLayerProperties(&n, layers.data());
        for (auto& p : layers)
            if (!strcmp(p.layerName, "VK_LAYER_KHRONOS_validation")) haveVal = true;
        if (!haveVal) logf("validation layer not available");
    }
    std::vector<const char*> exts{VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    if (haveVal) exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const char* layer = "VK_LAYER_KHRONOS_validation";

    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "BlackHole";
    ai.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &ai;
    ci.enabledExtensionCount = (uint32_t)exts.size();
    ci.ppEnabledExtensionNames = exts.data();
    if (haveVal) {
        ci.enabledLayerCount = 1;
        ci.ppEnabledLayerNames = &layer;
    }
    VK_CHECK(vkCreateInstance(&ci, nullptr, &inst));

    if (haveVal) {
        auto fn = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT d{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        d.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        d.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        d.pfnUserCallback = debugCallback;
        if (fn) fn(inst, &d, nullptr, &dbg);
    }

    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = hinst;
    sci.hwnd = hwnd;
    VK_CHECK(vkCreateWin32SurfaceKHR(inst, &sci, nullptr, &surface));
}

void App::pickDevice() {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(inst, &n, devs.data());
    int best = -1;
    for (auto d : devs) {
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qp(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qp.data());
        int fam = -1;
        for (uint32_t i = 0; i < qn; i++) {
            VkBool32 sup = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface, &sup);
            if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && sup) {
                fam = (int)i;
                break;
            }
        }
        if (fam < 0) continue;

        uint32_t en = 0;
        vkEnumerateDeviceExtensionProperties(d, nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> ep(en);
        vkEnumerateDeviceExtensionProperties(d, nullptr, &en, ep.data());
        bool hasSwap = std::any_of(ep.begin(), ep.end(), [](auto& e) {
            return !strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        });
        if (!hasSwap) continue;

        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(d, HDR_FMT, &fp);
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                    VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                    VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if ((fp.optimalTilingFeatures & need) != need) continue;

        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 1000
                    : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 100
                                                                             : 10;
        if (score > best) {
            best = score;
            phys = d;
            qfam = (uint32_t)fam;
            devName = p.deviceName;
        }
    }
    if (!phys) throw std::runtime_error("no suitable Vulkan GPU found");
    logf("GPU: " + devName);
}

void App::createDevice() {
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = &ext;
    VK_CHECK(vkCreateDevice(phys, &dci, nullptr, &dev));
    vkGetDeviceQueue(dev, qfam, 0, &queue);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = qfam;
    VK_CHECK(vkCreateCommandPool(dev, &pci, nullptr, &cmdPool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = cmdPool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = FRAMES;
    VK_CHECK(vkAllocateCommandBuffers(dev, &cai, cmd));

    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (int i = 0; i < FRAMES; i++) {
        VK_CHECK(vkCreateSemaphore(dev, &si, nullptr, &imgAvail[i]));
        VK_CHECK(vkCreateFence(dev, &fi, nullptr, &fence[i]));
    }
}

uint32_t App::memType(uint32_t bits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    throw std::runtime_error("no suitable memory type");
}

VkShaderModule App::loadShader(const char* name) {
    auto code = readFile(exeDir() / "shaders" / name);
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule m;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m));
    return m;
}

VkRenderPass App::makePass(VkFormat fmt, VkImageLayout finalLayout, VkPipelineStageFlags srcStage) {
    VkAttachmentDescription a{};
    a.format = fmt;
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    a.finalLayout = finalLayout;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = srcStage | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rci.attachmentCount = 1;
    rci.pAttachments = &a;
    rci.subpassCount = 1;
    rci.pSubpasses = &sp;
    rci.dependencyCount = 1;
    rci.pDependencies = &dep;
    VkRenderPass rp;
    VK_CHECK(vkCreateRenderPass(dev, &rci, nullptr, &rp));
    return rp;
}

VkPipeline App::makePipe(VkRenderPass rp, VkPipelineLayout layout, VkShaderModule frag) {
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = frag;
    st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2;
    gp.pStages = st;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &ds;
    gp.layout = layout;
    gp.renderPass = rp;
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &p));
    return p;
}

void App::createStatic() {
    vs = loadShader("fullscreen.vert.spv");
    fsScene = loadShader("scene.frag.spv");
    fsComp = loadShader("composite.frag.spv");

    scenePass = makePass(HDR_FMT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT);
    compPass = makePass(swapFmt, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0);

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    VK_CHECK(vkCreateSampler(dev, &sci, nullptr, &sampler));

    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 1;
    dli.pBindings = &b;
    VK_CHECK(vkCreateDescriptorSetLayout(dev, &dli, nullptr, &dsl));
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    VK_CHECK(vkCreateDescriptorPool(dev, &dpi, nullptr, &dpool));
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = dpool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &dsl;
    VK_CHECK(vkAllocateDescriptorSets(dev, &dai, &dset));

    VkPushConstantRange pr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushScene)};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &pr;
    VK_CHECK(vkCreatePipelineLayout(dev, &lci, nullptr, &sceneLayout));
    VkPushConstantRange pr2{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushComp)};
    lci.pPushConstantRanges = &pr2;
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &dsl;
    VK_CHECK(vkCreatePipelineLayout(dev, &lci, nullptr, &compLayout));

    scenePipe = makePipe(scenePass, sceneLayout, fsScene);
    compPipe = makePipe(compPass, compLayout, fsComp);
}

void App::destroySwapViews() {
    for (auto f : swapFbs) vkDestroyFramebuffer(dev, f, nullptr);
    for (auto v : swapViews) vkDestroyImageView(dev, v, nullptr);
    for (auto s : renderDone) vkDestroySemaphore(dev, s, nullptr);
    swapFbs.clear();
    swapViews.clear();
    renderDone.clear();
}

void App::createSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps));
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == UINT32_MAX) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        ext = {std::clamp((uint32_t)rc.right, caps.minImageExtent.width, caps.maxImageExtent.width),
               std::clamp((uint32_t)rc.bottom, caps.minImageExtent.height, caps.maxImageExtent.height)};
    }
    if (ext.width == 0 || ext.height == 0) {
        minimized = true;
        return;
    }

    if (swapFmt == VK_FORMAT_UNDEFINED) {
        uint32_t n = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &n, nullptr);
        std::vector<VkSurfaceFormatKHR> fmts(n);
        vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &n, fmts.data());
        swapFmt = fmts[0].format;
        for (auto& f : fmts)
            if ((f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_R8G8B8A8_SRGB) &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                swapFmt = f.format;
                break;
            }
    }

    uint32_t pn = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &pn, nullptr);
    std::vector<VkPresentModeKHR> modes(pn);
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &pn, modes.data());
    auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync) {
        if (has(VK_PRESENT_MODE_MAILBOX_KHR)) mode = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (has(VK_PRESENT_MODE_IMMEDIATE_KHR)) mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }

    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    swapTransferSrc = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface;
    ci.minImageCount = count;
    ci.imageFormat = swapFmt;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (swapTransferSrc ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = swap;
    VkSwapchainKHR ns;
    VK_CHECK(vkCreateSwapchainKHR(dev, &ci, nullptr, &ns));
    destroySwapViews();
    if (swap) vkDestroySwapchainKHR(dev, swap, nullptr);
    swap = ns;
    swapExt = ext;

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(dev, swap, &n, nullptr);
    swapImgs.resize(n);
    vkGetSwapchainImagesKHR(dev, swap, &n, swapImgs.data());

    if (!compPass) createStatic();   // needs swapFmt

    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (auto img : swapImgs) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = swapFmt;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v;
        VK_CHECK(vkCreateImageView(dev, &vi, nullptr, &v));
        swapViews.push_back(v);
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = compPass;
        fi.attachmentCount = 1;
        fi.pAttachments = &v;
        fi.width = ext.width;
        fi.height = ext.height;
        fi.layers = 1;
        VkFramebuffer fb;
        VK_CHECK(vkCreateFramebuffer(dev, &fi, nullptr, &fb));
        swapFbs.push_back(fb);
        VkSemaphore s;
        VK_CHECK(vkCreateSemaphore(dev, &si, nullptr, &s));
        renderDone.push_back(s);
    }
    minimized = false;
}

void App::destroyHDR() {
    if (hdrFb) vkDestroyFramebuffer(dev, hdrFb, nullptr);
    if (hdrView0) vkDestroyImageView(dev, hdrView0, nullptr);
    if (hdrViewAll) vkDestroyImageView(dev, hdrViewAll, nullptr);
    if (hdrImg) vkDestroyImage(dev, hdrImg, nullptr);
    if (hdrMem) vkFreeMemory(dev, hdrMem, nullptr);
    hdrFb = VK_NULL_HANDLE;
    hdrView0 = hdrViewAll = VK_NULL_HANDLE;
    hdrImg = VK_NULL_HANDLE;
    hdrMem = VK_NULL_HANDLE;
}

void App::createHDR() {
    destroyHDR();
    hdrExt = {std::max(1u, (uint32_t)std::lround(swapExt.width * scale)),
              std::max(1u, (uint32_t)std::lround(swapExt.height * scale))};
    uint32_t full = 1 + (uint32_t)std::floor(std::log2((double)std::max(hdrExt.width, hdrExt.height)));
    hdrMips = std::min(full, 8u);

    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = HDR_FMT;
    ii.extent = {hdrExt.width, hdrExt.height, 1};
    ii.mipLevels = hdrMips;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(dev, &ii, nullptr, &hdrImg));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, hdrImg, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(dev, &ai, nullptr, &hdrMem));
    VK_CHECK(vkBindImageMemory(dev, hdrImg, hdrMem, 0));

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = hdrImg;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = HDR_FMT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(dev, &vi, nullptr, &hdrView0));
    vi.subresourceRange.levelCount = hdrMips;
    VK_CHECK(vkCreateImageView(dev, &vi, nullptr, &hdrViewAll));

    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = scenePass;
    fi.attachmentCount = 1;
    fi.pAttachments = &hdrView0;
    fi.width = hdrExt.width;
    fi.height = hdrExt.height;
    fi.layers = 1;
    VK_CHECK(vkCreateFramebuffer(dev, &fi, nullptr, &hdrFb));

    VkDescriptorImageInfo dii{sampler, hdrViewAll, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = dset;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &dii;
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
}

void App::ensureReadback() {
    VkDeviceSize need = VkDeviceSize(swapExt.width) * swapExt.height * 4;
    if (rbBuf && rbSize >= need) return;
    if (rbBuf) {
        vkDestroyBuffer(dev, rbBuf, nullptr);
        vkFreeMemory(dev, rbMem, nullptr);
    }
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = need;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VK_CHECK(vkCreateBuffer(dev, &bi, nullptr, &rbBuf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, rbBuf, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex =
        memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(dev, &ai, nullptr, &rbMem));
    VK_CHECK(vkBindBufferMemory(dev, rbBuf, rbMem, 0));
    rbSize = need;
}

void App::saveShot(const fs::path& path) {
    uint32_t w = swapExt.width, h = swapExt.height;
    void* p = nullptr;
    VK_CHECK(vkMapMemory(dev, rbMem, 0, VkDeviceSize(w) * h * 4, 0, &p));
    const uint8_t* src = (const uint8_t*)p;
    bool bgr = swapFmt == VK_FORMAT_B8G8R8A8_SRGB || swapFmt == VK_FORMAT_B8G8R8A8_UNORM;
    std::vector<uint8_t> rgb(size_t(w) * h * 3);
    for (size_t i = 0; i < size_t(w) * h; i++) {
        rgb[i * 3 + 0] = src[i * 4 + (bgr ? 2 : 0)];
        rgb[i * 3 + 1] = src[i * 4 + 1];
        rgb[i * 3 + 2] = src[i * 4 + (bgr ? 0 : 2)];
    }
    vkUnmapMemory(dev, rbMem);
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
    if (writePNG(path, w, h, rgb)) logf("saved " + path.string());
    else logf("failed to write " + path.string());
}

// ------------------------------------------------------------------------------ frame
static void imgBarrier(VkCommandBuffer cb, VkImage img, uint32_t baseMip, uint32_t mips, VkImageLayout from,
                       VkImageLayout to, VkAccessFlags srcA, VkAccessFlags dstA, VkPipelineStageFlags srcS,
                       VkPipelineStageFlags dstS) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = srcA;
    b.dstAccessMask = dstA;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mips, 0, 1};
    vkCmdPipelineBarrier(cb, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
}

static void setViewport(VkCommandBuffer cb, VkExtent2D e) {
    VkViewport vp{0, 0, (float)e.width, (float)e.height, 0, 1};
    VkRect2D sc{{0, 0}, e};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
}

void App::record(VkCommandBuffer cb, uint32_t img, const PushScene& ps, const PushComp& pcmp, bool shot) {
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cb, &bi));

    // 1. scene -> HDR mip 0
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = scenePass;
    rb.framebuffer = hdrFb;
    rb.renderArea = {{0, 0}, hdrExt};
    vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    setViewport(cb, hdrExt);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, scenePipe);
    vkCmdPushConstants(cb, sceneLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ps), &ps);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);

    // 2. mip chain for bloom
    imgBarrier(cb, hdrImg, 0, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    if (hdrMips > 1)
        imgBarrier(cb, hdrImg, 1, hdrMips - 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);
    int32_t w = (int32_t)hdrExt.width, h = (int32_t)hdrExt.height;
    for (uint32_t i = 1; i < hdrMips; i++) {
        int32_t nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        VkImageBlit bl{};
        bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1};
        bl.srcOffsets[1] = {w, h, 1};
        bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
        bl.dstOffsets[1] = {nw, nh, 1};
        vkCmdBlitImage(cb, hdrImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hdrImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &bl, VK_FILTER_LINEAR);
        imgBarrier(cb, hdrImg, i, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);
        w = nw;
        h = nh;
    }
    imgBarrier(cb, hdrImg, 0, hdrMips, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    // 3. composite -> swapchain
    rb.renderPass = compPass;
    rb.framebuffer = swapFbs[img];
    rb.renderArea = {{0, 0}, swapExt};
    vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    setViewport(cb, swapExt);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, compPipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, compLayout, 0, 1, &dset, 0, nullptr);
    vkCmdPushConstants(cb, compLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pcmp), &pcmp);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);

    if (shot) {
        imgBarrier(cb, swapImgs[img], 0, 1, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {swapExt.width, swapExt.height, 1};
        vkCmdCopyImageToBuffer(cb, swapImgs[img], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbBuf, 1, &c);
        imgBarrier(cb, swapImgs[img], 0, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                   VK_ACCESS_TRANSFER_READ_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb.buffer = rbBuf;
        bb.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &bb, 0,
                             nullptr);
    }
    VK_CHECK(vkEndCommandBuffer(cb));
}

// returns true if a shot was captured this frame
bool App::drawFrame(const PushScene& ps, const PushComp& pcmp, bool wantShot) {
    VK_CHECK(vkWaitForFences(dev, 1, &fence[frame], VK_TRUE, UINT64_MAX));
    uint32_t img = 0;
    VkResult r = vkAcquireNextImageKHR(dev, swap, UINT64_MAX, imgAvail[frame], VK_NULL_HANDLE, &img);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        swapDirty = true;
        return false;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) VK_CHECK(r);
    VK_CHECK(vkResetFences(dev, 1, &fence[frame]));

    bool shot = wantShot && swapTransferSrc;
    if (shot) ensureReadback();
    VK_CHECK(vkResetCommandBuffer(cmd[frame], 0));
    record(cmd[frame], img, ps, pcmp, shot);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &imgAvail[frame];
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd[frame];
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &renderDone[img];
    VK_CHECK(vkQueueSubmit(queue, 1, &si, fence[frame]));

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderDone[img];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swap;
    pi.pImageIndices = &img;
    r = vkQueuePresentKHR(queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) swapDirty = true;
    else if (r != VK_SUCCESS) VK_CHECK(r);

    if (shot) VK_CHECK(vkWaitForFences(dev, 1, &fence[frame], VK_TRUE, UINT64_MAX));
    frame = (frame + 1) % FRAMES;
    return shot;
}

int App::run() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (testMode()) vsync = false;
    createWindow();
    createInstance();
    pickDevice();
    createDevice();
    createSwapchain();
    if (minimized) throw std::runtime_error("window has no area");
    createHDR();
    swapDirty = false;

    LARGE_INTEGER freq, prev, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);
    double animTime = 0.0, fpsTimer = 0.0;
    int fpsFrames = 0, framesDone = 0;
    double benchAccum = 0.0;
    int benchCount = 0;

    while (!quit) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) quit = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;
        if (minimized) {
            WaitMessage();
            QueryPerformanceCounter(&prev);
            if (!minimized) swapDirty = true;
            continue;
        }
        if (swapDirty) {
            vkDeviceWaitIdle(dev);
            createSwapchain();
            if (minimized) continue;
            hdrDirty = true;
            swapDirty = false;
        }
        if (hdrDirty) {
            vkDeviceWaitIdle(dev);
            createHDR();
            hdrDirty = false;
        }

        QueryPerformanceCounter(&now);
        double dt = double(now.QuadPart - prev.QuadPart) / double(freq.QuadPart);
        prev = now;
        if (!paused) animTime += std::min(dt, 0.1);
        float t = testMode() ? opt.shotTime : (float)animTime;

        float yaw = 0.55f + t * 0.012f + yawOff;
        float pitch = 0.11f + 0.045f * std::sin(t * 0.041f) + pitchOff;
        if (opt.hasYaw) yaw = opt.yaw;
        if (opt.hasPitch) pitch = opt.pitch;
        pitch = std::clamp(pitch, -1.45f, 1.45f);
        pitchOff = pitch - (0.11f + 0.045f * std::sin(t * 0.041f));

        PushScene ps{(float)hdrExt.width, (float)hdrExt.height, t, 0.80f, yaw, pitch, dist, 0.07f};
        PushComp pc{(float)swapExt.width, (float)swapExt.height, t, exposure};

        bool lastTestFrame = testMode() && framesDone + 1 >= opt.frames;
        bool want = shotRequested || lastTestFrame;
        auto f0 = std::chrono::steady_clock::now();
        bool captured = drawFrame(ps, pc, want);
        if (testMode()) {
            vkDeviceWaitIdle(dev);
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - f0).count();
            if (framesDone > 0) {
                benchAccum += ms;
                benchCount++;
            }
        }
        if (captured) {
            fs::path out;
            if (testMode()) out = opt.shot;
            else {
                SYSTEMTIME st;
                GetLocalTime(&st);
                out = exeDir() / "screenshots" /
                      std::format("blackhole_{:04}{:02}{:02}_{:02}{:02}{:02}.png", st.wYear, st.wMonth, st.wDay,
                                  st.wHour, st.wMinute, st.wSecond);
            }
            saveShot(out);
            shotRequested = false;
        }
        framesDone++;
        if (lastTestFrame && captured) quit = true;
        if (testMode() && framesDone > opt.frames + 10) {
            logf("capture failed");
            quit = true;
        }

        fpsTimer += dt;
        fpsFrames++;
        if (fpsTimer > 0.5 && !testMode()) {
            std::wstring title = std::format(L"Black Hole  |  {:.0f} fps  |  {}x{} @ {:.0f}%  |  {}", fpsFrames / fpsTimer,
                                             swapExt.width, swapExt.height, scale * 100.0f,
                                             vsync ? L"vsync" : L"uncapped");
            SetWindowTextW(hwnd, title.c_str());
            fpsTimer = 0;
            fpsFrames = 0;
        }
    }
    if (testMode() && benchCount > 0)
        logf(std::format("avg frame {:.2f} ms over {} frames at {}x{} (scene {}x{})", benchAccum / benchCount,
                         benchCount, swapExt.width, swapExt.height, hdrExt.width, hdrExt.height));
    if (dev) vkDeviceWaitIdle(dev);
    return g_validationErrors ? 2 : 0;
}

void App::destroy() {
    if (dev) {
        vkDeviceWaitIdle(dev);
        if (rbBuf) vkDestroyBuffer(dev, rbBuf, nullptr);
        if (rbMem) vkFreeMemory(dev, rbMem, nullptr);
        destroyHDR();
        destroySwapViews();
        if (swap) vkDestroySwapchainKHR(dev, swap, nullptr);
        if (scenePipe) vkDestroyPipeline(dev, scenePipe, nullptr);
        if (compPipe) vkDestroyPipeline(dev, compPipe, nullptr);
        if (sceneLayout) vkDestroyPipelineLayout(dev, sceneLayout, nullptr);
        if (compLayout) vkDestroyPipelineLayout(dev, compLayout, nullptr);
        if (scenePass) vkDestroyRenderPass(dev, scenePass, nullptr);
        if (compPass) vkDestroyRenderPass(dev, compPass, nullptr);
        if (dpool) vkDestroyDescriptorPool(dev, dpool, nullptr);
        if (dsl) vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
        if (sampler) vkDestroySampler(dev, sampler, nullptr);
        for (auto m : {vs, fsScene, fsComp})
            if (m) vkDestroyShaderModule(dev, m, nullptr);
        for (int i = 0; i < FRAMES; i++) {
            if (imgAvail[i]) vkDestroySemaphore(dev, imgAvail[i], nullptr);
            if (fence[i]) vkDestroyFence(dev, fence[i], nullptr);
        }
        if (cmdPool) vkDestroyCommandPool(dev, cmdPool, nullptr);
        vkDestroyDevice(dev, nullptr);
        dev = VK_NULL_HANDLE;
    }
    if (inst) {
        if (surface) vkDestroySurfaceKHR(inst, surface, nullptr);
        if (dbg) {
            auto fn =
                (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(inst, "vkDestroyDebugUtilsMessengerEXT");
            if (fn) fn(inst, dbg, nullptr);
        }
        vkDestroyInstance(inst, nullptr);
        inst = VK_NULL_HANDLE;
    }
    if (hwnd) {
        DestroyWindow(hwnd);
        hwnd = nullptr;
    }
}

// ------------------------------------------------------------------------------ entry
int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
            return argv[++i];
        };
        try {
            if (a == "--validate") opt.validate = true;
            else if (a == "--shot") opt.shot = next();
            else if (a == "--time") opt.shotTime = std::stof(next());
            else if (a == "--frames") opt.frames = std::max(1, std::stoi(next()));
            else if (a == "--yaw") { opt.yaw = std::stof(next()); opt.hasYaw = true; }
            else if (a == "--pitch") { opt.pitch = std::stof(next()); opt.hasPitch = true; }
            else if (a == "--dist") opt.dist = std::stof(next());
            else if (a == "--scale") opt.scale = std::clamp(std::stof(next()), 0.25f, 2.0f);
            else if (a == "--size") {
                auto s = next();
                auto x = s.find('x');
                opt.w = (uint32_t)std::stoul(s.substr(0, x));
                opt.h = (uint32_t)std::stoul(s.substr(x + 1));
            }
        } catch (const std::exception& e) {
            fprintf(stderr, "bad argument %s: %s\n", a.c_str(), e.what());
            return 1;
        }
    }
    g_log = _wfopen((exeDir() / "blackhole.log").c_str(), L"w");
    int rc = 0;
    try {
        App app(opt);
        rc = app.run();
    } catch (const std::exception& e) {
        logf(std::string("fatal: ") + e.what());
        if (opt.shot.empty()) MessageBoxA(nullptr, e.what(), "Black Hole", MB_ICONERROR);
        rc = 1;
    }
    if (g_log) fclose(g_log);
    return rc;
}
