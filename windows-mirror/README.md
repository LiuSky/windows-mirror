# iPhone Valeria Windows continuous mirror POC

这是一个真正的 USB 连续屏幕镜像 POC，不是截图轮询。程序完成 Apple
QuickTime/Valeria 握手，持续接收 `FEED` 视频和 `EAT!` 音频，把
H.264/HEVC 转成 Annex-B，并通过独立的低延迟管道喂给 `ffplay.exe`
显示实时窗口。

## 数据路径

```text
iPhone (iOS 18, Apple mode 2)
  -> Apple USBCCGP + AppleLowerFilter（保留）
  -> MI_02 FF/2A/FF + Microsoft inbox winusb.sys
  -> PacketFramer（分片/粘包）
  -> QuickTime/Valeria PING/SYNC/ASYN
  -> CMSampleBuffer avc1/hvc1/hev1 + LPCM
  -> bounded writer queue -> ffplay live window
```

播放器写入在线程中进行，不会阻塞 USB 协议线程的 `NEED` 回包。如果
播放器落后，队列会丢弃积压数据，等待下一个 H.264 IDR / HEVC IRAP，
先重发 SPS/PPS 或 VPS/SPS/PPS，再恢复实时画面。

## 驱动边界（重要）

本目录不包含 libusb0、libusb-win32 filter 或自定义内核 `.sys`。

- Apple 官方父节点 `usbccgp + AppleLowerFilter` 保留。
- Apple 官方 MI_01（usbmux）保留。
- 只有 PID `12A8` 的 MI_02 使用微软系统自带 `winusb.sys`。

默认路线不安装项目 INF：先在设备管理器中只给 MI_02 手动选择 Microsoft
系统自带的 **WinUSB Device**（`winusb.inf / winusb.sys`），再由脚本给这个
devnode 写入应用 GUID。它会在绑定期间占用同一个 MI_02 硬件 ID，因此 USB
网络共享会暂时不可用。详见 [driver/README.md](driver/README.md)。若严格
要求“MI_02 也必须使用 Apple INF”，公开 WinUSB API 目前无法打开它的 bulk
端点，这套实时后端就无法工作。

## 构建

要求 Windows 10/11、Visual Studio 2022 C++、Windows SDK 和 CMake。在
“x64 Native Tools Command Prompt for VS 2022”运行：

```powershell
cd windows-mirror
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

同一次构建会生成：

- `build\Release\iphone-valeria-mirror.exe`：连续镜像主程序；
- `build\activation\Release\valeria-activate.exe`：MI_01 激活/描述符诊断；
- `build\Release\valeria_core_tests.exe`：协议和真实 golden packet 回归。

## 首次准备

1. 安装/修复当前 Apple Devices USB 驱动（必须是
   `usbccgp + AppleLowerFilter` 的现代栈，不是传统 `usbaapl64.sys` 父驱动）。
2. 安装 FFmpeg，并确保 `ffplay.exe` 在 `PATH`，或记住它的绝对路径。
3. 在管理员 PowerShell 先检查 Apple 栈：

```powershell
.\scripts\Test-AppleUsbStack.ps1
```

4. 首先经官方 MI_01 激活 mode 2，让初始状态中可能不存在的 MI_02 出现：

```powershell
.\scripts\Activate-ValeriaUsbMode.ps1 `
  -ActivationExe .\build\activation\Release\valeria-activate.exe
.\scripts\Prepare-InboxWinUsbMi02.ps1 -OpenDeviceManager
```

5. 按脚本打印的精确 instance ID，只在设备管理器中给 MI_02 选择 Microsoft
   **WinUSB Device**。随后运行准备脚本打印出的
   `Install-ValeriaWinUsb.ps1 -ManifestPath ...`；它只注册 GUID，不安装 INF。
   拔下手机并插回同一物理端口。

6. 再次激活并验证 Apple mode 2。正确请求是
   `C0/52/value=0/index=2/length=1`，响应必须是一字节 `00`；随后仍必须用
   descriptor 确认当前接口为 `FF/2A/FF`：

```powershell
.\scripts\Enable-ValeriaMirrorMode.ps1 `
  -ActivationExe .\build\activation\Release\valeria-activate.exe
.\scripts\Get-ValeriaUsbState.ps1 -RequireReady
```

不要硬编码 configuration 5/6 或端点号。AppleLowerFilter 选择配置，主程序
再动态验证 interface class/subclass/protocol 和 bulk IN/OUT pipes。

## 启动实时镜像

连接一台已解锁/已信任的 iPhone：

```powershell
.\build\Release\iphone-valeria-mirror.exe `
  --device YOUR-IPHONE-UDID `
  --ffplay C:\ffmpeg\bin\ffplay.exe `
  --force-h264
```

没有 `--ffplay` 时会从 `PATH` 自动寻找。默认 HPD1 按 iOS 18 需要的固定
顺序发送四个 key：`DisplaySize`、`HEVCDecoderSupports444`、
`H264DecoderSupports444`、`Valeria`，并同时支持 `avc1`、`hvc1` 和
`hev1`。如需请求 AVC，使用 `--force-h264`；HEVC key 仍会保留，只把值设
为 false，避免 iOS 18 因 key set 不完整而停住。首轮真机建议保留
`--force-h264`，因为真实 golden 已覆盖 H.264；链路稳定后再去掉它验证 HEVC。

成功的验收信号不是“收到 USB 包”，而是：

- 弹出 `iPhone USB Screen Mirror` 连续视频窗口；
- 控制台出现 `[FIRST_FRAME]`；
- `[LIVE] fps=... frames=... duration=...` 持续增长。

这里的 FPS 是收到的 `FEED` sample 速率，不是 ffplay 内部渲染统计；正式
验收还必须实际观察窗口画面持续变化五分钟。

`--video-dump` 和 `--audio-dump` 只用于诊断。`--diagnostic-only` 会明确
关闭窗口，这种模式不能算镜像成功。当前 POC 已完整解析并暴露 LPCM
回调/可选 PCM dump；实时窗口先显示视频，音频同步播放可在后续渲染层接入。

按 Ctrl+C 会发送 `HPA0`/`HPD0`，等待 `RELS`，再关闭 WinUSB 和 ffplay。

## 回归覆盖

CTest 同时覆盖合成边界和 Daniel Paulus 的未修改 MIT golden packets，
包括 USB 分片/粘包、CWPA/CVRP/AFMT、1126×2436 AVC1 的 90,750-byte
访问单元、48 kHz 双声道 16-bit/4,096-byte LPCM，以及 AFMT reply 的逐字节
一致性。第三方来源与许可证见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
