#!/usr/bin/env python3
"""Load the sample plugin into a foreground BESS daemon and query its registry."""

from __future__ import annotations

import argparse
import errno
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def check_unload_lifetime(client) -> None:
    """Roadmap 28.4 (D-090): a plugin with a live module is not unloaded --
    the refusal names the module -- and is once the module is gone, taking
    its class with it."""
    from pybess.bess import BESS
    from builtin_pb import bess_msg_pb2 as bess_msg
    import supdate_msg_pb2
    plugin = next(p for p in client.list_plugins().paths if 'sequential_update' in p)
    request = bess_msg.CreateModuleRequest(name='su0', mclass='SequentialUpdate')
    request.arg.Pack(supdate_msg_pb2.SequentialUpdateArg())
    client._request('CreateModule', request)
    try:
        client.unload_plugin(plugin)
    except BESS.Error as error:
        if error.code != errno.EBUSY or 'module su0' not in error.errmsg:
            raise RuntimeError(f'unexpected refusal: {error}')
    else:
        raise RuntimeError('a plugin with a live module was unloaded')
    if 'SequentialUpdate' not in client.list_mclasses().names:
        raise RuntimeError('a refused unload removed the class')
    client.destroy_module('su0')
    client.unload_plugin(plugin)
    if 'SequentialUpdate' in client.list_mclasses().names:
        raise RuntimeError('the unloaded plugin left its class registered')
    if plugin in client.list_plugins().paths:
        raise RuntimeError('the unloaded plugin is still listed')


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
                    break
                except Exception as error:  # gRPC reports several transient errors
                    last_error = error
                    client.disconnect()
                    time.sleep(0.1)
            else:
                raise RuntimeError(f'timed out waiting for plugin registry: {last_error}')
            check_unload_lifetime(client)
            return 0
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
