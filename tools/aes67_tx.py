#!/usr/bin/env python3
"""
aes67_tx.py -- minimal AES67 / ST2110-30 sender (L16) for protocol validation.

Purpose
-------
Prove that the RTP packet path works on this machine before writing any C++.
No third-party dependencies: Python standard library only.

What it sends
-------------
- RTP version 2, payload type 96 (dynamic)
- L16 big-endian PCM, interleaved channels
- 1 ms packets by default (48 frames @ 48 kHz)
- multicast destination, interface-bound

This is a protocol test tool, not the product. The C++ implementation
(src/) is the actual deliverable; this tool exists so the wire format can be
verified against real receivers (or our own aes67_rx.py) in minutes.

Usage
-----
    python aes67_tx.py --iface 192.168.1.10 --dst 239.69.1.1:5004 \
                       --channels 2 --rate 48000 --ptime 1 --seconds 10

    # verify with the receiver in another terminal:
    python aes67_rx.py --iface 192.168.1.10 --group 239.69.1.1:5004 \
                       --channels 2 --rate 48000 --seconds 10 --out rx.wav
"""

import argparse
import math
import socket
import struct
import time

RTP_PAYLOAD_TYPE_L16 = 96
RTP_PAYLOAD_TYPE_L24 = 97
SAMPLES_PER_MS_AT_48K = 48


def sdp_text(addr, port, channels, rate, ptime_ms):
    """Build a minimal AES67-style SDP body (informational; TX does not need it)."""
    return (
        "v=0\r\n"
        "o=- 1 1 IN IP4 {ip}\r\n"
        "s=OpenAES67 test stream\r\n"
        "c=IN IP4 {group}/32\r\n"
        "t=0 0\r\n"
        "m=audio {port} RTP/AVP 96\r\n"
        "a=rtpmap:96 L16/{rate}/{ch}\r\n"
        "a=ptime:{ptime}\r\n"
        "a=ts-refclk:localmac=00-00-00-00-00-00\r\n"
        "a=mediaclk:direct=0\r\n"
    ).format(ip=addr, group=addr, port=port, rate=rate, ch=channels, ptime=ptime_ms)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iface", required=True, help="local IPv4 of the NIC to send on")
    ap.add_argument("--dst", default="239.69.1.1:5004", help="group:port")
    ap.add_argument("--channels", type=int, default=2)
    ap.add_argument("--rate", type=int, default=48000)
    ap.add_argument("--ptime", type=float, default=1.0, help="packet time in ms")
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--freq", type=float, default=1000.0, help="tone frequency for channel 0")
    ap.add_argument("--ttl", type=int, default=32)
    ap.add_argument("--codec", default="l16", choices=["l16", "l24"],
                    help="sample encoding on the wire")
    ap.add_argument("--print-sdp", action="store_true")
    args = ap.parse_args()

    host, port = args.dst.rsplit(":", 1)
    port = int(port)

    frames_per_packet = max(1, int(round(args.rate * args.ptime / 1000.0)))
    total_packets = int(round(args.seconds * 1000.0 / args.ptime))
    period = args.ptime / 1000.0

    if args.print_sdp:
        print(sdp_text(host, port, args.channels, args.rate, int(args.ptime)))

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, args.ttl)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(args.iface))

    ssrc = 0x5EED0001
    seq = 0
    ts = 0
    samples_done = 0
    t0 = time.perf_counter()

    print("AES67 TX")
    print("  iface      : %s" % args.iface)
    print("  dst        : %s:%d" % (host, port))
    bytes_per_sample = 3 if args.codec == "l24" else 2
    pt = RTP_PAYLOAD_TYPE_L24 if args.codec == "l24" else RTP_PAYLOAD_TYPE_L16
    print("  format     : %s %d ch @ %d Hz" % (args.codec.upper(), args.channels, args.rate))
    print("  packet     : %d frames (%.3f ms)" % (frames_per_packet, args.ptime))
    print("  packets    : %d (%.1f s)" % (total_packets, args.seconds))
    print("  payload/pt : %d bytes" % (frames_per_packet * bytes_per_sample * args.channels))

    sent = 0
    for n in range(total_packets):
        # build interleaved L16 frames: ch0 = tone, other channels = silence
        payload = bytearray(frames_per_packet * bytes_per_sample * args.channels)
        base = samples_done
        for i in range(frames_per_packet):
            t = (base + i) / float(args.rate)
            v = int(20000.0 * math.sin(2.0 * math.pi * args.freq * t))
            if v > 32767:
                v = 32767
            elif v < -32768:
                v = -32768
            off = i * bytes_per_sample * args.channels
            if bytes_per_sample == 3:
                # L24: 16-bit value in the top two bytes, low byte zero
                payload[off + 0] = (v >> 8) & 0xFF
                payload[off + 1] = v & 0xFF
                payload[off + 2] = 0
            else:
                payload[off + 0] = (v >> 8) & 0xFF
                payload[off + 1] = v & 0xFF
            # remaining channels stay zero

        header = struct.pack(
            "!BBHII",
            0x80,                       # V=2, P=0, X=0, CC=0
            pt,                         # M=0, PT from --codec
            seq & 0xFFFF,
            ts & 0xFFFFFFFF,
            ssrc,
        )
        sock.sendto(header + payload, (host, port))

        sent += 1
        samples_done += frames_per_packet
        seq = (seq + 1) & 0xFFFF
        ts = (ts + frames_per_packet) & 0xFFFFFFFF

        # pace to real time
        target = t0 + n * period
        now = time.perf_counter()
        if target > now:
            time.sleep(target - now)

    elapsed = time.perf_counter() - t0
    print("done: %d packets in %.2f s (%.1f pkt/s)" % (sent, elapsed, sent / elapsed if elapsed else 0))
    sock.close()


if __name__ == "__main__":
    main()
