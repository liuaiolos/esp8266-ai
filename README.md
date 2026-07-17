<p align="center">
  <img src="docs/images/logo.svg" width="72" alt="logo">
</p>

<h1 align="center">AI Mac 小屏幕</h1>

<p align="center">桌上的一台 AI 状态小电脑 —— M5Stack Core ESP32 · 开源硬件 · 桌面伴侣</p>

<p align="center">
  中文 ·
  <a href="README.en.md">English</a>
</p>

<p align="center">
  <a href="https://mac.qust.me">官网</a> ·
  <a href="https://mac.qust.me/#flash">网页刷机</a> ·
  <a href="https://github.com/pengchujin/esp8266-ai/releases/latest">下载</a>
</p>

<p align="center">
  <img src="docs/images/hero.jpg" width="640" alt="AI Mac 小屏幕">
</p>

一块 320×240 的复古小电视，放在桌上实时显示 **Claude Code / Codex CLI 在干什么、额度还剩多少**。不需要任何 API key：数据来自本机已有的 CLI 登录凭据和会话日志，由配套的 Mac / Windows 桥接程序在局域网内提供给设备。

## 功能

| | |
|---|---|
| <img src="docs/images/feature1.jpg" width="360" alt="AI 工作状态"> | **AI 工作状态与额度**<br>桌宠动起来 = AI 正在干活。方形进度环 + 大字显示会话 / 周额度的真实用量；若 Codex 账户仅提供周额度，自动只显示周额度。额度用满自动换成重置倒计时，等你审批时整圈边框红闪提醒。 |
| <img src="docs/images/feature2.jpg" width="360" alt="网速监视"> | **网速实时监视**<br>任务管理器风格的上下行曲线，56 秒滚动窗口，量程自动调整。 |
| <img src="docs/images/music.jpg" width="360" alt="音乐播放"> | **音乐播放显示**<br>专辑封面、歌名、歌手、进度条实时同步；音乐响起自动切入，停止自动切回。 |
| <img src="docs/images/feature3.jpg" width="360" alt="桌宠可换"> | **可换桌宠**<br>内置 [petdex.dev](https://petdex.dev) 画廊 3300+ 开源桌宠，也可上传任意 GIF，设备板上直接解码，无需重烧固件。 |

## 快速上手

需要的东西：一台 **M5Stack Core Basic / Gray（M5GO v2.6，ESP32、16MB Flash）** 和一根 USB **数据**线。Core2、M5Stick 与旧版 ESP8266 SD2 小电视不适用此固件。

### 第 1 步 · 刷固件

安装 PlatformIO 后，在仓库根目录构建并烧录：

```sh
cd firmware
pio run -e m5stack-core-esp32 -t upload --upload-port /dev/cu.usbserial-…
```

省略 `--upload-port` 时 PlatformIO 会尝试自动识别串口。分区、LittleFS 和 M5GO 屏幕配置已经写在 `firmware/platformio.ini`；完整说明见 [firmware/README-M5STACK.md](firmware/README-M5STACK.md)。

### 第 2 步 · 配 WiFi

设备首次开机会开热点 **`AI-Clock-Setup`**：手机连上后自动弹出配网页（没弹就用浏览器打开 `192.168.4.1`），选择家里 WiFi、输入密码，完成。

### 第 3 步 · 装桥接程序

从 [Releases](https://github.com/pengchujin/esp8266-ai/releases/latest) 下载并打开：

- **macOS**：`AIClockBridge-*-macOS.dmg`，拖入 Applications（ad-hoc 签名，首次启动需在「系统设置 → 隐私与安全性」允许，并同意本地网络权限）
- **Windows**：`AIClockBridge-*-Windows-x64.exe`，双击即用

桥接程序常驻菜单栏 / 托盘，会**自动发现并配对**同一局域网内的设备——到这里屏幕就活了。

<p align="center">
  <img src="docs/images/working.jpg" width="640" alt="工作演示">
</p>

日常使用都在托盘图标上：**左键**打开设备画面的实时镜像（底部有屏幕亮度滑条），**右键**是完整菜单（额度详情、屏幕切换、更换桌宠、音乐/网速页等）。

### USB 有线直连（可选）

若路由器开启了客户端隔离，或暂时不想配置 Wi‑Fi，可保持 macOS bridge 运行并用 USB 数据线连接设备。bridge 识别到兼容串口后会向设备推送状态和网速数据；设备屏幕会切换到有线数据源。烧录固件前请退出 bridge，避免两个程序同时读取串口。

## 常见问题

- **屏幕边框红色闪烁**：设备连不上桥接程序——确认电脑端程序在运行、和设备在同一 WiFi。
- **额度一直显示 `-`**：本机没有登录过 Claude Code / Codex CLI，桥接程序读不到凭据。
- **想换桌宠**：右键托盘图标 → 「更换桌宠动画…」，挑一个点上传就行。

### Codex 状态没有及时停下来 / 一直在工作

桥接程序会扫描 `~/.codex/sessions` 作为兼容兜底，但日志的最后一次写入并不代表 Codex 仍在执行——特别是任务完成或等待审批时，Codex 还会继续落盘。推荐配置 Codex hooks，让生命周期事件直接推送给桥接程序；桥接会以这些事件为准，并自动在没有 hooks 时退回日志扫描。

脚本已随仓库版本控制，位于 [`scripts/codex-status-hook.sh`](scripts/codex-status-hook.sh)。在已克隆的仓库根目录执行下面命令安装到 Codex 配置目录外的稳定位置：

```sh
mkdir -p ~/.ai-clock
install -m 755 scripts/codex-status-hook.sh ~/.ai-clock/codex-status-hook.sh
```

如果系统没有 `install` 命令，改用：

```sh
mkdir -p ~/.ai-clock
cp scripts/codex-status-hook.sh ~/.ai-clock/codex-status-hook.sh
chmod 755 ~/.ai-clock/codex-status-hook.sh
```

然后在 `~/.codex/hooks.json` 的 `hooks` 中，为以下六个事件各加入一条 command hook：`SessionStart`、`UserPromptSubmit`、`PreToolUse`、`PermissionRequest`、`PostToolUse`、`Stop`。command 分别填写：

```text
~/.ai-clock/codex-status-hook.sh <事件名>
```

例如 `Stop` 项可写成（保留你已有的其他 hooks）：

```json
"Stop": [{
  "hooks": [{ "type": "command", "command": "~/.ai-clock/codex-status-hook.sh Stop", "timeout": 2 }]
}]
```

首次运行时若 Codex 要求确认 hook，请在 Codex 中批准。`PermissionRequest` 会让屏幕停止工作动画并显示红色提醒；`Stop` 会立即结束动画，不会再被最后的 JSONL 写入误判成工作中。

确认 `~/.codex/config.toml` 的 `[features]` 中已启用 `hooks = true`；如 Codex 提示信任新 hook，在 TUI 中运行 `/hooks` 后批准。以后更新仓库后，重新执行上面的安装命令即可更新已安装脚本。

## 开发

```
firmware/     M5Stack Core ESP32 固件（PlatformIO + Arduino，含板上 GIF 解码）
mac-app/      macOS 菜单栏桥接（Swift/SPM，零第三方依赖）
windows-app/  Windows 托盘桥接（C# / .NET 8 WinForms）
tools/        GIF → RGB565 内置精灵图转换脚本
scripts/      版本控制的用户辅助脚本（含 Codex 状态 hook）
docs/         开发文档（硬件引脚、HTTP API、架构细节）
```

### 构建、测试与打包

需要 PlatformIO、Xcode/Swift 工具链，以及 Windows 桥接程序所需的 .NET 8 SDK。以下命令均从仓库根目录开始执行；若 `pio` 未加入 `PATH`，可用 `~/.platformio/penv/bin/pio` 替代。

```bash
# 固件（M5Stack Core ESP32）
cd firmware && pio run -e m5stack-core-esp32
cd firmware && pio run -e m5stack-core-esp32 -t upload --upload-port /dev/cu.usbserial-…

# macOS 桥接：运行、测试、Release 应用包
cd mac-app && swift run
cd mac-app && swift test
cd mac-app && ./scripts/package-macos-app.sh release  # dist/AI Clock Bridge.app

# Windows 桥接：构建、运行、发布单文件
cd windows-app && dotnet build AIClockBridge/AIClockBridge.csproj
cd windows-app && dotnet run --project AIClockBridge/AIClockBridge.csproj
cd windows-app && dotnet publish AIClockBridge/AIClockBridge.csproj -c Release -r win-x64 --self-contained false
```

硬件引脚表、屏幕驱动的坑、设备 HTTP API、GIF 板上解码架构等细节见 **[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)**。

硬件、固件、软件全部开源，拿去改、拿去做、拿去卖都行。
