/* -*- linux-c -*-
 *
 * common.c - staprun suid/user common code
 *
 * This file is part of systemtap, and is free software.  You can
 * redistribute it and/or modify it under the terms of the GNU General
 * Public License (GPL); either version 2, or (at your option) any
 * later version.
 *
 * Copyright (C) 2007-2017 Red Hat Inc.
 */

#include "staprun.h"
#include <sys/types.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <assert.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <limits.h>
#include "../git_version.h"
#include "../version.h"

#ifndef OPEN_MAX
#define OPEN_MAX 256
#endif

/* variables needed by parse_args() */
int verbose;
int suppress_warnings;
int target_pid;
int target_namespaces_pid;
unsigned int buffer_size;
unsigned int reader_timeout_ms;
char *target_cmd;
char *outfile_name;
int rename_mod;
int attach_mod;
int delete_mod;
int load_only;
int need_uprobes;
const char *uprobes_path = NULL;
int daemon_mode;
off_t fsize_max;
int fnum_max;
int remote_id;
const char *remote_uri;
int read_stdin;
int relay_basedir_fd;
int color_errors;
color_modes color_mode;
int monitor;
int monitor_interval;

/* module variables */
char *modname = NULL;
char *modpath = "";
char *modoptions[MAXMODOPTIONS];

int control_channel = -1; /* NB: fd==0 possible */

static char path_buf[PATH_MAX];
static char *get_abspath(char *path)
{
	int len;
	if (path[0] == '/')
		return path;

	len = strlen(getcwd(path_buf, PATH_MAX));
	if (len + 2 + strlen(path) >= PATH_MAX)
		return NULL;
	path_buf[len] = '/';
	/* Note that this strcpy() call is OK, since we checked
	 * the length earlier to make sure the string would fit. */
	strcpy(&path_buf[len + 1], path);
	return path_buf;
}

int make_outfile_name(char *buf, int max, int fnum, int cpu, time_t t, int bulk)
{
	int len;
	if (PATH_MAX < max)
		max = PATH_MAX;
	len = stap_strfloctime(buf, max, outfile_name, t);
	if (len < 0) {
		err(_("Invalid FILE name format\n"));
		return -1;
	}
	/* special case: for testing we sometimes want to write to /dev/null */
	if (strcmp(outfile_name, "/dev/null") == 0) {
		/* This strcpy() call is OK since we know that the
		 * buffer is at least PATH_MAX bytes long at this
		 * point. */
		strcpy(buf, "/dev/null");
	} else {
		if (bulk) {
			if (snprintf_chk(&buf[len], max - len, "_cpu%d.%d",
					 cpu, fnum))
				return -1;
		} else {
			/* stream mode */
			if (snprintf_chk(&buf[len], max - len, ".%d", fnum))
				return -1;
		}
	}
	return 0;
}

void parse_args(int argc, char **argv)
{
	int c;
	char *s;

	/* Initialize option variables. */
	verbose = 0;
	suppress_warnings = 0;
	target_pid = 0;
	target_namespaces_pid = 0;
	buffer_size = 0;
        reader_timeout_ms = 0;
	target_cmd = NULL;
	outfile_name = NULL;
	rename_mod = 0;
	attach_mod = 0;
	delete_mod = 0;
        read_stdin = 0;
	load_only = 0;
	need_uprobes = 0;
	daemon_mode = 0;
	fsize_max = 0;
	fnum_max = 0;
	monitor = 0;
        monitor_interval = 1;
        remote_id = -1;
        remote_uri = NULL;
        relay_basedir_fd = -1;
        color_mode = color_auto;
        color_errors = isatty(STDERR_FILENO)
                && strcmp(getenv("TERM") ?: "notdumb", "dumb");

	while ((c = getopt(argc, argv, "ALu::vihb:t:dc:o:x:N:S:DwRr:VT:C:M:"
#ifdef HAVE_OPENAT
                           "F:"
#endif
                        )) != EOF) {
		switch (c) {
		case 'u':
			need_uprobes = 1;
			if (optarg)
			  uprobes_path = strdup (optarg);
			break;
		case 'v':
			verbose++;
			break;
		case 'w':
			suppress_warnings=1;
			break;
		case 'b':
			buffer_size = (unsigned)atoi(optarg);
			if (buffer_size < 1 || buffer_size > 4095) {
				err(_("Invalid buffer size '%d' (should be 1-4095).\n"), buffer_size);
				usage(argv[0],1);
			}
			break;
		case 't':
		case 'x':
			target_pid = atoi(optarg);
			break;
    case 'N':
      target_namespaces_pid = atoi(optarg);
      // error out if it's obviously and invalid pid
      if (target_namespaces_pid < 1) {
        err(_("Invalid target namespaces pid %d (should be > 0).\n"), target_namespaces_pid);
				usage(argv[0],1);
      }
      break;
		case 'd':
			/* delete module */
			delete_mod = 1;
			break;
		case 'c':
			target_cmd = optarg;
			break;
		case 'o':
			outfile_name = optarg;
			break;
                case 'i':
                        read_stdin = 1;
                        break;
		case 'R':
			rename_mod = 1;
			break;
		case 'A':
			attach_mod = 1;
			break;
		case 'L':
			load_only = 1;
			break;
		case 'D':
			daemon_mode = 1;
			break;
		case 'F':
			relay_basedir_fd = atoi(optarg);
			if (relay_basedir_fd < 0) {
				err(_("Invalid file descriptor option '%s'.\n"), optarg);
				usage(argv[0],1);
			}
			break;
		case 'S':
			assert(optarg != 0); // optarg can't be NULL (or getopt would choke)
			fsize_max = strtoul(optarg, &s, 10);
			fsize_max <<= 20;
			if (s[0] == ',')
				fnum_max = (int)strtoul(&s[1], &s, 10);
			if (s[0] != '\0') {
				err(_("Invalid file size option '%s'.\n"), optarg);
				usage(argv[0],1);
			}
			break;
		case 'r':
			assert(optarg != 0); // optarg can't be NULL (or getopt would choke)
			/* parse ID:URL */
			remote_id = strtoul(optarg, &s, 10);
			if (s[0] == ':')
                                remote_uri = strdup (& s[1]);
                        
                        if (remote_id < 0 || remote_uri == 0 || remote_uri[0] == '\0') {
                                err(_("Cannot process remote id option '%s'.\n"), optarg);
				usage(argv[0],1);
                        }
			break;
		case 'V':
                        /* PRERELEASE */
                        printf(_("Systemtap module loader/runner (version %s, %s)\n"
                              "Copyright (C) 2005-2017 Red Hat, Inc. and others\n"
                              "This is free software; see the source for copying conditions.\n"),
                            VERSION, STAP_EXTENDED_VERSION);
                        _exit(0);
                        break;
                case 'h':
                        usage(argv[0],0);
                        break;
                case 'T':
                        reader_timeout_ms = (unsigned)atoi(optarg);
                        if (reader_timeout_ms < 1) {
                                err(_("Invalid reader timeout value '%d' (should be >= 1).\n"), reader_timeout_ms);
                                usage(argv[0],1);
                        }
                        break;
		case 'C':
			assert(optarg != 0); // optarg can't be NULL (or getopt would choke)
			if (!strcmp(optarg, "never"))
				color_mode = color_never;
			else if (!strcmp(optarg, "auto"))
				color_mode = color_auto;
			else if (!strcmp(optarg, "always"))
				color_mode = color_always;
			else {
				err(_("Invalid option '%s' for -C."), optarg);
				usage(argv[0],1);
			}
			color_errors = color_mode == color_always
				|| (color_mode == color_auto && isatty(STDERR_FILENO)
						&& strcmp(getenv("TERM") ?: "notdumb", "dumb"));
			break;
		case 'M':
			monitor = 1;
			monitor_interval = atoi(optarg);
			if (monitor_interval < 1) {
				err(_("Invalid monitor interval\n"));
			}
			break;
		default:
			usage(argv[0],1);
		}
	}
	if (outfile_name) {
		char tmp[PATH_MAX];
		int ret;
		outfile_name = get_abspath(outfile_name);
		if (outfile_name == NULL) {
			err(_("File name is too long.\n"));
			usage(argv[0],1);
		}
		ret = stap_strfloctime(tmp, PATH_MAX - 21,
                                       /* = _cpuNNNNNN.SSSSSSSSSS */
				       outfile_name, time(NULL));
		if (ret < 0) {
			err(_("Filename format is invalid or too long.\n"));
			usage(argv[0],1);
		}
	}
	if (attach_mod && load_only) {
		err(_("You can't specify the '-A' and '-L' options together.\n"));
		usage(argv[0],1);
	}

	if (attach_mod && buffer_size) {
		err(_("You can't specify the '-A' and '-b' options together.  The '-b'\n"
		    "buffer size option only has an effect when the module is inserted.\n"));
		usage(argv[0],1);
	}

	if (attach_mod && target_cmd) {
		err(_("You can't specify the '-A' and '-c' options together.  The '-c cmd'\n"
		    "option used to start a command only has an effect when the module\n"
		    "is inserted.\n"));
		usage(argv[0],1);
	}

	if (attach_mod && target_pid) {
		err(_("You can't specify the '-A' and '-x' options together.  The '-x pid'\n"
		    "option only has an effect when the module is inserted.\n"));
		usage(argv[0],1);
	}

	if (target_cmd && target_pid) {
		err(_("You can't specify the '-c' and '-x' options together.\n"));
		usage(argv[0],1);
	}

	if (daemon_mode && load_only) {
		err(_("You can't specify the '-D' and '-L' options together.\n"));
		usage(argv[0],1);
	}
	if (daemon_mode && delete_mod) {
		err(_("You can't specify the '-D' and '-d' options together.\n"));
		usage(argv[0],1);
	}
	if (daemon_mode && target_cmd) {
		err(_("You can't specify the '-D' and '-c' options together.\n"));
		usage(argv[0],1);
	}
	if (daemon_mode && outfile_name == NULL) {
		err(_("You have to specify output FILE with '-D' option.\n"));
		usage(argv[0],1);
	}
	if (outfile_name == NULL && fsize_max != 0) {
		err(_("You have to specify output FILE with '-S' option.\n"));
		usage(argv[0],1);
	}
}

void usage(char *prog, int rc)
{
	printf(_("\n%s [-v] [-w] [-V] [-h] [-u] [-c cmd ] [-x pid] [-u user] [-A|-L|-d] [-C WHEN]\n"
                "\t[-b bufsize] [-R] [-r N:URI] [-o FILE [-D] [-S size[,N]]] MODULE [module-options]\n"), prog);
	printf(_("-v              Increase verbosity.\n"
	"-V              Print version number and exit.\n"
	"-h              Print this help text and exit.\n"
	"-w              Suppress warnings.\n"
	"-u              Load uprobes.ko\n"
	"-c cmd          Command \'cmd\' will be run and staprun will\n"
	"                exit when it does.  The '_stp_target' variable\n"
	"                will contain the pid for the command.\n"
	"-x pid          Sets the '_stp_target' variable to pid.\n"
  "-N pid          Sets the '_stp_namespaces_pid' variable to pid.\n"
	"-o FILE         Send output to FILE. This supports strftime(3)\n"
	"                formats for FILE.\n"
	"-b buffer size  The systemtap module specifies a buffer size.\n"
	"                Setting one here will override that value.  The\n"
	"                value should be an integer between 1 and 4095 \n"
	"                which be assumed to be the buffer size in MB.\n"
	"                That value will be per-cpu in bulk mode.\n"
	"-L              Load module and start probes, then detach.\n"
	"-A              Attach to loaded systemtap module.\n"
	"-C WHEN         Enable colored errors. WHEN must be either 'auto',\n"
	"                'never', or 'always'. Set to 'auto' by default.\n"
#ifdef HAVE_MONITOR_LIBS                 
	"-M INTERVAL     Enable monitor mode.\n"
#endif
        "-d              Delete a module.  Only detached or unused modules\n"
	"                the user has permission to access will be deleted. Use \"*\"\n"
	"                (quoted) to delete all unused modules.\n"
        "-R              Have staprun create a new name for the module before\n"
        "                inserting it. This allows the same module to be inserted\n"
        "                more than once.\n"
        "-r N:URI        Pass N:URI data to tapset functions remote_id()/remote_uri().\n"
	"-D              Run in background. This requires '-o' option.\n"
	"-S size[,N]     Switches output file to next file when the size\n"
	"                of file reaches the specified size. The value\n"
	"                should be an integer greater than 1 which is\n"
	"                assumed to be the maximum file size in MB.\n"
	"                When the number of output files reaches N, it\n"
	"                switches to the first output file. You can omit\n"
	"                the second argument.\n"
        "-T timeout      Specifies upper limit on amount of time reader thread\n"
        "                will wait for new full trace buffer. Value should be an\n"
        "                integer >= 1, which is timeout value in ms. Default 200ms.\n"
#ifdef HAVE_OPENAT
        "-F fd           Specifies file descriptor for module relay directory\n"
#endif
	"\n"
	"MODULE can be either a module name or a module path.  If a\n"
	"module name is used, it is searched in the following directory:\n"));
        {
                struct utsname utsbuf;
                int rc = uname (& utsbuf);
                if (! rc)
                        printf("/lib/modules/%s/systemtap\n", utsbuf.release);
                else
                        printf("/lib/modules/`uname -r`/systemtap\n");
        }
	exit(rc);
}

/*
 * parse_modpath.  Here's how this code interprets the global modpath:
 *
 * (1) If modpath contains a '/', it is assumed to be an absolute or
 * relative file path to a module (such as "../foo.ko" or
 * "/tmp/stapXYZ/stap_foo.ko").
 *
 * (2) If modpath doesn't contain a '/' and ends in '.ko', it is a file
 * path to a module in the current directory (such as "foo.ko").
 *
 * (3) If modpath doesn't contain a '/' and doesn't end in '.ko', then
 * it is a module name and the full pathname of the module is
 * '/lib/modules/`uname -r`/systemtap/PATH.ko'.  For instance, if
 * modpath was "foo", the full module pathname would be
 * '/lib/modules/`uname -r`/systemtap/foo.ko'.
 */
void parse_modpath(const char *inpath)
{
	const char *mptr = strrchr(inpath, '/');
	char *ptr;

	dbug(3, "inpath=%s\n", inpath);

	/* If we couldn't find a '/', ... */
	if (mptr == NULL) {
		size_t plen = strlen(inpath);

		/* If the path ends with the '.ko' file extension,
		 * then we've got a module in the current
		 * directory. */
		if (plen > 3 && strcmp(&inpath[plen - 3], ".ko") == 0) {
			mptr = inpath;
			modpath = strdup(inpath);
			if (!modpath) {
				err(_("Memory allocation failed. Exiting.\n"));
				exit(1);
			}
		} else {
			/* If we didn't find the '.ko' file extension, then
			 * we've just got a module name, not a module path.
			 * Look for the module in /lib/modules/`uname
			 * -r`/systemtap. */

			struct utsname utsbuf;
			int len;
			#define MODULE_PATH "/lib/modules/%s/systemtap/%s.ko"

			/* First, we need to figure out what the
			 * kernel version. */
			if (uname(&utsbuf) != 0) {
				perr(_("Unable to determine kernel version, uname failed"));
				exit(-1);
			}

			/* Build the module path, which will look like
			 * '/lib/modules/KVER/systemtap/{path}.ko'. */
			len = sizeof(MODULE_PATH) + sizeof(utsbuf.release) + strlen(inpath);
			modpath = malloc(len);
			if (!modpath) {
				err(_("Memory allocation failed. Exiting.\n"));
				exit(1);
			}
			
			if (snprintf_chk(modpath, len, MODULE_PATH, utsbuf.release, inpath))
				exit(-1);

			dbug(2, "modpath=\"%s\"\n", modpath);

			mptr = strrchr(modpath, '/');
			mptr++;
		}
	} else {
		/* We found a '/', so the module name starts with the next
		 * character. */
		mptr++;

		modpath = strdup(inpath);
		if (!modpath) {
			err(_("Memory allocation failed. Exiting.\n"));
			exit(1);
		}
	}

	modname = strdup(mptr);
	if (!modname) {
		err(_("Memory allocation failed. Exiting.\n"));
		exit(1);
	}

	ptr = strrchr(modname, '.');
	if (ptr)
		*ptr = '\0';

	/* We've finally got a real modname.  Make sure it isn't too
	 * long.  If it is too long, init_module() will appear to
	 * work, but the module can't be removed (because you end up
	 * with control characters in the module name). */
	if (strlen(modname) > MODULE_NAME_LEN) {
		err(_("Module name ('%s') is too long.\n"), modname);
		exit(1);
	}
}

#define ERR_MSG "\nUNEXPECTED FATAL ERROR in staprun. Please file a bug report.\n"
static void fatal_handler (int signum)
{
        int rc;
        char *str = strsignal(signum);
        rc = write (STDERR_FILENO, ERR_MSG, sizeof(ERR_MSG));
        rc = write (STDERR_FILENO, str, strlen(str));
        rc = write (STDERR_FILENO, "\n", 1);
        (void) rc; /* notused */
	_exit(1);
}

void setup_signals(void)
{
	sigset_t s;
	struct sigaction a;

	/* blocking all signals while we set things up */
	sigfillset(&s);
#ifdef SINGLE_THREADED
	sigprocmask(SIG_SETMASK, &s, NULL);
#else
	pthread_sigmask(SIG_SETMASK, &s, NULL);
#endif
	/* set some of them to be ignored */
	memset(&a, 0, sizeof(a));
	sigfillset(&a.sa_mask);
	a.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &a, NULL);
	sigaction(SIGUSR2, &a, NULL);

	/* for serious errors, handle them in fatal_handler */
	a.sa_handler = fatal_handler;
	sigaction(SIGBUS, &a, NULL);
	sigaction(SIGFPE, &a, NULL);
	sigaction(SIGILL, &a, NULL);
	sigaction(SIGSEGV, &a, NULL);
	sigaction(SIGXCPU, &a, NULL);
	sigaction(SIGXFSZ, &a, NULL);

	/* unblock all signals */
	sigemptyset(&s);

#ifdef SINGLE_THREADED
	sigprocmask(SIG_SETMASK, &s, NULL);
#else
	pthread_sigmask(SIG_SETMASK, &s, NULL);
#endif
}

/*
 * set FD_CLOEXEC for any file descriptor
 */
int set_clexec(int fd)
{
	int val;
	if ((val = fcntl(fd, F_GETFD, 0)) < 0)
		goto err;
	
	if ((val = fcntl(fd, F_SETFD, val | FD_CLOEXEC)) < 0)
		goto err;	
	
	return 0;
err:
	perr("fcntl failed");
	close(fd);
	return -1;
}

/*
 * In multithreaded programs, using fcntl() to set F_CLOEXEC
 * (close-on-exec) on a file descriptor is subject to a race condition
 * that another thread may call fork() and exec() between the time the
 * file is opened and fcntl() is called.
 *
 * So, when possible (since Linux 2.6.23), we use the O_CLOEXEC flag
 * when calling open() and openat() to avoid this race condition. When
 * O_CLOEXEC isn't available, the best we can do is call fcntl()
 * immediately after open() is called.
 *
 * The same logic applies to openat() and pipe().
 */

int open_cloexec(const char *pathname, int flags, mode_t mode)
{
#ifdef O_CLOEXEC
	return open(pathname, flags | O_CLOEXEC, mode);
#else
	int fd = open(pathname, flags, mode);
	if (fd >= 0) {
		if (set_clexec(fd) < 0) {
			close(fd);
			fd = -1;
		}
	}
	return fd;
#endif
}

#ifdef HAVE_OPENAT
int openat_cloexec(int dirfd, const char *pathname, int flags, mode_t mode)
{
#ifdef O_CLOEXEC
	return openat(dirfd, pathname, flags | O_CLOEXEC, mode);
#else
	int fd = openat(dirfd, pathname, flags, mode);
	if (fd >= 0) {
		if (set_clexec(fd) < 0) {
			close(fd);
			fd = -1;
		}
	}
	return fd;
#endif
}
#endif

int pipe_cloexec(int pipefd[2])
{
#ifdef O_CLOEXEC
	return pipe2(pipefd, O_CLOEXEC);
#else
	int rc = pipe(pipefd);
	if (rc == 0) {
		if (set_clexec(pipefd[0]) < 0 || set_clexec(pipefd[1] < 0)) {
			goto err;
		}
	}
	return rc;

err:
	close(pipefd[0]);
	close(pipefd[1]);
	pipefd[0] = -1;
	pipefd[1] = -1;
	return -1;
#endif
}

/**
 *      send_request - send request to kernel over control channel
 *      @type: the relay-app command id
 *      @data: pointer to the data to be sent
 *      @len: length of the data to be sent
 *
 *      Returns 0 on success, non-zero otherwise.
 */
int send_request(int type, void *data, int len)
{
	char buf[1024];
        int rc = 0;

	PROBE3(stapio, send__ctlmsg, type, data, len);
	/* Before doing memcpy, make sure 'buf' is big enough. */
	if ((len + sizeof(type)) > (int)sizeof(buf)) {
		_err(_("exceeded maximum send_request size.\n"));
		return -1;
	}
	memcpy(buf, &type, sizeof (type));
	memcpy(&buf[sizeof (type)], data, len);

	errno = 0;
        assert (control_channel >= 0);
	rc = write (control_channel, buf, len + sizeof (type));
        if (rc < 0) return rc;
	/* A bug in the transport layer of older modules causes them to return sizeof (type) fewer
	   bytes written than actual. This is fixed in newer modules. So accept both. */
	return (rc != len && rc != len + (int)sizeof (type));
}

#include <stdarg.h>

static int use_syslog = 0;

void eprintf(const char *fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	if (use_syslog)
		vsyslog(LOG_ERR, fmt, va);
	else
		vfprintf(stderr, fmt, va);
	va_end(va);
}

void switch_syslog(const char *name)
{
	openlog(name, LOG_PID, LOG_DAEMON);
	use_syslog = 1;
	color_errors = 0;
}

void print_color(const char *type)
{
	if (!color_errors)
		return;

	if (type == NULL) // Reset
		eprintf("\033[m\033[K");
	else {
		char *seq = parse_stap_color(type);
		if (seq != NULL) {
			eprintf("\033[");
			eprintf(seq);
			eprintf("m\033[K");
			free(seq);
		}
	}
}

/* Parse SYSTEMTAP_COLORS and returns the SGR parameter(s) for the given
type. The env var SYSTEMTAP_COLORS must be in the following format:
'key1=val1:key2=val2:' etc... where valid keys are 'error', 'warning',
'source', 'caret', 'token' and valid values constitute SGR parameter(s).
For example, the default setting would be:
'error=01;31:warning=00;33:source=00;34:caret=01:token=01'
*/
char *parse_stap_color(const char *type)
{
	const char *key, *col, *eq;
	int n = strlen(type);
	int done = 0;

	key = getenv("SYSTEMTAP_COLORS");
	if (key == NULL)
		key = "error=01;31:warning=00;33:source=00;34:caret=01:token=01";
	else if (*key == '\0')
		return NULL; // disable colors if set but empty

	while (!done) {
		if (!(col = strchr(key, ':'))) {
			col = strchr(key, '\0');
			done = 1;
		}
		if (!((eq = strchr(key, '=')) && eq < col))
			return NULL; /* invalid syntax: no = in range */
		if (!(key < eq && eq < col-1))
			return NULL; /* invalid syntax: key or val empty */
		if (strspn(eq+1, "0123456789;") < (size_t)(col-eq-1))
			return NULL; /* invalid syntax: invalid char in val */
		if (eq-key == n && !strncmp(key, type, n))
			return strndup(eq+1, col-eq-1);
		if (!done) key = col+1; /* advance to next key */
	}

	return NULL; /* key not found */
}

void
closefrom(int lowfd)
{
	long fd, maxfd;
	char *endp;
	struct dirent *dent;
	DIR *dirp;

	/* Check for a /proc/self/fd directory. */
	if ((dirp = opendir("/proc/self/fd"))) {
		int dir_fd = dirfd(dirp);
		while ((dent = readdir(dirp)) != NULL) {
			fd = strtol(dent->d_name, &endp, 10);
			if (dent->d_name != endp && *endp == '\0'
			    && fd >= 0 && fd < INT_MAX && fd >= lowfd
			    && fd != dir_fd)
				(void) close((int)fd);
		}
		(void) closedir(dirp);
	}
	else {
		/*
		 * Here we fall back on sysconf(). Why? It is possible
		 * /proc isn't mounted, we're out of file descriptors,
		 * etc., which could cause the opendir() to fail. Also
		 * note thet it is possible to open a file descriptor
		 * and then drop the rlimit such that it is below the
		 * open fd.
		 */
		maxfd = sysconf(_SC_OPEN_MAX);
		if (maxfd < 0)
			maxfd = OPEN_MAX;

		for (fd = lowfd; fd < maxfd; fd++)
			(void) close((int) fd);
	}
}
