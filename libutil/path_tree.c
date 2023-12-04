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
#include "dbop.h"
#include "gpathop.h"
#include "path_tree.h"
#include "gtags_helper.h"

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

#define HASHBUCKETS	256

/* internal variables */
static int total_accepted_paths = 0;
static char *file_parse_states = NULL;
static char *rootrealpath;
static const int allow_blank = 1;
static PATH_NODE *rootpath = NULL;
static DBOP *search_bucket = NULL;

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
static void walk_destroy_path_tree(PATH_NODE *);
static int walk_build_path_tree(PATH_NODE *, STRBUF *);
static inline void add_childpath(PATH_NODE *, const char *);
static inline void update_search_bucket(PATH_NODE *);
static int walk_path_tree(PATH_NODE *, STRBUF *, void *);
static inline const char *construct_path_from_leaf(PATH_NODE *);


int
check_path(const char *path)
{
	struct stat st, lst;
	if (stat(path, &st) < 0) {
		warning("cannot stat path '%s'. ignored.", trimpath(path));
		return 0;
	}
	if (S_ISSOCK(st.st_mode) || S_ISFIFO(st.st_mode) || S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode)) {
		warning("file is not regular file '%s'. ignored.", trimpath(path));
		return 0;
	}
	if (S_ISDIR(st.st_mode) && unallowed_symlink(path)) {
		warning("unallowed symbolic link detected. '%s' is ignored.", trimpath(path));
		return 0;
	}
	if (access(path, R_OK) < 0) {
		if (!get_skip_unreadable())
			die("cannot read file '%s'.", trimpath(path));
		warning("cannot read '%s'. ignored.", trimpath(path));
		return 0;
	}
#ifndef __DJGPP__
	if (get_skip_symlink() > 0) {
#if defined(_WIN32) && !defined(__CYGWIN__)
		DWORD attr = GetFileAttributes(path);
		if (attr != -1 && (attr & FILE_ATTRIBUTE_REPARSE_POINT))
#else
		if (lstat(path, &lst) < 0) {
			warning("cannot lstat '%s'. ignored.", trimpath(path));
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
		if (skipthisfile(path))
			return 0;
		if (!allow_blank && locatestring(path, " ", MATCH_FIRST)) {
			warning("'%s' ignored, because it includes blank.", trimpath(path));
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
add_childpath(PATH_NODE *parentpath, const char *name)
{
	PATH_NODE *path;
	PATH_LIST *node;

	if (!parentpath || !name)
		return;
	/* init current path node */
	path = check_malloc(sizeof(PATH_NODE));
	path->parentpath = parentpath;
	strlimcpy(path->name, name, sizeof(path->name));
	path->childpaths = NULL;
	/* init the path list node as a childpath of parent */
	node = check_malloc(sizeof(PATH_LIST));
	node->path = path;
	node->next = NULL;
	if (parentpath->childpaths) {
		node->next = parentpath->childpaths;
	}
	parentpath->childpaths = node;
}

void
update_search_bucket(PATH_NODE *path)
{
	PATH_LIST *node, *head;
	size_t len;
	if (!path || !search_bucket)
		return ;
	len = strlen(path->name) + 1;
	node = check_malloc(sizeof(PATH_LIST));
	node->path = path;
	node->next = NULL;
	head = (PATH_LIST *)dbop_get_generic(search_bucket, path->name, len);
	if (head) {
		node->next = head;
	}
	dbop_put_generic(search_bucket, path->name, len, node, sizeof(*node), 0);
}

int
walk_build_path_tree(PATH_NODE *path_node, STRBUF *sb)
{
	DIR *dirp;
	struct dirent *dp;
	struct stat st;
	const char *path = strbuf_value(sb);;

	if ((dirp = opendir(path)) == NULL) {
		warning("cannot open directory '%s'. ignored.", trimpath(path));
		return -1;
	}
	strbuf_putc(sb, SEP);
	while ((dp = readdir(dirp)) != NULL) {
		if (ignore_path(dp->d_name))
			continue;
		strbuf_puts(sb, dp->d_name);
		if (check_path(strbuf_value(sb))) {
			(void)stat(strbuf_value(sb), &st);
			if (S_ISDIR(st.st_mode)) { /* directory */
				add_childpath(path_node, dp->d_name);
				(void)walk_build_path_tree(path_node->childpaths->path, sb);
			} else if (S_ISREG(st.st_mode)) { /* regular file */
				total_accepted_paths += 1;  /* accept regular files */
				add_childpath(path_node, dp->d_name);
				update_search_bucket(path_node->childpaths->path);
			}
		}
		strbuf_rtruncate(sb, strlen(dp->d_name));
	}
	(void)closedir(dirp);
	strbuf_rtruncate(sb, 1);  /* truncate SEP */
	return 0;  /* current directory build done */
}

void
build_path_tree(const char *start)
{
	STRBUF *sb = strbuf_open(0);
	if (!start)
		start = ".";  /* default root path */
	if ((rootrealpath = realpath(start, NULL)) == NULL)
		die("can't get real path of root path '%s'.", trimpath(start));
	if (!check_path(start))
		die("check root path failed: %s", trimpath(start));
	if (!rootpath) {
		rootpath = check_malloc(sizeof(PATH_NODE));
		rootpath->parentpath = NULL;  /* rootpath parent path is NULL */
		strlimcpy(rootpath->name, start, sizeof(rootpath->name));
		rootpath->childpaths = NULL;
	}
	if (!search_bucket) {
		search_bucket = dbop_open(NULL, 1, 0644, 0);
		if (!search_bucket)
			die("can't create name search hash bucket.");
	}
	/* start building path tree recursively from rootdir */
	strbuf_puts(sb, start);
	if (walk_build_path_tree(rootpath, sb) < 0) {
		destroy_path_tree();
		die("recursively build path tree failed.");
	}
	/* allocate memory for parse states */
	if (!file_parse_states)
		file_parse_states = check_calloc(total_accepted_paths, sizeof(char));
	strbuf_close(sb);
}

void
walk_destroy_path_tree(PATH_NODE *path_node)
{
	PATH_NODE *childpath;
	PATH_LIST *freenode = NULL, *tmpnode;
	if (!path_node) /* reach leaf node */
		return;
	/* free file name search map in bucket */
	if (!path_node->childpaths && search_bucket) {
		freenode = dbop_get_generic(search_bucket, path_node->name, strlen(path_node->name)+1);
		while (freenode) {
			tmpnode = freenode->next;
			free(freenode);
			freenode = tmpnode;
		}
	}
	freenode = path_node->childpaths;
	while (freenode) {
		childpath = freenode->path;
		tmpnode = freenode->next;
		free(freenode);
		freenode = tmpnode;
		walk_destroy_path_tree(childpath);
	}
	free(path_node);
}

void
destroy_path_tree(void)
{
	if (rootpath) {
		walk_destroy_path_tree(rootpath);
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
	if (search_bucket) {
		dbop_close(search_bucket);
		search_bucket = NULL;
	}
	/* reset total accepted paths */
	total_accepted_paths = 0;
}

int
walk_path_tree(PATH_NODE *path, STRBUF *sb, void *data)
{
	PATH_LIST *walknode;
	if (path->parentpath)
		strbuf_putc(sb, SEP);
	strbuf_puts(sb, path->name);
	if (!path->childpaths) {  /* reach end of a path */
		gtags_handle_path(strbuf_value(sb), data);
	} else { /* go deeper in the path tree */
		for (walknode = path->childpaths; walknode; walknode = walknode->next) {
			walk_path_tree(walknode->path, sb, data);
		}
	}
	strbuf_rtruncate(sb, strlen(path->name));
	strbuf_rtruncate(sb, 1);
	return 0;
}

void
path_tree_traverse(void *data)
{
	STATIC_STRBUF(sb);
	strbuf_clear(sb);
	walk_path_tree(rootpath, sb, data);
}

const char *
construct_path_from_leaf(PATH_NODE *leaf)
{
	STATIC_STRBUF(sb);
	PATH_NODE *path;
	strbuf_clear(sb);
	for (path = leaf; path; path=path->parentpath) {
		strbuf_prepends(sb, path->name);
		if (path->parentpath)
			strbuf_prependc(sb, SEP);
	}
	return strbuf_value(sb);
}

void
path_tree_search_name(const char *name, void *data)
{
	const char *path;
	PATH_LIST *walknode = dbop_get_generic(search_bucket, (void *)name, strlen(name)+1);
	while (walknode) {
		path = construct_path_from_leaf(walknode->path);
		gtags_handle_path(path, data);
		walknode = walknode->next;
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
