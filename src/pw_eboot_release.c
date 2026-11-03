/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* From: plastikman/orbis-win src/reloc-wine-app/app_entry.c @ 2610706 (LGPL-2.1-or-later);
 * see pw_eboot_release.h for what was kept and what was rewritten. */
#include "pw_eboot_release.h"
#include <string.h>

static uintptr_t page_down(uintptr_t value) { return value & ~(PW_EBOOT_PAGE - 1); }

int pw_eboot_release_plan(const PwPrxSegment *segments, uint32_t count, PwEbootRelease *out)
{
    PwEbootRange ranges[PW_PRX_MAX_SEGMENTS];

    if (!segments || !out || !count || count > PW_PRX_MAX_SEGMENTS) return PW_EBOOT_ERR_ARGUMENT;
    for (uint32_t i = 0; i < count; i++)
    {
        uintptr_t base = (uintptr_t)segments[i].address, size = segments[i].size;

        if (!base || !size || base >= PW_EBOOT_LIMIT || size > PW_EBOOT_LIMIT - base) return PW_EBOOT_ERR_RANGE;
        ranges[i].base = page_down(base);
        ranges[i].end = page_down(base + size + PW_EBOOT_PAGE - 1);
    }
    /* Insertion sort by base, then merge ranges that overlap or touch. */
    for (uint32_t i = 1; i < count; i++)
        for (uint32_t j = i; j > 0 && ranges[j].base < ranges[j - 1].base; j--)
        {
            PwEbootRange swap = ranges[j];
            ranges[j] = ranges[j - 1];
            ranges[j - 1] = swap;
        }
    out->range_count = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        PwEbootRange *last = out->range_count ? &out->ranges[out->range_count - 1] : NULL;

        if (last && ranges[i].base <= last->end)
        {
            if (ranges[i].end > last->end) last->end = ranges[i].end;
        }
        else out->ranges[out->range_count++] = ranges[i];
    }
    out->low_base = out->ranges[0].base;
    out->low_end = out->ranges[out->range_count - 1].end;
    return PW_EBOOT_OK;
}

static int read_info(const PwEbootReleaseOps *ops, PwPrxSegment *segments, uint32_t *count, int *rc)
{
    uint8_t info[PW_PRX_MODULE_INFO_BYTES];
    uint64_t size_word = sizeof(info);

    memset(info, 0, sizeof(info));
    memcpy(info, &size_word, sizeof(size_word));
    *count = 0;
    if ((*rc = ops->eboot_info(ops->context, info)) != 0) return PW_EBOOT_ERR_MODULE_INFO;
    /* The console may clear the size word on success; the parser checks it. */
    memcpy(info, &size_word, sizeof(size_word));
    return pw_prx_parse_module_info(info, NULL, segments, count) == PW_PRX_OK ? PW_EBOOT_OK
                                                                               : PW_EBOOT_ERR_MODULE_INFO;
}

int pw_eboot_release(const PwEbootReleaseOps *ops, PwEbootRelease *out)
{
    PwPrxSegment after[PW_PRX_MAX_SEGMENTS];
    int status;

    if (!out) return PW_EBOOT_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!ops || !ops->eboot_info || !ops->unmap) return out->status = PW_EBOOT_ERR_ARGUMENT;

    if ((status = read_info(ops, out->segments, &out->segment_count, &out->info_rc)) != PW_EBOOT_OK)
        return out->status = status;
    if ((status = pw_eboot_release_plan(out->segments, out->segment_count, out)) != PW_EBOOT_OK)
        return out->status = status;

    for (uint32_t i = 0; i < out->range_count; i++)
    {
        int rc = ops->unmap(ops->context, (void *)out->ranges[i].base, out->ranges[i].end - out->ranges[i].base);

        if (rc)
        {
            out->unmap_rc = rc;
            out->status = PW_EBOOT_ERR_UNMAP;
            break;
        }
        out->unmapped++;
    }

    /* Whether module 0's info still lists the unmapped segments: anything
     * that walks the module list afterwards (unwinding, dl_iterate_phdr)
     * would read them. */
    status = read_info(ops, after, &out->after_count, &out->after_rc);
    if (out->after_rc) out->after = PW_EBOOT_AFTER_ERR;
    else out->after = (status == PW_EBOOT_OK && out->after_count) ? PW_EBOOT_AFTER_LISTED : PW_EBOOT_AFTER_GONE;
    return out->status;
}

const char *pw_eboot_release_after_name(int after)
{
    switch (after)
    {
    case PW_EBOOT_AFTER_LISTED: return "listed";
    case PW_EBOOT_AFTER_GONE: return "gone";
    case PW_EBOOT_AFTER_ERR: return "err";
    default: return "-";
    }
}

int pw_eboot_copy_argv(int argc, char *const *argv, char *store, size_t store_size, char **out, int max_args)
{
    size_t used = 0;
    int n = 0;

    if (!out || max_args < 1) return 0;
    for (int i = 0; argv && store && i < argc && n < max_args - 1 && argv[i]; i++)
    {
        size_t length = strlen(argv[i]) + 1;

        if (length > store_size - used) break;
        memcpy(store + used, argv[i], length);
        out[n++] = store + used;
        used += length;
    }
    out[n] = NULL;
    return n;
}
