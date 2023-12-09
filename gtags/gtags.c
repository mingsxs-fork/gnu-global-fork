/*
 * Copyright (c) 1997, 1998, 1999, 2000, 2001, 2002, 2003, 2005, 2006, 2008,
 *	2009, 2010, 2012, 2014, 2015, 2016, 2018, 2020
 *	Tama Communications Corporation
 *
 * This file is part of GNU GLOBAL.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <ctype.h>
#include <utime.h>
#include <signal.h>
#include <stdio.h>
#include <errno.h>
#if TIME_WITH_SYS_TIME
#include <sys/time.h>
#include <time.h>
#else
#if HAVE_SYS_TIME_H
#include <sys/time.h>
#else
#include <time.h>
#endif
#endif
#ifdef STDC_HEADERS
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#else
#include <strings.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include "getopt.h"

#include "global.h"
#include "parser.h"
#include "const.h"

/*
 * enable [set] globbing, if available
 */
#ifdef __CRT_GLOB_BRACKET_GROUPS__
int _CRT_glob = __CRT_GLOB_USE_MINGW__ | __CRT_GLOB_BRACKET_GROUPS__;
#endif

/*
 gtags - create tag files for global.
*/

static void usage(void);
static void help(void);
int printconf(const char *);
int main(int, char **);
int incremental(const char *, const char *);
void updatetags(const char *, const char *, IDSET *, STRBUF *);
void createtags(const char *, const char *);

int cflag;					/**< compact format */
int iflag;					/**< incremental update */
int Iflag;					/**< make  idutils index */
int Oflag;					/**< use objdir */
int qflag;					/**< quiet mode */
int wflag;					/**< warning message */
int vflag;					/**< verbose mode */
int show_version;
int show_help;
int show_config;
int skip_unreadable;
int skip_symlink;
int accept_dotfiles;
char *gtagsconf;
char *gtagslabel;
int debug;
const char *config_name;
const char *file_list;
const char *dump_target;
char *single_update;
int statistics = STATISTICS_STYLE_NONE;
int explain;
struct gtags_priv_data g_priv_data;  /* global gtags private data */
#ifdef USE_SQLITE3
int use_sqlite3;
#endif

#define GTAGSFILES "gtags.files"

int extractmethod;
int total;

static void
usage(void)
{
	if (!qflag)
		fputs(usage_const, stderr);
	exit(2);
}
static void
help(void)
{
	fputs(usage_const, stdout);
	fputs(help_const, stdout);
	exit(0);
}
/**
 * printconf: print configuration data.
 *
 *	@param[in]	name	label of config data
 *	@return		exit code
 */
int
printconf(const char *name)
{
	int num;
	int exist = 1;

	if (getconfn(name, &num))
		fprintf(stdout, "%d\n", num);
	else if (getconfb(name))
		fprintf(stdout, "1\n");
	else {
		STRBUF *sb = strbuf_open(0);
		if (getconfs(name, sb))
			fprintf(stdout, "%s\n", strbuf_value(sb));
		else
			exist = 0;
		strbuf_close(sb);
	}
	return exist;
}

const char *short_options = "cC:d:f:iIn:oOqvwse";
struct option const long_options[] = {
	/*
	 * These options have long name and short name.
	 * We throw them to the processing of short options.
	 *
	 * Though the -o(--omit-gsyms) was removed, this code
	 * is left for compatibility.
	 */
	{"compact", no_argument, NULL, 'c'},
	{"directory", required_argument, NULL, 'C'},
	{"dump", required_argument, NULL, 'd'},
	{"file", required_argument, NULL, 'f'},
	{"idutils", no_argument, NULL, 'I'},
	{"incremental", no_argument, NULL, 'i'},
	{"max-args", required_argument, NULL, 'n'},
	{"omit-gsyms", no_argument, NULL, 'o'},		/* removed */
	{"objdir", no_argument, NULL, 'O'},
	{"quiet", no_argument, NULL, 'q'},
	{"verbose", no_argument, NULL, 'v'},
	{"warning", no_argument, NULL, 'w'},

	/*
	 * The following are long name only.
	 */
#define OPT_CONFIG		128
#define OPT_PATH		129
#define OPT_SINGLE_UPDATE	130
#define OPT_ACCEPT_DOTFILES	131
#define OPT_SKIP_UNREADABLE	132
#define OPT_GTAGSSKIP_SYMLINK	133
	/* flag value */
	{"accept-dotfiles", no_argument, NULL, OPT_ACCEPT_DOTFILES},
	{"debug", no_argument, &debug, 1},
	{"explain", no_argument, &explain, 1},
#ifdef USE_SQLITE3
	{"sqlite3", no_argument, &use_sqlite3, 1},
#endif
	{"skip-unreadable", no_argument, NULL, OPT_SKIP_UNREADABLE},
	{"statistics", no_argument, &statistics, STATISTICS_STYLE_TABLE},
	{"version", no_argument, &show_version, 1},
	{"help", no_argument, &show_help, 1},

	/* accept value */
	{"config", optional_argument, NULL, OPT_CONFIG},
	{"gtagsconf", required_argument, NULL, OPT_GTAGSCONF},
	{"gtagslabel", required_argument, NULL, OPT_GTAGSLABEL},
	{"skip-symlink", optional_argument, NULL, OPT_GTAGSSKIP_SYMLINK},
	{"path", required_argument, NULL, OPT_PATH},
	{"single-update", required_argument, NULL, OPT_SINGLE_UPDATE},
	{ 0 }
};

static const char *langmap = DEFAULTLANGMAP;	/**< langmap */
static const char *gtags_parser;		/**< gtags_parser */
/**
 * load configuration variables.
 */
static void
configuration()
{
	STRBUF *sb = strbuf_open(0);

	if (getconfb("extractmethod"))
		extractmethod = 1;
	strbuf_reset(sb);
	if (getconfs("langmap", sb))
		langmap = check_strdup(strbuf_value(sb));
	strbuf_reset(sb);
	if (getconfs("gtags_parser", sb))
		gtags_parser = check_strdup(strbuf_value(sb));
	strbuf_close(sb);
}
int
main(int argc, char **argv)
{
	char dbpath[MAXPATHLEN];
	char cwd[MAXPATHLEN];
	int optchar;
	int option_index = 0;
	STATISTICS_TIME *tim;
	STRBUF *sb = strbuf_open(0);

	/*
	 * pick up --gtagsconf, --gtagslabel and --directory (-C).
	 */
	if (preparse_options(argc, argv) < 0)
		usage();
	/*
	 * Get the project root directory.
	 */
	if (!vgetcwd(cwd, MAXPATHLEN))
		die("cannot get current directory.");
	canonpath(cwd);
	/*
	 * Load configuration file.
	 */
	openconf(cwd);
	configuration();
	setenv_from_config();
	{
		char *env = getenv("GTAGS_OPTIONS");
		if (env && *env)
			argv = prepend_options(&argc, argv, env);
	}
	/*
	 * Execute gtags_hook before the jobs.
	 * The hook is not executed when the following options are specified.
         *	--help, --dump, --config, --version
	 * These are informational options only.
	 */
	{
		int skip_hook = 0;

		/* Make a decision whether to execute gtags_hook. */
		while ((optchar = getopt_long(argc, argv,
			short_options, long_options, &option_index)) != EOF) {
			switch (optchar) {
			case OPT_CONFIG:
			case 'd':
				skip_hook++;
				break;
			default:
				break;
			}
		}
		optind = 1;		/* Reset getopt(3) library. */
		if (show_version || show_help)
			skip_hook++;
		if (skip_hook) {
			if (debug)
				fprintf(stderr, "Gtags_hook is skipped.\n");
		} else if (getconfs("gtags_hook", sb)) {
			char *p = serialize_options(argc, argv);
			set_env("GTAGS_COMMANDLINE", p);
			free(p);
			if (system(strbuf_value(sb)))
				fprintf(stderr, "gtags-hook failed: %s\n", strbuf_value(sb));
		}
	}
	logging_arguments(argc, argv);
	while ((optchar = getopt_long(argc, argv, short_options, long_options, &option_index)) != EOF) {
		switch (optchar) {
		case 0:
			/* already flags set */
			break;
		case OPT_CONFIG:
			show_config = 1;
			if (optarg)
				config_name = optarg;
			break;
		case OPT_GTAGSCONF:
		case OPT_GTAGSLABEL:
		case 'C':
			/* These options are already parsed in preparse_options(). */
			break;
		case OPT_SINGLE_UPDATE:
			iflag++;
			single_update = optarg;
			break;
		case OPT_ACCEPT_DOTFILES:
			accept_dotfiles = 1;
			break;
		case OPT_SKIP_UNREADABLE:
			skip_unreadable = 1;
			break;
		case OPT_GTAGSSKIP_SYMLINK:
			skip_symlink = SKIP_SYMLINK_FOR_ALL;
			if (optarg) {
				if (!strcmp(optarg, "f"))
					skip_symlink = SKIP_SYMLINK_FOR_FILE;
				else if (!strcmp(optarg, "d"))
					skip_symlink = SKIP_SYMLINK_FOR_DIR;
				else if (!strcmp(optarg, "a"))
					skip_symlink = SKIP_SYMLINK_FOR_ALL;
				else
					die("--skip-symlink: %s: unknown type.", optarg);
			}
			break;
		case 'c':
			cflag++;
			break;
		case 'd':
			dump_target = optarg;
			break;
		case 'f':
			file_list = optarg;
			break;
		case 'i':
			iflag++;
			break;
		case 'I':
			Iflag++;
			break;
		case 'o':
			/*
			 * Though the -o(--omit-gsyms) was removed, this code
			 * is left for compatibility.
			 */
			break;
		case 'O':
			Oflag++;
			break;
		case 'q':
			qflag++;
			break;
		case 'w':
			wflag++;
			break;
		case 'v':
			vflag++;
			break;
		default:
			usage();
			break;
		}
	}
	if (skip_symlink)
		set_skip_symlink(skip_symlink);
	if (qflag) {
		vflag = 0;
		setquiet();
	}
	if (vflag)
		setverbose();
	if (wflag)
		set_langmap_wflag();
	if (show_version)
		version(NULL, vflag);
	if (show_help)
		help();

	argc -= optind;
        argv += optind;

	/* If dbpath is specified, -O(--objdir) option is ignored. */
	if (argc > 0)
		Oflag = 0;
	if (show_config) {
		openconf(setupdbpath(0) == 0 ? get_root() : NULL);
		if (config_name)
			printconf(config_name);
		else
			fprintf(stdout, "%s\n", getconfline());
		exit(0);
	} else if (dump_target) {
		/*
		 * Dump a tag file.
		 */
		DBOP *dbop = NULL;
		const char *dat = 0;
		int is_gpath = 0;

		if (!test("f", dump_target))
			die("file '%s' not found.", dump_target);
		if ((dbop = dbop_open(dump_target, 0, 0, DBOP_RAW)) == NULL)
			die("file '%s' is not a tag file.", dump_target);
		/*
		 * The file which has a NEXTKEY record is GPATH.
		 */
		if (dbop_get(dbop, NEXTKEY))
			is_gpath = 1;
		for (dat = dbop_first(dbop, NULL, NULL, 0); dat != NULL; dat = dbop_next(dbop)) {
			const char *flag = is_gpath ? dbop_getflag(dbop) : "";

			if (*flag)
				printf("%s\t%s\t%s\n", dbop->lastkey, dat, flag);
			else
				printf("%s\t%s\n", dbop->lastkey, dat);
		}
		dbop_close(dbop);
		exit(0);
	} else if (Iflag) {
#define REQUIRED_MKID_VERSION "4.5"
		char *p;

		if (!usable("mkid"))
			die("mkid not found.");
		if (read_first_line("mkid --version", sb))
			die("mkid cannot executed.");
		p = strrchr(strbuf_value(sb), ' ');
		if (p == NULL)
			die("invalid version string of mkid: %s", strbuf_value(sb));
		switch (check_version(p + 1, REQUIRED_MKID_VERSION)
#ifdef _WIN32
			| strcmp(p + 1, "3.2.99") == 0
#endif
			)  {
		case 1:		break;	/* OK */
		case 0:		die("mkid version %s or later is required.", REQUIRED_MKID_VERSION);
		default:	die("invalid version string of mkid: %s", strbuf_value(sb));
		}
	}

	/*
	 * If 'gtags.files' exists, use it as a file list.
	 * If the file_list other than "-" is given, it must be readable file.
	 */
	if (file_list == NULL && test("f", GTAGSFILES))
		file_list = GTAGSFILES;
	if (file_list && strcmp(file_list, "-")) {
		if (test("d", file_list))
			die("'%s' is a directory.", file_list);
		else if (!test("f", file_list))
			die("'%s' not found.", file_list);
		else if (!test("r", file_list))
			die("'%s' is not readable.", file_list);
	}
	/*
	 * Regularize the path name for single updating (--single-update).
	 */
	if (single_update) {
		static char regular_path_name[MAXPATHLEN];
		char *p = single_update;
		
#if _WIN32 || __DJGPP__
		for (; *p; p++)
			if (*p == '\\')
				*p = '/';
		p = single_update;
#define LOCATEFLAG MATCH_AT_FIRST|IGNORE_CASE
#else
#define LOCATEFLAG MATCH_AT_FIRST
#endif
		if (isabspath(p)) {
			char *q = locatestring(p, cwd, LOCATEFLAG);

			if (q && *q == '/')
				snprintf(regular_path_name, MAXPATHLEN, "./%s", q + 1);
			else
				die("path '%s' is out of the project.", p);

		} else {
			if (p[0] == '.' && p[1] == '/')
				snprintf(regular_path_name, MAXPATHLEN, "%s", p);
			else
				snprintf(regular_path_name, MAXPATHLEN, "./%s", p);
		}
		single_update = regular_path_name;
	}
	/*
	 * Decide directory (dbpath) in which gtags make tag files.
	 *
	 * Gtags create tag files at current directory by default.
	 * If dbpath is specified as an argument then use it.
	 * If the -i option specified and both GTAGS and GRTAGS exists
	 * at one of the candidate directories then gtags use existing
	 * tag files.
	 */
	if (iflag) {
		if (argc > 0) {
			if (realpath(*argv, dbpath) == NULL)
				die("invalid dbpath given: %s.", *argv);
		} else if (!gtagsexist(cwd, dbpath, MAXPATHLEN, vflag))
			strlimcpy(dbpath, cwd, sizeof(dbpath));
	} else {
		if (argc > 0) {
			if (realpath(*argv, dbpath) == NULL)
				die("invalid dbpath given: %s.", *argv);
		} else if (Oflag) {
			char *objdir = getobjdir(cwd, vflag);

			if (objdir == NULL)
				die("Objdir not found.");
			strlimcpy(dbpath, objdir, sizeof(dbpath));
		} else
			strlimcpy(dbpath, cwd, sizeof(dbpath));
	}
	if (iflag && (!test("f", makepath(dbpath, dbname(GTAGS), NULL)) ||
		!test("f", makepath(dbpath, dbname(GRTAGS), NULL)) ||
		!test("f", makepath(dbpath, dbname(GPATH), NULL)))) {
		if (wflag)
			warning("GTAGS, GRTAGS or GPATH not found. -i option ignored.");
		iflag = 0;
	}
	if (!test("d", dbpath))
		die("directory '%s' not found.", dbpath);
	/*
	 * Start processing.
	 */
	if (accept_dotfiles)
		set_accept_dotfiles();
	if (skip_unreadable)
		set_skip_unreadable();
	if (vflag) {
		const char *config_path = getconfigpath();
		const char *config_label = getconfiglabel();

		fprintf(stderr, "[%s] Gtags started.\n", now());
		if (config_path)
			fprintf(stderr, " Using configuration file '%s'.\n", config_path);
		else {
			fprintf(stderr, " Using default configuration.\n");
			if (getenv("GTAGSLABEL"))
				fprintf(stderr, " GTAGSLABEL(--gtagslabel) ignored since configuration file not found.\n");
		}
		if (config_label)
			fprintf(stderr, " Using configuration label '%s'.\n", config_label);
		if (file_list)
			fprintf(stderr, " Using '%s' as a file list.\n", file_list);
	}
	/*
	 * initialize parser.
	 */
	if (vflag && gtags_parser)
		fprintf(stderr, " Using plug-in parser.\n");
	parser_init(langmap, gtags_parser);
	/*
	 * Start statistics.
	 */
	init_statistics();
	/*
	 * incremental update.
	 */
	if (iflag) {
		/*
		 * Version check. If existing tag files are old enough
		 * gtagsopen() abort with error message.
		 */
		GTOP *gtop = gtags_open(dbpath, cwd, GTAGS, GTAGS_MODIFY, 0);
		gtags_close(gtop);
		/*
		 * GPATH is needed for incremental updating.
		 * Gtags check whether or not GPATH exist, since it may be
		 * removed by mistake.
		 */
		if (!test("f", makepath(dbpath, dbname(GPATH), NULL)))
			die("Old version tag file found. Please remake it.");
		(void)incremental(dbpath, cwd);
		print_statistics(statistics);
		exit(0);
	}
	/*
	 * create GTAGS and GRTAGS
	 */
	createtags(dbpath, cwd);
	/*
	 * create idutils index.
	 */
	if (Iflag) {
		FILE *op;
		GFIND *gp;
		const char *path;

		tim = statistics_time_start("Time of creating ID");
		if (vflag)
			fprintf(stderr, "[%s] Creating indexes for idutils.\n", now());
		strbuf_reset(sb);
		/*
		 * Since idutils stores the value of PWD in ID file, we need to
		 * force idutils to follow our style.
		 */
#if _WIN32 || __DJGPP__
		strbuf_puts(sb, "mkid --files0-from=-");
#else
		strbuf_sprintf(sb, "PWD=%s mkid --files0-from=-", quote_shell(cwd));
#endif
		if (vflag)
			strbuf_puts(sb, " -v");
		strbuf_sprintf(sb, " --file=%s/ID", quote_shell(dbpath));
		if (vflag) {
#ifdef __DJGPP__
			if (is_unixy())	/* test for 4DOS as well? */
#endif
			strbuf_puts(sb, " 1>&2");
		} else {
			strbuf_puts(sb, " >" NULL_DEVICE);
#ifdef __DJGPP__
			if (is_unixy())	/* test for 4DOS as well? */
#endif
			strbuf_puts(sb, " 2>&1");
		}
		if (debug)
			fprintf(stderr, "executing mkid like: %s\n", strbuf_value(sb));
		op = popen(strbuf_value(sb), "w");
		if (op == NULL)
			die("cannot execute '%s'.", strbuf_value(sb));
		gp = gfind_open(dbpath, NULL, GPATH_BOTH, 0);
		while ((path = gfind_read(gp)) != NULL) {
			fputs(path, op);
			fputc('\0', op);
		}
		gfind_close(gp);
		if (pclose(op) != 0)
			die("terminated abnormally '%s' (errno = %d).", strbuf_value(sb), errno);
		if (test("f", makepath(dbpath, "ID", NULL)))
			if (chmod(makepath(dbpath, "ID", NULL), 0644) < 0)
				die("cannot chmod ID file.");
		statistics_time_end(tim);
	}
	if (vflag)
		fprintf(stderr, "[%s] Done.\n", now());
	closeconf();
	strbuf_close(sb);
	print_statistics(statistics);
	static_strbuf_free();
	return 0;
}
/**
 * incremental: incremental update
 *
 *	@param[in]	dbpath	dbpath directory
 *	@param[in]	root	root directory of source tree
 *	@return		0: not updated, 1: updated
 */
int
incremental(const char *dbpath, const char *root)
{
	STATISTICS_TIME *tim;
	struct stat statp;
	STRBUF *addlist = strbuf_open(0);
	STRBUF *deletelist = strbuf_open(0);
	STRBUF *addlist_other = strbuf_open(0);
	IDSET *deleteset, *findset;
	int updated = 0;
	const char *path;
	unsigned int id, limit;
	struct gtags_path_add_data add_data;
	/* init gtags private data */
	memset(&g_priv_data, 0, sizeof(g_priv_data));
	g_priv_data.gconf.vflag =			!!vflag;
	g_priv_data.gconf.wflag =			!!wflag;
	g_priv_data.gconf.qflag =			!!qflag;
	g_priv_data.gconf.debug =			!!debug;
	g_priv_data.gconf.iflag =			!!iflag;
	g_priv_data.gconf.extractmethod =	!!extractmethod;
	g_priv_data.gconf.explain =			!!explain;
	g_priv_data.gconf.incremental =		!!1;

	tim = statistics_time_start("Time of inspecting %s and %s.", dbname(GTAGS), dbname(GRTAGS));
	if (vflag) {
		fprintf(stderr, " Tag found in '%s'.\n", dbpath);
		fprintf(stderr, " Incremental updating.\n");
	}
	/*
	 * get modified time of GTAGS.
	 */
	path = makepath(dbpath, dbname(GTAGS), NULL);
	if (stat(path, &statp) < 0)
		die("stat failed '%s'.", path);

	if (gpath_open(dbpath, 2) < 0)
		die("GPATH not found.");
	/*
	 * deleteset:
	 *	The list of the path name which should be deleted from GPATH.
	 * findset:
	 *	The list of the path name which exists in the current project.
	 *	A project is limited by the --file option.
	 */
	deleteset = idset_open(gpath_nextkey());
	findset = idset_open(gpath_nextkey());
	total = 0;
	/*
	 * Make add list and delete list for update.
	 */
	if (single_update) {
		int type;
		const char *fid;

		if (skipthisfile(single_update))
			goto exit;
		if (test("b", single_update))
			goto exit;
		fid = gpath_path2fid(single_update, &type);
		if (fid == NULL) {
			/* new file */
			if (!test("f", single_update))
				die("'%s' not found.", single_update);
			type = issourcefile(single_update) ? GPATH_SOURCE : GPATH_OTHER;
			if (type == GPATH_OTHER)
				strbuf_puts0(addlist_other, single_update);
			else {
				strbuf_puts0(addlist, single_update);
				total++;
			}
		} else if (!test("f", single_update)) {
			/* delete */
			if (type != GPATH_OTHER) {
				idset_add(deleteset, atoi(fid));
				total++;
			}
			strbuf_puts0(deletelist, single_update);
		} else {
			/* update */
			if (type == GPATH_OTHER)
				goto exit;
			idset_add(deleteset, atoi(fid));
			strbuf_puts0(addlist, single_update);
			total++;
		}
	} else {
		/* init path handler params */
		add_data.addlist = addlist;
		add_data.dellist = deletelist;
		add_data.addlist_other = addlist_other;
		add_data.findset = findset;
		add_data.delset = deleteset;
		add_data.gtag_sb = &statp;
		add_data.path_sb = NULL;
		g_priv_data.npath_done = &total;
		g_priv_data.add_data = &add_data;
		if (file_list)
			find_proc_filelist(file_list, root, gtags_add_path, &g_priv_data);
		else
			find_proc_path_all(NULL, gtags_add_path, &g_priv_data);
		/*
		 * make delete list.
		 */
		limit = gpath_nextkey();
		for (id = 1; id < limit; id++) {
			char fid[MAXFIDLEN];
			int type;

			snprintf(fid, sizeof(fid), "%d", id);
			/*
			 * This is a hole of GPATH. The hole increases if the deletion
			 * and the addition are repeated.
			 */
			if ((path = gpath_fid2path(fid, &type)) == NULL)
				continue;
			/*
			 * The file which does not exist in the findset is treated
			 * assuming that it does not exist in the file system.
			 */
			if (type == GPATH_OTHER) {
				if (!idset_contains(findset, id) || !test("f", path) || test("b", path))
					strbuf_puts0(deletelist, path);
			} else {
				if (!idset_contains(findset, id) || !test("f", path)) {
					strbuf_puts0(deletelist, path);
					idset_add(deleteset, id);
				}
			}
		}
	}
	statistics_time_end(tim);
	/*
	 * execute updating.
	 */
	if ((!idset_empty(deleteset) || strbuf_getlen(addlist) > 0) ||
	    (strbuf_getlen(deletelist) + strbuf_getlen(addlist_other) > 0))
	{
		int db;
		updated = 1;
		tim = statistics_time_start("Time of updating %s and %s.", dbname(GTAGS), dbname(GRTAGS));
		if (!idset_empty(deleteset) || strbuf_getlen(addlist) > 0)
			updatetags(dbpath, root, deleteset, addlist);
		if (strbuf_getlen(deletelist) + strbuf_getlen(addlist_other) > 0) {
			const char *start, *end, *p;

			if (vflag)
				fprintf(stderr, "[%s] Updating '%s'.\n", now(), dbname(GPATH));
			/* gpath_open(dbpath, 2); */
			if (strbuf_getlen(deletelist) > 0) {
				start = strbuf_value(deletelist);
				end = start + strbuf_getlen(deletelist);

				for (p = start; p < end; p += strlen(p) + 1)
					gpath_delete(p);
			}
			if (strbuf_getlen(addlist_other) > 0) {
				start = strbuf_value(addlist_other);
				end = start + strbuf_getlen(addlist_other);

				for (p = start; p < end; p += strlen(p) + 1) {
					gpath_put(p, GPATH_OTHER);
				}
			}
			/* gpath_close(); */
		}
		/*
		 * Update modification time of tag files
		 * because they may have no definitions.
		 */
		for (db = GTAGS; db < GTAGLIM; db++)
			utime(makepath(dbpath, dbname(db), NULL), NULL);
		statistics_time_end(tim);
	}
exit:
	if (vflag) {
		if (updated)
			fprintf(stderr, " Global databases have been modified.\n");
		else
			fprintf(stderr, " Global databases are up to date.\n");
		fprintf(stderr, "[%s] Done.\n", now());
	}
	strbuf_close(addlist);
	strbuf_close(deletelist);
	strbuf_close(addlist_other);
	gpath_close();
	idset_close(deleteset);
	idset_close(findset);

	return updated;
}
/**
 * updatetags: update tag file.
 *
 *	@param[in]	dbpath		directory in which tag file exist
 *	@param[in]	root		root directory of source tree
 *	@param[in]	deleteset	bit array of fid of deleted or modified files 
 *	@param[in]	addlist		'\0' separated list of added or modified files
 */
void
updatetags(const char *dbpath, const char *root, IDSET *deleteset, STRBUF *addlist)
{
	int seqno;
	const char *path, *start, *end;
	struct gtags_path gpath;
	struct gtags_path_proc_data proc_data;
	g_priv_data.gpath = &gpath;
	g_priv_data.proc_data = &proc_data;

	if (vflag)
		fprintf(stderr, "[%s] Updating '%s' and '%s'.\n", now(), dbname(GTAGS), dbname(GRTAGS));
	/*
	 * Open tag files.
	 */
	proc_data.gtop[GTAGS] = gtags_open(dbpath, root, GTAGS, GTAGS_MODIFY, 0);
	if (test("f", makepath(dbpath, dbname(GRTAGS), NULL))) {
		proc_data.gtop[GRTAGS] = gtags_open(dbpath, root, GRTAGS, GTAGS_MODIFY, 0);
	} else {
		/*
		 * If you set NULL to proc_data.gtop[GRTAGS], parse_file() doesn't write to
		 * GRTAGS. See gtags_put_symbol().
		 */
		proc_data.gtop[GRTAGS] = NULL;
	}
	/*
	 * Delete tags from GTAGS.
	 */
	if (!idset_empty(deleteset)) {
		if (vflag) {
			char fid[MAXFIDLEN];
			int total = idset_count(deleteset);
			unsigned int id;

			seqno = 1;
			for (id = idset_first(deleteset); id != END_OF_ID; id = idset_next(deleteset)) {
				snprintf(fid, sizeof(fid), "%d", id);
				path = gpath_fid2path(fid, NULL);
				if (path == NULL)
					die("GPATH is corrupted.");
				fprintf(stderr, " [%d/%d] deleting tags of %s\n", seqno++, total, path + 2);
			}
		}
		gtags_delete(proc_data.gtop[GTAGS], deleteset);
		if (proc_data.gtop[GRTAGS] != NULL)
			gtags_delete(proc_data.gtop[GRTAGS], deleteset);
	}
	/*
	 * Set flags.
	 */
	proc_data.gtop[GTAGS]->flags = 0;
	if (extractmethod)
		proc_data.gtop[GTAGS]->flags |= GTAGS_EXTRACTMETHOD;
	proc_data.gtop[GRTAGS]->flags = proc_data.gtop[GTAGS]->flags;
	if (vflag)
		g_priv_data.gconf.parser_flags |= PARSER_VERBOSE;
	if (debug)
		g_priv_data.gconf.parser_flags |= PARSER_DEBUG;
	if (wflag)
		g_priv_data.gconf.parser_flags |= PARSER_WARNING;
	if (explain)
		g_priv_data.gconf.parser_flags |= PARSER_EXPLAIN;
	if (getenv("GTAGSFORCEENDBLOCK"))
		g_priv_data.gconf.parser_flags |= PARSER_END_BLOCK;
	/*
	 * Add tags to GTAGS and GRTAGS.
	 */
	start = strbuf_value(addlist);
	end = start + strbuf_getlen(addlist);
	seqno = 0;
	for (path = start; path < end; path += strlen(path) + 1) {
		strlimcpy(gpath.path, path, sizeof(gpath.path));
		gpath_put(gpath.path, GPATH_SOURCE);
		gpath.fid = gpath_path2fid(gpath.path, NULL);
		if (gpath.fid == NULL)
			die("GPATH is corrupted.('%s' not found)", gpath.path);
		gpath.seq = ++seqno;
		if (vflag)
			fprintf(stderr, " [%d/%d] extracting tags of %s\n", seqno, total, trimpath(gpath.path));
		parse_file(&gpath, g_priv_data.gconf.parser_flags, gtags_put_symbol, &g_priv_data);
		gtags_flush(proc_data.gtop[GTAGS], gpath.fid);
		if (proc_data.gtop[GRTAGS] != NULL)
			gtags_flush(proc_data.gtop[GRTAGS], gpath.fid);
	}
	parser_exit();
	gtags_close(proc_data.gtop[GTAGS]);
	if (proc_data.gtop[GRTAGS] != NULL)
		gtags_close(proc_data.gtop[GRTAGS]);
}
/**
 * createtags: create tags file
 *
 *	@param[in]	dbpath	dbpath directory
 *	@param[in]	root	root directory of source tree
 */
void
createtags(const char *dbpath, const char *root)
{
	STATISTICS_TIME *tim;
	STRBUF *sb = strbuf_open(0);
	int openflags;
	struct gtags_path_proc_data proc_data;
	/* init gtags private data */
	memset(&g_priv_data, 0, sizeof(g_priv_data));
	g_priv_data.proc_data = &proc_data;
	g_priv_data.gconf.vflag =			!!vflag;
	g_priv_data.gconf.wflag = 			!!wflag;
	g_priv_data.gconf.qflag = 			!!qflag;
	g_priv_data.gconf.debug = 			!!debug;
	g_priv_data.gconf.iflag = 			!!iflag;
	g_priv_data.gconf.extractmethod =	!!extractmethod;
	g_priv_data.gconf.explain =			!!explain;
	g_priv_data.npath_done = &total;

	tim = statistics_time_start("Time of creating %s and %s.", dbname(GTAGS), dbname(GRTAGS));
	if (vflag)
		fprintf(stderr, "[%s] Creating '%s' and '%s'.\n", now(), dbname(GTAGS), dbname(GRTAGS));
	openflags = cflag ? GTAGS_COMPACT : 0;
#ifdef USE_SQLITE3
	if (use_sqlite3)
		openflags |= GTAGS_SQLITE3;
#endif
	proc_data.gtop[GTAGS] = gtags_open(dbpath, root, GTAGS, GTAGS_CREATE, openflags);
	proc_data.gtop[GTAGS]->flags = 0;
	if (extractmethod)
		proc_data.gtop[GTAGS]->flags |= GTAGS_EXTRACTMETHOD;
	proc_data.gtop[GRTAGS] = gtags_open(dbpath, root, GRTAGS, GTAGS_CREATE, openflags);
	proc_data.gtop[GRTAGS]->flags = proc_data.gtop[GTAGS]->flags;
	g_priv_data.gconf.parser_flags = 0;
	if (vflag)
		g_priv_data.gconf.parser_flags |= PARSER_VERBOSE;
	if (debug)
		g_priv_data.gconf.parser_flags |= PARSER_DEBUG;
	if (wflag)
		g_priv_data.gconf.parser_flags |= PARSER_WARNING;
	if (explain)
		g_priv_data.gconf.parser_flags |= PARSER_EXPLAIN;
	if (getenv("GTAGSFORCEENDBLOCK"))
		g_priv_data.gconf.parser_flags |= PARSER_END_BLOCK;
	/*
	 * Add tags to GTAGS and GRTAGS.
	 */
	if (file_list)
		find_proc_filelist(file_list, root, gtags_proc_path, &g_priv_data);
	else
		find_proc_path_all(NULL, gtags_proc_path, &g_priv_data);
	/* start parsing source files */
	gtags_close(proc_data.gtop[GTAGS]);
	gtags_close(proc_data.gtop[GRTAGS]);
	statistics_time_end(tim);
	strbuf_reset(sb);
	if (getconfs("GTAGS_extra", sb)) {
		tim = statistics_time_start("Time of executing GTAGS_extra command");
		if (system(strbuf_value(sb)))
			fprintf(stderr, "GTAGS_extra command failed: %s\n", strbuf_value(sb));
		statistics_time_end(tim);
	}
	strbuf_reset(sb);
	if (getconfs("GRTAGS_extra", sb)) {
		tim = statistics_time_start("Time of executing GRTAGS_extra command");
		if (system(strbuf_value(sb)))
			fprintf(stderr, "GRTAGS_extra command failed: %s\n", strbuf_value(sb));
		statistics_time_end(tim);
	}
	strbuf_close(sb);
}
