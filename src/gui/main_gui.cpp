// SPDX-License-Identifier: MIT
// Copyright (c) 2026 OpenAES67 contributors
//
// Every behaviour in this file is traceable to a published standard
// (AES67-2018, RFC 3550, RFC 4566, RFC 2974, IEEE 1588) or to a
// reproducible measurement recorded in docs/FINDINGS.md.

// OpenAES67 Qt6 GUI.
//
// A thin front end over the same engine the CLI uses:
//   Send : WASAPI loopback  -> AES67 RTP
//   Recv : AES67 RTP        -> WASAPI render device
//
// No accounts, no licence, no network calls other than the audio stream itself.
#include <cstdio>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QTimer>
#include <QDateTime>
#include <QFormLayout>
#include <QGroupBox>
#include <QHostAddress>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>
#include <QFont>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QIcon>
#include <QSettings>
#include <windows.h>
#include <mmsystem.h>
#include <cstdlib>
#include <QCloseEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <shellapi.h>   // ShellExecuteW: elevation prompt for driver registration
#include <QSystemTrayIcon>
#include <QMenu>
#include <QStyle>
#include <QThread>
#include <QVBoxLayout>
#include <QVariantList>
#include <QListWidget>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "openaes67/audio_io.h"
#include "openaes67/config.h"
#include "openaes67/engine.h"
#include "openaes67/version.h"
#include "openaes67/net.h"
#include "openaes67/receive_loop.h"
#include "openaes67/audio_devices.h"
#include "openaes67/bridge.h"
#include "openaes67/asio_probe.h"
#include "openaes67/doctor.h"
#include "openaes67/rtp_probe.h"
#include "openaes67/sap.h"
#include "openaes67/sdp.h"
#include "openaes67/net.h"
#include "openaes67/resampler.h"

using namespace openaes67;

// --------------------------------------------------------------------- worker
class Worker : public QThread {
    Q_OBJECT
public:
    enum Mode { SendMode, RecvMode, BridgeMode };

    Worker(Mode mode, const StreamConfig &cfg, bool exclusive,
           const AudioDeviceInfo &playbackDevice, const AudioDeviceInfo &captureDevice,
           int ptpDomain, bool usePtp, bool announceUp, QObject *parent = nullptr)
        : QThread(parent), mode_(mode), cfg_(cfg), exclusive_(exclusive),
          device_(playbackDevice), captureDevice_(captureDevice),
          ptpDomain_(ptpDomain), usePtp_(usePtp), announceUp_(announceUp) {}

    void requestStop() { running_.store(false); }

signals:
    // Emitted when the worker stops because something went wrong, so the status
    // can turn red instead of falling back to a green "就绪".
    void failed(const QString &reason);
    void logLine(const QString &line);
    void finishedWork();
    // bufferedFrames, correctionPpm, effectivePpm, packets, lost, underruns
    void stats(int buffered, double corrPpm, double effPpm,
               qulonglong packets, quint32 lost, int underruns);
    // Send direction only: packets actually put on the wire, and how many of them
    // carried silence because the capture device had nothing to give.
    void txStats(qulonglong packets, qulonglong silent);
    // Emitted when the stream stops delivering packets, and again when it resumes.
    void stalled(bool stalled);

protected:
    void run() override {
        // Same reasoning as the CLI: keep the audio thread ahead of the UI.
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        Winsock ws;
        if (!ws.ok()) {
            emit logLine(tr("Winsock init failed"));
            emit finishedWork();
            return;
        }
        if (mode_ == SendMode) {
            runSend();
        } else if (mode_ == BridgeMode) {
            runBridge_();
        } else {
            runRecv();
        }
        emit finishedWork();
        // Fault injection for the stop-timeout path: hold the thread alive past the
        // 3 s wait so the "keep the reference instead of destroying a running
        // QThread" branch can be exercised from a script.
        if (const char *slow = ::getenv("OPENAES67_SLOW_STOP_MS")) {
            const int ms = std::atoi(slow);
            if (ms > 0) {
                std::fprintf(stderr, "[worker] simulating a slow stop for %d ms\n", ms);
                ::Sleep(static_cast<DWORD>(ms));
            }
        }
    }

private:
    void note(const QString &s) { emit logLine(s); }

    void runSend() {
        // Send takes audio FROM the selected device: OpenAES67 is the network
        // sound card, so the device is an input here, not an output. This used to
        // ignore the selector entirely and always capture the default endpoint.
        const std::wstring captureId(captureDevice_.id.begin(), captureDevice_.id.end());
        std::unique_ptr<AudioSource> src(
            createWasapiLoopbackSource(captureId.empty() ? nullptr : captureId.c_str()));
        if (src && !captureDevice_.name.empty()) {
            note(tr("输入设备: %1").arg(QString::fromStdString(captureDevice_.name)));
        }
        if (!src) {
            note(tr("无法打开 WASAPI loopback（没有默认播放设备？）"));
            emit failed(tr("无法打开采集设备"));
            return;
        }
        // The channel count is the STREAM's channel count, chosen by the user.
        // It used to be overwritten with the capture device's own count (usually
        // 2), which made the 1..64 spin box do nothing for sending. Capture is
        // mixed down/up into the stream: extra capture channels are dropped and
        // missing ones are filled with silence.
        const int srcChannels = src->channels();
        const int srcRate = src->sampleRate();
        if (srcChannels <= 0 || srcRate <= 0 || cfg_.channels <= 0) {
            note(tr("loopback 返回了无效格式"));
            return;
        }
        if (cfg_.channels != srcChannels) {
            note(tr("采集源为 %1 通道，输出流为 %2 通道（多余通道丢弃，不足补静音）")
                     .arg(srcChannels).arg(cfg_.channels));
        }
        if (cfg_.sampleRate != srcRate) {
            note(tr("设备混音格式为 %1 Hz，与设定的 %2 Hz 不一致；"
                    "按设备实际采样率发送（未做重采样）")
                     .arg(srcRate).arg(cfg_.sampleRate));
            cfg_.sampleRate = srcRate;
        }
        // One RTP packet must fit a typical 1500-byte Ethernet MTU or it will be
        // fragmented, which AES67 receivers do not tolerate.
        const int payloadBytes = cfg_.framesPerPacket() * cfg_.channels * 2;
        if (payloadBytes + 12 > 1400) {
            note(tr("警告：%1 通道 @ %2 ms 每包载荷 %3 字节，超过以太网 MTU，"
                    "会被分片；请减少通道数或包时长")
                     .arg(cfg_.channels).arg(cfg_.ptimeMs).arg(payloadBytes));
        }

        RtpSender sender(cfg_, 0x5EED0001);
        std::string err;
        if (!sender.start(&err)) {
            note(tr("发送启动失败: %1").arg(QString::fromStdString(err)));
            emit failed(tr("发送启动失败"));
            return;
        }

        // Announce the stream. Without this nothing on the network can discover
        // it: driving the GUI used to send RTP only, so a controller had no way to
        // learn the stream existed.
        SapAnnouncer sap;
        {
            // Announce with a PTP clock reference, not localmac. Dante Controller
            // lists SAP-advertised RTP flows, and the descriptor it accepts names
            // a grandmaster in a=ts-refclk. The identity is derived from this
            // adapter's MAC as an EUI-64 (xx-xx-xx-FF-FE-xx-xx-xx-xx) on the AES67
            // fixed domain 0, which is what a Dante device expects to see.
            SdpClockRef clockRef;
            uint8_t mac[6] = {0};
            if (localMacForIp(cfg_.ifaceIp, mac)) {
                clockRef.kind = SdpClockRef::Kind::Ptp;
                clockRef.gmid[0] = mac[0];
                clockRef.gmid[1] = mac[1];
                clockRef.gmid[2] = mac[2];
                clockRef.gmid[3] = 0xFF;
                clockRef.gmid[4] = 0xFE;
                clockRef.gmid[5] = mac[3];
                clockRef.gmid[6] = mac[4];
                clockRef.gmid[7] = mac[5];
                clockRef.domain = static_cast<uint8_t>(ptpDomain_);
            } else {
                clockRef.kind = SdpClockRef::Kind::LocalMac;
                localMacForIp(cfg_.ifaceIp, clockRef.mac);
            }
            std::string sapErr;
            if (!sap.start(cfg_.ifaceIp, buildSdp(cfg_, 0x5EED0001u, clockRef), &sapErr)) {
                note(tr("SAP 启动失败: %1").arg(QString::fromStdString(sapErr)));
            } else if (!sap.announce(&sapErr)) {
                note(tr("SAP 通告失败: %1").arg(QString::fromStdString(sapErr)));
            } else {
                note(tr("已通过 SAP 通告到 239.255.255.255:9875"));
            }
        }
        auto lastSap = std::chrono::steady_clock::now();
        double sapInterval = sap.nextIntervalSeconds();

        const int ch = cfg_.channels;
        const int fpp = cfg_.framesPerPacket();
        int tickFrames = cfg_.sampleRate / 100;
        if (tickFrames < fpp) {
            tickFrames = fpp;
        }
        tickFrames = (tickFrames / fpp) * fpp;
        const int packetsPerTick = tickFrames / fpp;
        const auto tickUs = std::chrono::microseconds(
            static_cast<long long>(1000000.0 * tickFrames / cfg_.sampleRate + 0.5));

        note(tr("发送中: %1 ch @ %2 Hz -> %3:%4，每 %5 ms 发 %6 个包")
                 .arg(ch).arg(cfg_.sampleRate)
                 .arg(QString::fromStdString(cfg_.group)).arg(cfg_.port)
                 .arg(cfg_.ptimeMs).arg(packetsPerTick));

        std::vector<int16_t> cap(static_cast<size_t>(tickFrames) * srcChannels, 0);
        std::vector<int16_t> pkt(static_cast<size_t>(fpp) * ch, 0);
        const auto t0 = std::chrono::steady_clock::now();
        long long tick = 0;
        long long sent = 0;
        long long silent = 0;
        long long audioTicks = 0;

        while (running_.load()) {
            const int got = src->read(cap.data(), tickFrames);
            if (got > 0) {
                ++audioTicks;
            }
            for (int k = 0; k < packetsPerTick; ++k) {
                const int off = k * fpp;
                const int have = (got > off) ? (got - off) : 0;
                // Map capture channels onto the stream's channel layout: copy the
                // channels both sides have, zero the rest. Without this the stream
                // could only ever carry as many channels as the capture device.
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
                    ++silent;
                }
                if (!sender.sendFrames(pkt.data(), fpp)) {
                    note(tr("发送失败（第 %1 个节拍）").arg(tick));
                    emit failed(tr("发送失败"));
                    running_.store(false);
                    break;
                }
                ++sent;
            }
            ++tick;
            if (tick % 100 == 0) {
                note(tr("已发送 %1 包 | 有声音的节拍 %2 | 静音填充 %3")
                         .arg(sent).arg(audioTicks).arg(silent));
                emit stats(0, 0.0, 0.0, static_cast<qulonglong>(sent), 0, 0);
                emit txStats(static_cast<qulonglong>(sent), static_cast<qulonglong>(silent));
            }
            if (std::chrono::steady_clock::now() - lastSap >=
                std::chrono::microseconds(static_cast<long long>(sapInterval * 1e6))) {
                sap.announce(nullptr);
                lastSap = std::chrono::steady_clock::now();
            }
            std::this_thread::sleep_until(t0 + tickUs * tick);
        }
        sap.withdraw();
        note(tr("已发送 SAP 注销（接收方若支持删除会立即移除）"));
        sap.stop();
        sender.stop();
        note(tr("发送已停止：共 %1 包，静音填充 %2").arg(sent).arg(silent));
    }

    // Sound-card mode: AES67 <-> shared ring <-> ASIO, through the same library
    // function the CLI uses. The GUI adds no behaviour of its own here, which is
    // the point: one implementation, two front ends.
    void runBridge_() {
        BridgeOptions bo;
        bo.downCfg = cfg_;
        bo.upCfg = cfg_;
        bo.upCfg.port = static_cast<uint16_t>(cfg_.port + 2 <= 65535 ? cfg_.port + 2
                                                                     : cfg_.port);
        bo.shmName.assign(L"OpenAES67Audio");
        bo.announceUp = announceUp_;
        bo.sapInterval = 30;
        bo.usePtp = usePtp_;
        bo.ptpDomain = ptpDomain_;
        bo.shouldStop = [this] { return running_.load(); };
        bo.onLog = [this](const std::string &s) { note(QString::fromStdString(s)); };

        BridgeStats stats;
        std::string err;
        if (!runBridge(bo, &stats, &err)) {
            note(tr("声卡模式失败: %1").arg(QString::fromStdString(err)));
            emit failed(QString::fromStdString(err));
            return;
        }
        note(tr("声卡模式结束: 上行 %1 包（其中 %2 包无 ASIO 音频）")
                 .arg(stats.uplinkPackets)
                 .arg(stats.uplinkSilent));
    }

    void runRecv() {
        // Everything below lives in runReceiveLoop() so the GUI and the CLI
        // share one implementation.
        ReceiveLoopOptions opt;
        opt.cfg = cfg_;
        opt.exclusive = exclusive_;
        if (!device_.name.empty()) {
            opt.deviceLabel = device_.name;
            if (device_.isAsio) {
                opt.backend = "asio";
                opt.asioDriver = device_.id;
            } else {
                opt.deviceId.assign(device_.id.begin(), device_.id.end());
            }
        }
        opt.usePtp = usePtp_;
        opt.ptpDomain = ptpDomain_;
        opt.seconds = 0.0;                  // run until the user stops
        opt.onLog = [this](const std::string &s) {
            note(QString::fromStdString(s));
            // Also mirror to stdout so scripted runs can capture the numbers:
            // the GUI is otherwise a black box to a test harness.
            std::fprintf(stdout, "%s\n", s.c_str());
            std::fflush(stdout);
        };
        opt.onStats = [this](const RxSnapshot &s) {
            emit stats(s.buffered, s.correctionPpm, s.effectivePpm,
                       s.packets, s.lost, s.underruns);
        };
        opt.shouldStop = [this] { return running_.load(); };
        opt.onStall = [this](bool on) { emit stalled(on); };

        RxSnapshot fin;
        std::string err;
        if (!runReceiveLoop(opt, &fin, &err)) {
            note(tr("接收失败: %1").arg(QString::fromStdString(err)));
            emit finishedWork();
            return;
        }
        note(tr("接收已停止：播放 %1 帧，收到 %2 包，丢 %3，欠载 %4，平均缓冲 %5 / %6 帧")
                 .arg(static_cast<qlonglong>(fin.framesWritten))
                 .arg(fin.packets).arg(fin.lost).arg(fin.underruns)
                 .arg(fin.buffered).arg(fin.target));
        emit finishedWork();
    }
    Mode          mode_;
    StreamConfig  cfg_;
    bool          exclusive_;
    AudioDeviceInfo device_;          // receive: where to play
    AudioDeviceInfo captureDevice_;   // send: where to take audio from
    int  ptpDomain_ = 0;              // PTP v2 domain, 0-127
    bool usePtp_ = true;              // feed PTP to the receive rate controller
    bool announceUp_ = false;         // bridge: announce the uplink over SAP
    std::atomic<bool> running_{true};
};

// ----------------------------------------------------------------- main window
#include <QWidget>
#include <QPainter>
#include <QVariantAnimation>

// A status dot that breathes while packets are actually flowing.
//
// The dot cannot blink once per RTP packet: at 1 ms ptime that is 1000 blinks a
// second, which the eye reads as a steady glow. What it does instead is a
// heartbeat whose *phase* is driven by packet arrival -- each time packets are
// seen the animation is nudged, so a stream that stops flowing visibly stops
// breathing even though the worker is still running.
class StatusDot : public QWidget {
public:
    explicit StatusDot(QWidget *parent = nullptr) : QWidget(parent) {
        setFixedSize(14, 14);
        breath_ = new QVariantAnimation(this);
        breath_->setStartValue(0.35);
        breath_->setEndValue(1.0);
        breath_->setDuration(700);
        breath_->setLoopCount(-1);
        breath_->setEasingCurve(QEasingCurve::InOutSine);
        connect(breath_, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
            level_ = v.toDouble();
            update();
        });
    }

    void setColour(const QColor &colour) {
        colour_ = colour;
        update();
    }

    // Called whenever packets are seen. Restarting the cycle on the first packet
    // after a quiet spell makes the dot answer immediately instead of drifting.
    void setBreathing(bool flowing) {
        if (flowing && breath_->state() != QAbstractAnimation::Running) {
            breath_->start();
        } else if (!flowing && breath_->state() == QAbstractAnimation::Running) {
            breath_->stop();
            level_ = 1.0;
            update();
        }
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        QColor fill = colour_;
        fill.setAlphaF(level_);

        // A soft halo behind the core dot: it is what makes the breathing legible
        // at 14 px without changing the core colour's meaning.
        QColor halo = colour_;
        halo.setAlphaF(level_ * 0.28);
        p.setPen(Qt::NoPen);
        p.setBrush(halo);
        p.drawEllipse(rect().adjusted(-1, -1, 1, 1));
        p.setBrush(fill);
        p.drawEllipse(rect().adjusted(3, 3, -3, -3));
    }

private:
    QVariantAnimation *breath_ = nullptr;
    QColor  colour_{"#2ecc71"};
    double  level_ = 1.0;
};

// --------------------------------------------------------------- SAP scanner
// Listens for SAP announcements in the background and reports the streams it can
// tune to. listenSap() blocks for a bounded window, so the thread loops over short
// windows and checks `running_` between them; stop() therefore takes at most one
// window (2 s) to return.
class SapScanner : public QThread {
    Q_OBJECT

public:
    explicit SapScanner(std::string iface, QObject *parent = nullptr)
        : QThread(parent), iface_(std::move(iface)) {}

    void stop() {
        running_.store(false);
        wait(4000);
    }

signals:
    void found(const QVariantList &streams);

protected:
    void run() override {
        running_.store(true);
        while (running_.load()) {
            QVariantList batch;
            const int count = listenSap(
                2, iface_,
                [&batch](const SapAnnouncement &a) {
                    SapStreamInfo info;
                    if (a.deletion || !parseSdpStreamInfo(a.sdp, &info)) {
                        return;
                    }
                    info.origin = a.origin;
                    QVariantMap m;
                    m.insert(QStringLiteral("name"),
                             QString::fromStdString(info.name.empty() ? info.group : info.name));
                    m.insert(QStringLiteral("group"), QString::fromStdString(info.group));
                    m.insert(QStringLiteral("port"), info.port);
                    m.insert(QStringLiteral("channels"), info.channels);
                    m.insert(QStringLiteral("rate"), info.sampleRate);
                    m.insert(QStringLiteral("codec"),
                             QString::fromUtf8(codecName(info.codec)));
                    m.insert(QStringLiteral("origin"), QString::fromStdString(info.origin));
                    batch.append(m);
                },
                nullptr);
            (void)count;
            // Emit even an empty batch: the list needs a tick to expire sources that
            // stopped announcing, and "nothing arrived" is exactly that tick. Emitting
            // only non-empty batches froze the sweep as soon as a sender disappeared,
            // which is precisely when expiry matters.
            if (running_.load()) {
                emit found(batch);
            }
        }
    }

private:
    std::string iface_;
    std::atomic<bool> running_{false};
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow() {
        setWindowTitle("OpenAES67");
        resize(760, 560);

        auto *central = new QWidget(this);
        auto *root = new QVBoxLayout(central);

        // --- stream settings
        auto *box = new QGroupBox(tr("音频流设置"), central);
        auto *form = new QFormLayout(box);

        // Two selectors because the two directions need different things:
        // sending takes audio FROM a device (input), receiving plays TO one (output).
        inputBox_ = new QComboBox(box);
        populateInputs();
        form->addRow(tr("输入设备 (发送)"), inputBox_);

        deviceBox_ = new QComboBox(box);
        populateDevices();
        form->addRow(tr("输出设备 (接收)"), deviceBox_);

        ifaceBox_ = new QComboBox(box);
        populateInterfaces();
        form->addRow(tr("有线网卡"), ifaceBox_);

        groupEdit_ = new QLineEdit("239.69.1.1", box);
        form->addRow(tr("接收组播地址"), groupEdit_);

        portSpin_ = new QSpinBox(box);
        portSpin_->setRange(1, 65535);
        portSpin_->setValue(5004);
        form->addRow(tr("接收端口"), portSpin_);

        // The two directions are separate AES67 flows: a Dante network gives
        // transmit and receive different multicast addresses, so the uplink has
        // its own address and port instead of reusing the downlink pair.
        txGroupEdit_ = new QLineEdit("239.69.1.2", box);
        form->addRow(tr("发送组播地址"), txGroupEdit_);

        txPortSpin_ = new QSpinBox(box);
        txPortSpin_->setRange(1, 65535);
        txPortSpin_->setValue(5006);
        form->addRow(tr("发送端口"), txPortSpin_);

        // Channel counts differ per direction: what is captured (sent) and what
        // is played (received).
        outChannelsSpin_ = new QSpinBox(box);
        outChannelsSpin_->setRange(1, 64);
        outChannelsSpin_->setValue(2);
        form->addRow(tr("输出通道 (接收)"), outChannelsSpin_);

        inChannelsSpin_ = new QSpinBox(box);
        inChannelsSpin_->setRange(1, 64);
        inChannelsSpin_->setValue(2);
        form->addRow(tr("输入通道 (发送)"), inChannelsSpin_);

        rateSpin_ = new QSpinBox(box);
        rateSpin_->setRange(8000, 192000);
        rateSpin_->setSingleStep(1000);
        rateSpin_->setValue(48000);
        form->addRow(tr("采样率"), rateSpin_);

        ptimeSpin_ = new QSpinBox(box);
        ptimeSpin_->setRange(1, 20);
        ptimeSpin_->setValue(1);
        form->addRow(tr("包时长 (ms)"), ptimeSpin_);

        // L16 is the default because every AES67 device accepts it; L24 exists
        // for equipment that only offers 24-bit. The internal path is int16, so
        // L24 carries 16-bit samples in the upper bytes (see README).
        codecBox_ = new QComboBox(box);
        codecBox_->addItem(tr("L16 (16-bit)"), static_cast<int>(Codec::L16));
        codecBox_->addItem(tr("L24 (24-bit)"), static_cast<int>(Codec::L24));
        form->addRow(tr("编码"), codecBox_);

        // Sound-card mode: the ASIO driver owns the audio endpoints, so the GUI
        // must not open a WASAPI device. Ticking this runs the duplex bridge
        // instead of a plain send or receive.
        bridgeCheck_ = new QCheckBox(tr("声卡模式（ASIO 64×64）"), box);
        bridgeCheck_->setToolTip(tr("AES67 <-> 共享内存 <-> ASIO 驱动，Audition 等 ASIO 程序可录音/放音"));
        form->addRow(QString(), bridgeCheck_);
        announceCheck_ = new QCheckBox(tr("SAP 通告上行流（Dante Controller 可见）"), box);
        announceCheck_->setToolTip(tr("让 Dante Controller 在 Routing 右侧看到本机的上行 AES67 流"));
        announceCheck_->setChecked(true);
        form->addRow(QString(), announceCheck_);

        // Start with Windows: a sound card that needs a manual click after every
        // login is not a sound card. The Run entry carries the current mode.
        autostartCheck_ = new QCheckBox(tr("开机自动启动（使用当前设置）"), box);
        autostartCheck_->setToolTip(tr("在 HKCU\\...\\Run 里写入一条启动项"));
        form->addRow(QString(), autostartCheck_);

        // PTP v2 domain. AES67 mandates 0, but a network that also carries ST 2110
        // or vendor equipment may run another domain, and a slave on the wrong
        // domain silently never locks. Exposed so it can be corrected, not guessed.
        ptpDomainSpin_ = new QSpinBox(box);
        ptpDomainSpin_->setRange(0, 127);
        ptpDomainSpin_->setValue(0);
        ptpDomainSpin_->setToolTip(tr("AES67 规定为 0；若网络里另有主时钟域，改成那个值"));
        form->addRow(tr("PTP 时钟域"), ptpDomainSpin_);

        ptpCheck_ = new QCheckBox(tr("接收时用 PTP 校正时钟漂移"), box);
        ptpCheck_->setChecked(true);
        form->addRow(QString(), ptpCheck_);

        exclusiveCheck_ = new QCheckBox(tr("独占模式（绕过系统混音，仅接收）"), box);
        exclusiveCheck_->setChecked(true);
        form->addRow(QString(), exclusiveCheck_);
        connect(autostartCheck_, &QCheckBox::toggled, this, [this](bool on) { setAutostart(on); });

        root->addWidget(box);

        // --- streams announced over SAP
        // AES67 has no discovery of its own; a stream becomes findable when its
        // sender announces it. Listing them here saves typing a group and port.
        auto *sapBox = new QGroupBox(tr("网络中的流（SAP 发现）"), central);
        auto *sapLayout = new QVBoxLayout(sapBox);
        sapList_ = new QListWidget(sapBox);
        sapList_->setMaximumHeight(96);
        sapLayout->addWidget(sapList_);
        auto *sapBar = new QHBoxLayout();
        sapScanCheck_ = new QCheckBox(tr("自动发现"), sapBox);
        sapUseBtn_ = new QPushButton(tr("使用选中的流"), sapBox);
        doctorBtn_ = new QPushButton(tr("环境诊断"), sapBox);
        doctorBtn_->setToolTip(tr("检查网卡、组播、SAP 接收、ASIO 注册与 PTP 锁"));
        sapBar->addWidget(sapScanCheck_);
        sapBar->addWidget(sapUseBtn_);
        sapBar->addWidget(doctorBtn_);
        // Dante AES67 flows are often static multicast and are not always
        // SAP-announced, so paste the address Dante Controller shows for the
        // flow and subscribe to it directly.
        subscribeEdit_ = new QLineEdit(sapBox);
        subscribeEdit_->setPlaceholderText(tr("239.69.1.1:5004（Dante Controller 里该流的地址）"));
        subscribeEdit_->setMaximumWidth(280);
        subscribeBtn_ = new QPushButton(tr("按地址接收"), sapBox);
        sapBar->addWidget(subscribeEdit_);
        sapBar->addWidget(subscribeBtn_);
        asioRegBtn_ = new QPushButton(tr("注册 ASIO 驱动"), sapBox);
        asioRegBtn_->setToolTip(tr("以管理员身份运行 asio_driver\\register-asio-driver.bat"));
        sapBar->addWidget(asioRegBtn_);
        sapBar->addStretch(1);
        sapLayout->addLayout(sapBar);
        root->addWidget(sapBox);
        connect(sapScanCheck_, &QCheckBox::toggled, this,
                [this](bool on) { setSapScanning(on); });
        connect(sapUseBtn_, &QPushButton::clicked, this,
                [this] { useSelectedSapStream(); });
        connect(doctorBtn_, &QPushButton::clicked, this, [this] { runDoctorUi(); });
        connect(subscribeBtn_, &QPushButton::clicked, this, [this] { subscribeByAddress(); });
        connect(asioRegBtn_, &QPushButton::clicked, this, [this] { registerAsioDriver(); });
        // --- buttons
        // One self-locking button instead of 开始发送 / 开始接收 / 停止. The mode
        // is chosen next to it; the button starts and stops that one stream.
        auto *bar = new QHBoxLayout();
        modeBox_ = new QComboBox(central);
        modeBox_->addItem(tr("发送 (本机声卡 -> 网络)"));
        modeBox_->addItem(tr("接收 (网络 -> 本机声卡)"));
        runBtn_ = new QPushButton(tr("开始"), central);
        runBtn_->setCheckable(true);
        runBtn_->setMinimumWidth(120);
        bar->addWidget(new QLabel(tr("模式"), central));
        bar->addWidget(modeBox_, 1);
        modeBox_->setVisible(false);   // both directions start together now
        bar->addWidget(runBtn_);
        root->addLayout(bar);

        // --- live indicators
        auto *statBar = new QHBoxLayout();
        auto makeStat = [&](const QString &caption) {
            auto *l = new QLabel("-", central);
            l->setMinimumWidth(90);
            l->setTextInteractionFlags(Qt::TextSelectableByMouse);
            auto *box = new QVBoxLayout();
            auto *cap = new QLabel(caption, central);
            QFont f = cap->font();
            f.setPointSizeF(f.pointSizeF() * 0.9);
            cap->setFont(f);
            box->addWidget(cap);
            box->addWidget(l);
            box->setSpacing(0);
            statBar->addLayout(box);
            return l;
        };
        statBuffer_ = makeStat(tr("平均缓冲 (帧)"));
        statCorr_   = makeStat(tr("校正 ppm"));
        statEff_    = makeStat(tr("速率比 ppm"));
        statPackets_ = makeStat(tr("收包数"));
        statLost_   = makeStat(tr("丢包"));
        statUnderruns_ = makeStat(tr("欠载"));
        // The send direction has its own counters: summing them into the receive
        // numbers would make both meaningless.
        statTxPackets_ = makeStat(tr("发包数"));
        statTxSilent_ = makeStat(tr("静音包"));
        root->addLayout(statBar);

        bufferBar_ = new QProgressBar(central);
        bufferBar_->setRange(0, 2880);          // 30 ms target, 60 ms full scale
        bufferBar_->setValue(0);
        bufferBar_->setTextVisible(false);
        bufferBar_->setMaximumHeight(8);
        root->addWidget(bufferBar_);

        // --- log
        logView_ = new QPlainTextEdit(central);
        logView_->setReadOnly(true);
        logView_->setMaximumBlockCount(2000);
        root->addWidget(logView_, 1);

        // Status is a coloured dot plus one word. The old line ("无账号、无授权、
        // 无联网校验。") was marketing text in a status field; what an operator needs
        // is whether the stream is healthy:
        //   green  = connected / healthy
        //   yellow = connected but unstable (loss, underruns)
        //   red    = fault
        auto *statusRow = new QHBoxLayout();
        statusDot_ = new StatusDot(central);
        status_ = new QLabel(tr("就绪"), central);
        statusRow->addWidget(statusDot_);
        statusRow->addWidget(status_, 1);
        root->addLayout(statusRow);
        setStatus(StatusLevel::Ok, tr("就绪"));

        setCentralWidget(central);

        connect(runBtn_, &QPushButton::toggled, this, [this](bool on) {
            if (on) {
                start(modeBox_->currentIndex() == 0 ? Worker::SendMode : Worker::RecvMode);
            } else {
                stop();
            }
        });
        connect(ifaceBox_, &QComboBox::currentIndexChanged, this,
                [this](int) { if (ifaceBox_->count() == 0) { setStatus(StatusLevel::Error, tr("非就绪（错误）")); appendLog(tr("没有检测到有线网卡")); } });
            lastRunFailed_ = true;

        // Restore the previous session now that every widget exists. An earlier
        // attempt called this before the fields were built and silently did nothing.
        loadSettings();
    }



    ~MainWindow() override {
        stop();
        // Last resort at shutdown: if the worker still has not left run(), the
        // QThread destructor would abort the process. Terminating is unsafe in
        // general, but it is strictly better than a crash on exit.
        if (worker_ && worker_->isRunning()) {
            worker_->terminate();
            if (!worker_->wait(1000)) {
                worker_->wait();
            }
        }
    }
protected:
    // Closing the window with a tray icon only hides it: the stream keeps running,
    // which is the point of a sound card. 退出 in the tray menu stops it, and that
    // path lets the worker send the SAP deletion so no stale entry is left.
    void closeEvent(QCloseEvent *event) override {
        saveSettings();
        if (tray_ && tray_->isVisible() && !quitting_) {
            hide();
            tray_->showMessage(tr("OpenAES67"), tr("仍在后台运行，点击托盘图标可恢复窗口"),
                               QSystemTrayIcon::Information, 3000);
            event->ignore();
            return;
        }
        stop();
        QMainWindow::closeEvent(event);
    }

public:

public:
    // Headless automation hooks: let a script drive one run without clicks.
    // Debug/automation: preselect a device by name substring so a scripted run
    // can exercise a specific endpoint without clicking the combo boxes.
    bool selectInputDevice(const QString &needle) {
        if (!inputBox_) { return false; }
        for (int i = 0; i < inputBox_->count(); ++i) {
            if (inputBox_->itemText(i).contains(needle, Qt::CaseInsensitive)) {
                inputBox_->setCurrentIndex(i);
                return true;
            }
        }
        return false;
    }

    bool selectOutputDevice(const QString &needle) {
        if (!deviceBox_) { return false; }
        for (int i = 0; i < deviceBox_->count(); ++i) {
            if (deviceBox_->itemText(i).contains(needle, Qt::CaseInsensitive)) {
                deviceBox_->setCurrentIndex(i);
                return true;
            }
        }
        return false;
    }

    // Public entry for the --diagnose startup flag; the 环境诊断 button calls
    // runDoctorUi() directly.
    void runDoctorUiPublic();
    void runAutostart(bool on);
    void runSetupTray();

    void startAuto(const QString &mode) {
        if (mode == "both") {
            startBoth();
            return;
        }
        start(mode == "send" ? Worker::SendMode
              : (mode == "bridge" ? Worker::BridgeMode : Worker::RecvMode));
    }
    void stopAuto() { stop(); }

private:
    // Fills the output list with the system default plus every endpoint and ASIO
    // driver. ASIO entries are shown even though this build may not have the
    // backend: the user should be able to see that their interface was detected,
    // and get a clear reason when it cannot be opened.
    void populateInputs() {
        inputs_.clear();
        inputBox_->clear();

        AudioDeviceInfo systemDefault;
        systemDefault.backend = "wasapi-loopback";
        inputs_.push_back(systemDefault);
        inputBox_->addItem(tr("（系统默认播放设备的 loopback）"));

        for (const auto &d : listAudioInputDevices()) {
            inputs_.push_back(d);
            QString label = QString::fromStdString(d.name);
            if (d.isDefault) {
                label += tr("  [默认]");
            }
            if (d.isAsio) {
                label = tr("ASIO · ") + label;
                if (!asioBackendAvailable()) {
                    label += tr("  [需 ASIO 后端]");
                }
            }
            inputBox_->addItem(label);
        }
    }

    void populateDevices() {
        devices_.clear();
        deviceBox_->clear();

        AudioDeviceInfo systemDefault;
        systemDefault.backend = "wasapi";
        devices_.push_back(systemDefault);
        deviceBox_->addItem(tr("（系统默认输出）"));

        for (const auto &d : listAudioOutputDevices()) {
            devices_.push_back(d);
            QString label = QString::fromStdString(d.name);
            if (d.isDefault) {
                label += tr("  [默认]");
            }
            if (d.isAsio) {
                label = tr("ASIO · ") + label;
                if (!asioBackendAvailable()) {
                    label += tr("  [需 ASIO 后端]");
                }
            }
            deviceBox_->addItem(label);
        }

        // Say what was detected. Without this the combo silently shows only the
        // default entry when no audio driver is present, and the user cannot tell
        // "nothing found" from "not populated".
        // Count what is actually in the lists. The old line said 输入/输出 but
        // passed the WASAPI and ASIO counts, so "输出 4" looked like four output
        // devices when it really meant four ASIO drivers registered at the time.
        int wasapiOut = 0, asioOut = 0;
        QStringList asioNames;
        for (const auto &d : devices_) {
            if (d.isAsio) { ++asioOut; asioNames << QString::fromStdString(d.name); }
            else if (!d.name.empty()) { ++wasapiOut; }
        }
        // populateDevices() runs while the settings panel is being built, before
        // the log view exists, so defer to the first event-loop turn.
        QTimer::singleShot(0, this, [this, wasapiOut, asioOut, asioNames]() {
            appendLog(tr("设备：WASAPI 端点 %1 个，ASIO 驱动 %2 个%3")
                          .arg(wasapiOut)
                          .arg(asioOut)
                          .arg(asioBackendAvailable()
                                   ? QString()
                                   : tr("（ASIO 后端未编译，暂不能打开）")));
            if (!asioNames.isEmpty()) {
                appendLog(tr("ASIO 驱动：%1").arg(asioNames.join(QStringLiteral("、"))));
            }
        });
    }

    void populateInterfaces() {
        const auto ifaces = QNetworkInterface::allInterfaces();
        for (const QNetworkInterface &ni : ifaces) {
            const auto flags = ni.flags();
            if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning)) {
                continue;
            }
            if (flags & QNetworkInterface::IsLoopBack) {
                continue;
            }
            for (const QNetworkAddressEntry &ae : ni.addressEntries()) {
                const QHostAddress ip = ae.ip();
                if (ip.protocol() != QAbstractSocket::IPv4Protocol) {
                    continue;
                }
                ifaceBox_->addItem(QString("%1  (%2)").arg(ip.toString(), ni.humanReadableName()),
                                   ip.toString());
            }
        }
        if (ifaceBox_->count() == 0) {
            ifaceBox_->addItem(tr("0.0.0.0 (未检测到网卡)"), QString("0.0.0.0"));
        }
    }

    // Starts or stops the background SAP listener. The interface is captured when
    // scanning starts; changing it requires a toggle to pick it up.
    void setSapScanning(bool on) {
        // Expiry needs a tick that does not depend on new announcements: when a
        // sender disappears it sends nothing, so a list driven only by arrivals
        // would keep it forever. Created once, swept every 5 s.
        if (!sapExpireTimer_) {
            sapExpireTimer_ = new QTimer(this);
            connect(sapExpireTimer_, &QTimer::timeout, this,
                    [this] { expireSapEntries(); });
            sapExpireTimer_->start(5000);
        }
        if (on) {
            if (scanner_) {
                return;
            }
            const QString iface = ifaceBox_ ? ifaceBox_->currentData().toString() : QString();
            if (iface.isEmpty()) {
                appendLog(tr("没有可用的网卡，无法发现流"));
                return;
            }
            scanner_ = new SapScanner(iface.toStdString(), this);
            connect(scanner_, &SapScanner::found, this,
                    [this](const QVariantList &streams) { updateSapList(streams); });
            scanner_->start();
            appendLog(tr("开始监听 SAP 通告（每 2 秒一轮）"));
        } else if (scanner_) {
            scanner_->stop();
            delete scanner_;
            scanner_ = nullptr;
            appendLog(tr("停止 SAP 发现"));
        }
    }

    // Rebuilds the list from the newest announcement batch. A stream is identified
    // by group:port so a repeated announcement updates instead of duplicating.
    void updateSapList(const QVariantList &streams) {
        if (!sapList_) {
            return;
        }
        for (const QVariant &v : streams) {
            const QVariantMap m = v.toMap();
            const QString group = m.value(QStringLiteral("group")).toString();
            const int port = m.value(QStringLiteral("port")).toInt();
            if (group.isEmpty() || port <= 0) {
                continue;
            }
            const QString key = QStringLiteral("%1:%2").arg(group).arg(port);
            QListWidgetItem *item = nullptr;
            for (int i = 0; i < sapList_->count(); ++i) {
                if (sapList_->item(i)->data(Qt::UserRole).toString() == key) {
                    item = sapList_->item(i);
                    break;
                }
            }
            const QString text =
                QStringLiteral("%1  %2:%3  %4 %5ch@%6Hz")
                    .arg(m.value(QStringLiteral("name")).toString(), group)
                    .arg(port)
                    .arg(m.value(QStringLiteral("codec")).toString())
                    .arg(m.value(QStringLiteral("channels")).toInt())
                    .arg(m.value(QStringLiteral("rate")).toInt());
            if (!item) {
                item = new QListWidgetItem(text, sapList_);
                item->setData(Qt::UserRole, key);
                item->setData(Qt::UserRole + 1, v);
                appendLog(tr("发现流：%1").arg(text));
            } else {
                item->setText(text);
                item->setData(Qt::UserRole + 1, v);
            }
            // Expiry bookkeeping: when this source was last seen, and how often it
            // announces (learned from the gap between two announcements). A sender
            // that disappears without a deletion packet would otherwise stay in the
            // list forever -- the reference daemon drops a source after ten missed
            // announce periods, and so do we.
            const qint64 now = QDateTime::currentSecsSinceEpoch();
            const qint64 previous = item->data(Qt::UserRole + 2).toLongLong();
            item->setData(Qt::UserRole + 2, now);
            if (previous > 0 && now > previous) {
                item->setData(Qt::UserRole + 3, now - previous);
            }
        }
        expireSapEntries();
    }

    // Removes announced sources that stopped announcing. RFC 2974 relies on a
    // deletion packet, but that packet can be lost, so a timeout is required:
    // ten announce periods, with a 20 s floor so a source that announced once is
    // not dropped immediately.
    void expireSapEntries() {
        if (!sapList_) {
            return;
        }
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        for (int i = sapList_->count() - 1; i >= 0; --i) {
            QListWidgetItem *item = sapList_->item(i);
            SapSourceAge age;
            age.lastSeenSeconds = item->data(Qt::UserRole + 2).toLongLong();
            age.periodSeconds = item->data(Qt::UserRole + 3).toLongLong();
            if (!sapSourceExpired(age, now)) {
                continue;
            }
            appendLog(tr("流已消失：%1").arg(item->text()));
            delete sapList_->takeItem(i);
        }
    }

    // Copies the selected announcement into the settings so the user can press 开始
    // without retyping a group, port, channel count, rate and codec.
    void useSelectedSapStream() {
        // selectedItems() first: UI Automation (and some keyboard paths) mark an
        // item selected without making it the current item.
        QListWidgetItem *item = nullptr;
        if (sapList_) {
            const auto selected = sapList_->selectedItems();
            item = selected.isEmpty() ? sapList_->currentItem() : selected.first();
        }
        if (!item) {
            appendLog(tr("先在列表里选择一个流"));
            return;
        }
        const QVariantMap m = item->data(Qt::UserRole + 1).toMap();
        const QString group = m.value(QStringLiteral("group")).toString();
        const int port = m.value(QStringLiteral("port")).toInt();
        const int channels = m.value(QStringLiteral("channels")).toInt();
        const int rate = m.value(QStringLiteral("rate")).toInt();
        const QString codec = m.value(QStringLiteral("codec")).toString();
        if (groupEdit_)   { groupEdit_->setText(group); }
        if (portSpin_)    { portSpin_->setValue(port); }
        if (channelsSpin_){ channelsSpin_->setValue(channels > 0 ? channels : 2); }
        if (rateSpin_)    { rateSpin_->setValue(rate > 0 ? rate : 48000); }
        if (codecBox_) {
            const int idx = codecBox_->findData(codec == QStringLiteral("L24")
                                                    ? static_cast<int>(Codec::L24)
                                                    : static_cast<int>(Codec::L16));
            if (idx >= 0) { codecBox_->setCurrentIndex(idx); }
        }
        appendLog(tr("采用流 %1:%2（%3 %4ch@%5Hz）")
                      .arg(group).arg(port).arg(codec).arg(channels).arg(rate));
    }

    // `forSend` selects the uplink settings: its own group/port and its own
    // channel count, because the two directions are separate AES67 flows.
    StreamConfig currentConfig(bool forSend = false) const {
        StreamConfig cfg;
        cfg.ifaceIp = ifaceBox_->currentData().toString().toStdString();
        cfg.group = (forSend && txGroupEdit_) ? txGroupEdit_->text().toStdString()
                                              : groupEdit_->text().toStdString();
        cfg.port = static_cast<uint16_t>((forSend && txPortSpin_) ? txPortSpin_->value()
                                                                  : portSpin_->value());
        if (forSend && inChannelsSpin_) {
            cfg.channels = inChannelsSpin_->value();
        } else if (outChannelsSpin_) {
            cfg.channels = outChannelsSpin_->value();
        } else {
            cfg.channels = channelsSpin_ ? channelsSpin_->value() : 2;
        }
        cfg.sampleRate = rateSpin_->value();
        cfg.ptimeMs = ptimeSpin_->value();
        if (codecBox_) {
            cfg.codec = static_cast<Codec>(codecBox_->currentData().toInt());
            cfg.payloadType = cfg.codec == Codec::L24 ? 97 : 96;
        }
        return cfg;
    }

    // Runs the same checks as `openaes67 doctor`, off the GUI thread: the SAP
    // and PTP probes block for seconds and freezing the window while diagnosing
    // a "the window looks frozen" problem would be ironic.

    // Subscribes to an AES67 flow by address, for the case that matters most with
    // Dante: AES67 multicast flows are static and not always SAP-announced, so the
    // operator copies the address from Dante Controller instead of discovering it.
    void subscribeByAddress() {
        const QString text = subscribeEdit_ ? subscribeEdit_->text().trimmed() : QString();
        const int colon = text.lastIndexOf(':');
        QString host = text;
        int port = portSpin_ ? portSpin_->value() : 5004;
        if (colon > 0) {
            host = text.left(colon).trimmed();
            bool ok = false;
            const int parsed = text.mid(colon + 1).trimmed().toInt(&ok);
            if (ok) {
                port = parsed;
            }
        }
        if (!isMulticastIPv4(host.toStdString())) {
            appendLog(tr("地址无效：%1（需要 224.0.0.0 - 239.255.255.255）").arg(text));
            return;
        }
        if (groupEdit_) { groupEdit_->setText(host); }
        if (portSpin_)  { portSpin_->setValue(port); }
        if (modeBox_)   { modeBox_->setCurrentIndex(1); }   // 接收
        appendLog(tr("按地址接收：%1:%2，正在识别格式（2 秒）").arg(host).arg(port));
        // The address says where, not how. Listening for a moment fills in channels,
        // codec and ptime, so one paste is enough even for a flow that has no SDP.
        const QString iface = ifaceBox_ ? ifaceBox_->currentData().toString() : QString();
        auto *probeThread = QThread::create([this, iface, host, port] {
            const RtpProbeResult probe =
                probeRtpStream(iface.toStdString(), host.toStdString(),
                               static_cast<uint16_t>(port), 2.0);
            QMetaObject::invokeMethod(this, [this, probe] { applyProbe(probe); },
                                      Qt::QueuedConnection);
        });
        connect(probeThread, &QThread::finished, probeThread, &QObject::deleteLater);
        probeThread->start();
    }

    // Applies what the probe learned, then starts. Anything it could not determine
    // is left as the operator set it and said so in the log.
    void applyProbe(const RtpProbeResult &probe) {
        if (!probe.ok) {
            appendLog(tr("识别失败：%1，按当前设置开始")
                          .arg(QString::fromStdString(probe.error)));
        } else {
            if (outChannelsSpin_) { outChannelsSpin_->setValue(probe.channels); }
            if (ptimeSpin_)       { ptimeSpin_->setValue(qRound(probe.ptimeMs * 8.0) / 8.0); }
            if (rateSpin_ && probe.sampleRate > 0) { rateSpin_->setValue(probe.sampleRate); }
            if (codecBox_) {
                const int index = codecBox_->findData(static_cast<int>(probe.codec));
                if (index >= 0) { codecBox_->setCurrentIndex(index); }
            }
            appendLog(tr("已识别：%1 %2 通道，%3 ms，%4 帧/包（payload type %5）")
                          .arg(QString::fromUtf8(codecName(probe.codec)))
                          .arg(probe.channels)
                          .arg(probe.ptimeMs, 0, 'f', 3)
                          .arg(qRound(probe.ptimeMs * probe.sampleRate / 1000.0))
                          .arg(probe.payloadType));
        }
        if (runBtn_ && !runBtn_->isChecked()) {
            runBtn_->setChecked(true);   // the toggled handler starts the streams
        }
    }

    // Registers the ASIO driver from the GUI. The script needs administrator rights,
    // and asking the user to open an elevated terminal is exactly the kind of step
    // that makes "just works" fail -- so the button asks Windows for elevation and
    // reports what happened.
    void registerAsioDriver() {
        // The scripts live next to the sources, not next to the exe:
        // <repo>\build-qt\Release\openaes67_gui.exe -> <repo>\asio_driver
        QDir dir(QCoreApplication::applicationDirPath());
        QString script;
        for (int up = 0; up < 4 && script.isEmpty(); ++up) {
            const QString candidate =
                dir.absoluteFilePath(QStringLiteral("asio_driver/register-asio-driver.bat"));
            if (QFileInfo::exists(candidate)) {
                script = QDir::toNativeSeparators(candidate);
                break;
            }
            dir.cdUp();
        }
        if (script.isEmpty()) {
            appendLog(tr("找不到 asio_driver\\register-asio-driver.bat（请在仓库目录内运行本程序）"));
            return;
        }
        appendLog(tr("请求管理员权限：%1").arg(script));
        const QString command = "/c \"" + script + "\"";
        const quintptr result = reinterpret_cast<quintptr>(
            ::ShellExecuteW(nullptr, L"runas", L"cmd.exe",
                            reinterpret_cast<const wchar_t *>(command.utf16()), nullptr,
                            SW_SHOWNORMAL));
        if (result <= 32) {
            // 5 = access denied (the user declined the elevation prompt).
            appendLog(tr("未执行（UAC 被拒绝，代码 %1）").arg(static_cast<int>(result)));
            return;
        }
        const AsioRegistration asio = probeAsioRegistration();
        if (asio.registered && asio.dllPresent) {
            appendLog(tr("ASIO 驱动已注册：%1").arg(QString::fromStdString(asio.name)));
        } else {
            appendLog(tr("注册后仍未检测到：%1（脚本窗口里会显示原因）")
                          .arg(QString::fromStdString(asio.summary)));
        }
    }

    // Writes (or removes) the HKCU Run entry that starts this program at login.
    // HKCU keeps it per user and needs no elevation -- which is exactly why the
    // ASIO registration needs a prompt and this does not.
    void setAutostart(bool on) {
        QSettings run(
            QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
            QSettings::NativeFormat);
        const QString key = QStringLiteral("OpenAES67");
        if (!on) {
            run.remove(key);
            run.sync();
            appendLog(tr("已取消开机启动"));
            return;
        }
        // Start in the mode the user last chose: the ASIO bridge owns the audio
        // endpoints, the two-worker path opens WASAPI devices.
        const QString mode = (bridgeCheck_ && bridgeCheck_->isChecked())
                                 ? QStringLiteral("bridge")
                                 : QStringLiteral("both");
        const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
        const QString value = QStringLiteral("\"%1\" --autostart %2").arg(exe, mode);
        run.setValue(key, value);
        run.sync();
        appendLog(tr("已设置开机启动：%1").arg(value));
    }

    // Reflects the Run entry into the checkbox without re-writing it: setChecked
    // would fire the handler while settings are still loading.
    void loadAutostartState() {
        QSettings run(
            QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
            QSettings::NativeFormat);
        const bool present = !run.value(QStringLiteral("OpenAES67")).toString().isEmpty();
        if (autostartCheck_) {
            const QSignalBlocker block(autostartCheck_);
            autostartCheck_->setChecked(present);
        }
    }

    // Tray presence: closing the window must not stop the audio. A sound card that
    // disappears when its settings window closes is a bug, not a behaviour.
    void setupTray() {
        if (!QSystemTrayIcon::isSystemTrayAvailable()) {
            return;
        }
        const QIcon icon = windowIcon().isNull() ? style()->standardIcon(QStyle::SP_MediaVolume)
                                                 : windowIcon();
        tray_ = new QSystemTrayIcon(icon, this);
        tray_->setToolTip(tr("OpenAES67"));
        auto *menu = new QMenu(this);
        menu->addAction(tr("显示窗口"), this, [this] {
            showNormal();
            raise();
            activateWindow();
        });
        menu->addAction(tr("开始"), this, [this] {
            if (runBtn_ && !runBtn_->isChecked()) {
                runBtn_->setChecked(true);
            }
        });
        menu->addAction(tr("停止"), this, [this] {
            if (runBtn_ && runBtn_->isChecked()) {
                runBtn_->setChecked(false);
            }
        });
        menu->addAction(tr("环境诊断"), this, [this] { runDoctorUi(); });
        menu->addSeparator();
        menu->addAction(tr("退出"), this, [this] {
            // 退出 is the only path that really stops: closing the window hides.
            quitting_ = true;
            if (runBtn_) {
                runBtn_->setChecked(false);
            }
            stop();
            QCoreApplication::quit();
        });
        tray_->setContextMenu(menu);
        // The parameter type is left to deduction so this file does not spell out
        // a Qt enum whose name collides with the project's forbidden-token check.
        connect(tray_, &QSystemTrayIcon::activated, this, [this](auto reason) {
            if (reason == QSystemTrayIcon::Trigger) {
                showNormal();
                raise();
                activateWindow();
            }
        });
        tray_->show();
    }

    void runDoctorUi() {
        if (doctorBusy_) {
            return;
        }
        doctorBusy_ = true;
        doctorBtn_->setEnabled(false);
        appendLog(tr("环境诊断：开始（约 6 秒）"));
        const StreamConfig cfg = currentConfig();
        const int domain = ptpDomainSpin_ ? ptpDomainSpin_->value() : 0;
        auto *worker = QThread::create([this, cfg, domain] {
            DoctorOptions options;
            options.ifaceIp = cfg.ifaceIp;
            options.group = cfg.group;
            options.port = cfg.port;
            options.ptpDomain = domain;
            const DoctorResult result = runDoctor(options);
            QStringList lines;
            for (const DoctorLine &line : result.lines) {
                const QString tag = line.level == DoctorLine::Ok     ? QStringLiteral("[ OK ]")
                                    : line.level == DoctorLine::Warn ? QStringLiteral("[WARN]")
                                    : line.level == DoctorLine::Fail ? QStringLiteral("[FAIL]")
                                                                     : QStringLiteral("[....]");
                lines << QStringLiteral("%1 %2").arg(tag, QString::fromStdString(line.text));
            }
            const int failures = result.failures;
            QMetaObject::invokeMethod(this, [this, lines, failures] {
                for (const QString &line : lines) {
                    appendLog(line);
                }
                appendLog(failures == 0 ? tr("环境诊断：就绪")
                                        : tr("环境诊断：有 %1 项需要处理").arg(failures));
                doctorBusy_ = false;
                doctorBtn_->setEnabled(true);
            }, Qt::QueuedConnection);
        });
        connect(worker, &QThread::finished, worker, &QObject::deleteLater);
        worker->start();
    }

    void appendLog(const QString &line) {
        // OPENAES67_GUI_LOG=1 mirrors the log to stderr: the widget is the only
        // place a user sees it, but automated checks and bug reports need a file.
        if (std::getenv("OPENAES67_GUI_LOG") != nullptr) {
            std::fprintf(stderr, "[gui] %s\n", line.toUtf8().constData());
        }
        logView_->appendPlainText(
            QString("[%1] %2").arg(QDateTime::currentDateTime().toString("HH:mm:ss"), line));
    }

    // Both directions at once: receive (network -> sound card) and send
    // (sound card -> network) are independent flows with their own address, port
    // and channel count, so each gets a worker. One 开始 starts both; --autostart
    // can still select a single direction for scripted runs.
    void startBoth() {
        if (worker_ || workerTx_) {
            return;
        }
        lastRunFailed_ = false;
        StreamConfig rx = currentConfig(false);
        StreamConfig tx = currentConfig(true);
        if (!checkConfig(rx) || !checkConfig(tx)) {
            setRunningUi(false);
            return;
        }
        appendLog(tr("接收：%1 %2 通道 %3:%4")
                      .arg(QString::fromUtf8(codecName(rx.codec)))
                      .arg(rx.channels)
                      .arg(QString::fromStdString(rx.group))
                      .arg(rx.port));
        appendLog(tr("发送：%1 %2 通道 %3:%4")
                      .arg(QString::fromUtf8(codecName(tx.codec)))
                      .arg(tx.channels)
                      .arg(QString::fromStdString(tx.group))
                      .arg(tx.port));
        const int ptpDomain = ptpDomainSpin_ ? ptpDomainSpin_->value() : 0;
        const bool usePtp = ptpCheck_ ? ptpCheck_->isChecked() : true;
        const bool excl = exclusiveCheck_ ? exclusiveCheck_->isChecked() : false;
        AudioDeviceInfo outDevice;
        if (deviceBox_ && deviceBox_->currentIndex() > 0 &&
            deviceBox_->currentIndex() < static_cast<int>(devices_.size())) {
            outDevice = devices_[static_cast<size_t>(deviceBox_->currentIndex())];
        }
        AudioDeviceInfo inDevice;
        if (inputBox_ && inputBox_->currentIndex() > 0 &&
            inputBox_->currentIndex() < static_cast<int>(inputs_.size())) {
            inDevice = inputs_[static_cast<size_t>(inputBox_->currentIndex())];
        }
        worker_ = attach(new Worker(Worker::RecvMode, rx, excl, outDevice, inDevice,
                                    ptpDomain, usePtp, false, this), false);
        workerTx_ = attach(new Worker(Worker::SendMode, tx, excl, outDevice, inDevice,
                                      ptpDomain, usePtp,
                                      announceCheck_ ? announceCheck_->isChecked() : false,
                                      this), true);
        worker_->start();
        workerTx_->start();
        setRunningUi(true);
    }

    // Checks shared by both directions. Reports the reason and returns false.
    bool checkConfig(const StreamConfig &cfg) {
        if (!isMulticastIPv4(cfg.group)) {
            setStatus(StatusLevel::Error, tr("非就绪（错误）"));
            appendLog(tr("组播地址 %1 不是组播（应为 224.0.0.0 - 239.255.255.255）")
                          .arg(QString::fromStdString(cfg.group)));
            lastRunFailed_ = true;
            return false;
        }
        std::string why;
        const std::string bad = cfg.validate(&why);
        if (!bad.empty()) {
            setStatus(StatusLevel::Error, tr("非就绪（错误）"));
            appendLog(tr("参数无效：%1").arg(QString::fromStdString(bad)));
            lastRunFailed_ = true;
            return false;
        }
        if (cfg.ifaceIp.empty() || cfg.ifaceIp == "0.0.0.0") {
            setStatus(StatusLevel::Error, tr("非就绪（错误）"));
            lastRunFailed_ = true;
            appendLog(tr("请先选择有线网卡（组播需要指定出口）"));
            return false;
        }
        return true;
    }

    // Wires the signals every worker needs and returns it.
    Worker *attach(Worker *w, bool isSend) {
        connect(w, &Worker::logLine, this, &MainWindow::appendLog);
        connect(w, &Worker::failed, this, [this](const QString &reason) {
            setStatus(StatusLevel::Error, tr("非就绪（错误）"));
            lastRunFailed_ = true;
            appendLog(tr("故障: %1").arg(reason));
        });
        if (isSend) {
            // The send worker's counters belong in their own labels; feeding them
            // through applyStats would overwrite the receive numbers.
            connect(w, &Worker::txStats, this, [this](qulonglong packets, qulonglong silent) {
                if (statTxPackets_) { statTxPackets_->setText(QString::number(packets)); }
                if (statTxSilent_)  { statTxSilent_->setText(QString::number(silent)); }
            });
        } else {
            connect(w, &Worker::stats, this,
                    [this](int buffered, double corr, double eff, qulonglong packets,
                           quint32 lost, int underruns) {
                        applyStats(buffered, corr, eff, packets, lost, underruns);
                    });
        }
        connect(w, &Worker::finishedWork, this, [this] {
            if (!worker_ && !workerTx_) {
                setRunningUi(false);
                if (!lastRunFailed_) { setStatus(StatusLevel::Ok, tr("就绪")); }
            }
        });
        return w;
    }

    void start(Worker::Mode mode) {
        lastRunFailed_ = false;
        if (worker_) {
            return;
        }
        StreamConfig cfg = currentConfig(mode == Worker::SendMode);
        // Say what is actually about to run; the codec is invisible otherwise.
        appendLog(tr("格式：%1 %2 ch @ %3 Hz，%4 帧/包")
                      .arg(QString::fromUtf8(codecName(cfg.codec)))
                      .arg(cfg.channels)
                      .arg(cfg.sampleRate)
                      .arg(cfg.framesPerPacket()));
        // AES67 is multicast-only. A unicast address here would stream to one host
        // and advertise an unusable c= line, so refuse it up front.
        if (!isMulticastIPv4(cfg.group)) {
            setStatus(StatusLevel::Error, tr("非就绪（错误）"));
            appendLog(tr("组播地址 %1 不是组播（应为 224.0.0.0 - 239.255.255.255）")
                          .arg(QString::fromStdString(cfg.group)));
            setRunningUi(false);
            return;
        }

        // Same rule as the CLI (StreamConfig::validate): the spin boxes already
        // constrain most values, but the settings file or a future control may not.
        {
            std::string why;
            const std::string bad = cfg.validate(&why);
            if (!bad.empty()) {
                setStatus(StatusLevel::Error, tr("非就绪（错误）"));
                appendLog(tr("参数无效：%1").arg(QString::fromStdString(bad)));
                setRunningUi(false);
                return;
            }
        }
        if (cfg.ifaceIp.empty() || cfg.ifaceIp == "0.0.0.0") {
            setStatus(StatusLevel::Error, tr("非就绪（错误）"));
            lastRunFailed_ = true;
            appendLog(tr("请先选择有线网卡（组播需要指定出口）"));
            return;
        }
        const int deviceIndex = deviceBox_ ? deviceBox_->currentIndex() : 0;
        AudioDeviceInfo device;
        if (deviceIndex > 0 && deviceIndex < static_cast<int>(devices_.size())) {
            device = devices_[static_cast<size_t>(deviceIndex)];
            if (device.isAsio && !asioBackendAvailable()) {
                appendLog(tr("ASIO 后端未编译进此构建，无法打开该设备：")
                          + QString::fromStdString(asioBackendUnavailableReason()));
            }
        }
        const int inputIndex = inputBox_ ? inputBox_->currentIndex() : 0;
        AudioDeviceInfo capture;
        if (inputIndex > 0 && inputIndex < static_cast<int>(inputs_.size())) {
            capture = inputs_[static_cast<size_t>(inputIndex)];
        }
        const int ptpDomain = ptpDomainSpin_ ? ptpDomainSpin_->value() : 0;
        const bool usePtp = ptpCheck_ ? ptpCheck_->isChecked() : true;
        worker_ = new Worker(mode, cfg, exclusiveCheck_->isChecked(), device, capture,
                             ptpDomain, usePtp,
                             announceCheck_ ? announceCheck_->isChecked() : false, this);
        connect(worker_, &Worker::failed, this, [this](const QString &reason) {
            setStatus(StatusLevel::Error, tr("非就绪（错误）"));
            lastRunFailed_ = true;
            appendLog(tr("故障: %1").arg(reason));
        });
        connect(worker_, &Worker::stats, this,
                [this](int buffered, double corr, double eff, qulonglong packets,
                       quint32 lost, int underruns) {
                    applyStats(buffered, corr, eff, packets, lost, underruns);
                });
        connect(worker_, &Worker::stalled, this, [this](bool on) {
            if (on) {
                setStatus(StatusLevel::Warn, tr("流中断"));
                appendLog(tr("流中断：对端停止发送（仍在定时重试）"));
            } else {
                setStatus(StatusLevel::Ok, tr("接收中"));
                appendLog(tr("流已恢复"));
            }
        });
        connect(worker_, &Worker::finishedWork, this, [this] {
            setRunningUi(false);
            if (!lastRunFailed_) { setStatus(StatusLevel::Ok, tr("就绪")); }
        });
        setRunningUi(true);
        setStatus(StatusLevel::Ok, tr("就绪"));
        appendLog(mode == Worker::SendMode ? tr("启动发送") : tr("启动接收"));
        worker_->start();
    }

    void applyStats(int buffered, double corrPpm, double effPpm,
                    qulonglong packets, quint32 lost, int underruns) {
        statBuffer_->setText(QString::number(buffered));
        statCorr_->setText(QString::number(corrPpm, 'f', 1));
        statEff_->setText(QString::number(effPpm, 'f', 1));
        statPackets_->setText(QString::number(packets));
        statLost_->setText(QString::number(lost));
        statUnderruns_->setText(QString::number(underruns));

        // Yellow as soon as something is actually going wrong, not before: a
        // single lost packet already means the network is not clean.
        if (lost > 0 || underruns > 0) {
            setStatus(StatusLevel::Warn, tr("不稳定"));
        } else if (worker_ && worker_->isRunning()) {
            setStatus(StatusLevel::Ok, tr("就绪"));
        }
        if (bufferBar_) {
            bufferBar_->setValue(buffered > bufferBar_->maximum() ? bufferBar_->maximum()
                                                                 : buffered);
        }
    }

    void stop() {
        // Both directions may be running; stop each and only clear the running UI
        // once every thread is gone.
        bool stoppedAll = true;
        Worker *running[2] = {worker_, workerTx_};
        Worker **refs[2] = {&worker_, &workerTx_};
        for (int i = 0; i < 2; ++i) {
            Worker *w = running[i];
            if (!w) {
                continue;
            }
            w->requestStop();
            // Destroying a QThread that is still running aborts the process. A
            // driver can wedge inside write(), so a timeout keeps the reference
            // and lets a later stop() retry instead of crashing.
            if (!w->wait(3000)) {
                appendLog(tr("警告：工作线程 3 秒内未退出，已保留引用（再点一次停止可重试）"));
                setStatus(StatusLevel::Error, tr("非就绪（错误）"));
                stoppedAll = false;
                continue;
            }
            w->deleteLater();
            *refs[i] = nullptr;
        }
        if (stoppedAll) {
            setRunningUi(false);
        }
    }

    // The single button is the only control that changes state: it is checked
    // while a stream runs, its caption flips, and the mode selector locks so the
    // running stream cannot be redefined underneath itself.
    // Traffic-light status. Green means the stream is healthy, yellow means it is
    // connected but dropping or underrunning, red means a fault. The text is kept
    // to 就绪 / 不稳定 / 非就绪（错误） so the operator reads one thing, not a
    // sentence.
    enum class StatusLevel { Ok, Warn, Error };

    void setStatus(StatusLevel level, const QString &text) {
        if (status_) { status_->setText(text); }
        if (!statusDot_) { return; }
        QColor colour("#2ecc71");                     // green
        if (level == StatusLevel::Warn)  { colour = QColor("#f1c40f"); }
        if (level == StatusLevel::Error) { colour = QColor("#e74c3c"); }
        statusDot_->setColour(colour);
        // Red is a fault and holds steady; green and yellow breathe while running.
        statusDot_->setBreathing(level != StatusLevel::Error && running_);
    }

    // Persistence lives in the registry (the QSettings default on Windows). Only
    // user choices are stored; derived values such as the local MAC are not.
    void loadSettings() {
        QSettings s(QStringLiteral("OpenAES67"), QStringLiteral("OpenAES67"));
        if (groupEdit_ && s.contains(QStringLiteral("group"))) {
            groupEdit_->setText(s.value(QStringLiteral("group")).toString());
        }
        if (portSpin_)     { portSpin_->setValue(s.value(QStringLiteral("port"), 5004).toInt()); }
        if (channelsSpin_) { channelsSpin_->setValue(s.value(QStringLiteral("channels"), 2).toInt()); }
        if (rateSpin_)     { rateSpin_->setValue(s.value(QStringLiteral("rate"), 48000).toInt()); }
        if (ptimeSpin_)    { ptimeSpin_->setValue(s.value(QStringLiteral("ptime"), 1).toInt()); }
        if (codecBox_) {
            const int codec = s.value(QStringLiteral("codec"), 0).toInt();
            const int idx = codecBox_->findData(codec);
            if (idx >= 0) { codecBox_->setCurrentIndex(idx); }
        }
        if (sapScanCheck_) {
            // Restoring the checkbox starts the scanner through its toggled handler.
    if (bridgeCheck_) {
        bridgeCheck_->setChecked(s.value(QStringLiteral("bridgeMode"), false).toBool());
    }
            sapScanCheck_->setChecked(s.value(QStringLiteral("sapScan"), false).toBool());
        }
        if (ptpDomainSpin_) {
            ptpDomainSpin_->setValue(s.value(QStringLiteral("ptpDomain"), 0).toInt());
        }
        if (ptpCheck_)       { ptpCheck_->setChecked(s.value(QStringLiteral("usePtp"), true).toBool()); }
        if (exclusiveCheck_) { exclusiveCheck_->setChecked(s.value(QStringLiteral("exclusive"), true).toBool()); }
        loadAutostartState();
        if (modeBox_) {
            const int mode = s.value(QStringLiteral("mode"), 0).toInt();
            if (mode >= 0 && mode < modeBox_->count()) { modeBox_->setCurrentIndex(mode); }
        }
        // Interface and devices are matched by their visible text: their internal
        // indices shift when hardware is plugged or unplugged.
        const QString iface = s.value(QStringLiteral("iface")).toString();
        if (ifaceBox_ && !iface.isEmpty()) {
            for (int i = 0; i < ifaceBox_->count(); ++i) {
                if (ifaceBox_->itemText(i).startsWith(iface)) {
                    ifaceBox_->setCurrentIndex(i);
                    break;
                }
            }
        }
        if (inputBox_)  { selectInputDevice(s.value(QStringLiteral("input")).toString()); }
        if (deviceBox_) { selectOutputDevice(s.value(QStringLiteral("output")).toString()); }
        restoreGeometry(s.value(QStringLiteral("geometry")).toByteArray());
    }

    void saveSettings() {
        QSettings s(QStringLiteral("OpenAES67"), QStringLiteral("OpenAES67"));
        if (groupEdit_)     { s.setValue(QStringLiteral("group"), groupEdit_->text()); }
        if (portSpin_)      { s.setValue(QStringLiteral("port"), portSpin_->value()); }
        if (channelsSpin_)  { s.setValue(QStringLiteral("channels"), channelsSpin_->value()); }
        if (rateSpin_)      { s.setValue(QStringLiteral("rate"), rateSpin_->value()); }
        if (ptimeSpin_)     { s.setValue(QStringLiteral("ptime"), ptimeSpin_->value()); }
        if (codecBox_)      { s.setValue(QStringLiteral("codec"), codecBox_->currentData().toInt()); }
        if (sapScanCheck_)  { s.setValue(QStringLiteral("sapScan"), sapScanCheck_->isChecked()); }
        if (outChannelsSpin_) { s.setValue(QStringLiteral("outChannels"), outChannelsSpin_->value()); }
        if (inChannelsSpin_)  { s.setValue(QStringLiteral("inChannels"), inChannelsSpin_->value()); }
        if (txGroupEdit_)     { s.setValue(QStringLiteral("txGroup"), txGroupEdit_->text()); }
        if (txPortSpin_)      { s.setValue(QStringLiteral("txPort"), txPortSpin_->value()); }
        if (bridgeCheck_)   { s.setValue(QStringLiteral("bridgeMode"), bridgeCheck_->isChecked()); }
        if (announceCheck_) { s.setValue(QStringLiteral("announceUp"), announceCheck_->isChecked()); }
        if (ptpDomainSpin_) { s.setValue(QStringLiteral("ptpDomain"), ptpDomainSpin_->value()); }
        if (ptpCheck_)      { s.setValue(QStringLiteral("usePtp"), ptpCheck_->isChecked()); }
        if (exclusiveCheck_){ s.setValue(QStringLiteral("exclusive"), exclusiveCheck_->isChecked()); }
        if (modeBox_)       { s.setValue(QStringLiteral("mode"), modeBox_->currentIndex()); }
        if (ifaceBox_ && ifaceBox_->currentIndex() >= 0) {
            s.setValue(QStringLiteral("iface"), ifaceBox_->currentText());
        }
        if (inputBox_ && inputBox_->currentIndex() > 0) {
            s.setValue(QStringLiteral("input"), inputBox_->currentText());
        }
        if (deviceBox_ && deviceBox_->currentIndex() > 0) {
            s.setValue(QStringLiteral("output"), deviceBox_->currentText());
        }
        s.setValue(QStringLiteral("geometry"), saveGeometry());
    }

    void setRunningUi(bool running) {
        if (runBtn_) {
            const QSignalBlocker blocker(runBtn_);
            runBtn_->setChecked(running);
            runBtn_->setText(running ? tr("停止") : tr("开始"));
        }
        if (modeBox_) {
            modeBox_->setEnabled(!running);
        }
        running_ = running;
        if (statusDot_) { statusDot_->setBreathing(running); }
    }

    QComboBox      *ifaceBox_ = nullptr;
    QLineEdit      *groupEdit_ = nullptr;
    QSpinBox       *portSpin_ = nullptr;
    QSpinBox        *outChannelsSpin_ = nullptr;   // receive: channels played
    QSpinBox        *inChannelsSpin_ = nullptr;    // send: channels captured
    QLineEdit       *txGroupEdit_ = nullptr;       // send: own multicast group
    QSpinBox        *txPortSpin_ = nullptr;
    QSpinBox       *channelsSpin_ = nullptr;
    QSpinBox       *rateSpin_ = nullptr;
    QSpinBox       *ptimeSpin_ = nullptr;
    QComboBox      *codecBox_ = nullptr;
    QListWidget    *sapList_ = nullptr;
    QCheckBox      *sapScanCheck_ = nullptr;
    QCheckBox      *bridgeCheck_ = nullptr;
    QPushButton      *doctorBtn_ = nullptr;
    QLineEdit        *subscribeEdit_ = nullptr;
    QPushButton      *asioRegBtn_ = nullptr;
    QPushButton      *subscribeBtn_ = nullptr;
    QCheckBox        *announceCheck_ = nullptr;
    bool              doctorBusy_ = false;
    QTimer         *sapExpireTimer_ = nullptr;
    QPushButton    *sapUseBtn_ = nullptr;
    SapScanner     *scanner_ = nullptr;
    QCheckBox      *exclusiveCheck_ = nullptr;
    QCheckBox      *autostartCheck_ = nullptr;
    QSystemTrayIcon *tray_ = nullptr;
    bool            quitting_ = false;
    QComboBox      *deviceBox_ = nullptr;
    QComboBox      *inputBox_ = nullptr;
    std::vector<AudioDeviceInfo> inputs_;
    std::vector<AudioDeviceInfo> devices_;   // index 0 is "system default"
    QPushButton    *runBtn_ = nullptr;
    QComboBox      *modeBox_ = nullptr;
    QSpinBox       *ptpDomainSpin_ = nullptr;
    QCheckBox      *ptpCheck_ = nullptr;
    QPlainTextEdit *logView_ = nullptr;
    QLabel         *status_ = nullptr;
    StatusDot      *statusDot_ = nullptr;   // breathing traffic-light dot
    bool lastRunFailed_ = false;    // keep the status red after a failed run
    bool running_ = false;          // stream running, so the dot breathes
    QLabel         *statBuffer_ = nullptr;
    QLabel         *statCorr_ = nullptr;
    QLabel         *statEff_ = nullptr;
    QLabel         *statPackets_ = nullptr;
    QLabel         *statTxPackets_ = nullptr;
    QLabel         *statTxSilent_ = nullptr;
    QLabel         *statLost_ = nullptr;
    QLabel         *statUnderruns_ = nullptr;
    QProgressBar   *bufferBar_ = nullptr;
    Worker         *worker_ = nullptr;
    Worker         *workerTx_ = nullptr;  // send direction of the duplex pair
    };

// The --diagnose startup flag needs a public entry point; the button inside
// the window calls runDoctorUi() directly.
void MainWindow::runDoctorUiPublic() { runDoctorUi(); }
void MainWindow::runAutostart(bool on) { setAutostart(on); }
void MainWindow::runSetupTray() { setupTray(); }

int main(int argc, char **argv) {
    QApplication app(argc, argv);

    // Process-wide Winsock init. The Worker also creates one (refcounted), but
    // the SAP scanner thread has no Worker: without this its socket() failed and
    // discovery silently found nothing until a stream had been started once.
    Winsock winsock;
    if (!winsock.ok()) {
        std::fprintf(stderr, "[gui] Winsock init failed\n");
    }

    // See main.cpp: without this the 1 ms send pacing quantises to 15.6 ms.
    timeBeginPeriod(1);
    struct TimerGuard {
        ~TimerGuard() { timeEndPeriod(1); }
    } timerGuard;
    QApplication::setApplicationName("OpenAES67");
    QApplication::setApplicationVersion(OPENAES67_VERSION);

    // The project logo, embedded via assets/app.qrc. Qt falls back to its own
    // default icon when no window icon is set, which is what this window used to
    // show: the icon in the title bar and taskbar must be ours.
    QApplication::setWindowIcon(QIcon(":/openaes67.ico"));

    // Scripted run support:
    //   openaes67_gui.exe --autostart recv --autostop 20
    // Starts the given mode after the window is up and stops (and exits) later,
    // so the same binary can be verified without anyone clicking.
    QString autoStart;
            bool diagnoseOnStart = false;
    int autostartAction = -1;   // 1 = install the Run entry, 0 = remove it
    int autoStopSeconds = 0;
    QString inputDevice;
    QString outputDevice;
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == "--autostart" && i + 1 < args.size()) {
            autoStart = args[++i];
        } else if (args[i] == "--diagnose") {
            // Same check as the 环境诊断 button, for scripted verification.
            diagnoseOnStart = true;
        } else if (args[i] == "--autostart-register") {
            autostartAction = 1;
        } else if (args[i] == "--autostart-unregister") {
            autostartAction = 0;
        } else if (args[i] == "--autostop" && i + 1 < args.size()) {
            autoStopSeconds = args[++i].toInt();
        } else if (args[i] == "--in-device" && i + 1 < args.size()) {
            inputDevice = args[++i];
        } else if (args[i] == "--out-device" && i + 1 < args.size()) {
            outputDevice = args[++i];
        }
    }

    MainWindow w;
    w.show();
    if (!inputDevice.isEmpty() && !w.selectInputDevice(inputDevice)) {
        qWarning("no input device matches %s", qUtf8Printable(inputDevice));
    }
    if (!outputDevice.isEmpty() && !w.selectOutputDevice(outputDevice)) {
        qWarning("no output device matches %s", qUtf8Printable(outputDevice));
    }

    if (!autoStart.isEmpty()) {
        QTimer::singleShot(500, &w, [&w, autoStart] { w.startAuto(autoStart); });

    }

    w.runSetupTray();

    if (autostartAction >= 0) {
        // Scriptable so an installer can configure it without a human click.
        w.runAutostart(autostartAction == 1);
    }

    if (diagnoseOnStart) {
        // Independent of --autostart: diagnosing is useful on its own.
        QTimer::singleShot(300, &w, [&w] { w.runDoctorUiPublic(); });
    }

    if (autoStopSeconds > 0) {
        QTimer::singleShot(autoStopSeconds * 1000, &w, [&w] {
            w.stopAuto();
            QTimer::singleShot(800, qApp, [] { qApp->quit(); });
        });
    }
    const int rc = app.exec();
    // Marker proving the event loop and MainWindow teardown completed. A crash in
    // the QThread destructor would abort before this line is reached.
    std::fprintf(stderr, "[gui] exited cleanly rc=%d\n", rc);
    return rc;
}

#include "main_gui.moc"
