import logging as log
from pathlib import Path


def _test_binary(cijoe) -> str:
    """Return the absolute path to the test_inotify binary."""

    build_dir = cijoe.getconf("xal.build_dir", str(Path(__file__).parent.parent.parent / "build"))
    return str(Path(build_dir) / "tests" / "integration" / "test_inotify")


def test_inotify_on_tmpfs(cijoe):
    """
    Run the C integration test binary without arguments.

    The binary uses mkdtemp to create a temporary directory under /tmp
    (tmpfs). This verifies dirty detection works on any filesystem and
    does not require a mounted XFS device.
    """

    binary = _test_binary(cijoe)

    err, state = cijoe.run(binary)
    assert not err, state.output()


def test_inotify_on_xfs(cijoe):
    """
    Run the C integration test binary against the XFS mountpoint.

    The binary creates a temporary subdirectory under the XFS mountpoint,
    watches it with xal_be_fiemap_open(), then triggers inotify events and
    asserts that the dirty flag is set. This exercises the full inotify
    path on a real XFS filesystem.

    Requires: the XFS device is mounted (run prep_inotify step first).
    """

    binary = _test_binary(cijoe)
    mountpoint = cijoe.getconf("xal.mountpoint", None)
    assert mountpoint, "xal.mountpoint must be set in the config"

    err, state = cijoe.run(f"{binary} {mountpoint}")
    assert not err, state.output()
