#!/usr/bin/env python3
"""
Mount an XFS device for inotify testing
========================================

Mounts the block device at the configured mountpoint and leaves it mounted.
Unlike prep_files, this script does NOT unmount after setup, because the
inotify tests require a live mounted filesystem.

Run a separate cleanup step (sudo umount) or re-run prep_loop at the start
of the next run to tear down the mount.
"""

import logging as log
from argparse import ArgumentParser, Namespace
from pathlib import Path
from cijoe.core.command import Cijoe


def add_args(parser: ArgumentParser):
    parser.add_argument(
        "--dev-path",
        type=str,
        help="Path to the block device",
    )
    parser.add_argument(
        "--mountpoint",
        type=Path,
        help="Path to mountpoint",
    )


def main(args: Namespace, cijoe: Cijoe) -> int:
    dev_path = cijoe.getconf("xal.dev_path", args.dev_path)
    mountpoint = cijoe.getconf("xal.mountpoint", str(args.mountpoint))

    err, _ = cijoe.run(f"mountpoint {mountpoint}")
    if not err:
        log.info(f"mountpoint({mountpoint}); already mounted, skipping")
        return 0

    err, _ = cijoe.run(f"sudo mkdir -p {mountpoint}")
    if err:
        log.error(f"mountpoint({mountpoint}); failed creating directory")
        return err

    err, _ = cijoe.run(f"sudo mount {dev_path} {mountpoint}")
    if err:
        log.error(f"mountpoint({mountpoint}); failed mounting {dev_path}")
        return err

    err, _ = cijoe.run(f"sudo chown -R $USER:$USER {mountpoint}")
    if err:
        log.error("chown failed")
        return err

    log.info(f"Mounted {dev_path} at {mountpoint}")
    return 0
