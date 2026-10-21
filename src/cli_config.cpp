#include "openaes67/cli_config.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace openaes67 {

namespace {

void trim(std::string &s) {
    const char *ws = " \t\r\n";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) {
        s.clear();
        return;
    }
    const auto e = s.find_last_not_of(ws);
    s = s.substr(b, e - b + 1);
}

bool truthy(const std::string &v) {
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

} // namespace

bool applyConfigFile(const char *path, CliOptions *options,
                     std::FILE *(*openFileUtf8)(const char *, const char *),
                     bool report) {
    if (!path || !options || !openFileUtf8) {
        return false;
    }
    std::FILE *f = openFileUtf8(path, "rb");
    if (!f) {
        std::fprintf(stderr, "cannot read config file: %s\n", path);
        return false;
    }

    char line[512];
    int lineNumber = 0;
    bool ok = true;
    while (std::fgets(line, sizeof(line), f)) {
        ++lineNumber;
        std::string text(line);

        // A file saved by a Windows editor starts with a UTF-8 BOM; without
        // this the first key is "\xEF\xBB\xBFiface" and is rejected as unknown.
        if (lineNumber == 1 && text.size() >= 3 &&
            static_cast<unsigned char>(text[0]) == 0xEF &&
            static_cast<unsigned char>(text[1]) == 0xBB &&
            static_cast<unsigned char>(text[2]) == 0xBF) {
            text.erase(0, 3);
        }
        const auto hash = text.find('#');
        if (hash != std::string::npos) {
            text.erase(hash);
        }
        const auto eq = text.find('=');
        if (eq == std::string::npos) {
            continue;   // blank line or a heading
        }
        std::string key = text.substr(0, eq);
        std::string value = text.substr(eq + 1);
        trim(key);
        trim(value);
        for (auto &ch : key) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        if (key.empty()) {
            continue;
        }

        StreamConfig &cfg = options->cfg;
        if (key == "iface") {
            cfg.ifaceIp = value;
        } else if (key == "group") {
            cfg.group = value;
        } else if (key == "port") {
            cfg.port = static_cast<uint16_t>(std::atoi(value.c_str()));
        } else if (key == "channels") {
            cfg.channels = std::atoi(value.c_str());
        } else if (key == "sample_rate" || key == "rate") {
            cfg.sampleRate = std::atoi(value.c_str());
        } else if (key == "ptime") {
            cfg.ptimeMs = std::atof(value.c_str());
        } else if (key == "ttl") {
            cfg.ttl = std::atoi(value.c_str());
        } else if (key == "codec") {
            Codec parsed = Codec::L16;
            if (parseCodecName(value, &parsed)) {
                cfg.codec = parsed;
            } else {
                std::fprintf(stderr, "%s:%d: unknown codec '%s'\n", path, lineNumber,
                             value.c_str());
                ok = false;
            }
        } else if (key == "seconds") {
            options->seconds = std::atof(value.c_str());
        } else if (key == "ptp_domain") {
            options->ptpDomain = std::atoi(value.c_str());
        } else if (key == "ptp") {
            options->usePtp = truthy(value);
        } else if (key == "ptp_lock") {
            options->ptpLock = truthy(value);
        } else if (key == "sap") {
            options->useSap = truthy(value);
        } else if (key == "sap_interval") {
            options->sapInterval = std::atoi(value.c_str());
        } else if (key == "shm") {
            options->shmName.assign(value.begin(), value.end());
        } else if (key == "send_group") {
            options->sendGroup = value;
        } else if (key == "send_port") {
            options->sendPort = static_cast<uint16_t>(std::atoi(value.c_str()));
        } else if (key == "discover") {
            options->discover = truthy(value);
        } else if (key == "discover_name") {
            options->discoverName = value;
        } else if (key == "discover_seconds") {
            options->discoverSeconds = std::atof(value.c_str());
        } else if (key == "refclk") {
            options->refclk = value;
        } else if (key == "device") {
            options->deviceName = value;
        } else if (key == "in_device") {
            options->inDeviceName = value;
        } else if (key == "backend") {
            options->backend = value;
        } else if (key == "asio_driver") {
            options->asioDriver = value;
        } else if (key == "out") {
            options->outPath = value;
        } else if (key == "out_bits") {
            options->outBits = std::atoi(value.c_str());
        } else if (key == "sdp_out") {
            options->sdpOutPath = value;
        } else if (key == "sdp_in") {
            options->sdpInPath = value;
        } else {
            if (report) {
                std::fprintf(stderr, "%s:%d: unknown key '%s'\n", path, lineNumber,
                             key.c_str());
            }
            ok = false;
        }
    }
    std::fclose(f);
    return ok;
}

} // namespace openaes67
