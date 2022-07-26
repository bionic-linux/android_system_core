/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* clang-format off */
/* see the comment in lwt_sched.h about issues and the many clang-format bugs */

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include "lwt.h"

const char *volatile __assert_file;
const char *volatile __assert_msg;
int volatile __assert_line;

static _Noreturn void assert_fail(const char *file, int line,
				      const char *msg)
{
	__assert_line = line;
	__assert_msg = msg;
	__assert_file = file;
	for (;;)
		*((volatile int *)11) = 0xDEADFEED;
}

#define	test_assert(expr)						\
	do {								\
		if (__builtin_expect(!(expr), 0))			\
			assert_fail(__FILE__, __LINE__, #expr);		\
	} while (0)

void errexit(const char *s, int error)
{
	fprintf(stderr, "%s: %s\n", s, strerror(error));
	exit(1);
}

#define	QUEUE_MAX	10

typedef unsigned long	data_t;

#define	DATA_LAST	((data_t) ~0uL)

typedef struct {
	lwt_mutex_t	 q_mutex;
	lwt_cond_t	 q_not_empty;
	lwt_cond_t	 q_not_full;
	int		 q_first;
	int		 q_last;
	int		 q_count;
	data_t		 q_data[QUEUE_MAX];
} queue_t;

typedef enum { READER, WRITER } kind_t;

typedef struct {
	kind_t		 a_kind;
	queue_t		*a_queue;
	lwt_t		 a_lwt;
	data_t		 a_work;
	data_t		 a_start;
	data_t		 a_sum;
	data_t		 a_steps;
} arg_t;

int queue_init(queue_t *q)
{
	int error = lwt_mutex_init(&q->q_mutex, NULL);
	if (error)
		return error;

	error = lwt_cond_init(&q->q_not_empty, NULL);
	if (error)
		return error;

	error = lwt_cond_init(&q->q_not_full, NULL);
	if (error)
		return error;

	q->q_first = 0;
	q->q_last  = 0;
	q->q_count = 0;
	return 0;
}

void queue_insert(queue_t *q, data_t data)
{
	lwt_mutex_lock(&q->q_mutex);
	while (q->q_count == QUEUE_MAX)
		lwt_cond_wait(&q->q_not_full, &q->q_mutex);
	if (q->q_count == 0)
		lwt_cond_broadcast(&q->q_not_empty, &q->q_mutex);
	++q->q_count;
	q->q_data[q->q_last] = data;
	if (++q->q_last >= QUEUE_MAX)
		q->q_last = 0;
	lwt_mutex_unlock(&q->q_mutex);
}

int queue_remove(queue_t *q)
{
	lwt_mutex_lock(&q->q_mutex);
	while (q->q_count == 0)
		lwt_cond_wait(&q->q_not_empty, &q->q_mutex);
	if (q->q_count == QUEUE_MAX)
		lwt_cond_broadcast(&q->q_not_full, &q->q_mutex);
	--q->q_count;
	int data = q->q_data[q->q_first];
	if (++q->q_first >= QUEUE_MAX)
		q->q_first = 0;
	lwt_mutex_unlock(&q->q_mutex);
	return data;
}

#define	WRITER_RETVAL	((void *) 0xDEADBEEF)

void *writer(arg_t *a)
{
	queue_t *q = a->a_queue;
	data_t data = a->a_start;
	data_t inc = a->a_sum;
	data_t steps = a->a_steps;
	data_t i = 0;
	for (i = 0; i < steps; ++i) {
		queue_insert(q, data);
		data += inc;
		++a->a_work;
	}
	queue_insert(q, DATA_LAST);
	lwt_exit(WRITER_RETVAL);
}

void *reader(arg_t *a)
{
	queue_t *q = a->a_queue;
	data_t data;
	for (;;) {
		data = queue_remove(q);
		if (data == DATA_LAST)
			return (void *) a->a_sum;
		a->a_sum += data;
		++a->a_work;
	}
}

#define	NREADERS	16
#define	NWRITERS	16
#define	NSTEPS		100

arg_t	args[NREADERS + NWRITERS];

#if defined(LWT_ARM64) || defined(LWT_X64)
#define	test_main(argc, argv)	main(argc, argv)
#endif

int test_main(int argc, char *argv[])
{
	data_t nsteps = NSTEPS;
	if (argc >= 2)
		nsteps = atol(argv[1]);

	size_t sched_attempt_steps = 0;
	if (argc == 3)
		sched_attempt_steps = atol(argv[2]);

	int error = lwt_init(sched_attempt_steps, SIGRTMIN);
	if (error)
		errexit("lwt_init() failed", error);

	queue_t	queue;
	error = queue_init(&queue);
	if (error)
		errexit("queue_init() failed", error);

	arg_t *a = args;
	int i;
	for (i = 0; i < NWRITERS; ) {
		++i;
		a->a_kind = WRITER;
		a->a_queue = &queue;
		a->a_work = 0;
		a->a_start = i;
		a->a_sum = NWRITERS;
		a->a_steps = nsteps;
		error = lwt_create(&a->a_lwt, NULL, (void *(*)(void *)) writer, a);
		if (error)
			errexit("lwt_create() writer failed", error);
		++a;
	}

	for (i = 0; i < NREADERS; ) {
		a->a_kind = READER;
		a->a_queue = &queue;
		a->a_work = 0;
		a->a_start = 0;
		a->a_sum = 0;
		a->a_steps = 0;
		++i;

		if (i == NREADERS)
			break;

		error = lwt_create(&a->a_lwt, NULL, (void *(*)(void *)) reader, a);
		if (error)
			errexit("lwt_create() reader failed", error);
		++a;
	}

#if 0
	const char *m = "blocking main(), return to continue it...";
	write(2, m, strlen(m));
#if 0
	char buf[100];
	read(0, buf, sizeof(buf));
#else
	pause();
#endif
#endif

	data_t sum = (data_t) reader(a);
	a = args;

	for (i = 0; i < NWRITERS; ++i) {
		void *retval;
		error = lwt_join(a->a_lwt, &retval);
		if (error)
			errexit("lwt_join() writer failed", error);
		test_assert(retval == WRITER_RETVAL);
		++a;
	}

	for (i = 0; i < NREADERS - 1; ++i) {
		void *retval;
		error = lwt_join(a->a_lwt, &retval);
		if (error)
			errexit("lwt_join() reader failed", error);
		sum += (data_t) retval;
		++a;
	}

	test_assert(sum == (NWRITERS * nsteps) * (NWRITERS * nsteps + 1) / 2);

	exit(0);
}

