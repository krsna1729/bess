# SPDX-License-Identifier: BSD-3-Clause
"""Seeds for packet_cursor_fuzz.cc: chain layout + op script.

Layout: u8 flags (bits 0-3 segments-1, bits 4-5 pkt_len mode, bit 6 null
cursor check), u8 delta, u8 data seed, per segment (u16 length, u8 headroom),
then ops (u8 op [+ u8 size selector]).
"""

import struct

SKIP, PEEK, READ_BYTES, U8, U16, U32, U64, ODD, MIXED, WIDE, MARK, RESTORE, FAIL = range(13)


def op(kind, slot=0):
    for v in range(256):
        if v % 13 == kind and (v >> 4) % 4 == slot:
            return v
    raise ValueError((kind, slot))


# Size selectors (see NextSize in the harness).
def small(n):
    return n & 63


def to_remaining(d=0):
    return 0x40 | (d + 16)


def to_segment_end(d=0):
    return 0x80 | (d + 16)


def huge(which):
    return 0xC0 | which


def chain(segments, mode=0, delta=0, seed=0, null=False, headroom=0):
    flags = (len(segments) - 1) | (mode << 4) | (0x40 if null else 0)
    out = bytes([flags, delta, seed])
    for n in segments:
        out += struct.pack("<HB", n, headroom)
    return out


def script(*ops):
    out = b""
    for o in ops:
        if isinstance(o, tuple):
            out += bytes([op(o[0], o[1] if o[0] in (MARK, RESTORE) else 0)])
            if o[0] not in (MARK, RESTORE):
                out += bytes([o[1]])
        else:
            out += bytes([op(o)])
    return out


# Walks a typical header stack read by typed reads and peeks.
HEADERS = script((PEEK, small(14)), (SKIP, small(12)), U16, (MARK, 1), U8, U8,
                 U16, U32, U64, (RESTORE, 1), ODD, MIXED, WIDE,
                 (READ_BYTES, to_remaining()), FAIL)
# Crosses every boundary one segment at a time.
BOUNDARIES = script((PEEK, to_segment_end()), (PEEK, to_segment_end(1)),
                    (SKIP, to_segment_end(-1)), U16, (SKIP, to_segment_end(-3)),
                    U64, (SKIP, to_segment_end(-2)), U32, (SKIP, to_segment_end(-1)),
                    WIDE, MIXED, (READ_BYTES, to_segment_end(2)), ODD,
                    (READ_BYTES, to_remaining(1)), (SKIP, to_remaining(1)),
                    (SKIP, to_remaining()), U8, FAIL)
HUGE = script(*[(SKIP, huge(i)) for i in range(8)], *[(PEEK, huge(i)) for i in range(8)],
              (READ_BYTES, huge(3)), (READ_BYTES, huge(6)), (SKIP, small(1)), FAIL)
MARKS = script((MARK, 0), (SKIP, small(9)), (MARK, 2), (READ_BYTES, small(17)),
               (MARK, 3), (RESTORE, 2), U32, (RESTORE, 0), U64, (RESTORE, 3),
               (READ_BYTES, to_remaining()), (RESTORE, 1), (PEEK, small(1)))


def seeds():
    return {
        "linear_headers": chain([64]) + HEADERS,
        "eth_ipv4_udp_split": chain([14, 20, 8, 22]) + BOUNDARIES,
        "empty_head": chain([0, 20, 30]) + script((PEEK, small(1)), (PEEK, small(20)),
                                                  U16, (SKIP, small(0))) + BOUNDARIES,
        "zero_segments_between": chain([5, 0, 0, 7, 0, 1, 0, 13, 0]) + BOUNDARIES,
        "one_byte_segments": chain([1] * 16, seed=7) + BOUNDARIES,
        "all_empty": chain([0, 0, 0], null=True) + HUGE,
        "pkt_len_short": chain([10, 10, 10], mode=1, delta=7) + BOUNDARIES,
        "pkt_len_long": chain([10, 10, 10], mode=2, delta=9) + BOUNDARIES,
        "large_segments": chain([1023, 600, 1023], headroom=128, seed=3) + HUGE + BOUNDARIES,
        "marks": chain([3, 40, 2, 0, 31]) + MARKS,
    }
