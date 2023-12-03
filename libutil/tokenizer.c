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
#include <stdio.h>
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
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


#include "die.h"
#include "strlimcpy.h"
#include "strbuf.h"
#include "checkalloc.h"
#include "vstack.h"
#include "tokenizer.h"
#include "likely.h"


#define STACK_EXPAND	32
#define empty_stack(ts)	(!ts || ts->stack_top < 0)

static VSTACK *tokenizer_stack = NULL;  /* global tokenizer stack */
static TOKENIZER *CURRENT;

/* when we call those default tokenizer ops functions,
 * it means the tokenizer object has already been created,
 * since we can only call them from an opened tokenizer.
 * and this guarantee the MACRO 'current' won't be NULL.
 */
static int	def_tokenizer_nexttoken(const char *, int (*)(const char *, int));
static void	def_tokenizer_pushbacktoken(void);
static int	def_tokenizer_peekchar(int);
static int	def_tokenizer_expectcharset(const char *, STRBUF *);
static inline void	def_tokenizer_pushbackchar(void);
static inline int	def_tokenizer_nextchar(void);
static inline void	def_tokenizer_skipnext(int);
static inline int	cp_at_first_nonspace(TOKENIZER *);

static struct tokenizer_ops default_ops = {
	.nexttoken		= def_tokenizer_nexttoken,
	.pushbacktoken	= def_tokenizer_pushbacktoken,
	.peekchar		= def_tokenizer_peekchar,
	.nextchar		= def_tokenizer_nextchar,
	.expectcharset	= def_tokenizer_expectcharset,
	.pushbackchar	= def_tokenizer_pushbackchar,
	.skipnext		= def_tokenizer_skipnext,
};


int opened_tokenizers (void)
{
	if (unlike(empty_stack(tokenizer_stack)))
		return 0;
	return (tokenizer_stack->stack_top + 1);
}

int
tokenizer_open (const char *path, struct tokenizer_ops *ops, void *lang_data)
{
	TOKENIZER *tokenizer;
	if (!tokenizer_stack)
		tokenizer_stack = vstack_open(sizeof(struct tokenizer), STACK_EXPAND);
	tokenizer = vstack_push(tokenizer_stack); /* allocate new tokenizer on stack */
	if (ops) {
		tokenizer->op = ops;
	} else {
		tokenizer->op = &default_ops;
	}
	tokenizer->lang_priv = lang_data; /* lang private data */
	strlimcpy(tokenizer->path, path, sizeof(tokenizer->path));
	if ((tokenizer->ip = fopen(path, "rb")) == NULL)
		goto failed;
	tokenizer->ib = strbuf_open(MAXBUFLEN);
	CURRENT = tokenizer; /* update CURRENT */
	return 1;

failed:
	(void)vstack_pop(tokenizer_stack);
	warning("open tokenizer failed, ignored");
	CURRENT = vstack_top(tokenizer_stack);
	return 0;
}

void
tokenizer_close (TOKENIZER *t)
{
	/* only close tokenizer if it's current tokenizer to avoid multiple close */
	if (t && t == CURRENT) {
		t = vstack_pop(tokenizer_stack);
		if (t->ib) {
			strbuf_close(t->ib);
			t->ib = NULL;
		}
		if (t->ip) {
			fclose(t->ip);
			t->ip = NULL;
		}
		/* update CURRENT tokenizer pointer */
		if (unlikely(empty_stack(tokenizer_stack)))
			CURRENT = NULL;
		else
			CURRENT = vstack_top(tokenizer_stack);
		/* free stack when empty */
		if (tokenizer_stack->stack_top <= 0) {
			vstack_close(tokenizer_stack);
			tokenizer_stack = NULL;
		}
	} else {
		warning("close tokenizer from an illegal level, ignored");
	}
}

static int
cp_at_first_nonspace (TOKENIZER *t)
{
	const char *sp = t->sp;
	const char *ep = t->cp ? t->cp - 1 : t->lp;

	while (sp < ep && *sp && isspace(*sp))
		sp++;
	return (sp == ep) ? 1 : 0;
}

TOKENIZER *
current_tokenizer (void)
{
	return CURRENT;  /* return current tokenizer */
}

static int
def_tokenizer_nextchar (void)
{
	TOKENIZER *t = CURRENT;
	if (unlikely(t->cp == NULL)) { /* fetch line buffer from input stream */
		t->sp = t->cp = strbuf_fgets(t->ib, t->ip, STRBUF_NOCRLF);
		if (unlikely(t->sp == NULL))
			return EOF; /* end of file */
		t->lineno ++;
	}
	if (unlikely(*t->cp == 0)) {
		t->lp = t->cp;
		t->cp = NULL;
		t->continued_line = 0;
		return (int)'\n';
	} else {
		return (int)*t->cp++;
	}
}

static int
def_tokenizer_expectcharset (const char *interested, STRBUF *out)
{
	TOKENIZER *t = CURRENT;
	int c;
	if (out)
		strbuf_clear(out);
	while ((c = t->op->nextchar()) != EOF && c != '\n' && strchr(interested, c) == NULL)
		if (out)
			strbuf_putc(out, (char)c);
	return (t->lasttoken = c);
}

static void
def_tokenizer_pushbacktoken (void)
{
	TOKENIZER *t = CURRENT;
	strlimcpy(t->ptoken, t->token, sizeof(t->ptoken));
}

static void
def_tokenizer_pushbackchar (void)
{
	TOKENIZER *t = CURRENT;
	if (likely(t->sp)) {
		if (likely(t->cp))
			t->cp--;
		else
			t->cp = t->lp;
	}
}

static void
def_tokenizer_skipnext (int n)
{
	TOKENIZER *t = CURRENT;
	while (n-- > 0)
		t->op->nextchar();
}

static int
def_tokenizer_peekchar (int immediate)
{
	TOKENIZER *t = CURRENT;
	int c;
	long pos;
	int comment = 0;

	if (likely(t->cp)) {
		if (likely(immediate))
			c = t->op->nextchar();
		else
			while ((c = t->op->nextchar()) != EOF && c != '\n') {
				if (unlikely(c == '/')) {			/* comment */
					if (likely((c = t->op->nextchar()) == '/')) {
						while ((c = t->op->nextchar()) != EOF)
							if (unlikely(c == '\n')) {
								t->op->pushbackchar();
								break;
							}
					} else if (likely(c == '*')) {
						comment = 1;
						while ((c = t->op->nextchar()) != EOF) {
							if (unlikely(c == '*')) {
								if (unlikely((c = t->op->nextchar()) == '/')) {
									comment = 0; /* cancel comment */
									break;
								}
							} else if (unlikely(c == '\n')) {
								t->op->pushbackchar();
								break;
							}
						}
					} else
						t->op->pushbackchar();
				} else if (likely(!isspace(c)))
					break;
			}
		if (c != EOF)
			t->op->pushbackchar();
		if (likely(c != '\n' || immediate))
			return c;
	}
	pos = ftell(t->ip);
	if (likely(immediate))
		c = getc(t->ip);
	else
		while ((c = getc(t->ip)) != EOF) {
			if (unlikely(comment)) {
				while ((c = getc(t->ip)) != EOF) {
					if (unlikely(c == '*')) {
						if (unlikely((c = getc(t->ip)) == '/')) {
							comment = 0;
							break;
						}
					}
				}
			} else if (unlikely(c == '/')) {			/* comment */
				if (likely((c = getc(t->ip)) == '/')) {
					while ((c = getc(t->ip)) != EOF)
						if (unlikely(c == '\n'))
							break;
				} else if (likely(c == '*')) {
					while ((c = getc(t->ip)) != EOF) {
						if (unlikely(c == '*')) {
							if (unlikely((c = getc(t->ip)) == '/'))
								break;
						}
					}
				} else
					break;
			} else if (likely(!isspace(c)))
				break;
		}
	(void)fseek(t->ip, pos, SEEK_SET);
	return c;
}


static int
def_tokenizer_nexttoken (const char *interested, int (* reserved)(const char *, int))
{
	TOKENIZER *t = CURRENT;
	int c;
	char *ptok;
	int sharp = 0, percent = 0;

/* function internal macros */
#define _tklen (ptok - t->token)
#define _append_tk { \
	ptok    = t->token; \
	*ptok++ = c; \
	*ptok++ = t->op->nextchar(); \
	*ptok   = '\0'; \
}
#define _break_on_rsvd { \
	if (reserved && (c = (*reserved)(t->token, _tklen)) != 0) \
		break; \
}
#define _break_on_norsvd { \
	if (reserved && (c = (*reserved)(t->token, _tklen)) == 0) \
		break; \
}
	if (unlikely(t->ptoken[0] != '\0')) {
		strlimcpy(t->token, t->ptoken, sizeof(t->token));
		t->ptoken[0] = '\0';
		return t->lasttoken;
	}
	for (;;) {
		/* skip spaces */
		if (likely(t->crflag))
			while ((c = t->op->nextchar()) != EOF && isspace(c) && c != '\n')
				;
		else
			while ((c = t->op->nextchar()) != EOF && isspace(c))
				;
		if (unlikely(c == EOF || c == '\n'))
			break;
		/* quoted string */
		if (unlikely(c == '"' || c == '\'')) {
			int quote = c;
			while ((c = t->op->nextchar()) != EOF) {
				if (unlikely(c == quote))
					break;
				if (unlikely(quote == '\'' && c == '\n'))
					break;
				if (unlikely(c == '\\' && (c = t->op->nextchar()) == EOF))
					break;
			}
		}
		/* comment */
		else if (unlikely(c == '/')) {
			if (likely((c = t->op->nextchar()) == '/')) {
				while ((c = t->op->nextchar()) != EOF)
					if (unlikely(c == '\n')) {
						t->op->pushbackchar();
						break;
					}
			} else if (likely(c == '*')) {
				while ((c = t->op->nextchar()) != EOF) {
					if (unlikely(c == '*')) {
						if (unlikely((c = t->op->nextchar()) == '/'))
							break;
						t->op->pushbackchar();
					}
				}
			} else
				t->op->pushbackchar();
		}
		/* continued line */
		else if (unlikely(c == '\\')) {
			if (unlikely(t->op->nextchar() == '\n'))
				t->continued_line = 1;
		}
		/* digit */
		else if (unlikely(isdigit(c))) {
			while ((c = t->op->nextchar()) != EOF && (c == '.' || isalnum(c)))
				;
			t->op->pushbackchar();
		}
		/* '#' like token or reserved words */
		else if (unlikely(c == '#' && t->mode & C_MODE)) {
			// recognize '##' as a token if it is reserved word
			if (unlikely(t->op->peekchar(1) == '#')) {
				_append_tk;
				_break_on_norsvd;
			} else if (unlikely(!t->continued_line && cp_at_first_nonspace(t))) {
				sharp = 1;
				continue;
			}
		}
		/* cpp namespace var */
		else if (unlikely(c == ':' && t->mode & CPP_MODE && t->op->peekchar(1) == ':')) {
			_append_tk;
			_break_on_norsvd;
		}
		/* yacc indicator */
		else if (unlikely(c == '%' && t->mode & Y_MODE)) {
			// recognize '%%' as a token if it is reserved word.
			if (cp_at_first(t)) {
				ptok = t->token;
				*ptok++ = c;
				if (likely((c = t->op->peekchar(1)) == '%' || c == '{' || c == '}')) {
					*ptok++ = t->op->nextchar();
					*ptok   = 0;
					_break_on_rsvd;
				} else if (unlikely(!isspace(c))) {
					percent = 1;
					continue;
				}
			}
		}
		/* symbol */
		else if (likely(c & 0x80 || isalpha(c) || c == '_')) {
			ptok = t->token;
			if (unlikely(sharp)) {
				sharp = 0;
				*ptok++ = '#';
			} else if (unlikely(percent)) {
				percent = 0;
				*ptok++ = '%';
			} else if (unlikely(c == 'L')) {
				int tmp = t->op->peekchar(1);
				if (unlikely(tmp == '"' || tmp == '\''))
					continue;
			}
			for (*ptok++ = c; (c = t->op->nextchar()) != EOF && (c & 0x80 || isalnum(c) || c == '_');) {
				if (_tklen < sizeof(t->token))
					*ptok++ = c;
			}
			if (unlikely(_tklen == sizeof(t->token))) {
				warning("symbol name is too long. (Ignored) [+%d %s]", t->lineno, t->path);
				t->token[0] = '\0';
				continue;
			}
			*ptok = 0; // end of symbol
			if (likely(c != EOF))
				t->op->pushbackchar();
			/* convert token string into token number */
			c = SYMBOL;
			if (reserved)
				c = (*reserved)(t->token, _tklen);
			break;
		}
/* remove function internal macros */
#undef _append_tk
#undef _break_on_rsvd
#undef _break_on_norsvd
#undef _tklen
		/* special char */
		else {
			if (unlikely(interested == NULL || strchr(interested, c)))
				break;
			/* otherwise ignore it */
		}
		sharp = percent = 0;
	}
	return (t->lasttoken = c);
}
