# 构建与工具链问题复盘

## RelayDesk 静态链接构建

- 现象：RelayDesk 目标发布时应尽量避免在应用目录携带 GLFW、C++ 运行时、Boost.Asio、JSON 等第三方 DLL。
- 影响范围：`relaydesk` 可执行文件、EUI-NEO 集成、vcpkg 或源码子目录引入的第三方依赖，以及 Windows 发布包。
- 根因：项目计划要求依赖优先静态链接；如果 CMake preset、运行库选项或第三方库构建方式不一致，容易生成依赖应用私有 DLL 的输出。
- 排查路径：先查看 `CMakeLists.txt` 和 `CMakePresets.json`，确认 `CMAKE_CXX_STANDARD`、`BUILD_SHARED_LIBS`、`CMAKE_MSVC_RUNTIME_LIBRARY`、输出目录和第三方依赖引入方式。
- 最终解决方案：CMake 骨架建立后，优先使用项目定义的 RelayDesk preset 构建和验证，不复用其他项目迁移来的输出目录或旧 preset。
- 验证方式：构建 `relaydesk` 目标后，检查发布目录内容，并使用 `dumpbin /dependents` 或等价工具确认依赖列表。
- 可复用经验：构建验证必须和 RelayDesk 实际发布配置一致，否则 DLL 缺失或依赖 DLL 泄漏容易被误判为代码问题。
- 当前状态：仓库已经有 MSVC/CMake 骨架；`windows-msvc-nmake-debug` preset 可在 VS 2022 Professional 的 `vcvars64.bat` 环境中完成配置、构建和测试。
- 当前 vcpkg 约定：CMake preset 使用项目同级 `../out/vcpkg`，不在 RelayDesk
  仓库内复制 vcpkg；目标 triplet 为 `x64-windows-static`。
- 待研究：EUI-NEO、Vulkan/OpenGL 后端、Boost.Asio、nlohmann/json、
  SHA-256 和 UUID 方案在 Windows 静态链接下的发布依赖。

快速检查清单：

1. 先运行 `cmake --list-presets` 确认可用 preset；如果还没有 preset，先检查项目是否已完成 CMake 骨架。
2. Debug 和 Release 都应使用 RelayDesk 自己的 preset，不复用其他项目的 `binaryDir`、vcpkg triplet 或缓存目录。
3. MSVC 构建重点确认 `/MT` 或 `/MTd`，对应 CMake 配置为 `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>`。
4. Boost.Asio 依赖通过 `vcpkg.json` 声明，由 `../out/vcpkg` 的 CMake
   toolchain 解析，不重新拉取 vcpkg。
5. 发布前检查输出目录，不允许携带第三方应用私有 DLL；Windows 系统 DLL 和 Vulkan Runtime 依赖按项目计划单独处理。

## 当前本机 MSVC 命令行验证

- 现象：使用 `Visual Studio 17 2022` 生成器配置时，CMake 在探测 `VCTargetsPath` 阶段失败。
- 影响范围：当前机器上的 VS 工程生成器配置流程；不影响已经加载 MSVC 环境后的 `cl` 和 `nmake`。
- 根因：MSBuild 计算 Windows SDK 目标版本时尝试访问 `C:\Users\wap80\AppData\Local\Microsoft SDKs`，当前环境返回访问拒绝。
- 排查路径：先确认 `cmake --preset windows-msvc-debug` 的错误发生在 `project()` 之前，再通过 `vcvars64.bat` 验证 `cl` 和 `nmake` 可用。
- 最终解决方案：当前命令行验证使用 `windows-msvc-nmake-debug` preset，并在命令前调用 VS 2022 Professional 的 `vcvars64.bat`。
- 验证方式：运行
  `cmd /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"" >nul && cmake --preset windows-msvc-nmake-debug && cmake --build --preset windows-msvc-nmake-debug && ctest --preset windows-msvc-nmake-debug"`。
- 可复用经验：不要因为 VS 生成器配置失败就切到 MinGW；本项目当前坚持 Windows/MSVC 路线，可以用 NMake preset 完成命令行验证。

快速检查清单：

1. 先通过 `vcvars64.bat` 进入 MSVC x64 环境。
2. 用 `where cl` 和 `where nmake` 确认当前 shell 没有落到非 MSVC 工具链。
3. 命令行构建优先使用 `windows-msvc-nmake-debug`。
4. VS 工程生成器的权限问题修复前，不要把 `windows-msvc-debug` 的失败误判为源码错误。
