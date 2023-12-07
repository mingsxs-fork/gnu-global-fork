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
#include <ctype.h>
#include <stdio.h>
#ifdef STDC_HEADERS
#include <stdlib.h>
#endif

#include "gpathop.h"
#include "gtagsop.h"
#include "parser.h"
#include "die.h"
#include "test.h"
#include "find.h"
#include "path.h"
#include "path_tree.h"
#include "gtags_helper.h"
#include "strlimcpy.h"


/**
 * static void gtags_put_symbol(int type, const char *tag, int lno, const char *path, const char *line_image, void *data)
 *
 * callback functions for built-in parser
 */
void
gtags_put_symbol(int type, const char *tag, int lno, const char *path, const char *line_image, void *data)
{
	const struct gtags_priv_data *priv_data = data;
	GTOP *gtop;
	const char *p;

	/*
	 * sanity checks
	 * These checks are required, because there is no telling what kind of string
	 * comes as 'a symbol' from external plug-in parsers.
	 */
	for (p = tag; *p; p++) {
		if (isspace(*p)) {
			if (priv_data->gconf.wflag)
				warning("symbol name includs a space character. (Ignored) [+%d %s]", lno, path);
			return;
		}
	}
	if (p == tag) {
		if (priv_data->gconf.wflag)
			warning("symbol name is null. (Ignored) [+%d %s]", lno, path);
		return;
	}
	if (p - tag >= IDENTLEN) {
		if (priv_data->gconf.wflag)
			warning("symbol name is too long. (Ignored) [+%d %s]", lno, path);
		return;
	}
	switch (type) {
	case PARSER_DEF:
		gtop = priv_data->gtop[GTAGS];
		break;
	case PARSER_REF_SYM:
		gtop = priv_data->gtop[GRTAGS];
		if (gtop == NULL)
			return;
		break;
	default:
		return;
	}
	gtags_put_using(gtop, tag, lno, priv_data->gpath->fid, line_image);
}

int
gtags_handle_path(const char *path, void *data)
{
	static unsigned int seqno = 0;
	struct gtags_priv_data *priv_data = data;
	struct gtags_path gpath = {0};  /* current gpath on stack */
	struct gtags_path *gpath_prev = priv_data->gpath; /* last gpath on stack */
	int parse_state = get_file_parse_state(path);
	if (parse_state != FILE_PARSE_NEW)  /* file is under parsing */
		goto safe_out;
	if (!issourcefile(path)) {
		if (!test("b", path))
			/* other file like 'Makefile', non-binary */
			gpath_put(path, GPATH_OTHER);
		goto safe_out;
	}
	strlimcpy(gpath.path, path, sizeof(gpath.path));
	gpath_put(gpath.path, GPATH_SOURCE);
	gpath.fid = gpath_path2fid(gpath.path, NULL);
	if (gpath.fid == NULL)
		die("GPATH is corrupted.('%s' not found)", gpath.path);
	gpath.seq = ++seqno;
	priv_data->gpath = &gpath;
	set_file_parse_state(gpath.path, FILE_PARSE_PENDING);
	parse_file(&gpath, priv_data->gconf.parser_flags, gtags_put_symbol, data);
	set_file_parse_state(gpath.path, FILE_PARSE_DONE);
	priv_data->gpath = gpath_prev; /* restore gpath stack */
	gtags_flush(priv_data->gtop[GTAGS]);
	gtags_flush(priv_data->gtop[GRTAGS]);
	(*priv_data->gpath_handled)++; /* update gpath counter */
safe_out:
	return 0;
}