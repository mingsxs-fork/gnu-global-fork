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
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#include "gparam.h"
#include "gtagsop.h"
#include "threading.h"

struct gtags_path {
	char path[MAXPATHLEN];
	const char *fid;
	int seqno;
};

struct gtags_path_proc_data {
	GTOP *gtop[GTAGLIM];
};

struct gtags_path_add_data {
	STRBUF *addlist;
	STRBUF *dellist;
	STRBUF *addlist_other;
	IDSET *findset;
	IDSET *delset;
	struct stat *gtag_sb;
	struct stat *path_sb;
};

struct gtags_conf {
	unsigned int vflag:1;
	unsigned int wflag:1;
	unsigned int qflag:1;
	unsigned int debug:1;
	unsigned int iflag:1;
	unsigned int explain:1;
	unsigned int extractmethod:1;
	unsigned int incremental:1;
	int parser_flags;
};

struct gtags_priv_data {
	struct gtags_conf gconf;	/* gtags config, global */
	struct gtags_path *gpath;	/* gtags path, on stack */
	unsigned int *npath_done;	/* global */
	/* private data used by handlers */
	struct gtags_path_proc_data *proc_data;	/* proc path data, global */
	struct gtags_path_add_data *add_data;	/* add path data, global */
};

/* misc data required by gtags jobs */
typedef struct gtags_job_data {
	void *priv_data; /* gtags_priv_data */
	STRHASH *strhash;
} GJOB_DATA;

void gtags_proc_path(const char *, void *);
void gtags_add_path(const char *, void *);
void gtags_put_symbol(int , const char *, int , const char *, const char *, void *);
void gtags_threading_proc_path(const char *, void *);

#endif /* ! _GTAGS_HELPER_H_ */
