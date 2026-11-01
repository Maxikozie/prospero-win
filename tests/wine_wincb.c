/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Window callbacks on the main thread and on new threads, for the PS4 port's
 * phase p5 ("p5w"). On the PS4, winmm's devices thread (a thread created
 * after the program's main thread) had CreateWindowW(L"Message", ...,
 * HWND_MESSAGE) aborted by WM_NCCREATE on both CPU backends, while the main
 * thread created its windows. This program makes the same kind of window in
 * every combination that could tell the suspects apart, and prints one line
 * per case:
 *
 *   WINCB case=NN thread=main|new|new_ga|new_desk parent=msg|top
 *         class=builtin|own|owndef cs=W|A hwnd=ok|0 gle=0x.. us=N
 *         msgs=<ids seen by the window procedure, hex>|- nc_lp=<WM_NCCREATE's
 *         lparam>|- nc_cs=ok|null|bad:<field>|- def_ret=N|- second=ok|0|-
 *
 *   thread  main: this program's main thread; new: a fresh thread whose first
 *           USER call is the CreateWindow (winmm's case); new_ga: a fresh
 *           thread that first calls GetAncestor(GA_ROOT) (the path of the
 *           console's worker desktop cache); new_desk: a fresh thread that
 *           first calls GetDesktopWindow() and PeekMessage (full USER setup);
 *   parent  msg: HWND_MESSAGE; top: none (a hidden top-level window);
 *   class   builtin: the "Message" class, as winmm makes it (case 2 is
 *           winmm's own call); own: a procedure that records each message,
 *           checks WM_NCCREATE's CREATESTRUCT and answers TRUE; owndef: the
 *           same, answering with DefWindowProc's result (def_ret);
 *   gle     GetLastError after the call; it is set to 0xc0de before it, so an
 *           unchanged value means nothing set an error;
 *   us      the CreateWindow call's duration.
 *
 * Probes, one line each:
 *   WINCB probe=direct thread=main|new proc=<the Message class's procedure>
 *         module=<its module> ret=N   (CallWindowProcW(WM_NCCREATE): a call
 *         made in user mode, with no callback from the kernel; 1 expected);
 *   WINCB probe=cbt thread=main|new calls=N same_tid=0|1|- hwnd=ok|0
 *         (a WH_CBT hook on the thread itself, then an own-class window);
 *   WINCB probe=cross ret=N res=0x.. gle=0x..   (a new thread sends WM_APP to
 *         a window of the main thread, which runs its procedure: 0x1235);
 *   WINCB hold_begin / WINCB hold n=8 alive=N windows=N / WINCB hold_end
 *         windows=N   (8 threads alive at once, each with one window, for the
 *         console title to sample its memory between the markers).
 *
 * The last line:
 *   WINCB done bits=32|64 cases=N ok=N main_ok=N/N new_ok=N/N direct_main=0|1
 *         direct_new=0|1 cbt_main=N cbt_new=N cross=ok|fail hold_windows=N
 *         first_fail=NN|-
 *
 * Exit 0 when every case and probe ran (whatever their results), 2 when the
 * program could not set itself up. Built with i686-w64-mingw32-gcc and
 * x86_64-w64-mingw32-gcc; tests/test_wine_wincb.py runs it under the host
 * Wine. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define MARK 0x5ec00000u
#define WAIT_MS 10000
#define HOLD_THREADS 8

enum thread_kind { T_MAIN, T_NEW, T_NEW_GA, T_NEW_DESK };
enum parent_kind { P_MSG, P_TOP };
enum class_kind { C_BUILTIN, C_OWN, C_OWNDEF };

struct wincase
{
    int id;
    enum thread_kind thread;
    enum parent_kind parent;
    enum class_kind cls;
    BOOL ansi;
    BOOL twice;
};

static const struct wincase cases[] =
{
    /* the core matrix, Unicode */
    {  1, T_MAIN, P_MSG, C_BUILTIN, FALSE, FALSE },
    {  2, T_NEW,  P_MSG, C_BUILTIN, FALSE, FALSE },
    {  3, T_MAIN, P_TOP, C_BUILTIN, FALSE, FALSE },
    {  4, T_NEW,  P_TOP, C_BUILTIN, FALSE, FALSE },
    {  5, T_MAIN, P_MSG, C_OWN,     FALSE, FALSE },
    {  6, T_NEW,  P_MSG, C_OWN,     FALSE, FALSE },
    {  7, T_MAIN, P_TOP, C_OWN,     FALSE, FALSE },
    {  8, T_NEW,  P_TOP, C_OWN,     FALSE, FALSE },
    /* the same in ANSI */
    {  9, T_MAIN, P_MSG, C_BUILTIN, TRUE,  FALSE },
    { 10, T_NEW,  P_MSG, C_BUILTIN, TRUE,  FALSE },
    { 11, T_MAIN, P_TOP, C_BUILTIN, TRUE,  FALSE },
    { 12, T_NEW,  P_TOP, C_BUILTIN, TRUE,  FALSE },
    { 13, T_MAIN, P_MSG, C_OWN,     TRUE,  FALSE },
    { 14, T_NEW,  P_MSG, C_OWN,     TRUE,  FALSE },
    { 15, T_MAIN, P_TOP, C_OWN,     TRUE,  FALSE },
    { 16, T_NEW,  P_TOP, C_OWN,     TRUE,  FALSE },
    /* DefWindowProc's answer to the CREATESTRUCT it is given */
    { 17, T_MAIN, P_MSG, C_OWNDEF,  FALSE, FALSE },
    { 18, T_NEW,  P_MSG, C_OWNDEF,  FALSE, FALSE },
    /* USER calls before the window */
    { 19, T_NEW_GA,   P_MSG, C_BUILTIN, FALSE, FALSE },
    { 20, T_NEW_GA,   P_MSG, C_OWN,     FALSE, FALSE },
    { 21, T_NEW_DESK, P_MSG, C_BUILTIN, FALSE, FALSE },
    { 22, T_NEW_DESK, P_MSG, C_OWN,     FALSE, FALSE },
    /* two windows in a row on one fresh thread */
    { 23, T_NEW, P_MSG, C_OWN, FALSE, TRUE },
};

static const char *thread_names[] = { "main", "new", "new_ga", "new_desk" };
static const char *parent_names[] = { "msg", "top" };
static const char *class_names[] = { "builtin", "own", "owndef" };

/* What the window procedures saw for the case that is running (one case runs
 * at a time). */
struct record
{
    int nmsgs;
    UINT msgs[8];
    BOOL nc_seen;
    LPARAM nc_lp;
    char nc_cs[40];
    BOOL def_called;
    LRESULT def_ret;
};

struct result
{
    BOOL ok;
    DWORD gle;
    LONGLONG us;
    struct record rec;
    int second;   /* -1 not tried, 0 failed, 1 made */
    BOOL timeout;
};

static struct record rec;
static int running_case;
static enum class_kind running_cls;
static HINSTANCE self;
static HWND target;   /* the main thread's window: the cross probe's target, GetAncestor's argument */
static LARGE_INTEGER qpf;

/* ---- window procedures ---------------------------------------------------- */

static void check_cs(LPARAM lp, BOOL ansi)
{
    void *want_params = (void *)(ULONG_PTR)(MARK | (UINT)running_case);

    if (!lp)
    {
        strcpy(rec.nc_cs, "null");
        return;
    }
    if (ansi)
    {
        const CREATESTRUCTA *cs = (const CREATESTRUCTA *)lp;
        if (cs->lpCreateParams != want_params) strcpy(rec.nc_cs, "bad:params");
        else if (cs->hInstance != self) strcpy(rec.nc_cs, "bad:instance");
        else if (!HIWORD(cs->lpszClass) || strcmp(cs->lpszClass, "WincbOwnA")) strcpy(rec.nc_cs, "bad:class");
        else if (cs->cx || cs->cy) strcpy(rec.nc_cs, "bad:size");
        else strcpy(rec.nc_cs, "ok");
    }
    else
    {
        const CREATESTRUCTW *cs = (const CREATESTRUCTW *)lp;
        const WCHAR *want_class = running_cls == C_OWNDEF ? L"WincbDefW" : L"WincbOwnW";
        if (cs->lpCreateParams != want_params) strcpy(rec.nc_cs, "bad:params");
        else if (cs->hInstance != self) strcpy(rec.nc_cs, "bad:instance");
        else if (!HIWORD(cs->lpszClass) || wcscmp(cs->lpszClass, want_class)) strcpy(rec.nc_cs, "bad:class");
        else if (cs->cx || cs->cy) strcpy(rec.nc_cs, "bad:size");
        else strcpy(rec.nc_cs, "ok");
    }
}

static void note(UINT msg, LPARAM lp, BOOL ansi)
{
    if (rec.nmsgs < (int)(sizeof(rec.msgs) / sizeof(rec.msgs[0]))) rec.msgs[rec.nmsgs] = msg;
    rec.nmsgs++;
    if (msg == WM_NCCREATE && !rec.nc_seen)
    {
        rec.nc_seen = TRUE;
        rec.nc_lp = lp;
        check_cs(lp, ansi);
    }
}

static LRESULT CALLBACK own_proc_w(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    note(msg, lp, FALSE);
    if (msg == WM_NCCREATE) return TRUE;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK own_proc_a(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    note(msg, lp, TRUE);
    if (msg == WM_NCCREATE) return TRUE;
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK def_proc_w(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT ret;

    note(msg, lp, FALSE);
    ret = DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_NCCREATE)
    {
        rec.def_called = TRUE;
        rec.def_ret = ret;
    }
    return ret;
}

/* The main thread's window and the hold threads' windows: no recording. */
static LRESULT CALLBACK target_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_APP) return (LRESULT)(wp + 1);
    if (msg == WM_NCCREATE) return TRUE;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static BOOL register_classes(void)
{
    WNDCLASSEXW w;
    WNDCLASSEXA a;

    memset(&w, 0, sizeof(w));
    w.cbSize = sizeof(w);
    w.hInstance = self;
    w.lpfnWndProc = own_proc_w;
    w.lpszClassName = L"WincbOwnW";
    if (!RegisterClassExW(&w)) return FALSE;
    w.lpfnWndProc = def_proc_w;
    w.lpszClassName = L"WincbDefW";
    if (!RegisterClassExW(&w)) return FALSE;
    w.lpfnWndProc = target_proc;
    w.lpszClassName = L"WincbTarget";
    if (!RegisterClassExW(&w)) return FALSE;
    memset(&a, 0, sizeof(a));
    a.cbSize = sizeof(a);
    a.hInstance = self;
    a.lpfnWndProc = own_proc_a;
    a.lpszClassName = "WincbOwnA";
    return RegisterClassExA(&a) != 0;
}

/* ---- waiting ---------------------------------------------------------------- */

/* Waits for HANDLE (NULL: only the time) while the calling thread keeps
 * handling its messages, so a send from another thread to its windows
 * completes. TRUE when the handle was signalled. */
static BOOL pump_wait(HANDLE handle, DWORD ms)
{
    DWORD end = GetTickCount() + ms;

    for (;;)
    {
        DWORD now = GetTickCount(), left = (LONG)(end - now) > 0 ? end - now : 0, w;
        MSG m;

        w = MsgWaitForMultipleObjects(handle ? 1 : 0, handle ? &handle : NULL, FALSE, left, QS_ALLINPUT);
        if (handle && w == WAIT_OBJECT_0) return TRUE;
        if (w == WAIT_TIMEOUT || w == WAIT_FAILED) return FALSE;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (!left) return FALSE;
    }
}

static BOOL run_on_new_thread(LPTHREAD_START_ROUTINE fn, void *arg)
{
    HANDLE thread = CreateThread(NULL, 0, fn, arg, 0, NULL);
    BOOL done;

    if (!thread) return FALSE;
    done = pump_wait(thread, WAIT_MS);
    if (done) CloseHandle(thread);   /* a hung thread keeps its handle: it may still write */
    return done;
}

/* ---- cases ------------------------------------------------------------------ */

static HWND create(const struct wincase *c)
{
    HWND parent = c->parent == P_MSG ? HWND_MESSAGE : NULL;
    void *params = c->cls == C_BUILTIN ? NULL : (void *)(ULONG_PTR)(MARK | (UINT)c->id);
    HINSTANCE instance = c->cls == C_BUILTIN ? NULL : self;

    if (c->ansi)
        return CreateWindowExA(0, c->cls == C_BUILTIN ? "Message" : "WincbOwnA", NULL, 0, 0, 0, 0, 0,
                               parent, NULL, instance, params);
    return CreateWindowExW(0, c->cls == C_BUILTIN ? L"Message" : c->cls == C_OWN ? L"WincbOwnW" : L"WincbDefW",
                           NULL, 0, 0, 0, 0, 0, parent, NULL, instance, params);
}

struct case_arg
{
    const struct wincase *c;
    struct result *r;
};

static DWORD WINAPI case_thread(void *param)
{
    struct case_arg *arg = param;
    const struct wincase *c = arg->c;
    struct result *r = arg->r;
    LARGE_INTEGER t0, t1;
    HWND hwnd;

    if (c->thread == T_NEW_GA) GetAncestor(target, GA_ROOT);
    if (c->thread == T_NEW_DESK)
    {
        MSG m;
        GetDesktopWindow();
        PeekMessageW(&m, NULL, 0, 0, PM_NOREMOVE);
    }
    memset(&rec, 0, sizeof(rec));
    SetLastError(0xc0de);
    QueryPerformanceCounter(&t0);
    hwnd = create(c);
    QueryPerformanceCounter(&t1);
    r->gle = GetLastError();
    r->us = (t1.QuadPart - t0.QuadPart) * 1000000 / qpf.QuadPart;
    r->ok = hwnd != NULL;
    r->rec = rec;
    r->second = -1;
    if (c->twice)
    {
        HWND again;
        memset(&rec, 0, sizeof(rec));
        again = create(c);
        r->second = again != NULL;
        if (again) DestroyWindow(again);
    }
    if (hwnd) DestroyWindow(hwnd);
    return 0;
}

static void print_case(const struct wincase *c, const struct result *r)
{
    char msgs[64] = "-", nc_lp[24] = "-", def_ret[24] = "-";
    const char *second = r->second < 0 ? "-" : r->second ? "ok" : "0";
    int i, n = r->rec.nmsgs < 8 ? r->rec.nmsgs : 8;

    if (r->timeout)
    {
        printf("WINCB case=%02d thread=%s parent=%s class=%s cs=%s hwnd=timeout\n", c->id,
               thread_names[c->thread], parent_names[c->parent], class_names[c->cls], c->ansi ? "A" : "W");
        fflush(stdout);
        return;
    }
    if (n)
    {
        int len = 0;
        for (i = 0; i < n; i++)
            len += snprintf(msgs + len, sizeof(msgs) - len, "%s%x", i ? "," : "", r->rec.msgs[i]);
        if (r->rec.nmsgs > n) snprintf(msgs + len, sizeof(msgs) - len, ",+%d", r->rec.nmsgs - n);
    }
    if (r->rec.nc_seen) snprintf(nc_lp, sizeof(nc_lp), "%p", (void *)r->rec.nc_lp);
    if (r->rec.def_called) snprintf(def_ret, sizeof(def_ret), "%ld", (long)r->rec.def_ret);
    printf("WINCB case=%02d thread=%s parent=%s class=%s cs=%s hwnd=%s gle=0x%lx us=%ld msgs=%s nc_lp=%s "
           "nc_cs=%s def_ret=%s second=%s\n", c->id, thread_names[c->thread], parent_names[c->parent],
           class_names[c->cls], c->ansi ? "A" : "W", r->ok ? "ok" : "0", r->gle, (long)r->us, msgs, nc_lp,
           r->rec.nc_seen ? r->rec.nc_cs : "-", def_ret, second);
    fflush(stdout);
}

/* ---- probes ----------------------------------------------------------------- */

struct direct_result
{
    BOOL ran;
    void *proc;
    char module[64];
    LRESULT ret;
};

static DWORD WINAPI direct_probe(void *param)
{
    struct direct_result *d = param;
    WNDCLASSEXW wc;
    CREATESTRUCTW cs;
    HMODULE module;
    WCHAR path[MAX_PATH];

    d->ran = TRUE;
    strcpy(d->module, "-");
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    if (!GetClassInfoExW(NULL, L"Message", &wc))
    {
        snprintf(d->module, sizeof(d->module), "noclass:%lu", GetLastError());
        return 0;
    }
    d->proc = (void *)wc.lpfnWndProc;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (const WCHAR *)wc.lpfnWndProc, &module) &&
        GetModuleFileNameW(module, path, MAX_PATH))
    {
        const WCHAR *base = wcsrchr(path, '\\');
        if (!WideCharToMultiByte(CP_ACP, 0, base ? base + 1 : path, -1, d->module, sizeof(d->module), NULL, NULL))
            strcpy(d->module, "name");
    }
    else strcpy(d->module, "none");
    memset(&cs, 0, sizeof(cs));
    cs.lpszClass = L"Message";
    d->ret = CallWindowProcW(wc.lpfnWndProc, NULL, WM_NCCREATE, 0, (LPARAM)&cs);
    return 0;
}

static void print_direct(const char *thread, const struct direct_result *d, BOOL done)
{
    if (!done || !d->ran) printf("WINCB probe=direct thread=%s ret=timeout\n", thread);
    else printf("WINCB probe=direct thread=%s proc=%p module=%s ret=%ld\n", thread, d->proc, d->module, (long)d->ret);
    fflush(stdout);
}

struct cbt_result
{
    BOOL ran;
    LONG calls;
    int same_tid;   /* -1: no HCBT_CREATEWND seen */
    BOOL made;
    DWORD hook_gle;
};

static struct cbt_result *cbt_now;

static LRESULT CALLBACK cbt_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HCBT_CREATEWND && cbt_now)
    {
        cbt_now->calls++;
        cbt_now->same_tid = GetWindowThreadProcessId((HWND)wp, NULL) == GetCurrentThreadId();
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static DWORD WINAPI cbt_probe(void *param)
{
    struct cbt_result *r = param;
    HHOOK hook;
    HWND hwnd;

    r->ran = TRUE;
    r->same_tid = -1;
    cbt_now = r;
    if (!(hook = SetWindowsHookExW(WH_CBT, cbt_proc, NULL, GetCurrentThreadId())))
    {
        r->hook_gle = GetLastError();
        cbt_now = NULL;
        return 0;
    }
    running_case = 0;
    memset(&rec, 0, sizeof(rec));
    hwnd = CreateWindowExW(0, L"WincbTarget", NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, self, NULL);
    r->made = hwnd != NULL;
    UnhookWindowsHookEx(hook);
    cbt_now = NULL;
    if (hwnd) DestroyWindow(hwnd);
    return 0;
}

static void print_cbt(const char *thread, const struct cbt_result *r, BOOL done)
{
    if (!done || !r->ran) printf("WINCB probe=cbt thread=%s calls=timeout\n", thread);
    else if (r->hook_gle) printf("WINCB probe=cbt thread=%s calls=0 hook=err:%lu\n", thread, r->hook_gle);
    else printf("WINCB probe=cbt thread=%s calls=%ld same_tid=%s hwnd=%s\n", thread, r->calls,
                r->same_tid < 0 ? "-" : r->same_tid ? "1" : "0", r->made ? "ok" : "0");
    fflush(stdout);
}

struct cross_result
{
    BOOL ran;
    LRESULT ret;
    DWORD_PTR res;
    DWORD gle;
};

static DWORD WINAPI cross_probe(void *param)
{
    struct cross_result *r = param;

    r->ran = TRUE;
    SetLastError(0xc0de);
    r->ret = SendMessageTimeoutW(target, WM_APP, 0x1234, 0, SMTO_NORMAL, 5000, &r->res);
    r->gle = GetLastError();
    return 0;
}

/* ---- hold: HOLD_THREADS threads alive at once ---------------------------------- */

static volatile LONG hold_ready, hold_windows;
static HANDLE hold_release;

static DWORD WINAPI hold_thread(void *param)
{
    HWND hwnd = CreateWindowExW(0, L"WincbTarget", NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, self, NULL);

    (void)param;
    if (hwnd) InterlockedIncrement(&hold_windows);
    InterlockedIncrement(&hold_ready);
    WaitForSingleObject(hold_release, 30000);
    if (hwnd) DestroyWindow(hwnd);
    return 0;
}

static LONG hold_probe(void)
{
    HANDLE threads[HOLD_THREADS];
    DWORD start;
    int i, n = 0;

    printf("WINCB hold_begin\n");
    fflush(stdout);
    pump_wait(NULL, 1000);
    hold_release = CreateEventW(NULL, TRUE, FALSE, NULL);
    for (i = 0; i < HOLD_THREADS; i++)
        if ((threads[n] = CreateThread(NULL, 0, hold_thread, NULL, 0, NULL))) n++;
    start = GetTickCount();
    while (hold_ready < n && GetTickCount() - start < WAIT_MS) pump_wait(NULL, 20);
    printf("WINCB hold n=%d alive=%ld windows=%ld\n", HOLD_THREADS, (long)hold_ready, (long)hold_windows);
    fflush(stdout);
    pump_wait(NULL, 2000);
    SetEvent(hold_release);
    for (i = 0; i < n; i++)
    {
        if (pump_wait(threads[i], WAIT_MS)) CloseHandle(threads[i]);
    }
    printf("WINCB hold_end windows=%ld\n", (long)hold_windows);
    fflush(stdout);
    pump_wait(NULL, 1000);
    return hold_windows;
}

/* ---- main --------------------------------------------------------------------- */

int main(void)
{
    struct result results[sizeof(cases) / sizeof(cases[0])];
    struct direct_result direct_main, direct_new;
    struct cbt_result cbt_main, cbt_new;
    struct cross_result cross;
    int i, ok = 0, main_ok = 0, main_n = 0, new_ok = 0, new_n = 0, first_fail = 0;
    BOOL done;
    LONG held;
    char first[8] = "-";

    self = GetModuleHandleW(NULL);
    QueryPerformanceFrequency(&qpf);
    printf("WINCB start bits=%d main_tid=0x%lx cases=%d\n", (int)(sizeof(void *) * 8), GetCurrentThreadId(),
           (int)(sizeof(cases) / sizeof(cases[0])));
    fflush(stdout);
    if (!register_classes())
    {
        printf("WINCB done setup=register:%lu\n", GetLastError());
        fflush(stdout);
        return 2;
    }
    if (!(target = CreateWindowExW(0, L"WincbTarget", NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, self, NULL)))
    {
        printf("WINCB done setup=target:%lu\n", GetLastError());
        fflush(stdout);
        return 2;
    }

    memset(results, 0, sizeof(results));
    for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++)
    {
        const struct wincase *c = &cases[i];
        struct case_arg arg = { c, &results[i] };

        running_case = c->id;
        running_cls = c->cls;
        if (c->thread == T_MAIN) case_thread(&arg);
        else results[i].timeout = !run_on_new_thread(case_thread, &arg);
        print_case(c, &results[i]);
        if (results[i].timeout) results[i].ok = FALSE;
        if (results[i].ok && results[i].second != 0) ok++;
        else if (!first_fail) first_fail = c->id;
        if (c->thread == T_MAIN)
        {
            main_n++;
            if (results[i].ok) main_ok++;
        }
        else
        {
            new_n++;
            if (results[i].ok && results[i].second != 0) new_ok++;
        }
    }
    running_case = 0;

    memset(&direct_main, 0, sizeof(direct_main));
    direct_probe(&direct_main);
    print_direct("main", &direct_main, TRUE);
    memset(&direct_new, 0, sizeof(direct_new));
    done = run_on_new_thread(direct_probe, &direct_new);
    print_direct("new", &direct_new, done);

    memset(&cbt_main, 0, sizeof(cbt_main));
    cbt_probe(&cbt_main);
    print_cbt("main", &cbt_main, TRUE);
    memset(&cbt_new, 0, sizeof(cbt_new));
    done = run_on_new_thread(cbt_probe, &cbt_new);
    print_cbt("new", &cbt_new, done);

    memset(&cross, 0, sizeof(cross));
    done = run_on_new_thread(cross_probe, &cross);
    if (!done || !cross.ran) printf("WINCB probe=cross ret=timeout\n");
    else printf("WINCB probe=cross ret=%ld res=0x%lx gle=0x%lx\n", (long)cross.ret, (unsigned long)cross.res,
                cross.gle);
    fflush(stdout);

    held = hold_probe();

    DestroyWindow(target);
    if (first_fail) snprintf(first, sizeof(first), "%02d", first_fail);
    printf("WINCB done bits=%d cases=%d ok=%d main_ok=%d/%d new_ok=%d/%d direct_main=%d direct_new=%d "
           "cbt_main=%ld cbt_new=%ld cross=%s hold_windows=%ld first_fail=%s\n",
           (int)(sizeof(void *) * 8), (int)(sizeof(cases) / sizeof(cases[0])), ok, main_ok, main_n, new_ok, new_n,
           direct_main.ret == 1, direct_new.ran && direct_new.ret == 1, cbt_main.calls, cbt_new.calls,
           cross.ran && cross.ret && cross.res == 0x1235 ? "ok" : "fail", (long)held, first);
    fflush(stdout);
    return 0;
}
