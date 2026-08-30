#ifdef _WIN32

#include "valeria/QuickTimeSession.hpp"
#include "valeria/WinUsbTransport.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace valeria;

std::atomic_bool gStopRequested{false};

BOOL WINAPI consoleHandler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT ||
        event == CTRL_SHUTDOWN_EVENT) {
        gStopRequested.store(true, std::memory_order_relaxed);
        return TRUE;
    }
    return FALSE;
}

std::wstring wide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) {
        throw std::runtime_error("argument is not valid UTF-8");
    }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), count);
    return result;
}

std::string windowsError(const char* operation, DWORD error = GetLastError()) {
    std::ostringstream stream;
    stream << operation << " failed with Windows error " << error;
    return stream.str();
}

std::optional<std::wstring> findFfplayOnPath() {
    const DWORD required = SearchPathW(nullptr, L"ffplay.exe", nullptr, 0, nullptr, nullptr);
    if (required == 0) {
        return std::nullopt;
    }
    std::vector<wchar_t> buffer(required + 1, L'\0');
    if (SearchPathW(nullptr, L"ffplay.exe", nullptr,
                    static_cast<DWORD>(buffer.size()), buffer.data(), nullptr) == 0) {
        return std::nullopt;
    }
    return std::wstring(buffer.data());
}

class FfplayVideoPipe {
public:
    ~FfplayVideoPipe() { stop(); }

    void configure(std::wstring executable) { executable_ = std::move(executable); }

    void start(VideoCodec codec, const Bytes& parameterSets) {
        if (running_ && codec_ == codec) {
            std::lock_guard<std::mutex> lock(mutex_);
            parameterSets_ = parameterSets;
            queue_.clear();
            queuedBytes_ = 0;
            needsKeyFrame_ = true;
            return;
        }
        stop();
        if (executable_.empty()) {
            throw std::runtime_error("ffplay executable was not configured");
        }
        if (codec != VideoCodec::H264 && codec != VideoCodec::HEVC) {
            throw std::runtime_error("ffplay cannot start for an unknown codec");
        }

        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(attributes);
        attributes.bInheritHandle = TRUE;
        HANDLE childRead = INVALID_HANDLE_VALUE;
        if (!CreatePipe(&childRead, &writePipe_, &attributes, 1024U * 1024U)) {
            throw std::runtime_error(windowsError("CreatePipe(ffplay stdin)"));
        }
        if (!SetHandleInformation(writePipe_, HANDLE_FLAG_INHERIT, 0)) {
            CloseHandle(childRead);
            CloseHandle(writePipe_);
            writePipe_ = INVALID_HANDLE_VALUE;
            throw std::runtime_error(windowsError("SetHandleInformation(ffplay stdin)"));
        }

        const wchar_t* demuxer = codec == VideoCodec::H264 ? L"h264" : L"hevc";
        std::wstring command = L"\"" + executable_ +
            L"\" -hide_banner -loglevel warning -fflags nobuffer -flags low_delay "
            L"-framedrop -probesize 32 -analyzeduration 0 -sync video -f ";
        command += demuxer;
        command += L" -i pipe:0 -an -window_title \"iPhone USB Screen Mirror\"";

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = childRead;
        startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION process{};
        const BOOL created = CreateProcessW(executable_.c_str(), command.data(), nullptr,
                                            nullptr, TRUE, CREATE_NEW_PROCESS_GROUP, nullptr,
                                            nullptr, &startup, &process);
        const DWORD error = created ? ERROR_SUCCESS : GetLastError();
        CloseHandle(childRead);
        if (!created) {
            CloseHandle(writePipe_);
            writePipe_ = INVALID_HANDLE_VALUE;
            throw std::runtime_error(windowsError("CreateProcess(ffplay)", error));
        }
        process_ = process.hProcess;
        CloseHandle(process.hThread);
        codec_ = codec;
        parameterSets_ = parameterSets;
        queue_.clear();
        queuedBytes_ = 0;
        writerError_.clear();
        stopping_ = false;
        needsKeyFrame_ = false;
        running_ = true;
        if (!parameterSets_.empty()) {
            enqueueLocked(parameterSets_);
        }
        const HANDLE writerPipe = writePipe_;
        writer_ = std::thread([this, writerPipe] { writerLoop(writerPipe); });
    }

    // The protocol thread only enqueues. A slow renderer can never block NEED
    // flow; overflow drops until the next independently decodable key frame.
    void submit(const VideoSample& sample) {
        if (!running_ || sample.annexB.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!writerError_.empty()) {
            throw std::runtime_error(writerError_);
        }
        if (needsKeyFrame_ && !sample.keyFrame) {
            return;
        }
        if (sample.keyFrame) {
            needsKeyFrame_ = false;
            if (!parameterSets_.empty()) {
                enqueueLocked(parameterSets_);
            }
        }
        if (queuedBytes_ + sample.annexB.size() > kMaximumQueuedBytes) {
            queue_.clear();
            queuedBytes_ = 0;
            needsKeyFrame_ = true;
            if (!sample.keyFrame) {
                return;
            }
            needsKeyFrame_ = false;
            if (!parameterSets_.empty()) {
                enqueueLocked(parameterSets_);
            }
        }
        enqueueLocked(sample.annexB);
        condition_.notify_one();
    }

    void stop() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            queue_.clear();
            queuedBytes_ = 0;
        }
        condition_.notify_all();
        if (writer_.joinable()) {
            CancelSynchronousIo(writer_.native_handle());
        }
        if (writer_.joinable()) {
            writer_.join();
        }
        HANDLE pipe = writePipe_;
        writePipe_ = INVALID_HANDLE_VALUE;
        if (pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe);
        }
        if (process_) {
            if (WaitForSingleObject(process_, 1500) == WAIT_TIMEOUT) {
                TerminateProcess(process_, 0);
                WaitForSingleObject(process_, 1000);
            }
            CloseHandle(process_);
            process_ = nullptr;
        }
        queue_.clear();
        queuedBytes_ = 0;
        parameterSets_.clear();
        writerError_.clear();
        stopping_ = false;
        needsKeyFrame_ = false;
        running_ = false;
        codec_ = VideoCodec::Unknown;
    }

private:
    static constexpr std::size_t kMaximumQueuedBytes = 16U * 1024U * 1024U;

    void enqueueLocked(const Bytes& bytes) {
        if (bytes.empty()) {
            return;
        }
        queue_.push_back(bytes);
        queuedBytes_ += bytes.size();
    }

    void writerLoop(HANDLE pipe) noexcept {
        for (;;) {
            Bytes bytes;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
                if (stopping_) {
                    return;
                }
                bytes = std::move(queue_.front());
                queue_.pop_front();
                queuedBytes_ -= bytes.size();
            }

            std::size_t position = 0;
            while (position < bytes.size()) {
                const DWORD requested = static_cast<DWORD>(
                    std::min<std::size_t>(bytes.size() - position, MAXDWORD));
                DWORD written = 0;
                if (pipe == INVALID_HANDLE_VALUE ||
                    !WriteFile(pipe, bytes.data() + position, requested, &written, nullptr) ||
                    written == 0) {
                    const DWORD error = GetLastError();
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!stopping_) {
                        writerError_ = windowsError(
                            "writing continuous video to ffplay (did the window close?)",
                            error);
                    }
                    return;
                }
                position += written;
            }
        }
    }

    std::wstring executable_;
    HANDLE writePipe_ = INVALID_HANDLE_VALUE;
    HANDLE process_ = nullptr;
    VideoCodec codec_ = VideoCodec::Unknown;
    Bytes parameterSets_;
    std::deque<Bytes> queue_;
    std::size_t queuedBytes_ = 0;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::thread writer_;
    std::string writerError_;
    bool stopping_ = false;
    bool needsKeyFrame_ = false;
    bool running_ = false;
};

struct Options {
    std::string deviceSelector;
    std::optional<std::wstring> ffplay;
    std::string videoDump;
    std::string audioDump;
    std::uint32_t width = 1920;
    std::uint32_t height = 1200;
    bool advertiseHevc = true;
    bool diagnosticOnly = false;
};

void usage() {
    std::cout
        << "iphone-valeria-mirror [options]\n\n"
        << "  --device <UDID-fragment>   Select one connected iPhone\n"
        << "  --ffplay <path>            ffplay.exe used for the live mirror window\n"
        << "  --size <width>x<height>    Requested host display size (default 1920x1200)\n"
        << "  --force-h264               Keep the HEVC capability key but set it false\n"
        << "  --video-dump <path>        Optional Annex-B diagnostic dump\n"
        << "  --audio-dump <path>        Optional LPCM diagnostic dump\n"
        << "  --diagnostic-only          Do not open a live window (not mirror success)\n"
        << "  --help                     Show this text\n\n"
        << "Without --ffplay the program locates ffplay.exe on PATH. A live preview is\n"
        << "required unless --diagnostic-only is explicit. No screenshot API is used.\n";
}

std::uint32_t parsePositive(const std::string& value, const char* name) {
    std::size_t consumed = 0;
    const unsigned long parsed = std::stoul(value, &consumed, 10);
    if (consumed != value.size() || parsed == 0 || parsed > 16384) {
        throw std::runtime_error(std::string("invalid ") + name);
    }
    return static_cast<std::uint32_t>(parsed);
}

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        auto value = [&](const char* option) -> std::string {
            if (++index >= argc) {
                throw std::runtime_error(std::string(option) + " needs a value");
            }
            return argv[index];
        };
        if (argument == "--help") {
            usage();
            std::exit(0);
        } else if (argument == "--device") {
            options.deviceSelector = value("--device");
        } else if (argument == "--ffplay") {
            options.ffplay = wide(value("--ffplay"));
        } else if (argument == "--video-dump") {
            options.videoDump = value("--video-dump");
        } else if (argument == "--audio-dump") {
            options.audioDump = value("--audio-dump");
        } else if (argument == "--size") {
            const std::string dimensions = value("--size");
            const std::size_t separator = dimensions.find_first_of("xX");
            if (separator == std::string::npos) {
                throw std::runtime_error("--size must look like 1920x1200");
            }
            options.width = parsePositive(dimensions.substr(0, separator), "width");
            options.height = parsePositive(dimensions.substr(separator + 1), "height");
        } else if (argument == "--force-h264") {
            options.advertiseHevc = false;
        } else if (argument == "--diagnostic-only") {
            options.diagnosticOnly = true;
        } else {
            throw std::runtime_error("unknown option: " + argument);
        }
    }
    if (!options.diagnosticOnly && !options.ffplay) {
        options.ffplay = findFfplayOnPath();
        if (!options.ffplay) {
            throw std::runtime_error(
                "ffplay.exe was not found; install FFmpeg or pass --ffplay <path>");
        }
    }
    return options;
}

class MirrorSink {
public:
    explicit MirrorSink(const Options& options)
        : diagnosticOnly_(options.diagnosticOnly), started_(std::chrono::steady_clock::now()),
          lastReport_(started_) {
        if (options.ffplay) {
            viewer_.configure(*options.ffplay);
        }
        if (!options.videoDump.empty()) {
            videoDump_.open(options.videoDump, std::ios::binary | std::ios::trunc);
            if (!videoDump_) {
                throw std::runtime_error("cannot open video dump: " + options.videoDump);
            }
        }
        if (!options.audioDump.empty()) {
            audioDump_.open(options.audioDump, std::ios::binary | std::ios::trunc);
            if (!audioDump_) {
                throw std::runtime_error("cannot open audio dump: " + options.audioDump);
            }
        }
    }

    void onVideoFormat(const VideoFormat& format) {
        format_ = format;
        std::cout << "[FORMAT] video=" << codecName(format.codec) << ' '
                  << format.width << 'x' << format.height
                  << " nalLength=" << static_cast<unsigned>(format.nalLengthSize)
                  << " parameterSets=" << format.parameterSets.size() << '\n';
        if (!diagnosticOnly_) {
            viewer_.start(format.codec, format.annexBParameterSets);
            std::cout << "[MIRROR] ffplay live window started\n";
        }
        if (videoDump_) {
            videoDump_.write(reinterpret_cast<const char*>(format.annexBParameterSets.data()),
                             static_cast<std::streamsize>(format.annexBParameterSets.size()));
        }
    }

    void onVideoSample(const VideoSample& sample) {
        const auto now = std::chrono::steady_clock::now();
        if (frames_ == 0) {
            firstFrame_ = now;
            const double latency = std::chrono::duration<double>(now - started_).count();
            std::cout << std::fixed << std::setprecision(3)
                      << "[FIRST_FRAME] codec=" << codecName(sample.codec)
                      << " acquisitionSeconds=" << latency
                      << " bytes=" << sample.annexB.size() << '\n';
        }

        if (!diagnosticOnly_) {
            viewer_.submit(sample);
        }
        if (videoDump_) {
            videoDump_.write(reinterpret_cast<const char*>(sample.annexB.data()),
                             static_cast<std::streamsize>(sample.annexB.size()));
        }
        ++frames_;
        ++intervalFrames_;
        videoBytes_ += sample.annexB.size();

        if (now - lastReport_ >= std::chrono::seconds(1)) {
            const double interval = std::chrono::duration<double>(now - lastReport_).count();
            const double duration = std::chrono::duration<double>(now - *firstFrame_).count();
            std::cout << std::fixed << std::setprecision(1)
                      << "[LIVE] fps=" << intervalFrames_ / interval
                      << " frames=" << frames_ << " duration=" << duration << "s"
                      << " video=" << (videoBytes_ / (1024.0 * 1024.0)) << "MiB"
                      << " audio=" << (audioBytes_ / (1024.0 * 1024.0)) << "MiB\n";
            intervalFrames_ = 0;
            lastReport_ = now;
        }
    }

    void onAudioFormat(const AudioFormat& format) {
        std::cout << "[FORMAT] audio=" << fourcc(format.formatId) << ' '
                  << format.sampleRate << "Hz " << format.channelsPerFrame << "ch "
                  << format.bitsPerChannel << "bit flags=" << format.formatFlags << '\n';
        if (format.formatId != cm::kLpcm) {
            std::cerr << "[WARN] non-LPCM EAT stream is exposed through callbacks but is not "
                         "decoded by this POC\n";
        }
    }

    void onAudioSample(const AudioSample& sample) {
        audioBytes_ += sample.pcm.size();
        if (audioDump_) {
            audioDump_.write(reinterpret_cast<const char*>(sample.pcm.data()),
                             static_cast<std::streamsize>(sample.pcm.size()));
        }
    }

    void finish() {
        viewer_.stop();
        const auto end = std::chrono::steady_clock::now();
        const double duration = firstFrame_
            ? std::chrono::duration<double>(end - *firstFrame_).count()
            : 0.0;
        const double averageFps = duration > 0.0 ? frames_ / duration : 0.0;
        std::cout << std::fixed << std::setprecision(2)
                  << "[FINAL] firstFrame=" << (firstFrame_ ? "yes" : "no")
                  << " frames=" << frames_ << " duration=" << duration
                  << "s averageFps=" << averageFps << '\n';
    }

private:
    bool diagnosticOnly_ = false;
    FfplayVideoPipe viewer_;
    std::ofstream videoDump_;
    std::ofstream audioDump_;
    std::optional<VideoFormat> format_;
    std::chrono::steady_clock::time_point started_;
    std::chrono::steady_clock::time_point lastReport_;
    std::optional<std::chrono::steady_clock::time_point> firstFrame_;
    std::uint64_t frames_ = 0;
    std::uint64_t intervalFrames_ = 0;
    std::uint64_t videoBytes_ = 0;
    std::uint64_t audioBytes_ = 0;
};

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (options.diagnosticOnly) {
            std::cerr << "[DIAGNOSTIC] preview disabled explicitly; dumps/counters alone do "
                         "not count as screen-mirror success\n";
        }
        SetConsoleCtrlHandler(consoleHandler, TRUE);

        WinUsbTransport transport;
        std::cout << "[USB] opening Apple MI_02; activation uses official AppleLowerFilter "
                     "mode 2 and descriptor verification\n";
        transport.open(options.deviceSelector);
        const UsbInterfaceInfo& usb = transport.interfaceInfo();
        std::cout << "[USB] Valeria FF/2A/FF interface="
                  << static_cast<unsigned>(usb.interfaceNumber) << " alt="
                  << static_cast<unsigned>(usb.alternateSetting) << " bulkIn=0x"
                  << std::hex << static_cast<unsigned>(usb.bulkIn) << " bulkOut=0x"
                  << static_cast<unsigned>(usb.bulkOut) << std::dec << '\n';

        MirrorSink sink(options);
        SessionCallbacks callbacks;
        callbacks.onVideoFormat = [&](const VideoFormat& format) { sink.onVideoFormat(format); };
        callbacks.onVideoSample = [&](const VideoSample& sample) { sink.onVideoSample(sample); };
        callbacks.onAudioFormat = [&](const AudioFormat& format) { sink.onAudioFormat(format); };
        callbacks.onAudioSample = [&](const AudioSample& sample) { sink.onAudioSample(sample); };
        callbacks.onLog = [](const std::string& message) {
            std::cout << "[PROTO] " << message << '\n';
        };

        SessionOptions sessionOptions;
        sessionOptions.requestedWidth = options.width;
        sessionOptions.requestedHeight = options.height;
        sessionOptions.advertiseHevc = options.advertiseHevc;
        QuickTimeSession session(transport, sessionOptions, std::move(callbacks));
        std::cout << "[MIRROR] press Ctrl+C to stop\n";
        try {
            session.run(gStopRequested);
        } catch (...) {
            session.closeSession();
            sink.finish();
            transport.close();
            throw;
        }
        session.closeSession();
        sink.finish();
        transport.close();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] " << error.what() << '\n';
        return 1;
    }
}

#endif
