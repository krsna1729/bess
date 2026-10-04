# SPDX-License-Identifier: BSD-3-Clause
"""Seeds for checksum_plan_fuzz.cc (input format: see the harness)."""

import struct

import _packets
from _packets import eth, ip4, ipv4, ipv6, tcp, udp

V4, V6 = 0, 1
NET_NONE, NET_IPV4 = 0, 1
TR_NONE, TR_UDP, TR_TCP = 0, 1, 2
FIX, AUTO = 0x40, 0x80
ENC_NONE, ENC_IP, ENC_UDP, ENC_GTP = 0, 1, 2, 3
ALL_CAPS = 0xFF  # every checksum/encoding capability, single-segment TX only
MULTI_SEG = 1 << 8
CAP_IPV4, CAP_UDP, CAP_TCP, CAP_OUTER_IPV4, CAP_OUTER_UDP = 1, 2, 4, 8, 16
CAP_GENERIC_IP, CAP_GENERIC_UDP, CAP_GTP = 32, 64, 128

V6_SRC = b"\x20\x01" + b"\0" * 13 + b"\x01"
V6_DST = b"\x20\x01" + b"\0" * 13 + b"\x02"


def plan(ipv, net, tr, no=14, to=0, flags=0):
    return bytes([ipv | net << 2 | tr << 4 | flags]) + struct.pack("<HH", no, to)


def profile(caps=ALL_CAPS, outer=True, own_outer=None, inner=None,
            encoding=ENC_NONE, outer_ipv=V4, outer_offset=0):
    ctl = (1 if outer else 0) | (2 if inner else 0) | (4 if own_outer else 0)
    ctl |= encoding << 3 | outer_ipv << 6
    out = bytes([ctl]) + struct.pack("<HH", caps, outer_offset)
    if outer and own_outer:
        out += own_outer
    if inner:
        out += inner
    return out


def encode(main_plan, packet, splits=(), headroom=0, tailroom=0, prof=None):
    out = main_plan + bytes([len(splits)]) + bytes(splits)
    out += bytes([headroom, tailroom])
    out += prof if prof is not None else profile()
    return out + packet


def udp_zero_checksum_payload():
    """A 2-byte payload whose UDP checksum computes to 0 (sent as 0xffff)."""
    src, dst = ip4(10, 0, 0, 1), ip4(10, 0, 0, 2)
    for w in range(0x10000):
        seg = struct.pack("!HHHHH", 1234, 80, 10, 0, w)
        pseudo = src + dst + struct.pack("!BBH", 0, 17, 10)
        if _packets.csum(pseudo + seg) == 0:
            return struct.pack("!H", w)
    raise AssertionError("no payload")


def seeds():
    v4_udp = _packets.eth_ipv4_udp(b"hello")
    v4_udp_odd = _packets.eth_ipv4_udp(b"abc")
    v4_tcp = _packets.eth_ipv4_tcp(b"payload1")
    v6_udp = _packets.eth_ipv6_udp(b"hello world")
    v6_tcp = _packets.eth_ipv6_tcp(b"xyz")
    v4_opts = eth(ipv4(tcp(b"opt", options=b"\x02\x04\x05\xb4"), 6,
                       options=b"\x01\x01\x01\x00"))
    v4_udp_opts = eth(ipv4(udp(b"q"), 17, options=b"\x94\x04\x00\x00"))
    padded = _packets.eth_ipv4_udp(b"")
    padded += bytes(60 - len(padded))
    zero = _packets.eth_ipv4_udp(udp_zero_checksum_payload())
    deep = bytes(range(130)) + ipv4(udp(b"deep"), 17)

    v4_udp_plan = plan(V4, NET_IPV4, TR_UDP, 14, 34)
    v4_tcp_plan = plan(V4, NET_IPV4, TR_TCP, 14, 34)
    v6_udp_plan = plan(V6, NET_NONE, TR_UDP, 14, 54)
    v6_tcp_plan = plan(V6, NET_NONE, TR_TCP, 14, 54)

    s = {}
    # Accepted plans, flat and split at awkward offsets.
    s["v4_udp"] = encode(v4_udp_plan, v4_udp)
    s["v4_udp_split_ip_checksum"] = encode(v4_udp_plan, v4_udp_odd, [14, 11],
                                           headroom=3, tailroom=5)
    s["v4_udp_split_udp_checksum"] = encode(v4_udp_plan, v4_udp_odd, [41],
                                            prof=profile(ALL_CAPS | MULTI_SEG))
    s["v4_udp_zero_checksum"] = encode(plan(V4, NET_NONE, TR_UDP, 14, 34), zero,
                                       prof=profile(0))
    s["v4_udp_padded"] = encode(v4_udp_plan, padded, [7, 0, 30], 1, 1)
    s["v4_udp_ip_options"] = encode(plan(V4, NET_IPV4, TR_UDP, 14, 0, AUTO),
                                    v4_udp_opts, [19])
    s["v4_tcp"] = encode(v4_tcp_plan, v4_tcp, prof=profile(CAP_TCP))
    s["v4_tcp_split_tcp_checksum"] = encode(v4_tcp_plan, v4_tcp, [51, 1],
                                            prof=profile(CAP_TCP | MULTI_SEG))
    s["v4_tcp_options"] = encode(plan(V4, NET_IPV4, TR_TCP, 14, 0, AUTO),
                                 v4_opts, [1] * 7, 2, 2)
    s["v4_network_only"] = encode(plan(V4, NET_IPV4, TR_NONE, 14, 0), v4_tcp,
                                  prof=profile(CAP_IPV4))
    s["v4_transport_only"] = encode(plan(V4, NET_NONE, TR_TCP, 14, 34), v4_tcp,
                                    prof=profile(CAP_TCP))
    s["v4_nothing"] = encode(plan(V4, NET_NONE, TR_NONE, 0, 0), b"",
                             prof=profile(outer=False))
    s["v4_fix_lengths"] = encode(
        plan(V4, NET_IPV4, TR_UDP, 14, 34, FIX), v4_udp + b"extra")
    s["v4_l2_too_long"] = encode(plan(V4, NET_IPV4, TR_UDP, 130, 150), deep)
    s["v4_header_past_head"] = encode(v4_udp_plan, v4_udp, [20],
                                      prof=profile(ALL_CAPS | MULTI_SEG))
    s["v4_multiseg_no_tx"] = encode(v4_tcp_plan, v4_tcp, [60])
    s["v6_udp"] = encode(v6_udp_plan, v6_udp, prof=profile(CAP_UDP))
    s["v6_udp_split"] = encode(v6_udp_plan, v6_udp, [1] * 7, 4, 4)
    s["v6_tcp"] = encode(v6_tcp_plan, v6_tcp, [70, 3],
                         prof=profile(CAP_TCP | MULTI_SEG))
    s["v6_tcp_auto_fix"] = encode(plan(V6, NET_NONE, TR_TCP, 14, 0, FIX | AUTO),
                                  v6_tcp + b"\0")
    # Rejected plans.
    s["bad_v6_network_checksum"] = encode(plan(V6, NET_IPV4, TR_UDP, 14, 54), v6_udp)
    s["bad_enum_values"] = encode(bytes([0x3E]) + struct.pack("<HH", 14, 34), v4_udp)
    s["bad_protocol"] = encode(plan(V4, NET_IPV4, TR_TCP, 14, 34), v4_udp)
    s["bad_transport_offset"] = encode(plan(V4, NET_IPV4, TR_UDP, 14, 30), v4_udp)
    frag = eth(ipv4(udp(b"frag"), 17, flags_frag=0x2000))
    s["bad_fragment"] = encode(v4_udp_plan, frag)
    s["bad_truncated"] = encode(v4_udp_plan, v4_udp[:-1])
    bad_tcp = bytearray(v4_tcp)
    bad_tcp[14 + 20 + 12] = 0x40
    s["bad_tcp_offset"] = encode(v4_tcp_plan, bytes(bad_tcp))
    bad_udp_len = bytearray(v4_udp)
    bad_udp_len[14 + 20 + 5] += 1
    s["bad_udp_length"] = encode(v4_udp_plan, bytes(bad_udp_len))
    ext = eth(ipv6(udp(b"e", src=V6_SRC, dst=V6_DST), 0), 0x86DD)
    s["bad_v6_extension"] = encode(v6_udp_plan, ext)
    jumbo = bytearray(v6_udp)
    jumbo[14 + 4:14 + 6] = b"\0\0"
    s["bad_v6_jumbogram"] = encode(v6_udp_plan, bytes(jumbo))
    s["bad_profile_inner_untunneled"] = encode(
        v4_udp_plan, v4_udp, prof=profile(inner=plan(V4, NET_IPV4, TR_UDP, 14, 34)))
    s["bad_profile_encoding"] = encode(
        v4_udp_plan, v4_udp, prof=profile(encoding=5, outer_offset=14))

    # Tunnels: VXLAN-like generic UDP, IP-in-IP, GTP-U.
    inner_tcp = eth(ipv4(tcp(b"inner", src=ip4(192, 168, 0, 1), dst=ip4(192, 168, 0, 2)),
                         6, src=ip4(192, 168, 0, 1), dst=ip4(192, 168, 0, 2)))
    vxlan = eth(ipv4(udp(b"\x08\0\0\0\0\0\x01\0" + inner_tcp, dport=4789), 17))
    s["tunnel_generic_udp"] = encode(
        plan(V4, NET_IPV4, TR_TCP, 64, 84), vxlan,
        prof=profile(ALL_CAPS, own_outer=plan(V4, NET_IPV4, TR_UDP, 14, 34),
                     inner=plan(V4, NET_IPV4, TR_TCP, 64, 84),
                     encoding=ENC_UDP, outer_offset=14))
    inner_udp = ipv4(udp(b"ipip", src=ip4(172, 16, 0, 1), dst=ip4(172, 16, 0, 2)), 17,
                     src=ip4(172, 16, 0, 1), dst=ip4(172, 16, 0, 2))
    ipip = eth(ipv4(inner_udp, 4))
    s["tunnel_generic_ip"] = encode(
        plan(V4, NET_IPV4, TR_UDP, 34, 54), ipip,
        prof=profile(ALL_CAPS, own_outer=plan(V4, NET_IPV4, TR_NONE, 14, 0),
                     inner=plan(V4, NET_IPV4, TR_UDP, 34, 54),
                     encoding=ENC_IP, outer_offset=14))
    gtp_inner = ipv4(udp(b"gtp", src=ip4(10, 9, 0, 1), dst=ip4(10, 9, 0, 2)), 17,
                     src=ip4(10, 9, 0, 1), dst=ip4(10, 9, 0, 2))
    gtp = eth(ipv4(udp(struct.pack("!BBHI", 0x30, 0xFF, len(gtp_inner), 1) + gtp_inner,
                       dport=2152), 17))
    s["tunnel_gtp_inner_only"] = encode(
        plan(V4, NET_IPV4, TR_UDP, 50, 70), gtp,
        prof=profile(CAP_GTP | CAP_UDP | CAP_IPV4, outer=False,
                     inner=plan(V4, NET_IPV4, TR_UDP, 50, 70),
                     encoding=ENC_GTP, outer_offset=14))
    s["tunnel_no_encoding_caps"] = encode(
        plan(V4, NET_IPV4, TR_UDP, 14, 34), vxlan, [40],
        prof=profile(CAP_IPV4 | CAP_UDP | CAP_TCP | CAP_OUTER_IPV4 | CAP_OUTER_UDP,
                     own_outer=plan(V4, NET_IPV4, TR_UDP, 14, 34),
                     inner=plan(V4, NET_IPV4, TR_TCP, 64, 84),
                     encoding=ENC_UDP, outer_offset=14))
    return s
