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

#ifndef _GTAGS_HELPER_H_
#define _GTAGS_HELPER_H_

#ifdef HAVE_STRING_H
#include <string.h>
#else
#include <strings.h>
#endif
#include "gparam.h"
#include "gtagsop.h"

struct gtags_path {
	char path[MAXPATHLEN];
	const char *fid;
	unsigned int seq;
};

struct gtags_conf {
	int vflag;
	int wflag;
	int qflag;
	int debug;
	int iflag;
	int explain;
	int extractmethod;
	int parser_flags;
};

struct gtags_priv_data {
	unsigned int *gpath_handled;  /* global */
	GTOP *gtop[GTAGLIM]; /* global */
	struct gtags_path *gpath; /* on stack */
	struct gtags_conf gconf;  /* global */
};

void gtags_handle_path(const char *, void *);
void gtags_put_symbol(int , const char *, int , const char *, const char *, void *);

#endif /* ! _GTAGS_HELPER_H_ */
