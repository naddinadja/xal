"""
Tests for sharing an index across processes over POSIX shared memory.

test_shm_version_skew drives the ``xal_test_shm_version`` helper, which does the whole check in
one process: it opens a primary with ``opts.shm_name``, indexes, and attaches to its own published
regions with ``xal_from_shm()``: once as published, once with the version in the state region
edited, and once with it put back.

test_shm_primary_secondary runs the C integration tests, which open a primary on the mounted
device and attach secondaries with xal_from_shm(). test_server_publishes_index starts xal-server
on the same device and attaches to what it publishes, using the test binary's --attach mode.
"""

import errno
import json
import time
from pathlib import Path

import pytest
import yaml

SHM_NAME = "xal_test_shm_version"
SERVER_SHM_NAME = "xal_test_server"
NSAMPLES = 10


def test_shm_version_skew(cijoe):
    """A state region published at another version must be refused, not misread."""

    dev_path = cijoe.getconf("xal.dev_path", None)
    mountpoint = cijoe.getconf("xal.mountpoint", None)

    # The regions are created O_EXCL and outlive a primary that died without unlinking, so a
    # remnant of an earlier run would fail the open with -EEXIST rather than be reused.
    cijoe.run(f"rm -f /dev/shm/{SHM_NAME}_*")

    err, _ = cijoe.run(f"xal_test_shm_version {dev_path} {mountpoint}")
    assert not err


def _test_binary(cijoe) -> str:
    """Return the absolute path to the test_shm binary."""

    build_dir = cijoe.getconf("xal.build_dir", str(Path(__file__).parent.parent.parent / "build"))
    return str(Path(build_dir) / "tests" / "integration" / "test_shm")


def _server_log(cijoe) -> str:
    _, state = cijoe.run("sudo journalctl -t xal-server --no-pager -n 50")
    return state.output()


def test_shm_primary_secondary(cijoe, require_mount):
    dev_path = cijoe.getconf("xal.dev_path", None)

    err, state = cijoe.run(f"{_test_binary(cijoe)} {dev_path}")
    assert not err, state.output()


def test_server_publishes_index(cijoe, require_mount):
    """
    Start xal-server on the mounted device, compare the extents of a sample of files read
    through its published index to those reported by 'xfs_bmap', then stop it and check that
    the index is gone.
    """

    err, _ = cijoe.run("command -v xal-server")
    if err:
        pytest.skip("xal-server is not installed")

    dev_path = cijoe.getconf("xal.dev_path", None)
    artifacts_path = Path(cijoe.getconf("xal.artifacts.path"))
    conf_path = artifacts_path / "xal-server.conf"
    pid_path = artifacts_path / "xal-server.pid"
    out_path = artifacts_path / "xal_server_lookup.yaml"
    attach = f"sudo {_test_binary(cijoe)} --attach {SERVER_SHM_NAME}"

    # Watch mode 1 (dirty detection); without one the server freezes the filesystem
    conf_path.write_text(
        "log_level = 3\n"
        f'devices = [{{ uri = "{dev_path}", shm_name = "{SERVER_SHM_NAME}" }}]\n'
        "\n"
        "[xal]\n"
        "watchmode = 1\n"
    )

    bmap = json.loads((artifacts_path / "bmap.json").read_text())
    paths = sorted(bmap)
    assert paths, "bmap.json has no files"
    sample = paths[:: max(1, len(paths) // NSAMPLES)]

    cijoe.run(f"sudo rm -f /dev/shm/{SERVER_SHM_NAME}_*")

    err, state = cijoe.run(
        f"sudo sh -c 'nohup xal-server --config {conf_path} > /dev/null 2>&1 & "
        f"echo $! > {pid_path}'"
    )
    assert not err, state.output()
    pid = pid_path.read_text().strip()
    stopped = False

    try:
        # Attaching fails with EAGAIN, ESTALE or ENOENT until the first index is published
        for _ in range(120):
            err, _ = cijoe.run(attach)
            if not err:
                break
            time.sleep(0.5)
        else:
            raise AssertionError(f"xal-server never published; log:\n{_server_log(cijoe)}")

        quoted = " ".join(f"'{path}'" for path in sample)
        err, state = cijoe.run(f"{attach} {quoted} > {out_path}")
        assert not err, state.output()

        got = yaml.safe_load(out_path.read_text())
        diffs = []
        for path in sample:
            _, expected = bmap[path]
            if (got.get(path) or []) != (expected or []):
                diffs.append({"path": path, "expected": expected, "got": got.get(path)})
        assert not diffs

        cijoe.run(f"sudo kill {pid}")
        for _ in range(60):
            err, _ = cijoe.run(f"sudo kill -0 {pid}")
            if err:
                stopped = True
                break
            time.sleep(0.5)
        assert stopped, f"xal-server did not exit on SIGTERM; log:\n{_server_log(cijoe)}"

        err, _ = cijoe.run(attach)
        assert err == errno.ENOENT, f"attach after stop returned {err}, not ENOENT"

        err, _ = cijoe.run(f"ls /dev/shm/{SERVER_SHM_NAME}_*")
        assert err, "xal-server left shared memory behind"

    finally:
        if not stopped:
            cijoe.run(f"sudo kill -9 {pid}")
        cijoe.run(f"sudo rm -f /dev/shm/{SERVER_SHM_NAME}_*")
