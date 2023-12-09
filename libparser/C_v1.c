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
#include <stdio.h>
#ifdef HAVE_STRING_H
#include <string.h>
#else
#include <strings.h>
#endif

#include "internal.h"
#include "die.h"
#include "c_res.h"
#include "strbuf.h"
#include "strlimcpy.h"
#include "checkalloc.h"
#include "tokenizer.h"
#include "path.h"
#include "gtags_helper.h"

static void C_family(const struct parser_param *, int);
static void process_attribute(const struct parser_param *);
static int function_definition(const struct parser_param *, char *);
static void condition_macro(const struct parser_param *, int);
static int enumerator_list(const struct parser_param *);

#define IS_TYPE_QUALIFIER(c)	((c) == C_CONST || (c) == C_RESTRICT || (c) == C_VOLATILE)

#define DECLARATIONS    0
#define RULES           1
#define PROGRAMS        2

#define TYPE_C		0
#define TYPE_LEX	1
#define TYPE_YACC	2

#define MAXPIFSTACK	100

/* lang parser local env on stack */
struct _lang_local {
	struct _lang_stack {
		short start;
		short end;
		short if0only;
	} ifstack[MAXPIFSTACK];
	struct _lang_stack *curstack;
	int piflevel;
	int level;
	int externclevel;
};

static TOKENIZER *T = NULL; /* current tokenizer */

/**
 * yacc: read yacc file and pickup tag entries.
 */
void
yacc(const struct parser_param *param)
{
	static int parse_level = 0;
	struct gtags_priv_data *priv_data = param->arg;
	char *sign;
	if (priv_data->gconf.vflag) {
		sign = check_malloc(++parse_level);
		memset(sign, '-', parse_level);
		*(sign + parse_level - 1) = '\0';
		fprintf(stderr, " [%d] %s>START extracting YACC tags of %s\n", param->gpath->seq, sign, trimpath(param->gpath->path));
	}
	C_family(param, TYPE_YACC);
	if (priv_data->gconf.vflag) {
		fprintf(stderr, " [%d] %s>END extracting YACC tags of %s\n", param->gpath->seq, sign, trimpath(param->gpath->path));
		free(sign);
		--parse_level;
	}

}
/**
 * C: read C file and pickup tag entries.
 */
void
C(const struct parser_param *param)
{
	static int parse_level = 0;
	struct gtags_priv_data *priv_data = param->arg;
	char *sign;
	if (priv_data->gconf.vflag) {
		sign = check_malloc(++parse_level);
		memset(sign, '-', parse_level);
		*(sign + parse_level - 1) = '\0';
		fprintf(stderr, " [%d] %s>START extracting C tags of %s\n", param->gpath->seq, sign,trimpath(param->gpath->path));
	}
	C_family(param, TYPE_C);
	if (priv_data->gconf.vflag) {
		fprintf(stderr, " [%d] %s>END extracting C tags of %s\n", param->gpath->seq, sign, trimpath(param->gpath->path));
		free(sign);
		--parse_level;
	}
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
	STRBUF *sb = strbuf_pool_assign(0);
	struct _lang_local _local = {0};
	struct _lang_local *local = &_local;
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

	savelevel = -1;
	startmacro = startsharp = 0;

	if ((T = tokenizer_open(param->gpath, NULL, local)) == NULL)
		die("'%s' cannot open.", param->gpath->path);
	T->mode |= C_MODE;  /* allow token like '#xxx' */
	T->crflag = 1;  /* require '\n' as a token */
	if (type == TYPE_YACC)
		T->mode |= Y_MODE;  /* allow token like '%xxx' */

	while ((cc = T->op->nexttoken(interested, c_reserved_word)) != EOF) {
		switch (cc) {
		case SYMBOL:		/* symbol	*/
			if (inC && T->op->peekchar(0) == '('/* ) */) {
				if (param->isnotfunction(T->token)) {
					PUT(PARSER_REF_SYM, T->token, T->lineno, T->sp);
				} else if (local->level > 0 || startmacro) {
					PUT(PARSER_REF_SYM, T->token, T->lineno, T->sp);
				} else if (local->level == 0 && !startmacro && !startsharp) {
					char arg1[MAXTOKEN], savetok[MAXTOKEN], *saveline;
					int savelineno = T->lineno;

					strlimcpy(savetok, T->token, sizeof(savetok));
					strbuf_reset(sb);
					strbuf_puts(sb, T->sp);
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
				PUT(PARSER_REF_SYM, T->token, T->lineno, T->sp);
			}
			break;
		case '{':  /* } */
			DBG_PRINT(local->level, "{"); /* } */
			if (yaccstatus == RULES && local->level == 0)
				inC = 1;
			++local->level;
			if ((param->flags & PARSER_BEGIN_BLOCK) && cp_at_first(T)) {
				if ((param->flags & PARSER_WARNING) && local->level != 1)
					warning("forced level 1 block start by '{' at column 0 [+%d %s].", T->lineno, T->gpath->path); /* } */
				local->level = 1;
			}
			break;
			/* { */
		case '}':
			if (--local->level < 0) {
				if (local->externclevel > 0)
					local->externclevel--;
				else if (param->flags & PARSER_WARNING)
					warning("missing left '{' [+%d %s].", T->lineno, T->gpath->path); /* } */
				local->level = 0;
			}
			if ((param->flags & PARSER_END_BLOCK) && cp_at_first(T)) {
				if ((param->flags & PARSER_WARNING) && local->level != 0) /* { */
					warning("forced level 0 block end by '}' at column 0 [+%d %s].", T->lineno, T->gpath->path);
				local->level = 0;
			}
			if (yaccstatus == RULES && local->level == 0)
				inC = 0;
			/* { */
			DBG_PRINT(local->level, "}");
			break;
		case '\n':
			if (startmacro && local->level != savelevel) {
				if (param->flags & PARSER_WARNING)
					warning("different level before and after #define macro. reseted. [+%d %s].", T->lineno, T->gpath->path);
				local->level = savelevel;
			}
			startmacro = startsharp = 0;
			break;
		case YACC_SEP:		/* %% */
			if (local->level != 0) {
				if (param->flags & PARSER_WARNING)
					warning("forced level 0 block end by '%%' [+%d %s].", T->lineno, T->gpath->path);
				local->level = 0;
			}
			if (yaccstatus == DECLARATIONS) {
				PUT(PARSER_DEF, "yyparse", T->lineno, T->sp);
				yaccstatus = RULES;
			} else if (yaccstatus == RULES)
				yaccstatus = PROGRAMS;
			inC = (yaccstatus == PROGRAMS) ? 1 : 0;
			break;
		case YACC_BEGIN:	/* %{ */
			if (local->level != 0) {
				if (param->flags & PARSER_WARNING)
					warning("forced level 0 block end by '%%{' [+%d %s].", T->lineno, T->gpath->path);
				local->level = 0;
			}
			if (inC == 1 && (param->flags & PARSER_WARNING))
				warning("'%%{' appeared in C mode. [+%d %s].", T->lineno, T->gpath->path);
			inC = 1;
			break;
		case YACC_END:		/* %} */
			if (local->level != 0) {
				if (param->flags & PARSER_WARNING)
					warning("forced level 0 block end by '%%}' [+%d %s].", T->lineno, T->gpath->path);
				local->level = 0;
			}
			if (inC == 0 && (param->flags & PARSER_WARNING))
				warning("'%%}' appeared in Yacc mode. [+%d %s].", T->lineno, T->gpath->path);
			inC = 0;
			break;
		case YACC_UNION:	/* %union {...} */
			if (yaccstatus == DECLARATIONS)
				PUT(PARSER_DEF, "YYSTYPE", T->lineno, T->sp);
			break;
		/*
		 * #xxx
		 */
		case SHARP_DEFINE:
		case SHARP_UNDEF:
			startmacro = 1;
			savelevel = local->level;
			if ((c = T->op->nexttoken(interested, c_reserved_word)) != SYMBOL) {
				T->op->pushbacktoken();
				break;
			}
			if (T->op->peekchar(1) == '('/* ) */) {
				PUT(PARSER_DEF, T->token, T->lineno, T->sp);
				while ((c = T->op->nexttoken("()", c_reserved_word)) != EOF && c != '\n' && c != /* ( */ ')')
					if (c == SYMBOL)
						PUT(PARSER_REF_SYM, T->token, T->lineno, T->sp);
				if (c == '\n')
					T->op->pushbacktoken();
			} else {
				PUT(PARSER_DEF, T->token, T->lineno, T->sp);
			}
			break;
		case SHARP_INCLUDE:
		case SHARP_INCLUDE_NEXT:
		case SHARP_IMPORT:
		case SHARP_ERROR:
		case SHARP_LINE:
		case SHARP_PRAGMA:
		case SHARP_WARNING:
		case SHARP_IDENT:
		case SHARP_SCCS:
			while ((c = T->op->nexttoken(interested, c_reserved_word)) != EOF && c != '\n')
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
			(void)T->op->nexttoken(interested, c_reserved_word);
			break;
		case C_EXTERN: /* for 'extern "C"/"C++"' */
			if (T->op->peekchar(0) != '"') /* " */
				continue; /* If does not start with '"', continue. */
			while ((c = T->op->nexttoken(interested, c_reserved_word)) == '\n')
				;
			/*
			 * 'extern "C"/"C++"' block is a kind of namespace block.
			 * (It doesn't have any influence on level.)
			 */
			if (c == '{') /* } */
				local->externclevel++;
			else
				T->op->pushbacktoken();
			break;
		case C_STRUCT:
		case C_ENUM:
		case C_UNION:
			while ((c = T->op->nexttoken(interested, c_reserved_word)) == C___ATTRIBUTE__)
				process_attribute(param);
			while (c == '\n')
				c = T->op->nexttoken(interested, c_reserved_word);
			if (c == SYMBOL) {
				if (T->op->peekchar(0) == '{') /* } */ {
					PUT(PARSER_DEF, T->token, T->lineno, T->sp);
				} else {
					PUT(PARSER_REF_SYM, T->token, T->lineno, T->sp);
				}
				c = T->op->nexttoken(interested, c_reserved_word);
			}
			while (c == '\n')
				c = T->op->nexttoken(interested, c_reserved_word);
			if (c == '{' /* } */ && cc == C_ENUM) {
				enumerator_list(param);
			} else {
				T->op->pushbacktoken();
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
			if ((param->flags & PARSER_WARNING) && !startmacro && local->level == 0)
				warning("Out of function. %8s [+%d %s]", T->token, T->lineno, T->gpath->path);
			break;
		case C_TYPEDEF:
			{
				/*
				 * This parser is too complex to maintain.
				 * We should rewrite the whole.
				 */
				char savetok[MAXTOKEN];
				int savelineno = 0;
				int typedef_savelevel = local->level;

				savetok[0] = 0;

				/* skip type qualifiers */
				do {
					c = T->op->nexttoken("{}(),;", c_reserved_word);
				} while (IS_TYPE_QUALIFIER(c) || c == '\n');

				if ((param->flags & PARSER_WARNING) && c == EOF) {
					warning("unexpected eof. [+%d %s]", T->lineno, T->gpath->path);
					break;
				} else if (c == C_ENUM || c == C_STRUCT || c == C_UNION) {
					char *interest_enum = "{},;";
					int c_ = c;

					while ((c = T->op->nexttoken(interest_enum, c_reserved_word)) == C___ATTRIBUTE__)
						process_attribute(param);
					while (c == '\n')
						c = T->op->nexttoken(interest_enum, c_reserved_word);
					/* read tag name if exist */
					if (c == SYMBOL) {
						if (T->op->peekchar(0) == '{') /* } */ {
							PUT(PARSER_DEF, T->token, T->lineno, T->sp);
						} else {
							PUT(PARSER_REF_SYM, T->token, T->lineno, T->sp);
						}
						c = T->op->nexttoken(interest_enum, c_reserved_word);
					}
					while (c == '\n')
						c = T->op->nexttoken(interest_enum, c_reserved_word);
					if (c_ == C_ENUM) {
						if (c == '{') /* } */
							c = enumerator_list(param);
						else
							T->op->pushbacktoken();
					} else {
						for (; c != EOF; c = T->op->nexttoken(interest_enum, c_reserved_word)) {
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
							if (c == ';' && local->level == typedef_savelevel) {
								if (savetok[0]) {
									PUT(PARSER_DEF, savetok, savelineno, T->sp);
									savetok[0] = 0;
								}
								break;
							} else if (c == '{')
								local->level++;
							else if (c == '}') {
								savetok[0] = 0;
								if (--local->level == typedef_savelevel)
									break;
							} else if (c == SYMBOL) {
								if (local->level > typedef_savelevel)
									PUT(PARSER_REF_SYM, T->token, T->lineno, T->sp);
								/* save lastest token */
								strlimcpy(savetok, T->token, sizeof(savetok));
								savelineno = T->lineno;
							}
						}
						if (c == ';')
							break;
					}
					if ((param->flags & PARSER_WARNING) && c == EOF) {
						warning("unexpected eof. [+%d %s]", T->lineno, T->gpath->path);
						break;
					}
				} else if (c == SYMBOL) {
					PUT(PARSER_REF_SYM, T->token, T->lineno, T->sp);
				}
				savetok[0] = 0;
				while ((c = T->op->nexttoken("(),;", c_reserved_word)) != EOF) {
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
						local->level++;
					else if (c == ')')
						local->level--;
					else if (c == SYMBOL) {
						if (local->level > typedef_savelevel) {
							PUT(PARSER_REF_SYM, T->token, T->lineno, T->sp);
						} else {
							/* put latest token if any */
							if (savetok[0]) {
								PUT(PARSER_REF_SYM, savetok, savelineno, T->sp);
							}
							/* save lastest token */
							strlimcpy(savetok, T->token, sizeof(savetok));
							savelineno = T->lineno;
						}
					} else if (c == ',' || c == ';') {
						if (savetok[0]) {
							PUT(PARSER_DEF, savetok, T->lineno, T->sp);
							savetok[0] = 0;
						}
					}
					if (local->level == typedef_savelevel && c == ';')
						break;
				}
				if (param->flags & PARSER_WARNING) {
					if (c == EOF)
						warning("unexpected eof. [+%d %s]", T->lineno, T->gpath->path);
					else if (local->level != typedef_savelevel)
						warning("unmatched () block. (last at level %d.)[+%d %s]", local->level, T->lineno, T->gpath->path);
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
	if (param->flags & PARSER_WARNING) {
		if (local->level != 0)
			warning("unmatched {} block. (last at level %d.)[+%d %s]", local->level, T->lineno, T->gpath->path);
		if (local->piflevel != 0)
			warning("unmatched #if block. (last at level %d.)[+%d %s]", local->piflevel, T->lineno, T->gpath->path);
	}
	strbuf_pool_release(sb);
	/* close current tokenizer */
	tokenizer_close(T);
	T = NULL;
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
	while ((c = T->op->nexttoken("()", c_reserved_word)) != EOF) {
		if (c == '(')
			brace++;
		else if (c == ')')
			brace--;
		else if (c == SYMBOL) {
			PUT(PARSER_REF_SYM, T->token, T->lineno, T->sp);
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
	while ((c = T->op->nexttoken("()", c_reserved_word)) != EOF) {
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
				strlimcpy(arg1, T->token, MAXTOKEN);
			}
			PUT(PARSER_REF_SYM, T->token, T->lineno, T->sp);
		}
	}
	if (c == EOF)
		return 0;
	brace_level = 0;
	while ((c = T->op->nexttoken(",;[](){}=", c_reserved_word)) != EOF) {
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
			T->op->pushbacktoken();
			return 0;
		default:
			break;
		}
		if (c == '('/* ) */ || c == '[')
			brace_level++;
		else if (c == /* ( */')' || c == ']')
			brace_level--;
		else if (brace_level == 0
		    && ((c == SYMBOL && strcmp(T->token, "__THROW")) || IS_RESERVED_WORD(c)))
			isdefine = 1;
		else if (c == ';' || c == ',') {
			if (!isdefine)
				break;
		} else if (c == '{' /* } */) {
			T->op->pushbacktoken();
			return 1;
		} else if (c == /* { */'}')
			break;
		else if (c == '=')
			break;

		/* pick up symbol */
		if (c == SYMBOL)
			PUT(PARSER_REF_SYM, T->token, T->lineno, T->sp);
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
	struct _lang_local *local = T->lang_priv;

	local->curstack = &local->ifstack[local->piflevel];
	if (cc == SHARP_IFDEF || cc == SHARP_IFNDEF || cc == SHARP_IF) {
		DBG_PRINT(local->piflevel, "#if");
		if (++local->piflevel >= MAXPIFSTACK)
			die("#if stack over flow. [%s]", T->gpath->path);
		++local->curstack;
		local->curstack->start = local->level;
		local->curstack->end = -1;
		local->curstack->if0only = 0;
		if (T->op->peekchar(0) == '0')
			local->curstack->if0only = 1;
		else if ((cc = T->op->nexttoken(NULL, c_reserved_word)) == SYMBOL && !strcmp(T->token, "notdef"))
			local->curstack->if0only = 1;
		else
			T->op->pushbacktoken();
	} else if (cc == SHARP_ELIF || cc == SHARP_ELSE) {
		DBG_PRINT(local->piflevel - 1, "#else");
		if (local->curstack->end == -1)
			local->curstack->end = local->level;
		else if (local->curstack->end != local->level && (param->flags & PARSER_WARNING))
			warning("uneven level. [+%d %s]", T->lineno, T->gpath->path);
		local->level = local->curstack->start;
		local->curstack->if0only = 0;
	} else if (cc == SHARP_ENDIF) {
		int minus = 0;
		if (--local->piflevel < 0) {
			minus = 1;
			local->piflevel = 0;
		}
		DBG_PRINT(local->piflevel, "#endif");
		if (minus) {
			if (param->flags & PARSER_WARNING)
				warning("unmatched #if block. reseted. [+%d %s]", T->lineno, T->gpath->path);
		} else {
			if (local->curstack->if0only)
				local->level = local->curstack->start;
			else if (local->curstack->end != -1) {
				if (local->curstack->end != local->level && (param->flags & PARSER_WARNING))
					warning("uneven level. [+%d %s]", T->lineno, T->gpath->path);
				local->level = local->curstack->end;
			}
		}
	}
	while ((cc = T->op->nexttoken(NULL, c_reserved_word)) != EOF && cc != '\n') {
		if (cc == SYMBOL && strcmp(T->token, "defined") != 0)
			PUT(PARSER_REF_SYM, T->token, T->lineno, T->sp);
	}
}

/**
 * enumerator_list: process "symbol (= expression), ... "}
 */
static int
enumerator_list(const struct parser_param *param)
{
	struct _lang_local *local = T->lang_priv;
	int savelevel = local->level;
	int in_expression = 0;
	int c = '{';

	for (; c != EOF; c = T->op->nexttoken("{}(),=", c_reserved_word)) {
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
				PUT(PARSER_REF_SYM, T->token, T->lineno, T->sp);
			else
				PUT(PARSER_DEF, T->token, T->lineno, T->sp);
			break;
		case '{':
		case '(':
			local->level++;
			break;
		case '}':
		case ')':
			if (--local->level == savelevel)
				return c;
			break;
		case ',':
			if (local->level == savelevel + 1)
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
