/*  Copyright (C) 2023 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <sys/stat.h>
#include <urcu.h>

#ifdef ENABLE_CAP_NG
#include <cap-ng.h>
#endif

#include "libdnssec/crypto.h"
#include "libknot/libknot.h"
#include "contrib/strtonum.h"
#include "knot/ctl/process.h"
#include "knot/conf/conf.h"
#include "knot/conf/migration.h"
#include "knot/conf/module.h"
#include "knot/common/log.h"
#include "knot/common/process.h"
#include "knot/common/stats.h"
#include "knot/common/systemd.h"
#include "knot/server/server.h"
#include "knot/server/tcp-handler.h"

#define PROGRAM_NAME "knotd"

/* Signal flags. */
static volatile bool sig_req_stop = false;
static volatile bool sig_req_reload = false;
static volatile bool sig_req_zones_reload = false;

static int make_daemon(int nochdir, int noclose)
{
	int ret;

	switch (fork()) {
	case -1:
		/* Error */
		return -1;
	case 0:
		/* Forked */
		break;
	default:
		/* Exit the main process */
		_exit(0);
	}

	if (setsid() == -1) {
		return -1;
	}

	if (!nochdir) {
		ret = chdir("/");
		if (ret == -1)
			return errno;
	}

	if (!noclose) {
		ret  = close(STDIN_FILENO);
		ret += close(STDOUT_FILENO);
		ret += close(STDERR_FILENO);
		if (ret < 0) {
			return errno;
		}

		int fd = open("/dev/null", O_RDWR);
		if (fd == -1) {
			return errno;
		}

		if (dup2(fd, STDIN_FILENO) < 0) {
			close(fd);
			return errno;
		}
		if (dup2(fd, STDOUT_FILENO) < 0) {
			close(fd);
			return errno;
		}
		if (dup2(fd, STDERR_FILENO) < 0) {
			close(fd);
			return errno;
		}
		close(fd);
	}

	return 0;
}

struct signal {
	int signum;
	bool handle;
};

/*! \brief Signals used by the server. */
static const struct signal SIGNALS[] = {
	{ SIGHUP,  true  },  /* Reload server. */
	{ SIGUSR1, true  },  /* Reload zones. */
	{ SIGINT,  true  },  /* Terminate server. */
	{ SIGTERM, true  },  /* Terminate server. */
	{ SIGALRM, false },  /* Internal thread synchronization. */
	{ SIGPIPE, false },  /* Ignored. Some I/O errors. */
	{ 0 }
};

/*! \brief Server signal handler. */
static void handle_signal(int signum)
{
	switch (signum) {
	case SIGHUP:
		sig_req_reload = true;
		break;
	case SIGUSR1:
		sig_req_zones_reload = true;
		break;
	case SIGINT:
	case SIGTERM:
		if (sig_req_stop) {
			exit(EXIT_FAILURE);
		}
		sig_req_stop = true;
		break;
	default:
		/* ignore */
		break;
	}
}

/*! \brief Setup signal handlers and blocking mask. */
static void setup_signals(void)
{
	/* Block all signals. */
	static sigset_t all;
	sigfillset(&all);
	sigdelset(&all, SIGPROF);
	sigdelset(&all, SIGQUIT);
	sigdelset(&all, SIGILL);
	sigdelset(&all, SIGABRT);
	sigdelset(&all, SIGBUS);
	sigdelset(&all, SIGFPE);
	sigdelset(&all, SIGSEGV);
	pthread_sigmask(SIG_SETMASK, &all, NULL);

	/* Setup handlers. */
	struct sigaction action = { .sa_handler = handle_signal };
	for (const struct signal *s = SIGNALS; s->signum > 0; s++) {
		sigaction(s->signum, &action, NULL);
	}
}

/*! \brief Unblock server control signals. */
static void enable_signals(void)
{
	sigset_t mask;
	sigemptyset(&mask);

	for (const struct signal *s = SIGNALS; s->signum > 0; s++) {
		if (s->handle) {
			sigaddset(&mask, s->signum);
		}
	}

	pthread_sigmask(SIG_UNBLOCK, &mask, NULL);
}

/*! \brief Drop POSIX 1003.1e capabilities. */
static void drop_capabilities(void)
{
#ifdef ENABLE_CAP_NG
	/* Drop all capabilities. */
	if (capng_have_capability(CAPNG_EFFECTIVE, CAP_SETPCAP)) {
		capng_clear(CAPNG_SELECT_BOTH);

		/* Apply. */
		if (capng_apply(CAPNG_SELECT_BOTH) < 0) {
			log_error("failed to set process capabilities (%s)",
			          strerror(errno));
		}
	} else {
		log_info("process not allowed to set capabilities, skipping");
	}
#endif /* ENABLE_CAP_NG */
}

/*! \brief Event loop listening for signals and remote commands. */
static void event_loop(server_t *server, const char *socket)
{
	knot_ctl_t *ctl = knot_ctl_alloc();
	if (ctl == NULL) {
		log_fatal("control, failed to initialize (%s)",
		          knot_strerror(KNOT_ENOMEM));
		return;
	}

	// Set control timeout.
	knot_ctl_set_timeout(ctl, conf()->cache.ctl_timeout);

	/* Get control socket configuration. */
	char *listen;
	if (socket == NULL) {
		conf_val_t listen_val = conf_get(conf(), C_CTL, C_LISTEN);
		conf_val_t rundir_val = conf_get(conf(), C_SRV, C_RUNDIR);
		char *rundir = conf_abs_path(&rundir_val, NULL);
		listen = conf_abs_path(&listen_val, rundir);
		free(rundir);
	} else {
		listen = strdup(socket);
	}
	if (listen == NULL) {
		knot_ctl_free(ctl);
		log_fatal("control, empty socket path");
		return;
	}

	log_info("control, binding to '%s'", listen);

	/* Bind the control socket. */
	int ret = knot_ctl_bind(ctl, listen);
	if (ret != KNOT_EOK) {
		knot_ctl_free(ctl);
		log_fatal("control, failed to bind socket '%s' (%s)",
		          listen, knot_strerror(ret));
		free(listen);
		return;
	}
	free(listen);

	enable_signals();

	/* Notify systemd about successful start. */
	systemd_ready_notify();
	if (conf()->cache.srv_dbus_event & DBUS_EVENT_RUNNING) {
		systemd_emit_running(true);
	}

	/* Run event loop. */
	for (;;) {
		/* Interrupts. */
		if (sig_req_reload && !sig_req_stop) {
			sig_req_reload = false;
			server_reload(server, RELOAD_FULL);
		}
		if (sig_req_zones_reload && !sig_req_stop) {
			sig_req_zones_reload = false;
			reload_t mode = server->catalog_upd_signal ? RELOAD_CATALOG : RELOAD_ZONES;
			server->catalog_upd_signal = false;
			server_update_zones(conf(), server, mode);
		}
		if (sig_req_stop) {
			break;
		}

		// Update control timeout.
		knot_ctl_set_timeout(ctl, conf()->cache.ctl_timeout);

		if (sig_req_reload || sig_req_zones_reload) {
			continue;
		}

		ret = knot_ctl_accept(ctl);
		if (ret != KNOT_EOK) {
			continue;
		}

		ret = ctl_process(ctl, server);
		knot_ctl_close(ctl);
		if (ret == KNOT_CTL_ESTOP) {
			break;
		}
	}

	if (conf()->cache.srv_dbus_event & DBUS_EVENT_RUNNING) {
		systemd_emit_running(false);
	}

	/* Unbind the control socket. */
	knot_ctl_unbind(ctl);
	knot_ctl_free(ctl);
}

static void print_help(void)
{
	printf("Usage: %s [parameters]\n"
	       "\n"
	       "Parameters:\n"
	       " -c, --config <file>        Use a textual configuration file.\n"
	       "                             (default %s)\n"
	       " -C, --confdb <dir>         Use a binary configuration database directory.\n"
	       "                             (default %s)\n"
	       " -m, --max-conf-size <MiB>  Set maximum size of the configuration database (max 10000 MiB).\n"
	       "                             (default %d MiB)\n"
	       " -s, --socket <path>        Use a remote control UNIX socket path.\n"
	       "                             (default %s)\n"
	       " -d, --daemonize=[dir]      Run the server as a daemon (with new root directory).\n"
	       " -v, --verbose              Enable debug output.\n"
	       " -h, --help                 Print the program help.\n"
	       " -V, --version              Print the program version.\n",
	       PROGRAM_NAME, CONF_DEFAULT_FILE, CONF_DEFAULT_DBDIR,
	       CONF_MAPSIZE, RUN_DIR "/knot.sock");
}

static void print_version(void)
{
	printf("%s (Knot DNS), version %s\n", PROGRAM_NAME, PACKAGE_VERSION);
}

static int set_config(const char *confdb, const char *config, size_t max_conf_size)
{
	if (config != NULL && confdb != NULL) {
		log_fatal("ambiguous configuration source");
		return KNOT_EINVAL;
	}

	/* Choose the optimal config source. */
	bool import = false;
	if (confdb != NULL) {
		import = false;
	} else if (config != NULL){
		import = true;
	} else if (conf_db_exists(CONF_DEFAULT_DBDIR)) {
		import = false;
		confdb = CONF_DEFAULT_DBDIR;
	} else {
		import = true;
		config = CONF_DEFAULT_FILE;
	}

	/* Open confdb. */
	conf_t *new_conf = NULL;
	int ret = conf_new(&new_conf, conf_schema, confdb, max_conf_size, CONF_FREQMODULES);
	if (ret != KNOT_EOK) {
		log_fatal("failed to open configuration database '%s' (%s)",
		          (confdb != NULL) ? confdb : "", knot_strerror(ret));
		return ret;
	}

	/* Import the config file. */
	if (import) {
		ret = conf_import(new_conf, config, true, true);
		if (ret != KNOT_EOK) {
			log_fatal("failed to load configuration file '%s' (%s)",
			          config, knot_strerror(ret));
			conf_free(new_conf);
			return ret;
		}
	}

	// Migrate from old schema.
	ret = conf_migrate(new_conf);
	if (ret != KNOT_EOK) {
		log_error("failed to migrate configuration (%s)", knot_strerror(ret));
	}

	/* Update to the new config. */
	conf_update(new_conf, CONF_UPD_FNONE);

	return KNOT_EOK;
}

int main(int argc, char **argv)
{
	bool daemonize = false;
	const char *config = NULL;
	const char *confdb = NULL;
	size_t max_conf_size = (size_t)CONF_MAPSIZE * 1024 * 1024;
	const char *daemon_root = "/";
	char *socket = NULL;
	bool verbose = false;

	/* Long options. */
	struct option opts[] = {
		{ "config",        required_argument, NULL, 'c' },
		{ "confdb",        required_argument, NULL, 'C' },
		{ "max-conf-size", required_argument, NULL, 'm' },
		{ "socket",        required_argument, NULL, 's' },
		{ "daemonize",     optional_argument, NULL, 'd' },
		{ "verbose",       no_argument,       NULL, 'v' },
		{ "help",          no_argument,       NULL, 'h' },
		{ "version",       no_argument,       NULL, 'V' },
		{ NULL }
	};

	/* Set the time zone. */
	tzset();

	/* Parse command line arguments. */
	int opt = 0;
	while ((opt = getopt_long(argc, argv, "c:C:m:s:dvhV", opts, NULL)) != -1) {
		switch (opt) {
		case 'c':
			config = optarg;
			break;
		case 'C':
			confdb = optarg;
			break;
		case 'm':
			if (str_to_size(optarg, &max_conf_size, 1, 10000) != KNOT_EOK) {
				print_help();
				return EXIT_FAILURE;
			}
			/* Convert to bytes. */
			max_conf_size *= 1024 * 1024;
			break;
		case 's':
			socket = optarg;
			break;
		case 'd':
			daemonize = true;
			if (optarg) {
				daemon_root = optarg;
			}
			break;
		case 'v':
			verbose = true;
			break;
		case 'h':
			print_help();
			return EXIT_SUCCESS;
		case 'V':
			print_version();
			return EXIT_SUCCESS;
		default:
			print_help();
			return EXIT_FAILURE;
		}
	}

	/* Check for non-option parameters. */
	if (argc - optind > 0) {
		print_help();
		return EXIT_FAILURE;
	}

	/* Set file creation mask to remove all permissions for others. */
	umask(S_IROTH | S_IWOTH | S_IXOTH);

	/* Now check if we want to daemonize. */
	if (daemonize) {
		if (make_daemon(1, 0) != 0) {
			fprintf(stderr, "Daemonization failed, shutting down...\n");
			return EXIT_FAILURE;
		}
	}

	/* Setup base signal handling. */
	setup_signals();

	/* Initialize cryptographic backend. */
	dnssec_crypto_init();

	/* Initialize pseudorandom number generator. */
	srand(time(NULL));

	/* Initialize logging subsystem. */
	log_init();
	if (verbose) {
		log_levels_add(LOG_TARGET_STDOUT, LOG_SOURCE_ANY, LOG_MASK(LOG_DEBUG));
	}

	/* Set up the configuration */
	int ret = set_config(confdb, config, max_conf_size);
	if (ret != KNOT_EOK) {
		log_close();
		dnssec_crypto_cleanup();
		return EXIT_FAILURE;
	}

	/* Reconfigure logging. */
	log_reconfigure(conf());

	/* Initialize server. */
	server_t server;
	ret = server_init(&server, conf()->cache.srv_bg_threads);
	if (ret != KNOT_EOK) {
		log_fatal("failed to initialize server (%s)", knot_strerror(ret));
		conf_free(conf());
		log_close();
		dnssec_crypto_cleanup();
		return EXIT_FAILURE;
	}

	/* Reconfigure server workers, interfaces, and databases.
	 * @note This MUST be done before we drop privileges. */
	ret = server_reconfigure(conf(), &server);
	if (ret != KNOT_EOK) {
		log_fatal("failed to configure server");
		server_wait(&server);
		server_deinit(&server);
		conf_free(conf());
		log_close();
		dnssec_crypto_cleanup();
		return EXIT_FAILURE;
	}

	if (conf()->cache.srv_dbus_event != DBUS_EVENT_NONE) {
		ret = systemd_dbus_open();
		if (ret != KNOT_EOK) {
			log_error("d-bus: failed to open system bus (%s)",
			          knot_strerror(ret));
		} else {
			log_info("d-bus: connected to system bus");
		}
		int64_t delay = conf_get_int(conf(), C_SRV, C_DBUS_INIT_DELAY);
		sleep(delay);
	}

	/* Alter privileges. */
	int uid, gid;
	if (conf_user(conf(), &uid, &gid) != KNOT_EOK ||
	    log_update_privileges(uid, gid) != KNOT_EOK ||
	    proc_update_privileges(uid, gid) != KNOT_EOK) {
		log_fatal("failed to drop privileges");
		server_wait(&server);
		server_deinit(&server);
		conf_free(conf());
		systemd_dbus_close();
		log_close();
		dnssec_crypto_cleanup();
		return EXIT_FAILURE;
	}

	/* Drop POSIX capabilities. */
	drop_capabilities();

	/* Activate global query modules. */
	conf_activate_modules(conf(), &server, NULL, conf()->query_modules,
	                      &conf()->query_plan);

	/* Check and create PID file. */
	unsigned long pid = pid_check_and_create();
	if (pid == 0) {
		server_wait(&server);
		server_deinit(&server);
		conf_free(conf());
		systemd_dbus_close();
		log_close();
		dnssec_crypto_cleanup();
		return EXIT_FAILURE;
	}

	if (daemonize) {
		if (chdir(daemon_root) != 0) {
			log_warning("failed to change working directory to %s",
			            daemon_root);
		} else {
			log_info("changed directory to %s", daemon_root);
		}
	}

	/* Now we're going multithreaded. */
	rcu_register_thread();

	/* Populate zone database. */
	log_info("loading %zu zones", conf_id_count(conf(), C_ZONE));
	server_update_zones(conf(), &server, RELOAD_ZONES);

	/* Check number of loaded zones. */
	if (knot_zonedb_size(server.zone_db) == 0) {
		log_warning("no zones loaded");
	}

	stats_reconfigure(conf(), &server);

	/* Start it up. */
	log_info("starting server");
	conf_val_t async_val = conf_get(conf(), C_SRV, C_ASYNC_START);
	ret = server_start(&server, conf_bool(&async_val));
	if (ret != KNOT_EOK) {
		log_fatal("failed to start server (%s)", knot_strerror(ret));
		server_wait(&server);
		stats_deinit();
		server_deinit(&server);
		rcu_unregister_thread();
		pid_cleanup();
		conf_free(conf());
		systemd_dbus_close();
		log_close();
		dnssec_crypto_cleanup();
		return EXIT_FAILURE;
	}

	if (daemonize) {
		log_info("server started as a daemon, PID %lu", pid);
	} else {
		log_info("server started in the foreground, PID %lu", pid);
	}

	/* Start the event loop. */
	event_loop(&server, socket);

	/* Teardown server. */
	server_stop(&server);
	server_wait(&server);
	stats_deinit();

	/* Cleanup PID file. */
	pid_cleanup();

	/* Free server and configuration. */
	server_deinit(&server);
	conf_free(conf());

	/* Unhook from RCU. */
	rcu_unregister_thread();

	systemd_dbus_close();

	log_info("shutting down");
	log_close();

	dnssec_crypto_cleanup();

	return EXIT_SUCCESS;
}
