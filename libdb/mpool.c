/*-
 * Copyright (c) 1990, 1993, 1994
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#if defined(LIBC_SCCS) && !defined(lint)
static char sccsid[] = "@(#)mpool.c	8.5 (Berkeley) 7/26/94";
#endif /* LIBC_SCCS and not lint */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <sys/stat.h>

#include <errno.h>
#include <stdio.h>
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

#if (defined(_WIN32) && !defined(__CYGWIN__))
#define fsync _commit
#endif

#ifdef DB_CONCURRENT
#define _MPOOL_RWLOCK_RDLOCK(mp)	(void)pthread_rwlock_rdlock(mp->rwlock)
#define _MPOOL_RWLOCK_WRLOCK(mp)	(void)pthread_rwlock_wrlock(mp->rwlock)
#define _MPOOL_RWLOCK_UNLOCK(mp)	(void)pthread_rwlock_unlock(mp->rwlock)
#else
#define _MPOOL_RWLOCK_RDLOCK(mp)
#define _MPOOL_RWLOCK_WRLOCK(mp)
#define _MPOOL_RWLOCK_UNLOCK(mp)
#endif

#define	__MPOOLINTERFACE_PRIVATE
#include "mpool.h"
#include "die.h"

static BKT *mpool_bkt(MPOOL *);
static BKT *mpool_look(MPOOL *, pgno_t);
static int  mpool_write(MPOOL *, BKT *);

/**
 * mpool_open --
 *	Initialize a memory pool.
 *
 *	@param key
 *	@param fd
 *	@param pagesize
 *	@param maxcache
 */
MPOOL *
mpool_open(key, fd, pagesize, maxcache)
	void *key;
	int fd;
	pgno_t pagesize, maxcache;
{
	struct stat sb;
	MPOOL *mp;
	int entry;

	/*
	 * Get information about the file.
	 *
	 * XXX
	 * We don't currently handle pipes, although we should.
	 */
	if (fstat(fd, &sb))
		return (NULL);
	if (!S_ISREG(sb.st_mode)) {
		errno = ESPIPE;
		return (NULL);
	}

	/* Allocate and initialize the MPOOL cookie. */
	if ((mp = (MPOOL *)calloc(1, sizeof(MPOOL))) == NULL)
		return (NULL);
	CIRCLEQ_INIT(&mp->lqh);
	for (entry = 0; entry < HASHSIZE; ++entry)
		CIRCLEQ_INIT(&mp->hqh[entry]);
	mp->maxcache = maxcache;
	mp->npages = sb.st_size / pagesize;
	mp->pagesize = pagesize;
	mp->fd = fd;
#ifdef DB_CONCURRENT
	/* init concurrent sync locks */
	int r;
	if ((r = pthread_rwlock_init(&mp->rwlock, NULL)) != 0)
		die("mpool init rwlock failed, retcode: %d.", r);
#endif
	return (mp);
}

/**
 * mpool_filter --
 *	Initialize input/output filters.
 *
 *	@param mp
 *	@param pgin
 *	@param pgout
 *	@param pgcookie
 */
void
mpool_filter(mp, pgin, pgout, pgcookie)
	MPOOL *mp;
	void (*pgin)(void *, pgno_t, void *);
	void (*pgout)(void *, pgno_t, void *);
	void *pgcookie;
{
	_MPOOL_RWLOCK_WRLOCK(mp);
	mp->pgin = pgin;
	mp->pgout = pgout;
	mp->pgcookie = pgcookie;
	_MPOOL_RWLOCK_UNLOCK(mp);
}
	
/**
 * mpool_new --
 *	Get a new page of memory.
 *
 *	@param mp
 *	@param pgnoaddr
 */
void *
mpool_new(mp, pgnoaddr)
	MPOOL *mp;
	pgno_t *pgnoaddr;
{
	struct _hqh *head;
	BKT *bp;
	void *r = NULL;

	_MPOOL_RWLOCK_WRLOCK(mp);
	if (mp->npages == MAX_PAGE_NUMBER) {
		(void)error("mpool_new: page allocation overflow.\n");
		abort();
	}
#ifdef STATISTICS
	++mp->pagenew;
#endif
	/*
	 * Get a BKT from the cache.  Assign a new page number, attach
	 * it to the head of the hash chain, the tail of the lru chain,
	 * and return.
	 */
	if ((bp = mpool_bkt(mp)) == NULL)
		goto defer;
	*pgnoaddr = bp->pgno = mp->npages++;
	bp->flags = MPOOL_PINNED;

	head = &mp->hqh[HASHKEY(bp->pgno)];
	CIRCLEQ_INSERT_HEAD(head, bp, hq);
	CIRCLEQ_INSERT_TAIL(&mp->lqh, bp, q);
	r = bp->page;
defer:
	_MPOOL_RWLOCK_UNLOCK(mp);
	return r;
}

/**
 * mpool_get
 *	Get a page.
 *
 *	@param mp
 *	@param pgno
 *	@param flags
 */
void *
mpool_get(mp, pgno, flags)
	MPOOL *mp;
	pgno_t pgno;
	u_int flags;				/* XXX not used? */
{
	struct _hqh *head;
	BKT *bp;
	off_t off;
	int nr;
	void *r = NULL;

	/* Check for attempt to retrieve a non-existent page. */
	_MPOOL_RWLOCK_RDLOCK(mp);
	if (pgno >= mp->npages) {
		errno = EINVAL;
		goto defer;
	}

#ifdef STATISTICS
	_MPOOL_RWLOCK_UNLOCK(mp);
	_MPOOL_RWLOCK_WRLOCK(mp);
	++mp->pageget;
	_MPOOL_RWLOCK_UNLOCK(mp);
	_MPOOL_RWLOCK_RDLOCK(mp);
#endif

	/* Check for a page that is cached. */
	if ((bp = mpool_look(mp, pgno)) != NULL) {
#ifdef DEBUG
		if (bp->flags & MPOOL_PINNED) {
			(void)error("mpool_get: page %d already pinned\n", bp->pgno);
			abort();
		}
#endif
		/*
		 * Move the page to the head of the hash chain and the tail
		 * of the lru chain.
		 */
		_MPOOL_RWLOCK_UNLOCK(mp);
		_MPOOL_RWLOCK_WRLOCK(mp);
		head = &mp->hqh[HASHKEY(bp->pgno)];
		CIRCLEQ_REMOVE(head, bp, hq);
		CIRCLEQ_INSERT_HEAD(head, bp, hq);
		CIRCLEQ_REMOVE(&mp->lqh, bp, q);
		CIRCLEQ_INSERT_TAIL(&mp->lqh, bp, q);

		/* Return a pinned page. */
		bp->flags |= MPOOL_PINNED;
		r = bp->page;
		goto defer;
	}

	/* Get a page from the cache. */
	_MPOOL_RWLOCK_UNLOCK(mp);
	_MPOOL_RWLOCK_WRLOCK(mp);
	if ((bp = mpool_bkt(mp)) == NULL)
		goto defer;

	/* Read in the contents. */
#ifdef STATISTICS
	++mp->pageread;
#endif

	/*
	 * If both of `off_t' and `long' are 32 bits, the right operand
	 * of the multiplication is converted to `unsigned long',
	 * and the multiplication is done with unsigned 32 bits.
	 * It is equivalent to the case without cast.
	 *
	 * If `off_t' is 64 bits and `long' is 32 bits, the left operand
	 * of the multiplication is converted to `off_t',
	 * and the multiplication is done with signed 64 bits.
	 * Adding cast avoids integer overflow.
	 *
	 * If both of `off_t' and `long' are 64 bits, the right operand
	 * of the multiplication is converted to `unsigned long',
	 * and the multiplication is done with unsigned 64 bits.
	 * It is equivalent to the case without cast.
	 */
	off = mp->pagesize * (off_t)pgno;

#if defined(HAVE_PREAD) && !defined(__CYGWIN__)
	if ((nr = pread(mp->fd, bp->page, mp->pagesize, off)) != mp->pagesize) {
		if (nr >= 0)
			errno = EFTYPE;
		goto defer;
	}
#else
	if (lseek(mp->fd, off, SEEK_SET) != off)
		goto defer;
	if ((nr = read(mp->fd, bp->page, mp->pagesize)) != mp->pagesize) {
		if (nr >= 0)
			errno = EFTYPE;
		goto defer;
	}
#endif

	/* Set the page number, pin the page. */
	bp->pgno = pgno;
	bp->flags = MPOOL_PINNED;

	/*
	 * Add the page to the head of the hash chain and the tail
	 * of the lru chain.
	 */
	head = &mp->hqh[HASHKEY(bp->pgno)];
	CIRCLEQ_INSERT_HEAD(head, bp, hq);
	CIRCLEQ_INSERT_TAIL(&mp->lqh, bp, q);

	/* Run through the user's filter. */
	if (mp->pgin != NULL)
		(mp->pgin)(mp->pgcookie, bp->pgno, bp->page);

	r = bp->page;
defer:
	_MPOOL_RWLOCK_UNLOCK(mp);
	return r;
}

/**
 * mpool_put
 *	Return a page.
 *
 *	@param mp
 *	@param page
 *	@param flags
 */
int
mpool_put(mp, page, flags)
	MPOOL *mp;
	void *page;
	u_int flags;
{
	BKT *bp;

#ifdef STATISTICS
	_MPOOL_RWLOCK_WRLOCK(mp);
	++mp->pageput;
	_MPOOL_RWLOCK_UNLOCK(mp);
#endif
	bp = (BKT *)((char *)page - sizeof(BKT));
#ifdef DEBUG
	if (!(bp->flags & MPOOL_PINNED)) {
		(void)error("mpool_put: page %d not pinned\n", bp->pgno);
		abort();
	}
#endif
	bp->flags &= ~MPOOL_PINNED;
	bp->flags |= flags & MPOOL_DIRTY;
	return (RET_SUCCESS);
}

/**
 * mpool_close
 *	Close the buffer pool.
 *
 *	@param mp
 */
int
mpool_close(mp)
	MPOOL *mp;
{
	BKT *bp;

	_MPOOL_RWLOCK_WRLOCK(mp);
	/* Free up any space allocated to the lru pages. */
	while ((bp = mp->lqh.cqh_first) != (void *)&mp->lqh) {
		CIRCLEQ_REMOVE(&mp->lqh, mp->lqh.cqh_first, q);
		free(bp);
	}

	_MPOOL_RWLOCK_UNLOCK(mp);
#ifdef DB_CONCURRENT
	(void)pthread_rwlock_destroy(&mp->rwlock);
#endif
	/* Free the MPOOL cookie. */
	free(mp);
	return (RET_SUCCESS);
}

/**
 * mpool_sync
 *	Sync the pool to disk.
 *
 *	@param mp
 */
int
mpool_sync(mp)
	MPOOL *mp;
{
	BKT *bp;
	int r = RET_SUCCESS;

	_MPOOL_RWLOCK_WRLOCK(mp);
	/* Walk the lru chain, flushing any dirty pages to disk. */
	for (bp = mp->lqh.cqh_first;
	    bp != (void *)&mp->lqh; bp = bp->q.cqe_next)
		if (bp->flags & MPOOL_DIRTY &&
		    mpool_write(mp, bp) == RET_ERROR)
			return (RET_ERROR);

	/* Sync the file descriptor. */
	if (fsync(mp->fd))
		r = RET_ERROR;
	_MPOOL_RWLOCK_UNLOCK(mp);
	return r;
}

/**
 * mpool_bkt
 *	Get a page from the cache (or create one).
 *
 *	@param mp
 */
static BKT *
mpool_bkt(mp)
	MPOOL *mp;
{
	struct _hqh *head;
	BKT *bp;

	/* If under the max cached, always create a new page. */
	if (mp->curcache < mp->maxcache)
		goto new;

	/*
	 * If the cache is max'd out, walk the lru list for a buffer we
	 * can flush.  If we find one, write it (if necessary) and take it
	 * off any lists.  If we don't find anything we grow the cache anyway.
	 * The cache never shrinks.
	 */
	for (bp = mp->lqh.cqh_first;
	    bp != (void *)&mp->lqh; bp = bp->q.cqe_next)
		if (!(bp->flags & MPOOL_PINNED)) {
			/* Flush if dirty. */
			if (bp->flags & MPOOL_DIRTY &&
			    mpool_write(mp, bp) == RET_ERROR)
				return (NULL);
#ifdef STATISTICS
			++mp->pageflush;
#endif
			/* Remove from the hash and lru queues. */
			head = &mp->hqh[HASHKEY(bp->pgno)];
			CIRCLEQ_REMOVE(head, bp, hq);
			CIRCLEQ_REMOVE(&mp->lqh, bp, q);
#ifdef DEBUG
			{ void *spage;
				spage = bp->page;
				memset(bp, 0xff, sizeof(BKT) + mp->pagesize);
				bp->page = spage;
			}
#endif
			return (bp);
		}

new:	if ((bp = (BKT *)malloc(sizeof(BKT) + mp->pagesize)) == NULL)
		return (NULL);
#ifdef STATISTICS
	++mp->pagealloc;
#endif
#if defined(DEBUG) || defined(PURIFY)
	memset(bp, 0xff, sizeof(BKT) + mp->pagesize);
#endif
	bp->page = (char *)bp + sizeof(BKT);
	++mp->curcache;
	return (bp);
}

/**
 * mpool_write
 *	Write a page to disk.
 *
 *	@param mp
 *	@param bp
 */
static int
mpool_write(mp, bp)
	MPOOL *mp;
	BKT *bp;
{
	off_t off;

#ifdef STATISTICS
	++mp->pagewrite;
#endif

	/* Run through the user's filter. */
	if (mp->pgout)
		(mp->pgout)(mp->pgcookie, bp->pgno, bp->page);

	/* See the comment in mpool_get for cast addition. */
	off = mp->pagesize * (off_t)bp->pgno;

#if defined(HAVE_PWRITE) && !defined(__CYGWIN__)
	if (pwrite(mp->fd, bp->page, mp->pagesize, off) != mp->pagesize)
		return (RET_ERROR);
#else
	if (lseek(mp->fd, off, SEEK_SET) != off)
		return (RET_ERROR);
	if (write(mp->fd, bp->page, mp->pagesize) != mp->pagesize)
		return (RET_ERROR);
#endif

	bp->flags &= ~MPOOL_DIRTY;
	return (RET_SUCCESS);
}

/**
 * mpool_look
 *	Lookup a page in the cache.
 *
 *	@param mp
 *	@param pgno
 */
static BKT *
mpool_look(mp, pgno)
	MPOOL *mp;
	pgno_t pgno;
{
	struct _hqh *head;
	BKT *bp;

	head = &mp->hqh[HASHKEY(pgno)];
	for (bp = head->cqh_first; bp != (void *)head; bp = bp->hq.cqe_next)
		if (bp->pgno == pgno) {
#ifdef STATISTICS
			++mp->cachehit;
#endif
			return (bp);
		}
#ifdef STATISTICS
	++mp->cachemiss;
#endif
	return (NULL);
}

#ifdef STATISTICS
/**
 * mpool_stat
 *	Print out cache statistics.
 *
 *	@param mp
 */
void
mpool_stat(mp)
	MPOOL *mp;
{
	BKT *bp;
	int cnt;
	char *sep;

	(void)message("%lu pages in the file\n", (long unsigned int)mp->npages);
	(void)message("page size %lu, cacheing %lu pages of %lu page max cache\n",
	    mp->pagesize, (long unsigned int)mp->curcache, (long unsigned int)mp->maxcache);
	(void)message("%lu page puts, %lu page gets, %lu page new\n",
	    mp->pageput, mp->pageget, mp->pagenew);
	(void)message("%lu page allocs, %lu page flushes\n",
	    mp->pagealloc, mp->pageflush);
	if (mp->cachehit + mp->cachemiss)
		(void)message("%.0f%% cache hit rate (%lu hits, %lu misses)\n", 
		    ((double)mp->cachehit / (mp->cachehit + mp->cachemiss))
		    * 100, mp->cachehit, mp->cachemiss);
	(void)message("%lu page reads, %lu page writes\n",
	    mp->pageread, mp->pagewrite);

	sep = "";
	cnt = 0;
	for (bp = mp->lqh.cqh_first;
	    bp != (void *)&mp->lqh; bp = bp->q.cqe_next) {
		(void)message("%s%d", sep, bp->pgno);
		if (bp->flags & MPOOL_DIRTY)
			(void)message("d");
		if (bp->flags & MPOOL_PINNED)
			(void)message("P");
		if (++cnt == 10) {
			sep = "\n";
			cnt = 0;
		} else
			sep = ", ";
			
	}
	(void)message("\n");
}
#endif
