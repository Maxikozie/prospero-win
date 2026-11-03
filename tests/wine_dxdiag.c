/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* What the DirectX SDK's GetDXVersion asks (Psychonauts' startup check):
 * dxdiagn's provider, initialised, and DxDiag_SystemInfo's version. One line:
 *   DXDIAG computer=ok init=0x00000000 major=9 minor=0 letter=c
 * computer is GetComputerNameW (dxdiagn's system tree fails without it). */
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <dxdiag.h>
#include <stdio.h>

int main(void)
{
    IDxDiagProvider *provider = NULL;
    IDxDiagContainer *root = NULL, *info = NULL;
    DXDIAG_INIT_PARAMS params = { sizeof(params), DXDIAG_DX9_SDK_VERSION, FALSE, NULL };
    WCHAR name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = sizeof(name) / sizeof(name[0]);
    char computer[32], major[16] = "-", minor[16] = "-", letter[16] = "-";
    VARIANT v;
    HRESULT hr;

    if (GetComputerNameW(name, &size)) snprintf(computer, sizeof(computer), "ok");
    else snprintf(computer, sizeof(computer), "err:%lu", GetLastError());
    CoInitialize(NULL);
    hr = CoCreateInstance(&CLSID_DxDiagProvider, NULL, CLSCTX_INPROC_SERVER, &IID_IDxDiagProvider,
                          (void **)&provider);
    if (SUCCEEDED(hr)) hr = IDxDiagProvider_Initialize(provider, &params);
    if (SUCCEEDED(hr)) hr = IDxDiagProvider_GetRootContainer(provider, &root);
    if (SUCCEEDED(hr)) hr = IDxDiagContainer_GetChildContainer(root, L"DxDiag_SystemInfo", &info);
    if (SUCCEEDED(hr))
    {
        VariantInit(&v);
        if (SUCCEEDED(IDxDiagContainer_GetProp(info, L"dwDirectXVersionMajor", &v)) && V_VT(&v) == VT_UI4)
            snprintf(major, sizeof(major), "%lu", V_UI4(&v));
        VariantInit(&v);
        if (SUCCEEDED(IDxDiagContainer_GetProp(info, L"dwDirectXVersionMinor", &v)) && V_VT(&v) == VT_UI4)
            snprintf(minor, sizeof(minor), "%lu", V_UI4(&v));
        VariantInit(&v);
        if (SUCCEEDED(IDxDiagContainer_GetProp(info, L"szDirectXVersionLetter", &v)) && V_VT(&v) == VT_BSTR)
            snprintf(letter, sizeof(letter), "%ls", V_BSTR(&v));
    }
    printf("DXDIAG computer=%s init=0x%08lx major=%s minor=%s letter=%s\n", computer, (unsigned long)hr, major,
           minor, letter);
    fflush(stdout);
    return 0;
}
