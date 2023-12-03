/*
 * Copyright (c) Ming Li
 *
 * adgio.ming@gmail.com
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

#ifndef _PATH_TREE_H_
#define _PATH_TREE_H_

#ifndef NAME_MAX
#ifdef HAVE_LIMITS_H
#include <limits.h>
#else
#include "gparam.h"
#define NAME_MAX MAXTOKEN
#endif
#endif /* #ifndef NAME_MAX */

typedef struct path_node PATH_NODE;
typedef struct path_list PATH_LIST;

struct path_list {
	struct path_node *path;
	struct path_list *next;
};

struct path_node {
	char name[NAME_MAX];
	struct path_node *parentpath;  /* parent path node of current path */
	struct path_list *childpaths;  /* child paths contained by current path, a linked list */
};

typedef enum {
	FILE_PARSE_NEW = 0,
	FILE_PARSE_PENDING,
	FILE_PARSE_DONE,
} file_parse_state_e;

void build_path_tree(const char *);
void destroy_path_tree(void);
void path_tree_traverse(void *);
void path_tree_search_name(const char *, void *);
void set_file_parse_state(const char *, file_parse_state_e);
int get_file_parse_state(const char *);
const char *commonprefix(const char *, const char *);

#endif /* ! _PATH_TREE_H_ */


