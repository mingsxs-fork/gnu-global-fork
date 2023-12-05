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
#include "varray.h"
#define NAME_MAX 255
#endif
#endif /* #ifndef NAME_MAX */

typedef struct path_node PATH_NODE;

struct path_node {
	char name[NAME_MAX];
	VARRAY_LOC parentloc;	/* parent path node of current path */
	VARRAY *childpaths;		/* child paths included by current path */
};

typedef enum {
	FILE_PARSE_NEW		= 0,
	FILE_PARSE_PENDING	= 1,
	FILE_PARSE_DONE		= 2,
} file_parse_state_e;

void make_path_tree(const char *);
void free_path_tree(void);
int path_tree_traverse(int (*)(const char *, void *), void *);
int path_tree_search_name(const char *, int(*)(const char *, void *), void *);
void set_file_parse_state(const char *, file_parse_state_e);
int get_file_parse_state(const char *);

int path_tree_no_handler(const char *, void * __attribute__((unused)));

#endif /* ! _PATH_TREE_H_ */


