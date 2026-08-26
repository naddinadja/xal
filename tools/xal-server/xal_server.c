/**
 * xal-server -- publish xal indexes over POSIX shared memory
 * ==========================================================
 *
 * Reads a TOML configuration listing mounted devices, builds an xal index for each with the
 * FIEMAP backend, and publishes it under the shared memory name the config gives it. Readers
 * attach with xal_from_shm(); no socket or handshake is involved, so the names in the
 * configuration are the whole interface.
 *
 * Where the watch mode calls for it, a watcher per device re-indexes in place when the filesystem
 * changes, so an attached reader sees the update without reattaching.
 *
 * The process blocks until SIGTERM/SIGINT, then closes the indexes, which unlinks the shared
 * memory regions -- readers must be done by then.
 *
 * Usage: xal-server --config <path>
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <syslog.h>
#include <unistd.h>

#include <libxal.h>

#include <xal_server_conf.h>

static volatile sig_atomic_t g_stop = 0;

static void
on_signal(int signo __attribute__((unused)))
{
	g_stop = 1;
}

/**
 * Re-index the device whose filesystem the watcher reports as changed
 *
 * Called from the watch thread. xal_index() rewrites the pools in place under the sequence lock,
 * so readers attached to the shared memory see the update without reattaching. A failure leaves
 * the index dirty, which readers observe through xal_is_dirty(); it is not recoverable from here.
 */
static void
on_dirty(struct xal *xal, void *cb_args)
{
	const struct xal_server_dev *dev = cb_args;
	int err;

	err = xal_index(xal);
	if (err) {
		syslog(LOG_CRIT,
		       "FAILED: xal_index(%s); err(%d); the index is stale, restart "
		       "required",
		       dev->shm_name, err);
		return;
	}

	syslog(LOG_INFO, "re-indexed %s after a filesystem change", dev->uri);
}

/**
 * Report whether an index is published, or being published, under the given shared memory name
 *
 * A name another process owns turns xal_open_from_uri() away in four different ways: losing the
 * creation race (-EEXIST), reaching the region before it is readable (-EAGAIN) or before its
 * first index completes (-ESTALE), and -EINVAL when a secondary attach is refused because this
 * process asked for a watch mode or subtree that only the index builder may set. That last one is
 * the case xal-server normally hits, and it is indistinguishable from a genuine option error such
 * as a mistyped subtree -- so the name is asked directly rather than inferred from errno.
 *
 * The _state region is the one xal_close() unlinks last, so its presence means the name is taken.
 */
static bool
shm_name_taken(const char *shm_name)
{
	char shm_name_state[XAL_SERVER_SHM_NAME_MAXLEN + 8];
	int fd;

	snprintf(shm_name_state, sizeof(shm_name_state), "%s_state", shm_name);

	fd = shm_open(shm_name_state, O_RDONLY, 0);
	if (fd < 0) {
		return false;
	}
	close(fd);

	return true;
}

/**
 * Open, index and -- where the watch mode calls for it -- watch a single device
 *
 * On failure the handle is closed, so the caller has nothing to unwind.
 */
static int
publish(const struct xal_server_conf *conf, const struct xal_server_dev *dev, struct xal **out)
{
	struct xal_opts opts = {0};
	enum xal_procrole procrole;
	struct xal *xal;
	int err;

	opts.be = XAL_BACKEND_FIEMAP;
	opts.watch_mode = conf->watch_mode;
	opts.shm_name = dev->shm_name;

	if (strlen(dev->mountpoint)) {
		opts.mountpoint = dev->mountpoint;
	}
	if (strlen(dev->subtree)) {
		opts.subtree = dev->subtree;
	}

	err = xal_open_from_uri(dev->uri, &xal, &opts);
	if (err) {
		if (shm_name_taken(dev->shm_name)) {
			syslog(LOG_ERR,
			       "shm_name(%s) is already published, or is being published, by "
			       "another process; err(%d)",
			       dev->shm_name, err);
			return -EEXIST;
		}

		syslog(LOG_ERR, "FAILED: xal_open_from_uri(%s); err(%d)", dev->uri, err);
		return err;
	}

	/* A name already carrying an index makes this process a reader of someone else's pools: it
	 * cannot index into them, and closing them would not release the name. Two servers sharing
	 * a name is a configuration mistake, so refuse rather than idle. */
	procrole = xal_get_procrole(xal);
	if (procrole != XAL_PROCROLE_PRIMARY) {
		syslog(LOG_ERR, "shm_name(%s) is already published by another process",
		       dev->shm_name);
		err = -EEXIST;
		goto failed;
	}

	err = xal_index(xal);
	if (err) {
		syslog(LOG_ERR, "FAILED: xal_index(%s); err(%d)", dev->uri, err);
		goto failed;
	}

	/* XAL_WATCHMODE_REFLINK_SNAPSHOT pins the extents with reflink clones at index time
	 * instead of running an inotify watcher, so there is no thread to start for it. */
	if (conf->watch_mode && (conf->watch_mode != XAL_WATCHMODE_REFLINK_SNAPSHOT)) {
		err = xal_watch_filesystem(xal, on_dirty, (void *)dev);
		if (err) {
			syslog(LOG_ERR, "FAILED: xal_watch_filesystem(%s); err(%d)", dev->uri, err);
			goto failed;
		}
	}

	syslog(LOG_NOTICE, "published %s at shm_name(%s)", dev->uri, dev->shm_name);

	*out = xal;

	return 0;

failed:
	xal_close(xal);

	return err;
}

/**
 * Close every published index
 *
 * xal_close() joins the watch thread, so there is no watcher to stop first. It also unlinks the
 * shared memory, which is why this waits for the stop signal rather than running on the first
 * failure to publish.
 */
static void
unpublish(unsigned int nxals, struct xal **xals)
{
	for (unsigned int i = 0; i < nxals; i++) {
		xal_close(xals[i]);
	}

	free(xals);
}

static int
parse_args(int argc, char *argv[], const char **config_file_path)
{
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--config") == 0) {
			if (i + 1 >= argc) {
				syslog(LOG_CRIT, "--config must be followed by a path");
				return -EINVAL;
			}
			*config_file_path = argv[++i];
		} else {
			syslog(LOG_CRIT, "unexpected argument: %s", argv[i]);
			return -EINVAL;
		}
	}

	if (!*config_file_path) {
		syslog(LOG_CRIT, "no configuration file given, see --config");
		return -EINVAL;
	}

	return 0;
}

int
main(int argc, char **argv)
{
	struct xal_server_conf conf = {0};
	const char *config_file_path = NULL;
	struct xal **xals = NULL;
	unsigned int npublished = 0;
	struct sigaction sa = {0};
	sigset_t block, orig;
	int logopt = LOG_PID;
	int err;

	/* Run from a terminal, nothing would otherwise be visible without tailing the journal in
	 * another window. Under systemd stderr is already routed to the journal, so adding
	 * LOG_PERROR there would log every line twice. */
	if (isatty(STDERR_FILENO)) {
		logopt |= LOG_PERROR;
	}

	openlog("xal-server", logopt, LOG_DAEMON);

	err = parse_args(argc, argv, &config_file_path);
	if (err) {
		goto exit;
	}

	err = xal_server_conf_from_toml(config_file_path, &conf);
	if (err) {
		goto exit;
	}
	setlogmask(LOG_UPTO(conf.log_level));

	xals = calloc(conf.ndevs, sizeof(*xals));
	if (!xals) {
		err = -errno;
		syslog(LOG_CRIT, "FAILED: calloc(); err(%d)", err);
		goto exit;
	}

	/* Both of these must happen before the first publish, because xal_watch_filesystem()
	 * starts a watcher thread per device and a thread inherits the signal mask in effect at
	 * pthread_create(). Blocking here means every watcher starts with these signals blocked,
	 * so the kernel can only deliver a process-directed SIGTERM/SIGINT to this thread -- the
	 * one that waits for it. Installed after the publish loop instead, the handler could run
	 * on a watcher, setting g_stop while this thread stayed asleep in sigsuspend() forever.
	 *
	 * It also means a signal arriving mid-publish is held pending rather than taking the
	 * default action and killing the process outright, which would leave the shared memory
	 * regions behind for the next start to trip over. The process is still single-threaded
	 * here, so sigprocmask() is well defined. */
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);

	err = sigaction(SIGTERM, &sa, NULL);
	if (err) {
		err = -errno;
		syslog(LOG_CRIT, "FAILED: sigaction(SIGTERM); err(%d)", err);
		goto exit;
	}

	err = sigaction(SIGINT, &sa, NULL);
	if (err) {
		err = -errno;
		syslog(LOG_CRIT, "FAILED: sigaction(SIGINT); err(%d)", err);
		goto exit;
	}

	sigemptyset(&block);
	sigaddset(&block, SIGTERM);
	sigaddset(&block, SIGINT);

	err = sigprocmask(SIG_BLOCK, &block, &orig);
	if (err) {
		err = -errno;
		syslog(LOG_CRIT, "FAILED: sigprocmask(SIG_BLOCK); err(%d)", err);
		goto exit;
	}

	for (npublished = 0; npublished < conf.ndevs; npublished++) {
		err = publish(&conf, &conf.devs[npublished], &xals[npublished]);
		if (err) {
			goto exit;
		}
	}

	syslog(LOG_NOTICE, "serving %u device(s)", conf.ndevs);

	/* sigsuspend() atomically restores the unblocked mask and waits, which closes the pause()
	 * race where a signal arriving between the g_stop test and the wait would be delivered
	 * while blocked and leave the process asleep forever. A signal that arrived during the
	 * publish loop is still pending and is delivered on the first call here. */
	while (!g_stop) {
		sigsuspend(&orig);
	}

	syslog(LOG_NOTICE, "terminating");

exit:
	/* The mask is deliberately left blocked: a second SIGTERM landing during the close below
	 * would skip the shm unlink and leave exactly the stale regions this is here to avoid.
	 * SIGKILL still gets through for an operator who needs out of a slow close. */
	unpublish(npublished, xals);
	free(conf.devs);

	/* Both LOG_PERROR and the log mask depend on things that may themselves be what failed --
	 * a terminal check, and a level read out of the config -- so the reason for a non-zero
	 * exit goes to stderr unconditionally. A daemon that dies silently cannot be diagnosed. */
	if (err) {
		fprintf(stderr, "xal-server: exiting; err(%d) %s\n", err, strerror(abs(err)));
	}

	closelog();

	return err ? EXIT_FAILURE : EXIT_SUCCESS;
}
