#include "openaes67/sdp.h"
#include <sstream>

namespace openaes67 {

namespace {

std::string hexByte(uint8_t value) {
    static const char digits[] = "0123456789ABCDEF";
    std::string out(2, '0');
    out[0] = digits[(value >> 4) & 0x0F];
    out[1] = digits[value & 0x0F];
    return out;
}

} // namespace

std::string formatGmid(const uint8_t gmid[8]) {
    std::string out;
    for (int i = 0; i < 8; ++i) {
        if (i != 0) {
            out += '-';
        }
        out += hexByte(gmid[i]);
    }
    return out;
}

bool parseGmid(const std::string &text, uint8_t gmid[8]) {
    // Accept both the hyphenated AES67 form and a bare 16-digit string.
    std::string digits;
    for (const char ch : text) {
        if (ch == '-' || ch == ':' || ch == ' ') {
            continue;
        }
        const bool isHex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
                           (ch >= 'A' && ch <= 'F');
        if (!isHex) {
            return false;
        }
        digits += ch;
    }
    if (digits.size() != 16) {
        return false;
    }
    for (int i = 0; i < 8; ++i) {
        const std::string pair = digits.substr(static_cast<size_t>(i) * 2, 2);
        gmid[i] = static_cast<uint8_t>(std::stoul(pair, nullptr, 16));
    }
    return true;
}

std::string buildSdp(const StreamConfig &cfg, uint32_t ssrc, const SdpClockRef &clockRef,
                     uint32_t sessionVersion, SdpDirection direction) {
    // Field order matters: Audinate's "Sample SDP Specification" states that the
    // fields must appear in the order v, o, s, i, t, a, m, c, rtpmap, ptime,
    // ts-refclk, mediaclk, and that "descriptors that are legal SDP but not
    // well-formed for SMPTE will be silently ignored by Dante Controller".
    //
    // The previous version put c= at session level BEFORE m=, which is legal SDP
    // but not the layout Audinate parses, so the flow could never appear.
    std::ostringstream o;
    o << "v=0\r\n";
    o << "o=- " << ssrc << " " << (sessionVersion == 0 ? 1 : sessionVersion)
      << " IN IP4 " << cfg.ifaceIp << "\r\n";
    o << "s=OpenAES67 stream\r\n";
    o << "t=0 0\r\n";
    o << (direction == SdpDirection::RecvOnly ? "a=recvonly\r\n" : "a=sendonly\r\n");
    o << "m=audio " << cfg.port << " RTP/AVP " << static_cast<int>(cfg.payloadType) << "\r\n";
    o << "c=IN IP4 " << cfg.group << "/32\r\n";
    // RFC 4570 SSM source filter: names the unicast sender allowed on this group.
    // Ravenna/Dante receivers use it to reject spoofed or stray senders, and the
    // reference Linux daemon always emits it.
    o << "a=source-filter: incl IN IP4 " << cfg.group << " " << cfg.ifaceIp << "\r\n";
    o << "a=rtpmap:" << static_cast<int>(cfg.payloadType) << " " << codecName(cfg.codec) << "/"
      << cfg.sampleRate << "/" << cfg.channels << "\r\n";
    o << "a=ptime:" << cfg.ptimeMs << "\r\n";

    // AES67 section 8.2: a PTP-referenced sender must name the grandmaster.
    // A Dante device in AES67 mode rejects a stream that only claims a local
    // clock, which is why the old unconditional localmac was a trap.
    if (clockRef.kind == SdpClockRef::Kind::Ptp) {
        o << "a=ts-refclk:ptp=IEEE1588-2008:" << formatGmid(clockRef.gmid) << ":"
          << static_cast<int>(clockRef.domain) << "\r\n";
    } else {
        std::string mac;
        for (int i = 0; i < 6; ++i) {
            if (i != 0) {
                mac += '-';
            }
            mac += hexByte(clockRef.mac[i]);
        }
        o << "a=ts-refclk:localmac=" << mac << "\r\n";
    }

    o << "a=mediaclk:direct=0\r\n";

    // The four attributes below are what the reference Ravenna/AES67 endpoint
    // emits and what receivers use to size buffers and align streams. They are
    // appended after the fields Audinate's parser requires in order, so the
    // v,o,s,t,m,c,rtpmap,ptime,ts-refclk,mediaclk prefix stays untouched.
    o << "a=sync-time:0\r\n";
    o << "a=framecount:" << cfg.framesPerPacket() << "\r\n";
    if (clockRef.kind == SdpClockRef::Kind::Ptp) {
        o << "a=clock-domain:PTPv2 " << static_cast<int>(clockRef.domain) << "\r\n";
    }
    o << "a=mid:1\r\n";

    return o.str();
}

static std::string trim(const std::string &s) {
    const char *ws = " \t\r\n";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) {
        return std::string();
    }
    const auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

bool parseSdpAudioFormat(const std::string &sdp, int *channels, int *sampleRate,
                         Codec *codecOut) {
    const std::string key = "a=rtpmap:";
    size_t pos = sdp.find(key);
    while (pos != std::string::npos) {
        size_t eol = sdp.find("\r\n", pos);
        if (eol == std::string::npos) {
            eol = sdp.find(char(10), pos);     // tolerate LF-only SDP
        }
        if (eol == std::string::npos) {
            eol = sdp.size();
        }
        const std::string line = trim(sdp.substr(pos + key.size(), eol - pos - key.size()));
        size_t sp = line.find_first_of(" \t");
        if (sp != std::string::npos) {
            // RFC 4566 allows exactly one space, but a second one from a tolerant
            // parser must not turn "L16" into " L16" and lose the stream.
            const size_t encStart = line.find_first_not_of(" \t", sp);
            if (encStart == std::string::npos) {
                break;
            }
            const std::string enc = line.substr(encStart);
            const size_t slash1 = enc.find(char(47));
            if (slash1 != std::string::npos) {
                const size_t slash2 = enc.find(char(47), slash1 + 1);
                // RFC 4566 rtpmap is  <encoding>/<clock rate>[/<channels>].
                // Only L16 and L24 are linear PCM: accepting any encoding made the
                // receiver play OPUS or G.722 as PCM -- noise instead of a clear
                // refusal.
                Codec codec = Codec::L16;
                if (parseCodecName(enc.substr(0, slash1), &codec)) {
                    const int sr = std::atoi(
                        enc.substr(slash1 + 1, slash2 == std::string::npos
                                                       ? std::string::npos
                                                       : slash2 - slash1 - 1)
                            .c_str());
                    // The channels field is optional in RFC 4566 and defaults to 1.
                    const int ch = slash2 == std::string::npos
                                       ? 1
                                       : std::atoi(enc.substr(slash2 + 1).c_str());
                    // Bounded on purpose: this text arrived over the network. The
                    // parser used to accept any positive channel count, so
                    // "L16/48000/99999" produced a stream description that nothing
                    // downstream could carry, and that a receiver might size buffers
                    // from. These are the same limits the CLI documents (1..64
                    // channels, 8000..384000 Hz), so anything accepted here is a
                    // stream this build can actually open.
                    const bool chOk = ch >= 1 && ch <= 64;
                    const bool srOk = sr >= 8000 && sr <= 384000;
                    if (chOk && srOk) {
                        if (channels)   *channels = ch;
                        if (sampleRate) *sampleRate = sr;
                        if (codecOut)   *codecOut = codec;
                        return true;
                    }
                }
            }
        }
        pos = sdp.find(key, pos + 1);
    }
    return false;
}


bool parseSdpStreamInfo(const std::string &sdp, SapStreamInfo *out) {
    if (!out) {
        return false;
    }
    SapStreamInfo info;
    bool haveGroup = false;
    bool havePort = false;

    size_t pos = 0;
    while (pos <= sdp.size()) {
        size_t eol = sdp.find("\r\n", pos);
        if (eol == std::string::npos) {
            eol = sdp.find(char(10), pos);     // tolerate LF-only SDP
        }
        if (eol == std::string::npos) {
            eol = sdp.size();
        }
        const std::string line = trim(sdp.substr(pos, eol - pos));
        if (line.compare(0, 2, "s=") == 0) {
            info.name = line.substr(2);
        } else if (line.compare(0, 10, "a=recvonly") == 0) {
            // The announcer is receiving this stream, not sending it.
            info.recvOnly = true;
        } else if (line.compare(0, 10, "a=sendonly") == 0) {
            info.recvOnly = false;
        } else if (line.compare(0, 2, "c=") == 0) {
            // c=IN IP4 239.69.1.1/32 [extra addresses]
            const size_t ip4 = line.find("IP4");
            if (ip4 != std::string::npos) {
                size_t start = line.find_first_not_of(" \t", ip4 + 3);
                if (start != std::string::npos) {
                    size_t end = line.find_first_of(" \t/", start);
                    info.group = line.substr(start, end == std::string::npos
                                                       ? std::string::npos
                                                       : end - start);
                    haveGroup = !info.group.empty();
                }
            }
        } else if (line.compare(0, 8, "m=audio ") == 0) {
            // m=audio <port> RTP/AVP <pt>
            const size_t start = 8;
            size_t end = line.find_first_of(" \t", start);
            const std::string portText =
                line.substr(start, end == std::string::npos ? std::string::npos
                                                   : end - start);
            const long port = std::strtol(portText.c_str(), nullptr, 10);
            if (port > 0 && port <= 65535) {
                info.port = static_cast<uint16_t>(port);
                havePort = true;
            }
        }
        if (eol == sdp.size()) {
            break;
        }
        pos = eol + (sdp.compare(eol, 2, "\r\n") == 0 ? 2 : 1);
    }

    int channels = 0;
    int sampleRate = 0;
    Codec codec = Codec::L16;
    const bool haveFormat = parseSdpAudioFormat(sdp, &channels, &sampleRate, &codec);
    if (haveFormat) {
        info.channels = channels;
        info.sampleRate = sampleRate;
        info.codec = codec;
    }

    info.usable = haveGroup && havePort && haveFormat && info.port != 0;
    *out = info;
    return info.usable;
}

} // namespace openaes67
