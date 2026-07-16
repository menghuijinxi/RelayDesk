# 图片预览运行时问题复盘

## 现象

RelayDesk 在部分 Windows 电脑上图片卡片或图片预览弹层无法显示图片，但同一个包在开发机上可显示。

## 影响范围

- 聊天图片附件、表情包图片、图片预览弹层等所有走 EUI `ui.image().path(...)` 的路径。
- 用户目录、工作目录、图片文件名包含中文或其他非 ASCII 字符的电脑更容易触发。
- `.webp` 还额外依赖 Windows WIC 是否能生成 PNG 缩略图。

## 根因

图片发送前的缩略图由 RelayDesk 通过 Windows WIC 生成；真正显示时，EUI 使用 `stb_image` 从路径加载图片。旧实现有两个问题：

1. EUI 在 Windows 下用窄字符路径和 `GetModuleFileNameA` 解析本地图片路径。
2. `stb_image` 未启用 `STBI_WINDOWS_UTF8`，会用窄字符 `fopen` 打开图片。

当路径是 UTF-8，但目标电脑的系统 ANSI 代码页不能表示这些字符时，文件存在也会被当成打不开，表现为图片预览空白。

## 排查路径

1. `dumpbin /dependents relaydesk.exe` 确认没有遗漏第三方 DLL；当前只依赖系统 DLL，如 `SHELL32.dll`、`OPENGL32.dll`、`WS2_32.dll` 等。
2. 查 `src/main/app.cpp`，确认预览只把 `previewPath` 交给 `ui.image().path(...)`。
3. 查 EUI `core/render/image_source.cpp`，确认本地图片最终由 `stbi_load(resolvedPath.c_str(), ...)` 加载。
4. 查 EUI `3rd/stb_image.h`，确认只有定义 `STBI_WINDOWS_UTF8` 时，Windows 才会用 `_wfopen` 打开 UTF-8 路径。

## 解决方案

通过 FetchContent patch 修补 EUI：

- `cmake/PatchEuiNeoWindowsImagePaths.cmake` 给 `stb_image_impl.cpp` 注入 `STBI_WINDOWS_UTF8`。
- 同一 patch 将 EUI 图片路径解析改为 Windows UTF-16 路径，并在返回给 `stb_image` 前转回 UTF-8。
- `cmake/PatchEuiNeoSkia.cmake` 接入该 patch，保证 Skia backend 构建时自动应用。

## 验证方式

- 使用 VS18/VC145 环境构建 `windows-msvc-nmake-debug`。
- 使用 VS18 自带 CMake 构建 `windows-msvc-release`。
- 运行 `ctest --preset windows-msvc-nmake-debug --output-on-failure`。
- 再次运行 `dumpbin /dependents`，确认仍无第三方 DLL 发布遗漏。

## 可复用检查清单

1. 构建 Skia 时，CMakeCache、vcpkg 静态库、`cl/link` 必须使用同一 MSVC 工具集。
2. 如果出现 `__std_*` 未解析符号，优先检查是否用 VS2022/VC143 链接了 VS18/VC145 构建出的 Skia/Boost 静态库。
3. 图片路径相关问题先确认路径是否包含中文、空格、emoji 或非当前系统代码页字符。
4. `.webp` 不能只看扩展名；如果 WIC 不能生成 PNG 缩略图，而底层解码器也不支持 WebP，就应降级为普通文件或引入明确的 WebP 解码方案。

## 2026-06-16 SVG 文件图标空白

### 现象

文件传输卡片已预留左侧文件类型图标位置，但运行在
`out/build/啊啊啊/relaydesk.exe` 时，`.exe`、`.webm`、`.txt` 的
`vscode-icons` SVG 图标全部空白，fallback 文本图标也没有显示。

### 影响范围

- 走 `ui.image().path(...)` 加载 SVG 文件路径的 UI。
- exe 所在路径或资源路径包含中文等非 ASCII 字符时更容易触发。

### 根因

RelayDesk 自己用 `std::filesystem::path` 能找到 SVG 资源，所以渲染函数判断
“图标资源可用”并跳过 fallback。但 EUI 的 SVG 文件加载最终在
`core/render/image_source.cpp` 里使用 `std::ifstream(path, std::ios::binary)`，
这个窄字符路径在 Windows 中文目录下会打开失败，最终表现为空白图标。

### 最终解决方案

RelayDesk 不再把 SVG 文件路径直接传给 `ui.image().path(...)`。改为：

- 用 `std::filesystem::path` 定位 `assets/third_party/vscode-icons/icons/*.svg`。
- 用 `std::ifstream(std::filesystem::path, ...)` 读取 SVG 文本，保证 Windows 中文路径可打开。
- 用 `ui.svg(...).markup(svgText)` 渲染 SVG 文本。
- 只有读到非空 SVG 文本时才认为图标绘制成功；否则继续走 fallback。

### 验证方式

- `cmake --build out/build/windows-msvc-nmake-release --target relaydesk --config Release`
- `ctest --test-dir out/build/windows-msvc-nmake-release --output-on-failure`

### 快速检查清单

1. EUI SVG 图标空白时，先看 exe 或资源目录是否含中文路径。
2. 不要用 `ui.image().path(...)` 直接加载项目内 SVG 图标文件；优先读成文本后走 `ui.svg().markup(...)`。
3. 如果资源定位成功但画面空白，不能直接返回成功，应保留可见 fallback。

## 2026-07-16 SkiaUI 聊天图片直接使用原图

### 现象与目标

SkiaUI 版曾把 `Image` 消息和普通文件一起渲染成传输卡片。恢复聊天图片后，
不能沿用低分辨率缩略图，否则在高 DPI 或大窗口中会明显模糊。

### 最终方案

- 根据消息的 `local_path` 解析工作目录相对路径，并确认原图可由 WIC 探测。
- 聊天图片和左键查看层的 `<img>` 都直接指向原图，不读取或生成缩略图。
- 原图无法访问或解码时才降级为原有文件卡片。
- 右键菜单复用平台层的图片剪贴板转换，并通过资源管理器定位原文件。

### SkiaUI 事件约束

SkiaUI 0.1.8 在右键释放后也会发送带 `MouseButton::Right` 的 `Click`。应用如果只判断
事件类型，会先打开右键菜单，随后又执行图片左键查看命令。所有点击命令必须同时要求
`ElementEventType::Click` 和 `MouseButton::Left`；右键菜单只处理右键 `MouseUp`。

### 验证方式

- `relaydesk_skiaui_image_message_capture` 生成测试原图并检查原图像素、左键查看和右键菜单。
- `relaydesk_skiaui_render_to_bgra_pixels_capture` 继续检查切换会话后的滚动位置。
- 使用真实历史记录验证 BMP 原图显示、查看层和图片右键菜单。
