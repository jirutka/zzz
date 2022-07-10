// vim: set ts=4:
// Copyright 2021 - present, Jakub Jirutka <jakub@jirutka.cz>.
// SPDX-License-Identifier: MIT
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define PROGNAME         "zzz"

#ifndef VERSION
  #define VERSION        "0.1.1"
#endif

#ifndef ZZZ_HOOKS_DIR
  #define ZZZ_HOOKS_DIR  "/etc/zzz.d"
#endif

#ifndef ZZZ_LOCK_FILE
  #define ZZZ_LOCK_FILE  "/tmp/zzz.lock"
#endif

#define FLAG_VERBOSE     0x0001

#define ERR_GENERAL       1
#define ERR_WRONG_USAGE  10
#define ERR_UNSUPPORTED  11
#define ERR_LOCK         12
#define ERR_SUSPEND      20
#define ERR_HOOK         21

#define RC_OK             0
#define RC_ERR           -1

#define log_err(format, ...) \
	do { \
		fprintf(stderr, PROGNAME ": ERROR: " format "\n", __VA_ARGS__); \
		syslog(LOG_ERR, format, __VA_ARGS__); \
	} while (0)

#define log_errno(format, ...) \
	log_err(format ": %s", __VA_ARGS__, strerror(errno))

#define log_info(format, ...) \
	do { \
		syslog(LOG_INFO, format, __VA_ARGS__); \
		printf(format "\n", __VA_ARGS__); \
	} while (0)

#define log_debug(format, ...) \
	if (flags & FLAG_VERBOSE) { \
		syslog(LOG_DEBUG, format, __VA_ARGS__); \
		printf(format "\n", __VA_ARGS__); \
	}


extern char **environ;

static const char *HELP_MSG =
	"Usage: " PROGNAME " [-v] [-n|s|S|z|Z|H|R|V|h]\n"
	"\n"
	"Suspend or hibernate the system.\n"
	"\n"
	"Options:\n"
	"  -n   Dry run (sleep for 5s instead of suspend/hibernate).\n"
	"  -s   Low-power idle (ACPI S1).\n"
	"  -S   Deprecated alias for -s.\n"
	"  -z   Suspend to RAM (ACPI S3). [default for zzz(8)]\n"
	"  -Z   Hibernate to disk & power off (ACPI S4). [default for ZZZ(8)]\n"
	"  -H   Hibernate to disk & suspend (aka suspend-hybrid).\n"
	"  -R   Hibernate to disk & reboot.\n"
	"  -v   Be verbose.\n"
	"  -V   Print program name & version and exit.\n"
	"  -h   Show this message and exit.\n"
	"\n"
	"Homepage: https://github.com/jirutka/zzz\n";

static unsigned int flags = 0;


static bool str_empty (const char *str) {
	return str == NULL || str[0] == '\0';
}

static bool str_equal (const char *str1, const char *str2) {
	return strcmp(str1, str2) == 0;
}

static bool str_ends_with (const char *str, const char *suffix) {
	int diff = strlen(str) - strlen(suffix);
	return diff > 0 && str_equal(&str[diff], suffix);
}

static int run_hook (const char *path, const char **argv) {
	posix_spawn_file_actions_t factions;
	int rc = RC_ERR;

	(void) posix_spawn_file_actions_init(&factions);
	(void) posix_spawn_file_actions_addclose(&factions, STDIN_FILENO);

	log_debug("Executing hook script: %s", path);

	argv[0] = path;  // XXX: mutates input argument!
	pid_t pid;
	if ((rc = posix_spawn(&pid, path, &factions, 0, (char *const *)argv, environ)) != 0) {
		log_errno("Unable to execute hook script %s", path);
		goto done;
	}

	int status;
	(void) waitpid(pid, &status, 0);

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		rc = WEXITSTATUS(status);
		log_err("Hook script %s exited with code %d", path, rc);
		goto done;
	}
	rc = RC_OK;

done:
	posix_spawn_file_actions_destroy(&factions);
	argv[0] = NULL;  // XXX: mutates input argument!

	return rc;
}

static int filter_hook_script (const struct dirent *entry) {
	struct stat sb;

	return stat(entry->d_name, &sb) >= 0
		&& S_ISREG(sb.st_mode)     // is regular file
		&& sb.st_mode & S_IXUSR    // is executable by owner
		&& !(sb.st_mode & S_IWOTH) // is not writable by others
		&& sb.st_uid == 0;         // is owned by root
}

static int run_hooks (const char *dirpath, const char **argv) {
	struct dirent **entries = NULL;
	int rc = RC_OK;

	if (chdir(dirpath) < 0) {
		errno = 0;
		goto done;
	}
	int n = 0;
	if ((n = scandir(".", &entries, filter_hook_script, alphasort)) < 0) {
		log_errno("%s", dirpath);
		rc = RC_ERR;
		goto done;
	}

	char path[PATH_MAX] = "\0";
	for (int i = 0; i < n; i++) {
		(void) snprintf(path, sizeof(path), "%s/%s", dirpath, entries[i]->d_name);

		if (run_hook(path, argv) != RC_OK) {
			rc = RC_ERR;
		}
	}

done:
	if (entries) free(entries);
	(void) chdir("/");

	return rc;
}

static int check_sleep_mode (const char *filepath, const char *mode) {
	FILE *fp = NULL;
	int rc = RC_ERR;

	if (access(filepath, W_OK) < 0) {
		log_errno("%s", filepath);
		goto done;
	}

	char line[64] = "\0";
	if ((fp = fopen(filepath, "r")) == NULL || fgets(line, sizeof(line), fp) == NULL) {
		log_errno("Failed to read %s", filepath);
		goto done;
	}

	// XXX: This is sloppy...
	if (strstr(line, mode) != NULL) {
		rc = RC_OK;
	}

done:
	if (fp) fclose(fp);
	return rc;
}

static int file_write (const char *filepath, const char *data) {
	FILE *fp = NULL;
	int rc = RC_ERR;

	if ((fp = fopen(filepath, "w")) == NULL) {
		log_errno("%s", filepath);
		goto done;
	}
	(void) setvbuf(fp, NULL, _IONBF, 0);  // disable buffering

	if (fputs(data, fp) < 0) {
		log_errno("%s", filepath);
		goto done;
	}
	rc = RC_OK;

done:
	if (fp) fclose(fp);
	return rc;
}

static int execute (const char* zzz_mode, const char* sleep_state, const char* hibernate_mode) {
	int lock_fd = -1;
	int rc = EXIT_SUCCESS;

	// Check if we can fulfil the request.
	if (!str_empty(sleep_state) && check_sleep_mode("/sys/power/state", sleep_state) < 0) {
		return ERR_UNSUPPORTED;
	}
	if (!str_empty(hibernate_mode) && check_sleep_mode("/sys/power/disk", hibernate_mode) < 0) {
		return ERR_UNSUPPORTED;
	}

	// Obtain exclusive lock.
	if ((lock_fd = open(ZZZ_LOCK_FILE, O_CREAT | O_RDWR | O_CLOEXEC, 0600)) < 0) {
		log_errno("Failed to write %s", ZZZ_LOCK_FILE);
		return ERR_LOCK;
	}
	if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
		log_err("%s", "Another instance of zzz is running");
		return ERR_LOCK;
	}

	// The first element will be replaced in run_hook() with the script path.
	const char *hook_args[] = { NULL, "pre", zzz_mode, NULL };

	if (run_hooks(ZZZ_HOOKS_DIR, hook_args) < 0) {
		rc = ERR_HOOK;
	}
	// For compatibility with zzz on Void Linux.
	if (run_hooks(ZZZ_HOOKS_DIR "/suspend", hook_args) < 0) {
		rc = ERR_HOOK;
	}

	if (!str_empty(hibernate_mode) && file_write("/sys/power/disk", hibernate_mode) < 0) {
		rc = ERR_SUSPEND;
		goto done;
	}

	log_info("Going to %s (%s)", zzz_mode, sleep_state);

	if (str_empty(sleep_state)) {
		sleep(5);

	} else if (file_write("/sys/power/state", sleep_state) < 0) {
		log_err("Failed to %s system", zzz_mode);
		rc = ERR_SUSPEND;
		goto done;
	}

	log_info("System resumed from %s (%s)", zzz_mode, sleep_state);

	hook_args[1] = "post";
	if (run_hooks(ZZZ_HOOKS_DIR, hook_args) < 0) {
		rc = ERR_HOOK;
	}
	// For compatibility with zzz on Void Linux.
	if (run_hooks(ZZZ_HOOKS_DIR "/resume", hook_args) < 0) {
		rc = ERR_HOOK;
	}

done:
	// Release lock.
	if (unlink(ZZZ_LOCK_FILE) < 0) {
		log_errno("Failed to remove lock file %s", ZZZ_LOCK_FILE);
		rc = ERR_GENERAL;
	}
	if (flock(lock_fd, LOCK_UN) < 0) {
		log_errno("Failed to release lock on %s", ZZZ_LOCK_FILE);
		rc = ERR_GENERAL;
	}
	(void) close(lock_fd);

	return rc;
}

int main (int argc, char **argv) {
	char *zzz_mode = "suspend";
	char *sleep_state = "mem";
	char *hibernate_mode = "";

	if (str_equal(argv[0], "ZZZ") || str_ends_with(argv[0], "/ZZZ")) {
		zzz_mode = "hibernate";
		sleep_state = "disk";
		hibernate_mode = "platform";
	}

	int optch;
	opterr = 0;  // don't print implicit error message on unrecognized option
	while ((optch = getopt(argc, argv, "nsSzHRZvhV")) != -1) {
		switch (optch) {
			case -1:
				break;
			case 'n':
				zzz_mode = "noop";
				sleep_state = "";
				hibernate_mode = "";
				break;
			case 's':
			case 'S':
				zzz_mode = "standby";
				sleep_state = "freeze";
				hibernate_mode = "";
				break;
			case 'z':
				zzz_mode = "suspend";
				sleep_state = "mem";
				hibernate_mode = "";
				break;
			case 'H':
				zzz_mode = "hibernate";
				sleep_state = "disk";
				hibernate_mode = "suspend";
				break;
			case 'R':
				zzz_mode = "hibernate";
				sleep_state = "disk";
				hibernate_mode = "reboot";
				break;
			case 'Z':
				zzz_mode = "hibernate";
				sleep_state = "disk";
				hibernate_mode = "platform";
				break;
			case 'v':
				flags |= FLAG_VERBOSE;
				break;
			case 'h':
				printf("%s", HELP_MSG);
				return EXIT_SUCCESS;
			case 'V':
				puts(PROGNAME " " VERSION);
				return EXIT_SUCCESS;
			default:
				fprintf(stderr, "%1$s: Invalid option: -%2$c (see %1$s -h)\n", PROGNAME, optopt);
				return ERR_WRONG_USAGE;
		}
	}

	(void) chdir("/");

	// Clear environment variables.
	environ = NULL;

	// Set environment for hooks.
	setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
	setenv("ZZZ_MODE", zzz_mode, 1);
	setenv("ZZZ_HIBERNATE_MODE", hibernate_mode, 1);

	// Open connection to syslog.
	openlog(PROGNAME, LOG_PID, LOG_USER);

	return execute(zzz_mode, sleep_state, hibernate_mode);
}
