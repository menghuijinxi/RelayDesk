# 构建与工具链问题复盘

## RelayDesk 静态链接构建

- 现象：RelayDesk 目标发布时应尽量避免在应用目录携带 GLFW、C++ 运行时、Asio、JSON 等第三方 DLL。
- 影响范围：`relaydesk` 可执行文件、EUI-NEO 集成、vcpkg 或源码子目录引入的第三方依赖，以及 Windows 发布包。
- 根因：项目计划要求依赖优先静态链接；如果 CMake preset、运行库选项或第三方库构建方式不一致，容易生成依赖应用私有 DLL 的输出。
- 排查路径：先查看 `CMakeLists.txt` 和 `CMakePresets.json`，确认 `CMAKE_CXX_STANDARD`、`BUILD_SHARED_LIBS`、`CMAKE_MSVC_RUNTIME_LIBRARY`、输出目录和第三方依赖引入方式。
- 最终解决方案：CMake 骨架建立后，优先使用项目定义的 RelayDesk preset 构建和验证，不复用其他项目迁移来的输出目录或旧 preset。
- 验证方式：构建 `relaydesk` 目标后，检查发布目录内容，并使用 `dumpbin /dependents` 或等价工具确认依赖列表。
- 可复用经验：构建验证必须和 RelayDesk 实际发布配置一致，否则 DLL 缺失或依赖 DLL 泄漏容易被误判为代码问题。
- 当前状态：仓库中尚未提交 `CMakeLists.txt` 和 `CMakePresets.json`；具体 preset 名称、输出目录和命令以后以实际配置为准。
- 待研究：EUI-NEO、Vulkan/OpenGL 后端、Asio、nlohmann/json、SHA-256 和 UUID 方案的实际引入方式，以及它们在 Windows 静态链接下的发布依赖。

快速检查清单：

1. 先运行 `cmake --list-presets` 确认可用 preset；如果还没有 preset，先检查项目是否已完成 CMake 骨架。
2. Debug 和 Release 都应使用 RelayDesk 自己的 preset，不复用其他项目的 `binaryDir`、vcpkg triplet 或缓存目录。
3. MSVC 构建重点确认 `/MT` 或 `/MTd`，对应 CMake 配置为 `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>`。
4. 发布前检查输出目录，不允许携带第三方应用私有 DLL；Windows 系统 DLL 和 Vulkan Runtime 依赖按项目计划单独处理。
