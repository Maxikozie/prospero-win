/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_HIST_H
#define PW_HIST_H
/*
 * A fixed histogram of durations in microseconds, for the title's per-present
 * timings (phase p, docs/ps4/HANDOFF-p.md in ps4-wine): 100 us buckets up to
 * 100 ms and one overflow bucket, no allocation, so adding a sample costs the
 * same in every frame. Minimum, maximum and the sum are exact; the 95th
 * percentile comes from the buckets (the upper edge of the bucket that holds
 * it, never above the maximum).
 */
#include <stddef.h>
#include <stdint.h>

enum { PW_HIST_BUCKET_US = 100, PW_HIST_BUCKETS = 1000 };   /* bucket PW_HIST_BUCKETS: 100 ms and more */

typedef struct PwHist {
    uint32_t bucket[PW_HIST_BUCKETS + 1];
    uint64_t n, sum_us, min_us, max_us;
} PwHist;

typedef struct PwHistSummary {
    uint64_t n, min_us, avg_us, p95_us, max_us;
} PwHistSummary;

void pw_hist_reset(PwHist *hist);
void pw_hist_add(PwHist *hist, uint64_t us);
/* All zeros when the histogram is empty. avg is rounded down. */
void pw_hist_summary(const PwHist *hist, PwHistSummary *summary);
/* "NAME_min=A NAME_avg=B NAME_p95=C NAME_max=D" (no leading space), with
 * "-" for each value of an empty histogram. Returns what snprintf returns. */
int pw_hist_fields(const PwHist *hist, const char *name, char *out, size_t size);
/* The non-empty buckets from index *next on as "index:count,..." into out,
 * as many as fit; *next becomes the index to continue from, PW_HIST_BUCKETS + 1
 * when every bucket is written. Returns the number of buckets written. For
 * records of bounded size (netlog): call until *next passes the end. */
unsigned pw_hist_sparse(const PwHist *hist, unsigned *next, char *out, size_t size);
#endif
