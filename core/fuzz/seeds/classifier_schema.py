# SPDX-License-Identifier: BSD-3-Clause
"""Seeds for classifier_schema_fuzz.cc (input format: see the harness)."""

import _packets

PACKET, METADATA = 0, 1
HUGE_SOURCE, HUGE_KEY, HUGE_SIZE = 0x10, 0x20, 0x40
MASK, WRONG_MASK = 1 << 2, 2 << 2


def key(source, source_offset, key_offset, size, mask=None, flags=0):
    """A key field; `mask` is a bytes object of `size` bytes (or wrong size)."""
    ctl = source | flags
    out = bytes([0, source_offset, key_offset, size])
    if mask is not None:
        ctl |= MASK if len(mask) == size else WRONG_MASK
        out += mask
    return bytes([ctl]) + out[1:]


def result(value_offset, destination_offset, size, flags=0):
    return bytes([flags, value_offset, destination_offset, size])


def controls(packet_len=64, metadata_len=32, key_slack=1, batch=1,
             stride_slack=0, trims=(), value_slack=1, metadata_ctl=0x80,
             result_batch=1, result_trims=()):
    """The 64-byte control block. Slack 1 means exact size."""
    out = bytes([packet_len, metadata_len, key_slack, batch - 1, stride_slack])
    trims = list(trims) + [(0, 0)] * (batch - len(trims))
    for p, m in trims:
        out += bytes([p, m])
    out += bytes([value_slack, metadata_ctl, result_batch - 1])
    result_trims = list(result_trims) + [(0, 0)] * (result_batch - len(result_trims))
    for v, m in result_trims:
        out += bytes([v, m])
    assert len(out) <= 64
    return out + bytes(64 - len(out))


def encode(key_size, value_size, check, keys=(), results=(), ctl=None,
           content=None):
    out = bytes([key_size, value_size, 1 if check else 0, len(keys)])
    out += b"".join(keys)
    out += bytes([len(results)]) + b"".join(results)
    out += ctl if ctl is not None else controls()
    out += content if content is not None else _packets.eth_ipv4_tcp(b"payload")
    return out


def seeds():
    ipv6_udp = _packets.eth_ipv6_udp(b"hello world")
    s = {}
    # Valid schemas.
    s["ethertype"] = encode(2, 0, True, [key(PACKET, 12, 0, 2)])
    s["ipv4_5tuple"] = encode(
        13, 8, True,
        [key(PACKET, 26, 0, 4), key(PACKET, 30, 4, 4), key(PACKET, 23, 8, 1),
         key(PACKET, 34, 9, 4)],
        [result(0, 0, 2), result(2, 4, 4)],
        controls(batch=4, trims=[(0, 0), (0xC0, 0), (0xFF, 0), (0, 0)]))
    s["metadata_only"] = encode(
        8, 4, True, [key(METADATA, 0, 0, 4), key(METADATA, 8, 4, 4)],
        [result(0, 16, 4)])
    s["mixed_masked"] = encode(
        9, 6, True,
        [key(PACKET, 26, 0, 4, mask=b"\xff\xff\xff\x00"),
         key(METADATA, 2, 4, 4), key(PACKET, 23, 8, 1, mask=b"\x0f")],
        [result(0, 1, 3), result(3, 8, 3)],
        controls(batch=3, stride_slack=3, trims=[(0, 0xC0), (0, 0), (0xC0, 0)]))
    for width in (1, 2, 4, 8):
        s[f"masked_width_{width}"] = encode(
            width, 1, True,
            [key(PACKET, 14, 0, width, mask=bytes([0xF0 | i for i in range(width)]))],
            [result(0, 0, 1)])
        s[f"exact_width_{width}"] = encode(
            width, 0, True, [key(METADATA, 32 - width, 0, width)],
            ctl=controls(metadata_len=32, batch=2, trims=[(0, 0), (0, 0xC0)]))
    s["gapped_key"] = encode(
        16, 0, True, [key(PACKET, 0, 0, 4), key(PACKET, 6, 8, 4)],
        ctl=controls(batch=2, stride_slack=7))
    s["adjacent_coalesce"] = encode(
        12, 0, True,
        [key(PACKET, 0, 0, 6), key(PACKET, 6, 6, 6)])
    s["adjacent_masked_no_coalesce"] = encode(
        4, 0, True,
        [key(PACKET, 0, 0, 2), key(PACKET, 2, 2, 2, mask=b"\xff\x0f")])
    s["unsorted_three_ops"] = encode(
        12, 0, True,
        [key(METADATA, 4, 8, 4), key(PACKET, 30, 0, 4), key(PACKET, 0, 4, 4)],
        ctl=controls(batch=8, stride_slack=1,
                     trims=[(0xC0 + i, 0xC0 - i) for i in range(8)]))
    s["field_at_packet_end"] = encode(
        4, 0, True, [key(PACKET, 60, 0, 4)],
        ctl=controls(packet_len=64, batch=2, trims=[(0, 0), (0xC0, 0)]))
    s["short_key_span"] = encode(
        4, 0, True, [key(PACKET, 0, 0, 4)], ctl=controls(key_slack=0))
    s["assume_available"] = encode(
        8, 0, False, [key(PACKET, 100, 0, 4), key(METADATA, 60, 4, 4)],
        ctl=controls(packet_len=16, metadata_len=8, batch=3))
    s["no_key_fields"] = encode(4, 0, True)
    s["huge_source_offset"] = encode(
        4, 0, True, [key(PACKET, 3, 0, 4, flags=HUGE_SOURCE)])
    s["huge_destination"] = encode(
        1, 4, True, [key(PACKET, 0, 0, 1)],
        [result(0, 8, 4, flags=0x2)], ctl=controls(metadata_ctl=8))
    s["results_adjacent"] = encode(
        1, 12, True, [key(PACKET, 0, 0, 1)],
        [result(4, 4, 4), result(0, 0, 4), result(8, 8, 4)],
        ctl=controls(result_batch=3, result_trims=[(0, 0), (0xC0, 0), (0, 0xC0)]))
    s["result_metadata_short"] = encode(
        1, 8, True, [key(PACKET, 0, 0, 1)], [result(0, 0, 4), result(4, 40, 4)],
        ctl=controls(metadata_ctl=20))
    s["ipv6_ports"] = encode(
        36, 0, True,
        [key(PACKET, 22, 0, 16), key(PACKET, 38, 16, 16), key(PACKET, 54, 32, 4)],
        ctl=controls(packet_len=len(ipv6_udp), batch=2), content=ipv6_udp)
    # Invalid schemas.
    s["invalid_empty_key"] = encode(0, 0, True)
    s["invalid_zero_size"] = encode(4, 0, True, [key(PACKET, 0, 0, 0)])
    s["invalid_source"] = encode(4, 0, True, [key(2, 0, 0, 4)])
    s["invalid_key_overlap"] = encode(
        8, 0, True, [key(PACKET, 0, 0, 4), key(PACKET, 0, 3, 4)])
    s["invalid_key_out_of_range"] = encode(4, 0, True, [key(PACKET, 0, 1, 4)])
    s["invalid_mask_size"] = encode(  # mode 2 carries size + 1 mask bytes
        4, 0, True, [key(PACKET, 0, 0, 4, mask=b"\xff" * 5)])
    s["invalid_huge_key_offset"] = encode(
        4, 0, True, [key(PACKET, 0, 1, 4, flags=HUGE_KEY)])
    s["invalid_result_overlap"] = encode(
        1, 8, True, [key(PACKET, 0, 0, 1)], [result(0, 0, 4), result(4, 2, 4)])
    s["invalid_result_value_range"] = encode(
        1, 4, True, [key(PACKET, 0, 0, 1)], [result(2, 0, 4)])
    s["invalid_result_overflow"] = encode(
        1, 4, True, [key(PACKET, 0, 0, 1)], [result(0, 1, 4, flags=0x2)])
    return s
