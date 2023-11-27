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
#include "gtagsop.h"

struct put_func_data {
	GTOP *gtop[GTAGLIM];
	const char *fid;
};

struct gtags_conf_data {
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
	int *path_seqno;
	struct put_func_data put_data;
	struct gtags_conf_data conf_data;
};

void gtags_handle_path(const char *, void *);
void gtags_put_symbol(int , const char *, int , const char *, const char *, void *);

#define reset_gtags_priv_data(data) do {\
	memset(&data.conf_data, 0x0, sizeof(struct gtags_conf_data));\
	memset(&data.put_data, 0x0, sizeof(struct put_func_data));\
	data.path_seqno = NULL;\
} while (0)

#endif /* ! _GTAGS_HELPER_H_ */
