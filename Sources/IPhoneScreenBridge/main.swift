import AppKit
import AVFoundation
import CoreImage
import CoreMedia
import CoreMediaIO

private let captureQueue = DispatchQueue(label: "dev.local.iphone-screen-bridge.capture")

private final class PreviewView: NSView {
    let previewLayer = AVCaptureVideoPreviewLayer()

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
        previewLayer.videoGravity = .resizeAspect
        previewLayer.backgroundColor = NSColor.black.cgColor
        layer?.addSublayer(previewLayer)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func layout() {
        super.layout()
        previewLayer.frame = bounds
    }
}

private final class AppDelegate: NSObject, NSApplicationDelegate, AVCaptureVideoDataOutputSampleBufferDelegate {
    private let session = AVCaptureSession()
    private let ciContext = CIContext(options: [.cacheIntermediates: false])
    private let previewView = PreviewView(frame: NSRect(x: 0, y: 0, width: 520, height: 820))
    private let statusLabel = NSTextField(labelWithString: "正在初始化…")
    private var window: NSWindow?
    private var discoveryTimer: Timer?
    private var frameCount = 0
    private var firstFrameWritten = false
    private var firstFrameURL: URL?
    private var startedAt: ContinuousClock.Instant?

    func applicationDidFinishLaunching(_ notification: Notification) {
        createWindow()
        requestVideoPermissionAndStart()
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }

    func applicationWillTerminate(_ notification: Notification) {
        discoveryTimer?.invalidate()
        if session.isRunning {
            session.stopRunning()
        }
    }

    private func createWindow() {
        let content = NSView(frame: previewView.frame)
        content.wantsLayer = true
        content.layer?.backgroundColor = NSColor.black.cgColor

        previewView.translatesAutoresizingMaskIntoConstraints = false
        statusLabel.translatesAutoresizingMaskIntoConstraints = false
        statusLabel.textColor = .white
        statusLabel.backgroundColor = NSColor.black.withAlphaComponent(0.65)
        statusLabel.drawsBackground = true
        statusLabel.alignment = .center
        statusLabel.font = .monospacedSystemFont(ofSize: 12, weight: .medium)
        statusLabel.maximumNumberOfLines = 2

        content.addSubview(previewView)
        content.addSubview(statusLabel)
        NSLayoutConstraint.activate([
            previewView.leadingAnchor.constraint(equalTo: content.leadingAnchor),
            previewView.trailingAnchor.constraint(equalTo: content.trailingAnchor),
            previewView.topAnchor.constraint(equalTo: content.topAnchor),
            previewView.bottomAnchor.constraint(equalTo: content.bottomAnchor),
            statusLabel.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 12),
            statusLabel.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -12),
            statusLabel.bottomAnchor.constraint(equalTo: content.bottomAnchor, constant: -12),
            statusLabel.heightAnchor.constraint(greaterThanOrEqualToConstant: 30),
        ])

        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 520, height: 820),
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered,
            defer: false
        )
        window.title = "iPhone Screen Bridge"
        window.contentView = content
        window.center()
        window.makeKeyAndOrderFront(nil)
        self.window = window
        NSApp.activate(ignoringOtherApps: true)
    }

    private func requestVideoPermissionAndStart() {
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized:
            enableAndDiscoverScreenDevices()
        case .notDetermined:
            updateStatus("等待 macOS 相机权限…")
            AVCaptureDevice.requestAccess(for: .video) { [weak self] granted in
                DispatchQueue.main.async {
                    if granted {
                        self?.enableAndDiscoverScreenDevices()
                    } else {
                        self?.updateStatus("未获得相机权限，请在系统设置中允许后重启")
                    }
                }
            }
        case .denied, .restricted:
            updateStatus("相机权限被拒绝，请在系统设置 → 隐私与安全性 → 相机中允许")
        @unknown default:
            updateStatus("未知的相机权限状态")
        }
    }

    private func enableAndDiscoverScreenDevices() {
        var address = CMIOObjectPropertyAddress(
            mSelector: CMIOObjectPropertySelector(kCMIOHardwarePropertyAllowScreenCaptureDevices),
            mScope: CMIOObjectPropertyScope(kCMIOObjectPropertyScopeGlobal),
            mElement: CMIOObjectPropertyElement(kCMIOObjectPropertyElementMain)
        )
        var allow: UInt32 = 1
        let status = CMIOObjectSetPropertyData(
            CMIOObjectID(kCMIOObjectSystemObject),
            &address,
            0,
            nil,
            UInt32(MemoryLayout<UInt32>.size),
            &allow
        )
        guard status == noErr else {
            updateStatus("CoreMediaIO 启用失败：\(status)")
            return
        }

        updateStatus("正在等待 USB iPhone 屏幕设备…")
        discoverAndStartIfAvailable()
        discoveryTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            self?.discoverAndStartIfAvailable()
        }
    }

    private func discoverAndStartIfAvailable() {
        guard !session.isRunning else {
            discoveryTimer?.invalidate()
            discoveryTimer = nil
            return
        }

        let devices = AVCaptureDevice.DiscoverySession(
            deviceTypes: [.external],
            mediaType: .muxed,
            position: .unspecified
        ).devices
        guard let device = devices.first else {
            return
        }

        discoveryTimer?.invalidate()
        discoveryTimer = nil
        do {
            try configureSession(device: device)
        } catch {
            updateStatus("建立捕获会话失败：\(error.localizedDescription)")
        }
    }

    private func configureSession(device: AVCaptureDevice) throws {
        let input = try AVCaptureDeviceInput(device: device)
        let output = AVCaptureVideoDataOutput()
        output.alwaysDiscardsLateVideoFrames = true
        output.videoSettings = [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA
        ]
        output.setSampleBufferDelegate(self, queue: captureQueue)

        session.beginConfiguration()
        session.sessionPreset = .high
        guard session.canAddInput(input) else {
            session.commitConfiguration()
            throw BridgeError.cannotAddInput
        }
        session.addInput(input)
        guard session.canAddOutput(output) else {
            session.removeInput(input)
            session.commitConfiguration()
            throw BridgeError.cannotAddOutput
        }
        session.addOutput(output)
        session.commitConfiguration()

        previewView.previewLayer.session = session
        updateStatus("已连接 \(device.localizedName)，等待第一帧…")
        captureQueue.async { [weak self] in
            self?.session.startRunning()
        }
    }

    func captureOutput(
        _ output: AVCaptureOutput,
        didOutput sampleBuffer: CMSampleBuffer,
        from connection: AVCaptureConnection
    ) {
        guard let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else {
            return
        }

        frameCount += 1
        if startedAt == nil {
            startedAt = .now
        }

        let width = CVPixelBufferGetWidth(pixelBuffer)
        let height = CVPixelBufferGetHeight(pixelBuffer)
        if !firstFrameWritten {
            firstFrameWritten = true
            writeFirstFrame(pixelBuffer)
        }

        if frameCount == 1 || frameCount.isMultiple(of: 30) {
            let elapsed = startedAt.map {
                let components = $0.duration(to: .now).components
                return Double(components.seconds) + Double(components.attoseconds) / 1e18
            } ?? 0
            let fps = elapsed > 0 ? Double(frameCount) / elapsed : 0
            let saved = firstFrameURL?.path ?? "正在保存首帧"
            DispatchQueue.main.async { [weak self] in
                self?.updateStatus(String(format: "%dx%d · %.1f fps · %@", width, height, fps, saved))
            }
        }
    }

    private func writeFirstFrame(_ pixelBuffer: CVPixelBuffer) {
        let image = CIImage(cvPixelBuffer: pixelBuffer)
        guard let cgImage = ciContext.createCGImage(image, from: image.extent) else {
            return
        }
        let bitmap = NSBitmapImageRep(cgImage: cgImage)
        guard let png = bitmap.representation(using: .png, properties: [:]) else {
            return
        }

        do {
            let bundleURL = Bundle.main.bundleURL
            let root: URL
            if bundleURL.pathExtension == "app" {
                root = bundleURL
                    .deletingLastPathComponent() // dist
                    .deletingLastPathComponent() // project root
            } else {
                root = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
            }
            let directory = root.appendingPathComponent("captures", isDirectory: true)
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            let url = directory.appendingPathComponent("first-frame.png")
            try png.write(to: url, options: .atomic)
            firstFrameURL = url
            print("FIRST_FRAME=\(url.path)")
            fflush(stdout)
        } catch {
            print("FIRST_FRAME_ERROR=\(error.localizedDescription)")
            fflush(stdout)
        }
    }

    private func updateStatus(_ text: String) {
        if Thread.isMainThread {
            statusLabel.stringValue = text
        } else {
            DispatchQueue.main.async { [weak self] in
                self?.statusLabel.stringValue = text
            }
        }
        print("STATUS=\(text)")
        fflush(stdout)
    }
}

private enum BridgeError: LocalizedError {
    case cannotAddInput
    case cannotAddOutput

    var errorDescription: String? {
        switch self {
        case .cannotAddInput:
            "AVCaptureSession 无法添加 iPhone 输入"
        case .cannotAddOutput:
            "AVCaptureSession 无法添加视频输出"
        }
    }
}

let app = NSApplication.shared
private let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
