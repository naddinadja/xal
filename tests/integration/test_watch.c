#define _GNU_SOURCE
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libxal.h>
#include <xal.h>
#include <xal_be_fiemap.h>

#include "test.h"

/*
 * Tests the lifecycle of the watch thread: starting and stopping it, restarting it, starting it
 * twice, stopping it when it is not running, closing with it running, and stopping it after it
 * exited on its own.
 *
 * The watched directories hold only subdirectories, so the FIEMAP ioctl is never called and the
 * tests run on filesystems without it, such as tmpfs. The thread exits on its own when the
 * filesystem it watches is unmounted; that test mounts a tmpfs to unmount, so it needs root and
 * is skipped without it.
 *
 * Usage: test_watch [basedir]
 *
 * basedir defaults to /tmp.
 */

#define POLL_TIMEOUT_MS 2000

static const char *g_basedir = "/tmp";

static bool
poll_until_dirty(struct xal *xal, int timeout_ms)
{
	for (int i = 0; i < timeout_ms; i++) {
		if (xal_is_dirty(xal)) {
			return true;
		}
		usleep(1000);
	}

	return false;
}

static int
open_and_index(char *path, enum xal_watchmode watch_mode, struct xal **xal)
{
	struct xal_opts opts = {0};
	int err;

	opts.watch_mode = watch_mode;

	err = xal_be_fiemap_open(xal, path, &opts);
	if (err) {
		return err;
	}

	err = xal_index(*xal);
	if (err) {
		xal_close(*xal);
		return err;
	}

	return 0;
}

static int
make_tmpdir(char *tmpdir, size_t tmpdir_nbytes)
{
	snprintf(tmpdir, tmpdir_nbytes, "%s/xal_watch_XXXXXX", g_basedir);
	TEST_ASSERT(mkdtemp(tmpdir) != NULL);

	return 0;
}

static int
test_watch_before_index(void)
{
	struct xal_opts opts = { .watch_mode = XAL_WATCHMODE_DIRTY_DETECTION };
	char tmpdir[256];
	struct xal *xal;
	int err;

	err = make_tmpdir(tmpdir, sizeof(tmpdir));
	TEST_ASSERT(err == 0);

	err = xal_be_fiemap_open(&xal, tmpdir, &opts);
	TEST_ASSERT(err == 0);

	err = xal_watch_filesystem(xal, NULL, NULL);

	xal_close(xal);
	rmdir(tmpdir);
	TEST_ASSERT(err == -EINVAL);

	return 0;
}

static int
test_stop_without_start(void)
{
	char tmpdir[256];
	struct xal *xal;
	int err;

	err = make_tmpdir(tmpdir, sizeof(tmpdir));
	TEST_ASSERT(err == 0);

	err = open_and_index(tmpdir, XAL_WATCHMODE_DIRTY_DETECTION, &xal);
	TEST_ASSERT(err == 0);

	err = xal_stop_watching_filesystem(xal);

	xal_close(xal);
	rmdir(tmpdir);
	TEST_ASSERT(err == -EINVAL);

	return 0;
}

static int
check_start_stop(struct xal *xal)
{
	int err;

	err = xal_watch_filesystem(xal, NULL, NULL);
	TEST_ASSERT(err == 0);

	err = xal_stop_watching_filesystem(xal);
	TEST_ASSERT(err == 0);

	err = xal_stop_watching_filesystem(xal);
	TEST_ASSERT(err == -EINVAL);

	return 0;
}

static int
test_start_stop(void)
{
	char tmpdir[256];
	struct xal *xal;
	int err;

	err = make_tmpdir(tmpdir, sizeof(tmpdir));
	TEST_ASSERT(err == 0);

	err = open_and_index(tmpdir, XAL_WATCHMODE_DIRTY_DETECTION, &xal);
	TEST_ASSERT(err == 0);

	err = check_start_stop(xal);

	xal_close(xal);
	rmdir(tmpdir);
	TEST_ASSERT(err == 0);

	return 0;
}

static int
check_start_twice(struct xal *xal)
{
	int err;

	err = xal_watch_filesystem(xal, NULL, NULL);
	TEST_ASSERT(err == 0);

	err = xal_watch_filesystem(xal, NULL, NULL);
	TEST_ASSERT(err == 0);

	// One thread was started, so there is one to stop
	err = xal_stop_watching_filesystem(xal);
	TEST_ASSERT(err == 0);

	err = xal_stop_watching_filesystem(xal);
	TEST_ASSERT(err == -EINVAL);

	return 0;
}

static int
test_start_twice(void)
{
	char tmpdir[256];
	struct xal *xal;
	int err;

	err = make_tmpdir(tmpdir, sizeof(tmpdir));
	TEST_ASSERT(err == 0);

	err = open_and_index(tmpdir, XAL_WATCHMODE_DIRTY_DETECTION, &xal);
	TEST_ASSERT(err == 0);

	err = check_start_twice(xal);

	xal_close(xal);
	rmdir(tmpdir);
	TEST_ASSERT(err == 0);

	return 0;
}

static void
count_cb(struct xal *XAL_UNUSED(xal), void *cb_args)
{
	atomic_int *count = cb_args;

	atomic_fetch_add(count, 1);
}

/*
 * A restarted watcher still detects changes and calls the callback it was restarted with
 */
static int
check_restart(struct xal *xal, char *subdir)
{
	atomic_int count = 0;
	int err;

	err = xal_watch_filesystem(xal, NULL, NULL);
	TEST_ASSERT(err == 0);

	err = xal_stop_watching_filesystem(xal);
	TEST_ASSERT(err == 0);

	err = xal_watch_filesystem(xal, count_cb, &count);
	TEST_ASSERT(err == 0);

	err = mkdir(subdir, 0755);
	TEST_ASSERT(err == 0);

	// The callback runs after the dirty mark, so wait for it rather than for the mark
	for (int i = 0; i < POLL_TIMEOUT_MS && !atomic_load(&count); i++) {
		usleep(1000);
	}

	err = xal_stop_watching_filesystem(xal);
	TEST_ASSERT(err == 0);
	TEST_ASSERT(xal_is_dirty(xal));
	TEST_ASSERT(atomic_load(&count) == 1);

	return 0;
}

static int
test_restart(void)
{
	char tmpdir[256];
	char subdir[300];
	struct xal *xal;
	int err;

	err = make_tmpdir(tmpdir, sizeof(tmpdir));
	TEST_ASSERT(err == 0);
	snprintf(subdir, sizeof(subdir), "%s/subdir", tmpdir);

	err = open_and_index(tmpdir, XAL_WATCHMODE_DIRTY_DETECTION, &xal);
	TEST_ASSERT(err == 0);

	err = check_restart(xal, subdir);

	xal_close(xal);
	rmdir(subdir);
	rmdir(tmpdir);
	TEST_ASSERT(err == 0);

	return 0;
}

/*
 * xal_close() joins a running watcher without it being stopped first
 */
static int
test_close_while_watching(void)
{
	char tmpdir[256];
	struct xal *xal;
	int err;

	err = make_tmpdir(tmpdir, sizeof(tmpdir));
	TEST_ASSERT(err == 0);

	err = open_and_index(tmpdir, XAL_WATCHMODE_DIRTY_DETECTION, &xal);
	TEST_ASSERT(err == 0);

	err = xal_watch_filesystem(xal, NULL, NULL);

	xal_close(xal);
	rmdir(tmpdir);
	TEST_ASSERT(err == 0);

	return 0;
}

/*
 * Unmounting the watched filesystem makes the thread exit on its own, in extent-update mode,
 * after marking the index dirty. Stopping it afterwards reaps it and succeeds, once.
 */
static int
check_stop_after_exit(struct xal *xal, char *mnt)
{
	int err;

	err = xal_watch_filesystem(xal, NULL, NULL);
	TEST_ASSERT(err == 0);

	err = umount2(mnt, 0);
	TEST_ASSERT(err == 0);

	TEST_ASSERT(poll_until_dirty(xal, POLL_TIMEOUT_MS));

	err = xal_stop_watching_filesystem(xal);
	TEST_ASSERT(err == 0);

	err = xal_stop_watching_filesystem(xal);
	TEST_ASSERT(err == -EINVAL);

	return 0;
}

static int
test_stop_after_exit(void)
{
	char tmpdir[256];
	char mnt[300];
	char subdir[350];
	struct xal *xal;
	int err;

	if (geteuid() != 0) {
		printf("SKIP test_stop_after_exit: needs root to mount a tmpfs\n");
		return 0;
	}

	err = make_tmpdir(tmpdir, sizeof(tmpdir));
	TEST_ASSERT(err == 0);
	snprintf(mnt, sizeof(mnt), "%s/mnt", tmpdir);
	snprintf(subdir, sizeof(subdir), "%s/subdir", mnt);

	err = mkdir(mnt, 0755);
	TEST_ASSERT(err == 0);

	err = mount("tmpfs", mnt, "tmpfs", 0, NULL);
	TEST_ASSERT(err == 0);

	err = mkdir(subdir, 0755);
	if (!err) {
		err = open_and_index(mnt, XAL_WATCHMODE_EXTENT_UPDATE, &xal);
	}
	if (!err) {
		err = check_stop_after_exit(xal, mnt);
		xal_close(xal);
	}

	umount2(mnt, MNT_DETACH);
	rmdir(mnt);
	rmdir(tmpdir);
	TEST_ASSERT(err == 0);

	return 0;
}

int
main(int argc, char *argv[])
{
	int failures = 0;

	if (argc > 1) {
		g_basedir = argv[1];
	}

	TEST_RUN(test_watch_before_index);
	TEST_RUN(test_stop_without_start);
	TEST_RUN(test_start_stop);
	TEST_RUN(test_start_twice);
	TEST_RUN(test_restart);
	TEST_RUN(test_close_while_watching);
	TEST_RUN(test_stop_after_exit);

	return failures;
}
