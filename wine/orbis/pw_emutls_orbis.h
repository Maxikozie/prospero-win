/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_EMUTLS_ORBIS_H
#define PW_EMUTLS_ORBIS_H

#include <stddef.h>
#include <stdint.h>

/* The control block clang (and gcc) emit as __emutls_v.<name> for each
 * thread-local variable under -femulated-tls. */
struct pw_emutls_control
{
    size_t size;        /* of the object, in bytes */
    size_t align;       /* of the object, in bytes */
    uintptr_t index;    /* 0 until first use, then 1-based in this process */
    const void *value;  /* __emutls_t.<name>: the initial bytes, or NULL for zeros */
};

void *__emutls_get_address(struct pw_emutls_control *control);

#endif
