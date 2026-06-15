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
