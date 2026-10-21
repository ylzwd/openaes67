#!/usr/bin/env python3
"""
aes67_rx.py -- minimal AES67 / ST2110-30 receiver (L16) for protocol validation.

Joins the multicast group on a specific interface, parses RTP, writes a WAV file,
and reports sequence-gap / timing statistics. Standard library only.

Usage
-----
    python aes67_rx.py --iface 192.168.1.10 --group 239.69.1.1:5004 \
                       --channels 2 --rate 48000 --seconds 10 --out rx.wav
"""

import argparse
import socket
import struct
import time
import wave


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iface", required=True, help="local IPv4 of the NIC to listen on")
    ap.add_argument("--group", default="239.69.1.1:5004", help="group:port")
    ap.add_argument("--channels", type=int, default=2)
    ap.add_argument("--rate", type=int, default=48000)
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--codec", default="l16", choices=["l16", "l24"],
                    help="sample encoding expected on the wire")
    ap.add_argument("--out", default="rx.wav")
    args = ap.parse_args()

    grp, port = args.group.rsplit(":", 1)
    port = int(port)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    except OSError:
        pass
    sock.bind(("", port))

    mreq = socket.inet_aton(grp) + socket.inet_aton(args.iface)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
    sock.settimeout(1.0)

    print("AES67 RX")
    print("  iface   : %s" % args.iface)
    print("  group   : %s:%d" % (grp, port))
    print("  out     : %s" % args.out)

    pcm = bytearray()
    packets = 0
    gaps = 0
    last_seq = None
    ssrc = None
    first_ts = None
    t_start = time.perf_counter()
    deadline = t_start + args.seconds
    bytes_per_sample = 3 if args.codec == "l24" else 2
    bytes_per_frame = bytes_per_sample * args.channels
    print("  codec   : %s (%d bytes/sample)" % (args.codec.upper(), bytes_per_sample))

    while time.perf_counter() < deadline:
        try:
            data, _src = sock.recvfrom(65535)
        except socket.timeout:
            continue
        if len(data) < 12:
            continue

        b0, b1, seq, ts, this_ssrc = struct.unpack("!BBHII", data[:12])
        version = (b0 >> 6) & 0x03
        if version != 2:
            continue
        payload = data[12:]

        if ssrc is None:
            ssrc = this_ssrc
            first_ts = ts
        elif this_ssrc != ssrc:
            continue

        if last_seq is not None:
            expected = (last_seq + 1) & 0xFFFF
            if seq != expected:
                gaps += 1
        last_seq = seq

        if len(payload) % bytes_per_frame == 0:
            pcm.extend(payload)
        packets += 1

    elapsed = time.perf_counter() - t_start
    sock.close()

    frames = len(pcm) // bytes_per_frame if bytes_per_frame else 0

    # AES67 payload is big-endian (network order) but WAV stores little-endian.
    if bytes_per_sample == 3:
        # L24: keep the top 16 bits so the WAV stays 16-bit and comparable with L16
        # captures; the low byte of every sample is dropped.
        out16 = bytearray()
        for i in range(0, len(pcm) - 2, 3):
            out16 += bytes((pcm[i + 1], pcm[i + 0]))
        pcm = out16
    else:
        swapped = bytearray(len(pcm))
        swapped[0::2] = pcm[1::2]
        swapped[1::2] = pcm[0::2]
        pcm = swapped

    with wave.open(args.out, "wb") as w:
        w.setnchannels(args.channels)
        w.setsampwidth(2)
        w.setframerate(args.rate)
        w.writeframes(bytes(pcm))

    print("")
    print("result")
    print("  packets   : %d" % packets)
    print("  frames    : %d (%.3f s of audio)" % (frames, frames / float(args.rate) if args.rate else 0))
    print("  seq gaps  : %d" % gaps)
    print("  ssrc      : %s" % ("0x%08X" % ssrc if ssrc is not None else "none"))
    print("  rate      : %.1f pkt/s" % (packets / elapsed if elapsed else 0))
    print("  wav       : %s" % args.out)
    if packets == 0:
        print("")
        print("NOTE: no packets received.")
        print("  - is the sender running?")
        print("  - Windows Firewall may be blocking UDP %d for python.exe" % port)
        print("  - multicast needs --iface set to the NIC that carries the stream")


if __name__ == "__main__":
    main()
