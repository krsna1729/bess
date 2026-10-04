# SPDX-License-Identifier: BSD-3-Clause
"""Seeds for core/fuzz/nat_fuzz.cc (input format in the harness's top comment)."""

import _packets as P

# Palette indices (nat_fuzz.cc kPalette).
IN1, IN2, IN3, LAN, EXT1, EXT2, REMOTE = range(7)

TCP, UDP, ICMP, OTHER = range(4)
# ICMP type indices (kIcmpTypes): echo request, echo reply, timestamp, ...,
# destination unreachable, time exceeded.
ECHO, ECHO_REPLY, TIMESTAMP, DEST_UNREACH, TIME_EXCEEDED = 0, 1, 2, 6, 7
SYN, SYNACK, ACK, PSHACK = 0x02, 0x12, 0x10, 0x18
# Mapping-relative frames (kind 2).
REPLY, REFRESH, SPOOF, TO_INSIDE = range(4)
VLAN, IPOPT, UDP0, BADIP, BADL4, MF, CUT, PAD = (1 << i for i in range(8))


def header(config=0, cap=8, seed=7, coarse=False):
    return bytes([config | (0x80 if coarse else 0), (cap - 1) % 256]) + P.le16(seed)


def step(frames, reverse=False, delta=1, expire=None):
    long_delta = delta > 255
    ctl = (1 if reverse else 0) | (len(frames) - 1) << 1
    ctl |= (0x10 if expire is not None else 0) | (0x20 if long_delta else 0)
    out = bytes([ctl]) + (P.le16(delta) if long_delta else bytes([delta]))
    if expire is not None:
        out += bytes([expire])
    return out + b"".join(frames)


def addr(a):
    return bytes([a]) if isinstance(a, int) else bytes([7]) + P.le32(a[1])


def opts_bytes(opts, cut, pad):
    out = bytes([opts])
    if opts & CUT:
        out += bytes([cut])
    if opts & PAD:
        out += bytes([pad])
    return out


def raw(data, split=0):
    return b"\x00" + P.le16(len(data)) + data + bytes([split])


def v4(proto, aux, src, sport, dst, dport, payload=0, opts=0, cut=0, pad=0, split=0):
    return (b"\x01" + bytes([proto, aux]) + addr(src) + P.le16(sport) + addr(dst) +
            P.le16(dport) + bytes([payload]) + opts_bytes(opts, cut, pad) + bytes([split]))


def mapping(sel, index, remote=REMOTE, rport=443, payload=0, opts=0, cut=0, pad=0, split=0):
    return (b"\x02" + bytes([sel, index]) + addr(remote) + P.le16(rport) + bytes([payload]) +
            opts_bytes(opts, cut, pad) + bytes([split]))


def v6(proto, sport, dport, payload=0, split=0):
    return b"\x03" + bytes([proto]) + P.le16(sport) + P.le16(dport) + bytes([payload, split])


def udp_out(sport, src=IN1, dport=53):
    return v4(UDP, 0, src, sport, REMOTE, dport, payload=12)


def seeds():
    s = {}
    s["tcp_syn_data_reply"] = header() + step([v4(TCP, SYN, IN1, 40000, REMOTE, 443)]) + \
        step([mapping(REPLY, 0)], reverse=True) + \
        step([mapping(REFRESH, 0, payload=40)]) + \
        step([v4(TCP, PSHACK, IN1, 40000, REMOTE, 443, payload=33)])
    s["tcp_ack_privileged"] = header(config=1) + step([v4(TCP, ACK, IN2, 22, REMOTE, 22, 5)]) + \
        step([mapping(REPLY, 0, rport=22)], reverse=True)
    s["udp_out_reply"] = header() + step([udp_out(5353)]) + \
        step([mapping(REPLY, 0, rport=53, payload=20)], reverse=True)
    s["udp_no_checksum"] = header() + step([v4(UDP, 0, IN2, 6000, REMOTE, 53, 8, opts=UDP0)]) + \
        step([mapping(REPLY, 0, rport=53, opts=UDP0)], reverse=True)
    s["udp_port_zero"] = header() + step([v4(UDP, 0, IN1, 0, REMOTE, 53, 4)])
    s["icmp_echo"] = header() + step([v4(ICMP, ECHO, IN1, 0x1234, REMOTE, 1, 16)]) + \
        step([mapping(REPLY, 0, rport=1, payload=16)], reverse=True)
    s["icmp_timestamp_id0"] = header(config=1) + \
        step([v4(ICMP, TIMESTAMP, IN3, 0, REMOTE, 1, 12)])
    s["icmp_error_quoting_flow"] = header() + step([udp_out(7000)]) + \
        step([v4(ICMP, DEST_UNREACH, REMOTE, 0, EXT1, 0)], reverse=True) + \
        step([v4(ICMP, TIME_EXCEEDED, IN1, 7000, REMOTE, 53)])
    # Three echo replies (sequence 0, no payload) on a pool whose ICMP span is
    # identifiers 0-2: one is rewritten to identifier 0, an all-zero message
    # whose checksum must be 0xffff (the bug this harness found in Rewrite).
    s["icmp_reply_to_id_zero"] = header(config=1) + \
        step([v4(ICMP, ECHO_REPLY, IN1, 1 + i, REMOTE, 0) for i in range(3)])
    s["inbound_unsolicited"] = header() + \
        step([v4(UDP, 0, REMOTE, 53, EXT1, 1025, 4), v4(TCP, SYN, REMOTE, 80, EXT1, 1024)],
             reverse=True) + \
        step([mapping(TO_INSIDE, 0)], reverse=True)
    s["truncated"] = header() + step([
        v4(TCP, SYN, IN1, 40001, REMOTE, 443, 4, opts=CUT, cut=10),
        v4(UDP, 0, IN1, 40002, REMOTE, 53, 8, opts=CUT, cut=3),
        v4(ICMP, ECHO, IN1, 9, REMOTE, 1, 0, opts=CUT, cut=30),
        raw(P.eth_ipv4_udp(sport=4000)[:20]),
    ])
    s["ipv6"] = header() + step([v6(0, 40000, 443), v6(1, 5353, 53, 9), v6(2, 7, 1, 4)]) + \
        step([v6(1, 53, 5353, 3)], reverse=True)
    s["exhaust_ports"] = header(cap=16) + \
        step([udp_out(40000 + i) for i in range(8)]) + \
        step([v4(UDP, 0, IN2, 100 + i, REMOTE, 53) for i in range(5)])
    s["table_full"] = header(cap=2) + step([udp_out(5000), udp_out(5001), udp_out(5002)])
    s["expiry_exact"] = header(cap=4) + step([udp_out(5000)], delta=0) + \
        step([udp_out(5001)], delta=500) + \
        step([mapping(REFRESH, 0)], delta=400) + \
        step([mapping(REPLY, 0)], reverse=True, delta=300, expire=0xFF) + \
        step([mapping(REPLY, 0)], reverse=True, delta=1000, expire=0xFF)
    s["expiry_budget_coarse"] = header(cap=8, coarse=True) + \
        step([udp_out(5000 + i) for i in range(4)], delta=3) + \
        step([mapping(REPLY, 1)], reverse=True, delta=1001, expire=1) + \
        step([mapping(REPLY, 1)], reverse=True, delta=2, expire=0) + \
        step([mapping(REPLY, 1)], reverse=True, delta=9, expire=0xFF)
    s["conflicts"] = header(config=2, cap=8) + \
        step([udp_out(40000), udp_out(40001), v4(UDP, 0, IN1, 1000, REMOTE, 53)]) + \
        step([mapping(SPOOF, 0), mapping(SPOOF, 1)]) + \
        step([mapping(TO_INSIDE, 0)], reverse=True) + \
        step([v4(UDP, 0, EXT1, 2000, REMOTE, 53), v4(TCP, SYN, IN1, 1001, REMOTE, 80)]) + \
        step([v4(UDP, 0, LAN, 5000 + i, REMOTE, 53) for i in range(3)])
    s["layouts"] = header() + step([
        v4(TCP, SYN, IN1, 41000, REMOTE, 443, 10, opts=VLAN),
        v4(UDP, 0, IN2, 41001, REMOTE, 53, 10, opts=IPOPT),
        v4(UDP, 0, IN3, 41002, REMOTE, 53, 50, split=50),
        v4(ICMP, ECHO, LAN, 77, REMOTE, 1, 20, opts=PAD, pad=6),
        v4(UDP, 0, IN1, 41003, REMOTE, 53, 30, opts=MF),
        v4(TCP, ACK, IN2, 41004, REMOTE, 80, 3, opts=BADIP | BADL4),
        v4(OTHER, 47, IN1, 0, REMOTE, 0, 24),
        v4(UDP, 0, (7, 0x0A0A0A0A), 41005, (7, 0x01020304), 53, 2),
    ])
    s["batch_mixed"] = header(config=1, cap=16) + step([
        udp_out(30000), udp_out(30000), udp_out(30001, IN2),
        v4(TCP, SYN, IN3, 30002, REMOTE, 443),
        v4(ICMP, ECHO, IN1, 5, REMOTE, 1, 8), udp_out(0),
        v4(UDP, 0, REMOTE, 53, EXT2, 65531), udp_out(30003, LAN),
    ]) + step([mapping(REPLY, i, rport=53) for i in range(8)], reverse=True)
    s["raw_packets"] = header() + step([
        raw(P.eth_ipv4_tcp(sport=42000, dport=443)),
        raw(P.eth_ipv4_udp(b"query", sport=42001, dport=53)),
        raw(P.eth_ipv4_icmp(b"ping!")),
        raw(P.eth_ipv6_udp()),
        raw(P.eth(b"\x00\x01\x08\x00\x06\x04\x00\x01" + bytes(20), 0x0806)),
        raw(P.eth_ipv4_tcp(b"x" * 40, sport=42002, dport=80, flags=0x18), split=40),
    ]) + step([raw(P.eth_ipv4_udp(b"answer", sport=53, dport=1024, src=P.ip4(8, 8, 8, 8),
                                  dst=P.ip4(198, 51, 100, 1)))], reverse=True)
    return s
