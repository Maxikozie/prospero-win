/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* A 64-bit GDI program for the PS4 port's milestones d2 and d3: win32u
 * inside Wine (offscreen drawing read back with GetPixel, the screen the
 * user driver reports, a window whose frames reach the title's sink) and
 * FreeType with Wine's fonts (the families GDI enumerates, Tahoma among
 * them, ink of "Wg" drawn in Tahoma). One line on stdout:
 *
 *   GDI64 screen=800x600 pixel=ff8000 fonts=N tahoma=1 text_px=N window=ok dir=N:E open=0|E add=N:E fonts_after=N
 *
 * fonts/tahoma/text_px are what Wine's own font loading found. dir, open,
 * add and fonts_after diagnose it afterwards: FindFirstFile over
 * C:\windows\Fonts\*.ttf (the NtQueryDirectoryFile listing win32u's
 * load_directory_fonts uses; count and last error), CreateFile of
 * tahoma.ttf by name, AddFontResourceEx of it (FreeType given the file),
 * and the family count after that.
 *
 * then exit 0 (1 when the line could not be completed). Built with
 * x86_64-w64-mingw32-gcc -lgdi32 -luser32; tests/test_wine_gdi64.py runs it
 * under the host Wine. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int font_families, tahoma;

static int CALLBACK count_family(const LOGFONTW *font, const TEXTMETRICW *metrics, DWORD type, LPARAM param)
{
    (void)metrics; (void)type; (void)param;
    font_families++;
    if (!lstrcmpiW(font->lfFaceName, L"Tahoma")) tahoma = 1;
    return 1;
}

/* A 32-bit top-down DIB section selected into a new memory DC. */
static HDC dib_dc(int width, int height, DWORD **bits, HBITMAP *bitmap)
{
    BITMAPINFO info;
    HDC dc = CreateCompatibleDC(NULL);

    memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    if (!dc || !(*bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, (void **)bits, NULL, 0))) return NULL;
    SelectObject(dc, *bitmap);
    return dc;
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        HBRUSH green = CreateSolidBrush(RGB(0, 255, 0));
        RECT inner = { 100, 75, 300, 225 };
        FillRect(dc, &inner, green);
        DeleteObject(green);
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

/* An orange 400x300 pop-up with a green rectangle, shown for two seconds;
 * "ok", or the failing call and its error ("class:1407"). */
static const char *show_window(void)
{
    static char failed[48];
    WNDCLASSW cls;
    HWND window;
    DWORD end;
    MSG msg;

    memset(&cls, 0, sizeof(cls));
    cls.lpfnWndProc = window_proc;
    cls.hInstance = GetModuleHandleW(NULL);
    cls.hbrBackground = CreateSolidBrush(RGB(255, 128, 0));
    cls.lpszClassName = L"gdi64";
    if (!RegisterClassW(&cls)) {
        snprintf(failed, sizeof(failed), "class:%lu", GetLastError());
        return failed;
    }
    if (!(window = CreateWindowExW(0, L"gdi64", L"gdi64", WS_POPUP | WS_VISIBLE, 200, 150, 400, 300,
                                   NULL, NULL, cls.hInstance, NULL))) {
        snprintf(failed, sizeof(failed), "create:%lu", GetLastError());
        return failed;
    }
    UpdateWindow(window);
    end = GetTickCount() + 2000;
    while ((int)(end - GetTickCount()) > 0) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(16);
    }
    DestroyWindow(window);
    return "ok";
}

int main(void)
{
    HBITMAP bitmap, text_bitmap;
    DWORD *bits, *text_bits;
    HDC dc, text_dc, screen;
    COLORREF pixel = CLR_INVALID;
    LOGFONTW all;
    int text_px = 0, dir_count = 0, added, fonts_found, tahoma_found;
    DWORD dir_error, open_error = 0, add_error;
    const char *window;

    /* Offscreen: an orange fill read back. */
    if ((dc = dib_dc(64, 64, &bits, &bitmap))) {
        RECT all_of_it = { 0, 0, 64, 64 };
        HBRUSH orange = CreateSolidBrush(RGB(255, 128, 0));
        FillRect(dc, &all_of_it, orange);
        DeleteObject(orange);
        pixel = GetPixel(dc, 10, 10);
    }

    /* The families GDI enumerates. */
    screen = GetDC(NULL);
    memset(&all, 0, sizeof(all));
    all.lfCharSet = DEFAULT_CHARSET;
    if (screen) EnumFontFamiliesExW(screen, &all, count_family, 0, 0);
    fonts_found = font_families;
    tahoma_found = tahoma;

    /* "Wg" in 32-pixel Tahoma, black on white: the pixels that are not white. */
    if ((text_dc = dib_dc(128, 64, &text_bits, &text_bitmap))) {
        RECT all_of_it = { 0, 0, 128, 64 };
        HFONT font = CreateFontW(-32, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, DEFAULT_PITCH, L"Tahoma");
        FillRect(text_dc, &all_of_it, GetStockObject(WHITE_BRUSH));
        if (font) SelectObject(text_dc, font);
        SetTextColor(text_dc, RGB(0, 0, 0));
        SetBkMode(text_dc, TRANSPARENT);
        TextOutW(text_dc, 8, 8, L"Wg", 2);
        GdiFlush();
        for (int i = 0; i < 128 * 64; i++)
            if ((text_bits[i] & 0xffffff) != 0xffffff) text_px++;
        if (font) DeleteObject(font);
    }

    window = show_window();

    /* Diagnostics, after the measurements above. */
    {
        WIN32_FIND_DATAW find;
        HANDLE handle = FindFirstFileW(L"C:\\windows\\Fonts\\*.ttf", &find);
        DWORD err;
        if (handle == INVALID_HANDLE_VALUE) err = GetLastError();
        else {
            do dir_count++; while (FindNextFileW(handle, &find));
            err = GetLastError();
            FindClose(handle);
        }
        dir_error = err;
        handle = CreateFileW(L"C:\\windows\\Fonts\\tahoma.ttf", GENERIC_READ, FILE_SHARE_READ, NULL,
                             OPEN_EXISTING, 0, NULL);
        if (handle == INVALID_HANDLE_VALUE) open_error = GetLastError();
        else CloseHandle(handle);
        added = AddFontResourceExW(L"C:\\windows\\Fonts\\tahoma.ttf", FR_PRIVATE, NULL);
        add_error = added ? 0 : GetLastError();
        font_families = 0;
        if (screen) EnumFontFamiliesExW(screen, &all, count_family, 0, 0);
    }
    printf("GDI64 screen=%dx%d pixel=%02x%02x%02x fonts=%d tahoma=%d text_px=%d window=%s dir=%d:%lu open=%lu "
           "add=%d:%lu fonts_after=%d\n",
           GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
           pixel == CLR_INVALID ? 0 : GetRValue(pixel), pixel == CLR_INVALID ? 0 : GetGValue(pixel),
           pixel == CLR_INVALID ? 0 : GetBValue(pixel), fonts_found, tahoma_found, text_px, window,
           dir_count, dir_error, open_error, added, add_error, font_families);
    fflush(stdout);
    return pixel == CLR_INVALID ? 1 : 0;
}
