"""
Regression test for paths longer than a single name.

The FIEMAP backend used to store the whole path in xal_inode.name, so indexing failed with
-ENAMETOOLONG once any path passed 255 bytes. This creates such a path on the mounted device,
with a leaf name of the maximum 255 bytes, and checks that the file is found by path and that its
extents match those reported by 'xfs_bmap'.
"""

import re
from pathlib import Path

import pytest
import yaml

DEPTH = 8
XFS_BMAP_RE = re.compile(r"^\d+:\s+\[(\d+)\.\.(\d+)\]:\s+(\d+)\.\.(\d+)$")


@pytest.fixture
def long_path(cijoe, require_mount):
    """Create a file at a path well past 255 bytes, and remove it afterwards."""

    top = Path(cijoe.getconf("xal.mountpoint", None)) / "long-path"
    path = top.joinpath(*[f"{i}-" + "d" * 58 for i in range(DEPTH)]) / ("f" * 255)
    assert len(str(path)) > 255

    err, state = cijoe.run(
        f"mkdir -p '{path.parent}' && "
        f"dd if=/dev/urandom of='{path}' bs=64K count=4 conv=fsync status=none"
    )
    assert not err, state.output()

    yield path

    cijoe.run(f"rm -rf '{top}'")


def _xfs_bmap(cijoe, path: Path, out_path: Path) -> list:
    err, state = cijoe.run(f"xfs_bmap '{path}' > {out_path}")
    assert not err, state.output()

    extents = []
    for line in out_path.read_text().splitlines():
        match = XFS_BMAP_RE.match(line.strip())
        if match:
            extents.append([int(val) for val in match.groups()])

    return extents


@pytest.mark.parametrize("lookup_arg", ["", "--file_lookup_map"], ids=["traverse", "hashmap"])
def test_long_path_lookup(cijoe, fiemap_cmd, long_path, lookup_arg):
    dev_path = cijoe.getconf("xal.dev_path", None)
    artifacts_path = Path(cijoe.getconf("xal.artifacts.path"))
    out_path = artifacts_path / "xal_long_path.yaml"

    expected = _xfs_bmap(cijoe, long_path, artifacts_path / "xfs_bmap_long_path.out")
    assert expected, "xfs_bmap reported no extents"

    err, state = cijoe.run(
        f"{fiemap_cmd} {lookup_arg} --filename '{long_path}' {dev_path} > {out_path}"
    )
    assert not err, state.output()

    (got,) = yaml.safe_load(out_path.read_text()).values()
    assert (got or []) == expected
