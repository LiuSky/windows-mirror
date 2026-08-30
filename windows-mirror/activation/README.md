# Valeria activation probe（Windows）

这个小程序只负责连续屏幕镜像链路的第一关：在保留 Apple 官方驱动的前提下，让 iPhone 切到 Valeria USB mode，并确认当前 USB configuration 真的出现且选中了 `FF/2A/FF` 接口。

它不是截图工具，也不是已经完成的屏幕镜像。只有后续 MI02 bulk 端点能够完成 QuickTime/Valeria 握手并持续收到 `FEED` 视频与 `EAT!` 音频，整个 Windows 镜像方案才算打通。

## 它做什么

1. 用 SetupAPI 枚举 Apple Catalog 驱动公开的 MI01 GUID：`{664be590-54bd-4964-8a8c-6cd1314f6dc2}`。
2. 用微软系统自带的 `winusb.sys` 打开 MI01，不安装 libusb0、libusb-win32 或 libusb filter。
3. 读取当前 mode、当前 configuration 和所有可读 USB descriptors。
4. 使用 `--enable` 时发送 Valeria mode 2 请求，关闭旧句柄并观察 USB 断开/重枚举。
5. 重开 MI01，只有“当前 configuration 内存在 `FF/2A/FF`”才输出 `VALERIA_ACTIVE`。
6. 如果机器使用传统 AMDS `usbaapl64.sys`，明确输出 `UNSUPPORTED_CLASSIC_AMDS`，不会猜测其私有 IOCTL ABI。

程序不会停止 Apple 服务、不会写 `OriginalConfigurationValue`、不会改注册表、不会替换驱动。Apple Catalog 的 `AppleLowerFilter` 会根据 mode 返回值选择 preferred configuration；mode 2 常见返回以 `5` 开头，因此正常路径是它自动选择 configuration 5。

## 协议依据与旧代码差异

截至本实现核对的 libimobiledevice/usbmuxd master `3ded00c9985a5108cfc7591a309f9a23d57a8cba`，vendor request 的方向始终是 device-to-host (`0xC0`)：

| 操作 | bmRequestType | bRequest | wValue | wIndex | wLength | 响应 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| GET mode | `0xC0` | `0x45` | `0` | `0` | `4` | 常见 initial=`03 03 03 00`，隐藏 mode=`05 03 03 00` |
| SET Valeria | `0xC0` | `0x52` | `0` | `2` | `1` | 必须返回 1 字节 `00` |

源码位置：

- [usbmuxd `submit_vendor_specific`](https://github.com/libimobiledevice/usbmuxd/blob/3ded00c9985a5108cfc7591a309f9a23d57a8cba/src/usb.c#L368-L380)
- [GET/SET mode 参数](https://github.com/libimobiledevice/usbmuxd/blob/3ded00c9985a5108cfc7591a309f9a23d57a8cba/src/usb.c#L680-L724)
- [初始 GET mode 参数](https://github.com/libimobiledevice/usbmuxd/blob/3ded00c9985a5108cfc7591a309f9a23d57a8cba/src/usb.c#L788-L805)

用户原仓库中的旧请求是 `0x40/0x52/wIndex=2/wLength=0`。这个实现刻意不用旧格式；方向和响应长度是 iOS 18 排障时必须验证的差异。

## 构建

需要 Windows 10/11、Visual Studio 2022 C++ 工具链、Windows SDK 和 CMake。在 “x64 Native Tools Command Prompt for VS 2022” 中：

```powershell
cd windows-mirror\activation
cmake -S . -B build -A x64
cmake --build build --config Release
```

输出文件：`build\Release\valeria-activate.exe`。

## 使用

先只读探测：

```powershell
.\build\Release\valeria-activate.exe --probe
```

确认只连接一台 iPhone 后激活：

```powershell
.\build\Release\valeria-activate.exe --enable --wait-ms 15000
```

多台设备时用设备 instance ID 中的序列号片段选择：

```powershell
.\build\Release\valeria-activate.exe --enable --device SERIAL_SUBSTRING
```

如果 `CreateFile` 返回 access denied 或 sharing violation，先关闭 Apple Devices/iTunes 等会占用 MI01 的程序，再从管理员终端重试。激活器不会擅自停止 `Apple Mobile Device Service`。

## 结果与退出码

| 退出码 | `RESULT state` | 含义 |
| ---: | --- | --- |
| `0` | `PROBE_COMPLETE` / `VALERIA_ACTIVE` | 探测完成；激活模式下只有 active configuration 含 `FF/2A/FF` 才成功 |
| `2` | `REENUMERATED_BUT_UNVERIFIED` 等 | 请求可能已接受，但描述符/句柄不足以确认 Valeria，不能当作镜像成功 |
| `10` | `NO_APPLE_USB_DEVICE` | 未发现匹配设备 |
| `11` | `UNSUPPORTED_CLASSIC_AMDS` | 传统 `usbaapl64.sys` 栈没有公开 MI01 WinUSB 接口 |
| `12` | `MI01_WINUSB_INTERFACE_MISSING` | Apple USB 节点存在，但目标 GUID 不存在 |
| `13` | `MI01_PRESENT_BUT_NOT_OPENABLE` | GUID 存在但 WinUSB 句柄打不开 |
| `14` | `AMBIGUOUS_DEVICE` | `--enable` 匹配多台设备 |
| `20` | `SET_MODE_FAILED` | `C0/52` 没有得到 1 字节 `00`，且激活未验证 |
| `21` | `MI01_DID_NOT_REAPPEAR` / `REENUMERATION_TIMEOUT` | USB 重枚举超时 |
| `22` | `VALERIA_VISIBLE_BUT_NOT_ACTIVE` | 能看到 Valeria descriptor，但父驱动没有选中它所在 configuration |

`VALERIA_ACTIVE` 仅表示 USB 激活门槛通过。下一步仍需给 MI02 的 `FF/2A/FF` 接口绑定微软 inbox WinUSB，打开 bulk IN/OUT，并证明连续媒体包到达。
