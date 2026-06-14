# RelayDesk 项目计划

内部英文项目名：**RelayDesk**

## 1. 项目目标

RelayDesk 是一个面向内网环境的点对点桌面聊天工具。目标是在没有中心服务器的局域网内，自动发现正在运行本软件的用户，并支持文本、简单表情、文件、文件夹传输和本地聊天记录查看。

核心要求：

- 使用 C++23 和 CMake 开发。
- UI 使用 [sudoevolve/EUI-NEO](https://github.com/sudoevolve/EUI-NEO)。
- 聊天记录使用 JSONL 存储。
- 聊天记录、默认发送文件副本、默认接收文件和文件夹都存储在软件工作目录下。
- 默认显示当前电脑名作为用户名。
- 用户可以修改自己的显示名。
- 用户名不作为聊天记录关系 ID。
- 聊天关系按目标电脑的稳定设备 ID 保存，避免因为用户修改显示名导致历史记录断开。
- 项目依赖优先使用静态链接，发布时避免应用目录携带一组额外 DLL。

非目标：

- 不做公网聊天和跨 NAT 穿透。
- 不依赖中心服务器保存消息。
- MVP 不做移动端。
- MVP 不强制实现端到端加密，但协议设计预留安全层。

## 2. 技术选型

### 2.1 基础技术

- 语言：C++23。
- 构建：CMake。
- UI：EUI-NEO，通过 CMake `FetchContent` 拉取固定 commit 集成，不使用 git submodule。
- 窗口后端：优先使用 EUI-NEO 默认 GLFW 后端。
- 渲染后端：优先 Vulkan，OpenGL 仅作为兼容 fallback 或诊断后端。
- JSON：使用 vcpkg 提供的 `nlohmann-json`，便于 JSONL 读写和协议头序列化。
- 网络：使用 vcpkg 提供的 Boost.Asio，通过 CMake `find_package(Boost)` 接入。
- 哈希：SHA-256，用于文件完整性校验、附件去重和内容校验。
- UUID：UUIDv7 或随机 UUIDv4，用于消息 ID、传输任务 ID。

### 2.2 EUI-NEO 集成方式

RelayDesk 使用 `FetchContent` 集成 EUI-NEO：

- `FetchContent_Declare(eui_neo ...)`
- 使用 `${RELAYDESK_EUI_NEO_SOURCE_DIR}/core/app/glfw_app_main.cpp`
- 应用代码实现 `app::dslAppConfig()` 和 `app::compose()`
- 调用 `eui_neo_configure_app(target)`
- Skia 后端相关修改通过项目内 CMake patch 脚本应用到 FetchContent 构建副本，不直接修改第三方源码仓库。

计划中的 CMake 结构：

```cmake
cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

project(RelayDesk LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(BUILD_SHARED_LIBS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(eui_neo
    GIT_REPOSITORY "https://github.com/sudoevolve/EUI-NEO.git"
    GIT_TAG "<pinned-commit>"
)
FetchContent_MakeAvailable(eui_neo)
FetchContent_GetProperties(eui_neo)
set(RELAYDESK_EUI_NEO_SOURCE_DIR "${eui_neo_SOURCE_DIR}")

add_executable(relaydesk
    ${RELAYDESK_EUI_NEO_SOURCE_DIR}/core/app/glfw_app_main.cpp
    src/main/app.cpp
    src/ui/main_window.cpp
    src/net/discovery_service.cpp
    src/net/peer_session.cpp
    src/storage/history_store.cpp
    src/storage/identity_store.cpp
    src/transfer/transfer_manager.cpp
)

eui_neo_configure_app(relaydesk)
target_include_directories(relaydesk PRIVATE src)

target_link_options(relaydesk PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/INCREMENTAL:NO>
    $<$<CXX_COMPILER_ID:GNU>:-static>
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc>
    $<$<CXX_COMPILER_ID:GNU>:-static-libstdc++>
)
```

### 2.3 静态链接与分发策略

目标：发布目录尽量只有 `relaydesk.exe`、配置模板和必要资源文件，不随程序携带 GLFW、JSON、Asio、运行时库等第三方 DLL。

规则：

- 所有可控第三方库优先以 static library 方式构建。
- CMake 全局设置 `BUILD_SHARED_LIBS=OFF`。
- MSVC 使用 `/MT` 和 `/MTd`，避免依赖 Visual C++ Redistributable DLL。
- MinGW 构建时使用 `-static -static-libgcc -static-libstdc++`。
- 第三方依赖通过 vcpkg、FetchContent 或预编译静态库引入，不使用 git submodule，不使用只提供动态库的包。
- CI 和发布流程必须检查输出目录，不允许出现非系统依赖 DLL。

Windows 上仍然会依赖系统自带 DLL，例如 `kernel32.dll`、`user32.dll`、`ws2_32.dll` 等。Vulkan 也通常依赖目标机器显卡驱动或 Vulkan Runtime 提供的 `vulkan-1.dll`。项目目标是不随 RelayDesk 发布包携带一组应用私有 DLL；如果目标机器没有 Vulkan Runtime，则启动时提示安装显卡驱动/Vulkan Runtime，或自动切换到 OpenGL fallback。

建议增加发布检查脚本：

```text
tools/check_release_dependencies.ps1
```

检查内容：

- 发布目录是否只包含允许的 `.exe`、资源、配置和文档。
- 使用 `dumpbin /dependents` 或 `llvm-objdump -p` 检查 `relaydesk.exe` 依赖。
- 允许 Windows 系统 DLL 和 Vulkan Runtime。
- 禁止出现第三方应用私有 DLL，例如 `glfw3.dll`、`libstdc++-6.dll`、`libgcc_s_seh-1.dll`。

## 3. 项目目录规划

```text
RelayDesk/
  CMakeLists.txt
  CMakePresets.json
  cmake/
    eui-skia/
  src/
    main/
      app.cpp
    ui/
      main_window.h
      main_window.cpp
      chat_view.h
      chat_view.cpp
      peer_list_view.h
      peer_list_view.cpp
      settings_view.h
      settings_view.cpp
    net/
      protocol.h
      framed_socket.h
      discovery_service.h
      discovery_service.cpp
      peer_session.h
      peer_session.cpp
      peer_registry.h
      peer_registry.cpp
    storage/
      app_paths.h
      identity_store.h
      identity_store.cpp
      history_store.h
      history_store.cpp
      attachment_store.h
      attachment_store.cpp
    transfer/
      transfer_manifest.h
      transfer_manager.h
      transfer_manager.cpp
    platform/
      computer_name.h
      computer_name_win.cpp
      computer_name_posix.cpp
      file_dialog.h
      file_dialog_win.cpp
    core/
      event_bus.h
      app_state.h
      time.h
      uuid.h
  tests/
    storage_tests.cpp
    protocol_tests.cpp
    transfer_tests.cpp
  docs/
    protocol.md
    storage.md
```

## 4. 身份与用户名设计

### 4.0 软件工作目录

RelayDesk 默认使用软件工作目录保存所有可变数据。为避免 Windows 快捷方式或启动器改变 current working directory，软件工作目录定义为：

```text
work_dir = relaydesk.exe 所在目录
```

后续可以通过启动参数或设置项覆盖：

```text
relaydesk.exe --work-dir D:/RelayDeskWork
```

默认数据布局：

```text
<work_dir>/
  relaydesk.exe
  data/
    identity.json
    config.json
    logs/
    peers/
      <peer_device_id>/
        profile.json
        messages.jsonl
        attachments/
    stickers/
      favorites/
        manifest.json
        items/
      packs/
        <pack_id>/
          manifest.json
          items/
    transfers/
      inbox/
        <peer_device_id>/
          <transfer_id>/
      outbox/
        <peer_device_id>/
          <transfer_id>/
      temp/
        <transfer_id>/
```

规则：

- 聊天记录 JSONL 默认保存到 `<work_dir>/data/peers/<peer_device_id>/messages.jsonl`。
- 收藏表情默认保存到 `<work_dir>/data/stickers/favorites/`。
- 导入的表情包默认保存到 `<work_dir>/data/stickers/packs/<pack_id>/`。
- 接收的文件和文件夹默认保存到 `<work_dir>/data/transfers/inbox/<peer_device_id>/<transfer_id>/`。
- 发送的文件和文件夹默认先复制成发送快照，保存到 `<work_dir>/data/transfers/outbox/<peer_device_id>/<transfer_id>/`，再从该快照传输。
- 文件传输临时数据默认保存到 `<work_dir>/data/transfers/temp/<transfer_id>/`。
- 用户可以在设置中修改软件工作目录；修改后新数据写入新目录，旧数据通过迁移工具移动。
- 如果 `relaydesk.exe` 所在目录不可写，启动时提示用户选择一个可写的软件工作目录，不静默降级到系统目录。

### 4.1 本机身份

首次启动时生成本机身份文件：

默认路径：

```text
<work_dir>/data/identity.json
```

身份文件示例：

```json
{
  "schema_version": 1,
  "device_id": "01JZ9N0K7Y6YV6WM8AQZ9DPC6P",
  "install_id": "01JZ9N0K7Y5F2H9G6BJ9FH9T04",
  "created_at": "2026-06-11T14:00:00Z",
  "host_name": "DESKTOP-7K92P1",
  "display_name": "DESKTOP-7K92P1"
}
```

字段说明：

- `device_id`：本机稳定 ID，用作聊天关系、消息归属、传输归属的核心 ID。
- `install_id`：本次安装 ID，用于诊断，不作为聊天关系 ID。
- `host_name`：当前电脑名，启动时刷新。
- `display_name`：用户可修改的显示名，默认等于电脑名。

重要规则：

- 聊天记录路径、会话 ID、联系人 ID 只使用 `device_id`。
- `display_name` 只用于界面显示和历史消息快照。
- 用户改名后，旧消息仍能显示当时的名称快照，新消息显示新名称。
- 如果需要跨系统重装仍保持同一设备 ID，需要增加“导出/导入身份”功能，或者由管理员部署固定身份文件。MVP 先保证用户名变化不会影响历史关系。

### 4.2 对端身份

发现到对端后，保存对端资料：

```json
{
  "schema_version": 1,
  "device_id": "01JZ9N1EQJ3BNR1P7PMYD30KX4",
  "host_name": "DESKTOP-OFFICE-12",
  "display_name": "Alice-PC",
  "last_addresses": ["192.168.1.42"],
  "tcp_port": 39171,
  "capabilities": ["rich_message", "text", "emoji", "image", "file", "folder"],
  "first_seen_at": "2026-06-11T14:02:00Z",
  "last_seen_at": "2026-06-11T14:30:00Z"
}
```

本地联系人索引：

```text
<work_dir>/
  data/
    peers/
      01JZ9N1EQJ3BNR1P7PMYD30KX4/
        profile.json
        messages.jsonl
        attachments/
```

## 5. 聊天记录 JSONL 设计

每行是一条独立记录。追加写入，便于恢复和增量索引。

核心规则：

- 一条用户发送的聊天消息只写成一条 `message` 记录。
- 一条消息可以包含多个有序内容块，字段名为 `parts`。
- `parts` 按用户发送时的组合顺序保存，UI 按顺序渲染。
- 文本、表情、图片、文件、文件夹可以混在同一条消息里。
- 图片、文件和文件夹的二进制内容不写入 JSONL，只写元数据、传输 ID 和软件工作目录内的相对路径。
- 新写入统一使用 `schema_version = 2`。
- 旧版 `schema_version = 1` 的单 `content_type` 记录仍可兼容读取，加载时转换成只有一个 `part` 的消息；不主动重写旧历史。

混合消息示例：

```json
{
  "schema_version": 2,
  "record_type": "message",
  "message_id": "01JZ9Q4D4T8H9NFN5Z8R3N1WBZ",
  "conversation_id": "dm_01JZ9N0K7Y6YV6WM8AQZ9DPC6P_01JZ9N1EQJ3BNR1P7PMYD30KX4",
  "direction": "out",
  "sender_device_id": "01JZ9N0K7Y6YV6WM8AQZ9DPC6P",
  "receiver_device_id": "01JZ9N1EQJ3BNR1P7PMYD30KX4",
  "sender_display_name_snapshot": "Bob-PC",
  "receiver_display_name_snapshot": "Alice-PC",
  "created_at": "2026-06-11T14:05:12Z",
  "delivery_state": "sent",
  "parts": [
    {
      "part_id": "p1",
      "type": "text",
      "text": "这个版本你看一下"
    },
    {
      "part_id": "p2",
      "type": "emoji",
      "emoji": "thumbs_up"
    },
    {
      "part_id": "p3",
      "type": "image",
      "transfer_id": "01JZ9Q74ZAZT4V2Y3S43DSVDK3",
      "transfer_state": "completed",
      "file_name": "screenshot.png",
      "file_size": 283923,
      "sha256": "b7e23ec29af22b0b4e41da31e868d57226121c84d4d13150c10d33c37c4c7f74",
      "local_path": "data/transfers/outbox/01JZ9N1EQJ3BNR1P7PMYD30KX4/01JZ9Q74ZAZT4V2Y3S43DSVDK3/screenshot.png"
    },
    {
      "part_id": "p4",
      "type": "file",
      "transfer_id": "01JZ9Q82BY1EVXXDF1XG5S1V32",
      "transfer_state": "completed",
      "file_name": "report.pdf",
      "file_size": 2388102,
      "sha256": "9cfc7e22f38d3a9fd3d33e7b54854ef534fc5579d52d1aaf8c72e35d8f7a4a90",
      "local_path": "data/transfers/outbox/01JZ9N1EQJ3BNR1P7PMYD30KX4/01JZ9Q82BY1EVXXDF1XG5S1V32/report.pdf"
    },
    {
      "part_id": "p5",
      "type": "folder",
      "transfer_id": "01JZ9Q8WQA1S280Y2EB5VZ3J6P",
      "transfer_state": "completed",
      "file_name": "ProjectDocs",
      "local_path": "data/transfers/outbox/01JZ9N1EQJ3BNR1P7PMYD30KX4/01JZ9Q8WQA1S280Y2EB5VZ3J6P/ProjectDocs",
      "manifest_path": "data/transfers/outbox/01JZ9N1EQJ3BNR1P7PMYD30KX4/01JZ9Q8WQA1S280Y2EB5VZ3J6P/manifest.json"
    }
  ]
}
```

实际写入 JSONL 时仍序列化为单行 JSON，上面的格式仅用于说明。

字段规则：

- `message_id`：整条聊天消息的稳定 ID，所有回执、重试、传输状态更新都引用它。
- `part_id`：消息内内容块 ID，只需要在同一条消息内唯一。
- `type`：内容块类型，取值为 `text`、`emoji`、`image`、`file`、`folder`。
- `delivery_state`：整条消息外壳的投递状态，取值为 `pending`、`sent`、`delivered`、`received`、`failed`、`cancelled`。
- `transfer_state`：图片、文件、文件夹内容块的传输状态，取值为 `pending`、`offered`、`transferring`、`completed`、`failed`、`cancelled`。
- `local_path` 和 `manifest_path` 必须是相对 `<work_dir>` 的路径，禁止绝对路径和 `..` 路径穿越。
- `sha256` 用于文件完整性校验；文件夹整体可在 manifest 中记录每个文件的 SHA-256。

兼容读取规则：

- `schema_version = 1` 且存在 `content_type` 的旧记录，加载为一条 `schema_version = 2` 内存消息。
- 旧 `content_type = text` 转换为一个 `type = text` 的 `part`。
- 旧 `content_type = emoji` 转换为一个 `type = emoji` 的 `part`。
- 旧 `content_type = image/file/folder` 转换为一个对应类型的文件类 `part`，沿用旧 `transfer_id`、`file_name`、`file_size`、`sha256`、`local_path`、`manifest_path` 字段。
- 新代码不再写入顶层 `content_type`。

会话 ID 规则：

```text
conversation_id = "dm_" + min(local_device_id, peer_device_id) + "_" + max(local_device_id, peer_device_id)
```

这样无论消息方向如何，本机与同一台对端电脑的聊天都落在同一个关系里。

JSONL 读写策略：

- 写入时先序列化为单行 JSON，末尾加 `\n`。
- 单条记录追加失败时不能破坏已存在记录。
- 启动加载时遇到损坏行，记录日志并跳过，不中断整个历史加载。
- 同一条消息的内容组合在写入前确定，后续只通过状态更新事件或索引刷新改变投递/传输状态，不拆成多条用户消息。
- 为提高历史列表速度，可增加 `messages.index`，但 JSONL 是权威数据源。
- 后续 schema 变更使用 `schema_version` 做兼容迁移。

### 5.4 聊天内联预览

聊天区应按资源能力决定展示方式，而不是只对图片做临时特例。
只要内容块的资源类型已被客户端支持阅读或播放，就应在聊天消息内部直接预览。

规则：

- 图片内容块在传输完成且本地文件可读时，直接在消息气泡内显示缩略图。
- 暂不支持预览的文件继续显示为文件卡片，提供打开、另存、重试、取消等操作。
- 后续支持常见视频格式时，应在消息内部显示视频封面和播放控件，用户无需先打开外部播放器才能查看。
- 视频预览不进入当前 MVP；当前实现只要求图片内联预览。
- 内联预览必须仍然保留文件名、大小、传输状态和失败/重试状态，不允许因为预览成功而丢失文件传输语义。
- 预览能力应由内容类型和本地文件可读性决定；历史记录中的 `type`、`local_path`、`file_name`、`file_size`、`sha256` 仍是权威元数据。

当前已知缺口：

- 当前已经可以发送图片和普通文件，但图片消息在聊天窗口中还不能稳定显示可读预览画面，需要补齐多 part 图片消息和接收完成后的内联预览渲染。
- 图片拖入发送队列后目前只能看到缩略图，不能点击放大查看完整图片；待发附件队列需要增加图片查看器入口，点击缩略图后以弹窗或独立预览层显示原图。

### 5.5 表情收藏与表情包导入

收藏表情和导入表情包使用独立的表情资源目录，不直接引用聊天传输目录中的图片文件。
这样用户清理 `data/transfers/` 时不会破坏已经收藏或导入的表情。

收藏表情目录：

```text
<work_dir>/data/stickers/favorites/
  manifest.json
  items/
    <sticker_id>.<ext>
```

导入表情包目录：

```text
<work_dir>/data/stickers/packs/<pack_id>/
  manifest.json
  items/
    <sticker_id>.<ext>
```

`manifest.json` 使用 UTF-8 JSON，示例：

```json
{
  "schema_version": 1,
  "pack_id": "favorites",
  "name": "收藏表情",
  "import_format": "relaydesk_manifest",
  "items": [
    {
      "id": "cat",
      "name": "cat.png",
      "path": "data/stickers/favorites/items/cat.png"
    }
  ]
}
```

规则：

- 用户在聊天图片消息上右键选择“收藏为表情”时，软件把该图片复制到收藏表情 `items/` 目录，并写入收藏表情 `manifest.json`。
- 导入表情包时，软件把可用图片复制到 `data/stickers/packs/<pack_id>/items/`，导入完成后不再依赖原始导入目录。
- 选择自定义表情时，软件在消息编辑区展示待发送缩略图；发送时把这些表情写成 `type = image` 的消息内容块。
- `path` 必须是相对 `<work_dir>` 的路径，禁止绝对路径和 `..` 路径穿越。
- 支持的图片扩展名为 `.png`、`.jpg`、`.jpeg`、`.gif`、`.webp`。
- 表情包导入兼容 RelayDesk `manifest.json`、常见 `OwO.json` 本地图片结构，以及普通图片文件夹。
- `OwO.json` 中的远程 URL 和 `data:` 图片不在 MVP 内自动下载；导入器只处理本地图片路径。
- 同名表情复制时自动生成唯一文件名，不覆盖已经收藏或导入的表情文件。

## 6. 内网发现设计

目标：扫描并展示内网中所有正在运行 RelayDesk 的用户。

### 6.1 发现方式

优先级：

1. UDP 广播：向每个活动网卡所在子网发送 `DISCOVERY_QUERY`。
2. UDP 组播：加入固定组播地址，降低跨子网配置复杂度。
3. 被动监听：收到其他客户端广播时自动加入列表。
4. 可选主动扫描：当广播被网络策略屏蔽时，扫描本机网段内指定端口。

建议端口：

- UDP discovery：25581
- TCP peer session：39171

发现包示例：

```json
{
  "protocol": "relaydesk.discovery",
  "version": 1,
  "type": "hello",
  "device_id": "01JZ9N0K7Y6YV6WM8AQZ9DPC6P",
  "host_name": "DESKTOP-7K92P1",
  "display_name": "Bob-PC",
  "tcp_port": 39171,
  "capabilities": ["rich_message", "text", "emoji", "image", "file", "folder"],
  "timestamp": "2026-06-11T14:00:00Z"
}
```

刷新策略：

- 启动时立即广播 3 次，间隔 500ms。
- 在线状态每 15 秒广播一次心跳。
- 超过 45 秒未收到心跳，标记为离线。
- UI 保留离线联系人，便于查看历史记录。

### 6.2 去重与冲突处理

- 以 `device_id` 作为唯一键。
- 同一个 `device_id` 出现在多个 IP 时，保留最近可连通地址列表。
- 发现本机 `device_id` 时忽略。
- 如果发现两台机器使用相同 `device_id`，标记身份冲突，禁止自动连接，提示用户重新生成其中一台设备身份。

## 7. 通信协议设计

### 7.1 连接模型

- 每个客户端启动一个 TCP 监听端口。
- 发送消息时建立或复用到对端的 TCP 连接。
- UI 线程不直接做网络 IO，网络层运行在独立 `io_context` 线程。
- 所有网络事件通过线程安全队列投递到应用状态。

### 7.2 帧格式

建议二进制帧头 + JSON 头 + 可选二进制正文：

```text
magic      4 bytes   "RLDK"
version    2 bytes
type       2 bytes
flags      4 bytes
header_len 4 bytes
body_len   8 bytes
header     header_len bytes, UTF-8 JSON
body       body_len bytes
```

多字节整数统一使用网络字节序（big-endian）。

消息类型：

- `profile_hello`
- `profile_update`
- `chat_message`
- `chat_message_update`
- `delivery_receipt`
- `transfer_offer`
- `transfer_accept`
- `transfer_reject`
- `transfer_chunk`
- `transfer_complete`
- `transfer_cancel`
- `heartbeat`

`chat_message` 发送整条消息外壳和全部 `parts` 元数据。文本和表情内容直接放在 JSON 头内；图片、文件、文件夹内容块只放 `transfer_id`、文件名、大小、哈希和路径等元数据，实际二进制内容通过后续传输帧发送。

当一条 `chat_message` 包含图片、文件或文件夹时：

1. 发送端先为每个文件类 `part` 创建发送快照和 `transfer_id`。
2. 发送端发送一条 `chat_message`，其中每个文件类 `part` 都包含 `message_id` 内唯一的 `part_id` 和全局唯一的 `transfer_id`。
3. 发送端为每个文件类 `part` 发送 `transfer_offer`，`transfer_offer` 必须同时引用 `message_id`、`part_id` 和 `transfer_id`。
4. 传输进度、完成、失败和取消事件都引用同一组 ID，UI 在原消息内部更新对应内容块状态。

### 7.3 可靠性

- TCP 保证顺序传输，但应用层仍保存 `message_id` 去重。
- 发送消息先写本地 JSONL，状态为 `pending`。
- 对端确认后更新状态为 `delivered`。
- 文件类内容块传输完成并校验 SHA-256 后，将对应 `part.transfer_state` 更新为 `completed`。
- 断线后文件传输可按 chunk offset 续传。

## 8. 文件和文件夹传输设计

### 8.1 文件传输

流程：

1. 发送端在消息编辑区添加文件内容块。
2. 发送端把文件复制到 `<work_dir>/data/transfers/outbox/<peer_device_id>/<transfer_id>/`，形成发送快照。
3. 计算快照文件大小和 SHA-256，可边传边计算以避免大文件阻塞。
4. 发送包含该文件 `part` 的 `chat_message`。
5. 发送引用 `message_id`、`part_id` 和 `transfer_id` 的 `transfer_offer`。
6. 接收端在同一条消息内显示文件卡片，用户接受或拒绝。
7. 接受后发送端从 outbox 快照按 chunk 发送。
8. 接收端写入 `<work_dir>/data/transfers/temp/<transfer_id>/`。
9. 完成后校验 SHA-256。
10. 校验通过后移动到 `<work_dir>/data/transfers/inbox/<peer_device_id>/<transfer_id>/`，并更新 JSONL 中对应 `part` 的传输状态。

默认 chunk 大小：1 MiB。

### 8.2 文件夹传输

文件夹不建议直接打成一个临时大压缩包作为唯一方案，因为大文件夹失败后恢复成本高。MVP 使用 manifest 方式：

```json
{
  "schema_version": 1,
  "transfer_id": "01JZ9R4ZF4E47PCW9Y8XZPQBVZ",
  "type": "folder",
  "root_name": "ProjectDocs",
  "entries": [
    {
      "relative_path": "README.md",
      "type": "file",
      "size": 4210,
      "mtime": "2026-06-11T10:00:00Z",
      "sha256": "..."
    },
    {
      "relative_path": "images/logo.png",
      "type": "file",
      "size": 12991,
      "mtime": "2026-06-10T09:12:00Z",
      "sha256": "..."
    }
  ]
}
```

安全规则：

- `relative_path` 必须归一化。
- 禁止绝对路径。
- 禁止 `..` 路径穿越。
- Windows 保留设备名要转义或拒绝。
- 发送端先把文件夹复制为 `<work_dir>/data/transfers/outbox/<peer_device_id>/<transfer_id>/<root_name>/` 快照，再根据快照生成 manifest 和传输内容。
- 接收端所有内容先写入 `<work_dir>/data/transfers/temp/<transfer_id>/`，完成校验后再移动到 `<work_dir>/data/transfers/inbox/<peer_device_id>/<transfer_id>/`。

## 9. UI 设计

主界面布局：

- 左侧：在线/离线用户列表，显示头像占位、显示名、电脑名、在线状态、最后活跃时间。
- 中间：聊天记录区域，按时间展示消息；每条消息内部按 `parts` 顺序展示文本、表情、图片、文件和文件夹内容块。
- 底部：输入框、表情按钮、发送文件按钮、发送文件夹按钮、发送按钮。
- 右侧可选：当前会话详情和传输队列。

主要页面：

- 聊天页：默认首页。
- 历史页：按联系人、时间、关键词筛选 JSONL 记录。
- 传输页：查看进行中、已完成、失败的文件/文件夹传输。
- 设置页：修改显示名、查看设备 ID、导出/导入身份、修改软件工作目录、端口设置。

交互要求：

- 正在扫描时显示轻量状态，不阻塞聊天。
- 离线用户仍可点击查看历史。
- 发送失败的消息可以重试。
- 文件类内容块传输失败可以在原消息内重试或取消。
- 用户一次点击发送时，输入框文字、已选表情、图片、文件、文件夹组成同一条消息，不拆成多条消息。
- 用户名修改后立即广播 `profile_update`，其他客户端更新联系人列表。

## 10. 应用状态和线程模型

线程划分：

- UI 主线程：EUI-NEO 渲染和用户交互。
- 网络线程：Boost.Asio `io_context`，负责 discovery、TCP session、文件传输。
- 存储线程：顺序写 JSONL，避免 UI 卡顿。
- 哈希线程池：大文件 SHA-256 和文件夹 manifest 生成。

核心状态：

```cpp
struct PeerInfo {
    DeviceId device_id;
    std::string host_name;
    std::string display_name;
    std::vector<std::string> addresses;
    uint16_t tcp_port;
    bool online;
    TimePoint last_seen_at;
};

struct MessagePart {
    PartId part_id;
    MessagePartType type;
    std::optional<std::string> text;
    std::optional<std::string> emoji;
    std::optional<TransferId> transfer_id;
    std::optional<std::string> file_name;
    std::optional<uint64_t> file_size;
    std::optional<std::string> sha256;
    std::optional<std::filesystem::path> local_path;
    std::optional<std::filesystem::path> manifest_path;
    std::optional<TransferState> transfer_state;
};

struct ChatMessage {
    MessageId message_id;
    ConversationId conversation_id;
    DeviceId sender_device_id;
    DeviceId receiver_device_id;
    std::string sender_display_name_snapshot;
    std::vector<MessagePart> parts;
    DeliveryState delivery_state;
    TimePoint created_at;
};
```

事件流：

```text
UI action -> command queue -> network/storage service -> app event -> app state -> UI render
```

## 11. 安全和权限

MVP 必须实现：

- 文件名和文件夹路径安全校验。
- 文件 SHA-256 校验。
- 消息和传输任务去重。
- 限制单次文件大小和文件夹总大小，可在设置中调整。
- 对未知设备的首次文件接收必须确认。
- 日志不记录消息正文和完整文件内容。

后续增强：

- 局域网共享密钥或组织码。
- TLS 或 Noise Protocol 加密。
- 设备信任列表。
- 管理员预配置策略。
- Windows 防火墙规则自动创建提示。

## 12. 里程碑

### M0：项目骨架

产出：

- CMake 项目初始化，语言标准固定为 C++23。
- EUI-NEO 通过 FetchContent 固定 commit 集成。
- Vulkan 优先的空白窗口启动成功。
- OpenGL fallback 能在 Vulkan 不可用时作为兼容路径保留。
- 软件工作目录、数据目录、日志目录、传输目录确定。
- 第三方依赖默认按静态库构建。

验收：

- Windows Debug/Release 都能构建。
- 启动显示 RelayDesk 主窗口。
- 构建日志确认 `CMAKE_CXX_STANDARD=23`。
- Release 输出目录不包含 GLFW、C++ 运行时、Asio 等第三方 DLL。
- 默认数据根目录为 `<work_dir>/data/`，不写入 `%APPDATA%`、`%LOCALAPPDATA%` 或 `%ProgramData%`。

### M1：身份和本地存储

产出：

- 首次启动生成 `identity.json`。
- 默认显示名读取电脑名。
- 设置页可修改显示名。
- JSONL 追加写入和读取模块完成。
- 聊天记录默认写入软件工作目录下的 `data/peers/`。

验收：

- 修改显示名后，`device_id` 不变。
- 手工写入多条 JSONL 后能正常加载历史。
- 移动整个软件目录后，聊天记录仍可随目录一起保留。

### M2：内网发现

产出：

- UDP discovery 服务。
- 在线用户列表。
- 心跳和离线状态。
- `profile_update` 支持。

验收：

- 同一局域网两台电脑启动后能互相发现。
- 改名后对端列表能更新显示名，历史关系不变。

### M3：文本和表情聊天

产出：

- TCP peer session。
- `schema_version = 2` 的 `parts` 聊天记录模型。
- 文本和简单表情可以在同一条消息内组合发送和接收。
- 兼容读取旧 `schema_version = 1` 单内容类型历史记录。
- 消息状态：pending、sent、delivered、failed。

验收：

- 两台电脑能互发文本、表情以及文本加表情的混合消息。
- 重启应用后能查看历史。
- 改名后历史仍按同一对端归档。

### M4：文件传输

产出：

- 图片和单文件内容块发送/接收。
- 文字、表情、图片、文件可以组合在同一条消息中发送。
- 传输进度 UI。
- SHA-256 校验。
- 失败重试。

验收：

- 发送大文件时 UI 不冻结。
- 传输完成后文件哈希一致。
- 取消和失败状态能更新原消息内对应 `part`。
- 发送文件默认保留到 `data/transfers/outbox/`。
- 接收文件默认保存到 `data/transfers/inbox/`。

### M5：文件夹传输

产出：

- 文件夹 manifest。
- 文件夹内容块可以和文字、表情、图片、文件组合在同一条消息中发送。
- 多文件顺序/并发传输。
- 路径安全校验。
- 文件夹接收目录恢复。

验收：

- 嵌套目录结构能完整恢复。
- 路径穿越测试被拒绝。
- 中断后能按已完成文件续传。
- 发送文件夹默认保留到 `data/transfers/outbox/`。
- 接收文件夹默认保存到 `data/transfers/inbox/`。

### M6：历史查看和搜索

产出：

- 按联系人查看历史。
- 按关键词搜索文本。
- 按 `part` 类型筛选包含图片、文件或文件夹的消息。
- 历史分页加载。

验收：

- 10000 条消息加载不卡 UI。
- 搜索不会修改 JSONL 原始记录。
- 历史中的文件和文件夹路径都指向软件工作目录下的相对路径。

### M7：打包和内测

产出：

- Windows 安装包或 zip 发布包。
- 默认配置模板。
- 用户使用说明。
- 日志和诊断导出。
- 发布依赖检查脚本。

验收：

- 新机器解压/安装后可直接运行。
- 首次启动能提示网络权限。
- 内测问题可通过日志定位。
- 发布目录不携带第三方 DLL。
- `dumpbin /dependents` 或等价工具检查结果符合允许列表。
- Vulkan Runtime 缺失时能给出明确提示，或自动切换 OpenGL fallback。

## 13. 风险和应对

| 风险 | 影响 | 应对 |
| --- | --- | --- |
| UDP 广播被交换机或防火墙屏蔽 | 无法发现用户 | 增加组播和可选主动扫描 |
| 设备 ID 文件丢失 | 历史关系断开 | 增加身份导出/导入，管理员部署固定身份 |
| 大文件传输阻塞 UI | 用户体验差 | 网络和哈希放入后台线程 |
| JSONL 文件过大 | 历史加载慢 | 分联系人存储、分页加载、增加索引 |
| 用户改名造成显示混乱 | 历史辨识困难 | 保存 display name snapshot，同时当前资料单独更新 |
| 同一身份被复制到多台机器 | 消息关系冲突 | 发现同 ID 多地址时提示身份冲突 |
| 文件夹路径穿越 | 安全风险 | 严格路径归一化和临时目录接收 |
| 发送文件默认复制到 outbox | 占用更多磁盘空间 | 增加工作目录容量提示、清理工具和发送快照保留策略 |

## 14. MVP 验收清单

- 启动后自动生成稳定 `device_id`。
- 默认用户名显示电脑名。
- 用户可以修改显示名。
- 两台内网电脑能互相发现。
- 发现列表显示显示名、电脑名、在线状态。
- 能发送和接收文本。
- 能发送和接收简单表情。
- 能把文本、表情、图片、文件、文件夹组合为同一条消息发送和接收。
- 能发送和接收单个文件。
- 能发送和接收文件夹。
- 能查看离线历史记录。
- 聊天记录保存为 JSONL。
- 聊天记录默认保存在软件工作目录下。
- 发送和接收的文件、文件夹默认保存在软件工作目录下。
- 聊天关系和历史路径不使用用户名。
- 改名后历史仍归属于同一台对端电脑。
