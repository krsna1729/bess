# SPDX-License-Identifier: BSD-3-Clause
"""Seeds for conntrack_fuzz.cc (input format: the harness's top comment).

Stage A (parser): valid frames of each protocol, VLAN tags, IPv4 options,
IPv6 extension headers, fragments, truncations and lies, each with a
total_len mode. Stage B (tracker): scripted connection lives -- a TCP
handshake, data and close (IPv4/IPv6), reset, reopen after TIME_WAIT, UDP
request/reply, ICMP echo and errors quoting tracked flows, fragments, zones,
a full table, Remove/SetDeadline and expiry with small and full budgets.
"""

import struct

import _packets as P

SYN, ACK, FIN, RST = 0x02, 0x10, 0x01, 0x04

# -- stage A ----------------------------------------------------------------------


def parse(frame, mode=0, k=0):
    return bytes([0, mode]) + struct.pack("<H", k) + frame


def v6_with_ext(ext_chain, first_nh, l4):
    src = b"\x20\x01" + b"\0" * 13 + b"\x01"
    dst = b"\x20\x01" + b"\0" * 13 + b"\x02"
    body = ext_chain + l4
    return P.eth(P.ipv6(body, first_nh, src, dst), 0x86DD)


def stage_a():
    tcp4 = P.eth_ipv4_tcp(b"data", flags=SYN | ACK)
    udp4 = P.eth_ipv4_udp(b"hello")
    icmp4 = P.eth_ipv4_icmp(b"ping")
    udp6 = P.eth_ipv6_udp(b"hello")
    tcp6 = P.eth_ipv6_tcp(b"", flags=SYN)
    v6src = b"\x20\x01" + b"\0" * 13 + b"\x01"
    v6dst = b"\x20\x01" + b"\0" * 13 + b"\x02"
    one_tag = P.eth(P.ipv4(P.udp(b"x"), 17), vlan=7)
    icmp6 = P.eth(P.ipv6(P.icmp6(b"ping", src=v6src, dst=v6dst), 58, v6src, v6dst), 0x86DD)
    opts = P.eth(P.ipv4(P.tcp(b"", options=b"\x01\x01\x01\x00"), 6,
                        options=b"\x01\x01\x01\x00" * 3))
    # An 802.1ad EtherType with no tag: the IP header is read as the tag.
    bare_tag = P.eth(P.ipv4(P.udp(b"x"), 17), ethertype=0x88A8)
    # A hand-made double tag: 802.1ad then 802.1Q, then IPv4.
    l2 = b"\x02\0\0\0\0\x02\x02\0\0\0\0\x01"
    double = l2 + b"\x88\xa8\x00\x05\x81\x00\x00\x07\x08\x00" + P.ipv4(P.udp(b"x"), 17)
    gre4 = P.eth(P.ipv4(b"\x00\x00\x08\x00", 47))
    hbh = bytes([60, 0]) + b"\x01\x04\0\0\0\0"          # hop-by-hop -> dest opts
    dopt = bytes([44, 0]) + b"\x01\x04\0\0\0\0"         # dest opts -> fragment
    frag_first = bytes([17, 0, 0x00, 0x01, 0, 0, 0, 9])  # offset 0, M
    frag_later = bytes([17, 0, 0x05, 0xC8, 0, 0, 0, 9])  # offset 185
    ext = v6_with_ext(hbh + dopt + frag_first, 0, P.udp(b"z" * 8, src=v6src, dst=v6dst))
    later6 = v6_with_ext(frag_later, 44, b"\x5a" * 16)
    frag4_first = P.eth(P.ipv4(P.udp(b"y" * 8), 17, flags_frag=0x2000))
    frag4_later = P.eth(P.ipv4(b"\x5a" * 16, 17, flags_frag=0x00B9))
    arp = P.eth(b"\x00\x01\x08\x00\x06\x04\x00\x01" + b"\0" * 20, 0x0806)
    mismatch = P.eth(P.ipv6(P.udp(b"m"), 17), 0x0800)  # EtherType says IPv4
    lie = bytearray(udp4)
    lie[16:18] = struct.pack("!H", 200)  # IPv4 total length beyond the frame
    out = {
        "a_tcp4": parse(tcp4),
        "a_tcp4_total_eq": parse(tcp4, 1),
        "a_tcp4_total_less": parse(tcp4, 2, 10),
        "a_tcp4_total_more": parse(tcp4, 3, 100),
        "a_udp4": parse(udp4),
        "a_udp4_total_more": parse(udp4, 3, 7),
        "a_icmp4": parse(icmp4),
        "a_udp6": parse(udp6),
        "a_tcp6": parse(tcp6, 1),
        "a_icmp6": parse(icmp6),
        "a_ipv4_options": parse(opts),
        "a_one_tag": parse(one_tag),
        "a_bare_tag": parse(bare_tag),
        "a_double_tag": parse(double),
        "a_gre4_other": parse(gre4),
        "a_ipv6_ext_first_fragment": parse(ext),
        "a_ipv6_later_fragment": parse(later6),
        "a_ipv4_first_fragment": parse(frag4_first, 3, 200),
        "a_ipv4_later_fragment": parse(frag4_later),
        "a_arp_not_ip": parse(arp),
        "a_version_mismatch": parse(mismatch),
        "a_length_lie": parse(bytes(lie)),
        "a_length_lie_total_more": parse(bytes(lie), 3, 200),
        "a_tcp4_cut_in_l4": parse(tcp4[:14 + 20 + 13]),
        "a_tcp4_cut_in_l4_total": parse(tcp4[:14 + 20 + 13], 3, 30),
        "a_ipv6_cut_in_ext": parse(ext[:14 + 40 + 5], 3, 64),
        "a_runt": parse(tcp4[:13]),
    }
    return out


# -- stage B ----------------------------------------------------------------------

TCP, UDP, ECHO, ECHO_REPLY, ERROR, GRE, ICMP_OTHER, FRAG = range(8)
V6, VLAN, SWAP = 8, 16, 32
# Endpoints: client 10.0.0.1:1000 (index 0 / port 0) -> server 10.0.0.2:80
# (index 1 / port 2).
CLIENT_SERVER = 0 | (1 << 2) | (0 << 4) | (2 << 6)
OTHER_CLIENT = 2 | (1 << 2) | (1 << 4) | (2 << 6)  # 192.168.7.9:2000 -> server


def tracker(capacity, steps, config=0, start=100):
    return bytes([1, capacity - 1, config]) + struct.pack("<H", start) + b"".join(steps)


def track(kind, ends=CLIENT_SERVER, operands=b"", delta=1, zone=0, may_create=True):
    op = (zone << 3) | (0 if may_create else 0x30)
    return bytes([op, delta, kind, ends]) + operands


def tcp(flags, reply=False, v6=False, **kw):
    return track(TCP | (SWAP if reply else 0) | (V6 if v6 else 0),
                 operands=bytes([flags]), **kw)


def udp(reply=False, v6=False, **kw):
    return track(UDP | (SWAP if reply else 0) | (V6 if v6 else 0), operands=b"\x01", **kw)


def expire(delta, full=True, budget=0):
    return bytes([4 | (8 if full else 0) | (budget << 4), delta])


def remove(pick, delta=0):
    return bytes([5, delta, pick])


def set_deadline(pick, ahead, delta=0):
    return bytes([6, delta, pick, ahead])


def raw(frame, delta=0):
    return bytes([7, delta, len(frame)]) + frame


def error_about(inner_kind, v6=False, ends=(1 | (0 << 2)), qdst=1, time_exceeded=False):
    # An error from the server's side (index 1) to the client (index 0) about
    # the client's packet to `qdst`, ports client 1000 -> 80.
    b2 = qdst | (0 << 2) | (2 << 4) | (inner_kind << 6)
    if time_exceeded:
        b2 |= 0x80
    return track(ERROR | (V6 if v6 else 0), ends=ends, operands=bytes([b2]))


def stage_b():
    handshake = [tcp(SYN), tcp(SYN | ACK, reply=True), tcp(ACK), tcp(ACK | 0x08),
                 tcp(ACK, reply=True)]
    close = [tcp(FIN | ACK), tcp(FIN | ACK, reply=True), tcp(ACK)]
    out = {
        "b_tcp4_lifecycle": tracker(4, handshake + close + [expire(16), expire(2)]),
        "b_tcp6_lifecycle": tracker(4, [tcp(SYN, v6=True), tcp(SYN | ACK, reply=True, v6=True),
                                        tcp(ACK, v6=True), tcp(RST, reply=True, v6=True),
                                        expire(3)], config=2),
        "b_tcp_reset": tracker(4, handshake + [tcp(RST), tcp(ACK), expire(2)]),
        "b_tcp_reopen_after_time_wait": tracker(
            4, handshake + close + [tcp(SYN, reply=True), tcp(SYN | ACK),
                                    tcp(ACK, reply=True), expire(50)]),
        "b_tcp_reopen_refused": tracker(4, handshake + close + [
            tcp(SYN, may_create=False), tcp(SYN)]),
        "b_tcp_simultaneous_open": tracker(4, [tcp(SYN), tcp(SYN, reply=True),
                                               tcp(SYN | ACK), tcp(ACK, reply=True)]),
        "b_tcp_pickup": tracker(4, [tcp(ACK), tcp(ACK, reply=True), expire(60)], config=4),
        "b_tcp_invalid_starts": tracker(4, [tcp(ACK), tcp(SYN | ACK), tcp(FIN), tcp(0)]),
        "b_udp_request_reply": tracker(4, [udp(), expire(3), udp(delta=2), expire(4),
                                           udp(), udp(reply=True), expire(22, full=False, budget=1),
                                           expire(2)]),
        "b_udp6_zones": tracker(4, [udp(v6=True), udp(v6=True, zone=1),
                                    udp(reply=True, v6=True, zone=1), expire(5)], config=3),
        "b_icmp_echo_and_errors": tracker(8, [
            track(ECHO), track(ECHO_REPLY | SWAP), track(ECHO_REPLY, ends=OTHER_CLIENT),
            udp(), tcp(SYN),
            error_about(1), error_about(0, time_exceeded=True), error_about(2),
            error_about(1, qdst=2),  # no such flow
            track(ICMP_OTHER), expire(7)]),
        "b_icmp6_echo_and_errors": tracker(8, [
            track(ECHO | V6), track(ECHO_REPLY | SWAP | V6), udp(v6=True),
            error_about(1, v6=True), error_about(2, v6=True, time_exceeded=True),
            track(ICMP_OTHER | V6)]),
        "b_gre_and_fragments": tracker(4, [
            track(GRE), track(GRE | SWAP | VLAN), track(FRAG, operands=b"\x01"),
            track(FRAG, operands=b"\x00"), track(FRAG | V6, operands=b"\x01"),
            track(FRAG | V6, operands=b"\x00"), expire(40)], config=1),
        "b_full_table_remove_deadline": tracker(2, [
            udp(), tcp(SYN, ends=OTHER_CLIENT), track(ECHO), remove(0), track(ECHO),
            set_deadline(1, 40), expire(10), remove(5), set_deadline(3, 1),
            expire(60), remove(0)]),
        "b_raw_frames": tracker(4, [
            raw(P.eth_ipv4_udp(b"q", 1000, 80)),
            raw(P.eth_ipv4_udp(b"r", 80, 1000, P.ip4(10, 0, 0, 2), P.ip4(10, 0, 0, 1))),
            raw(P.eth_ipv4_tcp(b"", 1000, 80, SYN)[:40]),
            raw(P.eth_ipv6_tcp(b"", 1000, 80, SYN)),
            expire(30)]),
    }
    return out


def seeds():
    out = stage_a()
    out.update(stage_b())
    return out
