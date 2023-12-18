/*
 * Copyright (c) 2011, 2019 Tama Communications Corporation
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
#ifdef STDC_HEADERS
#include <stdlib.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "gparam.h"
#include "logging.h"
#include "path.h"

/*

Logging utility:

+------------------------------------
|main(int argc, char **argv)
|{
|	logging_printf("Start\n");
|	logging_arguments(argc, argv);
|	...

% setenv GTAGSLOGGING /tmp/log
% global -x main
% cat /tmp/log
Start
0: |global|
1: |-x|
2: |main|
% _

See logging_printf() for more details.
*/
static FILE *logging_handler = NULL;
static int ignore = 0;

FILE *
open_logging_handler(void)
{
	FILE *handler;
	char *path;

	if (logging_handler)
		return logging_handler;

	path = getenv("GTAGSLOGGING");
	if (path != NULL && (handler = fopen(path, "a")) != NULL)
		logging_handler = handler;
	else {
		logging_handler = stderr;
		ignore = 1;  /* ignore */
	}

	return logging_handler;
}
/**
 * logging_printf: print a message into the logging file.
 *
 *	@param[in]	s	printf style format (fmt) string
 *
 *		Log messages are appended to the logging file; which is opened using 
 *		'fopen(xx, "a")' on the first call to logging_printf() or
 *		logging_arguments().
 *		The logging file's filename should be in the OS environment variable
 *		"GTAGSLOGGING".
 *		If "GTAGSLOGGING" is not setup or the logging file cannot be
 *		opened, logging is disabled; logging_printf() and logging_arguments()
 *		then do nothing.
 *
 *	[Note] The logging file stays open for the life of the progam.
 */
void
logging_printf(const char *s, ...)
{
	va_list ap;

	if (ignore)
		return;
	if (!logging_handler)
		(void)open_logging_handler();

	va_start(ap, s);
	vfprintf(logging_handler, s, ap);
	va_end(ap);
}
/**
 * logging_flush: flush the logging buffer.
 */
void
logging_flush(void)
{
	if (logging_handler)
		fflush(logging_handler);
}

/**
 * logging_arguments: print arguments into the logging file.
 *
 *	@param[in]	argc
 *	@param[in]	argv
 *
 *	Uses:
 *		logging_printf()
 */
void
logging_arguments(int argc, char **argv)
{
	int i;
	char buf[MAXPATHLEN];

	if (vgetcwd(buf, sizeof(buf)))
		logging_printf("In |%s|\n", buf);
	for (i = 0; i < argc; i++) {
		if (ignore)
			break;
		logging_printf("%d: |%s|\n", i, argv[i]);
	}
	logging_flush();
}
