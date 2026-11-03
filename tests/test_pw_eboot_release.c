/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_eboot_release.h"
#include <assert.h>
#include <string.h>

/* A fake module 0: the record handed out before and after the release, the
 * unmaps seen, and which unmap fails. */
typedef struct Fake {
    uint8_t before[PW_PRX_MODULE_INFO_BYTES], after[PW_PRX_MODULE_INFO_BYTES];
    int before_rc, after_rc, calls, fail_at, fail_rc, clear_size;
    unsigned unmaps;
    PwEbootRange seen[8];
} Fake;

static void record(uint8_t *info, const uint64_t (*segments)[3], uint32_t count)
{
    uint64_t size = PW_PRX_MODULE_INFO_BYTES;
    memset(info, 0, PW_PRX_MODULE_INFO_BYTES);
    memcpy(info, &size, sizeof(size));
    strcpy((char *)info + 8, "eboot.bin");
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t bytes = (uint32_t)segments[i][1], prot = (uint32_t)segments[i][2];
        memcpy(info + 0x108 + i * 16, &segments[i][0], 8);
        memcpy(info + 0x108 + i * 16 + 8, &bytes, 4);
        memcpy(info + 0x108 + i * 16 + 12, &prot, 4);
    }
    memcpy(info + 0x148, &count, sizeof(count));
}

static int fake_info(void *context, void *info)
{
    Fake *fake = context;
    uint64_t size;
    int rc = fake->calls++ ? fake->after_rc : fake->before_rc;
    memcpy(&size, info, sizeof(size));
    assert(size == PW_PRX_MODULE_INFO_BYTES);
    if (rc) return rc;
    memcpy(info, fake->calls == 1 ? fake->before : fake->after, PW_PRX_MODULE_INFO_BYTES);
    if (fake->clear_size) memset(info, 0, 8);
    return 0;
}

static int fake_unmap(void *context, void *address, size_t bytes)
{
    Fake *fake = context;
    assert(fake->unmaps < 8);
    fake->seen[fake->unmaps].base = (uintptr_t)address;
    fake->seen[fake->unmaps].end = (uintptr_t)address + bytes;
    return ++fake->unmaps == (unsigned)fake->fail_at ? fake->fail_rc : 0;
}

/* Today's eboot (build/title/eboot.elf, loaded at 0x400000): R E, R from
 * mid-page, RELRO, RW with 1.9 MiB of .bss; one range 0x400000-0x6cc000. */
static const uint64_t title[4][3] = {
    { 0x400000, 0x8ca90, 5 }, { 0x48ca90, 0x43df8, 1 }, { 0x4d4000, 0x81f8, 3 }, { 0x4e0000, 0x1e96d8, 3 },
};
/* A small stub: code, then data after a hole. */
static const uint64_t stub[2][3] = { { 0x400000, 0x7a10, 5 }, { 0x40c000, 0x1200, 3 } };

static void setup(Fake *fake, const uint64_t (*segments)[3], uint32_t count)
{
    memset(fake, 0, sizeof(*fake));
    record(fake->before, segments, count);
    record(fake->after, segments, count);
}

int main(void)
{
    PwEbootReleaseOps ops = { fake_info, fake_unmap, NULL };
    PwEbootRelease r;
    Fake fake;
    ops.context = &fake;

    /* Today's eboot: four segments, one merged range; the info still lists
     * them afterwards. */
    setup(&fake, title, 4);
    assert(pw_eboot_release(&ops, &r) == PW_EBOOT_OK && r.status == PW_EBOOT_OK);
    assert(r.segment_count == 4 && r.range_count == 1 && r.unmapped == 1 && fake.unmaps == 1);
    assert(fake.seen[0].base == 0x400000 && fake.seen[0].end == 0x6cc000);
    assert(r.low_base == 0x400000 && r.low_end == 0x6cc000);
    assert(r.after == PW_EBOOT_AFTER_LISTED && r.after_count == 4);
    assert(!strcmp(pw_eboot_release_after_name(r.after), "listed"));

    /* The stub: two ranges with a hole; the console cleared the size word
     * (FW 12.02 does) and the info lists nothing afterwards. */
    setup(&fake, stub, 2);
    record(fake.after, stub, 0);
    fake.clear_size = 1;
    assert(pw_eboot_release(&ops, &r) == PW_EBOOT_OK);
    assert(r.range_count == 2 && fake.unmaps == 2);
    assert(fake.seen[0].base == 0x400000 && fake.seen[0].end == 0x408000);
    assert(fake.seen[1].base == 0x40c000 && fake.seen[1].end == 0x410000);
    assert(r.low_end == 0x410000 && r.after == PW_EBOOT_AFTER_GONE);

    /* The second unmap fails: its rc is kept and nothing after it is unmapped. */
    setup(&fake, stub, 2);
    fake.fail_at = 1;
    fake.fail_rc = (int)0x80020016;
    assert(pw_eboot_release(&ops, &r) == PW_EBOOT_ERR_UNMAP);
    assert(r.unmap_rc == (int)0x80020016 && r.unmapped == 0 && fake.unmaps == 1);
    setup(&fake, stub, 2);
    fake.fail_at = 2;
    fake.fail_rc = 22;
    assert(pw_eboot_release(&ops, &r) == PW_EBOOT_ERR_UNMAP && r.unmapped == 1 && fake.unmaps == 2);
    assert(r.after == PW_EBOOT_AFTER_LISTED);

    /* Module info unreadable before: nothing is unmapped. After: reported. */
    setup(&fake, stub, 2);
    fake.before_rc = (int)0x80020002;
    assert(pw_eboot_release(&ops, &r) == PW_EBOOT_ERR_MODULE_INFO && r.info_rc == (int)0x80020002);
    assert(fake.unmaps == 0);
    setup(&fake, stub, 2);
    fake.after_rc = -1;
    assert(pw_eboot_release(&ops, &r) == PW_EBOOT_OK && r.after == PW_EBOOT_AFTER_ERR && r.after_rc == -1);
    assert(!strcmp(pw_eboot_release_after_name(r.after), "err"));

    /* Malformed records: count 0, count 5, address 0, size 0. */
    setup(&fake, stub, 0);
    assert(pw_eboot_release(&ops, &r) == PW_EBOOT_ERR_MODULE_INFO && fake.unmaps == 0);
    setup(&fake, title, 4);
    { uint32_t five = 5; memcpy(fake.before + 0x148, &five, 4); }
    assert(pw_eboot_release(&ops, &r) == PW_EBOOT_ERR_MODULE_INFO && fake.unmaps == 0);
    { static const uint64_t zero_address[1][3] = { { 0, 0x4000, 5 } };
      setup(&fake, zero_address, 1);
      assert(pw_eboot_release(&ops, &r) == PW_EBOOT_ERR_MODULE_INFO && fake.unmaps == 0); }
    { static const uint64_t zero_size[1][3] = { { 0x400000, 0, 5 } };
      setup(&fake, zero_size, 1);
      assert(pw_eboot_release(&ops, &r) == PW_EBOOT_ERR_MODULE_INFO && fake.unmaps == 0); }

    /* A segment at or reaching past 4 GiB is refused before any unmap. */
    { static const uint64_t high[2][3] = { { 0x400000, 0x4000, 5 }, { 0x800000000, 0x4000, 3 } };
      setup(&fake, high, 2);
      assert(pw_eboot_release(&ops, &r) == PW_EBOOT_ERR_RANGE && fake.unmaps == 0); }
    { static const uint64_t across[1][3] = { { 0xffffc000, 0x8000, 5 } };
      setup(&fake, across, 1);
      assert(pw_eboot_release(&ops, &r) == PW_EBOOT_ERR_RANGE && fake.unmaps == 0); }

    /* The stub laid out by tools/pw_stub_orbis.ld: code at 0x400000, data at 0x7fff0000, which is
     * kept (libkernel reads module 0's data after the release). */
    {
        PwPrxSegment far[2] = { { (const void *)0x400000, 0x1788, 5 }, { (const void *)0x7fff0000, 0x580, 3 } };
        PwPrxSegment across[1] = { { (const void *)0x7ffe8000, 0x10000, 3 } };
        PwPrxSegment above[1] = { { (const void *)0x7fff0000, 0x580, 3 } };
        assert(pw_eboot_release_plan(far, 2, &r) == PW_EBOOT_OK);
        assert(r.range_count == 1 && r.ranges[0].base == 0x400000 && r.ranges[0].end == 0x404000);
        assert(r.kept == 1 && r.low_base == 0x400000 && r.low_end == 0x404000);
        assert(pw_eboot_release_plan(across, 1, &r) == PW_EBOOT_ERR_RANGE);
        assert(pw_eboot_release_plan(above, 1, &r) == PW_EBOOT_ERR_RANGE);
        setup(&fake, (const uint64_t (*)[3])(const uint64_t[2][3]){ { 0x400000, 0x1788, 5 }, { 0x7fff0000, 0x580, 3 } }, 2);
        assert(pw_eboot_release(&ops, &r) == PW_EBOOT_OK && r.kept == 1 && fake.unmaps == 1);
        assert(fake.seen[0].base == 0x400000 && fake.seen[0].end == 0x404000 && r.low_end == 0x404000);
    }
    /* The planner alone: unsorted, overlapping, contained ranges merge. */
    {
        PwPrxSegment segments[3] = {
            { (const void *)0x420000, 0x100, 3 }, { (const void *)0x400000, 0x4100, 5 }, { (const void *)0x404000, 0x10, 1 },
        };
        assert(pw_eboot_release_plan(segments, 3, &r) == PW_EBOOT_OK);
        assert(r.range_count == 2 && r.ranges[0].base == 0x400000 && r.ranges[0].end == 0x408000);
        assert(r.ranges[1].base == 0x420000 && r.ranges[1].end == 0x424000);
        assert(pw_eboot_release_plan(segments, 0, &r) == PW_EBOOT_ERR_ARGUMENT);
        assert(pw_eboot_release_plan(segments, 5, &r) == PW_EBOOT_ERR_ARGUMENT);
    }
    /* The argument vector the stub hands over: copied, strings and all. */
    {
        char a0[] = "path=C:\\fixed32.exe", a1[] = "x", *argv[] = { a0, a1, NULL }, store[64], *out[4];
        assert(pw_eboot_copy_argv(2, argv, store, sizeof(store), out, 4) == 2);
        assert(!strcmp(out[0], "path=C:\\fixed32.exe") && !strcmp(out[1], "x") && !out[2]);
        assert(out[0] != a0 && out[0] >= store && out[1] < store + sizeof(store));
        a0[0] = 'X';
        assert(out[0][0] == 'p');
        assert(pw_eboot_copy_argv(2, argv, store, sizeof(store), out, 2) == 1 && !out[1]);
        assert(pw_eboot_copy_argv(2, argv, store, 5, out, 4) == 0 && !out[0]);
        assert(pw_eboot_copy_argv(0, NULL, store, sizeof(store), out, 4) == 0 && !out[0]);
        assert(pw_eboot_copy_argv(1, argv, store, sizeof(store), out, 0) == 0);
    }
    /* Missing ops. */
    { PwEbootReleaseOps none = { NULL, fake_unmap, &fake };
      assert(pw_eboot_release(&none, &r) == PW_EBOOT_ERR_ARGUMENT);
      assert(pw_eboot_release(&ops, NULL) == PW_EBOOT_ERR_ARGUMENT); }
    return 0;
}
