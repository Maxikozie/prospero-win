/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * __emutls_get_address for app PRXs built with -femulated-tls.
 *
 * A PRX is linked -pie, and lld relaxes a -pie image's TLS accesses to
 * local-exec: fixed offsets from %fs:0 into the main executable's block
 * (measured on ntdll.shared.elf: a PT_TLS segment, no TLS dynamic
 * relocation, only %fs:0-relative loads). In a module that is not the eboot
 * those offsets point into somebody else's memory, and the SDK's link.x does
 * not even collect -fdata-sections' .tbss.<name> sections. Code with real
 * thread-locals (RADV in libvulkan.prx: ACO's instruction buffer, u_debug,
 * u_call_once, u_qsort) is therefore compiled with -femulated-tls, which
 * turns each variable into a control block __emutls_v.<name> and each access
 * into a call here. The OpenOrbis SDK ships no emutls runtime.
 *
 * Every thread gets an array of object pointers, indexed by a number given to
 * each variable on its first use in the process; an object is allocated on
 * the thread's first use and filled from the variable's initial bytes or with
 * zeros. Thread keys are libkernel's; the memory comes from posix_memalign and
 * goes back through free, which an importing PRX takes from ntdll (the malloc
 * family in build_wine_orbis.sh's import_also), so both ends are one heap.
 */
#include "pw_emutls_orbis.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct pw_emutls_array
{
    size_t count;
    void *object[];
};

static pthread_once_t key_once = PTHREAD_ONCE_INIT;
static pthread_key_t key;
static pthread_mutex_t index_lock = PTHREAD_MUTEX_INITIALIZER;
static uintptr_t last_index;

static void free_array(void *data)
{
    struct pw_emutls_array *array = data;
    size_t i;

    for (i = 0; i < array->count; i++) free(array->object[i]);
    free(array);
}

static void create_key(void)
{
    if (pthread_key_create(&key, free_array)) abort();
}

static uintptr_t object_index(struct pw_emutls_control *control)
{
    uintptr_t index = __atomic_load_n(&control->index, __ATOMIC_ACQUIRE);

    if (index) return index;
    pthread_mutex_lock(&index_lock);
    if (!(index = control->index))
    {
        index = ++last_index;
        __atomic_store_n(&control->index, index, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&index_lock);
    return index;
}

static void *new_object(const struct pw_emutls_control *control)
{
    size_t align = control->align < sizeof(void *) ? sizeof(void *) : control->align;
    void *object;

    if (posix_memalign(&object, align, control->size ? control->size : 1)) abort();
    if (control->value) memcpy(object, control->value, control->size);
    else memset(object, 0, control->size);
    return object;
}

static struct pw_emutls_array *grow(struct pw_emutls_array *array, uintptr_t index)
{
    size_t old = array ? array->count : 0, count = old ? old * 2 : 16;

    while (count < index) count *= 2;
    if (!(array = realloc(array, sizeof(*array) + count * sizeof(void *)))) abort();
    memset(array->object + old, 0, (count - old) * sizeof(void *));
    array->count = count;
    if (pthread_setspecific(key, array)) abort();
    return array;
}

void *__emutls_get_address(struct pw_emutls_control *control)
{
    uintptr_t index = object_index(control);
    struct pw_emutls_array *array;

    pthread_once(&key_once, create_key);
    array = pthread_getspecific(key);
    if (!array || array->count < index) array = grow(array, index);
    if (!array->object[index - 1]) array->object[index - 1] = new_object(control);
    return array->object[index - 1];
}
