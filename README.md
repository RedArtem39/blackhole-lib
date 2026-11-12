# blackhole-lib

Чёрная дыра в реальном времени: луч гнётся гравитацией и проходит сквозь объёмный аккреционный диск.
Всё процедурное, без текстур. Vulkan, C API + C++ обёртка. Шейдеры вшиты в библиотеку.

## Сборка

```
C:\Users\User\Desktop\BlackHole\build.bat
```

Нужны Vulkan SDK (`VULKAN_SDK`) и MSVC. В `build\` появятся:

| файл | что это |
|---|---|
| `blackhole_static.lib` | статическая библиотека (линковать ещё `vulkan-1.lib user32.lib`) |
| `blackhole.dll` + `blackhole.lib` | DLL и import lib (`#define BH_USE_DLL` перед include) |
| `viewer.exe` | окно с управлением (C++) |
| `wallpaper.exe` | рендер обоев в PNG, `--set` ставит на рабочий стол (C) |
| `gallery.exe` | все пресеты с нескольких ракурсов (C, через DLL) |

Заголовки: `include/blackhole.h` (C) и `include/blackhole.hpp` (C++).

## C

```c
#include "blackhole.h"

bh_renderer* r;
bh_renderer_create(NULL, &r);

bh_params p;
bh_params_preset(&p, BH_PRESET_FROST);      /* или bh_params_default(&p) */
p.camera_pitch   = 0.3f;
p.ring_color     = (bh_color){1.0f, 0.5f, 0.2f};
p.ring_intensity = 1.5f;
p.render_scale   = 1.5f;                     /* суперсэмплинг для обоев */

bh_render_png(r, &p, 3840, 2160, "wall.png");
/* или в память: bh_render_rgba8(r, &p, w, h, pixels, 0); */

bh_renderer_destroy(r);
```

## C++

```cpp
#include "blackhole.hpp"

bh::Renderer renderer;
bh::Params p(bh::Preset::Crimson);
p.disk_rotation_speed = 2.0f;
auto pixels = renderer.render(p, 1920, 1080);        // std::vector<uint8_t>, RGBA8

bh::WindowTarget target(renderer, hwnd);             // в любое окно / дочернее окно GUI
while (running) { p.time += dt; target.present(p); }
```

Плавный переход между видами: `bh::Params::lerp(a, b, t)` / `bh_params_lerp`.

## Параметры (`bh_params`)

Сначала `bh_params_default` или `bh_params_preset`, потом меняй что нужно.

- **анимация:** `time` (секунды, двигаешь сам)
- **камера:** `camera_yaw`, `camera_pitch`, `camera_distance`, `camera_roll`, `fov_degrees`, `center_x/center_y` (сдвиг дыры по экрану)
- **гравитация:** `lensing` (0 = свет не гнётся)
- **диск:** `disk_inner_radius`, `disk_outer_radius`, `disk_thickness`, `disk_brightness`, `disk_opacity`, `disk_rotation_speed` (минус = в другую сторону), `disk_flow_period`, `disk_detail`, `disk_streaks`, `disk_temperature`, `doppler_strength`, `redshift_strength`
- **цвета диска** (от холодного к горячему): `disk_color_edge`, `disk_color_warm`, `disk_color_hot`, `disk_color_core`, `disk_color_shift` (подсветка набегающей стороны)
- **фотонное кольцо:** `ring_color`, `ring_intensity` (0 = выкл), `ring_width`, `ring_radius`
- **фон:** `transparent_background` (альфа, premultiplied), `background_color`, `star_density`, `star_brightness`, `milky_way`, `nebula_tint`, `sky_rotation`, `seed`
- **пост:** `exposure`, `bloom_strength`, `bloom_radius`, `vignette`, `grain`, `chromatic_aberration`, `saturation`, `contrast`
- **качество:** `render_scale` (0.25–4), `max_steps`, `step_scale`

Пресеты: `gargantua`, `frost`, `crimson`, `monochrome`, `void`, `quasar`.

## Ошибки

Функции возвращают `bh_result` (`BH_OK = 0`, `BH_SKIPPED = 1`, ошибки < 0), текст в `bh_get_last_error()`.
В C++ ошибки летят как `bh::Error`. Валидация Vulkan: `bh_renderer_desc.enable_validation = 1`
(в примерах — переменная окружения `BH_VALIDATE=1`).

Один `bh_renderer` и всё, что от него создано, используется из одного потока.
