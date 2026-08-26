#ifndef XAL_SERVER_CONF_H
#define XAL_SERVER_CONF_H

#include <libxal.h>

#define XAL_SERVER_URI_MAXLEN 256
#define XAL_SERVER_SHM_NAME_MAXLEN 64

struct xal_server_dev {
	char uri[XAL_SERVER_URI_MAXLEN];
	char shm_name[XAL_SERVER_SHM_NAME_MAXLEN];
	char mountpoint[XAL_PATH_MAXLEN + 1];
	char subtree[XAL_PATH_MAXLEN + 1];
};

struct xal_server_conf {
	int log_level;
	enum xal_watchmode watch_mode;
	unsigned int ndevs;
	struct xal_server_dev *devs;
};

/**
 * Parse the TOML configuration file
 *
 * We expect the configuration file to have keys:
 * - log_level (int)
 * - devices (array of tables with 'uri' and 'shm_name'; 'mountpoint' and 'subtree' optional)
 * - xal.watchmode (int)
 *
 * There is no default configuration: without a device list there is nothing to publish, so a
 * missing or unreadable file is an error rather than something to fall back from.
 *
 * Problems are reported to syslog. On success the caller owns conf->devs and releases it with
 * free(); on failure conf->devs is NULL.
 *
 * @param path Path to the configuration file
 * @param conf Configuration struct to load into
 *
 * @return On success, 0 is returned. On error, negative errno is returned to indicate the error.
 */
int
xal_server_conf_from_toml(const char *path, struct xal_server_conf *conf);

#endif /* XAL_SERVER_CONF_H */
