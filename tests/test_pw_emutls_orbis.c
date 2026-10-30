/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* wine/orbis/pw_emutls_orbis.c on the host: a variable's object is the same
 * address on every call from one thread and a different one on each live thread,
 * starts as the variable's initial bytes (or zeros), keeps the alignment the
 * control block asks for, survives the per-thread array growing past many
 * variables, and is given one index however many threads race to first use
 * it. */
#include "pw_emutls_orbis.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

enum { THREADS = 8, MANY = 100 };

static const int init_value = 1234;
static struct pw_emutls_control with_value = { sizeof(int), sizeof(int), 0, &init_value };
static struct pw_emutls_control zeroed = { 64, 64, 0, NULL };
static struct pw_emutls_control many[MANY];
static pthread_barrier_t start, alive;
static void *seen[THREADS];

static void *worker(void *arg)
{
    int *counter, i;
    unsigned char *block;

    pthread_barrier_wait(&start);
    counter = __emutls_get_address(&with_value);
    assert(*counter == 1234);
    *counter += (int)(intptr_t)arg;
    assert(__emutls_get_address(&with_value) == counter);
    block = __emutls_get_address(&zeroed);
    assert(((uintptr_t)block & 63) == 0);
    for (i = 0; i < 64; i++) assert(block[i] == 0);
    memset(block, 0xab, 64);
    for (i = 0; i < MANY; i++) *(int *)__emutls_get_address(&many[i]) = i + (int)(intptr_t)arg;
    for (i = 0; i < MANY; i++) assert(*(int *)__emutls_get_address(&many[i]) == i + (int)(intptr_t)arg);
    assert(*(int *)__emutls_get_address(&with_value) == 1234 + (int)(intptr_t)arg);
    seen[(intptr_t)arg] = counter;
    /* Every thread's objects are alive here; an exited thread's are freed and may be handed out again. */
    pthread_barrier_wait(&alive);
    for (i = 0; i < THREADS; i++) assert(i == (intptr_t)arg || seen[i] != counter);
    pthread_barrier_wait(&alive);
    return NULL;
}

int main(void)
{
    pthread_t threads[THREADS];
    intptr_t i, j;

    for (i = 0; i < MANY; i++) many[i] = (struct pw_emutls_control){ sizeof(int), sizeof(int), 0, NULL };
    pthread_barrier_init(&start, NULL, THREADS);
    pthread_barrier_init(&alive, NULL, THREADS);
    for (i = 0; i < THREADS; i++) assert(!pthread_create(&threads[i], NULL, worker, (void *)i));
    for (i = 0; i < THREADS; i++) assert(!pthread_join(threads[i], NULL));
    /* One index per variable, whatever the race. */
    assert(with_value.index && zeroed.index && with_value.index != zeroed.index);
    for (i = 0; i < MANY; i++)
        for (j = i + 1; j < MANY; j++) assert(many[i].index != many[j].index);
    /* The main thread starts from the initial bytes too. */
    assert(*(int *)__emutls_get_address(&with_value) == 1234);
    return 0;
}
