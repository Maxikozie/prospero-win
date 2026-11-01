/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* A 64-bit Direct3D 11 program for the PS4 port's milestone e, run under
 * DXVK 2.6.2 (DXVK deleted its own d3d11 tests in 2022). A 1920x1080 popup
 * window and a swapchain of that size (the one VideoOut size known to be
 * accepted), shaders compiled at run time by D3DCompile (Wine's builtin
 * d3dcompiler_47), and FRAMES frames each cleared to blue with one yellow
 * triangle drawn from SV_VertexID over the centre. Only 0 and 1 colour
 * components, so a UNORM or sRGB target cannot shift them; swapped red and
 * blue fail both. At frame READBACK the back buffer is copied to a staging
 * texture and the centre (960,540, inside the triangle) and 240,135
 * (outside it) are read. One line on stdout:
 *
 *   D3D11 frames=N swapchain=WxH level=XXXX hr=0x0 at=- px_c=ffff00 px_m=0000ff ms_avg=N.N
 *
 * hr and at name the first failing call. Exit 0 when every frame was
 * presented. Built with x86_64-w64-mingw32-gcc -ld3d11 -ldxgi
 * -ld3dcompiler_47 -luser32; tests/test_wine_d3d11tri64.py runs it under the
 * host Wine with DXVK. */
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <stdio.h>
#include <string.h>

enum { WIDTH = 1920, HEIGHT = 1080, FRAMES = 600, READBACK = 300 };

static const char vertex_shader[] =
    "float4 main(uint id : SV_VertexID) : SV_Position\n"
    "{\n"
    "    const float2 corner[3] = { float2(0.0, 0.8), float2(0.7, -0.7), float2(-0.7, -0.7) };\n"
    "    return float4(corner[id], 0.0, 1.0);\n"
    "}\n";
static const char pixel_shader[] =
    "float4 main() : SV_Target\n"
    "{\n"
    "    return float4(1.0, 1.0, 0.0, 1.0);\n"
    "}\n";

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

static ID3DBlob *compile(const char *source, const char *target)
{
    ID3DBlob *code = NULL, *errors = NULL;
    HRESULT hr = D3DCompile(source, strlen(source), NULL, NULL, NULL, "main", target, 0, 0, &code, &errors);
    if (errors) {
        fprintf(stderr, "%s: %s\n", target, (const char *)ID3D10Blob_GetBufferPointer(errors));
        ID3D10Blob_Release(errors);
    }
    check(hr, target[0] == 'v' ? "D3DCompile vs" : "D3DCompile ps");
    return SUCCEEDED(hr) ? code : NULL;
}

/* 0xRRGGBB of one R8G8B8A8 pixel of a mapped staging texture. */
static unsigned pixel(const D3D11_MAPPED_SUBRESOURCE *map, unsigned x, unsigned y)
{
    const BYTE *p = (const BYTE *)map->pData + (size_t)y * map->RowPitch + (size_t)x * 4;
    return (unsigned)p[0] << 16 | (unsigned)p[1] << 8 | p[2];
}

int main(void)
{
    WNDCLASSW window_class = { .lpfnWndProc = window_proc, .hInstance = GetModuleHandleW(NULL),
                               .lpszClassName = L"d3d11tri64" };
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    const FLOAT blue[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    DXGI_SWAP_CHAIN_DESC desc;
    D3D11_VIEWPORT viewport = { 0.0f, 0.0f, (FLOAT)WIDTH, (FLOAT)HEIGHT, 0.0f, 1.0f };
    D3D_FEATURE_LEVEL level = 0;
    IDXGISwapChain *swapchain = NULL;
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    ID3D11Texture2D *back = NULL, *staging = NULL;
    ID3D11RenderTargetView *target = NULL;
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    ID3DBlob *vs_code, *ps_code;
    LARGE_INTEGER frequency, start, end;
    unsigned frames = 0, centre = 0xffffffu, mark = 0xffffffu;
    HWND window;

    RegisterClassW(&window_class);
    window = CreateWindowExW(0, L"d3d11tri64", L"d3d11tri64", WS_POPUP | WS_VISIBLE, 0, 0, WIDTH, HEIGHT, NULL, NULL,
                             window_class.hInstance, NULL);
    if (!window) check(HRESULT_FROM_WIN32(GetLastError()), "CreateWindowEx");

    memset(&desc, 0, sizeof(desc));
    desc.BufferDesc.Width = WIDTH;
    desc.BufferDesc.Height = HEIGHT;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferDesc.RefreshRate.Numerator = 60;
    desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.OutputWindow = window;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (window && !check(D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, levels, 1,
                                                       D3D11_SDK_VERSION, &desc, &swapchain, &device, &level,
                                                       &context), "D3D11CreateDeviceAndSwapChain")) {
        D3D11_TEXTURE2D_DESC copy;
        if (!check(IDXGISwapChain_GetBuffer(swapchain, 0, &IID_ID3D11Texture2D, (void **)&back), "GetBuffer"))
            check(ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)back, NULL, &target),
                  "CreateRenderTargetView");
        if (back) {
            ID3D11Texture2D_GetDesc(back, &copy);
            copy.Usage = D3D11_USAGE_STAGING;
            copy.BindFlags = 0;
            copy.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            copy.MiscFlags = 0;
            check(ID3D11Device_CreateTexture2D(device, &copy, NULL, &staging), "CreateTexture2D staging");
        }
        if ((vs_code = compile(vertex_shader, "vs_4_0"))) {
            check(ID3D11Device_CreateVertexShader(device, ID3D10Blob_GetBufferPointer(vs_code),
                                                  ID3D10Blob_GetBufferSize(vs_code), NULL, &vs), "CreateVertexShader");
            ID3D10Blob_Release(vs_code);
        }
        if ((ps_code = compile(pixel_shader, "ps_4_0"))) {
            check(ID3D11Device_CreatePixelShader(device, ID3D10Blob_GetBufferPointer(ps_code),
                                                 ID3D10Blob_GetBufferSize(ps_code), NULL, &ps), "CreatePixelShader");
            ID3D10Blob_Release(ps_code);
        }
    }

    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    for (; SUCCEEDED(failed_hr) && frames < FRAMES; frames++) {
        MSG message;
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        ID3D11DeviceContext_OMSetRenderTargets(context, 1, &target, NULL);
        ID3D11DeviceContext_ClearRenderTargetView(context, target, blue);
        ID3D11DeviceContext_RSSetViewports(context, 1, &viewport);
        ID3D11DeviceContext_IASetPrimitiveTopology(context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(context, vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(context, ps, NULL, 0);
        ID3D11DeviceContext_Draw(context, 3, 0);
        if (frames + 1 == READBACK) {
            D3D11_MAPPED_SUBRESOURCE map;
            ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging, (ID3D11Resource *)back);
            if (!check(ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &map), "Map")) {
                centre = pixel(&map, WIDTH / 2, HEIGHT / 2);
                mark = pixel(&map, 240, 135);
                ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
            }
        }
        if (check(IDXGISwapChain_Present(swapchain, 1, 0), "Present")) break;
    }
    QueryPerformanceCounter(&end);
    {
        double ms = frames ? (double)(end.QuadPart - start.QuadPart) * 1000.0 / (double)frequency.QuadPart / frames : 0.0;
        printf("D3D11 frames=%u swapchain=%ux%u level=%x hr=0x%lx at=%s px_c=%06x px_m=%06x ms_avg=%.1f\n", frames,
               (unsigned)desc.BufferDesc.Width, (unsigned)desc.BufferDesc.Height, (unsigned)level,
               (unsigned long)failed_hr, failed_at, centre, mark, ms);
        fflush(stdout);
    }
    if (context) ID3D11DeviceContext_ClearState(context);
    if (target) ID3D11RenderTargetView_Release(target);
    if (staging) ID3D11Texture2D_Release(staging);
    if (back) ID3D11Texture2D_Release(back);
    if (vs) ID3D11VertexShader_Release(vs);
    if (ps) ID3D11PixelShader_Release(ps);
    if (swapchain) IDXGISwapChain_Release(swapchain);
    if (context) ID3D11DeviceContext_Release(context);
    if (device) ID3D11Device_Release(device);
    if (window) DestroyWindow(window);
    return frames == FRAMES && SUCCEEDED(failed_hr) ? 0 : 1;
}
