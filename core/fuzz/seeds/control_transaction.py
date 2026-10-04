# SPDX-License-Identifier: BSD-3-Clause
"""Seeds for control_transaction_fuzz.

Input: u64 epoch, then records [u8 ctl][u16 length][ApplyTransactionRequest]
(ctl 0), or [u8 1] to apply the previous request again. Resources: "meters",
"actions" (may name meters, and next hops that never register), "rules"
(scope-snapshot capable), "hidden" (not bound), see the harness.
"""

import struct

from resource_codec import (action_key, action_value, any_, field_bin, field_int,
                            ld, meter_key, rule_key, rule_value, sr_tcm, tr_tcm, u)

EPOCH = struct.pack("<Q", 0x1234_5678_9ABC_DEF0)

REFERENTIAL = 1
SNAPSHOT = 2


def op(resource, key=None, value=None, erase=False):
    """A TransactionOp; key/value are (type name, payload) or None."""
    out = ld(1, resource)
    if erase:
        out += u(2, 1)
    if key is not None:
        out += ld(3, any_("bess.pb." + key[0], key[1]))
    if value is not None:
        out += ld(4, any_("bess.pb." + value[0], value[1]))
    return out


def meter(i, value=None):
    return op("meters", ("MeterIdKey", meter_key(i)), ("MeterPolicyValue", value or sr_tcm()))


def erase_meter(i):
    return op("meters", ("MeterIdKey", meter_key(i)), erase=True)


def action(i, meter_id=0, next_hop=0):
    return op("actions", ("ActionIdKey", action_key(i)),
              ("ActionValue", action_value(meter_id, next_hop)))


def erase_action(i):
    return op("actions", ("ActionIdKey", action_key(i)), erase=True)


def rule(a, b, gate):
    return op("rules", ("ExactMatchRuleKey", rule_key(field_int(a), field_int(b))),
              ("ExactMatchRuleValue", rule_value(gate=gate)))


def erase_rule(a, b):
    return op("rules", ("ExactMatchRuleKey", rule_key(field_int(a), field_int(b))), erase=True)


def request(ops=(), request_id=None, expected=None, consistency=None):
    out = b""
    if request_id is not None:
        out += ld(1, request_id)
    if expected is not None:
        out += u(2, expected)  # explicit presence, even for 0
    out += b"".join(ld(3, o) for o in ops)
    if consistency:
        out += u(4, consistency)
    return out


def rec(req):
    return b"\x00" + struct.pack("<H", len(req)) + req


REPEAT = b"\x01"


def seq(*records):
    return EPOCH + b"".join(records)


def seeds():
    dependent = request([action(5, meter_id=7), meter(7)], request_id="t1")
    other = request([meter(8)], request_id="t1")
    return {
        "empty": EPOCH,
        "empty_request": seq(rec(b"")),
        "meter_insert": seq(rec(request([meter(1)]))),
        "meter_update_erase": seq(rec(request([meter(1)])),
                                  rec(request([meter(1, tr_tcm())])),
                                  rec(request([erase_meter(1)]))),
        "dependent_set": seq(rec(dependent)),
        "replay": seq(rec(dependent), REPEAT),
        "id_reused": seq(rec(dependent), rec(other)),
        "erase_referenced": seq(rec(request([meter(7), action(5, meter_id=7)])),
                                rec(request([erase_meter(7)])),
                                rec(request([erase_meter(7), erase_action(5)]))),
        "repoint_and_erase": seq(rec(request([meter(7), meter(8), action(5, meter_id=7)])),
                                 rec(request([action(5, meter_id=8), erase_meter(7)]))),
        "missing_reference": seq(rec(request([action(5, meter_id=9)], request_id="m"))),
        "undeclared_reference": seq(rec(request([meter(1), action(2, next_hop=3)]))),
        "duplicate_key": seq(rec(request([meter(1), meter(2), meter(1, tr_tcm())]))),
        "erase_missing": seq(rec(request([meter(1), erase_meter(3)], request_id="e"))),
        "erase_with_value": seq(rec(request([op("meters", ("MeterIdKey", meter_key(1)),
                                                ("MeterPolicyValue", sr_tcm()), erase=True)]))),
        "wrong_key_type": seq(rec(request([op("meters", ("ActionIdKey", action_key(1)),
                                              ("MeterPolicyValue", sr_tcm()))]))),
        "missing_value": seq(rec(request([meter(1), op("meters", ("MeterIdKey", meter_key(2)))]))),
        "meter_id_zero": seq(rec(request([meter(0)]))),
        "no_profile": seq(rec(request([meter(1, b"")]))),
        "bad_url": seq(rec(request([ld(1, "meters") + ld(3, ld(1, "bess.pb.MeterIdKey") +
                                                         ld(2, meter_key(1)))]))),
        "truncated_key": seq(rec(request([op("meters", ("MeterIdKey", b"\x08"),
                                             ("MeterPolicyValue", sr_tcm()))]))),
        "unknown_resource": seq(rec(request([op("next_hops", ("MeterIdKey", meter_key(1)))]))),
        "hidden_resource": seq(rec(request([op("hidden", ("MeterIdKey", meter_key(1)),
                                               ("MeterPolicyValue", sr_tcm()))]))),
        "expected_generation": seq(rec(request([meter(1)], expected=0)),
                                   rec(request([meter(2)], request_id="g", expected=5)),
                                   REPEAT,
                                   rec(request([meter(2)], request_id="g", expected=1))),
        "scope_snapshot": seq(rec(request([rule(1, 80, 3), rule(2, 443, 8192)],
                                          consistency=SNAPSHOT)),
                              rec(request([erase_rule(1, 80)], consistency=SNAPSHOT))),
        "scope_unsupported": seq(rec(request([rule(1, 80, 3), meter(1)], request_id="s",
                                             consistency=SNAPSHOT)), REPEAT),
        "referential_explicit": seq(rec(request([rule(1, 80, 3)], consistency=REFERENTIAL))),
        "unknown_consistency": seq(rec(request([meter(1)], request_id="c", consistency=7))),
        "rule_bad_gate": seq(rec(request([rule(1, 80, 9000)]))),
        "rule_bin_fields": seq(rec(request([op("rules", ("ExactMatchRuleKey",
                                                         rule_key(field_bin(b"\x0a\x00\x00\x01"),
                                                                  field_bin(b"\x00\x50"))),
                                               ("ExactMatchRuleValue", rule_value(gate=1)))]))),
        "rule_erase_missing": seq(rec(request([erase_rule(9, 9)]))),
        "mixed": seq(rec(request([meter(1), meter(2), action(1, meter_id=1),
                                  action(2, meter_id=2), action(3), rule(1, 1, 1)],
                                 request_id="a")),
                     rec(request([erase_action(1), erase_meter(1), action(2, meter_id=1),
                                  meter(1)], request_id="b")),
                     rec(request([erase_action(1), erase_action(2), erase_action(3),
                                  erase_meter(1), erase_meter(2), erase_rule(1, 1)],
                                 request_id="c")),
                     REPEAT),
        "unparsable": seq(rec(b"\x0a\x05ab"), rec(request([meter(1)]))),
    }
