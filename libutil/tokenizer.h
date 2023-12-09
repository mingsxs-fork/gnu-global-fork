/*
 * Copyright (c) 1997, 1998, 1999, 2000, 2001, 2008, 2011, 2014
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

#ifndef _TOKENIZER_H_
#define _TOKENIZER_H_

#include <ctype.h>
#include "gparam.h"

#include "strbuf.h"
#include "gtags_helper.h"

#define SYMBOL		0
#define C_MODE		1
#define CPP_MODE	2
#define Y_MODE		4


typedef struct tokenizer TOKENIZER;

/* ops that we want to exposed to lang parser functions
 * or customizable by them inversely
 */
struct tokenizer_ops {
	int (*nextchar)
		(void);
	int (*nexttoken)
		(const char *, int (*)(const char *, int));
	void (*pushbackchar)
		(void);
	void (*pushbacktoken)
		(void);
	int (*peekchar)
		(int);
	void (*skipnext)
		(int);
	int (*expectcharset)
		(const char *, STRBUF *);
};

struct tokenizer {
/* tokenizer internal states */
	char token[MAXTOKEN];
	char ptoken[MAXTOKEN];
	const char *sp;
	const char *cp;
	const char *lp;
	int crflag;
	int mode;
	int continued_line;
	int lasttoken;
	int lineno;
	FILE *ip;
	STRBUF *ib;
	const struct gtags_path *gpath;
	void *lang_priv;	/* language private data */

/* tokenizer ops */
	struct tokenizer_ops *op;
};


TOKENIZER *tokenizer_open(const struct gtags_path *, struct tokenizer_ops *, void *);
void tokenizer_close(TOKENIZER *);
TOKENIZER *current_tokenizer(void);
int opened_tokenizers(void);

#define cp_at_first(t) (t->sp && t->sp == (t->cp ? t->cp - 1 : t->lp))

#if 0
#define cp_at_first_nonspace(t) ({ \
		const char *__sp = t->sp; \
		const char *__ep = t->cp ? (t->cp - 1) : t->lp; \
		while (__sp < __ep && *__sp && isspace(*__sp)) \
			*__sp++; \
		(__sp == __cp) ? 1 : 0; \
		})
#endif

#endif /* ! _TOKENIZER_H_ */
