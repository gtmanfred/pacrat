#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <locale.h>
#include <getopt.h>
#include <sys/stat.h>
#include <dirent.h>

#include <alpm.h>

/* macros {{{ */
#define STREQ(x,y)            (strcmp((x),(y)) == 0)

#ifndef PACMAN_ROOT
	#define PACMAN_ROOT "/"
#endif
#ifndef PACMAN_DB
	#define PACMAN_DBPATH "/var/lib/pacman"
#endif

#define NC          "\033[0m"
#define BOLD        "\033[1m"

#define BLACK       "\033[0;30m"
#define RED         "\033[0;31m"
#define GREEN       "\033[0;32m"
#define YELLOW      "\033[0;33m"
#define BLUE        "\033[0;34m"
#define MAGENTA     "\033[0;35m"
#define CYAN        "\033[0;36m"
#define WHITE       "\033[0;37m"
#define BOLDBLACK   "\033[1;30m"
#define BOLDRED     "\033[1;31m"
#define BOLDGREEN   "\033[1;32m"
#define BOLDYELLOW  "\033[1;33m"
#define BOLDBLUE    "\033[1;34m"
#define BOLDMAGENTA "\033[1;35m"
#define BOLDCYAN    "\033[1;36m"
#define BOLDWHITE   "\033[1;37m"
/* }}} */

typedef enum __loglevel_t {
	LOG_INFO    = 1,
	LOG_ERROR   = (1 << 1),
	LOG_WARN    = (1 << 2),
	LOG_DEBUG   = (1 << 3),
	LOG_VERBOSE = (1 << 4),
	LOG_BRIEF   = (1 << 5)
} loglevel_t;

typedef enum __operation_t {
	OP_LIST = 1,
	OP_PULL = (1 << 1),
	OP_PUSH = (1 << 2)
} operation_t;

enum {
	CONF_PACNEW  = 1,
	CONF_PACSAVE = (1 << 1),
	CONF_PACORIG = (1 << 2)
};

enum {
	OP_DEBUG = 1000
};

typedef struct __file_t {
	char *path;
	char *hash;
} file_t;

typedef struct __backup_t {
	const char *pkgname;
	file_t system;
	file_t local;
	const char *hash;
} backup_t;

typedef struct __strings_t {
	const char *error;
	const char *warn;
	const char *info;
	const char *pkg;
	const char *nc;
} strings_t;

static int cwr_fprintf(FILE *, loglevel_t, const char *, ...) __attribute__((format(printf,3,4)));
static int cwr_printf(loglevel_t, const char *, ...) __attribute__((format(printf,2,3)));
static int cwr_vfprintf(FILE *, loglevel_t, const char *, va_list) __attribute__((format(printf,3,0)));
static void copy(const char *, const char *);
static void mkpath(const char *, mode_t);
static void archive(const backup_t *);
static char *get_hash(const char *);
static void file_init(file_t *, const char *, char *);
static int check_pacfiles(const char *);
static alpm_list_t *alpm_find_backups(alpm_pkg_t *, int);
static alpm_list_t *alpm_all_backups(int);
static int parse_options(int, char*[]);
static int strings_init(void);
static void print_status(backup_t *);
static void usage(void);
static void version(void);
static void free_backup(void *);

/* runtime configuration */
static struct {
	operation_t opmask;
	loglevel_t logmask;

	short color;
	int all : 1;

	alpm_list_t *targets;
} cfg;

alpm_handle_t *pmhandle;
strings_t *colstr;

int cwr_fprintf(FILE *stream, loglevel_t level, const char *format, ...) /* {{{ */
{
	int ret;
	va_list args;

	va_start(args, format);
	ret = cwr_vfprintf(stream, level, format, args);
	va_end(args);

	return ret;
} /* }}} */

int cwr_printf(loglevel_t level, const char *format, ...) /* {{{ */
{
	int ret;
	va_list args;

	va_start(args, format);
	ret = cwr_vfprintf(stdout, level, format, args);
	va_end(args);

	return ret;
} /* }}} */

int cwr_vfprintf(FILE *stream, loglevel_t level, const char *format, va_list args) /* {{{ */
{
	const char *prefix;
	char bufout[128];

	if (!(cfg.logmask & level))
		return 0;

	switch (level) {
		case LOG_VERBOSE:
		case LOG_INFO:
			prefix = colstr->info;
			break;
		case LOG_ERROR:
			prefix = colstr->error;
			break;
		case LOG_WARN:
			prefix = colstr->warn;
			break;
		case LOG_DEBUG:
			prefix = "debug:";
			break;
		default:
			prefix = "";
			break;
	}

	/* f.l.w.: 128 should be big enough... */
	snprintf(bufout, 128, "%s %s", prefix, format);

	return vfprintf(stream, bufout, args);
} /* }}} */

void copy(const char *src, const char *dest) /* {{{ */
{
	struct stat st;
	stat(src, &st);

	int in  = open(src, O_RDONLY);
	int out = open(dest, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode);
	char buf[8192];

	ssize_t ret;
	while ((ret = read(in, buf, sizeof(buf))) > 0) {
		if (write(out, buf, (size_t)ret) != ret) {
			perror("write");
			exit(EXIT_FAILURE);
		}
	}

	close(in);
	close(out);
} /* }}} */

void mkpath(const char *path, mode_t mode) /* {{{ */
{
	struct stat st;

	if (stat(path, &st) != 0) {
		if (mkdir(path, mode) != 0) {
			perror("mkdir");
			exit(EXIT_FAILURE);
		}
	}
	else if (!S_ISDIR(st.st_mode)) {
		perror("stat");
		exit(EXIT_FAILURE);
	}
} /* }}} */

void archive(const backup_t *backup) /* {{{ */
{
	struct stat st;
	char dest[PATH_MAX];
	char *ptr, *root;
	snprintf(dest, PATH_MAX, "%s%s", backup->pkgname, backup->system.path);

	root = dest + strlen(backup->pkgname);

	for (ptr = dest + 1; *ptr; ptr++) {
		if (*ptr == '/') {
			*ptr = '\0';

			int mode = 0777;
			if (ptr > root) {
				if (stat(root, &st) != 0)
					perror("stat");
				mode = st.st_mode;
			}
			mkpath(dest, mode);

			*ptr = '/';
		}
	}

	copy(backup->system.path, dest);
} /* }}} */

char *get_hash(const char *path) /* {{{ */
{
	char *hash = alpm_compute_md5sum(path);
	if(!hash) {
		cwr_fprintf(stderr, LOG_ERROR, "failed to compute hash for %s\n", path);
		exit(EXIT_FAILURE);
	}
	return hash;
} /* }}} */

void file_init(file_t *file, const char *path, char *hash) /* {{{ */
{
	file->hash = hash ? hash : get_hash(path);
	file->path = strdup(path);
} /* }}} */

int check_pacfiles(const char *file) /* {{{ */
{
	char path[PATH_MAX];
	int ret = 0;

	snprintf(path, PATH_MAX, "%s.pacnew", file);
	if (access(path, R_OK) == 0)
		ret |= CONF_PACNEW;

	snprintf(path, PATH_MAX, "%s.pacsave", file);
	if (access(path, R_OK) == 0)
		ret |= CONF_PACSAVE;

	snprintf(path, PATH_MAX, "%s.pacorig", file);
	if (access(path, R_OK) == 0)
		ret |= CONF_PACORIG;

	return ret;
} /* }}} */

alpm_list_t *alpm_find_backups(alpm_pkg_t *pkg, int everything) /* {{{ */
{
	alpm_list_t *backups = NULL;
	const alpm_list_t *i;
	struct stat st;
	char path[PATH_MAX];

	const char *pkgname = alpm_pkg_get_name(pkg);

	for (i = alpm_pkg_get_backup(pkg); i; i = i->next) {
		const alpm_backup_t *backup = i->data;

		snprintf(path, PATH_MAX, "%s%s", PACMAN_ROOT, backup->name);

		/* check if we can access the file */
		if (access(path, R_OK) != 0) {
			cwr_fprintf(stderr, LOG_WARN, "can't access %s\n", path);
			continue;
		}

		/* check if there is a pacnew/pacsave/pacorig file */
		int pacfiles = check_pacfiles(path);
		if (pacfiles & CONF_PACNEW)
			cwr_fprintf(stderr, LOG_WARN, "pacnew file detected %s\n", path);
		if (pacfiles & CONF_PACSAVE)
			cwr_fprintf(stderr, LOG_WARN, "pacsave file detected %s\n", path);
		if (pacfiles & CONF_PACORIG)
			cwr_fprintf(stderr, LOG_WARN, "pacorig file detected %s\n", path);

		/* filter unmodified files */
		char *hash = get_hash(path);
		if (!everything && STREQ(backup->hash, hash)) {
			free(hash);
			continue;
		}

		cwr_fprintf(stderr, LOG_DEBUG, "found backup: %s\n", path);

		/* mark the file to be operated on then */
		backup_t *b = malloc(sizeof(backup_t));
		memset(b, 0, sizeof(backup_t));

		b->pkgname = pkgname;
		b->hash = backup->hash;
		file_init(&b->system, path, hash);

		/* look for a local copy */
		snprintf(path, PATH_MAX, "%s/%s", pkgname, backup->name);

		size_t status = stat(path, &st);
		if (status == 0 && S_ISREG (st.st_mode)) {
			cwr_fprintf(stderr, LOG_DEBUG, "found local copy: %s\n", path);
			file_init(&b->local, path, NULL);
		}

		backups = alpm_list_add(backups, b);
	}

	return backups;
} /* }}} */

alpm_list_t *alpm_all_backups(int everything) /* {{{ */
{
	alpm_list_t *backups = NULL;
	const alpm_list_t *i;

	alpm_db_t *db = alpm_get_localdb(pmhandle);
	alpm_list_t *targets = cfg.targets ? alpm_db_search(db, cfg.targets) : alpm_db_get_pkgcache(db);

	for (i = targets; i; i = i->next) {
		alpm_list_t *pkg_backups = alpm_find_backups(i->data, everything);
		backups = alpm_list_join(backups, pkg_backups);
	}

	return backups;
} /* }}} */

int parse_options(int argc, char *argv[]) /* {{{ */
{
	int opt, option_index = 0;

	static const struct option opts[] = {
		/* operations */
		{"pull",    no_argument,       0, 'p'},
		{"list",    no_argument,       0, 'l'},

		/* options */
		{"all",     no_argument,       0, 'a'},
		{"color",   optional_argument, 0, 'c'},
		{"debug",   no_argument,       0, OP_DEBUG},
		{"help",    no_argument,       0, 'h'},
		{"verbose", no_argument,       0, 'v'},
		{"version", no_argument,       0, 'V'},
		{0, 0, 0, 0}
	};

	while ((opt = getopt_long(argc, argv, "plac:hvV", opts, &option_index)) != -1) {
		switch(opt) {
			case 'p':
				cfg.opmask |= OP_PULL;
				break;
			case 'l':
				cfg.opmask |= OP_LIST;
				break;
			case 'a':
				cfg.all |= 1;
				break;
			case 'c':
				if (!optarg || STREQ(optarg, "auto")) {
					if (isatty(fileno(stdout))) {
						cfg.color = 1;
					} else {
						cfg.color = 0;
					}
				} else if (STREQ(optarg, "always")) {
					cfg.color = 1;
				} else if (STREQ(optarg, "never")) {
					cfg.color = 0;
				} else {
					fprintf(stderr, "invalid argument to --color\n");
					return 1;
				}
				break;
			case 'h':
				usage();
				return 1;
			case 'v':
				cfg.logmask |= LOG_VERBOSE;
				break;
			case 'V':
				version();
				return 2;
			case OP_DEBUG:
				cfg.logmask |= LOG_DEBUG;
				break;
			default:
				return 1;
		}
	}

#define NOT_EXCL(val) (cfg.opmask & (val) && (cfg.opmask & ~(val)))

	/* check for invalid operation combos */
	if (NOT_EXCL(OP_LIST) || NOT_EXCL(OP_PULL) || NOT_EXCL(OP_PUSH)) {
		fprintf(stderr, "error: invalid operation\n");
		return 2;
	}

	while (optind < argc) {
		if (!alpm_list_find_str(cfg.targets, argv[optind])) {
			cwr_fprintf(stderr, LOG_DEBUG, "adding target: %s\n", argv[optind]);
			cfg.targets = alpm_list_add(cfg.targets, strdup(argv[optind]));
		}
		optind++;
	}

	return 0;
} /* }}} */

int strings_init(void) /* {{{ */
{
	colstr = malloc(sizeof(strings_t));
	if (!colstr)
		return 1;

	if (cfg.color > 0) {
		colstr->error = BOLDRED "::" NC;
		colstr->warn  = BOLDYELLOW "::" NC;
		colstr->info  = BOLDBLUE "::" NC;
		colstr->pkg   = BOLD;
		colstr->nc    = NC;
	} else {
		colstr->error = "error:";
		colstr->warn  = "warning:";
		colstr->info  = "::";
		colstr->pkg   = "";
		colstr->nc    = "";
	}

	return 0;
} /* }}} */

void print_status(backup_t *b) /* {{{ */
{
	/* printf("%s %s%s%s %s\n", colstr->info, colstr->pkg, b->pkgname, colstr->nc, b->path); */
	printf("%s%s%s %s\n", colstr->pkg, b->pkgname, colstr->nc, b->system.path);
	if (!b->local.path) {
		printf("  file not locally tracked\n");
	} else if (!STREQ(b->system.hash, b->local.hash)) {
		printf("  %s hashes don't match!\n", colstr->warn);
		printf("     %s\n     %s\n", b->system.hash, b->local.hash);
	}
} /* }}} */

void usage(void) /* {{{ */
{
	fprintf(stderr, "pacrat %s\n"
			"Usage: pacrat <operations> [options]...\n\n", PACRAT_VERSION);
	fprintf(stderr,
			" Operations:\n"
			"  -u, --update            check for updates against AUR -- can be combined "
			"with the -d flag\n\n");
	fprintf(stderr, " General options:\n"
			"  -h, --help              display this help and exit\n"
			"  -V, --version           display version\n\n");
	fprintf(stderr, " Output options:\n"
			"  -c, --color[=WHEN]      use colored output. WHEN is `never', `always', or `auto'\n"
			"      --debug             show debug output\n"
			"  -v, --verbose           output more\n\n");
} /* }}} */

void version(void) /* {{{ */
{
	printf("\n " PACRAT_VERSION "\n");
	printf("     \\   (\\,/)\n"
		   "      \\  oo   '''//,        _\n"
		   "       ,/_;~,       \\,     / '\n"
		   "       \"'   \\    (    \\    !\n"
		   "             ',|  \\    |__.'\n"
		   "             '~  '~----''\n"
		   "\n"
		   "             Pacrat....\n\n");
} /* }}} */

void free_backup(void *ptr) { /* {{{ */
	backup_t *backup = ptr;
	free(backup->system.path);
	free(backup->system.hash);
	free(backup->local.path);
	free(backup->local.hash);
	free(backup);
} /* }}} */

int main(int argc, char *argv[])
{
	int ret;
	enum _alpm_errno_t err;

	setlocale(LC_ALL, "");

	cfg.logmask = LOG_ERROR | LOG_WARN | LOG_INFO;

	if ((ret = parse_options(argc, argv)) != 0)
		return ret;

	cwr_fprintf(stderr, LOG_DEBUG, "initializing alpm\n");
	pmhandle = alpm_initialize(PACMAN_ROOT, PACMAN_DBPATH, &err);
	if (!pmhandle) {
		cwr_fprintf(stderr, LOG_ERROR, "failed to initialize alpm library\n");
		goto finish;
	}

	if ((ret = strings_init()) != 0) {
		return ret;
	}

	if (cfg.opmask & OP_LIST) {
		alpm_list_t *backups = alpm_all_backups(cfg.all), *i;
		for (i = backups; i; i = i->next)
			print_status(i->data);
		alpm_list_free_inner(backups, free_backup);
		alpm_list_free(backups);
	} else if (cfg.opmask & OP_PULL) {
		alpm_list_t *backups = alpm_all_backups(cfg.all), *i;
		for (i = backups; i; i = i->next)
			archive(i->data);
		alpm_list_free_inner(backups, free_backup);
		alpm_list_free(backups);
	}

finish:
	alpm_release(pmhandle);
	return ret;
}
/* vim: set ts=4 sw=4 noet: */
