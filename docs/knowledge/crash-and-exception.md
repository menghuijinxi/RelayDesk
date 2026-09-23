# 崩溃与异常诊断

## 分发版本崩溃后没有任何现场可查

### 现象

软件分发出去以后，用户只能反馈"闪退了"。开发侧拿不到调用栈、拿不到出错模块、
拿不到崩溃前应用在做什么，连进程退出码都可能被入口的 `catch (...)` 吞成 1，无法判断
是崩溃、是未捕获异常，还是正常退出。

### 影响范围

所有分发出去的 `relaydesk_skiaui` 版本。崩溃可能发生在主线程（UI、渲染），也可能发生在
工作线程（网络发现、异步刷新），后者连 `std::terminate` 都不会经过。

### 根因

修复前项目只有一条按需写入的文本日志（`data/logs/discovery.log`），并且存在四个缺口：

1. `/EHsc` 下 `catch (...)` **抓不到访问违例**。访问违例是结构化异常（SEH），不是
   C++ 异常；MSVC 默认的同步异常模型不会把它交给 `catch (...)`。
2. 没有任何转储能力，全项目搜不到 `MiniDumpWriteDump` / `SetUnhandledExceptionFilter`。
3. 工作线程上的结构化异常会直接杀掉线程，既不进 `std::terminate`，也不留任何痕迹。
4. Release 构建没有 `/Zi`，**根本不产出 PDB**，即使拿到裸地址也无法还原成函数名。

### 排查路径

1. 搜索 `SetUnhandledExceptionFilter`、`MiniDumpWriteDump`、`dbghelp`、`_set_se_translator`：
   全部为空，确认没有任何崩溃采集。
2. 检查入口的异常处理：`relaydesk_skiaui` 的 `wWinMain` 只有 `catch (...)` + `return 1`。
3. 检查编译选项：`CMAKE_CXX_FLAGS_RELEASE = /O2 /Ob2 /DNDEBUG`，没有 `/Zi`。
4. 检查构建输出目录：只有 `.exe`，没有 `.pdb`。

### 解决方案

新增 `src/platform/crash_report.h` + `src/platform/crash_report_win.cpp`，在 `wWinMain`
最早期（进入参数解析、单实例检查、UI 初始化之前）安装多层捕获：

| 入口 | 覆盖场景 |
| --- | --- |
| `AddVectoredExceptionHandler` | 任意线程上的致命结构化异常（访问违例、除零、栈溢出等） |
| `SetUnhandledExceptionFilter` | 未被上面接管的未处理结构化异常 |
| `std::set_terminate` | 未捕获的 C++ 异常 |
| `_set_purecall_handler` | 纯虚函数调用 |
| `_set_invalid_parameter_handler` / `_CrtSetReportHook2` | CRT 参数检查与断言 |
| `signal(SIGABRT/SIGFPE/SIGILL/SIGSEGV)` | C 运行时信号 |

一次崩溃的产物落在 `data/crashes/crash-<本地时间戳>/`：

- `crash.dmp` —— `MiniDumpWithFullMemory`，含所有线程栈与内存
- `report.txt` —— 人类可读摘要：版本、构建时间、异常码与地址、出错模块 + 偏移、
  寄存器、最近操作上下文、最近诊断日志、已加载模块列表
- `metadata.txt` —— 键值对形式，便于脚本解析与后续上报
- `logs/` —— 崩溃时刻 `data/logs` 的**快照**
- `symbolize.txt` —— 如何还原调用栈的操作说明

关键设计取舍：

- **转储必须最先写**。崩溃后进程状态还在，日志快照和文本报告都可以稍后补，但转储一旦
  错过时机就拿不到当时的线程与内存。
- **必须复制日志快照**。崩溃后程序通常会被立刻重启，`discovery.log` 随即被新内容覆盖；
  只有把日志一起拷进崩溃目录，事后才能还原用户现场。
- **崩溃路径只用预分配的固定内存**。全局路径缓冲区是固定 `char[]`，活动上下文是固定
  环形缓冲区，崩溃处理器里不做任何动态分配，避免分配失败演变成二次崩溃。
- **采集过程自身受 SEH 保护**。`MiniDumpWriteDump` 自己也可能触发异常，采集期间出现的
  二次异常必须忽略，否则一次崩溃会被反复重入。
- **环形保留最近 10 份**，避免磁盘无限增长。

配套改动：

- Release 加 `/Zi` + 链接 `/DEBUG:FULL`，分发版本从此保留 PDB。
- `cmake/ArchiveSymbols.cmake` 在每次构建后把 exe/pdb 归档到
  `out/symbols/v<版本>-<配置>/`，并写入 `manifest.txt`（含 exe 的 SHA256）。
  崩溃报告刻意不带 pdb（体积远超转储本身），排查时用归档按版本取回。
- `tools/symbolize-crash.ps1` 用归档里的 pdb 把转储符号化成 `symbolized.txt`。

### 上报方式

崩溃采集与上报只走互联网，不再实现任何局域网（设备间）回传逻辑：

- 崩溃采集**不在崩溃进程里上传**，只在崩溃路径里把现场落盘
  （`data/crashes/crash-<时间戳>/`）。网络操作绝不会塞进异常上下文。
- 转储、日志快照、`report.txt`/`metadata.txt`/`symbolize.txt` **全部写完之后**，崩溃
  处理器立刻用启动参数拉起同一个 exe 的崩溃处理模式；崩溃进程不等待子进程。
- 崩溃处理与聊天是**同一个 exe**，通过启动参数切换模式：
  `--relaydesk-crash-reporter` 进入崩溃处理模式，
  `--relaydesk-upload-crash-reports` 进入静默上传模式。
- 崩溃处理模式里弹窗询问用户是否上传：有桌面时弹原生对话框；没有桌面、无法创建
  窗口时自动回退到控制台菜单，同样由用户决定。用户选择"不上传"会标记该报告已处理，
  选择"取消/退出"则保留现场，下次可再处理。
- 用户确认上传后，崩溃处理进程再用启动参数拉起同一个 exe 的
  `--relaydesk-upload-crash-reports` 模式，并等待上传进程返回；只有上传成功才标记报告。
- 上传模式走 `relaydesk::platform::runCrashUploadMode`，递归扫描整个崩溃报告目录，
  排除上传完成标记 `uploaded.txt` 后，把 `crash.dmp`、`report.txt`、`metadata.txt`、
  `symbolize.txt` 以及 `logs/` 下的全部普通文件流式压缩成一个 `tar.gz`。压缩过程使用
  zlib，不会把完整转储读进内存。
- 客户端把这一个压缩包作为 `multipart/form-data` 的 `file` 字段发送到崩溃收集服务器
  （接口见 `MinidumpServer/docs/client-integration.md`），并使用
  `submission_id=<reportId>` 标识本次提交。归档内部保留崩溃目录的相对路径，例如
  `logs/discovery.log`；空文件也会保留。
- 上传地址默认是 `http://39.99.153.9:10019/`，`crash_upload_url` 配置或环境变量可以覆盖；
  只有压缩包上传成功后，才调用 `markCrashReportUploaded` 标记已处理。

这样收敛的好处：崩溃进程只关心"把现场写到本地并拉起处理程序"；弹窗询问、网络上传、
失败后重试全部留在独立的新进程里，崩溃路径既简单又不会因为网络调用二次崩溃。

### 验证方式

- `relaydesk_skiaui.exe --relaydesk-crash-test` 故意触发一次写空指针，确认
  `data/crashes/crash-*/` 下同时产出 70 MB 转储、报告、元数据、日志快照与说明文件。
- 报告里能读到 `exception_name=EXCEPTION_ACCESS_VIOLATION`、
  `access_violation_operation=write access_violation_address=0x0`、
  `failing_module=relaydesk_skiaui.exe` + `failing_module_offset=0x83D99`。
- `tools/symbolize-crash.ps1` 能自动匹配到 `out/symbols/v61-Release` 并打印降级指引
  （本机未安装 cdb）。
- 2026 年 9 月 23 日使用 `RELAYDESK_CRASH_UPLOAD_URL=http://127.0.0.1:10019/` 验证
  强制压缩上传逻辑：客户端先把同次崩溃的转储、报告和日志压成一个 `tar.gz`，再通过
  单文件 multipart 请求上传；服务端按 `submission_id` 保存该压缩包。
- 构建通过 VS 2026 的 `vcvars64.bat` + NMake `windows-msvc-nmake-release` 目标完成。

### 可复用经验

1. **`/EHsc` 不等于能捕获所有崩溃**。`catch (...)` 只处理 C++ 异常；访问违例属于 SEH，
   必须靠 `AddVectoredExceptionHandler` 或 `/EHa`（本项目未开启）才能拦到。
1. **只装 `SetUnhandledExceptionFilter` 是不够的**。它覆盖不到所有工作线程场景，
  向量化处理器才是覆盖面最广的入口，而且它必须能识别"哪些异常是致命的"，
  否则会把断点、单步这类正常控制流异常也当成崩溃。
1. **`WinHttpCrackUrl` 的组件指针需要调用方提供缓冲区**。只读取长度而不设置
   `lpszHostName` 等缓冲区时，后续得到的主机名为空，`WinHttpConnect` 会以错误 12005
   超时；上传端点必须使用真实解析出的主机名和路径。
3. **崩溃采集要有备份手段**。进程在进入我们的处理器之前就被系统终止时（栈严重损坏、
   堆已崩溃），只剩 Windows 错误报告的 LocalDumps 兜底；代码里已保留该开关
   （`CrashDumpType`/`WriteHelperDump`），默认关闭，排查极端情况时可临时打开。
4. **没有符号的转储几乎没用**。定位偏移量只是第一步，Release 必须产出并归档 PDB，
   而且 pdb 必须与 exe 严格来自同一次构建，版本串台会给出错误行号。

### 快速检查清单

- 崩溃目录下是否同时有 `crash.dmp`、`report.txt`、`logs/`。
- `report.txt` 里的 `failing_module` 是不是 `relaydesk_skiaui.exe`；如果是系统 dll，
  优先怀疑参数、句柄或生命周期错误。
- `access_violation_address` 是 `0x0` 还是接近 0 的小值 —— 通常是空指针或成员偏移访问。
- `logs/` 快照里崩溃时刻最后几条记录是什么操作。
- `out/symbols/` 里是否有与报告 `version` 匹配的 pdb；没有就先补归档再排查。
- 崩溃是否只发生在某个线程：看 `thread_id` 与报告中的模块列表。

## MSVC 不允许在含 C++ 析构对象的函数里使用 `__try`

### 现象

编译崩溃采集代码时报 `C2712: 无法在要求对象展开的函数中使用 __try`。

### 根因

MSVC 在同一函数内不能混用 SEH（`__try`/`__except`）与 C++ 对象自动展开。采集逻辑需要
`std::filesystem`、`std::string` 这些带析构的对象，直接被 `__try` 包住就会编译失败。

### 解决方案

把 `__try` 收敛到一个只含平凡可复制类型的 thunk，需要保护的逻辑以函数指针传入：

```cpp
struct ProtectedCall {
    void (*invoke)(void*) = nullptr;
    void* context = nullptr;
};

int runProtectedThunk(void* rawCall)
{
    const auto* call = static_cast<const ProtectedCall*>(rawCall);
    __try {
        call->invoke(call->context);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
    return 0;
}
```

受保护上下文用 `static_assert(std::is_trivially_copyable_v<Context>)` 约束，保证不会有人
往里塞带析构的类型。

### 可复用经验

需要"SEH 保护 + 正常使用标准库"时，不要把 `__try` 铺在业务函数里，而是做一个薄壳：
壳里只有 POD，真正的逻辑通过函数指针回调进去。

## 崩溃采集代码本身把更新助手模式弄崩了

### 现象

加入崩溃处理器后，`relaydesk_app_runtime_update_prompt_tests` 稳定失败：更新助手进程
不写日志、直接消失，退出码 3。禁用崩溃处理器后测试立刻通过。

### 根因

`wWinMain` 里 `installSkiaUiCrashHandler()` 被放在 `try` 块**外面**，而它内部会调用
`relaydesk::storage::createAppPaths()`。在测试沙箱等受限环境下，数据目录校验不通过时
该函数会抛异常；这个异常没有被任何 `catch` 接住，直接走到 `std::terminate`，
进程以退出码 3 结束，更新助手一个字都没写进日志。

也就是说：**一个纯粹的诊断辅助能力，变成了新的崩溃来源。**

### 解决方案

- `installSkiaUiCrashHandler()` 标记 `noexcept`，内部自行 `try/catch`；采集目录解析失败时
  只记录一条诊断信息，然后让程序照常启动。
- 该调用移入 `wWinMain` 的 `try` 块内，原因注释写清楚："安装失败时它自身不会抛出异常"。

### 可复用经验

1. 早期初始化里的**辅助能力**（日志、崩溃采集、遥测、更新检查）都必须自带异常边界，
   不能假设自己依赖的路径解析一定成功。
2. 判断"某次失败是不是这次改动造成的"，最有效的手段是**单点 A/B**：临时加一个环境变量
   开关关掉新功能，跑同一个测试对比。本次就是靠这个把范围从"整个改动"缩到"崩溃处理器"
   再到"`wWinMain` 里那一次调用"。
3. 空日志 + 退出码 3 是"未捕获 C++ 异常触发 `std::terminate`"的典型特征，值得记住。

## 中文注释的 .ps1 / .cmake 脚本必须带 UTF-8 BOM

### 现象

用工具写出的、内容是正确 UTF-8 但**没有 BOM** 的 `.ps1` 脚本，在 Windows PowerShell 5.1
下解析失败，报 `Missing expression after ','`、`The string is missing the terminator`，
并且回显的报错内容里中文全是乱码。

### 根因

Windows PowerShell 5.1 对没有 BOM 的脚本文件按**当前 ANSI 代码页**（本机为 GBK）解码，
而不是按 UTF-8。中文注释被按 GBK 解析后会吃掉引号或行尾，直接把语法结构破坏掉。

### 解决方案

脚本写完后统一转成 UTF-8 with BOM + LF：

```powershell
$bytes = [System.IO.File]::ReadAllBytes($path)
$text = [System.Text.Encoding]::UTF8.GetString($bytes) -replace "`r`n", "`n"
$utf8Bom = New-Object System.Text.UTF8Encoding($true)
[System.IO.File]::WriteAllText($path, $text, $utf8Bom)
```

### 可复用经验

1. `.ps1`、`.cmake` 这类会被外部解释器按"本机编码"读取的脚本，**必须带 BOM**；
   这与 `docs/cpp-code-standard.md` 里"脚本统一保存为 UTF-8 with BOM、LF 换行"的要求一致。
2. **不要用 `Set-Content` / `Get-Content -Raw` 去改写源码文件**。它们在 Windows PowerShell
   下按 ANSI 往返，会把 UTF-8 中文注释变成乱码，并且可能顺手把换行吞掉、把相邻两行拼成
   一行。改写源码请使用带编码参数的 .NET API 或专用编辑工具。
3. 排查脚本"语法正确却解析失败"时，第一件事是确认**文件编码与 BOM**，而不是怀疑语法。

## Windows 默认禁止运行 .ps1

### 现象

`tools/symbolize-crash.ps1` 直接执行时报
`cannot be loaded because running scripts is disabled on this system`。

### 解决方案

调用时显式绕过，只对本次调用生效，不修改本机设置：

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\symbolize-crash.ps1 -CrashDirectory <崩溃目录>
```

崩溃目录里的 `symbolize.txt` 已经写明这条命令，用户不需要再去查执行策略。
