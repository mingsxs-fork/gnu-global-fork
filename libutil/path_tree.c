/*
 * Copyright (c) Ming Li
 * adagio.ming@gmail.com
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
#ifdef HAVE_DIRENT_H
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#endif
#include <stdio.h>
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
#include <ctype.h>
#if defined(_WIN32) && !defined(__CYGWIN__)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef SLIST_ENTRY
#endif

#include "gparam.h"
#include "test.h"
#include "likely.h"
#include "strlimcpy.h"
#include "die.h"
#include "strbuf.h"
#include "find.h"
#include "strhash.h"
#include "path.h"
#include "makepath.h"
#include "checkalloc.h"
#include "locatestring.h"
#include "gpathop.h"
#include "path_tree.h"

/*
 * use an appropriate string comparison for the file system; define the position of the root slash.
 */
#if defined(_WIN32) || defined(__DJGPP__)
#define STRCMP stricmp
#define STRNCMP strnicmp
#define S_ISSOCK(mode) (0)
#else
#define STRCMP strcmp
#define STRNCMP strncmp
#endif

#if (defined(_WIN32) && !defined(__CYGWIN__)) || defined(__DJGPP__)
#define SEP '\\'
#else
#define SEP '/'
#endif

#define HASHBUCKETS 2048
#define VBLOC2ADDR(loc) ((PATH_NODE *)varray_assign(((VARRAY_LOC *)loc)->vb, ((VARRAY_LOC *)loc)->index, 0))
#define VALID_VBLOC(loc) (((VARRAY_LOC *)loc)->vb && ((VARRAY_LOC *)loc)->index >= 0)

/* internal variables */
static int total_accepted_paths = 0;
static const int allow_blank = 1;
static VARRAY_LOC rootpathloc = {0};
static PATH_NODE *rootpath = NULL;
static char *rootrealpath = NULL;
static char *file_parse_states = NULL;
static STRHASH *name_bucket = NULL;

#if 0
/* external variables */
extern int debug;  /* debug flag */
#endif
/* internal function declaration */
#if 0
static inline const char *basename(const char *);
#endif
static inline int check_path(const char *, struct stat *);
static inline int unallowed_symlink(const char *);
static void walk_close_path_tree(PATH_NODE *);
static int walk_build_path_tree(VARRAY_LOC *, STRBUF *);
static void walk_find_proc_path(STRBUF *, PATH_PROC, void *);
static void walk_traverse_path_tree(PATH_NODE *, STRBUF *, PATH_PROC, void *);
static inline void add_childpath(VARRAY_LOC *, const char *);
static inline void update_name_bucket(VARRAY_LOC *);
static const char *construct_path_from_leaf(VARRAY_LOC *);
static const char *commonprefix(const char *, const char *);


int
check_path(const char *fpath, struct stat *stp)
{
	struct stat st, lst;
	if (stat(fpath, &st) < 0) {
		warning("cannot stat path '%s'. ignored.", trimpath(fpath));
		return 0;
	}
	if (S_ISSOCK(st.st_mode) || S_ISFIFO(st.st_mode) || S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode)) {
		warning("file is not regular file '%s'. ignored.", trimpath(fpath));
		return 0;
	}
	if (S_ISDIR(st.st_mode) && unallowed_symlink(fpath)) {
		warning("unallowed symbolic link detected. '%s' is ignored.", trimpath(fpath));
		return 0;
	}
	if (access(fpath, R_OK) < 0) {
		if (!get_skip_unreadable())
			die("cannot read file '%s'.", trimpath(fpath));
		warning("cannot read '%s'. ignored.", trimpath(fpath));
		return 0;
	}
#ifndef __DJGPP__
	if (get_skip_symlink() > 0) {
#if defined(_WIN32) && !defined(__CYGWIN__)
		DWORD attr = GetFileAttributes(fpath);
		if (attr != -1 && (attr & FILE_ATTRIBUTE_REPARSE_POINT))
#else
		if (lstat(fpath, &lst) < 0) {
			warning("cannot lstat '%s'. ignored.", trimpath(fpath));
			return 0;
		}
		if (S_ISLNK(lst.st_mode))
#endif
		{
			if (((get_skip_symlink() & SKIP_SYMLINK_FOR_DIR) && S_ISDIR(st.st_mode)) ||
					((get_skip_symlink() & SKIP_SYMLINK_FOR_FILE) && S_ISREG(st.st_mode)))
			{
				return 0;
			}
		}
	}
#endif
	if (S_ISREG(st.st_mode)) {
		if (skipthisfile(fpath))
			return 0;
		if (!allow_blank && locatestring(fpath, " ", MATCH_FIRST)) {
			warning("'%s' ignored, because it includes blank.", trimpath(fpath));
			return 0;
		}
	}
	if (stp)
		memcpy(stp, &st, sizeof(struct stat));
	return 1;
}

int
unallowed_symlink(const char *dir)
{
	char *real;
	const char *prefix;
	int ret = 0;
	if (!strcmp(dir, ".") || !strcmp(dir, "./"))
		return 0;
	if ((real = realpath(dir, NULL)) == NULL)
		die("can't get real path of '%s'.", trimpath(dir));
	/* dir is not a symlink */
	if (locatestring(real, rootrealpath, MATCH_AT_FIRST) &&
			locatestring(real, trimpath(dir), MATCH_AT_LAST))
		return 0;
	/* check common prefix */
	prefix = commonprefix(rootrealpath, real);
	if (!strcmp(prefix, rootrealpath) || !strcmp(prefix, real)) {
		ret = 1;  /* unallowed symlink found */
	}
	free(real);
	return ret;
}

void
add_childpath(VARRAY_LOC *loc, const char *name)
{
	PATH_NODE *childpath, *parentpath;
	if (!loc || !name)
		return ;
	parentpath = VBLOC2ADDR(loc);
	if (unlikely(!parentpath->childpaths)) {
		parentpath->childpaths = varray_open(sizeof(PATH_NODE), 32);
	}
	childpath = varray_append(parentpath->childpaths);
	/* init child path node */
	strlimcpy(childpath->name, name, sizeof(childpath->name));
	memmove(&childpath->parentloc, loc, sizeof(VARRAY_LOC));
	childpath->childpaths = NULL;
}

void
update_name_bucket(VARRAY_LOC *loc)
{
	VARRAY_LOC *save;
	PATH_NODE *leafpath;
	struct sh_entry *entry;
	if (unlikely(!loc || !name_bucket))
		return ;
	leafpath = VBLOC2ADDR(loc);
	entry = strhash_assign(name_bucket, leafpath->name, 1);
	if (!entry->value)
		entry->value = varray_open(sizeof(VARRAY_LOC), 16);
	save = varray_append((VARRAY *)entry->value);
	memmove(save, loc, sizeof(VARRAY_LOC));
}

int
walk_build_path_tree(VARRAY_LOC *loc, STRBUF *sb)
{
	DIR *dirp;
	struct dirent *dp;
	struct stat st;
	VARRAY_LOC childloc;
	VARRAY *childpaths;

	const char *fpath = strbuf_value(sb);
	if ((dirp = opendir(fpath)) == NULL) {
		warning("cannot open directory '%s'. ignored.", trimpath(fpath));
		return -1;
	}
	strbuf_putc(sb, SEP);
	while ((dp = readdir(dirp)) != NULL) {
		if (ignore_path(dp->d_name))
			continue; /* ignore this file */
		strbuf_puts(sb, dp->d_name);
		if (check_path(strbuf_value(sb), &st)) {
			if (S_ISDIR(st.st_mode) || S_ISREG(st.st_mode)) {
				add_childpath(loc, dp->d_name);
				childpaths = VBLOC2ADDR(loc)->childpaths;
				childloc.vb = childpaths;
				childloc.index = childpaths->length - 1;
				if (S_ISDIR(st.st_mode)) {	/* directory */
					(void)walk_build_path_tree(&childloc, sb);
				} else {	/* regular file */
					total_accepted_paths += 1;  /* accept all regular files */
					update_name_bucket(&childloc);
				}
			}
		}
		strbuf_rtruncate(sb, strlen(dp->d_name));
	}
	(void)closedir(dirp);
	strbuf_rtruncate(sb, 1);  /* truncate SEP */
}

void
path_tree_build(const char *start)
{
	STRBUF *sb = strbuf_open(MAXPATHLEN);
	VARRAY *vb;
	if (!start)
		start = ".";  /* default root path */
	if ((rootrealpath = realpath(start, NULL)) == NULL)
		die("can't get real path of root path '%s'.", trimpath(start));
	if (!check_path(start, NULL))
		die("check root path failed: %s", trimpath(start));
	if (!rootpath) {
		vb = varray_open(sizeof(PATH_NODE), 1);
		rootpath = varray_append(vb);
		memset(rootpath, 0, sizeof(PATH_NODE));
		strlimcpy(rootpath->name, start, sizeof(rootpath->name));
		rootpathloc.vb = vb;
		rootpathloc.index = vb->length - 1;
	}
	if (!name_bucket) {
		name_bucket = strhash_open(HASHBUCKETS);
		if (!name_bucket)
			die("can't create name search hash bucket.");
	}
	/* start building path tree recursively from rootdir */
	strbuf_puts(sb, start);
	if (walk_build_path_tree(&rootpathloc, sb) < 0) {
		path_tree_close();
		die("recursively build path tree failed.");
	}
	/* allocate memory for parse states */
	if (!file_parse_states)
		file_parse_states = check_calloc(sizeof(char), total_accepted_paths);
	strbuf_close(sb);
}

void
walk_close_path_tree(PATH_NODE *path)
{
	PATH_NODE *childpath;
	struct sh_entry *entry;
	VARRAY *vb;
	if (!path) /* NULL path */
		return;
	/* reach leaf node, free the key in search bucket */
	if (!path->childpaths && name_bucket) {
		entry = strhash_pop(name_bucket, path->name);
		if (entry && entry->value) {
			varray_close((VARRAY *)entry->value);
		}
	}
	/* free childpaths first, then free the parentpath */
	if (path->childpaths) {
		vb = path->childpaths;
		for (int i = 0; i < vb->length; i++) {
			childpath = varray_assign(vb, i, 0);
			walk_close_path_tree(childpath);
		}
		varray_close(vb); /* close current childpaths varray */
	}
}

void
path_tree_close(void)
{
	if (rootpath) {
		walk_close_path_tree(rootpath);
		varray_close(rootpathloc.vb);  /* only rootpath node is allocated here */
		memset(&rootpathloc, 0, sizeof(VARRAY_LOC));
		rootpath = NULL;
	}
	if (rootrealpath) {
		free(rootrealpath);
		rootrealpath = NULL;
	}
	if (file_parse_states) {
		free(file_parse_states);
		file_parse_states = NULL;
	}
	if (name_bucket) {
		strhash_close(name_bucket);
		name_bucket = NULL;
	}
	/* reset total accepted paths */
	total_accepted_paths = 0;
	find_close();
}

void
walk_traverse_path_tree(PATH_NODE *path, STRBUF *sb, PATH_PROC proc, void *data)
{
	VARRAY *vb;
	PATH_NODE *childpath;
	if (path != rootpath) /* no root */
		strbuf_putc(sb, SEP);
	strbuf_puts(sb, path->name);
	if (!path->childpaths) {  /* reach end of a path */
		proc(strbuf_value(sb), data);
	} else { /* go deeper in the path tree */
		vb = path->childpaths;
		for (int i = 0; i < vb->length; i++) {
			childpath = varray_assign(vb, i, 0);
			walk_traverse_path_tree(childpath, sb, proc, data);
		}
	}
	strbuf_rtruncate(sb, strlen(path->name));
	strbuf_rtruncate(sb, 1);
}

void
path_tree_traverse(PATH_PROC proc, void *data)
{
	STATIC_STRBUF(sb);
	if (!rootpath)
		die ("build path tree first.");
	if (!proc)
		die ("no proc is given.");
	strbuf_clear(sb);
	walk_traverse_path_tree(rootpath, sb, proc, data);
}

const char *
construct_path_from_leaf(VARRAY_LOC *loc)
{
	STATIC_STRBUF(sb);
	strbuf_clear(sb);
	PATH_NODE *path;
	while (VALID_VBLOC(loc)) {
		path = VBLOC2ADDR(loc);
		strbuf_prepends(sb, path->name);
		if (path != rootpath)
			strbuf_prependc(sb, SEP);
		loc = &path->parentloc;
	}
	return strbuf_value(sb);
}

void
path_tree_search_name(const char *name, PATH_PROC proc, void *data)
{
	const char *fpath;
	struct sh_entry *entry;
	VARRAY *vb;
	VARRAY_LOC *loc;
	if (!proc)
		die("no proc is given.");
	entry = strhash_assign(name_bucket, name, 0);
	if (entry && entry->value) {
		vb = entry->value;
		for (int i = 0; i < vb->length; i++) {
			loc = varray_assign(vb, i, 0);
			fpath = construct_path_from_leaf(loc);
			proc(fpath, data);
		}
	}
}

void
set_file_parse_state(const char *fpath, file_parse_state_e state)
{
	int n_fid;
	n_fid = gpath_path2nfid(fpath, NULL);
	if (n_fid > total_accepted_paths)
		die("fid exceed total accepted files!");
	else if (n_fid == 0)
		warning("cannot find file path in db: %s\n", fpath);
	else
		file_parse_states[n_fid - 1] = (char)state;
}

int
get_file_parse_state(const char *fpath)
{
	int n_fid;
	n_fid = gpath_path2nfid(fpath, NULL);
	if (n_fid > total_accepted_paths)
		die("fid exceed total accepted files!");
	else if (n_fid == 0)  /* file path not found, parse not started yet */
		return FILE_PARSE_NEW;
	else
		return (int)file_parse_states[n_fid - 1];
}

const char *
commonprefix(const char *p1, const char *p2)
{
	STATIC_STRBUF(sb);
	strbuf_clear(sb);
	while (*p1 && *p1 == *p2++)
		strbuf_putc(sb, *p1++);
	return strbuf_value(sb);
}

int
path_tree_no_proc(const char *path, void *data)
{
	printf("%s\n", path);
	return 0;
}

void
walk_find_proc_path(STRBUF *sb, PATH_PROC proc, void *data)
{
	DIR *dirp;
	struct dirent *dp;
	struct stat st;
	const char *fpath = strbuf_value(sb);
	if ((dirp = opendir(fpath)) == NULL) {
		warning("cannot open directory '%s'. ignored.", trimpath(fpath));
		return;
	}
	strbuf_putc(sb, SEP);
	while ((dp = readdir(dirp)) != NULL) {
		if (ignore_path(dp->d_name))
			continue; /* ignore this file */
		strbuf_puts(sb, dp->d_name);
		if (check_path(strbuf_value(sb), &st)) {
			if (S_ISDIR(st.st_mode))
				(void)walk_find_proc_path(sb, proc, data);
			else if (S_ISREG(st.st_mode)) {
				total_accepted_paths += 1;
				/* process found path with given handler */
				(void)proc(strbuf_value(sb), data);
			}
		}
		strbuf_rtruncate(sb, strlen(dp->d_name)); /* truncate dp->d_name */
	}
	(void)closedir(dirp);
	strbuf_rtruncate(sb, 1);  /* truncate SEP */
}

void
find_proc_path_all(const char *start, PATH_PROC proc, void *data)
{
	STRBUF *sb;
	if (!proc)
		die("no path handler, work given up.");
	if (!start)
		start = ".";
	if (!check_path(start, NULL))
		die("check root path failed: %s", trimpath(start));
	if ((rootrealpath = realpath(start, NULL)) == NULL)
		die("can't get real path of root path '%s'.", trimpath(start));
	sb = strbuf_open(MAXPATHLEN);
	strbuf_puts(sb, start);
	walk_find_proc_path(sb, proc, data);
	strbuf_close(sb);
	find_close();
}

void
find_proc_filelist(const char *filename, const char *root, PATH_PROC proc, void *data)
{
	static FILE *temp = NULL;
	STRBUF *ib = strbuf_open(0);
	FILE *ip = NULL;
	char buf[MAXPATHLEN + 2];
	const char *path;
	/* we reserve 2 bytes ahead for changing from absolute path to relative path */
	char *real = &buf[2];
	char *ploc;
	int rootlen;

	if (!proc)
		die("no path handler, work given up.");

	if (!strcmp(filename, "-")) {
		/*
		 * If the filename is '-', copy standard input onto
		 * temporary file to be able to read repeatedly.
		 */
		if (temp == NULL) {
			temp = tmpfile();
			while (fgets(buf, sizeof(buf), stdin) != NULL)
				fputs(buf, temp);
		}
		rewind(temp);
		ip = temp;
	} else {
		ip = fopen(filename, "r");
		if (ip == NULL)
			die("cannot open '%s'.", trimpath(filename));
	}

	if ((rootrealpath = realpath(root, NULL)) == NULL)
		die("can't get real path of root: %s.", root);
	/*
	 * The buf has room for one character ' ' in front.
	 *
	 * __buf
	 * +---+---+---+---+   +---+
	 * |' '|   |   |   .....   |
	 * +---+---+---+---+   +---+
	 *     ^
	 *     buf
	 *      <---  bufsize   --->
	 */
	strbuf_clear(ib);
	memset(buf, 0, sizeof(buf));
	rootlen = strlen(rootrealpath);
	for (;;) {
		path = strbuf_fgets(ib, ip, STRBUF_NOCRLF);
		if (path == NULL) {
			/* EOF */
			break;
		}
		if (*path == '\0') {
			/* skip empty line.  */
			continue;
		}
		/*
		 * Lines which start with ". " are considered to be comments.
		 */
		if (*path == '.' && *(path + 1) == ' ')
			continue;
		/*
		 * Skip the following:
		 * o directory
		 * o file which does not exist
		 * o dead symbolic link
		 */
		if (!test("f", path)) {
			if (test("d", path))
				warning("'%s' is a directory. ignored.", trimpath(path));
			else
				warning("'%s' not found. ignored.", trimpath(path));
			continue;
		}
		/*
		 * normalize path name.
		 *
		 *	rootdir  /a/b/
		 *	buf      /a/b/c/d.c -> c/d.c -> ./c/d.c
		 */
		if (realpath(path, real) == NULL) {
			warning("can't get real path of: %s, ignored.", path);
			continue;
		}
		if ((ploc = locatestring(real, rootrealpath, MATCH_AT_FIRST)) == NULL)
			continue;
		if (*ploc-- != '/') {
			*ploc-- = '/';
		}
		*ploc = '.';
		path = ploc;
		/*
		 * Now GLOBAL can treat the path which includes blanks.
		 * This message is obsoleted.
		 */
		if (!allow_blank && locatestring(path, " ", MATCH_LAST)) {
			warning("'%s' ignored, because it includes blank.", trimpath(path));
			continue;
		}
		if (skipthisfile(path))
			continue;
		/* handle path */
		(void)proc(path, data);
	}
	fclose(ip);
	strbuf_close(ib);
	find_close();
}
