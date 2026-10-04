# SPDX-License-Identifier: BSD-3-Clause
"""Seeds for resource_codec_fuzz: [selector][u16 url length][url][message].

selector bits 0-1: 0 actions, 1 meters, 2 exact-match rules, 3 routes;
bit 2: 0 key, 1 value. Messages are hand-encoded protobuf wire format (the
encoder below is shared with control_transaction.py).
"""

import struct

PREFIX = "type.googleapis.com/"


# -- protobuf wire format -----------------------------------------------------

def varint(v):
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def u(field, v):
    """A varint field."""
    return varint(field << 3) + varint(v)


def ld(field, payload):
    """A length-delimited field (bytes, string, message)."""
    if isinstance(payload, str):
        payload = payload.encode()
    return varint((field << 3) | 2) + varint(len(payload)) + payload


def any_(type_name, payload, prefix=PREFIX):
    """A google.protobuf.Any packing `payload` as `type_name`."""
    return ld(1, prefix + type_name) + ld(2, payload)


# -- module messages (protobuf/module_msg.proto, util_msg.proto) --------------

def action_key(i):
    return u(1, i) if i else b""


def action_value(meter=0, next_hop=0):
    return (u(1, meter) if meter else b"") + (u(2, next_hop) if next_hop else b"")


def meter_key(i):
    return u(1, i) if i else b""


def sr_tcm(cir=1000, cbs=100, ebs=200):
    return ld(1, u(1, cir) + u(2, cbs) + u(3, ebs))


def tr_tcm(cir=1000, cbs=100, pir=2000, pbs=200):
    return ld(2, u(1, cir) + u(2, cbs) + u(3, pir) + u(4, pbs))


def rfc4115(cir=1000, cbs=100, eir=500, ebs=50):
    return ld(3, u(1, cir) + u(2, cbs) + u(3, eir) + u(4, ebs))


def field_int(v):
    return u(2, v)


def field_bin(b):
    return ld(1, b)


def rule_key(*fields):
    return b"".join(ld(1, f) for f in fields)


def rule_value(gate=0, action_id=0):
    return (u(1, gate) if gate else b"") + (u(2, action_id) if action_id else b"")


def route_key(ipv4, length, domain=0):
    return (ld(1, ipv4) if ipv4 else b"") + (u(2, length) if length else b"") + \
        (u(3, domain) if domain else b"")


def route_value(next_hop):
    return u(1, next_hop) if next_hop else b""


CODECS = [
    ("ActionIdKey", "ActionValue"),
    ("MeterIdKey", "MeterPolicyValue"),
    ("ExactMatchRuleKey", "ExactMatchRuleValue"),
    ("RouterRouteKey", "RouterRouteValue"),
]


def seed(codec, value_side, url, message):
    if isinstance(url, str):
        url = url.encode()
    return bytes([codec | (4 if value_side else 0)]) + struct.pack("<H", len(url)) + url + message


def ok(codec, value_side, message):
    name = CODECS[codec][1 if value_side else 0]
    return seed(codec, value_side, PREFIX + "bess.pb." + name, message)


def seeds():
    good_rule = rule_key(field_int(0x0A000001), field_bin(b"\x00\x50"))
    s = {
        # Accepted, each codec and side.
        "action_key": ok(0, False, action_key(5)),
        "action_key_zero": ok(0, False, action_key(0)),
        "action_value": ok(0, True, action_value(7, 9)),
        "action_value_empty": ok(0, True, b""),
        "meter_key": ok(1, False, meter_key(3)),
        "meter_srtcm": ok(1, True, sr_tcm()),
        "meter_trtcm": ok(1, True, tr_tcm()),
        "meter_rfc4115": ok(1, True, rfc4115()),
        "meter_no_profile": ok(1, True, b""),
        "meter_two_profiles": ok(1, True, sr_tcm() + tr_tcm()),
        "rule_key": ok(2, False, good_rule),
        "rule_key_bin_wrong_size": ok(2, False, rule_key(field_bin(b"\x01"), field_int(80))),
        "rule_key_one_field": ok(2, False, rule_key(field_int(1))),
        "rule_key_unset_field": ok(2, False, rule_key(b"", b"")),
        "rule_value": ok(2, True, rule_value(gate=3)),
        "rule_value_drop": ok(2, True, rule_value(gate=8192)),
        "rule_value_bad_gate": ok(2, True, rule_value(gate=8193)),
        "rule_value_action": ok(2, True, rule_value(action_id=4)),
        "route_key": ok(3, False, route_key("10.0.0.0", 8)),
        "route_key_domain": ok(3, False, route_key("192.168.1.0", 24, 3)),
        "route_key_bad_addr": ok(3, False, route_key("10.0.0", 8)),
        "route_key_len33": ok(3, False, route_key("10.0.0.0", 33)),
        "route_key_bad_utf8": ok(3, False, ld(1, b"\xff\xfe")),
        "route_value": ok(3, True, route_value(4)),
        "route_value_zero": ok(3, True, b""),
        # Unknown fields are kept and re-serialized.
        "action_key_unknown_field": ok(0, False, action_key(5) + u(15, 1) + ld(16, b"x")),
        # type_url: the rule is "the text after the last '/'".
        "url_slash_first": seed(0, False, "/bess.pb.ActionIdKey", action_key(5)),
        "url_two_slashes": seed(0, False, "a/b/bess.pb.ActionIdKey", action_key(5)),
        "url_no_slash": seed(0, False, "bess.pb.ActionIdKey", action_key(5)),
        "url_trailing_slash": seed(0, False, PREFIX + "bess.pb.ActionIdKey/", action_key(5)),
        "url_longer_name": seed(0, False, PREFIX + "bess.pb.ActionIdKeyX", action_key(5)),
        "url_prefix_name": seed(0, False, PREFIX + "bess.pb.ActionIdKe", action_key(5)),
        "url_other_type": seed(1, False, PREFIX + "bess.pb.ActionIdKey", meter_key(5)),
        "url_value_for_key": seed(1, False, PREFIX + "bess.pb.MeterPolicyValue", sr_tcm()),
        "url_empty": seed(0, True, "", action_value(1)),
        "url_only_slash": seed(0, True, "/", b""),
        # Truncated / malformed messages.
        "trunc_varint": ok(0, True, b"\x08"),
        "trunc_length": ok(1, True, b"\x0a\x05\x08\x01"),
        "trunc_nested": ok(2, False, rule_key(field_int(1))[:-1]),
        "bad_wire_type": ok(0, False, b"\x0f\x01"),
        "field_zero": ok(3, True, b"\x00\x01"),
        "empty": b"",
    }
    return s
