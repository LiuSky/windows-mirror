# ValeriaMirror Windows 实机验收包

这个 ZIP 是可以直接转交给测试人员的便携验收包，不需要 Visual Studio，
也不需要从源码构建。它运行连续 QuickTime/Valeria USB 媒体流；截图轮询不属于
本方案，也不能算验收成功。

请先把整个 ZIP 完整解压到普通文件夹，再运行其中的 `.cmd`；不要在 Windows 的
“压缩文件夹”预览窗口里直接双击脚本。

## 测试前准备

- 只连接一台已解锁、已在此电脑上点过“信任”的 iPhone。
- 从 Microsoft Store 安装最新版 **Apple Devices**，确认它能看到手机后关闭它。
- 安装包含 `ffplay.exe` 的 FFmpeg，并把它加入 `PATH`；也可以把一个可独立运行的
  `ffplay.exe` 放进本包的 `tools` 目录。
- 不要安装 libusb0、libusb-win32 或任何 USB filter。
- 所有首次准备步骤都必须使用同一根线和同一个物理 USB 端口。

## 验收步骤

1. 右键 `01-Preflight.cmd`，选择“以管理员身份运行”。它只检查 Apple USB 栈、
   激活 Apple mode 2，并确认 `MI_02` 出现；不会更换驱动。
2. 第一步成功后，右键 `02-Prepare-MI02.cmd`，选择“以管理员身份运行”。严格按
   窗口提示，只在设备管理器中把打印出的 `MI_02` 节点选择为 Microsoft
   **WinUSB Device**。绝对不要修改 PID `12A8` 父节点或 `MI_01`。
3. 脚本完成后，拔下 iPhone，再插回同一个 USB 端口。如果在 Parallels 虚拟机测试，
   出现 USB 归属提示时仍将 iPhone 分配给 Windows；物理 Windows 电脑可忽略此句。
4. 关闭 Apple Devices，然后双击 `03-Start-Mirror.cmd`。找不到 `ffplay.exe` 时，
   脚本会要求输入其完整路径。首轮固定请求 H.264。
5. 操作手机并观察 Windows 窗口。只有以下条件全部满足才算通过：

   - 控制台显示 `FF/2A/FF` bulk IN/OUT；
   - 出现 `[FIRST_FRAME]`；
   - `[LIVE]` 的 frames/duration 持续增长；
   - `iPhone USB Screen Mirror` 窗口随手机操作持续变化至少五分钟。

按 `Ctrl+C` 正常停止。`[LIVE] fps` 只代表收到的 FEED sample，不能单独替代
动态窗口验收。

## 恢复

需要恢复原来的 `MI_02` 驱动/USB 网络功能时，右键
`04-Restore-Apple-MI02.cmd` 并选择“以管理员身份运行”，随后按提示拔插同一
USB 端口。恢复脚本只使用步骤 02 首次保存的、切换 WinUSB 之前的有效清单，只处理
`MI_02`，不会修改 Apple 父节点或 `MI_01`。如果运行步骤 02 前 `MI_02` 就已经是
WinUSB，脚本无法猜出更早的网络驱动，只能在设备管理器中手工选回原 USB NCM/网络
驱动。

## 反馈内容

请反馈 Windows 版本/架构、每一步的成功或错误文本、是否出现动态窗口以及持续
时间。日志可能包含设备 instance ID；不要公开粘贴，私下发送前可以遮盖序列号。
不要上传屏幕录像、原始 H.264/HEVC、音频、pairing record 或驱动备份。

详细原理与手工命令见 `TECHNICAL-README.md` 和 `DRIVER-DETAILS.md`。
