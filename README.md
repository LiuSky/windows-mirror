# iPhone USB 实时屏幕镜像（Windows / iOS 18）

这个目录现在只做一件事：让 Windows 通过 USB 持续接收 iPhone 的 Valeria/CoreMedia 媒体流并显示实时画面。截图轮询、PNG/JPEG 刷新和 DVT 抓帧不属于本方案，也不会被当成成功或备用路径。

## 方案结论

完全只用 Apple 驱动无法从 Windows 用户态读取 Valeria 的 `MI_02` bulk 端点，因为当前 Apple USB INF 没有给这个子设备提供可打开的数据接口。可行的最小改动是：

| USB 节点 | 保留/使用的驱动 | 用途 |
| --- | --- | --- |
| `VID_05AC&PID_12A8` 父设备 | Apple 官方 `usbccgp + AppleLowerFilter` | 设备配置选择 |
| `MI_01` | Apple 官方 WinUSB/过滤组件 | usbmux 与 mode 2 激活 |
| `MI_02` | Microsoft 系统自带 `winusb.inf / winusb.sys` | Valeria `FF/2A/FF` bulk IN/OUT |

默认流程不安装 libusb0、libusb-win32、libusb filter、自定义 `.sys`、自签证书或项目 INF。它只在设备管理器里把 `MI_02` 绑定到 Windows 自带的 **WinUSB Device**；Apple 父设备和 `MI_01` 不动。代价是这个 USB 端口对应的 `MI_02` 在恢复原驱动前不能同时承担 USB 网络共享。

## 当前完成度

- Apple 官方 `MI_01` 激活器已实现：发送 iOS 18 使用的 `C0/52` mode 2 请求，并以活动 configuration 中存在 `FF/2A/FF` 为成功条件。
- `MI_02` 的 Microsoft inbox WinUSB 准备、校验、GUID 注册和恢复脚本已实现。
- Valeria 握手、二进制字典、CoreMedia sample buffer、H.264/HEVC、`FEED` 视频、`EAT!` 音频和 `NEED` 流控已实现。
- Windows 程序把连续 H.264/HEVC 码流送给 `ffplay.exe`，打开实时镜像窗口并每秒报告收到的 `FEED` 帧率。
- macOS 同一台 iPhone 11 / iOS 18.1.1 已通过 Apple CoreMediaIO 验证 Valeria 是约 58 fps 的连续流。
- 协议核心已通过单元测试和原项目真实 FEED/EAT 样本回归；Windows x64 与 ARM64 目标均可交叉编译。

最后一项尚未完成的是在真实 Windows 主机上用当前手机跑满 5 分钟。因此目前应称为“可编译、协议已回归的 Windows POC”，不能声称 Windows 实机已经打通。

## Windows 自动验证与 Mac 真机路线

仓库已提供 [`.github/workflows/windows-ci.yml`](.github/workflows/windows-ci.yml)。推送到 GitHub 后，它会在三个 GitHub 托管 Windows runner 上自动构建和测试：

| runner | 架构 | 作用 |
| --- | --- | --- |
| Windows Server 2022 + VS 2022 | x64 | 当前主发布工具链 |
| Windows Server 2025 + VS 2026 | x64 | 新工具链兼容性 |
| Windows 11 + VS 2026 | ARM64 | Apple Silicon Mac 上的 Windows 11 ARM 真机实验包 |

每个 job 会用 MSVC/Windows SDK 编译、链接 `setupapi.lib` 与 `winusb.lib`，运行全部 CTest/golden FEED/EAT 回归，解析并静态检查 PowerShell，实际启动两个 Windows EXE 的帮助入口，最后上传可下载的测试包。GitHub 托管 runner 是远端新建的 VM，无法看到插在本机的 iPhone，所以它不能证明 Apple USB 枚举、MI_02 bulk、连续解码或实时窗口；这部分仍必须用有物理 USB 的 Windows 环境。[GitHub 当前 runner 列表与边界](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)

### 直接交给测试人员的包

CI 成功后，在受信任 commit 对应的 **push** 或手动 Actions run 的 **Artifacts** 下载
`ValeriaMirror-Windows-x64-delivery`（普通 Windows 10/11 电脑）或
`ValeriaMirror-Windows-arm64-delivery`（Windows 11 ARM）。仓库当前的 PR workflow 只做
构建测试；无论名称多么相似，都不要把 `pull_request` event 的 artifact 交给测试人员。
GitHub 下载的外层压缩包包含 `DOWNLOAD-FIRST.txt`、便携测试 ZIP 与
`.sha256` 校验文件；把后两个文件直接交给测试人员即可。测试人员必须完整解压便携 ZIP，
先读 `README-FIRST.zh-CN.md`，再按
`01-Preflight.cmd` → `02-Prepare-MI02.cmd` → `03-Start-Mirror.cmd` 执行，不需要源码、
Visual Studio 或 CMake。

这是免编译的实机验收包，不是能静默修改 USB 驱动的 MSI。Apple Devices、`ffplay.exe`
以及首次只给 `MI_02` 手工选择 Microsoft **WinUSB Device** 仍是必要步骤；父节点和
`MI_01` 始终保留 Apple 官方驱动。

这台 Apple Silicon Mac 上的首选真机环境是 **Parallels Desktop 26 + Windows 11 ARM**：

1. 把 iPhone USB 独占分配给 Windows，并让 Parallels 记住该选择。Parallels 当前文档明确支持把 Apple iPhone 连接到 Windows。[USB 直通说明](https://docs.parallels.com/landing/pdfm-ug/parallels-desktop-for-mac-26-users-guide/use-windows-on-your-mac/connecting-external-devices)
2. 在 Windows 11 ARM 安装 Apple Devices。Microsoft Update Catalog 中 Apple 官方 `552.0.0.0` USB 驱动包含 ARM64 的 AppleLowerFilter/KMDF/USB filter，并覆盖 PID `12A8` 父设备和 `MI_01`。[Apple USBDevice 552.0.0.0](https://www.catalog.update.microsoft.com/Search.aspx?q=Apple+USBDevice)
3. 按本 README 的既有步骤只把 `MI_02` 绑定到 ARM64 Windows 自带的 `winusb.sys`；Microsoft 明确支持 x64 与 ARM64 的 inbox WinUSB。[WinUSB 安装说明](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/winusb-installation)
4. 下载 Actions 生成的 ARM64 artifact，在 VM 内完成真实 mode 2、`FF/2A/FF`、FEED/EAT、解码和五分钟动态窗口验收。

这条路径走的是 Windows 真实 PnP/Apple 驱动/WinUSB 栈，不是 macOS 模拟，也不是截图方案。最终发布前仍建议再在普通 x64 Windows 实机上做一次五分钟终验，以覆盖不同 CPU 架构和真实 PC USB 控制器。

后续可以把这台 Parallels VM 或专用 Windows 小主机注册成私有仓库的 self-hosted runner，手动触发 iPhone 硬件测试。硬件 gate 必须以 `decoded_frames`、`presented_frames` 和画面时间戳持续推进为通过条件，不能只看收到的 USB 字节或 FEED FPS；在这个自动验收入口实现前，不添加会产生假阳性的硬件 workflow。

## Windows 构建

需要 Windows 10/11 x64、当前版 Apple Devices、Visual Studio 2022 C++/Windows SDK、CMake，以及带 `ffplay.exe` 的 FFmpeg。

在 **x64 Native Tools Command Prompt for VS 2022** 中：

```powershell
cd C:\path\to\quit
cmake -S windows-mirror -B windows-mirror\build -A x64
cmake --build windows-mirror\build --config Release
ctest --test-dir windows-mirror\build -C Release --output-on-failure
```

预期生成：

```text
windows-mirror\build\activation\Release\valeria-activate.exe
windows-mirror\build\Release\iphone-valeria-mirror.exe
```

## 第一次配置 MI_02

以下 PowerShell 需要管理员权限，并且只连接一台已解锁、已信任的 iPhone。先关闭可能独占手机 USB 句柄的 Apple Devices/iTunes 窗口。

```powershell
cd C:\path\to\quit\windows-mirror
.\scripts\Test-AppleUsbStack.ps1
.\scripts\Activate-ValeriaUsbMode.ps1 `
  -ActivationExe .\build\activation\Release\valeria-activate.exe
.\scripts\Prepare-InboxWinUsbMi02.ps1 -OpenDeviceManager
```

必须先激活，因为初始 USB mode 可能还没有 `MI_02` 节点。随后在设备管理器中只改脚本打印出的那个 `MI_02`：

1. **更新驱动程序** → **浏览我的电脑以查找驱动程序**。
2. **让我从计算机上的可用驱动程序列表中选取**。
3. 选择 **通用串行总线设备** → **WinUSB Device (Microsoft)**。
4. 不要修改 `PID_12A8` 父节点，也不要修改 `MI_01`。

设备管理器完成后，运行准备脚本打印出的 manifest 命令，例如：

```powershell
.\scripts\Install-ValeriaWinUsb.ps1 `
  -ManifestPath 'C:\ProgramData\ValeriaMirror\DriverBackup\...\manifest.json'
```

拔下手机，再插回**同一个物理 USB 端口**，然后重新激活并做严格检查：

```powershell
.\scripts\Enable-ValeriaMirrorMode.ps1 `
  -ActivationExe .\build\activation\Release\valeria-activate.exe
.\scripts\Get-ValeriaUsbState.ps1 -RequireReady
```

`RequireReady` 只有在以下条件同时成立时才通过：Apple 父设备/`MI_01` 完整、`MI_02` 使用 Microsoft `winusb.inf`、项目 GUID 已注册、接口描述符为 `FF/2A/FF`。

## 启动实时镜像

```powershell
.\build\Release\iphone-valeria-mirror.exe `
  --ffplay C:\ffmpeg\bin\ffplay.exe `
  --force-h264
```

首轮硬件验收先用真实 golden 已覆盖的 H.264，确认 USB、握手和窗口都稳定后，再去掉 `--force-h264` 验证默认 HEVC 路径。

程序必须出现 `ffplay` 实时窗口，并持续打印类似日志：

```text
[USB] Valeria FF/2A/FF ... bulkIn=... bulkOut=...
[FORMAT] video=h264 1126x2436 ...
[FIRST_FRAME] ...
[LIVE] fps=... frames=... duration=... video=... audio=...
```

`--diagnostic-only` 和码流 dump 只用于协议排障，明确不算屏幕镜像成功。`[LIVE] fps` 是收到的 `FEED` sample 速率，不是播放器内部渲染统计；正式验收仍要求肉眼确认窗口画面持续变化至少 5 分钟，并且日志中持续有非零 FPS。完整规则见 [AGENTS.md](AGENTS.md)。

## 恢复 MI_02 原驱动

准备脚本会把恢复信息保存在 `%ProgramData%\ValeriaMirror\DriverBackup`。需要恢复 USB 网络功能时，在管理员 PowerShell 中：

```powershell
.\scripts\Restore-AppleMi02.ps1
```

再拔插一次同一 USB 端口。该脚本只恢复 `MI_02`，不会修改 Apple 父节点或 `MI_01`。如果 manifest 是在 WinUSB 已经被手动选中之后才创建的，它无法猜出更早的网络驱动，此时按脚本提示在设备管理器中手动选回原 USB NCM 驱动。

## 目录

- [`windows-mirror/`](windows-mirror/)：Windows 实时镜像程序与协议测试。
- [`windows-mirror/activation/`](windows-mirror/activation/)：只经 Apple 官方 `MI_01` 工作的模式激活器。
- [`windows-mirror/driver/`](windows-mirror/driver/)：inbox WinUSB 的首次配置、检查和恢复说明。
- [`docs/ios18-usb-findings.md`](docs/ios18-usb-findings.md)：iOS 18 USB 模式、官方驱动边界与协议依据。
- [`Sources/IPhoneScreenBridge/`](Sources/IPhoneScreenBridge/)：macOS CoreMediaIO 实机参照实现，用来证明当前手机确实能输出连续 Valeria 视频。

协议实现参考了 `quicktime_video_hack_windows` 与原始 `quicktime_video_hack` 的 MIT 代码和 fixtures；归属与许可证见 [`windows-mirror/THIRD_PARTY_NOTICES.md`](windows-mirror/THIRD_PARTY_NOTICES.md)。
