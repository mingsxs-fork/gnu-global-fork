/*
 * Copyright (c) Ming Li
 *
 * adgio.ming@gmail.com
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

#ifndef _THREADING_H_
#define _THREADING_H_

#ifdef HAVE_STRING_H
#include <string.h>
#else
#include <strings.h>
#endif
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#include <time.h>
#ifdef HAVE_PTHREAD_H
#include <pthread.h>
#endif
#ifdef HAVE_STDINT_H
#include <stdint.h>
#else
typedef unsigned long long uint64_t;
#endif
#include <stddef.h>

#ifdef __cplusplus__
extern "C" {
#endif

/*
 * thread_local keyword test macro for GNU C and other compiler,
 * pleaes refer to
 * https://stackoverflow.com/questions/18298280/how-to-declare-a-variable-as-thread-local-portably
 */
#ifndef thread_local
#if __STDC_VERSION__ >= 201112 && !defined __STDC_NO_THREADS__
#	define thread_local _Thread_local
#elif defined _WIN32 && ( \
		defined _MSC_VER || \
		defined __ICL || \
		defined __DMC__ || \
		defined __BORLANDC__ )
#	define thread_local __declspec(thread) 
/* note that ICC (linux) and Clang are covered by __GNUC__ */
#elif defined __GNUC__ || \
	defined __SUNPRO_C || \
	defined __xlC__
#	define thread_local __thread
#else
#	error "Cannot define thread_local"
#endif
#endif

#define SOFTWARE_MEMORY_BARRIER \
	asm volatile(""\
			: \
			: \
			: "memory")
#define HARDWARE_MEMORY_BARRIER \
	__sync_synchronize()

/* SQLITE3 should support concurrent access itself */
#if (!defined(USE_SQLITE3) && !defined(USE_THREADING))
#define USE_THREADING 1 /* enable threading */
#endif

typedef uint64_t data_t;  /* thread data type */

/* thread fifo implementation */
#define THREAD_FIFO_MAX_CAP	1024
#define JOB_MANAGER_MAX_CAP	100

#define THREAD_FIFO_DATA_TYPE_PTR	1
#define THREAD_FIFO_DATA_TYPE_STR	2
#define THREAD_FIFO_DATA_TYPE_INT	3

typedef struct thread_fifo_elem {
	data_t data;	/* DO NOT use thread stack memory if it's pointer */
	int size;		/* data size */
	int type;		/* possibly unnecessary */
	int needfree;	/* indicate the data is heap memory chunk and it needs free */
	/* data is a pointer shared between threads, so it needs a rwlock */
	pthread_rwlock_t rwlock;
} THREAD_FIFO_ELEM;

/* thread msgfifo is from left to right,
 * one thread pushs msg at right which is the endidx of the fifo,
 * the other pops msg from left which is the begidx of the fifo.
 */
typedef struct thread_fifo {
	int elemcap;		/* fifo elem capability */
	int begidx;			/* fifo begin index for poping elem from */
	int endidx;			/* fifo end index for pushing elem to */
	THREAD_FIFO_ELEM *elemfifo;	/* fixed length of array */
	/* thread synchronization */
	pthread_mutex_t mutex;
	pthread_cond_t on_full;
	pthread_cond_t on_empty;
} THREAD_FIFO;

THREAD_FIFO *thread_fifo_create(int);
void thread_fifo_flush(THREAD_FIFO *, void (*)(void *));
void thread_fifo_destroy(THREAD_FIFO *);
void *thread_fifo_push_elem(THREAD_FIFO *, struct timespec *);
void *thread_fifo_pop_elem(THREAD_FIFO *, struct timespec *);


/* JOB MANAGER IMPLEMENTATION */
#define GJOB_SIG_DONE		2	/* job completed normally */
#define GJOB_SIG_ERROR		3	/* job has nonfatal error */
#define GJOB_SIG_DEAD		4	/* job has fatal error and dead itself */

#define HANDLER_JOB_EXIT	255	/* handler exit */

#define GJOB_STATUS_ACTIVE		1
#define GJOB_STATUS_DONE		2
#define GJOB_STATUS_DEAD		-1

#define GJOB_STOPCODE_UNKNOWN	-1
#define GJOB_STOPCODE_SUCCESS	1
#define GJOB_STOPCODE_FAILURE	2

#define JOB_MANAGER_STATE_LAUNCHED	1	/* job manager has been launched */
#define JOB_MANAGER_STATE_WAITING	2	/* job manager is waiting jobs to complete */
#define JOB_MANAGER_STATE_READY		0

typedef struct gtags_job_desc GJOB_DESC;
typedef struct job_manager JOB_MANAGER;

struct job_manager {
	pid_t pid;		/* process id, shared by all threads */
	pthread_t launcher_tid;		/* who launch the job manager */
	pthread_t sighandler_tid;	/* who handle all signals from launched jobs, and also join jobs */
	pthread_t cmdhandler_tid;	/* who handle all commands from launched jobs if any */
	int cap;			/* how many jobs we can launch simultaneously */
	int jobs;			/* how many jobs are running currently */
	int state;			/* job manager has been launched */
	GJOB_DESC *jobarray;	/* job array */
	uint64_t *used_map;		/* job descriptor inuse bitmap */

	THREAD_FIFO *cmdfifo;	/* command fifo shared by jobs */
	THREAD_FIFO *sigfifo;	/* signal fifo shared by jobs */
	/*
	 * below thread synchronizations are shared among all tasks which
	 * are created and attached to this task pool, each lock/cond refers
	 * to some shared resource, e.g. operation flow or memory resources.
	 * and each resource can be acquired by a single thread.
	 */
	pthread_mutex_t mutex; /* used between launcher and handler */
	pthread_rwlock_t rwlock; /* used between handler and launched jobs */
	pthread_cond_t array_full;		/* launcher wait when jobarray is full */
	pthread_cond_t array_empty;	/* launcher wait for all jobs to complete */
};

struct gtags_job_desc {
	pthread_t tid; /* pthread thread id */
	pthread_t initiator_tid;
	int index;	/* job index in manager jobarray */
	int status;	/* job current status */
	int stopcode;	/* job return code, currently useless */
	void *(*routine)(void *);	/* job start routine for pthread_create */
	struct job_manager *manager; /* job manager entry */
	void *data;	/* job required misc data */
	pthread_rwlock_t rwlock;
};

JOB_MANAGER *job_manager_create(int, int, int);	/* used by main thread, probably the same with initiator thread */
void job_manager_launch(JOB_MANAGER *);		/* used by main thread, probably the same with initiator thread */
void job_manager_destroy(JOB_MANAGER *);	/* used by main thread, probably the same with initiator thread */
void job_manager_launch_job(JOB_MANAGER *, void *(*)(void *), void *);		/* used by initiator thread */
void job_manager_wait_jobs(JOB_MANAGER *);	/* used by main thread */
void job_manager_send_sig(JOB_MANAGER *, int, int);	/* used by job threads */
void job_manager_send_cmd(JOB_MANAGER *, int, int);	/* used by job thread */

#ifdef __cplusplus__
}
#endif
#endif /* ! _THREADING_H_ */
