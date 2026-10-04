# SPDX-License-Identifier: BSD-3-Clause
"""Seeds for tunnel_decap_fuzz.cc (input format: the harness's top comment).

Stage A (raw frames): one valid encapsulated packet per protocol over IPv4
and IPv6 -- VXLAN, Geneve with options, GRE with checksum/key/sequence, GTP-U
with a sequence number and an extension chain -- plus truncated variants
(with and without a total_len covering the cut), another port, a GTP-U echo
and a reserved GRE version. Stage B (round trip): parameters for each writer,
including invalid Geneve option lengths, GTP-U non-G-PDUs and short inners.
"""

import struct

import _packets as P

V6SRC = b"\x20\x01" + b"\0" * 13 + b"\x01"
V6DST = b"\x20\x01" + b"\0" * 13 + b"\x02"
INNER = P.eth_ipv4_udp(b"inner payload", 1000, 2000)
INNER_IP = INNER[14:]


def outer_udp(payload, dport, v6=False, sport=0xC123):
    if v6:
        return P.eth(P.ipv6(P.udp(payload, sport, dport, V6SRC, V6DST), 17), 0x86DD)
    return P.eth(P.ipv4(P.udp(payload, sport, dport), 17))


def outer_gre(payload, v6=False):
    if v6:
        return P.eth(P.ipv6(payload, 47), 0x86DD)
    return P.eth(P.ipv4(payload, 47))


def vxlan(vni, inner=INNER, flags=0x08):
    return bytes([flags, 0, 0, 0]) + struct.pack("!I", vni << 8) + inner


def geneve(vni, protocol, options=b"", oam=False, critical=False, inner=INNER):
    return (bytes([len(options) // 4, (0x80 if oam else 0) | (0x40 if critical else 0)]) +
            struct.pack("!HI", protocol, vni << 8) + options + inner)


def gre(protocol, payload, key=None, seq=None, checksum=False):
    flags = (0x8000 if checksum else 0) | (0x2000 if key is not None else 0) | \
            (0x1000 if seq is not None else 0)
    hdr = struct.pack("!HH", flags, protocol)
    if checksum:
        hdr += b"\0\0\0\0"
    if key is not None:
        hdr += struct.pack("!I", key)
    if seq is not None:
        hdr += struct.pack("!I", seq)
    if checksum:
        c = P.csum(hdr + payload)
        hdr = hdr[:4] + struct.pack("!H", c) + hdr[6:]
    return hdr + payload


def gtpu(teid, payload, seq=None, ext=(), msg=255):
    flags = 0x30 | (0x02 if seq is not None else 0) | (0x04 if ext else 0)
    opt = b""
    if flags & 0x06:
        nxt = ext[0][0] if ext else 0
        opt = struct.pack("!HBB", seq or 0, 0, nxt)
        for i, (_, content) in enumerate(ext):
            units = (len(content) + 2) // 4
            follow = ext[i + 1][0] if i + 1 < len(ext) else 0
            opt += bytes([units]) + content + bytes([follow])
    body = opt + payload
    return struct.pack("!BBHI", flags, msg, len(body), teid) + body


def raw(frame, mode=0, port=0, tl_mode=0, k=0, gre_off=0):
    return bytes([0, mode]) + struct.pack("<H", port) + bytes([tl_mode]) + \
        struct.pack("<H", k) + bytes([gre_off]) + frame


def round_trip(proto, inner, v6=False, vlan=False, port_mode=0, dst_port=0, cut=0xFFFF,
               ident=0, p1=0, p2=0, p3=0, extra=None):
    cfg = proto | (4 if v6 else 0) | (8 if vlan else 0) | (port_mode << 4)
    out = bytes([1, cfg]) + struct.pack("<HH", dst_port, cut) + struct.pack("<I", ident)
    out += bytes([p1]) + struct.pack("<H", p2) + struct.pack("<I", p3)
    if proto in (1, 3):
        extra = extra or b""
        out += bytes([len(extra)]) + extra
    return out + inner


def stage_a():
    pdu_session = (0x85, b"\x00\x01")  # PDU session container, 1 unit
    frames = {
        "vxlan4": outer_udp(vxlan(0x123456), 4789),
        "vxlan6": outer_udp(vxlan(0xABCDEF), 4789, v6=True),
        "geneve4_options": outer_udp(
            geneve(0x42, 0x6558, b"\x01\x02\x80\x01\xde\xad\xbe\xef", critical=True), 6081),
        "geneve6_ipv4_inner": outer_udp(geneve(7, 0x0800, inner=INNER_IP, oam=True), 6081,
                                        v6=True),
        "gre4_key_seq_csum": outer_gre(gre(0x0800, INNER_IP, key=0x1234, seq=9, checksum=True)),
        "gre6_key": outer_gre(gre(0x6558, INNER, key=0xFFFFFFFF), v6=True),
        "gre4_plain": outer_gre(gre(0x86DD, INNER_IP)),
        "gtpu4_seq_ext": outer_udp(gtpu(0xDEADBEEF, INNER_IP, seq=5,
                                        ext=(pdu_session, (0x40, b"\x11\x22\x33\x44\x55\x66"))),
                                   2152),
        "gtpu6_plain": outer_udp(gtpu(1, INNER_IP), 2152, v6=True),
        "gtpu4_echo": outer_udp(gtpu(0, b"", seq=1, msg=1), 2152),
        "gre4_bad_version": outer_gre(b"\x00\x01\x08\x00" + INNER_IP),
    }
    out = {}
    for name, f in frames.items():
        out["a_" + name] = raw(f, gre_off=34 if name.startswith("gre4") else 54)
    # Truncations: plain, and with total_len saying the rest exists.
    for name in ("vxlan4", "geneve4_options", "gre4_key_seq_csum", "gre6_key", "gtpu4_seq_ext"):
        f = frames[name]
        for cut in (14 + 20 + 8 + 4, 14 + 40 + 12, len(f) - len(INNER_IP) + 3):
            out["a_%s_cut%d" % (name, cut)] = raw(f[:cut])
            out["a_%s_cut%d_total" % (name, cut)] = raw(f[:cut], tl_mode=3, k=len(f) - cut)
    out["a_vxlan4_any_port"] = raw(outer_udp(vxlan(1), 8472), mode=2)
    out["a_vxlan4_other_port"] = raw(outer_udp(vxlan(1), 8472), mode=1, port=8472)
    out["a_geneve4_other_port"] = raw(outer_udp(geneve(1, 0x6558), 7000), mode=4, port=7000)
    out["a_gtpu4_total_less"] = raw(frames["gtpu4_seq_ext"], tl_mode=2, k=5)
    return out


def stage_b():
    small = INNER[:10]
    return {
        "b_vxlan4": round_trip(0, INNER, ident=0x123456, cut=60),
        "b_vxlan6_vlan_any_port": round_trip(0, INNER, v6=True, vlan=True, port_mode=3,
                                             dst_port=8472, ident=0xFF000001, cut=40),
        "b_vxlan4_wrong_port": round_trip(0, INNER, port_mode=1, dst_port=8472),
        "b_vxlan4_short_inner": round_trip(0, small),
        "b_geneve4_options": round_trip(1, INNER, ident=0x42, p1=8, p2=(1 << 2) | 2,
                                        extra=b"\x01\x02\x80\x01\xde\xad\xbe\xef", cut=70),
        "b_geneve6_ipv4": round_trip(1, INNER_IP, v6=True, p1=0, p2=(2 << 2) | 1, cut=100),
        "b_geneve_bad_options": round_trip(1, INNER, p1=6),
        "b_geneve_too_many_options": round_trip(1, INNER, p1=0xFF),
        "b_geneve_max_options": round_trip(1, INNER, p1=252, p3=0x1234, extra=bytes(range(200))),
        "b_gre4_key_seq_csum": round_trip(2, INNER_IP, ident=0x1234, p1=7, p2=0x0800, p3=9,
                                          cut=80),
        "b_gre6_key": round_trip(2, INNER, v6=True, ident=0xFFFFFFFF, p1=1, p2=0x6558, cut=60),
        "b_gre4_empty": round_trip(2, b"", p1=4, p2=0x0800),
        "b_gtpu4_seq_ext": round_trip(3, INNER_IP, ident=0xDEADBEEF, p1=0x06, p2=5,
                                      p3=(0x85 << 16) | (2 << 8) | 0x05,
                                      extra=bytes(range(1, 30)), cut=70),
        "b_gtpu6_plain": round_trip(3, INNER_IP, v6=True, vlan=True, ident=1, cut=80),
        "b_gtpu4_npdu_only": round_trip(3, INNER_IP, p1=0x01, p3=0x7700002A),
        "b_gtpu4_echo": round_trip(3, b"", p1=0x18 | 0x02, p2=1),
        "b_gtpu4_empty": round_trip(3, b"", ident=3),
    }


# Inputs that once failed, kept so every corpus replay checks them.
REGRESSIONS = {
    # ubsan_misaligned_ipv4_checksum: found by this fuzzer before M21 (UBSan, utils/checksum.h);
    # kept as a seed so the corpus replay checks it stays clean.
    "r_ubsan_misaligned_ipv4_checksum": bytes.fromhex("01020000ffff0000000004000800000000"),
    # ubsan_misaligned_calculate_sum_odd_base: found by this fuzzer before M21 (UBSan, utils/checksum.h);
    # kept as a seed so the corpus replay checks it stays clean.
    "r_ubsan_misaligned_calculate_sum_odd_base": bytes.fromhex("00004496013333333333333333333333333333333333333333333333333333333333333333333333333333333333333318181818a0a0a0a0a0a0a0a0a0a00004a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a011b5a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a03da0a0a0a018181818181808000086dd58"),
}


def seeds():
    out = stage_a()
    out.update(stage_b())
    out.update(REGRESSIONS)
    return out
