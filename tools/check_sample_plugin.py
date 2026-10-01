#!/usr/bin/env python3
"""Load the sample plugin into a foreground BESS daemon and query its registry."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bessd', required=True)
    parser.add_argument('--plugin-dir', required=True)
    parser.add_argument('--grpc-url', default='127.0.0.1:10515')
    args = parser.parse_args()

    from pybess.bess import BESS

    with tempfile.TemporaryDirectory(prefix='bess-plugin-') as temporary:
        pidfile = Path(temporary) / 'bessd.pid'
        command = [
            args.bessd,
            '-f',
            '-skip_root_check',
            '-m', '0',
            '-i', str(pidfile),
            '-modules', args.plugin_dir,
            '-grpc_url', args.grpc_url,
        ]
        log_path = Path(temporary) / 'bessd.log'
        log = open(log_path, 'wb')
        process = subprocess.Popen(
            command,
            stdout=subprocess.DEVNULL,
            stderr=log,
        )
        client = BESS()
        try:
            deadline = time.monotonic() + 30
            last_error = None
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError(
                        f'bessd exited with {process.returncode}')
                try:
                    client.connect(args.grpc_url)
                    classes = client.list_mclasses()
                    if 'SequentialUpdate' not in classes.names:
                        raise RuntimeError(
                            'sample plugin loaded but SequentialUpdate is absent: '
                            + repr(list(classes.names)))
                    if 'IncompatibleProbe' in classes.names:
                        raise RuntimeError(
                            'a plugin declaring an unsupported BESS API range '
                            'was loaded and left IncompatibleProbe registered')
                    refusal = log_path.read_text(errors='replace')
                    if 'incompatible_probe' not in refusal or 'refused' not in refusal:
                        raise RuntimeError(
                            'the daemon did not log refusing incompatible_probe')
                    return 0
                except Exception as error:  # gRPC reports several transient errors
                    last_error = error
                    client.disconnect()
                    time.sleep(0.1)
            raise RuntimeError(f'timed out waiting for plugin registry: {last_error}')
        finally:
            client.disconnect()
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            if process.stdout is not None:
                process.stdout.close()


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
