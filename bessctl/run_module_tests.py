#!/usr/bin/env python3

# Copyright (c) 2017, The Regents of the University of California.
# SPDX-License-Identifier: BSD-3-Clause

import argparse
import fnmatch
import glob
import os
import shlex
import subprocess
import sys
import unittest

this_dir = os.path.dirname(os.path.abspath(__file__))
bessctl_path = os.path.join(this_dir, 'bessctl')
bessctl = '%s %s' % (shlex.quote(sys.executable),
                      shlex.quote(bessctl_path))
default_test_dir = os.path.join(this_dir, 'module_tests')


class CommandError(subprocess.CalledProcessError):

    '''Identical to CalledProcessError, except it also shows the output'''

    def __str__(self):
        return '%s\n%s' % (super(CommandError, self).__str__(), self.output)


def run_cmd(cmd):
    args = shlex.split(cmd)
    try:
        ret = subprocess.check_call(args, stderr=subprocess.STDOUT)
    except subprocess.CalledProcessError as e:
        raise CommandError(e.returncode, e.cmd, e.output)


def main():

    arg_parser = argparse.ArgumentParser(
        description='Run per-module unit tests')
    arg_parser.add_argument('--test_name', type=str, default='*',
                            help='Name of a specific test to run.')
    arg_parser.add_argument('--test_dir', type=str, default=default_test_dir,
                            help='Path to the directory to serach for tests.')
    args = arg_parser.parse_args()

    any_failure = 0

    try:
        run_cmd('%s daemon start -m 0' % bessctl)
    except CommandError:
        raise Exception('bess daemon could not start')

    try:
        for file_name in glob.glob(
                os.path.join(args.test_dir, "{}.py".format(args.test_name))):
            print('Running test %s' % file_name)

            try:
                run_cmd('%s daemon reset -- run file %s' % (bessctl, file_name))
            except CommandError:
                any_failure = 1
                run_cmd('%s daemon start -m 0' % bessctl)
    finally:
        # The daemon runs as root (bessctl starts it through sudo); leaving it
        # behind after the suite outlives the test run and holds its memory.
        try:
            run_cmd('%s daemon stop' % bessctl)
        except CommandError:
            pass

    sys.exit(any_failure)


if __name__ == '__main__':
    main()
