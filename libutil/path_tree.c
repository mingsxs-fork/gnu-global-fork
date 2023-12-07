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

#include "find.h"
#include "makepath.h"
#include "checkalloc.h"
#include "path.h"
#include "die.h"
#include "strlimcpy.h"
#include "test.h"
#include "locatestring.h"
#include "strbuf.h"
#include "strhash.h"
#include "gpathop.h"
#include "path_tree.h"
#include "likely.h"

/*
 * use an appropriate string comparison for the file system; define the position of the root slash.
 */
#if defined(_WIN32) || defined(__DJGPP__)
#define STRCMP stricmp
#define STRNCMP strnicmp
#define ROOT 2
#define S_ISSOCK(mode) (0)
#else
#define STRCMP strcmp
#define STRNCMP strncmp
#define ROOT 0
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
static inline int check_path(const char *);
static inline int unallowed_symlink(const char *);
static void walk_free_path_tree(PATH_NODE *);
static int walk_make_path_tree(VARRAY_LOC *, STRBUF *);
static int walk_traverse_path_tree(PATH_NODE *, STRBUF *, int (*handler)(const char *, void *), void *);
static inline void add_childpath(VARRAY_LOC *, const char *);
static inline void update_name_bucket(VARRAY_LOC *);
static const char *construct_path_from_leaf(VARRAY_LOC *);
static const char *commonprefix(const char *, const char *);


int
check_path(const char *fpath)
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
		die("cannot get real path of '%s'.", trimpath(dir));
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
walk_make_path_tree(VARRAY_LOC *loc, STRBUF *sb)
{
	DIR *dirp;
	struct dirent *dp;
	struct stat st;
	VARRAY_LOC childloc;
	VARRAY *childpaths;
	int ret;

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
		if (check_path(strbuf_value(sb))) {
			(void)stat(strbuf_value(sb), &st);
			if (S_ISDIR(st.st_mode) || S_ISREG(st.st_mode)) {
				add_childpath(loc, dp->d_name);
				childpaths = VBLOC2ADDR(loc)->childpaths;
				childloc.vb = childpaths;
				childloc.index = childpaths->length - 1;
				if (S_ISDIR(st.st_mode)) {	/* directory */
					if ((ret = walk_make_path_tree(&childloc, sb)) != 0)
						return ret;
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
	return 0;  /* current directory build done */
}

void
make_path_tree(const char *start)
{
	STRBUF *sb = strbuf_open(0);
	VARRAY *vb;
	if (!start)
		start = ".";  /* default root path */
	if ((rootrealpath = realpath(start, NULL)) == NULL)
		die("can't get real path of root path '%s'.", trimpath(start));
	if (!check_path(start))
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
	if (walk_make_path_tree(&rootpathloc, sb) < 0) {
		free_path_tree();
		die("recursively build path tree failed.");
	}
	/* allocate memory for parse states */
	if (!file_parse_states)
		file_parse_states = check_calloc(sizeof(char), total_accepted_paths);
	strbuf_close(sb);
}

void
walk_free_path_tree(PATH_NODE *path)
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
			walk_free_path_tree(childpath);
		}
		varray_close(vb); /* close current childpaths varray */
	}
}

void
free_path_tree(void)
{
	if (rootpath) {
		walk_free_path_tree(rootpath);
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
}

int
walk_traverse_path_tree(PATH_NODE *path, STRBUF *sb, int (*handler)(const char *, void *), void *data)
{
	VARRAY *vb;
	PATH_NODE *childpath;
	int ret;
	if (path != rootpath) /* no root */
		strbuf_putc(sb, SEP);
	strbuf_puts(sb, path->name);
	if (!path->childpaths) {  /* reach end of a path */
		if (handler && (ret = handler(strbuf_value(sb), data)) != 0)
			return ret;
	} else { /* go deeper in the path tree */
		vb = path->childpaths;
		for (int i = 0; i < vb->length; i++) {
			childpath = varray_assign(vb, i, 0);
			walk_traverse_path_tree(childpath, sb, handler, data);
		}
	}
	strbuf_rtruncate(sb, strlen(path->name));
	strbuf_rtruncate(sb, 1);
	return 0;
}

int
path_tree_traverse(int (*handler)(const char *, void *), void *data)
{
	STATIC_STRBUF(sb);
	strbuf_clear(sb);
	/* will return result from the callback gtags_handle_path function */
	return walk_traverse_path_tree(rootpath, sb, handler, data);
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

int
path_tree_search_name(const char *name, int (*handler)(const char *, void *), void *data)
{
	const char *fpath;
	struct sh_entry *entry;
	VARRAY *vb;
	VARRAY_LOC *loc;
	int ret;
	entry = strhash_assign(name_bucket, name, 0);
	if (entry && entry->value) {
		vb = entry->value;
		for (int i = 0; i < vb->length; i++) {
			loc = varray_assign(vb, i, 0);
			fpath = construct_path_from_leaf(loc);
			if (handler && (ret = handler(fpath, data)) != 0)
				return ret;
		}
	}
	return 0;
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
path_tree_no_handler(const char *path, void *data)
{
	printf("%s\n", path);
	return 0;
}
