/*
 * Copyright (c) 1998, 1999, 2000, 2001, 2002, 2003, 2004, 2005, 2008,
 *	2010, 2015
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
#ifdef HAVE_STRING_H
#include <string.h>
#else
#include <strings.h>
#endif

#include "internal.h"
#include "die.h"
#include "strbuf.h"
#include "strlimcpy.h"
#include "tokenizer.h"
#include "c_res.h"
#include "gtags_helper.h"
#include "path_tree.h"
#include "gtagsop.h"

static void C_family(const struct parser_param *, int);
static void process_attribute(const struct parser_param *);
static int function_definition(const struct parser_param *, char *);
static void condition_macro(const struct parser_param *, int);
static int enumerator_list(const struct parser_param *);
static void process_inc_headers(const struct parser_param *, const char *);

#define IS_TYPE_QUALIFIER(c)	((c) == C_CONST || (c) == C_RESTRICT || (c) == C_VOLATILE)

#define DECLARATIONS    0
#define RULES           1
#define PROGRAMS        2

#define TYPE_C		0
#define TYPE_LEX	1
#define TYPE_YACC	2

#define MAXPIFSTACK	100

#if 0
/*
 * #ifdef stack.
 */
static struct {
	short start;		/* level when '#if' block started */
	short end;		/* level when '#if' block end */
	short if0only;		/* '#if 0' or notdef only */
} stack[MAXPIFSTACK], *cur;
static int piflevel;		/* condition macro level */
static int level;		/* brace level */
static int externclevel;	/* 'extern "C"' block level */
#endif

/* lang parser local env on stack */
struct _lang_local {
	struct _lang_stack {
		short start;
		short end;
		short if0only;
	} stack[MAXPIFSTACK];
	struct _lang_stack *curstack;
	int piflevel;
	int level;
	int externclevel;
}

TOKENIZER *tokenizer;

/**
 * yacc: read yacc file and pickup tag entries.
 */
void
yacc(const struct parser_param *param)
{
	C_family(param, TYPE_YACC);
}
/**
 * C: read C file and pickup tag entries.
 */
void
C(const struct parser_param *param)
{
	C_family(param, TYPE_C);
}
/**
 *	@param[in]	param	source file
 *	@param[in]	type	TYPE_C, TYPE_YACC, TYPE_LEX
 */
static void
C_family(const struct parser_param *param, int type)
{
	int c, cc;
	int savelevel;
	int startmacro, startsharp;
	const char *interested = "{}=;";
	STRBUF *sb = strbuf_open(0);
	struct _lang_local local;
	/*
	 * yacc file format is like the following.
	 *
	 * declarations
	 * %%
	 * rules
	 * %%
	 * programs
	 *
	 */
	int yaccstatus = (type == TYPE_YACC) ? DECLARATIONS : PROGRAMS;
	int inC = (type == TYPE_YACC) ? 0 : 1;	/* 1 while C source */

	local.level = local.piflevel = local.externclevel = 0;
	savelevel = -1;
	startmacro = startsharp = 0;

	if (!tokenizer_open(param->file, NULL, &local))
		die("'%s' cannot open.", param->file);
	tokenizer = current_tokenizer();  /* get current tokenizer for this file */
	tokenizer->mode |= C_MODE;  /* allow token like '#xxx' */
	tokenizer->crflag = 1;  /* require '\n' as a token */
	if (type == TYPE_YACC)
		tokenizer->mode |= Y_MODE;  /* allow token like '%xxx' */

	while ((cc = tokenizer->op->nexttoken(interested, c_reserved_word)) != EOF) {
		switch (cc) {
		case SYMBOL:		/* symbol	*/
			if (inC && tokenizer->op->peekchar(0) == '('/* ) */) {
				if (param->isnotfunction(tokenizer->token)) {
					PUT(PARSER_REF_SYM, tokenizer->token, tokenizer->lineno, tokenizer->sp);
				} else if (local.level > 0 || startmacro) {
					PUT(PARSER_REF_SYM, tokenizer->token, tokenizer->lineno, tokenizer->sp);
				} else if (local.level == 0 && !startmacro && !startsharp) {
					char arg1[MAXTOKEN], savetok[MAXTOKEN], *saveline;
					int savelineno = tokenizer->lineno;

					strlimcpy(savetok, tokenizer->token, sizeof(savetok));
					strbuf_reset(sb);
					strbuf_puts(sb, tokenizer->sp);
					saveline = strbuf_value(sb);
					arg1[0] = '\0';
					/*
					 * Guile function entry using guile-snarf is like follows:
					 *
					 * SCM_DEFINE (scm_list, "list", 0, 0, 1, 
					 *           (SCM objs),
					 *            "Return a list containing OBJS, the arguments to `list'.")
					 * #define FUNC_NAME s_scm_list
					 * {
					 *   return objs;
					 * }
					 * #undef FUNC_NAME
					 *
					 * We should assume the first argument as a function name instead of 'SCM_DEFINE'.
					 */
					if (function_definition(param, arg1)) {
						if (!strcmp(savetok, "SCM_DEFINE") && *arg1)
							strlimcpy(savetok, arg1, sizeof(savetok));
						PUT(PARSER_DEF, savetok, savelineno, saveline);
					} else {
						PUT(PARSER_REF_SYM, savetok, savelineno, saveline);
					}
				}
			} else {
				PUT(PARSER_REF_SYM, tokenizer->token, tokenizer->lineno, tokenizer->sp);
			}
			break;
		case '{':  /* } */
			DBG_PRINT(local.level, "{"); /* } */
			if (yaccstatus == RULES && local.level == 0)
				inC = 1;
			++(local.level);
			if ((param->flags & PARSER_BEGIN_BLOCK) && atfirst) {
				if ((param->flags & PARSER_WARNING) && local.level != 1)
					warning("forced level 1 block start by '{' at column 0 [+%d %s].", tokenizer->lineno, tokenizer->path); /* } */
				local.level = 1;
			}
			break;
			/* { */
		case '}':
			if (--(local.level) < 0) {
				if (local.externclevel > 0)
					local.externclevel--;
				else if (param->flags & PARSER_WARNING)
					warning("missing left '{' [+%d %s].", tokenizer->lineno, tokenizer->path); /* } */
				local.level = 0;
			}
			if ((param->flags & PARSER_END_BLOCK) && atfirst) {
				if ((param->flags & PARSER_WARNING) && local.level != 0) /* { */
					warning("forced level 0 block end by '}' at column 0 [+%d %s].", tokenizer->lineno, tokenizer->path);
				local.level = 0;
			}
			if (yaccstatus == RULES && local.level == 0)
				inC = 0;
			/* { */
			DBG_PRINT(local.level, "}");
			break;
		case '\n':
			if (startmacro && local.level != savelevel) {
				if (param->flags & PARSER_WARNING)
					warning("different level before and after #define macro. reseted. [+%d %s].", tokenizer->lineno, tokenizer->path);
				local.level = savelevel;
			}
			startmacro = startsharp = 0;
			break;
		case YACC_SEP:		/* %% */
			if (local.level != 0) {
				if (param->flags & PARSER_WARNING)
					warning("forced level 0 block end by '%%' [+%d %s].", tokenizer->lineno, tokenizer->path);
				local.level = 0;
			}
			if (yaccstatus == DECLARATIONS) {
				PUT(PARSER_DEF, "yyparse", tokenizer->lineno, tokenizer->sp);
				yaccstatus = RULES;
			} else if (yaccstatus == RULES)
				yaccstatus = PROGRAMS;
			inC = (yaccstatus == PROGRAMS) ? 1 : 0;
			break;
		case YACC_BEGIN:	/* %{ */
			if (local.level != 0) {
				if (param->flags & PARSER_WARNING)
					warning("forced level 0 block end by '%%{' [+%d %s].", tokenizer->lineno, tokenizer->path);
				local.level = 0;
			}
			if (inC == 1 && (param->flags & PARSER_WARNING))
				warning("'%%{' appeared in C mode. [+%d %s].", tokenizer->lineno, tokenizer->path);
			inC = 1;
			break;
		case YACC_END:		/* %} */
			if (local.level != 0) {
				if (param->flags & PARSER_WARNING)
					warning("forced level 0 block end by '%%}' [+%d %s].", tokenizer->lineno, tokenizer->path);
				local.level = 0;
			}
			if (inC == 0 && (param->flags & PARSER_WARNING))
				warning("'%%}' appeared in Yacc mode. [+%d %s].", tokenizer->lineno, tokenizer->path);
			inC = 0;
			break;
		case YACC_UNION:	/* %union {...} */
			if (yaccstatus == DECLARATIONS)
				PUT(PARSER_DEF, "YYSTYPE", tokenizer->lineno, tokenizer->sp);
			break;
		/*
		 * #xxx
		 */
		case SHARP_DEFINE:
		case SHARP_UNDEF:
			startmacro = 1;
			savelevel = local.level;
			if ((c = tokenizer->op->nexttoken(interested, c_reserved_word)) != SYMBOL) {
				tokenizer->op->pushbacktoken();
				break;
			}
			if (tokenizer->op->peekchar(1) == '('/* ) */) {
				PUT(PARSER_DEF, tokenizer->token, tokenizer->lineno, tokenizer->sp);
				while ((c = tokenizer->op->nexttoken("()", c_reserved_word)) != EOF && c != '\n' && c != /* ( */ ')')
					if (c == SYMBOL)
						PUT(PARSER_REF_SYM, tokenizer->token, tokenizer->lineno, tokenizer->sp);
				if (c == '\n')
					tokenizer->op->pushbacktoken();
			} else {
				PUT(PARSER_DEF, tokenizer->token, tokenizer->lineno, tokenizer->sp);
			}
			break;
		case SHARP_INCLUDE:
		case SHARP_INCLUDE_NEXT:
			/* process included headers */
			strbuf_clear(sb);
			/* skip to certain charset */
			(void)tokenizer->op->expectcharset("\"<", NULL);
			if ((c = tokenizer->op->expectcharset("\">", sb)) != EOF && c != '\n')
				process_inc_headers(param, strbuf_value(sb));
			break;
		case SHARP_IMPORT:
		case SHARP_ERROR:
		case SHARP_LINE:
		case SHARP_PRAGMA:
		case SHARP_WARNING:
		case SHARP_IDENT:
		case SHARP_SCCS:
			while ((c = tokenizer->op->nexttoken(interested, c_reserved_word)) != EOF && c != '\n')
				;
			break;
		case SHARP_IFDEF:
		case SHARP_IFNDEF:
		case SHARP_IF:
		case SHARP_ELIF:
		case SHARP_ELSE:
		case SHARP_ENDIF:
			condition_macro(param, cc);
			break;
		case SHARP_SHARP:		/* ## */
			(void)tokenizer->op->nexttoken(interested, c_reserved_word);
			break;
		case C_EXTERN: /* for 'extern "C"/"C++"' */
			if (tokenizer->op->peekchar(0) != '"') /* " */
				continue; /* If does not start with '"', continue. */
			while ((c = tokenizer->op->nexttoken(interested, c_reserved_word)) == '\n')
				;
			/*
			 * 'extern "C"/"C++"' block is a kind of namespace block.
			 * (It doesn't have any influence on level.)
			 */
			if (c == '{') /* } */
				local.externclevel++;
			else
				tokenizer->op->pushbacktoken();
			break;
		case C_STRUCT:
		case C_ENUM:
		case C_UNION:
			while ((c = tokenizer->op->nexttoken(interested, c_reserved_word)) == C___ATTRIBUTE__)
				process_attribute(param);
			while (c == '\n')
				c = tokenizer->op->nexttoken(interested, c_reserved_word);
			if (c == SYMBOL) {
				if (tokenizer->op->peekchar(0) == '{') /* } */ {
					PUT(PARSER_DEF, tokenizer->token, tokenizer->lineno, tokenizer->sp);
				} else {
					PUT(PARSER_REF_SYM, tokenizer->token, tokenizer->lineno, tokenizer->sp);
				}
				c = tokenizer->op->nexttoken(interested, c_reserved_word);
			}
			while (c == '\n')
				c = tokenizer->op->nexttoken(interested, c_reserved_word);
			if (c == '{' /* } */ && cc == C_ENUM) {
				enumerator_list(param);
			} else {
				tokenizer->op->pushbacktoken();
			}
			break;
		/* control statement check */
		case C_BREAK:
		case C_CASE:
		case C_CONTINUE:
		case C_DEFAULT:
		case C_DO:
		case C_ELSE:
		case C_FOR:
		case C_GOTO:
		case C_IF:
		case C_RETURN:
		case C_SWITCH:
		case C_WHILE:
			if ((param->flags & PARSER_WARNING) && !startmacro && local.level == 0)
				warning("Out of function. %8s [+%d %s]", tokenizer->token, tokenizer->lineno, tokenizer->path);
			break;
		case C_TYPEDEF:
			{
				/*
				 * This parser is too complex to maintain.
				 * We should rewrite the whole.
				 */
				char savetok[MAXTOKEN];
				int savelineno = 0;
				int typedef_savelevel = local.level;

				savetok[0] = 0;

				/* skip type qualifiers */
				do {
					c = tokenizer->op->nexttoken("{}(),;", c_reserved_word);
				} while (IS_TYPE_QUALIFIER(c) || c == '\n');

				if ((param->flags & PARSER_WARNING) && c == EOF) {
					warning("unexpected eof. [+%d %s]", tokenizer->lineno, tokenizer->path);
					break;
				} else if (c == C_ENUM || c == C_STRUCT || c == C_UNION) {
					char *interest_enum = "{},;";
					int c_ = c;

					while ((c = tokenizer->op->nexttoken(interest_enum, c_reserved_word)) == C___ATTRIBUTE__)
						process_attribute(param);
					while (c == '\n')
						c = tokenizer->op->nexttoken(interest_enum, c_reserved_word);
					/* read tag name if exist */
					if (c == SYMBOL) {
						if (tokenizer->op->peekchar(0) == '{') /* } */ {
							PUT(PARSER_DEF, tokenizer->token, tokenizer->lineno, tokenizer->sp);
						} else {
							PUT(PARSER_REF_SYM, tokenizer->token, tokenizer->lineno, tokenizer->sp);
						}
						c = tokenizer->op->nexttoken(interest_enum, c_reserved_word);
					}
					while (c == '\n')
						c = tokenizer->op->nexttoken(interest_enum, c_reserved_word);
					if (c_ == C_ENUM) {
						if (c == '{') /* } */
							c = enumerator_list(param);
						else
							tokenizer->op->pushbacktoken();
					} else {
						for (; c != EOF; c = tokenizer->op->nexttoken(interest_enum, c_reserved_word)) {
							switch (c) {
							case SHARP_IFDEF:
							case SHARP_IFNDEF:
							case SHARP_IF:
							case SHARP_ELIF:
							case SHARP_ELSE:
							case SHARP_ENDIF:
								condition_macro(param, c);
								continue;
							default:
								break;
							}
							if (c == ';' && local.level == typedef_savelevel) {
								if (savetok[0]) {
									PUT(PARSER_DEF, savetok, savelineno, tokenizer->sp);
									savetok[0] = 0;
								}
								break;
							} else if (c == '{')
								local.level++;
							else if (c == '}') {
								savetok[0] = 0;
								if (--(local.level) == typedef_savelevel)
									break;
							} else if (c == SYMBOL) {
								if (local.level > typedef_savelevel)
									PUT(PARSER_REF_SYM, tokenizer->token, tokenizer->lineno, tokenizer->sp);
								/* save lastest token */
								strlimcpy(savetok, tokenizer->token, sizeof(savetok));
								savelineno = tokenizer->lineno;
							}
						}
						if (c == ';')
							break;
					}
					if ((param->flags & PARSER_WARNING) && c == EOF) {
						warning("unexpected eof. [+%d %s]", tokenizer->lineno, tokenizer->path);
						break;
					}
				} else if (c == SYMBOL) {
					PUT(PARSER_REF_SYM, tokenizer->token, tokenizer->lineno, tokenizer->sp);
				}
				savetok[0] = 0;
				while ((c = tokenizer->op->nexttoken("(),;", c_reserved_word)) != EOF) {
					switch (c) {
					case SHARP_IFDEF:
					case SHARP_IFNDEF:
					case SHARP_IF:
					case SHARP_ELIF:
					case SHARP_ELSE:
					case SHARP_ENDIF:
						condition_macro(param, c);
						continue;
					default:
						break;
					}
					if (c == '(')
						local.level++;
					else if (c == ')')
						local.level--;
					else if (c == SYMBOL) {
						if (local.level > typedef_savelevel) {
							PUT(PARSER_REF_SYM, tokenizer->token, tokenizer->lineno, tokenizer->sp);
						} else {
							/* put latest token if any */
							if (savetok[0]) {
								PUT(PARSER_REF_SYM, savetok, savelineno, tokenizer->sp);
							}
							/* save lastest token */
							strlimcpy(savetok, tokenizer->token, sizeof(savetok));
							savelineno = tokenizer->lineno;
						}
					} else if (c == ',' || c == ';') {
						if (savetok[0]) {
							PUT(PARSER_DEF, savetok, tokenizer->lineno, tokenizer->sp);
							savetok[0] = 0;
						}
					}
					if (local.level == typedef_savelevel && c == ';')
						break;
				}
				if (param->flags & PARSER_WARNING) {
					if (c == EOF)
						warning("unexpected eof. [+%d %s]", tokenizer->lineno, tokenizer->path);
					else if (local.level != typedef_savelevel)
						warning("unmatched () block. (last at level %d.)[+%d %s]", local.level, tokenizer->lineno, tokenizer->path);
				}
			}
			break;
		case C___ATTRIBUTE__:
			process_attribute(param);
			break;
		default:
			break;
		}
	}
	strbuf_close(sb);
	if (param->flags & PARSER_WARNING) {
		if (local.level != 0)
			warning("unmatched {} block. (last at level %d.)[+%d %s]", local.level, tokenizer->lineno, tokenizer->path);
		if (local.piflevel != 0)
			warning("unmatched #if block. (last at level %d.)[+%d %s]", local.piflevel, tokenizer->lineno, tokenizer->path);
	}
	/* close current tokenizer */
	tokenizer_close(tokenizer);
	/* reset current tokenizer */
	tokenizer = current_tokenizer;
}
/**
 * process_attribute: skip attributes in '__attribute__((...))'.
 */
static void
process_attribute(const struct parser_param *param)
{
	int brace = 0;
	int c;
	/*
	 * Skip '...' in __attribute__((...))
	 * but pick up symbols in it.
	 */
	while ((c = tokenizer->op->nexttoken("()", c_reserved_word)) != EOF) {
		if (c == '(')
			brace++;
		else if (c == ')')
			brace--;
		else if (c == SYMBOL) {
			PUT(PARSER_REF_SYM, tokenizer->token, tokenizer->lineno, tokenizer->sp);
		}
		if (brace == 0)
			break;
	}
}
/**
 * function_definition: return if function definition or not.
 *
 *	@param	param	
 *	@param[out]	arg1	the first argument
 *	@return	target type
 */
static int
function_definition(const struct parser_param *param, char arg1[MAXTOKEN])
{
	int c;
	int brace_level, isdefine;
	int accept_arg1 = 0;

	brace_level = isdefine = 0;
	while ((c = tokenizer->op->nexttoken("()", c_reserved_word)) != EOF) {
		switch (c) {
		case SHARP_IFDEF:
		case SHARP_IFNDEF:
		case SHARP_IF:
		case SHARP_ELIF:
		case SHARP_ELSE:
		case SHARP_ENDIF:
			condition_macro(param, c);
			continue;
		default:
			break;
		}
		if (c == '('/* ) */)
			brace_level++;
		else if (c == /* ( */')') {
			if (--brace_level == 0)
				break;
		}
		/* pick up symbol */
		if (c == SYMBOL) {
			if (accept_arg1 == 0) {
				accept_arg1 = 1;
				strlimcpy(arg1, tokenizer->token, MAXTOKEN);
			}
			PUT(PARSER_REF_SYM, tokenizer->token, tokenizer->lineno, tokenizer->sp);
		}
	}
	if (c == EOF)
		return 0;
	brace_level = 0;
	while ((c = tokenizer->op->nexttoken(",;[](){}=", c_reserved_word)) != EOF) {
		switch (c) {
		case SHARP_IFDEF:
		case SHARP_IFNDEF:
		case SHARP_IF:
		case SHARP_ELIF:
		case SHARP_ELSE:
		case SHARP_ENDIF:
			condition_macro(param, c);
			continue;
		case C___ATTRIBUTE__:
			process_attribute(param);
			continue;
		case SHARP_DEFINE:
			tokenizer->op->pushbacktoken();
			return 0;
		default:
			break;
		}
		if (c == '('/* ) */ || c == '[')
			brace_level++;
		else if (c == /* ( */')' || c == ']')
			brace_level--;
		else if (brace_level == 0
		    && ((c == SYMBOL && strcmp(tokenizer->token, "__THROW")) || IS_RESERVED_WORD(c)))
			isdefine = 1;
		else if (c == ';' || c == ',') {
			if (!isdefine)
				break;
		} else if (c == '{' /* } */) {
			tokenizer->op->pushbacktoken();
			return 1;
		} else if (c == /* { */'}')
			break;
		else if (c == '=')
			break;

		/* pick up symbol */
		if (c == SYMBOL)
			PUT(PARSER_REF_SYM, tokenizer->token, tokenizer->lineno, tokenizer->sp);
	}
	return 0;
}

/**
 * condition_macro: 
 *
 *	@param	param	
 *	@param[in]	cc	token
 */
static void
condition_macro(const struct parser_param *param, int cc)
{
	struct _lang_local *plocal = tokenizer->lang_priv;
	plocal->curstack = &plocal->stack[plocal->piflevel];
	if (cc == SHARP_IFDEF || cc == SHARP_IFNDEF || cc == SHARP_IF) {
		DBG_PRINT(plocal->piflevel, "#if");
		if (++(plocal->piflevel) >= MAXPIFSTACK)
			die("#if stack over flow. [%s]", tokenizer->path);
		++(plocal->curstack);
		plocal->curstack->start = plocal->level;
		plocal->curstack->end = -1;
		plocal->curstack->if0only = 0;
		if (tokenizer->op->peekchar(0) == '0')
			plocal->curstack->if0only = 1;
		else if ((cc = tokenizer->op->nexttoken(NULL, c_reserved_word)) == SYMBOL && !strcmp(tokenizer->token, "notdef"))
			plocal->curstack->if0only = 1;
		else
			tokenizer->op->pushbacktoken();
	} else if (cc == SHARP_ELIF || cc == SHARP_ELSE) {
		DBG_PRINT(plocal->piflevel - 1, "#else");
		if (plocal->curstack->end == -1)
			plocal->curstack->end = plocal->level;
		else if (plocal->curstack->end != plocal->level && (param->flags & PARSER_WARNING))
			warning("uneven level. [+%d %s]", tokenizer->lineno, tokenizer->path);
		plocal->level = plocal->curstack->start;
		plocal->curstack->if0only = 0;
	} else if (cc == SHARP_ENDIF) {
		int minus = 0;

		--(plocal->piflevel);
		if (plocal->piflevel < 0) {
			minus = 1;
			plocal->piflevel = 0;
		}
		DBG_PRINT(plocal->piflevel, "#endif");
		if (minus) {
			if (param->flags & PARSER_WARNING)
				warning("unmatched #if block. reseted. [+%d %s]", tokenizer->lineno, tokenizer->path);
		} else {
			if (plocal->curstack->if0only)
				plocal->level = plocal->curstack->start;
			else if (plocal->curstack->end != -1) {
				if (plocal->curstack->end != plocal->level && (param->flags & PARSER_WARNING))
					warning("uneven level. [+%d %s]", tokenizer->lineno, tokenizer->path);
				plocal->level = plocal->curstack->end;
			}
		}
	}
	while ((cc = tokenizer->op->nexttoken(NULL, c_reserved_word)) != EOF && cc != '\n') {
		if (cc == SYMBOL && strcmp(tokenizer->token, "defined") != 0)
			PUT(PARSER_REF_SYM, tokenizer->token, tokenizer->lineno, tokenizer->sp);
	}
}

/**
 * enumerator_list: process "symbol (= expression), ... "}
 */
static int
enumerator_list(const struct parser_param *param)
{
	int savelevel = level;
	int in_expression = 0;
	int c = '{';

	for (; c != EOF; c = tokenizer->op->nexttoken("{}(),=", c_reserved_word)) {
		switch (c) {
		case SHARP_IFDEF:
		case SHARP_IFNDEF:
		case SHARP_IF:
		case SHARP_ELIF:
		case SHARP_ELSE:
		case SHARP_ENDIF:
			condition_macro(param, c);
			break;
		case SYMBOL:
			if (in_expression)
				PUT(PARSER_REF_SYM, tokenizer->token, tokenizer->lineno, tokenizer->sp);
			else
				PUT(PARSER_DEF, tokenizer->token, tokenizer->lineno, tokenizer->sp);
			break;
		case '{':
		case '(':
			level++;
			break;
		case '}':
		case ')':
			if (--level == savelevel)
				return c;
			break;
		case ',':
			if (level == savelevel + 1)
				in_expression = 0;
			break;
		case '=':
			in_expression = 1;
			break;
		default:
			break;
		}
	}

	return c;
}

/**
 * process_inc_headers: process included headers, make them parsed firstly
 */
void
process_inc_headers(const struct parser_param *param, const char *header)
{
	const char *p = header + strlen(header);
	char sep = '/';
#if (defined(_WIN32) && !defined(__CYGWIN__)) || defined(__DJGPP__)
	if (strchr(header, '\\') != NULL)
		sep = '\\';
#endif
	/* reversely find the sep to get header filename */
	while (p-- > header && *p != sep);
	++p;
	path_tree_search_name(p, param->arg);
}
