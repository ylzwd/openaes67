// OpenAES67 CLI
//
//   info   print resolved stream parameters
//   sdp    print the SDP for a stream
//   tx     send over AES67 (L16) -- a test tone, or the contents of a WAV file
//   rx     receive AES67 (L16) and write a WAV file
//
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

// timeBeginPeriod / timeEndPeriod live in mmsystem.h, which windows.h only
// includes without WIN32_LEAN_AND_MEAN. The CMake build defines it, the batch
// build does not, so the include is explicit here.
#include <windows.h>
#include <mmsystem.h>
#include <shellapi.h>   // CommandLineToArgvW (WIN32_LEAN_AND_MEAN hides it)

#include "openaes67/config.h"
#include "openaes67/engine.h"
#include "openaes67/net.h"
#include "openaes67/ptp.h"
#include "openaes67/receive_loop.h"
#include "openaes67/shared_audio_ring.h"
#include "openaes67/audio_devices.h"
#include "openaes67/asio_probe.h"
#include "openaes67/bench.h"
#include "openaes67/bridge.h"
#include "openaes67/cli_config.h"
#include "openaes67/version.h"
#include "openaes67/doctor.h"
#include "openaes67/rtp_probe.h"
#include "openaes67/text_encoding.h"
#include "openaes67/sap.h"
#include "openaes67/resampler.h"
#include "openaes67/rtp.h"
#include "openaes67/sdp.h"
#include "openaes67/audio_io.h"
#include "openaes67/wav.h"
#include <objbase.h>

using namespace openaes67;

#ifndef OPENAES67_VERSION
#define OPENAES67_VERSION "0.0.0-dev"
#endif
static const char *kVersion = OPENAES67_VERSION;

static void usage() {
    std::printf(
        "OpenAES67 " OPENAES67_VERSION "\n"
        "  AES67 / Dante-compatible audio for Windows\n"
        "\n"
        "usage:\n"
        "  openaes67 <command> [options]\n"
        "\n"
        "commands:\n"
        "  doctor         check the environment: interface, multicast, SAP, ASIO, PTP\n"
        "  probe          listen on an address and report the format it carries\n"
        "  info           show the effective stream configuration\n"
        "  sdp            print the SDP this configuration would announce\n"
        "  tx             send a tone or a WAV file (no audio device needed)\n"
        "  rx             receive to a WAV file (no audio device needed)\n"
        "  send           WASAPI loopback -> AES67\n"
        "  recv           AES67 -> render device\n"
        "  bridge         both directions through a shared ring (sound-card mode)\n"
        "  capture        WASAPI loopback -> WAV (diagnostic)\n"
        "  capture-in     WASAPI input endpoint -> WAV (diagnostic)\n"
        "  play           WAV -> default render device (diagnostic)\n"
        "  sap-listen     list the streams announced over SAP\n"
        "  ptp            measure PTPv2 offset against a grandmaster\n"
        "  shm-read       drain a shared section and report what arrived\n"
        "  list-devices   list audio outputs (WASAPI endpoints and ASIO drivers)\n"
        "  bench          measure the hot paths; with --baseline, gate on them\n"
        "  version        print the version and exit\n"
        "  help           this text\n"
        "\n"
        "common options:\n"
        "  --iface IP       local IPv4 of the wired NIC (default 0.0.0.0)\n"
        "  --group G        multicast group (default 239.69.1.1)\n"
        "  --port P         UDP port (default 5004)\n"
        "  --channels N     channel count (default 2, max 64)\n"
        "  --rate HZ        sample rate (default 48000)\n"
        "  --ptime MS       packet time in ms (default 1.0)\n"
        "  --codec NAME     L16 (default) or L24\n"
        "  --ttl N          multicast TTL (default 32)\n"
        "  --ssrc HEX       RTP SSRC (default: random per run, per RFC 3550)\n"
        "  --rtcp           send/receive RTCP reports on port+1\n"
        "  --config FILE    read defaults from a key = value file; flags still win\n"
        "  --sap            announce the stream over SAP\n"
        "  --sap-interval N base announcement interval in seconds (default 30)\n"
        "  --refclk SPEC    ptp:<gmid>[:<domain>] or local (ts-refclk for the SDP)\n"
        "  --sdp-out FILE   also write the SDP to a file\n"
        "  --print-sdp      print the SDP before sending\n"
        "\n"
        "tx / send options:\n"
        "  --wav FILE       send this WAV file (adopts its channels and rate)\n"
        "  --freq HZ        tone frequency (default 1000)\n"
        "  --amplitude N    tone amplitude (default 20000)\n"
        "  --seconds N      tone duration (default 6); ignored with --wav\n"
        "  --pace-ppm N     pace packets N ppm faster (+) or slower (-)\n"
        "  --ptp-lock       derive RTP timestamps from the grandmaster\n"
        "  --domain N       PTP domain used with --ptp / --ptp-lock (default 0)\n"
        "\n"
        "rx options:\n"
        "  --seconds N      capture window (default 6)\n"
        "  --out PATH       WAV output path (default rx.wav)\n"
        "  --out-bits N     WAV bit depth, 16 (default) or 24\n"
        "  --sdp-file FILE  derive group, port and format from an SDP file\n"
        "\n"
        "recv / bridge options:\n"
        "  --device NAME    output device by name or by index from list-devices\n"
        "  --exclusive      WASAPI exclusive mode (bypasses the audio engine)\n"
        "  --backend NAME   wasapi (default) or asio\n"
        "  --asio-driver ID ASIO registry key, implies --backend asio\n"
        "  --ptp            discipline the rate controller from PTP\n"
        "  --shm NAME       write into (recv) or read from (send) a shared section\n"
        "  --in-device NAME send: capture device by name or index\n"
        "  --send-group G   bridge: uplink multicast group\n"
        "  --send-port P    bridge: uplink port (default: receive port + 2)\n"
        "  --discover       bridge: find the downlink over SAP\n"
        "  --discover-name S  bridge: only accept names containing this\n"
        "  --discover-seconds N  bridge: how long to listen (default 4)\n"
        "\n"
        "probe options:\n"
        "  --seconds N      listen window (default 6)\n"
        "\n"
        "bench options:\n"
        "  --baseline FILE  compare against a recorded baseline (fails on regression)\n"
        "  --write-baseline write the baseline instead of comparing\n"
        "\n"
        "examples:\n"
        "  openaes67 doctor --iface 192.168.1.10\n"
        "  openaes67 tx --iface 192.168.1.10 --channels 8 --sap\n"
        "  openaes67 recv --iface 192.168.1.10 --device \x22Speakers\x22 --ptp\n"
        "  openaes67 bridge --iface 192.168.1.10 --channels 64 --shm OpenAES67Audio\n"
        "  openaes67 probe --iface 192.168.1.10 --group 239.69.1.1 --port 5004\n"
        "\n"
        "In --wav mode the whole file is sent; --seconds is ignored.\n");
}

// SDP is a CRLF protocol, and Windows stdout is in text mode: printf("%s", sdp)
// turns every "\n" into "\r\n", so a body that already uses CRLF becomes
// "\r\r\n". A strict parser rejects that, and `openaes67 sdp > file` is the
// documented way to get an SDP for `recv --sdp-file`. Write it byte-for-byte.
static void printSdpBytes(const std::string &sdp) {
    const int fd = _fileno(stdout);
    const int previous = _setmode(fd, _O_BINARY);
    std::fwrite(sdp.data(), 1, sdp.size(), stdout);
    std::fflush(stdout);
    if (previous != -1) {
        _setmode(fd, previous);
    }
}

static bool optEq(const char *a, const char *b) { return std::strcmp(a, b) == 0; }

// AudioSource over the capture side of a shared section: the ASIO driver writes
// the host's output there, and this is what puts it on the network. Keeping it an
// AudioSource means the send loop is identical for loopback and for the ring.
// Wide-to-UTF-8 for logs. Constructing std::string(name.begin(), name.end())
// from a wstring compiles but truncates every character (C4244).
std::string toUtf8(const std::wstring &w) {
    if (w.empty()) {
        return std::string();
    }
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                      static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string s(static_cast<size_t>(n), 0);
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n,
                          nullptr, nullptr);
    return s;
}

class SharedCaptureSource : public AudioSource {
public:
    explicit SharedCaptureSource(std::unique_ptr<SharedAudioRing> ring)
        : ring_(std::move(ring)) {}

    int read(int16_t *dst, int frames) override {
        return ring_ ? ring_->readCapture(dst, frames) : 0;
    }
    int channels() const override { return ring_ ? ring_->channels() : 0; }
    int sampleRate() const override { return ring_ ? ring_->sampleRate() : 0; }
    const char *backendName() const override { return "shm-capture"; }

private:
    std::unique_ptr<SharedAudioRing> ring_;
};

int main(int argc, char **argv) {
    // Windows passes ANSI arguments in argv, so a Chinese device name arrives as
    // GBK bytes and never matches the UTF-16 name from WASAPI. Rebuild the argument
    // vector from the UTF-16 command line instead.
    std::vector<std::string> argStore;
    std::vector<char *> argPtrs;
    {
        int wideCount = 0;
        LPWSTR *wide = ::CommandLineToArgvW(::GetCommandLineW(), &wideCount);
        if (wide) {
            argStore.reserve(static_cast<size_t>(wideCount));
            for (int i = 0; i < wideCount; ++i) {
                const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0,
                                                       nullptr, nullptr);
                std::string s(bytes > 1 ? bytes - 1 : 0, char(0));
                if (bytes > 1) {
                    ::WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, &s[0], bytes, nullptr,
                                          nullptr);
                }
                argStore.push_back(s);
            }
            ::LocalFree(wide);
            argPtrs.reserve(argStore.size() + 1);
            for (auto &s : argStore) {
                argPtrs.push_back(&s[0]);
            }
            argPtrs.push_back(nullptr);
            argc = static_cast<int>(argStore.size());
            argv = argPtrs.data();
        }
    }
    // Windows defaults to a 15.6 ms timer tick: every sleep_until() in the 1 ms
    // send loop overshot by up to that much, so packets left in bursts of ~15.
    // Measured p95 inter-packet gap was 14.7 ms for a 1 ms stream.
    timeBeginPeriod(1);
    struct TimerGuard {
        ~TimerGuard() { timeEndPeriod(1); }
    } timerGuard;
    // Unbuffered stdout so redirected logs show progress immediately.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    if (argc < 2) {
        usage();
        return 1;
    }

    const std::string cmd = argv[1];
    if (cmd == "-h" || cmd == "--help" || cmd == "help") {
        usage();
        return 0;
    }
    if (cmd == "-v" || cmd == "--version" || cmd == "version") {
        std::printf("OpenAES67 %s\n", kVersion);
        return 0;
    }

    StreamConfig cfg;
    cfg.ifaceIp = "0.0.0.0";
    uint32_t ssrc = 0;   // 0 = generate a random SSRC for this session
    double seconds = 6.0;
    double freq = 1000.0;
    int amplitude = 20000;
    std::string outPath = "rx.wav";
int outBits = 16;                // rx: WAV bit depth of the capture (16 or 24)
    std::string inWav;
    bool exclusive = false;
    int ptpDomain = 0;
    double pacePpm = 0.0;      // tx: send faster (+) or slower (-) than realtime
    bool   usePtp = false;     // recv: feed the PTP rate estimate to the controller
    std::wstring shmName;      // recv: write into this shared section instead of a device
std::string  sendGroup;        // bridge: uplink multicast group
uint16_t     sendPort = 0;      // bridge: uplink port (0 = recv port + 2)
bool         discoverStream = false;   // bridge: find the downlink over SAP
std::string  discoverName;           // bridge: only match names containing this
double       discoverSeconds = 4.0;  // bridge: how long to listen
    std::string  deviceName;   // recv: audio output by name or index ("" = default)
    std::string  inDeviceName; // tx/send: capture source by name or index ("" = default)
    bool         ptpLock = false;      // send: derive RTP timestamps from PTP
    std::string  backend;      // recv: "wasapi" (default) or "asio"
    std::string  asioDriver;   // recv: ASIO registry key when backend is asio
    std::string  refclk;       // sdp/tx: "ptp:<gmid>[:<domain>]" or "local"
    std::string  sdpOutPath;   // sdp/tx: also write the SDP to this file
std::string  sdpInPath;    // recv: derive the stream format from this SDP file
    bool         printSdp = false;  // tx: print the SDP before sending
    bool         useSap = false;    // tx: announce the stream over SAP
    int          sapBaseInterval = 30;  // SAP announcement base interval, seconds
    std::string  benchBaselinePath;     // bench: where to read/write the baseline
    bool         benchWriteBaseline = false;
    // Whether --group / --port were given explicitly: an SDP file supplies them
    // otherwise, but must never silently override the command line.
    bool groupGiven = false;
    bool portGiven = false;

    // --config FILE fills the defaults first; the loop below then applies the
    // command line, so an explicit flag always wins over the file.
    {
        std::string configPath;
        for (int i = 2; i < argc; ++i) {
            if (optEq(argv[i], "--config") && i + 1 < argc) {
                configPath = argv[i + 1];
                break;
            }
        }
        if (!configPath.empty()) {
            CliOptions fromFile;
            if (!applyConfigFile(configPath.c_str(), &fromFile, &openFileUtf8)) {
                return 2;
            }
            cfg = fromFile.cfg;
            seconds = fromFile.seconds;
            ptpDomain = fromFile.ptpDomain;
            usePtp = fromFile.usePtp;
            ptpLock = fromFile.ptpLock;
            useSap = fromFile.useSap;
            sapBaseInterval = fromFile.sapInterval;
            shmName = fromFile.shmName;
            sendGroup = fromFile.sendGroup;
            sendPort = fromFile.sendPort;
            discoverStream = fromFile.discover;
            discoverName = fromFile.discoverName;
            discoverSeconds = fromFile.discoverSeconds;
            refclk = fromFile.refclk;
            deviceName = fromFile.deviceName;
            inDeviceName = fromFile.inDeviceName;
            backend = fromFile.backend;
            asioDriver = fromFile.asioDriver;
            outPath = fromFile.outPath;
            outBits = fromFile.outBits;
            sdpOutPath = fromFile.sdpOutPath;
            sdpInPath = fromFile.sdpInPath;
        }
    }

    for (int i = 2; i < argc; ++i) {

        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : nullptr;
        if (optEq(a, "--iface") && v)          { cfg.ifaceIp = v; ++i; }
        else if (optEq(a, "--config") && v)        { ++i; }   // read in the pre-pass
        else if (optEq(a, "--group") && v)     { cfg.group = v; ++i; }
        else if (optEq(a, "--group") && v)     { cfg.group = v; groupGiven = true; ++i; }
        else if (optEq(a, "--send-group") && v) { sendGroup = v; ++i; }
        else if (optEq(a, "--send-port") && v)  { sendPort = static_cast<uint16_t>(std::atoi(v)); ++i; }
        else if (optEq(a, "--discover"))        { discoverStream = true; }
        else if (optEq(a, "--discover-name") && v) { discoverName = v; ++i; }
        else if (optEq(a, "--discover-seconds") && v) { discoverSeconds = std::atof(v); ++i; }
        else if (optEq(a, "--port") && v) {
            const long parsedPort = std::strtol(v, nullptr, 10);
            if (parsedPort < 1 || parsedPort > 65535) {
                std::fprintf(stderr, "invalid port %s (expected 1..65535)\n", v);
                return 1;
            }
            portGiven = true; cfg.port = static_cast<uint16_t>(parsedPort);
            ++i;
        }
        else if (optEq(a, "--channels") && v)  { cfg.channels = std::atoi(v); ++i; }
        else if (optEq(a, "--codec") && v) {
            Codec c;
            if (!parseCodecName(v, &c)) {
                std::fprintf(stderr, "invalid codec '%s' (use l16 or l24)\n", v);
                return 1;
            }
            cfg.codec = c;
            // AES67 deployments commonly use 96 for L16 and 97 for L24; the
            // SDP carries the mapping, but matching the habit avoids surprises.
            if (c == Codec::L24 && cfg.payloadType == 96) {
                cfg.payloadType = 97;
            } else if (c == Codec::L16 && cfg.payloadType == 97) {
                cfg.payloadType = 96;
            }
            ++i;
        }
        else if (optEq(a, "--rate") && v)      { cfg.sampleRate = std::atoi(v); ++i; }
        else if (optEq(a, "--ptime") && v)     { cfg.ptimeMs = std::atof(v); ++i; }
        else if (optEq(a, "--ssrc") && v)      { ssrc = static_cast<uint32_t>(std::strtoul(v, nullptr, 0)); ++i; }
        else if (optEq(a, "--seconds") && v)   { seconds = std::atof(v); ++i; }
        else if (optEq(a, "--freq") && v)      { freq = std::atof(v); ++i; }
        else if (optEq(a, "--amplitude") && v) { amplitude = std::atoi(v); ++i; }
        else if (optEq(a, "--ttl") && v)       { cfg.ttl = std::atoi(v); ++i; }
        else if (optEq(a, "--out") && v)       { outPath = v; ++i; }
        else if (optEq(a, "--out-bits") && v) {
            const int bits = std::atoi(v);
            if (bits != 16 && bits != 24) {
                std::fprintf(stderr, "invalid --out-bits %s (use 16 or 24)\n", v);
                return 1;
            }
            outBits = bits;
            ++i;
        }
        else if (optEq(a, "--wav") && v)       { inWav = v; ++i; }
        else if (optEq(a, "--exclusive"))    { exclusive = true; }
        else if (optEq(a, "--domain") && v)     { ptpDomain = std::atoi(v); ++i; }
        else if (optEq(a, "--pace-ppm") && v)  { pacePpm = std::atof(v); ++i; }
        else if (optEq(a, "--ptp"))            { usePtp = true; }
        else if (optEq(a, "--shm") && v)       { shmName.assign(v, v + std::strlen(v)); ++i; }
        else if (optEq(a, "--device") && v)    { deviceName = v; ++i; }
        else if (optEq(a, "--in-device") && v) { inDeviceName = v; ++i; }
        else if (optEq(a, "--ptp-lock"))       { ptpLock = true; }
        else if (optEq(a, "--backend") && v)   { backend = v; ++i; }
        else if (optEq(a, "--asio-driver") && v) { asioDriver = v; backend = "asio"; ++i; }
        else if (optEq(a, "--refclk") && v)    { refclk = v; ++i; }
        else if (optEq(a, "--sdp-out") && v)   { sdpOutPath = v; ++i; }
        else if (optEq(a, "--sdp-file") && v)  { sdpInPath = v; ++i; }
        else if (optEq(a, "--print-sdp"))      { printSdp = true; }
        else if (optEq(a, "--sap"))            { useSap = true; }
        else if (optEq(a, "--rtcp"))       { cfg.rtcp = true; }
        else if (optEq(a, "--sap-interval") && v) { sapBaseInterval = std::atoi(v); ++i; }
        else if (optEq(a, "--baseline") && v)      { benchBaselinePath = v; ++i; }
        else if (optEq(a, "--write-baseline"))     { benchWriteBaseline = true; }
        else {
            std::fprintf(stderr, "unknown option: %s\n", a);
            return 1;
        }
    }

    // Capture source, resolved once so every branch below sees it: the wav and
    // loopback paths are separate scopes. OpenAES67 takes audio FROM this device,
    // i.e. it is an input, not an output.
    std::wstring captureId;
    std::string  captureLabel;
    // Resolve --in-device once, before any command branch: both tx (the signal
    // generator) and send (loopback capture) need it, and they live in separate
    // scopes. Without this, the option was parsed and silently ignored.
    if (!inDeviceName.empty()) {
        const auto inputs = listAudioInputDevices();
        const bool numeric = inDeviceName.find_first_not_of("0123456789") == std::string::npos;
        const AudioDeviceInfo *match = nullptr;
        if (numeric) {
            const int wanted = std::atoi(inDeviceName.c_str());
            if (wanted >= 0 && wanted < static_cast<int>(inputs.size())) {
                match = &inputs[static_cast<size_t>(wanted)];
            }
        } else {
            for (const auto &d : inputs) {
                if (!d.isAsio && d.name.find(inDeviceName) != std::string::npos) {
                    match = &d;
                    break;
                }
            }
        }
        if (!match) {
            std::fprintf(stderr, "no capture source matches '%s' (try list-devices)\n",
                         inDeviceName.c_str());
            return 2;
        }
        if (match->isAsio) {
            std::fprintf(stderr,
                         "ASIO capture is not implemented in this build; cannot open '%s'\n",
                         match->name.c_str());
            return 2;
        }
        captureId.assign(match->id.begin(), match->id.end());
        captureLabel = match->name;
    }
    // Parameter validation lives in StreamConfig so the GUI and the tests share one
    // rule. Each check guards a failure that is otherwise silent: port 99999 wraps
    // to 34463, a 1-second ptime builds a 192 KB packet, 0 channels divides by zero.
    {
        std::string why;
        const std::string reason = cfg.validate(&why);
        if (!reason.empty()) {
            std::fprintf(stderr, "invalid stream parameters: %s\n", reason.c_str());
            return 1;
        }
        if (cfg.framesPerPacket() * cfg.channels * 2 > 1400) {
            std::fprintf(stderr,
                         "warning: %d ch @ %g ms is %d bytes/packet, above a safe MTU\n",
                         cfg.channels, cfg.ptimeMs,
                         cfg.framesPerPacket() * cfg.channels * 2);
        }
    }

    Winsock ws;
    if (!ws.ok()) {
        std::fprintf(stderr, "Winsock init failed\n");
        return 3;
    }

    // WASAPI needs COM; harmless for the pure network commands.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // Builds the a=ts-refclk value. A PTP-referenced stream must name the
    // grandmaster: a Dante device in AES67 mode will not lock to a stream that
    // only claims a local clock, which is what this used to emit unconditionally.
    auto makeClockRef = [&]() -> SdpClockRef {
        SdpClockRef ref;
        if (!refclk.empty() && refclk.rfind("ptp:", 0) == 0) {
            const std::string rest = refclk.substr(4);
            const auto colon = rest.find(':');
            const std::string gmidText = colon == std::string::npos ? rest : rest.substr(0, colon);
            if (!parseGmid(gmidText, ref.gmid)) {
                std::fprintf(stderr, "invalid GMID in --refclk: %s\n", gmidText.c_str());
                std::exit(2);
            }
            ref.kind = SdpClockRef::Kind::Ptp;
            ref.domain = colon == std::string::npos
                             ? 0
                             : static_cast<uint8_t>(std::atoi(rest.substr(colon + 1).c_str()));
            return ref;
        }
        ref.kind = SdpClockRef::Kind::LocalMac;
        if (!localMacForIp(cfg.ifaceIp, ref.mac)) {
            std::fprintf(stderr,
                         "warning: cannot read the MAC of %s; SDP will carry 00-00-00-00-00-00\n",
                         cfg.ifaceIp.c_str());
        }
        return ref;
    };

    auto emitSdp = [&](const SdpClockRef &ref, bool toStdout) {
        const std::string text = buildSdp(cfg, ssrc, ref);
        if (toStdout) {
            printSdpBytes(text);
        }
        if (!sdpOutPath.empty()) {
            FILE *file = openFileUtf8(sdpOutPath.c_str(), "wb");
            if (!file) {
                std::fprintf(stderr, "cannot write %s\n", sdpOutPath.c_str());
            } else {
                std::fwrite(text.data(), 1, text.size(), file);
                std::fclose(file);
                std::fprintf(stderr, "SDP written to %s\n", sdpOutPath.c_str());
            }
        }
    };
    if (cmd == "sdp") {
        emitSdp(makeClockRef(), true);
        return 0;
    }

    if (cmd == "info") {
        std::printf("iface      : %s\n", cfg.ifaceIp.c_str());
        std::printf("group      : %s:%u\n", cfg.group.c_str(), cfg.port);
        std::printf("format     : %s %d ch @ %d Hz\n", codecName(cfg.codec), cfg.channels,
                    cfg.sampleRate);
        std::printf("ptime      : %.3f ms -> %d frames/packet\n", cfg.ptimeMs, cfg.framesPerPacket());
        std::printf("payload    : %d bytes/packet (%s)\n", cfg.payloadBytes(),
                    codecName(cfg.codec));
        std::printf("ttl        : %d\n", cfg.ttl);
        std::printf("ssrc       : 0x%08X\n", ssrc);
        return 0;
    }

    // ------------------------------------------------------------------ tx
    if (cmd == "tx") {
        // AES67 is a multicast standard: refuse a unicast group instead of silently
        // streaming to a single host and advertising an unusable connection line.
        if (!isMulticastIPv4(cfg.group)) {
            std::fprintf(stderr,
                         "group %s is not a multicast address (expected 224.0.0.0 "
                         "- 239.255.255.255)\n", cfg.group.c_str());
            return 2;
        }
        SdpClockRef clockRef = makeClockRef();
        // PTP feed (only with --ptp): the slave runs on its own thread so the socket
        // work stays off the audio path (polling inline cost ~3 ms per packet and
        // dropped the rate to 327/s), and PtpMediaClock turns the clock into RTP units.
        std::unique_ptr<PtpFeed> txFeed;
        PtpMediaClock txClock;
        if (usePtp) {
            txFeed.reset(new PtpFeed());
            std::string ptpErr;
            if (!txFeed->start(cfg.ifaceIp, static_cast<uint8_t>(ptpDomain & 0xFF), &ptpErr)) {
                std::fprintf(stderr, "PTP start failed: %s\n", ptpErr.c_str());
                return 2;
            }
            std::printf("  ptp     : slave on domain %d, timestamps follow the grandmaster\n",
                        ptpDomain);
            // Do not start the stream before the clock is usable: the first packets
            // would carry the free-running counter and the stream would jump when PTP
            // took over, which the receiver sees as a discontinuity.
            for (int i = 0; i < 200 && !txFeed->valid(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
            if (!txFeed->valid()) {
                std::fprintf(stderr, "PTP: no grandmaster after 5 s, sending without lock\n");
            } else {
                // Timestamps now come from the grandmaster, so the SDP must name it:
                // ts-refclk:ptp=IEEE1588-2008:<gmid>:<domain>, not localmac.
                uint8_t gmid[8] = {0};
                if (txFeed->masterIdentity(gmid)) {
                    clockRef.kind = SdpClockRef::Kind::Ptp;
                    std::memcpy(clockRef.gmid, gmid, 8);
                    clockRef.domain = static_cast<uint8_t>(ptpDomain & 0xFF);
                    std::printf("  refclk  : ptp=%s:%d\n", formatGmid(gmid).c_str(),
                                ptpDomain);
                }
            }
        }

        SapAnnouncer sap;
        if (useSap) {
            std::string sapErr;
            if (!sap.start(cfg.ifaceIp, buildSdp(cfg, ssrc, clockRef), &sapErr)) {
                std::fprintf(stderr, "SAP: %s\n", sapErr.c_str());
            } else {
                if (!sap.announce(&sapErr)) {
                    std::fprintf(stderr, "SAP announce failed: %s\n", sapErr.c_str());
                } else {
                    std::printf("  sap     : announcing on 239.255.255.255:9875\n");
                }
            }
        }
        if (printSdp) {
            emitSdp(clockRef, true);
        } else if (!sdpOutPath.empty()) {
            emitSdp(clockRef, false);
        }
        WavData file;
        const bool fileMode = !inWav.empty();
        if (fileMode) {
            std::string err;
            if (!readWavPcm(inWav, &file, &err)) {
                std::fprintf(stderr, "cannot read %s: %s\n", inWav.c_str(), err.c_str());
                return 2;
            }
            cfg.channels = file.channels;
            cfg.sampleRate = file.sampleRate;
        }


        const uint32_t ssrcOriginal = ssrc;   // 0 means "pick a random one"
        RtpSender sender(cfg, ssrc);
        std::string err;
        if (!sender.start(&err)) {
            std::fprintf(stderr, "tx start failed: %s\n", err.c_str());
            return 2;
        }

        const int fpp = cfg.framesPerPacket();
        const long long ptimeUs = static_cast<long long>(cfg.ptimeMs * 1000.0 + 0.5);

        std::printf("OpenAES67 tx\n");
        std::printf("  iface   : %s\n", cfg.ifaceIp.c_str());
        std::printf("  dst     : %s:%u\n", cfg.group.c_str(), cfg.port);
        std::printf("  format  : %s %d ch @ %d Hz, %d frames/packet\n",
                    codecName(cfg.codec),
                    cfg.channels, cfg.sampleRate, fpp);
        // Echo the parameters that are otherwise invisible: without this there is no
        // way to confirm from the console that --ttl and --ssrc took effect.
        std::printf("  ttl     : %d\n", cfg.ttl);
        std::printf("  ssrc    : 0x%08X%s\n", sender.ssrc(),
                    ssrcOriginal == 0 ? " (random)" : "");

        std::vector<int16_t> frame(static_cast<size_t>(fpp) * cfg.channels);
        long long total = 0;
        if (fileMode) {
            const long long frames = file.frames();
            total = (frames + fpp - 1) / fpp;
            std::printf("  source  : %s (%lld frames, %.3f s, %d-bit%s)\n",
                        inWav.c_str(), frames,
                        static_cast<double>(frames) / cfg.sampleRate, file.sourceBits,
                        file.sourceBits > 16 ? " -> top 16 bits kept" : "");
        } else {
            total = static_cast<long long>(seconds * 1000.0 / cfg.ptimeMs + 0.5);
            std::printf("  source  : %.0f Hz tone, amplitude %d\n", freq, amplitude);
        }
        std::printf("  packets : %lld\n", total);
        if (pacePpm != 0.0) {
            std::printf("  pace    : %+.1f ppm\n", pacePpm);
        }

        const auto t0 = std::chrono::steady_clock::now();
        sap.setIntervalSeconds(sapBaseInterval);
        double sapInterval = sap.nextIntervalSeconds();
        auto lastSapAnnounce = t0;
        auto lastRtcp = t0;
        const std::string rtcpCname = "openaes67@" + cfg.ifaceIp;
        // Audio pacing must survive a busy desktop. THREAD_PRIORITY_TIME_CRITICAL
        // keeps this thread ahead of normal-priority work; the environment switch
        // exists so the effect can be measured A/B from one binary.
        if (::getenv("OPENAES67_NO_RT_PRIORITY") == nullptr) {
            ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        }
        long long sampleIndex = 0;
        long long sent = 0;
        unsigned sendErrors = 0;
        for (long long n = 0; n < total; ++n) {
            if (fileMode) {
                const long long base = n * fpp;
                const long long frames = file.frames();
                for (int i = 0; i < fpp; ++i) {
                    const long long f = base + i;
                    for (int c = 0; c < cfg.channels; ++c) {
                        int16_t v = 0;
                        if (f < frames) {
                            v = file.samples[static_cast<size_t>(f) * cfg.channels + c];
                        }
                        frame[static_cast<size_t>(i) * cfg.channels + c] = v;
                    }
                }
            } else {
                for (int i = 0; i < fpp; ++i) {
                    const double t = static_cast<double>(sampleIndex + i) / cfg.sampleRate;
                    int v = static_cast<int>(
                        amplitude * std::sin(2.0 * 3.14159265358979323846 * freq * t));
                    if (v > 32767) v = 32767;
                    if (v < -32768) v = -32768;
                    frame[static_cast<size_t>(i) * cfg.channels] = static_cast<int16_t>(v);
                    for (int c = 1; c < cfg.channels; ++c) {
                        frame[static_cast<size_t>(i) * cfg.channels + c] = 0;
                    }
                }
            }

            // Tolerate transient failures: the socket retries internally and a lost
            // packet is recovered by the receiver. Only a sustained run of failures
            // means the destination is unreachable, and then giving up is right.
            bool usedPtpStamp = false;
            if (txFeed) {
                const uint32_t ts =
                    txClock.update(txFeed->masterNs(), cfg.sampleRate, n, fpp);
                usedPtpStamp = sender.sendFramesAt(ts, frame.data(), fpp);
            }
            if (!usedPtpStamp && !sender.sendFrames(frame.data(), fpp)) {
                if (++sendErrors >= 100) {
                    std::fprintf(stderr, "send failed %u times in a row, giving up\n",
                                 sendErrors);
                    break;
                }
            } else {
                sendErrors = 0;
                ++sent;
            }
            sampleIndex += fpp;
            // SAP must repeat. RFC 2974 expects an announcement every few seconds;
            // a single one at startup is invisible to a receiver that starts later,
            // which is exactly how this failed the first time.
            if (useSap && std::chrono::steady_clock::now() - lastSapAnnounce >=
                              std::chrono::microseconds(
                                  static_cast<long long>(sapInterval * 1e6))) {
                sap.announce(nullptr);
                lastSapAnnounce = std::chrono::steady_clock::now();
                sapInterval = sap.nextIntervalSeconds();
            }
            // RTCP every 5 s: the interval RFC 3550 recommends for audio. Kept
            // (nothing: SAP and RTCP are independent)
            // outside the SAP block so reports work with or without announcements.
            if (cfg.rtcp && std::chrono::steady_clock::now() - lastRtcp >=
                                std::chrono::milliseconds(5000)) {
                sender.sendRtcpSenderReport(rtcpCname);
                sender.pollReceiverReports();
                if (sender.hasReceiverReport()) {
                    static uint32_t rrSeen = 0;
                    if (sender.rrPackets() != rrSeen) {
                        rrSeen = sender.rrPackets();
                        const RtcpReceptionStats &rr = sender.receiverReport();
                        std::printf(
                            "rtcp: RR from %s lost=%.1f%% cumulative=%d jitter=%u\n",
                            sender.rrCname().empty() ? "(no cname)"
                                                     : sender.rrCname().c_str(),
                            static_cast<double>(rr.fractionLost) * 100.0 / 256.0,
                            rr.cumulativeLost, rr.jitter);
                        std::fflush(stdout);
                    }
                }
                lastRtcp = std::chrono::steady_clock::now();
            }
            // Pace interval scales with --pace-ppm so the receive side sees a
            // genuine rate error to correct (positive = sender runs fast).
            const double scale = 1.0 / (1.0 + pacePpm * 1e-6);
            std::this_thread::sleep_until(
                t0 + std::chrono::microseconds(static_cast<long long>(ptimeUs * (n + 1) * scale)));
        }
        sender.stop();
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        sap.withdraw();
        sap.stop();
        std::printf("done: %lld packets in %.2f s (%.1f pkt/s)\n",
                    sent, elapsed, elapsed > 0 ? sent / elapsed : 0.0);
        return 0;
    }

    // ------------------------------------------------------------------ rx
    // ---------------------------------------------------------------- bridge
    // One process, both directions: this is the "sound card mode". The receive
    // side fills the render ring (ASIO inputs), the send side drains the capture
    // ring (ASIO outputs). Running them in one process is what makes the device
    // usable by just starting OpenAES67 once.
    if (cmd == "bridge") {
        // Discovery first: the point of bridge is that the operator does not
        // have to know the group, port and format of the stream they want.
        if (discoverStream) {
            std::printf("  discover: listening for SAP announcements (%.0f s)%s%s\n",
                        discoverSeconds,
                        discoverName.empty() ? "" : ", filtered by name",
                        "");
            SapStreamInfo picked;
            bool havePick = false;
            listenSap(
                static_cast<int>(discoverSeconds < 1.0 ? 1.0 : discoverSeconds),
                cfg.ifaceIp,
                [&](const SapAnnouncement &a) {
                    if (havePick || a.deletion) {
                        return;
                    }
                    SapStreamInfo info;
                    if (!parseSdpStreamInfo(a.sdp, &info)) {
                        return;
                    }
                    if (!discoverName.empty() &&
                        info.name.find(discoverName) == std::string::npos) {
                        return;
                    }
                    info.origin = a.origin;
                    picked = info;
                    havePick = true;
                },
                nullptr);
            if (!havePick) {
                std::fprintf(stderr,
                             "bridge: no SAP announcement found in %.0f s\n",
                             discoverSeconds);
                return 2;
            }
            cfg.group = picked.group;
            cfg.port = picked.port;
            if (picked.channels > 0) {
                cfg.channels = picked.channels;
            }
            if (picked.sampleRate > 0) {
                cfg.sampleRate = picked.sampleRate;
            }
            cfg.codec = picked.codec;
            // L24 implies payload type 97 unless the caller set one explicitly.
            if (cfg.payloadType == 0 || cfg.codec == Codec::L24) {
                cfg.payloadType = cfg.codec == Codec::L24 ? 97 : 96;
            }
            std::printf("  picked  : %s  %s %d ch @ %d Hz from %s\n",
                        picked.name.c_str(), codecName(picked.codec), picked.channels,
                        picked.sampleRate, picked.origin.c_str());
        }
        if (shmName.empty()) {
            std::fprintf(stderr, "bridge needs --shm NAME (the section the ASIO driver attaches to)\n");
            return 2;
        }
        if (shmName.empty()) {
            std::fprintf(stderr, "bridge needs --shm NAME (the section the ASIO driver attaches to)\n");
            return 2;
        }
        BridgeOptions bo;
        bo.downCfg = cfg;
        bo.upCfg = cfg;
        if (!sendGroup.empty()) {
            bo.upCfg.group = sendGroup;
        }
        bo.upCfg.port = sendPort != 0
                            ? sendPort
                            : static_cast<uint16_t>(cfg.port + 2 <= 65535 ? cfg.port + 2
                                                                          : cfg.port);
        bo.shmName = shmName;
        bo.seconds = seconds;
        bo.usePtp = usePtp;
        bo.ptpDomain = ptpDomain;
        bo.announceUp = useSap;
        bo.sapInterval = sapBaseInterval;
        std::printf("OpenAES67 bridge (AES67 <-> shared ring <-> ASIO)\n");
        std::printf("  section : %s, %d ch @ %d Hz\n", toUtf8(shmName).c_str(), cfg.channels,
                    cfg.sampleRate);
        std::printf("  downlink: %s:%u -> ring\n", cfg.group.c_str(), cfg.port);
        std::printf("  uplink  : ring -> %s:%u\n", bo.upCfg.group.c_str(), bo.upCfg.port);

        // Say whether a DAW can actually see the device. Without this the
        // symptom is "Audition shows nothing", which is indistinguishable from
        // a broken stream.
        {
            const AsioRegistration asio = probeAsioRegistration();
            if (asio.registered && asio.dllPresent) {
                std::printf("  asio    : %s registered (%s)\n", asio.name.c_str(),
                            asio.dllPath.c_str());
            } else if (asio.registered) {
                std::fprintf(stderr,
                             "  asio    : %s is registered but its DLL is missing: %s\n",
                             asio.name.c_str(), asio.dllPath.c_str());
            } else {
                std::fprintf(stderr,
                             "  asio    : not registered -- run asio_driver\\register-asio-driver.bat"
                             " as administrator so a DAW can list the device\n");
            }
        }

        BridgeStats stats;
        std::string bridgeErr;
        if (!runBridge(bo, &stats, &bridgeErr)) {
            std::fprintf(stderr, "bridge: %s\n", bridgeErr.c_str());
            return 2;
        }
        std::printf("bridge done: %lld uplink packets (%lld without ASIO audio)\n",
                    stats.uplinkPackets, stats.uplinkSilent);
        return 0;
    }

    if (cmd == "rx") {
        // Same SDP support as recv: without it a caller who passes --sdp-file to rx
        // silently gets the defaults, which is how an 8-channel stream ended up
        // decoded as 2 channels during testing.
        if (!sdpInPath.empty()) {
            std::FILE *f = openFileUtf8(sdpInPath.c_str(), "rb");
            if (!f) {
                std::fprintf(stderr, "cannot open SDP file: %s\n", sdpInPath.c_str());
                return 2;
            }
            std::string sdpBody;
            char sdpChunk[512];
            size_t sdpN = 0;
            while ((sdpN = std::fread(sdpChunk, 1, sizeof(sdpChunk), f)) > 0) {
                sdpBody.append(sdpChunk, sdpN);
            }
            std::fclose(f);
            // PowerShell redirection writes UTF-16; normalise before parsing.
            sdpBody = decodeTextToUtf8(sdpBody);
            int sdpCh = 0, sdpSr = 0;
            Codec sdpCodec = cfg.codec;
            if (!parseSdpAudioFormat(sdpBody, &sdpCh, &sdpSr, &sdpCodec)) {
                std::fprintf(stderr, "SDP does not describe an L16 stream: %s\n",
                             sdpInPath.c_str());
                return 2;
            }
            cfg.channels = sdpCh;
            cfg.sampleRate = sdpSr;
            cfg.codec = sdpCodec;

            // The SDP is also the authority on where the stream lives; only
            // fill from it when the caller did not say explicitly.
            SapStreamInfo where;
            if (parseSdpStreamInfo(sdpBody, &where)) {
                if (!groupGiven && !where.group.empty()) {
                    cfg.group = where.group;
                }
                if (!portGiven && where.port != 0) {
                    cfg.port = where.port;
                }
            }
            cfg.payloadType = sdpCodec == Codec::L24 ? 97 : 96;

            std::printf("  sdp     : %s -> %s %d ch @ %d Hz\n", sdpInPath.c_str(),
                        codecName(sdpCodec), sdpCh, sdpSr);
        }
        RtpReceiver receiver(cfg);
        std::string err;
        if (!receiver.start(&err)) {
            std::fprintf(stderr, "rx start failed: %s\n", err.c_str());
            return 2;
        }
        const int fpp = cfg.framesPerPacket();
        std::printf("OpenAES67 rx\n");
        std::printf("  iface   : %s\n", cfg.ifaceIp.c_str());
        std::printf("  group   : %s:%u\n", cfg.group.c_str(), cfg.port);
        std::printf("  out     : %s\n", outPath.c_str());

        std::vector<int16_t> chunk(static_cast<size_t>(fpp) * cfg.channels);
        std::vector<int16_t> all;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(static_cast<long long>(seconds * 1000.0));
        auto lastRr = std::chrono::steady_clock::now();
        uint32_t rtcpSeen = 0;
        const std::string rrCname = "openaes67@" + cfg.ifaceIp;
        while (std::chrono::steady_clock::now() < deadline) {
            const int n = receiver.pull(chunk.data(), fpp, 100);
            if (n > 0) {
                all.insert(all.end(), chunk.begin(),
                           chunk.begin() + static_cast<size_t>(n) * cfg.channels);
            }
            if (cfg.rtcp && receiver.rtcpActive()) {
                if (receiver.rtcpPackets() != rtcpSeen) {
                    rtcpSeen = receiver.rtcpPackets();
                    const RtcpSenderReport &sr = receiver.senderReport();
                    std::printf("  rtcp SR : ntp=%08X.%08X rtp=%u packets=%u octets=%u\n",
                                static_cast<uint32_t>(sr.ntp >> 32),
                                static_cast<uint32_t>(sr.ntp & 0xFFFFFFFF),
                                sr.rtpTimestamp, sr.packetCount, sr.octetCount);
                    std::fflush(stdout);
                }
                if (receiver.hasSenderReport() &&
                    std::chrono::steady_clock::now() - lastRr >=
                        std::chrono::milliseconds(5000)) {
                    receiver.sendReceiverReport(rrCname);
                    lastRr = std::chrono::steady_clock::now();
                }
            }
        }
        receiver.stop();
        const RxStats &st = receiver.stats();
        const JitterBuffer &jb = receiver.jitter();

        const bool wrote = (outBits == 24)
                               ? writeWavPcm24(outPath, all.data(), all.size(),
                                               cfg.channels, cfg.sampleRate)
                               : writeWavPcm16(outPath, all.data(), all.size(),
                                               cfg.channels, cfg.sampleRate);
        (void)wrote;

        const size_t frames = all.size() / static_cast<size_t>(cfg.channels);
        std::printf("\nresult\n");
        std::printf("  packets    : %u\n", st.packets);
        std::printf("  other ssrc : %u (from another source, ignored)\n", st.wrongSsrc);
        std::printf("  ssrc change: %u (sender restarts adopted)\n", st.ssrcChanges);
        if (st.wrongPayloadType > 0) {
            std::printf("  wrong pt   : %u (packets dropped: this stream is not payload type %u)\n",
                        st.wrongPayloadType, static_cast<unsigned>(cfg.payloadType));
        }
        std::printf("  frames     : %zu (%.3f s of audio)\n", frames,
                    cfg.sampleRate ? static_cast<double>(frames) / cfg.sampleRate : 0.0);
        std::printf("  lost       : %u\n", jb.lost());
        std::printf("  duplicates : %u\n", jb.duplicates());
        std::printf("  late       : %u\n", jb.late());
        std::printf("  jitter buf : %d packets remaining\n", jb.depth());
        std::printf("  wav        : %s\n", outPath.c_str());
        if (st.packets == 0) {
            std::printf("\nno packets received:\n");
            if (st.wrongPayloadType > 0) {
                std::printf("  - %u packets had a different payload type: the sender"
                            " is probably using another codec (--codec)\n",
                            st.wrongPayloadType);
            }
            std::printf("  - is the sender running?\n");
            std::printf("  - firewall may block UDP %u\n", cfg.port);
            std::printf("  - --iface must be the NIC carrying the stream\n");
        }
        return 0;
    }

    // ------------------------------------------------------------- capture
    if (cmd == "capture") {
        AudioSource *src = createWasapiLoopbackSource(captureId.empty() ? nullptr : captureId.c_str());
        if (!src) {
            std::fprintf(stderr, "cannot open WASAPI loopback (no default render device?)\n");
            return 2;
        }
        const int ch = src->channels();
        const int rate = src->sampleRate();
        std::printf("OpenAES67 capture (WASAPI loopback)\n");
        std::printf("  source  : default render device, %d ch @ %d Hz, backend=%s\n", ch, rate,
                    src->backendName());
        std::printf("  seconds : %.1f\n", seconds);

        std::vector<int16_t> all;
        std::vector<int16_t> buf(static_cast<size_t>(ch) * 480);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(static_cast<long long>(seconds * 1000.0));
        while (std::chrono::steady_clock::now() < deadline) {
            const int n = src->read(buf.data(), 480);
            if (n > 0) {
                all.insert(all.end(), buf.begin(),
                           buf.begin() + static_cast<size_t>(n) * ch);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        delete src;

        writeWavPcm16(outPath, all.data(), all.size(), ch, rate);
        const size_t frames = ch > 0 ? all.size() / static_cast<size_t>(ch) : 0;
        std::printf("  frames  : %zu (%.3f s)\n", frames,
                    rate ? static_cast<double>(frames) / rate : 0.0);
        std::printf("  wav     : %s\n", outPath.c_str());

        int peak = 0;
        for (int16_t v : all) {
            const int a = v < 0 ? -v : v;
            if (a > peak) peak = a;
        }
        std::printf("  peak    : %d\n", peak);
        if (peak == 0) {
            std::printf("  (silence: nothing was playing on the default device)\n");
        }
        return 0;
    }

    // ----------------------------------------------------------- capture-in
    // Records a real input endpoint, which is what a DAW/Audition sees as a
    // recording device. Loopback (above) records what a speaker is playing; this
    // records what a capture endpoint produces, so it is the check that answers
    // "will the recorder actually get the audio?".
    if (cmd == "capture-in") {
        std::wstring id;
        std::string label = "system default input";
        // --in-device names the recording endpoint: this command records a real
        // input, whereas tx/send loopback-capture a render endpoint.
        const std::string wanted = inDeviceName;
        if (!wanted.empty()) {
            const auto inputs = listCaptureEndpoints();
            bool found = false;
            for (const auto &d : inputs) {
                if (d.name.find(wanted) != std::string::npos) {
                    id.assign(d.id.begin(), d.id.end());
                    label = d.name;
                    found = true;
                    break;
                }
            }
            if (!found) {
                std::fprintf(stderr, "no capture endpoint matches '%s' (run list-devices)\n",
                             wanted.c_str());
                return 2;
            }
        } else if (listCaptureEndpoints().empty()) {
            std::fprintf(stderr, "no capture endpoints on this machine\n");
            return 2;
        }

        AudioSource *src = createWasapiCaptureSource(id.empty() ? nullptr : id.c_str());
        if (!src) {
            std::fprintf(stderr, "cannot open capture endpoint '%s'\n", label.c_str());
            return 2;
        }
        const int ch = src->channels();
        const int rate = src->sampleRate();
        std::printf("OpenAES67 capture-in (input endpoint)\n");
        std::printf("  source  : %s, %d ch @ %d Hz, backend=%s\n", label.c_str(), ch, rate,
                    src->backendName());
        std::printf("  seconds : %.1f\n", seconds);

        std::vector<int16_t> all;
        std::vector<int16_t> buf(static_cast<size_t>(ch) * 480);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(static_cast<long long>(seconds * 1000.0));
        while (std::chrono::steady_clock::now() < deadline) {
            const int n = src->read(buf.data(), 480);
            if (n > 0) {
                all.insert(all.end(), buf.begin(),
                           buf.begin() + static_cast<size_t>(n) * ch);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        delete src;

        writeWavPcm16(outPath, all.data(), all.size(), ch, rate);
        int peak = 0;
        long long crossings = 0;
        for (size_t i = 0; i < all.size(); ++i) {
            const int a = all[i] < 0 ? -all[i] : all[i];
            if (a > peak) peak = a;
            if (i > 0 && ((all[i - 1] < 0) != (all[i] < 0))) {
                ++crossings;
            }
        }
        const size_t frames = ch > 0 ? all.size() / static_cast<size_t>(ch) : 0;
        const double secondsCaptured = rate ? static_cast<double>(frames) / rate : 0.0;
        // Zero crossings over the mono-ish mix approximate the tone frequency, which
        // is enough to prove that a known test tone travelled through the device.
        const double captureFreq = secondsCaptured > 0.01 && ch > 0
                                ? static_cast<double>(crossings) / 2.0 /
                                      (secondsCaptured)
                                : 0.0;
        std::printf("  frames  : %zu (%.3f s)\n", frames, secondsCaptured);
        std::printf("  peak    : %d\n", peak);
        std::printf("  freq    : %.1f Hz (from zero crossings)\n", captureFreq);
        std::printf("  wav     : %s\n", outPath.c_str());
        if (peak == 0) {
            std::printf("  (silence: the endpoint produced no audio)\n");
        }
        return 0;
    }

    // ---------------------------------------------------------------- play
    if (cmd == "play") {
        if (inWav.empty()) {
            std::fprintf(stderr, "play needs --wav FILE\n");
            return 1;
        }
        WavData w;
        std::string err;
        if (!readWavPcm(inWav, &w, &err)) {
            std::fprintf(stderr, "cannot read %s: %s\n", inWav.c_str(), err.c_str());
            return 2;
        }
        AudioSink *sink = exclusive ? createWasapiRenderSinkExclusive(nullptr)
                                    : createWasapiRenderSink(nullptr);
        if (!sink) {
            std::fprintf(stderr, "cannot open WASAPI render (no default device?)\n");
            return 2;
        }
        std::printf("OpenAES67 play\n");
        std::printf("  file    : %s (%d ch @ %d Hz, %d frames)\n", inWav.c_str(),
                    w.channels, w.sampleRate, w.frames());
        std::printf("  device  : %d ch @ %d Hz, backend=%s\n", sink->channels(),
                    sink->sampleRate(), sink->backendName());
        if (sink->channels() != w.channels || sink->sampleRate() != w.sampleRate) {
            std::fprintf(stderr,
                         "format mismatch: device is %d ch @ %d Hz, file is %d ch @ %d Hz\n"
                         "(sample-rate conversion lands in M4)\n",
                         sink->channels(), sink->sampleRate(), w.channels, w.sampleRate);
            delete sink;
            return 2;
        }
        const int total = w.frames();
        int done = 0;
        while (done < total) {
            const int chunk = 480;
            const int n = (done + chunk <= total) ? chunk : (total - done);
            const int wrote = sink->write(&w.samples[static_cast<size_t>(done) * w.channels], n);
            if (wrote <= 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            done += wrote;
            std::this_thread::sleep_for(std::chrono::microseconds(1000));
        }
        // Let the shared-mode buffer drain before tearing the client down.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        delete sink;
        std::printf("  played  : %d frames (%.3f s)\n", done,
                    w.sampleRate ? static_cast<double>(done) / w.sampleRate : 0.0);
        return 0;
    }
    // ------------------------------------------------------------------ send
    // End-to-end transmit: WASAPI loopback -> AES67 RTP.
    // The device delivers audio in ~10 ms blocks, so we tick at 10 ms and emit
    // (10000 / ptime_us) RTP packets per tick. Polling once per 1 ms packet made
    // the loop take 25x realtime, which is why it is done this way.
    if (cmd == "send") {
        // Two sources: by default the WASAPI loopback of a render endpoint (what
        // the PC is playing); with --shm, the capture side of the shared section,
        // which is what the ASIO driver writes when a host plays into it.
        AudioSource *src = nullptr;
        if (!shmName.empty()) {
            std::string shmErr;
            auto ring = openSharedAudio(shmName.c_str(), &shmErr);
            if (!ring || !ring->valid()) {
                std::fprintf(stderr, "send: cannot open shared section: %s\n",
                             shmErr.c_str());
                return 2;
            }
            std::printf("  section : %d ch @ %d Hz (capture side)\n", ring->channels(),
                        ring->sampleRate());
            src = new SharedCaptureSource(std::move(ring));
        } else {
            src = createWasapiLoopbackSource(captureId.empty() ? nullptr : captureId.c_str());
        }
        if (!src) {
            std::fprintf(stderr, "cannot open WASAPI loopback (no default render device?)\n");
            return 2;
        }
        // Keep the requested stream channel count; the capture format only decides
        // how many channels we can fill. This used to be overwritten here, which is
        // why --channels had no effect on send.
        const int srcChannels = src->channels();
        const int srcRate = src->sampleRate();
        if (srcChannels <= 0 || srcRate <= 0 || cfg.channels <= 0) {
            std::fprintf(stderr, "loopback reported an invalid format\n");
            delete src;
            return 2;
        }
        if (cfg.channels != srcChannels) {
            std::printf("  mix     : capture %d ch -> stream %d ch (drop extra, pad silence)\n",
                        srcChannels, cfg.channels);
        }
        if (cfg.sampleRate != srcRate) {
            std::printf("  rate    : device mixes at %d Hz, using it instead of %d Hz"
                        " (no resampling)\n", srcRate, cfg.sampleRate);
            cfg.sampleRate = srcRate;
        }
        const int payloadBytes = cfg.framesPerPacket() * cfg.channels * 2;
        if (payloadBytes + 12 > 1400) {
            std::fprintf(stderr,
                         "warning: %d ch @ %g ms = %d byte payload, exceeds 1500-byte MTU "
                         "and will be fragmented\n",
                         cfg.channels, cfg.ptimeMs, payloadBytes);
        }

        RtpSender sender(cfg, ssrc);
        std::string err;
        // SDP state is needed by the tick loop: a grandmaster change must re-announce
        // with a bumped o= version, or receivers keep the stale clock reference.
        SdpClockRef wavClockRef = makeClockRef();
        const uint8_t zeroGmid[8] = {0};
        uint8_t     sapGmid[8] = {0};
        bool        sapHaveGmid = false;
        uint32_t    sapVersion = 1;
        // PTP is created before the SAP block: the announced clock reference must
        // name the grandmaster the stream is actually locked to, and a stream that
        // only claims a local clock is rejected by a Dante device in AES67 mode.
        std::unique_ptr<PtpSlave> ptp;
        long long ptpTicks = 0;
        if (ptpLock || (useSap && refclk.empty())) {
            ptp.reset(new PtpSlave());
            if (!ptp->start(cfg.ifaceIp, static_cast<uint8_t>(ptpDomain & 0xFF), &err)) {
                std::fprintf(stderr, "PTP start failed: %s\n", err.c_str());
                delete src;
                return 2;
            }
            std::printf("  clock   : PTP domain %d\n", ptpDomain);
        }

        SapAnnouncer sapWav;
        if (useSap) {
            std::string sapErr;
            // Prefer the live grandmaster over a static --refclk/localmac.
            // masterIdentity() returns a zero-filled buffer before a master is
            // seen, so "non-null" is not "locked": announcing that would publish
            // an all-zero GMID, which no receiver can lock to.
            const uint8_t *gm =
                (ptp && ptp->hasMaster()) ? ptp->masterIdentity() : nullptr;
            if (gm && std::memcmp(gm, zeroGmid, 8) == 0) {
                gm = nullptr;
            }
            if (gm) {
                std::memcpy(sapGmid, gm, 8);
                sapHaveGmid = true;
                wavClockRef.kind = SdpClockRef::Kind::Ptp;
                std::memcpy(wavClockRef.gmid, sapGmid, 8);
                wavClockRef.domain = static_cast<uint8_t>(ptpDomain & 0xFF);
            }
            if (!sapWav.start(cfg.ifaceIp, buildSdp(cfg, ssrc, wavClockRef, sapVersion),
                              &sapErr)) {
                std::fprintf(stderr, "SAP: %s\n", sapErr.c_str());
            } else if (sapWav.announce(&sapErr)) {
                std::printf("  sap     : announcing on 239.255.255.255:9875\n");
            } else {
                std::fprintf(stderr, "SAP announce failed: %s\n", sapErr.c_str());
            }
        }
        if (!sender.start(&err)) {
            std::fprintf(stderr, "send start failed: %s\n", err.c_str());
            delete src;
            return 2;
        }

        const int ch = cfg.channels;
        const int fpp = cfg.framesPerPacket();
        int tickFrames = cfg.sampleRate / 100;              // 10 ms
        if (tickFrames < fpp) {
            tickFrames = fpp;
        }
        tickFrames = (tickFrames / fpp) * fpp;              // whole packets
        const int packetsPerTick = tickFrames / fpp;
        const long long tickUs =
            static_cast<long long>(1000000.0 * tickFrames / cfg.sampleRate + 0.5);
        const long long ticks = static_cast<long long>(seconds * 1e6 / tickUs + 0.5);

        std::printf("OpenAES67 send (loopback -> AES67)\n");
        std::printf("  source  : %s, %d ch @ %d Hz\n", src->backendName(), ch, cfg.sampleRate);
        std::printf("  input   : %s\n",
                    captureLabel.empty() ? "(default loopback)" : captureLabel.c_str());
        std::printf("  dst     : %s:%u\n", cfg.group.c_str(), cfg.port);
        std::printf("  tick    : %d frames (%d packets of %d frames)\n",
                    tickFrames, packetsPerTick, fpp);
        std::printf("  ticks   : %lld (%.1f s)\n", ticks, seconds);

        std::vector<int16_t> cap(static_cast<size_t>(tickFrames) * srcChannels, 0);
        std::vector<int16_t> pkt(static_cast<size_t>(fpp) * ch, 0);
        const auto t0 = std::chrono::steady_clock::now();
        auto lastSapAnnounce = t0;
        auto lastRtcp = t0;
        const std::string rtcpCname = "openaes67@" + cfg.ifaceIp;
        long long sent = 0;
        unsigned sendErrors = 0;
        long long silentPackets = 0;
        long long ticksWithData = 0;

        // --- PTP-locked timestamps (AES67 a=mediaclk:direct=0) ----------------

        for (long long tick = 0; tick < ticks; ++tick) {
            uint32_t tsBase = 0;
            bool lockTick = false;
            if (ptp) {
                ptp->poll(0);
                if (++ptpTicks % 100 == 0) {
                    ptp->sendDelayReq();
                    // A new grandmaster (or the first lock) makes the announced clock
                    // reference stale. Re-announce with a bumped o= version: receivers
                    // ignore a changed body that keeps the same version.
                    const uint8_t *liveGm =
                        ptp->hasMaster() ? ptp->masterIdentity() : nullptr;
                    if (liveGm && std::memcmp(liveGm, zeroGmid, 8) == 0) {
                        liveGm = nullptr;
                    }
                    // Two masters briefly overlapping make the slave flip; a
                    // minimum spacing keeps that from spamming announcements.
                    static auto lastReannounce = std::chrono::steady_clock::now();
                    const bool mayReannounce =
                        std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - lastReannounce)
                            .count() >= 5;
                    const bool changed =
                        liveGm && (!sapHaveGmid || std::memcmp(liveGm, sapGmid, 8) != 0);
                    if (useSap && changed && mayReannounce) {
                        lastReannounce = std::chrono::steady_clock::now();
                        std::memcpy(sapGmid, liveGm, 8);
                        sapHaveGmid = true;
                        wavClockRef.kind = SdpClockRef::Kind::Ptp;
                        std::memcpy(wavClockRef.gmid, sapGmid, 8);
                        wavClockRef.domain = static_cast<uint8_t>(ptpDomain & 0xFF);
                        ++sapVersion;
                        sapWav.setSdp(buildSdp(cfg, ssrc, wavClockRef, sapVersion));
                        std::string sapErr;
                        if (sapWav.announce(&sapErr)) {
                            std::printf("  sap     : grandmaster %02X%02X%02X%02X%02X%02X%02X%02X changed, version %u\n",
                                        sapGmid[0], sapGmid[1], sapGmid[2], sapGmid[3],
                                        sapGmid[4], sapGmid[5], sapGmid[6], sapGmid[7],
                                        sapVersion);
                            std::fflush(stdout);
                        }
                    }
                }
                int64_t masterNs = 0;
                if (ptp->masterTimeNs(&masterNs)) {
                    tsBase = static_cast<uint32_t>(
                        (static_cast<double>(masterNs) * cfg.sampleRate) / 1e9);
                    lockTick = true;
                }
            }
            const auto tickStart = std::chrono::steady_clock::now();
            int got = src->read(cap.data(), tickFrames);
            const auto afterRead = std::chrono::steady_clock::now();
            if (got < 0) {
                got = 0;
            }
            if (got > 0) {
                ++ticksWithData;
            }
            for (int k = 0; k < packetsPerTick; ++k) {
                const int off = k * fpp;
                const int have = (got > off) ? (got - off) : 0;
                // Map capture channels onto the stream layout: copy the channels
                // both sides have, zero the rest, so --channels actually works.
                for (int i = 0; i < fpp; ++i) {
                    const bool rowPresent = (i < have);
                    for (int c = 0; c < ch; ++c) {
                        int16_t value = 0;
                        if (rowPresent && c < srcChannels) {
                            value = cap[static_cast<size_t>(off + i) * srcChannels + c];
                        }
                        pkt[static_cast<size_t>(i) * ch + c] = value;
                    }
                }
                if (have < fpp) {
                    ++silentPackets;
                }
                if (!sender.sendFrames(pkt.data(), fpp)) {
                    // A single failed send is not fatal any more: the socket retries
                    // transient errors internally and a lost packet during a hiccup is
                    // recovered by the receiver. Stop only after a sustained failure.
                    if (++sendErrors >= 100) {
                        std::fprintf(stderr, "send failed %u times, giving up\n", sendErrors);
                        tick = ticks;
                        break;
                    }
                    tick = ticks;
                    break;
                }
                ++sent;
            }
            const auto afterSend = std::chrono::steady_clock::now();
            std::this_thread::sleep_until(t0 + std::chrono::microseconds(tickUs * (tick + 1)));
            if (::getenv("OPENAES67_SEND_DEBUG") != nullptr && (tick % 20) == 0) {
                const auto toUs = [](auto a, auto b) {
                    return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
                };
                std::fprintf(stderr, "[send] tick=%lld read=%lldus packets=%lldus total=%lldus\n",
                             tick, (long long)toUs(tickStart, afterRead),
                             (long long)toUs(afterRead, afterSend),
                             (long long)toUs(tickStart, std::chrono::steady_clock::now()));
            }
        }
        sender.stop();
        delete src;

        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        sapWav.withdraw();
        sapWav.stop();
        std::printf("done: %lld packets in %.2f s (%.1f pkt/s)\n", sent, elapsed,
                    elapsed > 0 ? sent / elapsed : 0.0);
        std::printf("  ticks with audio: %lld / %lld\n", ticksWithData, ticks);
        std::printf("  silence-padded packets: %lld\n", silentPackets);
        return 0;
    }

    // ------------------------------------------------------------------ recv
    // The whole receive path lives in runReceiveLoop() so the CLI and the GUI
    // cannot drift apart. They used to be two copies of the same logic and the
    // buffer depth differed by 3.5x between them.
    if (cmd == "recv") {
        // An SDP file describes where the stream is as well as how it is
        // encoded, so it is read before the group is validated: relying on the
        // operator to repeat --group/--port is how `recv --sdp-file` used to fail
        // with "group  is not a multicast address" for a perfectly good SDP.
        if (!sdpInPath.empty()) {
            std::FILE *f = openFileUtf8(sdpInPath.c_str(), "rb");
            if (!f) {
                std::fprintf(stderr, "cannot open SDP file: %s\n", sdpInPath.c_str());
                return 2;
            }
            std::string body;
            char chunk[512];
            size_t n = 0;
            while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) { body.append(chunk, n); }
            std::fclose(f);
            // PowerShell redirection writes UTF-16; normalise before parsing.
            body = decodeTextToUtf8(body);
            int ch = 0, sr = 0;
        Codec sdpCodec = cfg.codec;
        if (!parseSdpAudioFormat(body, &ch, &sr, &sdpCodec)) {
                std::fprintf(stderr,
                             "SDP does not describe an L16/L24 stream: %s\n",
                             sdpInPath.c_str());
            }
            cfg.channels = ch;
            cfg.sampleRate = sr;
            cfg.codec = sdpCodec;
            cfg.payloadType = sdpCodec == Codec::L24 ? 97 : 96;
            // Where the stream lives comes from the SDP too, unless the caller
            // named it explicitly.
            SapStreamInfo where;
            if (parseSdpStreamInfo(body, &where)) {
                if (!groupGiven && !where.group.empty()) {
                    cfg.group = where.group;
                }
                if (!portGiven && where.port != 0) {
                    cfg.port = where.port;
                }
                std::printf("  location: %s:%u\n", cfg.group.c_str(),
                            cfg.port);
            }
            std::printf("  sdp     : %s -> %s %d ch @ %d Hz\n", sdpInPath.c_str(),
                        codecName(sdpCodec), ch, sr);
        }

        if (!isMulticastIPv4(cfg.group)) {
            std::fprintf(stderr,
                         "group %s is not a multicast address (expected 224.0.0.0 "
                         "- 239.255.255.255)\n", cfg.group.c_str());
            return 2;
        }

        ReceiveLoopOptions rl;
        rl.shmName = shmName;
        rl.backend = backend.empty() ? "wasapi" : backend;
        rl.asioDriver = asioDriver;
        rl.deviceLabel = asioDriver.empty() ? std::string() : ("ASIO: " + asioDriver);
        if (!deviceName.empty()) {
            const auto devices = listAudioOutputDevices();
            // Accept an index as printed by list-devices, or a name substring.
            const bool numeric = deviceName.find_first_not_of("0123456789") == std::string::npos;
            const AudioDeviceInfo *match = nullptr;
            if (numeric) {
                const int wanted = std::atoi(deviceName.c_str());
                if (wanted >= 0 && wanted < static_cast<int>(devices.size())) {
                    match = &devices[static_cast<size_t>(wanted)];
                }
            } else {
                // Substring matching is ambiguous when several devices share a prefix
                // (two "扬声器 (...)" entries here): say so instead of silently taking
                // the first one.
                std::vector<const AudioDeviceInfo *> matches;
                for (const auto &d : devices) {
                    if (!d.isAsio && d.name.find(deviceName) != std::string::npos) {
                        matches.push_back(&d);
                    }
                }
                if (matches.size() > 1) {
                    std::fprintf(stderr, "%zu devices match '%s':\n", matches.size(),
                                 deviceName.c_str());
                    for (const auto *d : matches) {
                        std::fprintf(stderr, "  - %s\n", d->name.c_str());
                    }
                    std::fprintf(stderr,
                                 "using '%s' (pass a longer name to pick another)\n",
                                 matches.front()->name.c_str());
                }
                if (!matches.empty()) {
                    match = matches.front();
                }
            }
            if (!match) {
                std::fprintf(stderr, "no audio output matches '%s' (try list-devices)\n",
                             deviceName.c_str());
                return 2;
            }
            rl.deviceLabel = match->name;
            if (match->isAsio) {
                rl.backend = "asio";
                rl.asioDriver = match->id;
            } else {
                rl.deviceId.assign(match->id.begin(), match->id.end());
            }
        }
        rl.cfg = cfg;
        rl.exclusive = exclusive;
        rl.usePtp = usePtp;
    rl.ptpDomain = ptpDomain;
        rl.seconds = seconds;
        rl.onLog = [](const std::string &s) { std::printf("  %s\n", s.c_str()); };


        std::printf("OpenAES67 recv (AES67 -> render)\n");
        std::printf("  iface   : %s\n", cfg.ifaceIp.c_str());
        std::printf("  group   : %s:%u\n", cfg.group.c_str(), cfg.port);
        std::printf("  seconds : %.1f\n", seconds);

        RxSnapshot fin;
        std::string recvErr;
        if (!runReceiveLoop(rl, &fin, &recvErr)) {
            std::fprintf(stderr, "recv failed: %s\n", recvErr.c_str());
            return 2;
        }
        std::printf("\nresult\n");
        std::printf("  packets    : %u\n", fin.packets);
        std::printf("  frames out : %lld (%.3f s)\n", fin.framesWritten,
                    cfg.sampleRate ? static_cast<double>(fin.framesWritten) / cfg.sampleRate
                                   : 0.0);
        std::printf("  lost       : %u\n", fin.lost);
        std::printf("  late       : %u\n", fin.late);
    std::printf("  other ssrc : %u (ignored)\n", fin.wrongSsrc);
    std::printf("  ssrc change: %u (restarts adopted)\n", fin.ssrcChanges);
        std::printf("  underruns  : %d\n", fin.underruns);
        std::printf("  buffer     : mean=%d target=%d frames\n", fin.buffered, fin.target);
        std::printf("  rate ratio : %+.1f ppm\n", fin.effectivePpm);
        std::printf("  out rate   : %.1f Hz (nominal %d)\n", fin.outRateHz, cfg.sampleRate);
        return 0;
    }
    // ------------------------------------------------------------- sap-listen
    // Receives SAP announcements and prints them. Proves the announcer works
    // without needing an AES67 receiver on the network.
    if (cmd == "sap-listen") {
        std::string sapErr;
        std::printf("Listening for SAP announcements for %.0f s on 239.255.255.255:9875\n",
                    seconds);
        const int count = listenSap(static_cast<int>(seconds), cfg.ifaceIp,
                                    [](const SapAnnouncement &a) {
                                        // Show the announced creator and, when it
                                        // differs, the peer that delivered the packet.
                                        if (a.from != a.origin) {
                                            std::printf("\n--- SAP %s (via %s)%s ---\n",
                                                        a.origin.c_str(), a.from.c_str(),
                                                        a.deletion ? " (deletion)" : "");
                                        } else {
                                            std::printf("\n--- SAP from %s%s ---\n",
                                                        a.origin.c_str(),
                                                        a.deletion ? " (deletion)" : "");
                                        }
                                        // SDP uses CRLF; a terminal renders the lone
                                        // CR as a blank line, so normalise for display.
                                        std::string printable = a.sdp;
                                        for (size_t i = 0; (i = printable.find("\r\n", i)) != std::string::npos; ) {
                                            printable.replace(i, 2, "\n");
                                        }
                                        std::printf("%s", printable.c_str());
                                    },
                                    &sapErr);
        if (!sapErr.empty()) {
            std::fprintf(stderr, "%s\n", sapErr.c_str());
            return 2;
        }
        std::printf("\n%d announcement(s) received\n", count);
        return count > 0 ? 0 : 1;
    }
    // ----------------------------------------------------------- list-devices
    // Enumerates what the machine offers, including ASIO drivers that this build
    // cannot open yet -- hiding those would leave the user guessing.
    // ------------------------------------------------------------- doctor
    // The checks live in src/doctor.cpp so the GUI runs exactly the same ones.
    // ------------------------------------------------------------------ bench
    // The hot paths, measured. `--write-baseline` records the numbers, and
    // `--check-baseline` fails when one of them regresses, so an optimisation
    // cannot silently make another path slower.
    if (cmd == "bench") {
        const std::vector<BenchCase> cases = runBenchmarks();
        std::printf("%-22s %10s %-12s\n", "case", "value", "unit");
        for (const BenchCase &c : cases) {
            std::printf("%-22s %10.3f %-12s\n", c.name.c_str(), c.value, c.unit.c_str());
        }
        if (!benchBaselinePath.empty()) {
            const bool write = benchWriteBaseline;
            std::string message;
            if (write) {
                if (!writeBenchBaseline(benchBaselinePath, cases, &message)) {
                    std::fprintf(stderr, "%s\n", message.c_str());
                    return 2;
                }
                std::printf("\nbaseline written to %s\n", benchBaselinePath.c_str());
                return 0;
            }
            std::string report;
            const bool ok = checkBenchBaseline(benchBaselinePath, cases, 10.0, &report);
            std::printf("\n%s", report.c_str());
            std::printf("%s\n", ok ? "BASELINE OK" : "BASELINE REGRESSED");
            return ok ? 0 : 1;
        }
        return 0;
    }

    if (cmd == "doctor") {
        DoctorOptions options;
        options.ifaceIp = cfg.ifaceIp;
        options.group = cfg.group;
        options.port = cfg.port;
        options.ptpDomain = ptpDomain;
        const DoctorResult result = runDoctor(options);
        std::printf("OpenAES67 doctor\n");
        for (const DoctorLine &line : result.lines) {
            const char *tag = line.level == DoctorLine::Ok     ? "[ OK ]"
                              : line.level == DoctorLine::Warn ? "[WARN]"
                              : line.level == DoctorLine::Fail ? "[FAIL]"
                                                               : "[....]";
            std::printf("  %s %s\n", tag, line.text.c_str());
        }
        std::printf("\n%s\n", result.ready()
                                ? "READY: no blocking problems found"
                                : "NOT READY: fix the [FAIL] lines above");
        return result.ready() ? 0 : 1;
    }

    // ------------------------------------------------------------------ probe
    // Listens to one multicast address and reports the format it carries. For a
    // Dante AES67 flow that is not SAP-announced, this replaces guessing the
    // channel count and codec.
    if (cmd == "probe") {
        if (!isMulticastIPv4(cfg.group)) {
            std::fprintf(stderr, "group %s is not a multicast address\n", cfg.group.c_str());
            return 2;
        }
        std::printf("probing %s:%u for %.1f s\n", cfg.group.c_str(), cfg.port, seconds);
        const RtpProbeResult probe = probeRtpStream(cfg.ifaceIp, cfg.group, cfg.port, seconds);
        if (!probe.ok) {
            std::fprintf(stderr, "probe failed: %s\n", probe.error.c_str());
            return 1;
        }
        std::printf("  packets    : %d\n", probe.packets);
        std::printf("  format     : %s %d ch @ %d Hz\n", codecName(probe.codec),
                    probe.channels, probe.sampleRate);
        std::printf("  ptime      : %.3f ms (%d frames/packet)\n", probe.ptimeMs,
                    static_cast<int>(probe.ptimeMs * probe.sampleRate / 1000.0 + 0.5));
        std::printf("  payload    : type %u, ssrc 0x%08X\n", probe.payloadType, probe.ssrc);
        std::printf("\nrecv command for this stream:\n");
        std::printf("  openaes67 recv --iface %s --group %s --port %u --channels %d "
                    "--ptime %.3f --codec %s\n",
                    cfg.ifaceIp.c_str(), cfg.group.c_str(), cfg.port, probe.channels,
                    probe.ptimeMs, codecName(probe.codec));
        return 0;
    }

    if (cmd == "list-devices") {
        const auto devices = listAudioOutputDevices();
        std::printf("OpenAES67 audio outputs\n\n");
        int index = 0;
        for (const auto &d : devices) {
            std::printf("  [%2d] %-7s %s%s\n", index, d.backend.c_str(),
                        d.name.c_str(), d.isDefault ? "   (default)" : "");
            ++index;
        }
        if (devices.empty()) {
            std::printf("  (none found)\n");
        }

        std::printf("\nInputs (what tx sends; WASAPI entries are loopback taps):\n\n");
        int inputIndex = 0;
        for (const auto &d : listAudioInputDevices()) {
            std::printf("  [%2d] %-16s %s%s\n", inputIndex, d.backend.c_str(), d.name.c_str(),
                        d.isDefault ? "   (default)" : "");
            ++inputIndex;
        }

        // Recording endpoints, which is a different list again: these are what an
        // application (Audition, a DAW) selects when it records. If the device you
        // want is missing from here, the recorder cannot see it either.
        std::printf("\nCapture endpoints (what a recorder sees; capture-in --in-device):\n\n");
        int captureIndex = 0;
        for (const auto &d : listCaptureEndpoints()) {
            std::printf("  [%2d] %-16s %s%s\n", captureIndex, d.backend.c_str(), d.name.c_str(),
                        d.isDefault ? "   (default)" : "");
            ++captureIndex;
        }

        std::printf("\nASIO backend: %s\n", asioBackendAvailable() ? "built in" : "not built");
        if (!asioBackendAvailable()) {
            std::printf("  %s\n", asioBackendUnavailableReason().c_str());
        }
        std::printf("\nUse --device <index|name> to choose a WASAPI endpoint,\n");
        std::printf("or --asio-driver \"<name>\" for an ASIO device.\n");
        return 0;
    }
    // -------------------------------------------------------------- shm-read
    // Drains a shared section and reports what arrived. Exists so the shared
    // ring can be verified with two real processes, without the kernel driver.
    if (cmd == "shm-read") {
        std::string shmErr;
        auto ring = openSharedAudio(shmName.c_str(), &shmErr);
        if (!ring) {
            std::fprintf(stderr, "shm-read: %s\n", shmErr.c_str());
            return 2;
        }
        std::printf("OpenAES67 shm-read\n");
        std::printf("  section : %ls\n", shmName.c_str());
        std::printf("  geometry: %d ch @ %d Hz, capacity %u frames\n",
                    ring->channels(), ring->sampleRate(), ring->capacityFrames());

        std::vector<int16_t> buf(static_cast<size_t>(ring->capacityFrames()) * ring->channels());
        long long frames = 0;
        long long nonzero = 0;
        double peak = 0.0;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(
                                  static_cast<long long>(seconds * 1000.0));
        // Pace the drain to real time. A reader that drains as fast as it can
        // measures bandwidth, not the ring: it made the writer's controller see a
        // permanent buffer deficit and pin its correction at the clamp
        // (observed: -20000 ppm, mean depth 365 of 1440).
        const auto started = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() < deadline) {
            const int got = ring->readRender(buf.data(), ring->capacityFrames());
            if (got <= 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            frames += got;
            const auto due = started + std::chrono::microseconds(
                static_cast<long long>(frames) * 1000000LL /
                (ring->sampleRate() > 0 ? ring->sampleRate() : 48000));
            std::this_thread::sleep_until(due);
            for (int i = 0; i < got; ++i) {
                const double v = std::abs(static_cast<double>(buf[static_cast<size_t>(i) * ring->channels()]));
                if (v > 0.5) ++nonzero;
                if (v > peak) peak = v;
            }
        }
        const double secs = seconds > 0.0 ? seconds : 1.0;
        std::printf("\nresult\n");
        std::printf("  frames   : %lld (%.0f frames/s)\n", frames, frames / secs);
        std::printf("  nonzero  : %lld samples\n", nonzero);
        std::printf("  peak     : %.0f\n", peak);
        std::printf("  underruns: %u\n", ring->renderUnderruns());
        return 0;
    }
    // ------------------------------------------------------------------- ptp
    // PTPv2 slave: measures the offset and path delay against a grandmaster.
    if (cmd == "ptp") {
        PtpSlave slave;
        std::string err;
        if (!slave.start(cfg.ifaceIp, static_cast<uint8_t>(ptpDomain), &err)) {
            std::fprintf(stderr, "ptp start failed: %s\n", err.c_str());
            return 2;
        }
        std::printf("OpenAES67 ptp (slave)\n");
        std::printf("  iface  : %s\n", cfg.ifaceIp.c_str());
        std::printf("  group  : %s  ports 319/320\n", kPtpDefaultGroup);
        std::printf("  domain : %d\n", ptpDomain);
        std::printf("  window : %.1f s\n", seconds);

        const auto t0 = std::chrono::steady_clock::now();
        auto lastSapAnnounce = t0;
        auto lastRtcp = t0;
        const std::string rtcpCname = "openaes67@" + cfg.ifaceIp;
        const auto deadline = t0 + std::chrono::milliseconds(static_cast<long long>(seconds * 1000.0));
        auto nextDelayReq = t0;
        auto nextPrint = t0 + std::chrono::milliseconds(500);
        int samples = 0;

        while (std::chrono::steady_clock::now() < deadline) {
            const bool newSample = slave.poll(5);
            if (newSample) {
                ++samples;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextDelayReq) {
                slave.sendDelayReq();
                nextDelayReq = now + std::chrono::milliseconds(250);
            }
            if (now >= nextPrint) {
                const auto &st = slave.stats();
                std::printf("  offset=%+10.3f ms  mean=%+10.3f ms  drift=%+8.2f ppm  path=%6.3f ms  sync=%u dresp=%u\n",
                            st.lastOffsetNs / 1e6, st.meanOffsetNs / 1e6,
                            st.freqValid ? st.freqPpm : 0.0,
                            st.pathDelayNs / 1e6, st.syncCount, st.delayRespCount);
                // Show both timebases side by side: the offset above can look perfect
                // while masterTime() sits on a different epoch entirely.
                int64_t masterNs = 0;
                if (slave.masterTimeNs(&masterNs)) {
                    const int64_t localNs = nowAsPtp().toNanos();
                    std::printf("               local=%.6f s  master=%.6f s  diff=%+.3f s\n",
                                localNs / 1e9, masterNs / 1e9,
                                (masterNs - localNs) / 1e9);
                }
                nextPrint = now + std::chrono::milliseconds(500);
            }
        }
        slave.stop();

        const auto &st = slave.stats();
        std::printf("\nresult\n");
        std::printf("  sync messages : %u\n", st.syncCount);
        std::printf("  follow-ups    : %u\n", st.followUpCount);
        std::printf("  delay resps   : %u\n", st.delayRespCount);
        std::printf("  offset samples: %d\n", samples);
        std::printf("  path delay    : %.3f ms\n", st.pathDelayNs / 1e6);
        if (st.haveFirst) {
            std::printf("  offset last   : %+10.3f ms\n", st.lastOffsetNs / 1e6);
            std::printf("  offset mean   : %+10.3f ms\n", st.meanOffsetNs / 1e6);
            std::printf("  offset min    : %+10.3f ms\n", st.minOffsetNs / 1e6);
            std::printf("  offset max    : %+10.3f ms\n", st.maxOffsetNs / 1e6);
            std::printf("  peak-to-peak  : %.3f ms\n", (st.maxOffsetNs - st.minOffsetNs) / 1e6);
            if (st.freqValid) {
                std::printf("  rate offset   : %+10.3f ppm   (master vs local clock)\n", st.freqPpm);
                std::printf("  filtered off  : %+10.3f ms   (drift removed at last sample)\n",
                            st.offsetFilteredNs / 1e6);
            } else {
                std::printf("  rate offset   : not enough samples yet (need >= 4 delay exchanges)\n");
            }
        } else {
            std::printf("  no samples: is a grandmaster sending on this interface?\n");
            std::printf("  try: python tools/ptp_master.py --iface <same ip>\n");
        }
        return 0;
    }
    std::fprintf(stderr, "unknown command: %s\n", cmd.c_str());
    usage();
    return 1;
}
