# SPDX-License-Identifier: BSD-3-Clause
"""Packet builders shared by the seed generators (valid checksums)."""

import struct


def le16(v):
    return struct.pack("<H", v & 0xFFFF)


def le32(v):
    return struct.pack("<I", v & 0xFFFFFFFF)


def csum(data):
    if len(data) % 2:
        data += b"\0"
    s = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def ip4(a, b, c, d):
    return bytes([a, b, c, d])


def eth(payload, ethertype=0x0800, dst=b"\x02\0\0\0\0\x02", src=b"\x02\0\0\0\0\x01",
        vlan=None):
    hdr = dst + src
    if vlan is not None:
        hdr += struct.pack("!HH", 0x8100, vlan)
    return hdr + struct.pack("!H", ethertype) + payload


def ipv4(payload, proto, src=ip4(10, 0, 0, 1), dst=ip4(10, 0, 0, 2), ttl=64,
         ident=1, flags_frag=0x4000, options=b""):
    ihl = 5 + len(options) // 4
    hdr = struct.pack("!BBHHHBBH4s4s", 0x40 | ihl, 0, 20 + len(options) + len(payload),
                      ident, flags_frag, ttl, proto, 0, src, dst) + options
    hdr = hdr[:10] + struct.pack("!H", csum(hdr)) + hdr[12:]
    return hdr + payload


def ipv6(payload, nh, src=b"\x20\x01" + b"\0" * 13 + b"\x01",
         dst=b"\x20\x01" + b"\0" * 13 + b"\x02", hlim=64):
    return struct.pack("!IHBB16s16s", 0x60000000, len(payload), nh, hlim, src, dst) + payload


def _pseudo4(src, dst, proto, length):
    return src + dst + struct.pack("!BBH", 0, proto, length)


def _pseudo6(src, dst, nh, length):
    return src + dst + struct.pack("!IxxxB", length, nh)


def udp(payload, sport=1234, dport=80, src=ip4(10, 0, 0, 1), dst=ip4(10, 0, 0, 2)):
    """UDP datagram with a checksum over the given L3 addresses (4 or 16 bytes)."""
    length = 8 + len(payload)
    seg = struct.pack("!HHHH", sport, dport, length, 0) + payload
    pseudo = (_pseudo4 if len(src) == 4 else _pseudo6)(src, dst, 17, length)
    c = csum(pseudo + seg) or 0xFFFF
    return seg[:6] + struct.pack("!H", c) + seg[8:]


def tcp(payload, sport=1234, dport=80, seq=1, ack=0, flags=0x02, window=65535,
        src=ip4(10, 0, 0, 1), dst=ip4(10, 0, 0, 2), options=b""):
    off = (5 + len(options) // 4) << 4
    seg = struct.pack("!HHIIBBHHH", sport, dport, seq, ack, off, flags, window, 0, 0) + options + payload
    pseudo = (_pseudo4 if len(src) == 4 else _pseudo6)(src, dst, 6, len(seg))
    c = csum(pseudo + seg)
    return seg[:16] + struct.pack("!H", c) + seg[18:]


def icmp4(payload, type_=8, code=0, ident=7, seq=1):
    msg = struct.pack("!BBHHH", type_, code, 0, ident, seq) + payload
    return msg[:2] + struct.pack("!H", csum(msg)) + msg[4:]


def icmp6(payload, type_=128, code=0, ident=7, seq=1, src=None, dst=None):
    src = src or b"\x20\x01" + b"\0" * 13 + b"\x01"
    dst = dst or b"\x20\x01" + b"\0" * 13 + b"\x02"
    msg = struct.pack("!BBHHH", type_, code, 0, ident, seq) + payload
    c = csum(_pseudo6(src, dst, 58, len(msg)) + msg)
    return msg[:2] + struct.pack("!H", c) + msg[4:]


def eth_ipv4_udp(payload=b"hello", sport=1234, dport=80, src=ip4(10, 0, 0, 1),
                 dst=ip4(10, 0, 0, 2)):
    return eth(ipv4(udp(payload, sport, dport, src, dst), 17, src, dst))


def eth_ipv4_tcp(payload=b"", sport=1234, dport=80, flags=0x02, src=ip4(10, 0, 0, 1),
                 dst=ip4(10, 0, 0, 2), seq=1, ack=0):
    return eth(ipv4(tcp(payload, sport, dport, seq, ack, flags, src=src, dst=dst), 6, src, dst))


def eth_ipv4_icmp(payload=b"ping", src=ip4(10, 0, 0, 1), dst=ip4(10, 0, 0, 2), type_=8):
    return eth(ipv4(icmp4(payload, type_), 1, src, dst))


def eth_ipv6_udp(payload=b"hello", sport=1234, dport=80):
    src = b"\x20\x01" + b"\0" * 13 + b"\x01"
    dst = b"\x20\x01" + b"\0" * 13 + b"\x02"
    return eth(ipv6(udp(payload, sport, dport, src, dst), 17, src, dst), 0x86DD)


def eth_ipv6_tcp(payload=b"", sport=1234, dport=80, flags=0x02):
    src = b"\x20\x01" + b"\0" * 13 + b"\x01"
    dst = b"\x20\x01" + b"\0" * 13 + b"\x02"
    return eth(ipv6(tcp(payload, sport, dport, flags=flags, src=src, dst=dst), 6, src, dst), 0x86DD)
