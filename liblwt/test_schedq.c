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

//  To test internal interfaces, its easiest to include the implementation
//  file and call the functions directly instead of exposing static functions
//  just for testing.

#include "lwt_sched.c"

#define	ins(name)	schedq_insert(schedq, sqix, name, name ## idix)
#define	rem()		schedq_remove(schedq, sqix)

#include <stdlib.h>
#include <stdio.h>

void errexit(const char *s)
{
	perror(s);
	exit(1);
}

int main()
{
	int error = lwt_init(0);
	if (error)
		errexit("lwt_init() failed");

	thr_t *a = thr_create_main();
	ureg_t aidix = THRID_INDEX(a->thra.thra_thrid);

	thr_t *b = thr_create_main();
	ureg_t bidix = THRID_INDEX(b->thra.thra_thrid);

	thr_t *c = thr_create_main();
	ureg_t cidix = THRID_INDEX(c->thra.thra_thrid);

	thr_t *d = thr_create_main();
	ureg_t didix = THRID_INDEX(d->thra.thra_thrid);

	thr_t *e = thr_create_main();
	ureg_t eidix = THRID_INDEX(e->thra.thra_thrid);

	thr_t *f = thr_create_main();
	ureg_t fidix = THRID_INDEX(f->thra.thra_thrid);

	thr_t *g = thr_create_main();
	ureg_t gidix = THRID_INDEX(g->thra.thra_thrid);

	thr_t *h = thr_create_main();
	ureg_t hidix = THRID_INDEX(h->thra.thra_thrid);

	schdom_t *schdom = &a->thr_core->core_hw->hw_schdom;
        schedq_t *schedq = &schdom->schdom_sqcls[a->thr_prio].sqcl_schedq;
        ureg_t sqix = schedq_index(schedq);

	thr_t *t;

	ins(a);
	ins(b);
	ins(c);
	ins(d);
	ins(e);
	ins(f);

	t = rem();
	assert(t == a);

	ins(g);
	ins(h);

	t = rem();
	assert(t == b);

	t = rem();
	assert(t == c);

	t = rem();
	assert(t == d);

	t = rem();
	assert(t == e);

	t = rem();
	assert(t == f);

	t = rem();
	assert(t == g);

	t = rem();
	assert(t == h);
}

