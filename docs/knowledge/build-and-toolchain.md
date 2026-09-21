# 构建与工具链问题复盘

## RelayDesk SkiaUI 静态链接构建

- 正式程序目标只有 `relaydesk_skiaui`，不再构建或维护旧 UI 可执行文件。
- Windows 构建使用 Visual Studio 2026 的 MSVC 工具链和项目 CMake preset。
- 正式发布使用 `windows-msvc-nmake-release-skiaui`。
- vcpkg 目标 triplet 为 `x64-windows-static`，依赖目录为项目同级
  `../out/vcpkg/installed-static`。
- MSVC Release 使用 `/MT`，避免目标电脑依赖单独安装 VC++ 运行库。
- `BUILD_SHARED_LIBS=OFF`，发布前使用 `dumpbin /dependents` 检查是否意外引入应用私有 DLL。

## 正式构建

先进入 Visual Studio 2026 x64 开发环境，再执行：

```powershell
cmake --preset windows-msvc-nmake-release-skiaui
cmake --build --preset windows-msvc-nmake-release-skiaui --target relaydesk_skiaui
```

正式构建必须同时保留同一次构建产生的 EXE 和 PDB。EXE 用于分发，PDB 只归档在开发侧，
用于将客户设备产生的 DMP 还原为函数名和源码行号。

## 快速检查清单

1. `cmake --list-presets` 中存在 `windows-msvc-nmake-release-skiaui`。
2. `cl`、`link`、`nmake` 来自 Visual Studio 2026。
3. 编译参数使用 `/MT`，不能重新引入 `/MD`。
4. 发布文件是 `relaydesk_skiaui.exe`，不发布 PDB。
5. 符号归档中的 EXE、PDB 与发布 EXE 来自同一次构建。
6. `dumpbin /dependents` 不包含 `VCRUNTIME*.dll` 或其他应用私有 DLL。
