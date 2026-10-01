import json
from pathlib import Path

import pytest
import yaml

NSAMPLES = 10


@pytest.mark.parametrize("lookup_arg", ["", "--file_lookup_map"], ids=["traverse", "hashmap"])
def test_lookup_compare_to_xfs_bmap(cijoe, fiemap_cmd, lookup_arg):
    """
    Look up a sample of files by path with 'xal --filename', using either tree traversal or the
    hash map, and compare the extents of the found inode to those reported by 'xfs_bmap'.
    """

    dev_path = cijoe.getconf("xal.dev_path", None)
    artifacts_path = Path(cijoe.getconf("xal.artifacts.path"))
    out_path = artifacts_path / "xal_lookup.yaml"

    bmap = json.loads((artifacts_path / "bmap.json").read_text())
    paths = sorted(bmap)
    assert paths, "bmap.json has no files"

    diffs = []
    for path in paths[:: max(1, len(paths) // NSAMPLES)]:
        _, expected = bmap[path]

        err, state = cijoe.run(
            f"{fiemap_cmd} {lookup_arg} --filename '{path}' {dev_path} > {out_path}"
        )
        assert not err, state.output()

        (got,) = yaml.safe_load(out_path.read_text()).values()
        if (got or []) != (expected or []):
            diffs.append({"path": path, "expected": expected, "got": got})

    assert not diffs


def test_lookup_after_reindex(cijoe, require_mount):
    """
    Run the C integration test for lookups across a re-index against the XFS mountpoint: it
    changes a directory tree between two calls to xal_index() and checks that lookups, by
    traversal and through the hash map, follow the change.
    """

    mountpoint = cijoe.getconf("xal.mountpoint", None)
    build_dir = cijoe.getconf("xal.build_dir", str(Path(__file__).parent.parent.parent / "build"))
    binary = Path(build_dir) / "tests" / "integration" / "test_lookup_map"

    err, state = cijoe.run(f"{binary} {mountpoint}")
    assert not err, state.output()
