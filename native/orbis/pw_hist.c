/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_hist.h"

#include <stdio.h>
#include <string.h>

void pw_hist_reset(PwHist *hist)
{
    memset(hist, 0, sizeof(*hist));
}

void pw_hist_add(PwHist *hist, uint64_t us)
{
    uint64_t index = us / PW_HIST_BUCKET_US;

    hist->bucket[index < PW_HIST_BUCKETS ? index : PW_HIST_BUCKETS]++;
    if (!hist->n || us < hist->min_us) hist->min_us = us;
    if (us > hist->max_us) hist->max_us = us;
    hist->sum_us += us;
    hist->n++;
}

void pw_hist_summary(const PwHist *hist, PwHistSummary *summary)
{
    uint64_t want, seen = 0;

    memset(summary, 0, sizeof(*summary));
    if (!hist->n) return;
    summary->n = hist->n;
    summary->min_us = hist->min_us;
    summary->max_us = hist->max_us;
    summary->avg_us = hist->sum_us / hist->n;
    /* The smallest bucket whose running count reaches 95 % of the samples
     * (rounded up, so one sample of 20 is the 95th percentile's own). */
    want = (hist->n * 95 + 99) / 100;
    for (unsigned i = 0; i <= PW_HIST_BUCKETS; i++) {
        seen += hist->bucket[i];
        if (seen < want) continue;
        summary->p95_us = i < PW_HIST_BUCKETS ? (uint64_t)(i + 1) * PW_HIST_BUCKET_US : hist->max_us;
        if (summary->p95_us > hist->max_us) summary->p95_us = hist->max_us;
        if (summary->p95_us < hist->min_us) summary->p95_us = hist->min_us;
        break;
    }
}

int pw_hist_fields(const PwHist *hist, const char *name, char *out, size_t size)
{
    PwHistSummary s;

    if (!hist->n)
        return snprintf(out, size, "%s_min=- %s_avg=- %s_p95=- %s_max=-", name, name, name, name);
    pw_hist_summary(hist, &s);
    return snprintf(out, size, "%s_min=%llu %s_avg=%llu %s_p95=%llu %s_max=%llu", name,
                    (unsigned long long)s.min_us, name, (unsigned long long)s.avg_us, name,
                    (unsigned long long)s.p95_us, name, (unsigned long long)s.max_us);
}

unsigned pw_hist_sparse(const PwHist *hist, unsigned *next, char *out, size_t size)
{
    unsigned written = 0;
    size_t used = 0;

    if (size) out[0] = 0;
    for (; *next <= PW_HIST_BUCKETS; (*next)++) {
        char item[32];
        int n;
        if (!hist->bucket[*next]) continue;
        n = snprintf(item, sizeof(item), "%s%u:%u", written ? "," : "", *next, hist->bucket[*next]);
        if (n < 0 || used + (size_t)n + 1 > size) break;
        memcpy(out + used, item, (size_t)n + 1);
        used += (size_t)n;
        written++;
    }
    return written;
}
