/*
 * Copyright (c) 1998, 1999, 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009,
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
#include "cpp_res.h"
#include "strbuf.h"
#include "strlimcpy.h"
#include "tokenizer.h"
#include "path.h"
#include "gtags_helper.h"


static void process_attribute(const struct parser_param *);
static int function_definition(const struct parser_param *);
static void condition_macro(const struct parser_param *, int);
static int enumerator_list(const struct parser_param *);

		/** max size of complete name of class */
#define MAXCOMPLETENAME 1024
		/** max size of class stack */
#define MAXCLASSSTACK   100
#define IS_CV_QUALIFIER(c)      ((c) == CPP_CONST || (c) == CPP_VOLATILE)

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
	int namespacelevel;	/**< namespace block level */
};


static TOKENIZER *T = NULL; /* current tokenizer */

/**
 * Cpp: read C++ file and pickup tag entries.
 */
void
Cpp(const struct parser_param *param)
{
	int c, cc;
	int savelevel;
	int startclass, startthrow, startmacro, startsharp, startequal;
	char classname[MAXTOKEN];
	char completename[MAXCOMPLETENAME];
	char *completename_limit = &completename[sizeof(completename)];
	int classlevel;
	struct {
		char *classname;
		char *terminate;
		int level;
	} stack[MAXCLASSSTACK];
	const char *interested = "{}=;~";
	struct _lang_local _local = {0};
	struct _lang_local *local = &_local;
	STRBUF *sb = strbuf_pool_assign(0);

	*classname = *completename = 0;
	stack[0].classname = completename;
	stack[0].terminate = completename;
	stack[0].level = 0;
	local->level = classlevel = local->piflevel = local->namespacelevel = 0;
	savelevel = -1;
	startclass = startthrow = startmacro = startsharp = startequal = 0;

	if ((T = tokenizer_open(param->gpath, NULL, local)) == NULL)
		die("'%s' cannot open.", param->gpath->path);
	T->crflag = 1;			/* require '\n' as a token */
	T->mode |= C_MODE;
	T->mode |= CPP_MODE;

	while ((cc = T->op->nexttoken(interested, cpp_reserved_word)) != EOF) {
		if (cc == '~' && local->level == stack[classlevel].level)
			continue;
		switch (cc) {
		case SYMBOL:		/* symbol	*/
			if (startclass || startthrow) {
				PUT(PARSER_REF_SYM, T->token, T->lineno, T->sp);
			} else if (T->op->peekchar(0) == '('/* ) */) {
				if (param->isnotfunction(T->token)) {
					PUT(PARSER_REF_SYM, T->token, T->lineno, T->sp);
				} else if (local->level > stack[classlevel].level || startequal || startmacro) {
					PUT(PARSER_REF_SYM, T->token, T->lineno, T->sp);
				} else if (local->level == stack[classlevel].level && !startmacro && !startsharp && !startequal) {
					char savetok[MAXTOKEN], *saveline;
					int savelineno = T->lineno;

					strlimcpy(savetok, T->token, sizeof(savetok));
					strbuf_reset(sb);
					strbuf_puts(sb, T->sp);
					saveline = strbuf_value(sb);
					if (function_definition(param)) {
						/* ignore constructor */
						if (strcmp(stack[classlevel].classname, savetok))
							PUT(PARSER_DEF, savetok, savelineno, saveline);
					} else {
						PUT(PARSER_REF_SYM, savetok, savelineno, saveline);
					}
				}
			} else {
				PUT(PARSER_REF_SYM, T->token, T->lineno, T->sp);
			}
			break;
		case CPP_USING:
			T->crflag = 0;
			/*
			 * using namespace name;
			 * using ...;
			 */
			if ((c = T->op->nexttoken(interested, cpp_reserved_word)) == CPP_NAMESPACE) {
				if ((c = T->op->nexttoken(interested, cpp_reserved_word)) == SYMBOL) {
					PUT(PARSER_REF_SYM, T->token, T->lineno, T->sp);
				} else {
					if (param->flags & PARSER_WARNING)
						warning("missing namespace name. [+%d %s].", T->lineno, T->gpath->path);
					T->op->pushbacktoken();
				}
			} else if (c  == SYMBOL) {
				char savetok[MAXTOKEN], *saveline;
				int savelineno = T->lineno;

				strlimcpy(savetok, T->token, sizeof(savetok));
				strbuf_reset(sb);
				strbuf_puts(sb, T->sp);
				saveline = strbuf_value(sb);
				if ((c = T->op->nexttoken(interested, cpp_reserved_word)) == '=') {
					PUT(PARSER_DEF, savetok, savelineno, saveline);
				} else {
					PUT(PARSER_REF_SYM, savetok, savelineno, saveline);
					while (c == SYMBOL) {
						PUT(PARSER_REF_SYM, T->token, T->lineno, T->sp);
						c = T->op->nexttoken(interested, cpp_reserved_word);
					}
				}
			} else {
				T->op->pushbacktoken();
			}
			T->crflag = 1;
			break;
		case CPP_NAMESPACE:
			T->crflag = 0;
			/*
			 * namespace name = ...;
			 * namespace [name] { ... }
			 * namespace name[::name]* { ... }
			 */
		cpp_namespace_loop:
			if ((c = T->op->nexttoken(interested, cpp_reserved_word)) == SYMBOL) {
			cpp_namespace_token_loop:
				PUT(PARSER_DEF, T->token, T->lineno, T->sp);
				if ((c = T->op->nexttoken(interested, cpp_reserved_word)) == '=') {
					T->crflag = 1;
					break;
				}
				if (c == CPP_WCOLON)
					goto cpp_namespace_loop;
				if (c == SYMBOL)
					goto cpp_namespace_token_loop;
			}
			/*
			 * Namespace block doesn't have any influence on level.
			 */
			if (c == '{') /* } */ {
				local->namespacelevel++;
			} else {
				if (param->flags & PARSER_WARNING)
					warning("missing namespace block. [+%d %s](0x%x).", T->lineno, T->gpath->path, c);
			}
			T->crflag = 1;
			break;
		case CPP_EXTERN: /* for 'extern "C"/"C++"' */
			if (T->op->peekchar(0) != '"') /* " */
				continue; /* If does not start with '"', continue. */
			while ((c = T->op->nexttoken(interested, cpp_reserved_word)) == '\n')
				;
			/*
			 * 'extern "C"/"C++"' block is a kind of namespace block.
			 * (It doesn't have any influence on level.)
			 */
			if (c == '{') /* } */
				local->namespacelevel++;
			else
				T->op->pushbacktoken();
			break;
		case CPP_STRUCT:
		case CPP_CLASS:
			DBG_PRINT(local->level, cc == CPP_CLASS ? "class" : "struct");
			while ((c = T->op->nexttoken(NULL, cpp_reserved_word)) == CPP___ATTRIBUTE__ || c == '\n')
				if (c == CPP___ATTRIBUTE__)
					process_attribute(param);
			if (c == SYMBOL) {
				char *saveline;
				int savelineno;
				do {
					if (c == SYMBOL) {
						savelineno = T->lineno;
						strbuf_reset(sb);
						strbuf_puts(sb, T->sp);
						saveline = strbuf_value(sb);
						strlimcpy(classname, T->token, sizeof(classname));
					}
					c = T->op->nexttoken(NULL, cpp_reserved_word);
					if (c == SYMBOL)
						PUT(PARSER_REF_SYM, classname, savelineno, saveline);
					else if (c == '<') {
						int templates = 1;
						for (;;) {
							c = T->op->nexttoken(NULL, cpp_reserved_word);
							if (c == SYMBOL)
								PUT(PARSER_REF_SYM, T->token, T->lineno, T->sp);
							if (c == '<') {
								if (T->op->peekchar(1) == '<')
									T->op->skipnext(1);
								else
									++templates;
							} else if (c == '>') {
								if (--templates == 0)
									break;
							} else if (c == EOF) {
								if (param->flags & PARSER_WARNING) 
									warning("failed to parse template [+%d %s].", savelineno, T->gpath->path);
								goto finish;
							}
						}
						c = T->op->nexttoken(NULL, cpp_reserved_word);
					} else if (c == CPP_FINAL) {
						c = T->op->nexttoken(NULL, cpp_reserved_word);
					}
				} while (c == SYMBOL || c == '\n');
				if (c == ':' || c == '{') /* } */ {
					startclass = 1;
					PUT(PARSER_DEF, classname, savelineno, saveline);
				} else
					PUT(PARSER_REF_SYM, classname, savelineno, saveline);
			}
			T->op->pushbacktoken();
			break;
		case '{':  /* } */
			DBG_PRINT(local->level, "{"); /* } */
			++local->level;
			if ((param->flags & PARSER_BEGIN_BLOCK) && atfirst) {
				if ((param->flags & PARSER_WARNING) && local->level != 1)
					warning("forced level 1 block start by '{' at column 0 [+%d %s].", T->lineno, T->gpath->path); /* } */
				local->level = 1;
			}
			if (startclass) {
				char *p = stack[classlevel].terminate;
				char *q = classname;

				if (++classlevel >= MAXCLASSSTACK)
					die("class stack over flow.[%s]", T->gpath->path);
				if (classlevel > 1 && p < completename_limit)
					*p++ = '.';
				stack[classlevel].classname = p;
				while (*q && p < completename_limit)
					*p++ = *q++;
				stack[classlevel].terminate = p;
				stack[classlevel].level = local->level;
				*p++ = 0;
			}
			startclass = startthrow = 0;
			break;
			/* { */
		case '}':
			if (--local->level < 0) {
				if (local->namespacelevel > 0)
					local->namespacelevel--;
				else if (param->flags & PARSER_WARNING)
					warning("missing left '{' [+%d %s].", T->lineno, T->gpath->path); /* } */
				local->level = 0;
			}
			if ((param->flags & PARSER_END_BLOCK) && atfirst) {
				if ((param->flags & PARSER_WARNING) && local->level != 0)
					/* { */
					warning("forced level 0 block end by '}' at column 0 [+%d %s].", T->lineno, T->gpath->path);
				local->level = 0;
			}
			if (local->level < stack[classlevel].level)
				*(stack[--classlevel].terminate) = 0;
			/* { */
			DBG_PRINT(local->level, "}");
			break;
		case '=':
			/* dirty hack. Don't mimic this. */
			if (T->op->peekchar(0) == '=') {
				T->op->skipnext(1);
			} else {
				startequal = 1;
			}
			break;
		case ';':
			startthrow = startequal = 0;
			break;
		case '\n':
			if (startmacro && local->level != savelevel) {
				if (param->flags & PARSER_WARNING)
					warning("different level before and after #define macro. reseted. [+%d %s].", T->lineno, T->gpath->path);
				local->level = savelevel;
			}
			startmacro = startsharp = 0;
			break;
		/*
		 * #xxx
		 */
		case SHARP_DEFINE:
		case SHARP_UNDEF:
			startmacro = 1;
			savelevel = local->level;
			if ((c = T->op->nexttoken(interested, cpp_reserved_word)) != SYMBOL) {
				T->op->pushbacktoken();
				break;
			}
			if (T->op->peekchar(1) == '('/* ) */) {
				PUT(PARSER_DEF, T->token, T->lineno, T->sp);
				while ((c = T->op->nexttoken("()", cpp_reserved_word)) != EOF && c != '\n' && c != /* ( */ ')')
					if (c == SYMBOL)
						PUT(PARSER_REF_SYM, T->token, T->lineno, T->sp);
				if (c == '\n')
					T->op->pushbacktoken();
			}  else {
				PUT(PARSER_DEF, T->token, T->lineno, T->sp);
			}
			break;
		case SHARP_IMPORT:
		case SHARP_INCLUDE:
		case SHARP_INCLUDE_NEXT:
		case SHARP_ERROR:
		case SHARP_LINE:
		case SHARP_PRAGMA:
		case SHARP_WARNING:
		case SHARP_IDENT:
		case SHARP_SCCS:
			while ((c = T->op->nexttoken(interested, cpp_reserved_word)) != EOF && c != '\n')
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
			(void)T->op->nexttoken(interested, cpp_reserved_word);
			break;
		case CPP_NEW:
			if ((c = T->op->nexttoken(interested, cpp_reserved_word)) == SYMBOL)
				PUT(PARSER_REF_SYM, T->token, T->lineno, T->sp);
			break;
		case CPP_ENUM:
		case CPP_UNION:
			while ((c = T->op->nexttoken(interested, cpp_reserved_word)) == CPP___ATTRIBUTE__)
				process_attribute(param);
			while (c == '\n')
				c = T->op->nexttoken(interested, cpp_reserved_word);
			if (c == SYMBOL) {
				if (T->op->peekchar(0) == '{') /* } */ {
					PUT(PARSER_DEF, T->token, T->lineno, T->sp);
				} else {
					PUT(PARSER_REF_SYM, T->token, T->lineno, T->sp);
				}
				c = T->op->nexttoken(interested, cpp_reserved_word);
			}
			while (c == '\n')
				c = T->op->nexttoken(interested, cpp_reserved_word);
			if (c == '{' /* } */ && cc == CPP_ENUM) {
				enumerator_list(param);
			} else {
				T->op->pushbacktoken();
			}
			break;
		case CPP_TEMPLATE:
			{
				int level = 0;

				while ((c = T->op->nexttoken("<>", cpp_reserved_word)) != EOF) {
					if (c == '<')
						++level;
					else if (c == '>') {
						if (--level == 0)
							break;
					} else if (c == SYMBOL) {
						PUT(PARSER_REF_SYM, T->token, T->lineno, T->sp);
					}
				}
				if (c == EOF && (param->flags & PARSER_WARNING))
					warning("template <...> isn't closed. [+%d %s].", T->lineno, T->gpath->path);
			}
			break;
		case CPP_OPERATOR:
			while ((c = T->op->nexttoken(";{", /* } */ cpp_reserved_word)) != EOF) {
				if (c == '{') /* } */ {
					T->op->pushbacktoken();
					break;
				} else if (c == ';') {
					break;
				} else if (c == SYMBOL) {
					PUT(PARSER_REF_SYM, T->token, T->lineno, T->sp);
				}
			}
			if (c == EOF && (param->flags & PARSER_WARNING))
				warning("'{' doesn't exist after 'operator'. [+%d %s].", T->lineno, T->gpath->path); /* } */
			break;
		/* control statement check */
		case CPP_THROW:
			startthrow = 1;
		case CPP_BREAK:
		case CPP_CASE:
		case CPP_CATCH:
		case CPP_CONTINUE:
		case CPP_DEFAULT:
		case CPP_DELETE:
		case CPP_DO:
		case CPP_ELSE:
		case CPP_FOR:
		case CPP_GOTO:
		case CPP_IF:
		case CPP_RETURN:
		case CPP_SWITCH:
		case CPP_TRY:
		case CPP_WHILE:
			if ((param->flags & PARSER_WARNING) && !startmacro && local->level == 0)
				warning("Out of function. %8s [+%d %s]", T->token, T->lineno, T->gpath->path);
			break;
		case CPP_TYPEDEF:
			{
				/*
				 * This parser is too complex to maintain.
				 * We should rewrite the whole.
				 */
				char savetok[MAXTOKEN];
				int savelineno = 0;
				int typedef_savelevel = local->level;
				int templates = 0;

				savetok[0] = 0;

				/* skip CV qualifiers */
				do {
					c = T->op->nexttoken("{}(),;", cpp_reserved_word);
				} while (IS_CV_QUALIFIER(c) || c == '\n');

				if ((param->flags & PARSER_WARNING) && c == EOF) {
					warning("unexpected eof. [+%d %s]", T->lineno, T->gpath->path);
					break;
				} else if (c == CPP_ENUM || c == CPP_STRUCT || c == CPP_UNION) {
					char *interest_enum = "{},;";
					int c_ = c;

					while ((c = T->op->nexttoken(interest_enum, cpp_reserved_word)) == CPP___ATTRIBUTE__)
						process_attribute(param);
					while (c == '\n')
						c = T->op->nexttoken(interest_enum, cpp_reserved_word);
					/* read tag name if exist */
					if (c == SYMBOL) {
						if (T->op->peekchar(0) == '{') /* } */ {
							PUT(PARSER_DEF, T->token, T->lineno, T->sp);
						} else {
							PUT(PARSER_REF_SYM, T->token, T->lineno, T->sp);
						}
						c = T->op->nexttoken(interest_enum, cpp_reserved_word);
					}
					while (c == '\n')
						c = T->op->nexttoken(interest_enum, cpp_reserved_word);
					if (c_ == CPP_ENUM) {
						if (c == '{') /* } */
							c = enumerator_list(param);
						else
							T->op->pushbacktoken();
					} else {
						for (; c != EOF; c = T->op->nexttoken(interest_enum, cpp_reserved_word)) {
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
				while ((c = T->op->nexttoken("()<>,;", cpp_reserved_word)) != EOF) {
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
					else if (c == '<')
						templates++;
					else if (c == '>')
						templates--;
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
							PUT(templates ? PARSER_REF_SYM : PARSER_DEF, savetok, T->lineno, T->sp);
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
		case CPP___ATTRIBUTE__:
			process_attribute(param);
			break;
		default:
			break;
		}
	}
finish:
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
	while ((c = T->op->nexttoken("()", cpp_reserved_word)) != EOF) {
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
 *	@return	target type
 */
static int
function_definition(const struct parser_param *param)
{
	int c;
	int brace_level;

	brace_level = 0;
	while ((c = T->op->nexttoken("()", cpp_reserved_word)) != EOF) {
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
		if (c == SYMBOL)
			PUT(PARSER_REF_SYM, T->token, T->lineno, T->sp);
	}
	if (c == EOF)
		return 0;
	if (T->op->peekchar(0) == ';') {
		(void)T->op->nexttoken(";", NULL);
		return 0;
	}
	brace_level = 0;
	while ((c = T->op->nexttoken(",;[](){}=", cpp_reserved_word)) != EOF) {
		switch (c) {
		case SHARP_IFDEF:
		case SHARP_IFNDEF:
		case SHARP_IF:
		case SHARP_ELIF:
		case SHARP_ELSE:
		case SHARP_ENDIF:
			condition_macro(param, c);
			continue;
		case CPP___ATTRIBUTE__:
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
		else if (brace_level == 0 && (c == ';' || c == ','))
			break;
		else if (c == '{' /* } */) {
			T->op->pushbacktoken();
			return 1;
		} else if (c == /* { */'}') {
			T->op->pushbacktoken();
			break;
		} else if (c == '=')
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
 *	@param[in]	param
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
			die("#if pifstack over flow. [%s]", T->gpath->path);
		++local->curstack;
		local->curstack->start = local->level;
		local->curstack->end = -1;
		local->curstack->if0only = 0;
		if (T->op->peekchar(0) == '0')
			local->curstack->if0only = 1;
		else if ((cc = T->op->nexttoken(NULL, cpp_reserved_word)) == SYMBOL && !strcmp(T->token, "notdef"))
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

		--local->piflevel;
		if (local->piflevel < 0) {
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
	while ((cc = T->op->nexttoken(NULL, cpp_reserved_word)) != EOF && cc != '\n') {
                if (cc == SYMBOL && strcmp(T->token, "defined") != 0) {
			PUT(PARSER_REF_SYM, T->token, T->lineno, T->sp);
		}
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

	for (; c != EOF; c = T->op->nexttoken("{}(),=", cpp_reserved_word)) {
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
