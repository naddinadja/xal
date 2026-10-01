import pytest


def _is_mounted(cijoe) -> bool:
    dev_path = cijoe.getconf("xal.dev_path", None)
    mountpoint = cijoe.getconf("xal.mountpoint", None)

    err, _ = cijoe.run(f"findmnt --source {dev_path} --mountpoint {mountpoint} > /dev/null")
    return not err


@pytest.fixture(params=["xfs", "fiemap"])
def xal_cmd(request, cijoe) -> str:
    """
    The 'xal' command for the given backend. XFS reads the unmounted device, FIEMAP needs it
    mounted, so a backend is skipped when the device is not in the state it requires. FIEMAP
    runs under sudo as it freezes the filesystem.
    """

    backend = request.param
    mounted = _is_mounted(cijoe)

    if backend == "xfs" and mounted:
        pytest.skip("the xfs backend requires the device to be unmounted")
    if backend == "fiemap" and not mounted:
        pytest.skip("the fiemap backend requires the device to be mounted")

    return f"sudo xal --backend {backend}" if backend == "fiemap" else f"xal --backend {backend}"


@pytest.fixture
def fiemap_cmd(cijoe) -> str:
    """The 'xal' command for the FIEMAP backend; skipped when the device is not mounted."""

    if not _is_mounted(cijoe):
        pytest.skip("the fiemap backend requires the device to be mounted")

    return "sudo xal --backend fiemap"


@pytest.fixture
def require_mount(cijoe):
    """Skip the test when the device is not mounted."""

    if not _is_mounted(cijoe):
        pytest.skip("the test requires the device to be mounted")
