/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../native/orbis/pw_hist.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static PwHist hist;

int main(void)
{
    PwHistSummary s;
    char out[256];
    unsigned next, n, total;

    /* Empty: zeros, and "-" in the fields. */
    pw_hist_reset(&hist);
    pw_hist_summary(&hist, &s);
    assert(s.n == 0 && s.min_us == 0 && s.avg_us == 0 && s.p95_us == 0 && s.max_us == 0);
    pw_hist_fields(&hist, "copy_us", out, sizeof(out));
    assert(!strcmp(out, "copy_us_min=- copy_us_avg=- copy_us_p95=- copy_us_max=-"));

    /* One sample: min, avg, p95 and max are that sample (p95 is clamped to it,
     * not the bucket's upper edge). */
    pw_hist_add(&hist, 1234);
    pw_hist_summary(&hist, &s);
    assert(s.n == 1 && s.min_us == 1234 && s.avg_us == 1234 && s.p95_us == 1234 && s.max_us == 1234);

    /* 100 samples 1..100 ms in 1 ms steps: the 95th is 95 ms, in the bucket
     * 95.0-95.1 ms, reported as its upper edge. */
    pw_hist_reset(&hist);
    for (unsigned i = 1; i <= 100; i++) pw_hist_add(&hist, i * 1000);
    pw_hist_summary(&hist, &s);
    assert(s.n == 100 && s.min_us == 1000 && s.max_us == 100000 && s.avg_us == 50500);
    assert(s.p95_us == 95100);
    pw_hist_fields(&hist, "guest_us", out, sizeof(out));
    assert(!strcmp(out, "guest_us_min=1000 guest_us_avg=50500 guest_us_p95=95100 guest_us_max=100000"));

    /* A tail in the overflow bucket: p95 there is the exact maximum. */
    pw_hist_reset(&hist);
    for (unsigned i = 0; i < 90; i++) pw_hist_add(&hist, 16600);
    for (unsigned i = 0; i < 10; i++) pw_hist_add(&hist, 250000 + i);
    pw_hist_summary(&hist, &s);
    assert(s.p95_us == 250009 && s.max_us == 250009 && s.min_us == 16600);
    assert(hist.bucket[166] == 90 && hist.bucket[PW_HIST_BUCKETS] == 10);

    /* 20 samples, 19 at 1 ms and one at 50 ms: ceil(0.95 * 20) = 19, so the
     * 95th percentile is in the 1.0-1.1 ms bucket, reported as its edge. */
    pw_hist_reset(&hist);
    for (unsigned i = 0; i < 19; i++) pw_hist_add(&hist, 1000);
    pw_hist_add(&hist, 50000);
    pw_hist_summary(&hist, &s);
    assert(s.p95_us == 1100 && s.max_us == 50000);
    /* 0 us lands in bucket 0; a p95 below the minimum cannot happen. */
    pw_hist_reset(&hist);
    pw_hist_add(&hist, 0);
    pw_hist_add(&hist, 0);
    pw_hist_summary(&hist, &s);
    assert(s.p95_us == 0 && s.min_us == 0 && s.max_us == 0);

    /* Sparse output in bounded pieces: every non-empty bucket once, in order. */
    pw_hist_reset(&hist);
    for (unsigned i = 0; i <= PW_HIST_BUCKETS; i += 7) pw_hist_add(&hist, (uint64_t)i * PW_HIST_BUCKET_US + 5);
    next = 0;
    total = 0;
    while (next <= PW_HIST_BUCKETS) {
        n = pw_hist_sparse(&hist, &next, out, 40);
        assert(n > 0 && strlen(out) < 40);
        total += n;
    }
    assert(total == PW_HIST_BUCKETS / 7 + 1);
    next = 0;
    n = pw_hist_sparse(&hist, &next, out, sizeof(out));
    assert(!strncmp(out, "0:1,7:1,14:1", 12));
    /* Too small for one item: nothing written, no progress past it. */
    next = 0;
    assert(pw_hist_sparse(&hist, &next, out, 3) == 0 && next == 0 && out[0] == 0);

    printf("test_pw_hist: all passed\n");
    return 0;
}
