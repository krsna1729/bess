# SPDX-License-Identifier: BSD-3-Clause
"""Seeds for packet_mutation_fuzz.cc: chain layout + op script.

Layout: u8 flags (bits 0-2 segments-1, bit 3 pkt_len override, bits 4-7 data
seed), [u32 pkt_len], per segment (u16 data length, u16 headroom, u16
tailroom, u8 storage: bits 0-1 direct/external/indirect/external-no-shinfo,
bit 2 shared), then ops (u8 op + arguments, see the harness).
"""

import struct

import _packets

PREPEND, APPEND, REMOVE, TRIM, FITS, NULL, PLAN, TOGGLE = range(8)
DIRECT, EXTERNAL, INDIRECT, EXTERNAL_NO_INFO = range(4)
SHARED = 4


# Size selectors (NextSize in the harness).
def small(n):
    return bytes([n])


def _rel(kind, d):
    return bytes([(kind << 5) | (d + 16)])


def headroom(d=0):
    return _rel(1, d)


def tailroom(d=0):
    return _rel(2, d)


def head_len(d=0):
    return _rel(3, d)


def last_len(d=0):
    return _rel(4, d)


def pkt_len(d=0):
    return _rel(5, d)


def special(k):
    return bytes([0xC0 | k])


def u64(v):
    return bytes([0xC7]) + struct.pack("<Q", v)


def u16(v):
    return bytes([0xE0]) + struct.pack("<H", v)


def chain(segments, override=None, seed=0):
    """segments: (data length, headroom, tailroom, storage) tuples."""
    flags = (len(segments) - 1) | (8 if override is not None else 0) | (seed << 4)
    out = bytes([flags])
    if override is not None:
        out += struct.pack("<I", override)
    for length, head, tail, storage in segments:
        out += struct.pack("<HHHB", length, head, tail, storage)
    return out


def ok(op, size, pattern=0x11):
    """A prepend/append expected to succeed (the harness then reads a pattern seed)."""
    return bytes([op]) + size + bytes([pattern])


def fail(op, size):
    return bytes([op]) + size


def fits(size, scratch_len):
    return bytes([FITS]) + size + struct.pack("<I", scratch_len)


# Edit plan calls.
def off(v):
    return bytes([v]) if v < 0xFF else b"\xff" + struct.pack("<H", v)


def remove_prefix(n):
    return bytes([0]) + off(n)


def prepend(header):
    return bytes([1, len(header)]) + header


def write(at, data):
    return bytes([2]) + off(at) + bytes([len(data)]) + data


def copy(to, frm, n):
    return bytes([3]) + off(to) + off(frm) + (bytes([n]) if n < 0xFF else b"\xff" + struct.pack("<H", n))


def adjust(at, old, new):
    assert len(old) == len(new) and len(old) < 17
    return bytes([4]) + off(at) + bytes([len(old)]) + old + new


def set_length(at, base):
    return bytes([5]) + off(at) + off(base)


def ipv4_checksum(at):
    return bytes([6]) + off(at)


def plan(*calls, mode=0):
    return bytes([PLAN, mode, len(calls)]) + b"".join(calls)


def vxlan_outer():
    inner = _packets.eth(b"\0" * 46)
    full = _packets.eth(_packets.ipv4(
        _packets.udp(b"\x08\0\0\0\0\0\x01\0" + inner, 49152, 4789), 17))
    return full[:50]


def seeds():
    simple = [(64, 128, 128, DIRECT)]
    three = [(20, 64, 64, DIRECT), (30, 64, 64, DIRECT), (40, 64, 64, DIRECT)]
    nat_old_ip, nat_new_ip = bytes([10, 0, 0, 1]), bytes([192, 168, 1, 1])
    nat_old_port, nat_new_port = b"\x30\x39", b"\xc0\x00"
    return {
        "linear_edges": chain(simple)
        + ok(PREPEND, small(14)) + ok(APPEND, small(4)) + fail(REMOVE, small(14))
        + fail(TRIM, small(4)) + ok(PREPEND, headroom()) + fail(PREPEND, small(1))
        + ok(APPEND, tailroom()) + fail(APPEND, small(1)) + fail(REMOVE, head_len())
        + fail(REMOVE, small(1)) + fail(TRIM, small(1)) + ok(PREPEND, small(3))
        + fail(TRIM, last_len()),
        "chain_crossing": chain(three)
        + fail(REMOVE, head_len(1)) + fail(TRIM, last_len(1)) + fail(REMOVE, head_len())
        + fail(TRIM, last_len()) + ok(PREPEND, headroom(-1)) + ok(APPEND, tailroom(-2))
        + fail(REMOVE, pkt_len()) + fail(TRIM, pkt_len(1)) + bytes([NULL]),
        # Found by the fuzzer: Build accepted an IPv4 header checksum over a
        # word a length or checksum step changes per packet (now kOutOfRange).
        "plan_ipv4_word_set_per_packet": chain([(64, 128, 64, DIRECT)])
        + plan(prepend(vxlan_outer()), set_length(18, 14), ipv4_checksum(14))
        + plan(prepend(vxlan_outer()), adjust(26, b"\x0a\0\0\x01", b"\x0a\0\0\x02"),
               ipv4_checksum(14))
        + plan(prepend(vxlan_outer()), set_length(16, 14), set_length(38, 34),
               adjust(24, b"\x0a\0\0\x01", b"\x0a\0\0\x02"), ipv4_checksum(14)),
        # Found by the fuzzer: removals summing past 16 bits wrapped (now
        # kOutOfRange); the exact 0xFFFF total still builds.
        "plan_remove_overflow": chain([(64, 0, 0, DIRECT)])
        + plan(remove_prefix(182), remove_prefix(182), remove_prefix(182),
               remove_prefix(0xFFFF))
        + plan(remove_prefix(0x8000), remove_prefix(0x7FFF)),
        # Prefix growth equal to the headroom fits; one byte more does not.
        "plan_exact_headroom": chain([(64, 8, 0, DIRECT)])
        + plan(remove_prefix(7), prepend(bytes(range(16))))
        + plan(remove_prefix(6), prepend(bytes(range(14)))),
        "zero_rooms": chain([(10, 0, 0, DIRECT), (0, 0, 0, DIRECT)])
        + fail(PREPEND, small(1)) + fail(APPEND, small(1)) + fail(TRIM, small(1))
        + fail(REMOVE, small(10)),
        "shared_storage": chain([(16, 32, 32, DIRECT | SHARED), (16, 32, 32, EXTERNAL | SHARED)])
        + fail(PREPEND, small(4)) + fail(APPEND, small(4)) + fail(REMOVE, small(4))
        + fail(TRIM, small(4)) + bytes([TOGGLE, 0]) + ok(PREPEND, small(4))
        + bytes([TOGGLE, 1]) + ok(APPEND, small(4)),
        "indirect_and_no_shinfo": chain([(16, 32, 32, INDIRECT | SHARED), (8, 8, 8, INDIRECT),
                                         (16, 32, 32, EXTERNAL_NO_INFO)])
        + fail(PREPEND, small(2)) + fail(APPEND, small(2)) + bytes([TOGGLE, 0])
        + ok(PREPEND, small(2)) + fail(TRIM, small(2)),
        "large_rooms": chain([(100, 60000, 5000, DIRECT)], seed=5)
        + fail(PREPEND, special(0)) + fail(PREPEND, special(1)) + ok(PREPEND, headroom())
        + fail(REMOVE, special(1)) + ok(APPEND, tailroom()) + fail(TRIM, special(0))
        + fits(special(0), 0) + fits(special(1), 0xFFFF0000),
        "pkt_len_near_max": chain([(64, 128, 128, DIRECT)], override=0xFFFFFFF0)
        + fail(PREPEND, small(16)) + ok(PREPEND, special(3)) + fail(APPEND, small(1))
        + fits(special(3), 0xFFFFFFFF) + fits(special(4), 0) + fail(TRIM, small(4)),
        "pkt_len_short": chain([(64, 16, 16, DIRECT), (64, 16, 16, DIRECT)], override=10)
        + fail(REMOVE, small(11)) + fail(REMOVE, small(10)) + fail(TRIM, small(1)),
        "huge_sizes": chain(three)
        + b"".join(fail(op, s) for op in (PREPEND, APPEND, REMOVE, TRIM)
                   for s in (special(2), special(5), u64(1 << 40), u16(0xFFFF)))
        + fits(special(5), 7) + fits(special(6), 0),
        "plan_vxlan_encap": chain([(64, 128, 64, DIRECT)])
        + plan(prepend(vxlan_outer()), set_length(16, 14), set_length(38, 34),
               ipv4_checksum(14)),
        "plan_vxlan_decap": chain([(120, 64, 0, DIRECT), (40, 0, 0, DIRECT)])
        + plan(remove_prefix(50), mode=1) + plan(remove_prefix(50), mode=2)
        + plan(remove_prefix(50)),
        "plan_gtp_decap_eth": chain([(100, 0, 0, DIRECT)])
        + plan(remove_prefix(36), prepend(bytes(range(14)))),
        "plan_nat": chain([(80, 16, 16, DIRECT)], seed=3)
        + plan(write(26, nat_new_ip), write(34, nat_new_port),
               adjust(24, nat_old_ip, nat_new_ip), adjust(50, nat_old_ip, nat_new_ip),
               adjust(50, nat_old_port, nat_new_port), copy(0, 6, 6), mode=2),
        "plan_needs_reshape": chain([(20, 64, 0, DIRECT | SHARED), (60, 0, 0, DIRECT)])
        + plan(write(30, b"\x01\x02")) + plan(prepend(b"\xaa" * 8))
        + bytes([TOGGLE, 0]) + plan(remove_prefix(30)) + plan(prepend(b"\xbb" * 65)),
        "plan_build_limits": chain([(64, 64, 64, DIRECT)])
        + plan(*[write(i * 8, b"\x01") for i in range(13)])
        + plan(*[write(i * 50, b"\x02" * 48) for i in range(4)])
        + plan(write(0xFFF0, b"\x03" * 15)) + plan(write(0xFFF0, b"\x03" * 16))
        + plan(copy(0, 1, 0)) + plan(copy(0, 1, 255)) + plan(copy(0, 1, 256))
        + plan(ipv4_checksum(0)) + plan(adjust(0, b"\x01", b"\x02")),
    }
