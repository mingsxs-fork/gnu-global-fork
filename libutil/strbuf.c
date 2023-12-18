/*
 * Copyright (c) 1997, 1998, 1999, 2000, 2002, 2005, 2006, 2010, 2014,
 *	2015
 *	Tama Communications Corporation
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
#ifdef STDC_HEADERS
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#else
#include <strings.h>
#endif

#include "checkalloc.h"
#include "die.h"
#include "strbuf.h"
#include "varray.h"
#include "likely.h"

#ifndef isblank
#define isblank(c)	((c) == ' ' || (c) == '\t')
#endif

#define STRBUF_POOL_EXPAND 32 /* to align with strbuf pool map */
#define first_zero_bit(x)	(int)(__builtin_ffs(~(unsigned int)x) - 1)

static STRBUF_POOL *sb_pool = NULL;
#ifdef USE_THREADING
pthread_rwlock_t sb_pool_rwlock = PTHREAD_RWLOCK_INITIALIZER;
#endif

/*

String buffer: usage and memory status

					[xxx]: string buffer
					'v': current pointer

Function call                           Memory status
----------------------------------------------------------
                                        (not exist)
                                         v
sb = strbuf_open(0);                    []
                                          v
strbuf_putc(sb, 'a');                   [a]
                                          v
char *s = strbuf_value(sb);             [a\0]           s == "a"
                                            v
strbuf_puts(sb, "bc");                  [abc]
                                            v
char *s = strbuf_value(sb);             [abc\0]         s == "abc"
                                            v
int len = strbuf_getlen(sb);            [abc\0]         len == 3
                                         v
strbuf_reset(sb);                       [abc\0]
                                         v
int len = strbuf_getlen(sb);            [abc\0]         len == 0
                                           v
strbuf_puts(sb, "XY");                  [XYc\0]
                                           v
char *s = strbuf_value(sb);             [XY\0]          s == "XY"

fp = fopen("/etc/passwd", "r");                                             v
char *s = strbuf_fgets(sb, fp, 0)       [root:*:0:0:Charlie &:/root:/bin/csh\0]
fclose(fp)				s == "root:*:0:0:Charlie &:/root:/bin/csh"

strbuf_close(sb);                       (not exist)
*/

/**
 * __strbuf_expandbuf: expand buffer so that afford to the length data at least.
 *
 *	@param[in]	sb	STRBUF structure
 *	@param[in]	length	required room
 */
void
__strbuf_expandbuf(STRBUF *sb, int length)
{
	int count = sb->curp - sb->sbuf;
	int newsize = sb->sbufsize + (length > EXPANDSIZE ? length : EXPANDSIZE);
	char *newbuf;

	newbuf = (char *)check_realloc(sb->sbuf, newsize + 1);
	sb->sbufsize = newsize;
	sb->sbuf = newbuf;

	sb->curp = sb->sbuf + count;
	sb->endp = sb->sbuf + sb->sbufsize;
}
void
__strbuf_init(STRBUF *sb, int init)
{
	int initsize = (init > 0) ? init : INITIALSIZE;
	if (sb->sbufsize == 0 || sb->sbufsize < initsize) {
		sb->sbufsize = initsize;
		if (sb->sbuf)
			(void) free(sb->sbuf);
		sb->sbuf = (char *)check_malloc(sb->sbufsize + 1);
	}
	sb->curp = sb->sbuf;
	sb->endp = sb->sbuf + sb->sbufsize;
}
/**
 * strbuf_open: open string buffer.
 *
 *	@param[in]	init	initial buffer size
 *			if 0 (zero) is specified then use default value (INITIALSIZE).
 *	@return	sb	STRBUF structure
 */
STRBUF *
strbuf_open(int init)
{
#ifdef USE_THREADING
	static thread_local STRBUF *sb;
#else
	STRBUF *sb;
#endif
	sb = check_calloc(sizeof(STRBUF), 1);

	sb->sbufsize = (init > 0) ? init : INITIALSIZE;
	sb->sbuf = (char *)check_malloc(sb->sbufsize + 1);
	sb->curp = sb->sbuf;
	sb->endp = sb->sbuf + sb->sbufsize;

	return sb;
}
/**
 * strbuf_reset: reset string buffer.
 *
 *	@param[in]	sb	string buffer
 */
void
strbuf_reset(STRBUF *sb)
{
	sb->curp = sb->sbuf;
}
/**
 * strbuf_clear: clear static string buffer.
 *
 *	@param[in]	sb	statically defined string buffer
 *
 * This function is used for the initializing of static string buffer.
 * For the detail, see STATIC_STRBUF(sb) macro in "strbuf.h".
 */
void
strbuf_clear(STRBUF *sb)
{
	if (sb == NULL)
		die("NULL string buffer. (strbuf_clear)");
	if (strbuf_empty(sb)) {
		sb->sbufsize = INITIALSIZE;
		sb->sbuf = (char *)check_malloc(sb->sbufsize + 1);
		sb->curp = sb->sbuf;
		sb->endp = sb->sbuf + sb->sbufsize;
	} else {
		strbuf_reset(sb);
	}
}
/**
 * strbuf_nputs: Put string with length
 *
 *	@param[in,out]	sb	string buffer
 *	@param[in]	s	string
 *	@param[in]	len	length of string
 */
void
strbuf_nputs(STRBUF *sb, const char *s, int len)
{
	if (len > 0) {
		if (sb->curp + len > sb->endp)
			__strbuf_expandbuf(sb, len);
		while (len-- > 0)
			*sb->curp++ = *s++;
	}
}
/**
 * strbuf_nputc: Put a character, len (number) times
 *
 *	@param[in,out]	sb	string buffer
 *	@param[in]	c	character
 *	@param[in]	len	number of times to put c
 *
 *	See strbuf_putc()
 */
void
strbuf_nputc(STRBUF *sb, int c, int len)
{
	if (len > 0) {
		if (sb->curp + len > sb->endp)
			__strbuf_expandbuf(sb, len);
		while (len-- > 0)
			*sb->curp++ = c;
	}
}
/**
 * strbuf_puts: Put string
 *
 *	@param[in,out]	sb	string buffer
 *	@param[in]	s	string
 */
void
strbuf_puts(STRBUF *sb, const char *s)
{
	while (*s) {
		if (sb->curp >= sb->endp)
			__strbuf_expandbuf(sb, 0);
		*sb->curp++ = *s++;
	}
}
/**
 * strbuf_puts_withterm: Put string until the terminator
 *
 *	@param[in,out]	sb	string buffer
 *	@param[in]	s	string
 *	@param[in]	c	terminator
 */
void
strbuf_puts_withterm(STRBUF *sb, const char *s, int c)
{
	while (*s && *s != c) {
		if (sb->curp >= sb->endp)
			__strbuf_expandbuf(sb, 0);
		*sb->curp++ = *s++;
	}
}
/**
 * strbuf_puts_nl: Put string with a new line
 *
 *	@param[in,out]	sb	string buffer
 *	@param[in]	s	string
 */
void
strbuf_puts_nl(STRBUF *sb, const char *s)
{
	while (*s) {
		if (sb->curp >= sb->endp)
			__strbuf_expandbuf(sb, 0);
		*sb->curp++ = *s++;
	}
	if (sb->curp >= sb->endp)
		__strbuf_expandbuf(sb, 0);
	*sb->curp++ = '\n';
}
/**
 * strbuf_putn: put digit string at the last of buffer.
 *
 *	@param[in,out]	sb	STRBUF structure
 *	@param[in]	n	number
 */
void
strbuf_putn(STRBUF *sb, int n)
{
	if (n == 0) {
		strbuf_putc(sb, '0');
	} else {
		char num[128];
		int i = 0;

		while (n) {
			if (i >= sizeof(num))
				die("Too big integer value.");
			num[i++] = n % 10 + '0';
			n = n / 10;
		}
		while (--i >= 0)
			strbuf_putc(sb, num[i]);
	}
}
/**
 * strbuf_putn64: put digit string at the last of buffer.
 *
 *	@param[in,out]	sb	STRBUF structure
 *	@param[in]	n	number
 */
void
strbuf_putn64(STRBUF *sb, long long n)
{
	if (n == 0) {
		strbuf_putc(sb, '0');
	} else {
		char num[128];
		int i = 0;

		while (n) {
			if (i >= sizeof(num))
				die("Too big integer value.");
			num[i++] = n % 10 + '0';
			n = n / 10;
		}
		while (--i >= 0)
			strbuf_putc(sb, num[i]);
	}
}
/**
 * strbuf_unputc: remove specified char from the last of buffer
 *
 *	@param[in,out]	sb	STRBUF structure
 *	@param[in]	c	character
 *	@return		0: do nothing, 1: removed
 */
int
strbuf_unputc(STRBUF *sb, int c)
{
	if (sb->curp > sb->sbuf && *(sb->curp - 1) == c) {
		sb->curp--;
		return 1;
	}
	return 0;
}
/**
 * strbuf_value: return the content of string buffer.
 *
 *	@param[in]	sb	STRBUF structure
 *	@return		string
 */
char *
strbuf_value(STRBUF *sb)
{
	*sb->curp = 0;
	return sb->sbuf;
}
/**
 * strbuf_trim: trim following blanks.
 *
 *	@param[in,out]	sb	STRBUF structure
 */
void
strbuf_trim(STRBUF *sb)
{
	char *p = sb->curp;

	while (p > sb->sbuf && isblank(*(p - 1)))
		*--p = 0;
	sb->curp = p;
}
/*
 * strbuf_fgets: read whole record into string buffer
 *
 *	@param[in,out]	sb	string buffer
 *	@param[in]	ip	input stream
 *	@param[in]	flags	flags
 *			STRBUF_NOCRLF:	remove last '\n' and/or '\r' if exist.
 *			STRBUF_APPEND:	append next record to existing data
 *			STRBUF_SHARPSKIP: skip lines which start with '#'
 *	@return		record buffer (NULL at end of file)
 *
 * Returned buffer has whole record.
 * The buffer end with '\0'. If STRBUF_NOCRLF is set then buffer doesn't
 * include '\r' and '\n'.
 */
char *
strbuf_fgets(STRBUF *sb, FILE *ip, int flags)
{
	if (!(flags & STRBUF_APPEND))
		strbuf_reset(sb);

	if (sb->curp >= sb->endp)
		__strbuf_expandbuf(sb, EXPANDSIZE);	/* expand buffer */

	if (flags & STRBUF_SHARPSKIP) {
		int c = 0;

		/* skip comment lines */
		while ((c = fgetc(ip)) == '#') {
	 		/* read and thrown away until the last of the line */
			while ((c = fgetc(ip)) != EOF && c != '\n')
				;
		}
		if (c == EOF)
			return NULL;
		/* push back the first character of the line */
		ungetc(c, ip);
	}
	for (;;) {
		if (!fgets(sb->curp, sb->endp - sb->curp, ip)) {
			if (sb->curp == sb->sbuf)
				return NULL;
			break;
		}
		sb->curp += strlen(sb->curp);
		if (sb->curp > sb->sbuf && *(sb->curp - 1) == '\n')
			break;
		else if (feof(ip)) {
			return sb->sbuf;
		}
		__strbuf_expandbuf(sb, EXPANDSIZE);	/* expand buffer */
	}
	if (flags & STRBUF_NOCRLF) {
		if (*(sb->curp - 1) == '\n')
			*(--sb->curp) = 0;
		if (sb->curp > sb->sbuf && *(sb->curp - 1) == '\r')
			*(--sb->curp) = 0;
	}
	return sb->sbuf;
}
/*
 * strbuf_sprintf: do sprintf into string buffer.
 *
 *	@param[in,out]	sb	STRBUF structure
 *	@param[in]	s	similar to sprintf()
 *			Currently the following format is supported.
 *			%s, %d, %<number>d, %<number>s, %-<number>d, %-<number>s
 */
void
strbuf_sprintf(STRBUF *sb, const char *s, ...)
{
	va_list ap;

	va_start(ap, s);
	strbuf_vsprintf(sb, s, ap);
	va_end(ap);
}
/*
 * strbuf_vsprintf: do vsprintf into string buffer.
 *
 *	@param[in,out]	sb	STRBUF structure
 *	@param[in]	s	similar to vsprintf()
 *			Currently the following format is supported.
 *			%s, %d, %<number>d, %<number>s, %-<number>d, %-<number>s
 *	@param[in]	ap 
 */
void
strbuf_vsprintf(STRBUF *sb, const char *s, va_list ap)
{
	for (; *s; s++) {
		/*
		 * Put the before part of '%'.
		 */
		{
			const char *p;
			for (p = s; *p && *p != '%'; p++)
				;
			if (p > s) {
				strbuf_nputs(sb, s, p - s);
				s = p;
			}
		}
		if (*s == '\0')
			break;
		if (*s == '%') {
			int c = (unsigned char)*++s;
			/*
			 * '%%' means '%'.
			 */
			if (c == '%') {
				strbuf_putc(sb, c);
			}
			/*
			 * If the optional number is specified then
			 * we forward the job to snprintf(3).
			 * o %<number>d
			 * o %<number>s
			 * o %-<number>d
			 * o %-<number>s
			 */
			else if (isdigit(c) || (c == '-' && isdigit((unsigned char)*(s + 1)))) {
				char format[32], buf[1024];
				int i = 0;

				format[i++] = '%';
				if (c == '-')
					format[i++] = *s++;
				while (isdigit((unsigned char)*s))
					format[i++] = *s++;
				format[i++] = c = *s;
				format[i] = '\0';
				if (c == 'd' || c == 'x')
					snprintf(buf, sizeof(buf), format, va_arg(ap, int));
				else if (c == 's')
					snprintf(buf, sizeof(buf), format, va_arg(ap, char *));
				else
					die("Unsupported control character '%c'.", c);
				strbuf_puts(sb, buf);
			} else if (c == 's') {
				strbuf_puts(sb, va_arg(ap, char *));
			} else if (c == 'd') {
				strbuf_putn(sb, va_arg(ap, int));
			} else {
				die("Unsupported control character '%c'.", c);
			}
		}
	}
}
/**
 * strbuf_close: close string buffer.
 *
 *	@param[in]	sb	STRBUF structure
 */
void
strbuf_close(STRBUF *sb)
{
	if (sb->name)
		(void)free(sb->name);
	(void)free(sb->sbuf);
	(void)free(sb);
}
/**
 * STRBUF *strbuf_open_tempbuf(void)
 *
 * Temporary string buffer for general purpose.
 *
 * Usage:
 *
 *	STRBUF *sbt = strbuf_open_tempbuf();
 *	....
 *	strbuf_puts(sbtemp, "xxx");
 *	...
 *	strbuf_release_tempbuf(sbt);
 *
 */
static int used = 0;

STRBUF *
strbuf_open_tempbuf(void)
{
	STATIC_STRBUF(sb);
	if (used)
		die("Internal error: temporary string buffer is already used.");
	used = 1;
	strbuf_clear(sb);
	return sb;
}

/**
 * [Note] this function is for use with strbuf_open_tempbuf() only.
 */
void
strbuf_release_tempbuf(STRBUF *sb)
{
	used = 0;
}
/**
 * next_string:
 */
char *
next_string(char *p)
{
	while (*p++)
		;
	return p;
}

/**
 * strbuf_prepends: prepend string to strbuf.
 */
void
strbuf_prepends(STRBUF *sb, const char *s)
{
	unsigned int slen = strlen(s);
	if (sb->curp + slen > sb->endp)
		__strbuf_expandbuf(sb, slen);
	memmove(sb->sbuf+slen, sb->sbuf, strbuf_getlen(sb));
	memcpy(sb->sbuf, s, slen);
	sb->curp += slen;
}

/**
 * strbuf_startswith: check if strbuf starts with a specific prefix.
 */
int
strbuf_startswith(STRBUF *sb, const char *s)
{
	char *p = sb->sbuf;
	while (*p++ == *s++);
	return *s == '\0' ? 1 : 0;
}

/**
 * strbuf_endswith: check if strbuf ends with a specific suffix.
 */
int
strbuf_endswith(STRBUF *sb, const char *s)
{
	char *p = sb->curp - 1;
	const char *sp = s + strlen(s) - 1;
	while (p >= sb->sbuf && sp >= s && *p-- == *sp--);
	return sp == s ? 1 : 0;
}

void
strbuf_pool_init (int limit)
{
#ifdef USE_THREADING
	(void)pthread_rwlock_wrlock(&sb_pool_rwlock);
#endif
	if (!sb_pool) {
		sb_pool = check_malloc(sizeof(STRBUF_POOL));
		sb_pool->limit = limit;
		sb_pool->vb = varray_open(sizeof(STRBUF), STRBUF_POOL_EXPAND);
		sb_pool->map = NULL;
		sb_pool->mapsize = sb_pool->assigned = sb_pool->released = 0;
#ifdef USE_THREADING
		int r;
		if ((r = pthread_mutex_init(&sb_pool->mutex, NULL)) != 0)
			die("strbuf pool init mutex failed, retcode: %d\n", r);
#endif
	} else { /* reinit */
		if ((sb_pool->assigned - sb_pool->released) > limit) {
			warning ("beyond strbuf pool limit, original: %d, new: %d, ignored", sb_pool->limit, limit);
			return ;
		}
		sb_pool->limit = limit; /* only update pool limit */
	}
#ifdef USE_THREADING
	(void)pthread_rwlock_unlock(&sb_pool_rwlock);
#endif
}

void
strbuf_pool_close (void)
{
	int i;
	STRBUF *sb;
#ifdef USE_THREADING
	(void)pthread_rwlock_wrlock(&sb_pool_rwlock);
#endif
	if (!sb_pool)
		goto defer;
	/* free all strbuf internal buffers assigned */
#ifdef USE_THREADING
	(void)pthread_mutex_lock(&sb_pool->mutex);
#endif
	for (i = 0; i < sb_pool->vb->length; i++) {
		sb = varray_assign(sb_pool->vb, i, 0);
		if (sb->name) {
			(void)free(sb->name);
			sb->name = NULL;
		}
		if (sb->sbuf) {
			(void)free(sb->sbuf);
			sb->sbuf = NULL;
		}
	}
	varray_close(sb_pool->vb);
	if (sb_pool->map)
		free(sb_pool->map);
#ifdef USE_THREADING
	(void)pthread_mutex_unlock(&sb_pool->mutex);
	(void)pthread_mutex_destroy(&sb_pool->mutex);
#endif
	free(sb_pool);
	sb_pool = NULL;
defer:
#ifdef USE_THREADING
	(void)pthread_rwlock_unlock(&sb_pool_rwlock);
#endif
	return;
}

STRBUF *
strbuf_pool_assign (int initsize)
{
	static int imapsave = 0;
	STRBUF *unused = NULL;
	register int imap, upper, k, index;
#ifdef USE_THREADING
	(void)pthread_rwlock_rdlock(&sb_pool_rwlock);
#endif
	if (!sb_pool)
		die("strbuf pool not initialized.");
#ifdef USE_THREADING
	(void)pthread_mutex_lock(&sb_pool->mutex);
#endif
	if (sb_pool->assigned - sb_pool->released >= sb_pool->limit) {
		warning("strbuf pool beyond limit: %d.", sb_pool->limit);
		goto defer;
	}
	/* first lookup in the allocated block */
	for (imap = imapsave, upper = sb_pool->mapsize; imap < upper; imap++) {
		k = first_zero_bit(sb_pool->map[imap]);
		if (k > -1) { /* found unused strbuf */
			index = imap * STRBUF_POOL_EXPAND + k;
			if (unlikely(index > sb_pool->vb->length))
				die("strbuf pool internal error, index beyond length");
			if (index < sb_pool->vb->length)
				unused = varray_assign(sb_pool->vb, index, 0);
			else {
				unused = varray_append(sb_pool->vb); /* allocate new strbuf */
				memset(unused, 0, sizeof(*unused));
			}
			break;
		}
		if (unlikely(imap == sb_pool->mapsize-1 && imapsave > 0)) {
			upper = imapsave;
			imap = -1; /* reset to loop the part before imapsave */
		}
	}
	imapsave = (imap == sb_pool->mapsize ? 0 : imap); /* update imapsave */
	if (unlikely(unused == NULL)) { /* need reallocate mapsize */
		++sb_pool->mapsize;
		if (sb_pool->map)
			sb_pool->map = check_realloc(sb_pool->map, sb_pool->mapsize * sizeof(unsigned int));
		else
			sb_pool->map = check_malloc(sb_pool->mapsize * sizeof(unsigned int));
		sb_pool->map[sb_pool->mapsize - 1] = 0;
		unused = varray_append(sb_pool->vb);
		memset(unused, 0, sizeof(*unused));
		index = sb_pool->vb->length - 1; /* last varray item */
	}
	__strbuf_init(unused, initsize);
	sb_pool->map[index / STRBUF_POOL_EXPAND] |= (1 << (index % STRBUF_POOL_EXPAND));
	sb_pool->assigned ++;
defer:
#ifdef USE_THREADING
	(void)pthread_mutex_unlock(&sb_pool->mutex);
	(void)pthread_rwlock_unlock(&sb_pool_rwlock);
#endif
	return unused;
}

void
strbuf_pool_release (STRBUF *sb)
{
	static int isave = 0;
	int i, upper;
#ifdef USE_THREADING
	(void)pthread_rwlock_rdlock(&sb_pool_rwlock);
#endif
	if (!sb_pool)
		die("strbuf pool not initialized.");
#ifdef USE_THREADING
	(void)pthread_mutex_lock(&sb_pool->mutex);
#endif
	for (i = isave, upper = sb_pool->vb->length; i < upper; i++) {
		if (sb_pool->map[i / STRBUF_POOL_EXPAND] & (1 << (i % STRBUF_POOL_EXPAND)) == 0)
			continue;
		if (unlikely(sb == varray_assign(sb_pool->vb, i, 0))) {
			/* reset strbuf */
			strbuf_reset(sb);
			sb_pool->map[i / STRBUF_POOL_EXPAND] &= ~(1 << (i % STRBUF_POOL_EXPAND));
			sb_pool->released ++;
			break;
		}
		if (unlikely(i == sb_pool->vb->length-1 && isave > 0)) {
			upper = isave;
			i = -1; /* reset to loop the part before isave */
		}
	}
	isave = (i == sb_pool->vb->length ? 0 : i); /* update isave */
#ifdef USE_THREADING
	(void)pthread_mutex_unlock(&sb_pool->mutex);
	(void)pthread_rwlock_unlock(&sb_pool_rwlock);
#endif
}
