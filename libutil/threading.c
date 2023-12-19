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
#include <assert.h>
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
#include <ctype.h>
#include <errno.h>
#if defined(_WIN32) && !defined(__CYGWIN__)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef SLIST_ENTRY
#endif

#include "die.h"
#include "likely.h"
#include "checkalloc.h"
#include "threading.h"

#define FIFO_NEXT(fifo, m)	(unlikely(fifo->m == fifo->elemcap) ? 0 : fifo->m+1)
#define FIFO_PREV(fifo, m)	(unlikely(fifo->m == 0) ? fifo->elemcap : fifo->m-1)
#define FIFO_EMPTY(fifo)	(fifo->begidx == fifo->endidx)
#define FIFO_FULL(fifo)		(unlikely(FIFO_NEXT(fifo, endidx) == fifo->begidx))

#define PTHREAD_FORCE_COND_DESTROY(cv) \
	if (pthread_cond_destroy(&cv) == EBUSY) {\
		(void)pthread_cond_broadcast(&cv);\
		(void)pthread_cond_destroy(&cv);\
	}

#define NANOPERSEC			1000000000


static inline int
pop_fifo_begidx (THREAD_FIFO *fifo)
{
	int begidx = fifo->begidx;
	fifo->begidx = FIFO_NEXT(fifo, begidx); /* shift index */
	return begidx;
}

static inline int
push_fifo_endidx (THREAD_FIFO *fifo)
{
	int endidx = fifo->endidx;
	fifo->endidx = FIFO_NEXT(fifo, endidx); /* shift index */
	return endidx;
}

static int
get_fifo_length (THREAD_FIFO *fifo)
{
	int length;
	(void)pthread_mutex_lock(&fifo->mutex);
	if (fifo->endidx >= fifo->begidx)
		length = fifo->endidx - fifo->begidx;
	else
		length = fifo->endidx + (fifo->elemcap+1) - fifo->begidx;
	(void)pthread_mutex_unlock(&fifo->mutex);
	return length;
}

static inline void
_thread_fifo_flush (THREAD_FIFO *fifo, void (*proc)(void *))
{
	THREAD_FIFO_ELEM *elem;
	while (!FIFO_EMPTY(fifo)) {
		elem = &fifo->elemfifo[pop_fifo_begidx(fifo)];
		if (proc)
			proc(elem);
		/* this is unnecessary, proc should free the elem->data if needed */
		(void)pthread_rwlock_wrlock(&elem->rwlock);
		if (elem->data && elem->needfree) {
			/* release rdlock, acquire wrlock instead */
			free((void *)elem->data);
			elem->size = elem->type = elem->needfree = elem->data = 0;
		}
		(void)pthread_rwlock_unlock(&elem->rwlock);
	}
}


THREAD_FIFO *
thread_fifo_create (int elemcap)
{
	int i, r;
	THREAD_FIFO *fifo;
	if (elemcap < 0 || elemcap > THREAD_FIFO_MAX_CAP)
		die("invalid thread fifo capability: %d.", elemcap);
	fifo = check_malloc(sizeof(THREAD_FIFO));
	fifo->elemcap = elemcap;
	fifo->begidx = fifo->endidx = 0;
	fifo->elemfifo = check_calloc(sizeof(THREAD_FIFO_ELEM), elemcap+1);
	/* init thread sync vars */
	if ((r = pthread_mutex_init(&fifo->mutex, NULL)) != 0)
		die("thread fifo init mutex failed, retcode: %d.", r);
	if ((r = pthread_cond_init(&fifo->on_full, NULL)) != 0)
		die("thread fifo init cv on_full failed, retcode: %d.", r);
	if ((r = pthread_cond_init(&fifo->on_empty, NULL)) != 0)
		die("thread fifo init cv on_empty failed, retcode: %d.", r);
	/* init all data rwlocks for fifo */
	for (i = 0; i < elemcap+1; i++) {
		if ((r = pthread_rwlock_init(&fifo->elemfifo[i].rwlock, NULL)) != 0)
			die("thread fifo init %dth elem rwlock failed, retcode: %d", i, r);
	}
	return fifo;
}

void
thread_fifo_flush (THREAD_FIFO *fifo, void (*proc)(void *))
{
	(void)pthread_mutex_lock(&fifo->mutex);
	_thread_fifo_flush(fifo, proc);
	(void)pthread_mutex_unlock(&fifo->mutex);
}

void
thread_fifo_destroy (THREAD_FIFO *fifo)
{
	(void)pthread_mutex_lock(&fifo->mutex);
	/* destroy all thread sync vars */
	PTHREAD_FORCE_COND_DESTROY(fifo->on_full);
	PTHREAD_FORCE_COND_DESTROY(fifo->on_empty);
	/* flush all elems before destroy fifo */
	_thread_fifo_flush(fifo, NULL);
	free(fifo->elemfifo);
	for (int i = 0; i < fifo->elemcap+1; i++)
		(void)pthread_rwlock_destroy(&fifo->elemfifo[i].rwlock);
	(void)pthread_mutex_unlock(&fifo->mutex);
	(void)pthread_mutex_destroy(&fifo->mutex);
	free(fifo);
}

/*
 * thread_fifo_push_elem: push a new elem to the thread fifo.
 *	@param[in]	fifo		descriptor
 *	@param[in]	timeout		timespec timeout on wait for push to finish
 *	@return		pointer to THREAD_FIFO_ELEM that will store the elem
 */
void *
thread_fifo_push_elem (THREAD_FIFO *fifo, struct timespec *timeout)
{
	THREAD_FIFO_ELEM *input;
	struct timeval now;
	struct timespec abstime;
	long nanosec;

	if (!fifo)
		return NULL;

	if (timeout) {
		(void)gettimeofday(&now, NULL);
		nanosec = (now.tv_usec * 1000) + timeout->tv_nsec; /* may overflow */
		abstime.tv_sec = now.tv_sec + timeout->tv_sec + (nanosec >= NANOPERSEC ? 1 : 0);
		abstime.tv_nsec = (nanosec >= NANOPERSEC ? (nanosec-NANOPERSEC) : nanosec);
	}

	(void)pthread_mutex_lock(&fifo->mutex);
	if (FIFO_FULL(fifo)) { /* fifo is full wait release cond signal to push */
		if (timeout) { /* timed cond wait */
			if (pthread_cond_timedwait(&fifo->on_full, &fifo->mutex, &abstime) == ETIMEDOUT) {
				(void)pthread_mutex_unlock(&fifo->mutex);
				return NULL;
			}
		} else /* normal cond wait */
			(void)pthread_cond_wait(&fifo->on_full, &fifo->mutex);
	}
	if (FIFO_EMPTY(fifo))
		(void)pthread_cond_broadcast(&fifo->on_empty);

	input = &fifo->elemfifo[push_fifo_endidx(fifo)];
	(void)pthread_mutex_unlock(&fifo->mutex);
	return (void *)input;
}

/*
 * thread_fifo_pop: pop a pushed elem from the thread fifo.
 *	@param[in]	fifo		descriptor
 *	@param[in]	timeout		timespec timeout on wait for pop to finish
 *	@return		pointer to THREAD_FIFO_ELEM that contains the pushed elem
 */
void *
thread_fifo_pop_elem (THREAD_FIFO *fifo, struct timespec *timeout)
{
	THREAD_FIFO_ELEM *output;
	struct timeval now;
	struct timespec abstime;
	long nanosec;

	if (!fifo)
		return NULL;

	if (timeout) {
		(void)gettimeofday(&now, NULL);
		nanosec = (now.tv_usec * 1000) + timeout->tv_nsec; /* may overflow */
		abstime.tv_sec = now.tv_sec + timeout->tv_sec + (nanosec >= NANOPERSEC ? 1 : 0);
		abstime.tv_nsec = (nanosec >= NANOPERSEC ? (nanosec-NANOPERSEC) : nanosec);
	}

	(void)pthread_mutex_lock(&fifo->mutex);
	if (FIFO_EMPTY(fifo)) {
		if (timeout) {
			if (pthread_cond_timedwait(&fifo->on_empty, &fifo->mutex, &abstime) == ETIMEDOUT) {
				(void)pthread_mutex_unlock(&fifo->mutex);
				return NULL;
			}
		} else
			(void)pthread_cond_wait(&fifo->on_empty, &fifo->mutex);
	}
	if (FIFO_FULL(fifo))
		(void)pthread_cond_broadcast(&fifo->on_full);

	output = &fifo->elemfifo[pop_fifo_begidx(fifo)];
	(void)pthread_mutex_unlock(&fifo->mutex);
	return (void *)output;
}

static inline GJOB_DESC *find_unused_job (JOB_MANAGER *, int *);
static inline int job_manager_idle (JOB_MANAGER *);

/* job signal */
typedef struct job_sig {
	pthread_t tid; /* who sends the signal */
	int index;
	int sig;
} JOB_SIG;

/* job command */
typedef struct job_cmd {
	pthread_t tid; /* who sends the command */
	int index;
	int cmd;
} JOB_CMD;

static inline void
gtags_handle_job_sig (JOB_MANAGER *manager, JOB_SIG *sig)
{
	void *retval;
	GJOB_DESC *job;

	(void)pthread_mutex_lock(&manager->mutex);
	(void)pthread_rwlock_wrlock(&manager->rwlock);

	job = &manager->jobarray[sig->index];
	/* job thread id should be the same */
	assert(pthread_equal(sig->tid, job->tid));

	switch (sig->sig) {
		case GJOB_SIG_ERROR:
			break;
		case GJOB_SIG_DONE: /* job completed */
			(void)pthread_join(sig->tid, &retval);
			(void)pthread_rwlock_wrlock(&job->rwlock);
			job->stopcode = *(int *)retval;
			job->status = GJOB_STATUS_DONE; /* change job status */
			(void)pthread_rwlock_unlock(&job->rwlock);
			/* unblock job launcher thread on creating new jobs if any */
			if (find_unused_job(manager, NULL) == NULL)
				(void)pthread_cond_signal(&manager->array_full);
			manager->jobs--;
			manager->used_map[sig->index >> 6] &= ~(1 << (sig->index & 0x3f));
			/* signal main thread job manager is currently empty */
			if (job_manager_idle(manager) && manager->state == JOB_MANAGER_STATE_WAITING)
				(void)pthread_cond_signal(&manager->array_empty);
			break;
		case GJOB_SIG_DEAD: /* job dead */
			break;
		default:
			break;
	}

	(void)pthread_rwlock_unlock(&manager->rwlock);
	(void)pthread_mutex_unlock(&manager->mutex);
}
static inline void
gtags_handle_job_cmd (JOB_MANAGER *manager, JOB_CMD *cmd)
{
	GJOB_DESC *job;
	(void)pthread_rwlock_rdlock(&manager->rwlock);
	job = &manager->jobarray[cmd->index];
	assert(pthread_equal(cmd->tid, job->tid));
	switch (cmd->cmd) {
		default:
			break;
	}
	(void)pthread_rwlock_unlock(&manager->rwlock);
}

static void *
gtags_job_sighandler (void *arg)
{
	GJOB_DESC *job;
	JOB_SIG *received;
	THREAD_FIFO_ELEM *sigdat;
	struct timespec wait_delay = { .tv_sec = 10, }; /* wait for 10 second each cycle */
	int stopped = 0; /* handler needs to stop */
	JOB_MANAGER *manager = arg;
	assert(manager && manager->sigfifo);
	/* keep poping and handling signals from fifo */
	while (!stopped && (sigdat = thread_fifo_pop_elem(manager->sigfifo, &wait_delay)) != NULL) {
		/* handle received signal */
		(void)pthread_rwlock_rdlock(&sigdat->rwlock);
		received = (JOB_SIG *)sigdat->data;
		assert(received != 0);
		if (received->sig == HANDLER_JOB_EXIT && received->tid == manager->launcher_tid)
			stopped = 1; /* received EXIT request from the launcher thread */
		else
			gtags_handle_job_sig(manager, received);
		if (sigdat->data && sigdat->needfree) {
			/* release rdlock, acquire wrlock instead */
			(void)pthread_rwlock_unlock(&sigdat->rwlock);
			(void)pthread_rwlock_wrlock(&sigdat->rwlock);
			(void)free((void *)sigdat->data);
			sigdat->needfree = sigdat->size = sigdat->type = sigdat->data = 0;
		}
		(void)pthread_rwlock_unlock(&sigdat->rwlock);
	}
	return NULL;
}

static void *
gtags_job_cmdhandler (void *arg)
{
	GJOB_DESC *job;
	JOB_CMD *received;
	THREAD_FIFO_ELEM *cmddat;
	int stopped = 0;
	JOB_MANAGER *manager = arg;
	assert(manager && manager->cmdfifo);
	while (!stopped && (cmddat = thread_fifo_pop_elem(manager->cmdfifo, NULL)) != NULL) {
		(void)pthread_rwlock_rdlock(&cmddat->rwlock);
		received = (JOB_CMD *)cmddat->data;
		assert(received != 0);
		if (received->cmd == HANDLER_JOB_EXIT)
			stopped = 1;
		else
			gtags_handle_job_cmd(manager, received);
		if (cmddat->data && cmddat->needfree) {
			/* release rdlock, acquire wrlock instead */
			(void)pthread_rwlock_unlock(&cmddat->rwlock);
			(void)pthread_rwlock_wrlock(&cmddat->rwlock);
			(void)free((void *)cmddat->data);
			cmddat->needfree = cmddat->size = cmddat->type = cmddat->data = 0;
		}
		(void)pthread_rwlock_unlock(&cmddat->rwlock);
	}
	return NULL;
}

#define GJOB_USED_MAP_SIZE(mg)	((mg->cap >> 6) + 1)

JOB_MANAGER *
job_manager_create (int manager_cap, int cmdfifo_cap, int sigfifo_cap)
{
	int i, r;
	JOB_MANAGER *manager = check_calloc(sizeof(JOB_MANAGER), 1);

	manager->cap = manager_cap;
	manager->pid = getpid();  /* process id */
	manager->jobarray = check_calloc(sizeof(GJOB_DESC), manager_cap);
	manager->used_map = check_calloc(8, GJOB_USED_MAP_SIZE(manager));
	manager->cmdfifo = thread_fifo_create(cmdfifo_cap);
	manager->sigfifo = thread_fifo_create(sigfifo_cap);
	/* init all threading sync vars */
	for (i = 0; i < manager_cap; i++) {
		if ((r = pthread_rwlock_init(&manager->jobarray[i].rwlock, NULL)) != 0)
			die("job manager init %dth job descriptor failed, retcode: %d", i, r);
	}
	if ((r = pthread_mutex_init(&manager->mutex, NULL)) != 0)
		die("job manager init mutex failed, retcode: %d", r);
	if ((r = pthread_rwlock_init(&manager->rwlock, NULL)) != 0)
		die("job manager init jobarray_rwlock failed, retcode: %d", r);
	if ((r = pthread_cond_init(&manager->array_full, NULL)) != 0)
		die("job manager init cv array_full failed, retcode: %d", r);
	if ((r = pthread_cond_init(&manager->array_empty, NULL)) != 0)
		die("job manager init cv array_empty failed, retcode: %d", r);
	manager->state = JOB_MANAGER_STATE_READY;

	return manager;
}

void
job_manager_launch (JOB_MANAGER *manager)
{
	(void)pthread_mutex_lock(&manager->mutex);
	(void)pthread_rwlock_wrlock(&manager->rwlock);
	assert(manager && manager->state == JOB_MANAGER_STATE_READY);
	manager->launcher_tid = pthread_self(); /* launcher tid */
	/* launch all handler jobs */
	(void)pthread_create(&manager->sighandler_tid, NULL, gtags_job_sighandler, manager);
	(void)pthread_create(&manager->cmdhandler_tid, NULL, gtags_job_cmdhandler, manager);
	manager->state = JOB_MANAGER_STATE_LAUNCHED;
	(void)pthread_rwlock_unlock(&manager->rwlock);
	(void)pthread_mutex_unlock(&manager->mutex);
}

void
job_manager_destroy (JOB_MANAGER *manager)
{
	/* please keep destroy steps below in order */
	GJOB_DESC *job;
	(void)pthread_mutex_lock(&manager->mutex);
	(void)pthread_rwlock_wrlock(&manager->rwlock);
	PTHREAD_FORCE_COND_DESTROY(manager->array_empty);
	PTHREAD_FORCE_COND_DESTROY(manager->array_full);
	/* wait handler jobs to complete */
	if (manager->state == JOB_MANAGER_STATE_LAUNCHED) {
		(void)pthread_rwlock_unlock(&manager->rwlock);
		(void)pthread_mutex_unlock(&manager->mutex);
		job_manager_wait_jobs(manager);
		(void)pthread_mutex_lock(&manager->mutex);
		(void)pthread_rwlock_wrlock(&manager->rwlock);
	}
	thread_fifo_destroy(manager->sigfifo);
	thread_fifo_destroy(manager->cmdfifo);
	manager->sigfifo = manager->cmdfifo = NULL;
	/* wait all running jobs to complete */
	for (int i = 0; i < manager->cap; i++) {
		job = &manager->jobarray[i];
		(void)pthread_rwlock_rdlock(&job->rwlock);
		/* wait for active job to complete */
		if (job->status == GJOB_STATUS_ACTIVE) {
			assert(job->tid);
			(void)pthread_join(job->tid, NULL);
		}
		/* clean up job data */
		(void)pthread_rwlock_unlock(&job->rwlock);
		(void)pthread_rwlock_destroy(&job->rwlock);
	}
	(void)free(manager->jobarray);
	(void)free(manager->used_map);
	(void)pthread_rwlock_unlock(&manager->rwlock);
	(void)pthread_rwlock_destroy(&manager->rwlock);
	(void)pthread_mutex_unlock(&manager->mutex);
	(void)pthread_mutex_destroy(&manager->mutex);
	free(manager);
}

GJOB_DESC *
find_unused_job (JOB_MANAGER *manager, int *unusedp)
{
	int i, unused, k = 0;
	for (i = 0; i < GJOB_USED_MAP_SIZE(manager); i++)
		if ((k = __builtin_ffsll(~(long long)manager->used_map[i])) > 0)
			break;
	unused = (i << 6) + k - 1;
	if (unusedp)
		*unusedp = (unused < manager->cap ? unused : -1);
	if (unused < 0 || unused >= manager->cap)
		return NULL; /* job array is full */
	return &manager->jobarray[unused];
}

int
job_manager_idle (JOB_MANAGER *manager)
{
	/* check if job manager idle */
	for (int i = 0; i < GJOB_USED_MAP_SIZE(manager); i++)
		if (manager->used_map[i] != 0)
			return 0;
	return 1;
}

void
job_manager_launch_job (JOB_MANAGER *manager, void *(*routine)(void *), void *jobdata)
{
	int unused;
	GJOB_DESC *job;
	if (!routine)
		return ;

	(void)pthread_mutex_lock(&manager->mutex);
	if ((job = find_unused_job(manager, &unused)) == NULL) { /* job array is now full */
		(void)pthread_cond_wait(&manager->array_full, &manager->mutex);
		job = find_unused_job(manager, &unused);
	}
	assert(job && unused >= 0);
	/* populate job descriptor */
//	(void)pthread_rwlock_wrlock(&manager->rwlock);
	job->index = unused;
	job->routine = routine;
	job->manager = manager;
	job->initiator_tid = pthread_self(); /* who launch the job */
	job->stopcode = GJOB_STOPCODE_UNKNOWN; /* reset stopcode */
	memset(&job->tid, 0, sizeof(pthread_t));
	job->data = jobdata;
	job->status = GJOB_STATUS_ACTIVE;
	/* insert a full memory barrier to before start job */
	HARDWARE_MEMORY_BARRIER;
	/* create job thread */
	(void)pthread_create(&job->tid, NULL, job->routine, job);
	/* set used bitmap */
	manager->used_map[unused >> 6] |= (1 << (unused & 0x3f));
	manager->jobs ++;
//	(void)pthread_rwlock_unlock(&manager->rwlock);
	(void)pthread_mutex_unlock(&manager->mutex);
}

void
job_manager_wait_jobs (JOB_MANAGER *manager)
{
	assert(manager && manager->cmdfifo && manager->sigfifo);
	assert(manager->state == JOB_MANAGER_STATE_LAUNCHED);
	assert(manager->cmdhandler_tid && manager->sighandler_tid);
	/* wait for all jobs to joined by sighandler thread */
	(void)pthread_mutex_lock(&manager->mutex);
	manager->state = JOB_MANAGER_STATE_WAITING;
	(void)pthread_cond_wait(&manager->array_empty, &manager->mutex);
	job_manager_send_sig(manager, HANDLER_JOB_EXIT, -1);
	job_manager_send_cmd(manager, HANDLER_JOB_EXIT, -1);
	/* wait command handler and signal handler to complete */
	(void)pthread_join(manager->cmdhandler_tid, NULL);
	(void)pthread_join(manager->sighandler_tid, NULL);
	memset(&manager->cmdhandler_tid, 0, sizeof(pthread_t));
	memset(&manager->sighandler_tid, 0, sizeof(pthread_t));
	manager->state = JOB_MANAGER_STATE_READY; /* reset manager to READY state */
	(void)pthread_mutex_unlock(&manager->mutex);
}

void
job_manager_send_sig (JOB_MANAGER *manager, int sig, int index)
{
	THREAD_FIFO_ELEM *elem;
	JOB_SIG *send_sig = check_calloc(sizeof(JOB_SIG), 1);
	assert(manager && manager->sigfifo);
	elem = thread_fifo_push_elem(manager->sigfifo, NULL);
	(void)pthread_rwlock_wrlock(&elem->rwlock);
	send_sig->sig = sig;
	send_sig->tid = pthread_self();
	send_sig->index = index;
	elem->data = (data_t)send_sig;
	elem->needfree = 1;
	(void)pthread_rwlock_unlock(&elem->rwlock);
}

void
job_manager_send_cmd (JOB_MANAGER *manager, int cmd, int index)
{
	THREAD_FIFO_ELEM *elem;
	JOB_CMD *send_cmd = check_calloc(sizeof(JOB_CMD), 1);
	assert(manager && manager->cmdfifo);
	elem = thread_fifo_push_elem(manager->cmdfifo, NULL);
	(void)pthread_rwlock_wrlock(&elem->rwlock);
	send_cmd->cmd = cmd;
	send_cmd->tid = pthread_self();
	send_cmd->index = index;
	elem->data = (data_t)send_cmd;
	elem->needfree = 1;
	(void)pthread_rwlock_unlock(&elem->rwlock);
}
