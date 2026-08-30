# iOS 18 / Windows USB 实时镜像调研结论

## 最终路线

iOS 18 仍通过 Apple 的 Valeria USB 接口输出连续屏幕媒体。目标链路是：

```text
iPhone screen
  → Valeria USB interface FF/2A/FF (MI_02)
  → Microsoft inbox winusb.sys
  → WinUsb_ReadPipe / WinUsb_WritePipe
  → QuickTime/Valeria FEED + EAT!
  → H.264/HEVC decoder
  → Windows live preview
```

这不是屏幕截图链路。任何 DVT screenshot、PNG/JPEG 轮询或 HTTP 图片刷新都被排除；它们不能替代持续媒体流。

## 当前实机与证据

当前连接设备为 iPhone 11（`iPhone12,1`）、iOS 18.1.1（22B91）、USB 已信任。macOS 的 Apple CoreMediaIO 采集组件能让它进入带 Valeria 的隐藏配置，并持续收到约 58 fps 的解码画面。这证明手机、线缆和 iOS 18 端的实时 Valeria 输出仍然可用。

macOS 观察到 mode 4 下的 configuration 6；Windows 官方 `AppleLowerFilter` 路线使用不同的 mode 2，并把 preferred configuration 限制在 5。因此 Windows 实现必须请求 mode 2、验证活动 configuration，而不能直接照抄 macOS 的 configuration 6，也不能继续使用旧项目写死的 configuration 数字。

## 原仓库在 iOS 18 上需要修正的地方

原 `quicktime_video_hack_windows` 的媒体协议方向仍成立：激活 Valeria、打开 bulk IN/OUT、完成 QuickTime 握手、持续接收 `FEED` 视频和 `EAT!` 音频。但旧 USB mode 请求使用了 `0x40/0x52/wIndex=2/wLength=0`。

当前 `usbmuxd` 对新设备使用：

| 操作 | bmRequestType | bRequest | wValue | wIndex | wLength | 结果 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| GET mode | `0xC0` | `0x45` | `0` | `0` | `4` | 返回 mode/config 候选字节 |
| SET Valeria | `0xC0` | `0x52` | `0` | `2` | `1` | 成功返回单字节 `00` |

发送 SET 后设备会断开并重枚举，因此异步 control transfer 的 `ERROR_DEVICE_NOT_CONNECTED` / `ERROR_OPERATION_ABORTED` 只能在这一条请求上被容忍；最终成功依据必须是重开设备后，活动 configuration 中确实出现 `FF/2A/FF`。

## Apple 官方 Windows 驱动的边界

Apple USB Catalog 的当前 `AppleUsb.inf` 结构是：

- `VID_05AC&PID_12A8` 父节点：Microsoft `usbccgp`，叠加 Apple `AppleLowerFilter`；`OriginalConfigurationValue=2`、`UsbccgpCapabilities=0x10`。
- `MI_01`：Apple 包装的 WinUSB 路径，发布接口 GUID `{664BE590-54BD-4964-8A8C-6CD1314F6DC2}`，并带 Apple UMDF/KMDF 过滤组件。
- Apple INF 没有给 Valeria `MI_02` 声明用户态 bulk binding。

所以“所有节点都只用 Apple INF”无法完成 Windows 实时镜像：能激活，并不等于应用能打开 `MI_02`。但这不要求写一个新内核驱动。Microsoft 官方支持在设备管理器中给指定 USB 功能选择系统自带 **WinUSB Device**，随后给该 devnode 注册应用 GUID。最终组合是 Apple 父节点 + Apple `MI_01` + Microsoft inbox WinUSB `MI_02`，没有项目 `.sys`，也没有 libusb filter。

Apple 设备本身没有为 `MI_02` 提供 Microsoft OS descriptor `USB\MS_COMP_WINUSB`，所以无法使用 firmware-driven 的免 INF 自动绑定；首次仍需要设备管理器手动选择，或者使用一个经过签名的匹配 INF。项目把后者隔离在 `driver/optional-inf/`，不作为默认方案。

## 第一次枚举顺序为什么重要

初始 mode 可能只有四个 configuration，此时 `MI_02` 根本不存在，不能先绑定驱动。正确顺序是：

1. 通过 Apple 官方 `MI_01` 请求 mode 2。
2. AppleLowerFilter 选择 configuration 5，Windows 枚举 Valeria `MI_02`。
3. 只给这个 `MI_02` 选择 Microsoft inbox WinUSB，并添加应用 GUID。
4. 拔插回同一物理端口，让 Windows 为同一实例注册 GUID 接口。
5. 再次经 `MI_01` 激活 mode 2。
6. 同时验证 Apple 栈未变、`MI_02` 是 `FF/2A/FF`、bulk IN/OUT 可开。

后续会话通常只需第 5、6 步。绑定与 GUID 是按设备实例/物理端口保存的，换 USB 端口可能需要重新执行首次流程。

## 连续媒体协议门槛

USB 能打开只是第一关。iOS 18 会因为 HPD1 字典的 key 缺失或顺序变化而停住，因此当前实现固定发送：

1. `DisplaySize`
2. `HEVCDecoderSupports444`
3. `H264DecoderSupports444`
4. `Valeria`

握手覆盖 PING、双 HPD1、CWPA、HPA1、CVRP/NEED，以及 AFMT/CLOK/TIME/SKEW/OG 等同步消息。每个 `FEED` 后严格发送一次 `NEED`；`FEED` 的 CoreMedia sample 转成 H.264/HEVC Annex-B 交给实时解码器，`EAT!` 的 LPCM 也持续解析。关闭时发送 HPA0/HPD0 并等待 release 通知。

当前回归已使用原 MIT 项目的真实二进制 fixtures 验证：

- 约 91 KB 的 `ASYN/FEED` 样本解析为 1126×2436 AVC1 和约 90 KB IDR Annex-B。
- 约 4 KB 的 `ASYN/EAT!` 样本解析为 48 kHz、双声道、16-bit LPCM。
- AFMT 回复与 golden packet 字节一致。
- CWPA、CVRP、CLOK、TIME、SKEW、OG 等回复路径能够消费真实输入。

这些测试证明协议实现与已知数据兼容，但不能代替 Windows 物理 USB 测试。

## 完成与未完成

已完成：

- iOS 18 mode 请求修正与 `FF/2A/FF` 描述符 gate。
- Microsoft inbox WinUSB-only 的 `MI_02` 配置/检查/恢复流程。
- WinUSB overlapped bulk transport，支持 read fragmentation 与 packet coalescing。
- Valeria/CoreMedia 连续视频和音频解析。
- H.264/HEVC 实时 `ffplay` 窗口、首帧与 FPS 日志。
- 单元测试、真实 fixtures 回归、Windows x64 交叉编译检查。

未完成：

- 当前工作机是 macOS，尚未在真实 Windows 上验证 Apple MI_01 是否允许当前进程打开、手动 inbox WinUSB binding 是否在这台 iPhone/端口保持，以及连续 bulk 会话是否跑满 5 分钟。

因此 Windows 真机只有满足 [AGENTS.md](../AGENTS.md) 的五项 gate 后才能标记为打通，不能把编译成功、激活成功、收到单帧或写出码流文件当成镜像成功。

## 主要依据

- [原 Windows 项目](https://github.com/chotgpt/quicktime_video_hack_windows)
- [libimobiledevice/usbmuxd 当前 USB mode 实现](https://github.com/libimobiledevice/usbmuxd/blob/master/src/usb.c)
- [Microsoft：WinUSB installation for developers](https://learn.microsoft.com/windows-hardware/drivers/usbcon/winusb-installation)
- [Microsoft：Automatic installation of WinUSB](https://learn.microsoft.com/windows-hardware/drivers/usbcon/automatic-installation-of-winusb)
- [Apple USB 驱动目录搜索结果](https://www.catalog.update.microsoft.com/Search.aspx?q=Apple%2C+Inc.+-+USBDevice+-+538.0.0.0)

第三方代码和 fixtures 的许可证说明见 [`windows-mirror/THIRD_PARTY_NOTICES.md`](../windows-mirror/THIRD_PARTY_NOTICES.md)。
