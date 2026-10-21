#include "openaes67/rtcp.h"

#include <chrono>
#include <cstring>

namespace openaes67 {

namespace {

void put16(uint8_t *p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v & 0xFF);
}
void put24(uint8_t *p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>(v & 0xFF);
}
void put32(uint8_t *p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(v & 0xFF);
}
uint16_t get16(const uint8_t *p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
uint32_t get24(const uint8_t *p) {
    return (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2];
}
uint32_t get32(const uint8_t *p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// RTCP header: V=2, P=0, RC (5 bits), PT (8), length in 32-bit words minus one.
void writeHeader(uint8_t *p, int reportCount, uint8_t pt, size_t bytes) {
    p[0] = static_cast<uint8_t>(0x80 | (reportCount & 0x1F));
    p[1] = pt;
    put16(p + 2, static_cast<uint16_t>(bytes / 4 - 1));
}

void writeBlock(uint8_t *p, const RtcpReportBlock &b) {
    put32(p + 0, b.ssrc);
    p[4] = b.fractionLost;
    put24(p + 5, static_cast<uint32_t>(b.cumulativeLost) & 0xFFFFFF);
    put32(p + 8, b.highestSequence);
    put32(p + 12, b.jitter);
    put32(p + 16, b.lastSenderReport);
    put32(p + 20, b.delaySinceLastSr);
}

void readBlock(const uint8_t *p, RtcpReportBlock *b) {
    b->ssrc = get32(p + 0);
    b->fractionLost = p[4];
    // Sign-extend the 24-bit cumulative loss count.
    int32_t lost = static_cast<int32_t>(get24(p + 5));
    if (lost & 0x800000) {
        lost |= static_cast<int32_t>(0xFF000000);
    }
    b->cumulativeLost = lost;
    b->highestSequence = get32(p + 8);
    b->jitter = get32(p + 12);
    b->lastSenderReport = get32(p + 16);
    b->delaySinceLastSr = get32(p + 20);
}

// SDES chunk with a single CNAME item, padded to a 32-bit boundary.
size_t writeSdes(uint8_t *p, size_t room, uint32_t ssrc, const std::string &cname) {
    const size_t itemLen = cname.size() + 2;   // type + length byte
    size_t chunk = 4 + itemLen + 1;            // SSRC + item + END
    const size_t padded = (chunk + 3) & ~static_cast<size_t>(3);
    if (room < 4 + padded) {
        return 0;
    }
    writeHeader(p, 1, kRtcpSdes, 4 + padded);
    put32(p + 4, ssrc);
    size_t off = 8;
    p[off++] = 1;                              // CNAME
    p[off++] = static_cast<uint8_t>(cname.size());
    std::memcpy(p + off, cname.data(), cname.size());
    off += cname.size();
    p[off++] = 0;                              // END
    while (off < 4 + padded) {
        p[off++] = 0;
    }
    return 4 + padded;
}

} // namespace

uint64_t rtcpNtpNow() {
    // Seconds since 1900-01-01 for the NTP epoch, then the fractional part at
    // 2^-32 resolution.
    const auto now = std::chrono::system_clock::now();
    const auto sinceEpoch = now.time_since_epoch();
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(sinceEpoch).count();
    const auto frac = std::chrono::duration_cast<std::chrono::nanoseconds>(sinceEpoch - 
                        std::chrono::seconds(secs)).count();
    constexpr int64_t kNtpOffset = 2208988800LL;   // 1900 -> 1970
    const uint64_t ntpSecs = static_cast<uint64_t>(secs + kNtpOffset) & 0xFFFFFFFFULL;
    const uint64_t ntpFrac = static_cast<uint64_t>((static_cast<double>(frac) / 1e9) *
                                                   4294967296.0) & 0xFFFFFFFFULL;
    return (ntpSecs << 32) | ntpFrac;
}

std::string buildRtcpSenderReport(const RtcpSenderReport &sr, const std::string &cname) {
    const int blocks = sr.blockCount > 0 ? 1 : 0;
    const size_t srBytes = 28 + static_cast<size_t>(blocks) * 24;
    // 8 bytes of slack: writeSdes rounds the chunk up to a 32-bit boundary and
    // the exact padding depends on the CNAME length. Under-allocating made it
    // return 0 and the compound packet silently lost its CNAME.
    std::string out(srBytes + 4 + 4 + cname.size() + 1 + 8, '\0');
    uint8_t *p = reinterpret_cast<uint8_t *>(&out[0]);

    writeHeader(p, blocks, kRtcpSenderReport, srBytes);
    put32(p + 4, sr.ssrc);
    put32(p + 8, static_cast<uint32_t>(sr.ntp >> 32));
    put32(p + 12, static_cast<uint32_t>(sr.ntp & 0xFFFFFFFF));
    put32(p + 16, sr.rtpTimestamp);
    put32(p + 20, sr.packetCount);
    put32(p + 24, sr.octetCount);
    if (blocks) {
        writeBlock(p + 28, sr.blocks[0]);
    }

    const size_t sdes = writeSdes(p + srBytes, out.size() - srBytes, sr.ssrc, cname);
    if (sdes == 0) {
        out.resize(srBytes);
        return out;
    }
    out.resize(srBytes + sdes);
    return out;
}

std::string buildRtcpReceiverReport(uint32_t reporterSsrc,
                                    const RtcpReceptionStats *stats,
                                    const std::string &cname) {
    const int blocks = stats ? 1 : 0;
    const size_t rrBytes = 8 + static_cast<size_t>(blocks) * 24;
    std::string out(rrBytes + 4 + 4 + cname.size() + 1 + 8, '\0');
    uint8_t *p = reinterpret_cast<uint8_t *>(&out[0]);

    writeHeader(p, blocks, kRtcpReceiverReport, rrBytes);
    put32(p + 4, reporterSsrc);
    if (blocks) {
        RtcpReportBlock b;
        b.ssrc = stats->ssrc;
        b.fractionLost = stats->fractionLost;
        b.cumulativeLost = stats->cumulativeLost;
        b.highestSequence = stats->highestSequence;
        b.jitter = stats->jitter;
        writeBlock(p + 8, b);
    }

    const size_t sdes = writeSdes(p + rrBytes, out.size() - rrBytes, reporterSsrc, cname);
    if (sdes == 0) {
        out.resize(rrBytes);
        return out;
    }
    out.resize(rrBytes + sdes);
    return out;
}

bool parseRtcpSenderReport(const uint8_t *data, size_t len, RtcpSenderReport *out) {
    if (!data || !out || len < 28) {
        return false;
    }
    if ((data[0] >> 6) != 2) {          // version
        return false;
    }
    if (data[1] != kRtcpSenderReport) {
        return false;
    }
    const int blocks = data[0] & 0x1F;
    const size_t need = 28 + static_cast<size_t>(blocks > 1 ? 1 : blocks) * 24;
    if (len < need || blocks > 1) {     // this endpoint never reports more than one
        return false;
    }

    RtcpSenderReport sr;
    sr.ssrc = get32(data + 4);
    sr.ntp = (static_cast<uint64_t>(get32(data + 8)) << 32) | get32(data + 12);
    sr.rtpTimestamp = get32(data + 16);
    sr.packetCount = get32(data + 20);
    sr.octetCount = get32(data + 24);
    sr.blockCount = blocks;
    if (blocks == 1) {
        readBlock(data + 28, &sr.blocks[0]);
    }
    *out = sr;
    return true;
}

bool parseRtcpReceiverReport(const uint8_t *data, size_t len,
                             RtcpReceptionStats *out, uint32_t *reporterSsrc) {
    if (!data || !out || len < 8) {
        return false;
    }
    if ((data[0] >> 6) != 2 || data[1] != kRtcpReceiverReport) {
        return false;
    }
    const int blocks = data[0] & 0x1F;
    if (len < 8 + static_cast<size_t>(blocks) * 24) {
        return false;
    }
    if (reporterSsrc) {
        *reporterSsrc = get32(data + 4);
    }
    if (blocks == 0) {
        return false;
    }
    RtcpReportBlock b;
    readBlock(data + 8, &b);
    out->ssrc = b.ssrc;
    out->fractionLost = b.fractionLost;
    out->cumulativeLost = b.cumulativeLost;
    out->highestSequence = b.highestSequence;
    out->jitter = b.jitter;
    return true;
}

bool parseRtcpCname(const uint8_t *data, size_t len, std::string *cname) {
    if (!data || !cname || len < 8) {
        return false;
    }
    // Walk the compound packet looking for the SDES packet.
    size_t off = 0;
    while (off + 4 <= len) {
        const uint8_t *p = data + off;
        if ((p[0] >> 6) != 2) {
            return false;
        }
        const size_t bytes = (static_cast<size_t>(get16(p + 2)) + 1) * 4;
        if (bytes < 4 || off + bytes > len) {
            return false;
        }
        if (p[1] == kRtcpSdes) {
            if (bytes < 8) {
                return false;
            }
            size_t q = off + 8;             // skip header + first SSRC
            const size_t end = off + bytes;
            while (q + 2 <= end) {
                const uint8_t type = data[q];
                if (type == 0) {            // END
                    break;
                }
                const size_t itemLen = data[q + 1];
                if (q + 2 + itemLen > end) {
                    return false;
                }
                if (type == 1) {            // CNAME
                    cname->assign(reinterpret_cast<const char *>(data + q + 2), itemLen);
                    return true;
                }
                q += 2 + itemLen;
            }
            return false;
        }
        off += bytes;
    }
    return false;
}

} // namespace openaes67
