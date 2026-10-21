#include "openaes67/doctor.h"

#include <chrono>
#include <cstdint>
#include <thread>

#include "openaes67/asio_probe.h"
#include "openaes67/config.h"
#include "openaes67/net.h"
#include "openaes67/ptp.h"
#include "openaes67/sap.h"

namespace openaes67 {

namespace {

void add(DoctorResult *result, DoctorLine::Level level, std::string text) {
    result->lines.push_back(DoctorLine{level, std::move(text)});
    if (level == DoctorLine::Fail) {
        ++result->failures;
    }
}

} // namespace

DoctorResult runDoctor(const DoctorOptions &options) {
    DoctorResult result;

    // 1. Interface: multicast needs a real local address to send and join from.
    if (options.ifaceIp.empty() || options.ifaceIp == "0.0.0.0") {
        add(&result, DoctorLine::Fail,
            "interface: not set (--iface is required for multicast)");
    } else {
        add(&result, DoctorLine::Ok, "interface: " + options.ifaceIp);
    }

    // 2. Group: AES67 is multicast-only; a unicast address silently streams to
    // one host and advertises an unusable c= line.
    if (isMulticastIPv4(options.group)) {
        add(&result, DoctorLine::Ok,
            "group: " + options.group + ":" + std::to_string(options.port) + " is multicast");
    } else {
        add(&result, DoctorLine::Fail,
            "group: " + options.group + " is not multicast (224.0.0.0 - 239.255.255.255)");
    }

    // 3. SAP receive: proves the multicast receive path (join + firewall). No
    // announcement is not fatal -- there may simply be no sender yet.
    if (options.checkSap) {
        add(&result, DoctorLine::Info, "sap: listening for announcements");
        int seen = 0;
        listenSap(options.sapSeconds, options.ifaceIp,
                  [&seen](const SapAnnouncement &) { ++seen; }, nullptr);
        if (seen > 0) {
            add(&result, DoctorLine::Ok,
                "sap: " + std::to_string(seen) + " announcement(s) received");
        } else {
            add(&result, DoctorLine::Warn,
                "sap: none received -- no AES67 sender on this network, or UDP 9875 is blocked");
        }
    }

    // 4. ASIO registration: without it a DAW never lists the device.
    {
        const AsioRegistration asio = probeAsioRegistration();
        if (asio.registered && asio.dllPresent) {
            add(&result, DoctorLine::Ok,
                "asio: " + asio.name + " registered (" + asio.dllPath + ")");
        } else if (asio.registered) {
            add(&result, DoctorLine::Fail,
                "asio: registered but the DLL is missing: " + asio.dllPath);
        } else {
            add(&result, DoctorLine::Fail,
                "asio: not registered -- run asio_driver\\register-asio-driver.bat as administrator");
        }
    }

    // 5. PTP: a Dante device in AES67 mode sends nothing until the clock locks.
    if (options.checkPtp) {
        PtpSlave slave;
        std::string ptpErr;
        if (!slave.start(options.ifaceIp, static_cast<uint8_t>(options.ptpDomain & 0xFF),
                         &ptpErr)) {
            add(&result, DoctorLine::Warn, "ptp: cannot start the slave: " + ptpErr);
        } else {
            bool locked = false;
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(options.ptpSeconds);
            while (std::chrono::steady_clock::now() < deadline) {
                slave.poll(0);
                if (slave.hasMaster()) {
                    locked = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (locked) {
                add(&result, DoctorLine::Ok,
                    "ptp: grandmaster seen on domain " + std::to_string(options.ptpDomain));
            } else {
                add(&result, DoctorLine::Warn,
                    "ptp: no grandmaster within " + std::to_string(options.ptpSeconds) +
                        " s -- Dante will not send audio until the clock locks");
            }
        }
    }

    return result;
}

} // namespace openaes67
