/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
/*======
This file is part of PerconaFT.


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.

----------------------------------------

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License, version 3,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.
======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

#include <toku_portability.h>
#include <errno.h>
#include <string.h>

#include "portability/toku_assert.h"
#include "util/minicron.h"

toku_instr_key *minicron_p_mutex_key;
toku_instr_key *minicron_p_condvar_key;
toku_instr_key *minicron_thread_key;

static void toku_gettime(toku_timespec_t *a) {
    struct timeval tv;
    gettimeofday(&tv, 0);
    a->tv_sec  = tv.tv_sec;
    a->tv_nsec = tv.tv_usec * 1000LL;
}
    

static int
timespec_compare (toku_timespec_t *a, toku_timespec_t *b) {
    if (a->tv_sec > b->tv_sec) return 1;
    if (a->tv_sec < b->tv_sec) return -1;
    if (a->tv_nsec > b->tv_nsec) return 1;
    if (a->tv_nsec < b->tv_nsec) return -1;
    return 0;
}

// Implementation notes:
//  When calling do_shutdown or change_period, the mutex is obtained, the variables in the minicron struct are modified, and
//  the condition variable is signalled.  Possibly the minicron thread will miss the signal.  To avoid this problem, whenever
//  the minicron thread acquires the mutex, it must check to see what the variables say to do (e.g., should it shut down?).

static void*
minicron_do (void *pv)
{
    struct minicron *CAST_FROM_VOIDP(p, pv);
    toku_mutex_lock(&p->mutex);
    while (1) {
        if (p->do_shutdown) {
            toku_mutex_unlock(&p->mutex);
            toku_instr_delete_current_thread();
            return toku_pthread_done(nullptr);
        }
        if (p->period_in_ms == 0) {
            // if we aren't supposed to do it then just do an untimed wait.
            toku_cond_wait(&p->condvar, &p->mutex);
        } 
        else if (p->period_in_ms <= 1000) {
            uint32_t period_in_ms = p->period_in_ms;
            toku_mutex_unlock(&p->mutex);
            usleep(period_in_ms * 1000);
            toku_mutex_lock(&p->mutex);
        }
        else {
            // Recompute the wakeup time every time (instead of once per call to f) in case the period changges.
            toku_timespec_t wakeup_at = p->time_of_last_call_to_f;
            wakeup_at.tv_sec += (p->period_in_ms/1000);
            wakeup_at.tv_nsec += (p->period_in_ms % 1000) * 1000000;
            toku_timespec_t now;
            toku_gettime(&now);
            int compare = timespec_compare(&wakeup_at, &now);
            // if the time to wakeup has yet to come, then we sleep
            // otherwise, we continue
            if (compare > 0) {
                int r = toku_cond_timedwait(&p->condvar, &p->mutex, &wakeup_at);
                if (r!=0 && r!=ETIMEDOUT) fprintf(stderr, "%s:%d r=%d (%s)", __FILE__, __LINE__, r, strerror(r));
                assert(r==0 || r==ETIMEDOUT);
            }
        }
        // Now we woke up, and we should figure out what to do
        if (p->do_shutdown) {
            toku_mutex_unlock(&p->mutex);
            toku_instr_delete_current_thread();
            return toku_pthread_done(nullptr);
        }
        if (p->period_in_ms > 1000) {
            toku_timespec_t now;
            toku_gettime(&now);
            toku_timespec_t time_to_call = p->time_of_last_call_to_f;
            time_to_call.tv_sec += p->period_in_ms/1000;
            time_to_call.tv_nsec += (p->period_in_ms % 1000) * 1000000;
            int compare = timespec_compare(&time_to_call, &now);
            if (compare <= 0) {
                toku_gettime(&p->time_of_last_call_to_f); // the measured period includes the time to make the call.
                toku_mutex_unlock(&p->mutex);
                int r = p->f(p->arg);
                assert(r==0);
                toku_mutex_lock(&p->mutex);
                
            }
        }
        else if (p->period_in_ms != 0) {
            toku_mutex_unlock(&p->mutex);
            int r = p->f(p->arg);
            assert(r==0);
            toku_mutex_lock(&p->mutex);
        }
    }
}

int
toku_minicron_setup(struct minicron *p, uint32_t period_in_ms, int(*f)(void *), void *arg)
{
    p->f = f;
    p->arg = arg;
    toku_gettime(&p->time_of_last_call_to_f);
    // printf("now=%.6f", p->time_of_last_call_to_f.tv_sec +
    // p->time_of_last_call_to_f.tv_nsec*1e-9);
    p->period_in_ms = period_in_ms;
    p->do_shutdown = false;
    toku_mutex_init(*minicron_p_mutex_key, &p->mutex, nullptr);
    toku_cond_init(*minicron_p_condvar_key, &p->condvar, nullptr);
    return toku_pthread_create(
        *minicron_thread_key, &p->thread, nullptr, minicron_do, p);
}

void toku_minicron_change_period(struct minicron *p, uint32_t new_period) {
    toku_mutex_lock(&p->mutex);
    p->period_in_ms = new_period;
    toku_cond_signal(&p->condvar);
    toku_mutex_unlock(&p->mutex);
}

/* unlocked function for use by engine status which takes no locks */
uint32_t
toku_minicron_get_period_in_seconds_unlocked(struct minicron *p)
{
    uint32_t retval = p->period_in_ms/1000;
    return retval;
}

/* unlocked function for use by engine status which takes no locks */
uint32_t
toku_minicron_get_period_in_ms_unlocked(struct minicron *p)
{
    uint32_t retval = p->period_in_ms;
    return retval;
}

int
toku_minicron_shutdown(struct minicron *p) {
    toku_mutex_lock(&p->mutex);
    assert(!p->do_shutdown);
    p->do_shutdown = true;
    //printf("%s:%d signalling\n", __FILE__, __LINE__);
    toku_cond_signal(&p->condvar);
    toku_mutex_unlock(&p->mutex);
    void *returned_value;
    //printf("%s:%d joining\n", __FILE__, __LINE__);
    int r = toku_pthread_join(p->thread, &returned_value);
    if (r!=0) fprintf(stderr, "%s:%d r=%d (%s)\n", __FILE__, __LINE__, r, strerror(r));
    assert(r==0);  assert(returned_value==0);
    toku_cond_destroy(&p->condvar);
    toku_mutex_destroy(&p->mutex);
    //printf("%s:%d shutdowned\n", __FILE__, __LINE__);
    return 0;
}

bool
toku_minicron_has_been_shutdown(struct minicron *p) {
    return p->do_shutdown;
}
