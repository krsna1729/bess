# Copyright (c) 2017, The Regents of the University of California.
# SPDX-License-Identifier: BSD-3-Clause

# What the conf/perftest scripts use. The module tests are C++ now
# (core/testing/module_harness.h, tests/live/).

import sys
import time


def get_root_tc(bess):
    # get tc information
    tcs = bess.list_tcs().classes_status
    root_tc = None
    for tc in tcs:
        if not getattr(tc, 'parent'):
            root_tc = getattr(tc, 'class')
            break

    return root_tc


def measure_tc_perf(bess, duration):
    root_tc = get_root_tc(bess)
    if not root_tc:
        raise Exception('Fail to find root tc')

    old = bess.get_tc_stats(root_tc.name)
    time.sleep(duration)
    new = bess.get_tc_stats(root_tc.name)

    sec_diff = new.timestamp - old.timestamp
    pps = (new.packets - old.packets) / sec_diff
    cps = (new.count - old.count) / sec_diff
    sys.stdout.write("pps: %12.f\tcounts/s: %12.f\n" % (pps, cps))
