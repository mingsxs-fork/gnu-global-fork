/*
 * Copyright (c) 2005, 2006 Tama Communications Corporation
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
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "checkalloc.h"
#include "die.h"
#include "strhash.h"
#include "hash-string.h"
#include "pool.h"

/*

String Hash (associative array): usage and memory status

hash = strhash_open(10);			// allocate hash buckets.

entry = strhash_assign(hash, "name1", 0);	// get entry for the name.

	entry == NULL				// entry not found.

entry = strhash_assign(hash, "name1", 1);	// allocate entry for the name.
						entry
						+-------------+
						|name: "name1"|
						|value:    *---->(void *)NULL
						+-------------+
// strhash_xxx() doesn't affect entry->value. So, you can use it freely.

entry->value = strhash_strdup(hash, "NAME1", 0);
						entry
						+-------------+
						|name: "name1"|
						|value:    *---->"NAME1"
						+-------------+

entry = strhash_assign(hash, "name1", 0);	// get entry of the name.

						entry
						+-------------+
						|name: "name1"|
						|value:    *---->"NAME1"
						+-------------+
char *s = (char *)entry->value;

	s == "NAME1"

strhash_close(hash);				// free resources.
						entry
*/

#define obstack_chunk_alloc check_malloc
#define obstack_chunk_free free

/**
 * strhash_open: open string hash table.
 *
 *	@param[in]	buckets	 size of bucket table
 *	@return	sh	STRHASH structure
 */
STRHASH *
strhash_open(int buckets)
{
	STRHASH *sh = (STRHASH *)check_calloc(sizeof(STRHASH), 1);
	int i;

	sh->htab = (struct sh_head *)check_calloc(sizeof(struct sh_head), buckets);
	for (i = 0; i < buckets; i++)
		SLIST_INIT(&sh->htab[i]);
	sh->buckets = buckets;
	sh->pool = pool_open();
	sh->pool_private = 1;
	sh->entries = 0;
	sh->cur_bucket = -1;
	return sh;
}
/**
 * strhash_init: init sizeof(STRHASH) memory to a new STRHASH object.
 *
 * @param[in]	sh	STRHASH memory allocated by others.
 * @param[in]	buckets	size of bucket table
 * @param[in]	pool	memory pool opened by otheres.
 *
 */
void
strhash_init(STRHASH *sh, int buckets, POOL *pool)
{
	sh->htab = (struct sh_head *)check_calloc(sizeof(struct sh_head), buckets);
	for (int i = 0; i < buckets; i++)
		SLIST_INIT(&sh->htab[i]);
	sh->buckets = buckets;
	sh->entries = 0;
	sh->cur_bucket = -1;
	sh->cur_entry = NULL;
	if (pool) {
		sh->pool = pool;
		sh->pool_private = 0;
	} else {
		sh->pool = pool_open();
		sh->pool_private = 1;
	}
}
/**
 * strhash_assign: assign hash entry.
 *
 *	@param[in]	sh	STRHASH structure
 *	@param[in]	name	name
 *	@param[in]	force	if entry not found, create it.
 *	@return		pointer of the entry
 *
 * If specified entry is found then it is returned, else if the force == 1
 * then new allocated entry is returned.
 * This procedure doesn't operate the contents of entry->value.
 */
struct sh_entry *
strhash_assign(STRHASH *sh, const char *name, int force)
{
	struct sh_head *head = &sh->htab[__hash_string(name) % sh->buckets];
	struct sh_entry *entry;

	/*
	 * Lookup the name's entry.
	 */
	SLIST_FOREACH(entry, head, ptr)
		if (strcmp(entry->name, name) == 0)
			break;
	/*
	 * If not found, allocate an entry.
	 */
	if (entry == NULL && force) {
		entry = pool_malloc(sh->pool, sizeof(struct sh_entry));
		entry->name = pool_strdup(sh->pool, name, 0);
		entry->value = NULL;
		SLIST_INSERT_HEAD(head, entry, ptr);
		sh->entries++;
	}
	return entry;
}
/**
 * strhash_strdup: allocate memory and copy string.
 *
 *	@param[in]	sh	STRHASH structure
 *	@param[in]	string	string
 *	@param[in]	size	size of string
 *	@return		allocated string
 *
 */
char *
strhash_strdup(STRHASH *sh, const char *string, int size)
{
	return pool_strdup(sh->pool, string, size);
}
/**
 * strhash_first: get first entry
 *
 *	@param[in]	sh	STRHASH structure
 */
struct sh_entry *
strhash_first(STRHASH *sh)
{
	sh->cur_bucket = -1;		/* to start from index 0. */
	sh->cur_entry = NULL;
	return strhash_next(sh);
}
/**
 * strhash_next: get next entry
 *
 *	@param[in]	sh	STRHASH structure
 */
struct sh_entry *
strhash_next(STRHASH *sh)
{
	struct sh_entry *entry = NULL;

	if (sh->buckets > 0 && sh->cur_bucket < sh->buckets) {
		entry = sh->cur_entry;
		if (entry == NULL) {
			while (++sh->cur_bucket < sh->buckets) {
				entry = SLIST_FIRST(&sh->htab[sh->cur_bucket]);
				if (entry)
					break;
			}
		}
		sh->cur_entry = (entry) ? SLIST_NEXT(entry, ptr) : NULL;
	}
	return entry;
}
/**
 * strhash_reset: reset string hash.
 *
 *	@param[in]	sh	STRHASH structure
 */
void
strhash_reset(STRHASH *sh)
{
	int i;

	/*
	 * Free and reinitialize entries for each bucket.
	 */
	for (i = 0; i < sh->buckets; i++) {
		SLIST_INIT(&sh->htab[i]);
	}
	/*
	 * Free all memory in sh->pool but leave it valid for further allocation.
	 */
	if (sh->pool_private)
		pool_reset(sh->pool);
	sh->entries = 0;
	sh->cur_entry = NULL;
	sh->cur_bucket = -1;
}
/**
 * strhash_dump: dump contents of the string table
 */
void
strhash_dump(STRHASH *sh)
{
	struct sh_entry *p;

	for (p = strhash_first(sh); p != NULL; p = strhash_next(sh))
		message("%s => %s\n", p->name, (char *)p->value);
}
/**
 * strhash_close: close hash array.
 *
 *	@param[in]	sh	STRHASH structure
 */
void
strhash_close(STRHASH *sh)
{
	if (sh->pool_private)
		pool_close(sh->pool);
	free(sh->htab);
	free(sh);
}
/**
 * strhash_close_buckets: close hash htabs.
 *
 *	@param[in]	sh	STRHASH structure
 */
void
strhash_close_buckets(STRHASH *sh)
{
	if (sh->pool_private)
		pool_close(sh->pool);
	free(sh->htab);
	sh->htab = NULL;
	sh->cur_entry = NULL;
	sh->cur_bucket = -1;
	sh->buckets = 0;
}

struct sh_entry *
strhash_pop(STRHASH *sh, const char *name)
{
	struct sh_head *head = &sh->htab[__hash_string(name) % sh->buckets];
	struct sh_entry *entry, *entry_prev = NULL, *entry_next;
	/*
	 * Lookup the name's entry.
	 */
	SLIST_FOREACH(entry, head, ptr) {
		if (strcmp(entry->name, name) == 0)
			break;
		entry_prev = entry;
	}
	if (entry) {
		entry_next = SLIST_NEXT(entry, ptr);
		if (entry_prev)
			SLIST_NEXT(entry_prev, ptr) = entry_next;
		else
			if (entry_next)
				SLIST_INSERT_HEAD(head, entry_next, ptr);
			else
				SLIST_INIT(head);
		SLIST_NEXT(entry, ptr) = NULL;
	}
	return entry;
}
