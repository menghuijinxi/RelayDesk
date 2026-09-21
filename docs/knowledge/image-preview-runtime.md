# 图片预览运行时问题复盘

## SkiaUI 聊天图片直接使用原图

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
