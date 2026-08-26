/**
 * xal-server: publish xal indexes over POSIX shared memory
 *
 * Builds an index per configured device with the FIEMAP backend and publishes it under the
 * shm_name from the config. Readers attach with xal_from_shm(); there is no socket, so those
 * names are the whole interface. Closing unlinks the regions, so readers must be done by then.
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
 * Runs on the watch thread. A failure leaves the index dirty for readers to see through
 * xal_is_dirty(), and is not recoverable from here.
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
 * Report whether an index is published, or being published, under the given name
 *
 * A name another process owns turns xal_open_from_uri() away as -EEXIST, -EAGAIN, -ESTALE or
 * -EINVAL depending on timing and options, and the last is indistinguishable from a mistyped
 * subtree. Asking the name directly avoids guessing from errno.
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
 * Open, index and, where the watch mode calls for it, watch a single device
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

	/* Attaching as a secondary would leave this device unpublished, and closing would not
	 * release the name, so two servers on one name is refused rather than tolerated. */
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

	/* Reflink snapshot mode pins extents with clones at index time and runs no watcher. */
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
 * xal_close() joins the watch thread, so there is no watcher to stop first. It also unlinks
 * the shared memory, so readers must be done by the time this runs.
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

	/* Under systemd stderr already reaches the journal, so LOG_PERROR there would log every
	 * line twice. */
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

	/* Installed before the first publish: a watcher thread inherits the mask from
	 * pthread_create(), and a signal delivered to one of those would set g_stop while this
	 * thread stayed asleep in sigsuspend(). Blocking early also keeps a mid-publish signal from
	 * killing the process and stranding the shm regions. */
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

	/* sigsuspend() restores the unblocked mask and waits atomically, so a signal arriving just
	 * before the wait cannot be missed. One pending from the publish loop lands here. */
	while (!g_stop) {
		sigsuspend(&orig);
	}

	syslog(LOG_NOTICE, "terminating");

exit:
	/* Left blocked: a second SIGTERM here would skip the unlink and strand the regions. */
	unpublish(npublished, xals);
	free(conf.devs);

	/* The log mask comes from the config, which may be what failed, so this path does not rely
	 * on syslog. */
	if (err) {
		fprintf(stderr, "xal-server: exiting; err(%d) %s\n", err, strerror(abs(err)));
	}

	closelog();

	return err ? EXIT_FAILURE : EXIT_SUCCESS;
}
