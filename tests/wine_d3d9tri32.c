/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* A 32-bit Direct3D 9 program for the PS4 port's milestone g (g1), run under
 * DXVK 2.6.2's d3d9 through WoW64: what Bloodlines needs below the game. A
 * 1920x1080 popup window and a windowed swapchain of that size (the one
 * VideoOut size known to be accepted), the fixed-function pipeline (no shader
 * compiler), FRAMES frames each cleared to blue with:
 *   - one yellow triangle over the centre from a D3DUSAGE_DYNAMIC vertex
 *     buffer locked with D3DLOCK_DISCARD every frame (host-visible memory
 *     the 32-bit program writes: WoW64's placed maps on the PS4);
 *   - a 64x64 green square centred on 240,135, textured from a MANAGED-pool
 *     texture filled once through LockRect (DXVK uploads it from a mapped
 *     staging buffer).
 * Only 0 and 1 colour components, so neither UNORM nor sRGB can shift them;
 * swapped red and blue fail both. At frame READBACK the back buffer is
 * copied to a system-memory surface (GetRenderTargetData) and the centre
 * (960,540) and 240,135 are read. One line on stdout:
 *
 *   D3D9 bits=32 frames=N swapchain=WxH hr=0x0 at=- px_c=ffff00 px_m=00ff00 texmem_mib=N ms_avg=N.N
 *
 * hr and at name the first failing call; texmem_mib is
 * GetAvailableTextureMem at the start (DXVK's d3d9.maxAvailableMemory, which
 * its vampire.exe profile sets to 1024). Exit 0 when every frame was
 * presented. Built with i686-w64-mingw32-gcc -ld3d9 -luser32
 * (scripts/ps4/prefix.sh in ps4-wine). */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

enum { WIDTH = 1920, HEIGHT = 1080, FRAMES = 600, READBACK = 300, MARK = 64, MARK_X = 240, MARK_Y = 135 };
#define BITS ((unsigned)(sizeof(void *) * 8))

typedef struct ColorVertex { float x, y, z, rhw; DWORD color; } ColorVertex;
#define FVF_COLOR (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)
typedef struct TexVertex { float x, y, z, rhw, u, v; } TexVertex;
#define FVF_TEX (D3DFVF_XYZRHW | D3DFVF_TEX1)

static HRESULT failed_hr = S_OK;
static const char *failed_at = "-";

static int check(HRESULT hr, const char *call)
{
    if (SUCCEEDED(hr)) return 0;
    if (SUCCEEDED(failed_hr)) { failed_hr = hr; failed_at = call; }
    return -1;
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcW(window, message, wparam, lparam);
}

static unsigned pixel(const D3DLOCKED_RECT *rect, unsigned x, unsigned y)
{
    const DWORD *row = (const DWORD *)((const BYTE *)rect->pBits + (size_t)y * (size_t)rect->Pitch);
    return (unsigned)(row[x] & 0xffffff);   /* X8R8G8B8: 0x00RRGGBB */
}

int main(void)
{
    /* The triangle d3d11tri64.exe draws, in pixels: (0,0.8), (0.7,-0.7), (-0.7,-0.7). */
    static const ColorVertex triangle[3] = {
        { WIDTH * 0.5f, HEIGHT * 0.1f, 0.0f, 1.0f, 0xffffff00 },
        { WIDTH * 0.85f, HEIGHT * 0.85f, 0.0f, 1.0f, 0xffffff00 },
        { WIDTH * 0.15f, HEIGHT * 0.85f, 0.0f, 1.0f, 0xffffff00 },
    };
    static const TexVertex square[4] = {
        { MARK_X - MARK / 2, MARK_Y - MARK / 2, 0.0f, 1.0f, 0.0f, 0.0f },
        { MARK_X + MARK / 2, MARK_Y - MARK / 2, 0.0f, 1.0f, 1.0f, 0.0f },
        { MARK_X - MARK / 2, MARK_Y + MARK / 2, 0.0f, 1.0f, 0.0f, 1.0f },
        { MARK_X + MARK / 2, MARK_Y + MARK / 2, 0.0f, 1.0f, 1.0f, 1.0f },
    };
    WNDCLASSW window_class = { .lpfnWndProc = window_proc, .hInstance = GetModuleHandleW(NULL),
                               .lpszClassName = L"d3d9tri32" };
    D3DPRESENT_PARAMETERS params;
    IDirect3D9 *d3d = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3DVertexBuffer9 *buffer = NULL;
    IDirect3DTexture9 *texture = NULL;
    IDirect3DSurface9 *back = NULL, *copy = NULL;
    LARGE_INTEGER frequency, start, end;
    unsigned frames = 0, centre = 0, mark = 0, texmem_mib = 0;
    HWND window;

    RegisterClassW(&window_class);
    window = CreateWindowExW(0, L"d3d9tri32", L"d3d9tri32", WS_POPUP | WS_VISIBLE, 0, 0, WIDTH, HEIGHT, NULL, NULL,
                             window_class.hInstance, NULL);
    if (!window) check(HRESULT_FROM_WIN32(GetLastError()), "CreateWindowExW");
    if (SUCCEEDED(failed_hr) && !(d3d = Direct3DCreate9(D3D_SDK_VERSION))) check(E_FAIL, "Direct3DCreate9");
    memset(&params, 0, sizeof(params));
    params.BackBufferWidth = WIDTH;
    params.BackBufferHeight = HEIGHT;
    params.BackBufferFormat = D3DFMT_X8R8G8B8;
    params.BackBufferCount = 1;
    params.SwapEffect = D3DSWAPEFFECT_DISCARD;
    params.hDeviceWindow = window;
    params.Windowed = TRUE;
    params.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    if (d3d)
        check(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                      D3DCREATE_HARDWARE_VERTEXPROCESSING, &params, &device), "CreateDevice");
    if (device) {
        D3DLOCKED_RECT rect;
        texmem_mib = IDirect3DDevice9_GetAvailableTextureMem(device) >> 20;
        check(IDirect3DDevice9_CreateVertexBuffer(device, sizeof(triangle), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                                  FVF_COLOR, D3DPOOL_DEFAULT, &buffer, NULL), "CreateVertexBuffer");
        if (!check(IDirect3DDevice9_CreateTexture(device, MARK, MARK, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED,
                                                  &texture, NULL), "CreateTexture") &&
            !check(IDirect3DTexture9_LockRect(texture, 0, &rect, NULL, 0), "LockRect texture")) {
            for (unsigned y = 0; y < MARK; y++) {
                DWORD *row = (DWORD *)((BYTE *)rect.pBits + (size_t)y * (size_t)rect.Pitch);
                for (unsigned x = 0; x < MARK; x++) row[x] = 0xff00ff00;
            }
            IDirect3DTexture9_UnlockRect(texture, 0);
        }
        check(IDirect3DDevice9_GetBackBuffer(device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &back), "GetBackBuffer");
        check(IDirect3DDevice9_CreateOffscreenPlainSurface(device, WIDTH, HEIGHT, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                           &copy, NULL), "CreateOffscreenPlainSurface");
        IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
        IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE);
        IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE, D3DZB_FALSE);
    }

    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    for (; device && SUCCEEDED(failed_hr) && frames < FRAMES; frames++) {
        MSG message;
        void *data;
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        if (check(IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 255), 1.0f, 0), "Clear") ||
            check(IDirect3DDevice9_BeginScene(device), "BeginScene"))
            break;
        if (!check(IDirect3DVertexBuffer9_Lock(buffer, 0, 0, &data, D3DLOCK_DISCARD), "Lock vertex buffer")) {
            memcpy(data, triangle, sizeof(triangle));
            IDirect3DVertexBuffer9_Unlock(buffer);
        }
        IDirect3DDevice9_SetStreamSource(device, 0, buffer, 0, sizeof(ColorVertex));
        IDirect3DDevice9_SetFVF(device, FVF_COLOR);
        IDirect3DDevice9_SetTexture(device, 0, NULL);
        check(IDirect3DDevice9_DrawPrimitive(device, D3DPT_TRIANGLELIST, 0, 1), "DrawPrimitive");
        IDirect3DDevice9_SetFVF(device, FVF_TEX);
        IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)texture);
        check(IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, square, sizeof(TexVertex)),
              "DrawPrimitiveUP");
        check(IDirect3DDevice9_EndScene(device), "EndScene");
        if (frames + 1 == READBACK && back && copy &&
            !check(IDirect3DDevice9_GetRenderTargetData(device, back, copy), "GetRenderTargetData")) {
            D3DLOCKED_RECT rect;
            if (!check(IDirect3DSurface9_LockRect(copy, &rect, NULL, D3DLOCK_READONLY), "LockRect copy")) {
                centre = pixel(&rect, WIDTH / 2, HEIGHT / 2);
                mark = pixel(&rect, MARK_X, MARK_Y);
                IDirect3DSurface9_UnlockRect(copy);
            }
        }
        if (check(IDirect3DDevice9_Present(device, NULL, NULL, NULL, NULL), "Present")) break;
    }
    QueryPerformanceCounter(&end);
    {
        double ms = frames ? (double)(end.QuadPart - start.QuadPart) * 1000.0 / (double)frequency.QuadPart / frames : 0.0;
        printf("D3D9 bits=%u frames=%u swapchain=%ux%u hr=0x%lx at=%s px_c=%06x px_m=%06x texmem_mib=%u ms_avg=%.1f\n",
               BITS, frames, (unsigned)params.BackBufferWidth, (unsigned)params.BackBufferHeight,
               (unsigned long)failed_hr, failed_at, centre, mark, texmem_mib, ms);
        fflush(stdout);
    }
    if (copy) IDirect3DSurface9_Release(copy);
    if (back) IDirect3DSurface9_Release(back);
    if (texture) IDirect3DTexture9_Release(texture);
    if (buffer) IDirect3DVertexBuffer9_Release(buffer);
    if (device) IDirect3DDevice9_Release(device);
    if (d3d) IDirect3D9_Release(d3d);
    if (window) DestroyWindow(window);
    return SUCCEEDED(failed_hr) && frames == FRAMES ? 0 : 1;
}
