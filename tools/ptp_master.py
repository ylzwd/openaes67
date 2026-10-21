#!/usr/bin/env python3
"""
ptp_master.py -- a deliberately simple PTPv2 test grandmaster.

It exists so the OpenAES67 PTP slave can be verified without any hardware:
the slave must measure the offset this master is told to inject.

Key trick: the master's notion of time is (local clock + --skew-ms). If the
slave reports roughly that skew, the whole Sync / Follow_Up / Delay_Req /
Delay_Resp path and the offset arithmetic are correct.

Messages (two-step, E2E, default multicast 224.0.1.129):
    Sync        -> udp 319   flags twoStep, body zeros
    Follow_Up   -> udp 320   body = preciseOriginTimestamp (t1)
    Delay_Req   <- udp 319   received from the slave
    Delay_Resp  -> udp 320   body = receiveTimestamp (t4) + requesting port

Usage:
    python ptp_master.py --iface 192.168.1.10 --skew-ms 123 --seconds 10
"""

import argparse
import socket
import struct
import time

PTP_EVENT_PORT = 319
PTP_GENERAL_PORT = 320
PTP_GROUP = "224.0.1.129"

SYNC = 0x0
DELAY_REQ = 0x1
FOLLOW_UP = 0x8
DELAY_RESP = 0x9
ANNOUNCE = 0xB

TWO_STEP_FLAG = 0x0002

UNIX_EPOCH_OFFSET = 0  # we use plain unix time as the PTP epoch for the test


def master_now_ns(skew_ns: int) -> int:
    return time.time_ns() + skew_ns


def ts_bytes(ns: int) -> bytes:
    """48-bit seconds + 32-bit nanoseconds, big endian."""
    secs = ns // 1_000_000_000
    nanos = ns % 1_000_000_000
    return struct.pack("!HII", (secs >> 32) & 0xFFFF, secs & 0xFFFFFFFF, nanos)


def header(msg_type: int, length: int, domain: int, clock_id: bytes,
           seq: int, flags: int, control: int) -> bytes:
    h = struct.pack(
        "!BBHBBHq4s8sHHBB",
        (msg_type << 4) & 0xF0,      # messageType << 4
        0x02,                        # versionPtp
        length,
        domain,
        0,                           # reserved
        flags,
        0,                           # correctionField (int64)
        b"\x00\x00\x00\x00",         # reserved
        clock_id,                    # sourcePortIdentity.clockIdentity
        1,                           # sourcePortIdentity.portNumber
        seq,
        control,
        0x7F,                        # logMessageInterval
    )
    assert len(h) == 34, len(h)
    return h


class Master:
    def __init__(self, iface: str, domain: int, skew_ns: int, clock_id: bytes, rate_ppm: float = 0.0, one_step: bool = False):
        self.domain = domain
        self.skew_ns = skew_ns
        self.rate_ppm = rate_ppm
        self.one_step = one_step
        self.t_start = time.time_ns()
        self.clock_id = clock_id
        self.seq = 0
        self.sync_sent = 0
        self.delay_req_rx = 0

        self.evt = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.evt.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.evt.bind(("", PTP_EVENT_PORT))
        mreq = socket.inet_aton(PTP_GROUP) + socket.inet_aton(iface)
        self.evt.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
        self.evt.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF,
                            socket.inet_aton(iface))
        self.evt.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 8)
        self.evt.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
        self.evt.settimeout(0.002)

        self.gen = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.gen.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF,
                            socket.inet_aton(iface))
        self.gen.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 8)
        self.gen.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)

    def now_ns(self) -> int:
        """Master time = local time + constant skew + rate skew accumulated."""
        now = time.time_ns()
        drift = int((now - self.t_start) * self.rate_ppm / 1e6)
        return now + self.skew_ns + drift

    def send_sync(self):
        t1 = self.now_ns()      # precise send time
        self.seq = (self.seq + 1) & 0xFFFF

        if self.one_step:
            # IEEE 1588 one-step: the timestamp rides in the Sync itself and there is
            # no Follow_Up. A slave that only understands two-step never locks.
            sync = header(SYNC, 34 + 10, self.domain, self.clock_id, self.seq,
                          0, 0x00) + ts_bytes(t1)
            self.evt.sendto(sync, (PTP_GROUP, PTP_EVENT_PORT))
            self.sync_sent += 1
            return

        sync = header(SYNC, 34 + 10, self.domain, self.clock_id, self.seq,
                      TWO_STEP_FLAG, 0x00) + ts_bytes(0)
        self.evt.sendto(sync, (PTP_GROUP, PTP_EVENT_PORT))

        follow = header(FOLLOW_UP, 34 + 10, self.domain, self.clock_id, self.seq,
                        0, 0x02) + ts_bytes(t1)
        self.gen.sendto(follow, (PTP_GROUP, PTP_GENERAL_PORT))
        self.sync_sent += 1

    def pump_delay_req(self):
        try:
            data, _addr = self.evt.recvfrom(128)
        except socket.timeout:
            return
        if len(data) < 44:
            return
        msg_type = (data[0] >> 4) & 0x0F
        if msg_type != DELAY_REQ:
            return
        if data[4] != self.domain:
            return
        recv_ns = self.now_ns()                          # t4
        seq = struct.unpack("!H", data[30:32])[0]
        requester = data[20:30]
        body = ts_bytes(recv_ns) + requester
        resp = header(DELAY_RESP, 34 + len(body), self.domain, self.clock_id, seq,
                      0, 0x03) + body
        self.gen.sendto(resp, (PTP_GROUP, PTP_GENERAL_PORT))
        self.delay_req_rx += 1

    def send_announce(self):
        announce = header(ANNOUNCE, 34 + 30, self.domain, self.clock_id,
                          self.seq, 0, 0x05)
        announce += ts_bytes(0) + struct.pack("!HBBBB", 37, 0, 128, 0x21, 128)
        announce += self.clock_id + struct.pack("!HB", 0, 0xA0)
        self.gen.sendto(announce, (PTP_GROUP, PTP_GENERAL_PORT))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iface", required=True)
    ap.add_argument("--domain", type=int, default=0)
    ap.add_argument("--skew-ms", type=float, default=0.0,
                    help="inject this many ms of offset into the master clock")
    ap.add_argument("--rate-ppm", type=float, default=0.0,
                    help="run the master clock fast (+) or slow (-) by this many ppm")
    ap.add_argument("--one-step", action="store_true",
                    help="one-step master: timestamp in Sync, no Follow_Up")
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument(
        "--clock-id",
        default="DEADBEEF00000001",
        help="16 hex digits: the grandmaster identity announced by this master."
        " Two masters with different ids let a test verify that the sender"
        " re-announces with a new SDP version when the grandmaster changes.",
    )
    ap.add_argument("--sync-ms", type=float, default=125.0)
    args = ap.parse_args()

    skew_ns = int(args.skew_ms * 1_000_000)
    clock_id = bytes.fromhex(args.clock_id)
    m = Master(args.iface, args.domain, skew_ns, clock_id, args.rate_ppm,
               one_step=args.one_step)

    print("PTP test master")
    print("  iface     : %s" % args.iface)
    print("  group     : %s  event=%d general=%d" % (PTP_GROUP, PTP_EVENT_PORT, PTP_GENERAL_PORT))
    print("  domain    : %d" % args.domain)
    print("  injected  : %.3f ms, rate %+.1f ppm" % (args.skew_ms, args.rate_ppm))
    print("  sync every: %.1f ms  (%s)" % (args.sync_ms,
                                            "one-step" if args.one_step else "two-step"))
    print("  duration  : %.1f s" % args.seconds)

    t_end = time.time() + args.seconds
    next_sync = time.time()
    next_announce = time.time() + 1.0
    sync_dt = args.sync_ms / 1000.0

    while time.time() < t_end:
        now = time.time()
        if now >= next_sync:
            m.send_sync()
            next_sync += sync_dt
        if now >= next_announce:
            m.send_announce()
            next_announce = now + 1.0
        m.pump_delay_req()

    print("done: sync_sent=%d delay_req_rx=%d" % (m.sync_sent, m.delay_req_rx))


if __name__ == "__main__":
    main()
