# SPDX-License-Identifier: BSD-3-Clause
"""Seeds for route_prefix_fuzz: byte 0 even = string stage, odd = Make stage."""

import struct

STRINGS = {
    "default": "0.0.0.0/0",
    "host": "1.2.3.4/32",
    "net24": "192.168.1.0/24",
    "hostbits": "192.168.1.7/24",
    "len1": "128.0.0.0/1",
    "max": "255.255.255.255/32",
    "leading_zero": "010.001.000.009/08",
    "len33": "10.0.0.0/33",
    "octet256": "10.0.0.256/8",
    "no_len": "10.0.0.0/",
    "no_slash": "10.0.0.0",
    "three_octets": "10.0.0/8",
    "empty_part": "10..0.0/8",
    "space": " 10.0.0.0/8",
    "trailing": "10.0.0.0/8x",
    "neg_len": "10.0.0.0/-1",
    "plus": "+1.2.3.4/8",
    "five_octets": "1.2.3.4.5/8",
    "addr_only_trailing": "1.2.3.4.5",
    "long_len": "1.2.3.4/0000000000000000000024",
    "empty": "",
    # ParseIpv4Address used to wrap out-of-range parts (sscanf %u); found by
    # this harness (crash-a4e048a2...).
    "overflow_octet": "1.2.3.0000000000000000000000000000000000002017612633061982232",
    "overflow_first_prefix": "4294967296.0.0.0/8",
    "lenient_spaces": " 1.\t2.3.4 trailing",
}

MAKE = {
    "make_default": (0, 0),
    "make_host": (0x01020304, 32),
    "make_net": (0x0A000000, 8),
    "make_hostbits": (0x0A000001, 8),
    "make_len33": (0, 33),
    "make_len255": (0xFFFFFFFF, 255),
    "make_top": (0x80000000, 1),
}


def seeds():
    out = {f"str_{k}": b"\0" + v.encode() for k, v in STRINGS.items()}
    for k, (addr, length) in MAKE.items():
        out[k] = b"\1" + struct.pack("<IB", addr, length)
    return out
