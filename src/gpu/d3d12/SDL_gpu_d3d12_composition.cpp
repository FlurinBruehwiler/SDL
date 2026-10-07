/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_error.h>

#if defined(SDL_PLATFORM_WINDOWS) && !defined(SDL_PLATFORM_XBOXONE) && !defined(SDL_PLATFORM_XBOXSERIES)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <dcomp.h>

#include "SDL_gpu_d3d12_composition.h"

#define MAX_COMPOSITION_TEXTURES 3

struct D3D12Composition
{
    HWND hwnd;
    POINT contentOrigin;
    POINT contentOffset;
    bool hasContent;

    ID3D12Device *device12;
    ID3D12Fence *fence12;
    ID3D11Device5 *device11;
    ID3D11DeviceContext4 *context11;
    ID3D11Fence *fence11;
    UINT64 fenceValue;

    ID3D11Texture2D *textures[MAX_COMPOSITION_TEXTURES];
    unsigned int textureCount;
    UINT width;
    UINT height;

    IDCompositionDevice *dcomp;
    IDCompositionTarget *target;
    IDCompositionVisual *root;
    IDCompositionVisual *background;
    IDCompositionVisual *content;
    IDCompositionSurface *surface;
    UINT surfaceWidth;
    UINT surfaceHeight;
};

template <typename T>
static void SafeRelease(T *&p)
{
    if (p) {
        p->Release();
        p = NULL;
    }
}

static bool Fail(const char *what, HRESULT hr)
{
    SDL_SetError("D3D12 composition: %s failed (0x%08lX)", what, (unsigned long)hr);
    return false;
}

static void ReleaseTextures(D3D12Composition *c)
{
    for (unsigned int i = 0; i < c->textureCount; i += 1) {
        SafeRelease(c->textures[i]);
    }
    c->textureCount = 0;
}

static bool CreateD3D11Device(D3D12Composition *c)
{
    typedef HRESULT(WINAPI * CreateDXGIFactory1Fn)(REFIID, void **);
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
    if (!dxgi || !d3d11) {
        SDL_SetError("D3D12 composition: could not load dxgi.dll or d3d11.dll");
        return false;
    }
    CreateDXGIFactory1Fn createFactory = (CreateDXGIFactory1Fn)GetProcAddress(dxgi, "CreateDXGIFactory1");
    PFN_D3D11_CREATE_DEVICE createDevice = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(d3d11, "D3D11CreateDevice");
    if (!createFactory || !createDevice) {
        SDL_SetError("D3D12 composition: missing CreateDXGIFactory1 or D3D11CreateDevice");
        return false;
    }

    IDXGIFactory4 *factory = NULL;
    HRESULT hr = createFactory(__uuidof(IDXGIFactory4), (void **)&factory);
    if (FAILED(hr)) {
        return Fail("CreateDXGIFactory1", hr);
    }
    IDXGIAdapter *adapter = NULL;
    hr = factory->EnumAdapterByLuid(c->device12->GetAdapterLuid(), __uuidof(IDXGIAdapter), (void **)&adapter);
    factory->Release();
    if (FAILED(hr)) {
        return Fail("EnumAdapterByLuid", hr);
    }

    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    hr = createDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &level, 1, D3D11_SDK_VERSION, &device, NULL, &context);
    adapter->Release();
    if (FAILED(hr)) {
        return Fail("D3D11CreateDevice", hr);
    }
    hr = device->QueryInterface(__uuidof(ID3D11Device5), (void **)&c->device11);
    device->Release();
    if (FAILED(hr)) {
        context->Release();
        return Fail("QueryInterface(ID3D11Device5)", hr);
    }
    hr = context->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&c->context11);
    context->Release();
    if (FAILED(hr)) {
        return Fail("QueryInterface(ID3D11DeviceContext4)", hr);
    }
    return true;
}

static bool CreateSharedFence(D3D12Composition *c)
{
    HRESULT hr = c->device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), (void **)&c->fence12);
    if (FAILED(hr)) {
        return Fail("ID3D12Device::CreateFence", hr);
    }
    HANDLE handle = NULL;
    hr = c->device12->CreateSharedHandle(c->fence12, NULL, GENERIC_ALL, NULL, &handle);
    if (FAILED(hr)) {
        return Fail("CreateSharedHandle(fence)", hr);
    }
    hr = c->device11->OpenSharedFence(handle, __uuidof(ID3D11Fence), (void **)&c->fence11);
    CloseHandle(handle);
    if (FAILED(hr)) {
        return Fail("OpenSharedFence", hr);
    }
    return true;
}

static bool CreateVisualTree(D3D12Composition *c, HWND hwnd, unsigned int backgroundRgb)
{
    typedef HRESULT(WINAPI * DCompositionCreateDeviceFn)(IDXGIDevice *, REFIID, void **);
    HMODULE dcompModule = LoadLibraryW(L"dcomp.dll");
    DCompositionCreateDeviceFn createDevice = dcompModule ? (DCompositionCreateDeviceFn)GetProcAddress(dcompModule, "DCompositionCreateDevice") : NULL;
    if (!createDevice) {
        SDL_SetError("D3D12 composition: could not load DCompositionCreateDevice");
        return false;
    }

    IDXGIDevice *dxgiDevice = NULL;
    HRESULT hr = c->device11->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgiDevice);
    if (FAILED(hr)) {
        return Fail("QueryInterface(IDXGIDevice)", hr);
    }
    hr = createDevice(dxgiDevice, __uuidof(IDCompositionDevice), (void **)&c->dcomp);
    dxgiDevice->Release();
    if (FAILED(hr)) {
        return Fail("DCompositionCreateDevice", hr);
    }

    if (FAILED(hr = c->dcomp->CreateTargetForHwnd(hwnd, TRUE, &c->target)) ||
        FAILED(hr = c->dcomp->CreateVisual(&c->root)) ||
        FAILED(hr = c->dcomp->CreateVisual(&c->background)) ||
        FAILED(hr = c->dcomp->CreateVisual(&c->content))) {
        return Fail("creating the visual tree", hr);
    }

    // A small surface in the background color, scaled to cover any window size. When the window
    // grows, the area the content has not reached yet shows this instead of what is behind the window.
    IDCompositionSurface *backgroundSurface = NULL;
    hr = c->dcomp->CreateSurface(16, 16, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_IGNORE, &backgroundSurface);
    if (FAILED(hr)) {
        return Fail("CreateSurface(background)", hr);
    }
    ID3D11Texture2D *texture = NULL;
    POINT offset;
    hr = backgroundSurface->BeginDraw(NULL, __uuidof(ID3D11Texture2D), (void **)&texture, &offset);
    if (FAILED(hr)) {
        backgroundSurface->Release();
        return Fail("BeginDraw(background)", hr);
    }
    ID3D11RenderTargetView *rtv = NULL;
    hr = c->device11->CreateRenderTargetView(texture, NULL, &rtv);
    if (SUCCEEDED(hr)) {
        float color[4] = {
            ((backgroundRgb >> 16) & 0xFF) / 255.0f,
            ((backgroundRgb >> 8) & 0xFF) / 255.0f,
            (backgroundRgb & 0xFF) / 255.0f,
            1.0f
        };
        D3D11_RECT rect = { offset.x, offset.y, offset.x + 16, offset.y + 16 };
        c->context11->ClearView(rtv, color, &rect, 1);
        rtv->Release();
    }
    texture->Release();
    backgroundSurface->EndDraw();

    D2D_MATRIX_3X2_F scale = { { { 4096.0f, 0.0f, 0.0f, 4096.0f, 0.0f, 0.0f } } };
    c->background->SetContent(backgroundSurface);
    c->background->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    c->background->SetTransform(scale);
    backgroundSurface->Release();

    c->root->AddVisual(c->background, FALSE, NULL);
    c->root->AddVisual(c->content, TRUE, c->background);
    c->target->SetRoot(c->root);

    hr = c->dcomp->Commit();
    if (FAILED(hr)) {
        return Fail("Commit", hr);
    }
    return true;
}

extern "C" void D3D12_DestroyComposition(D3D12Composition *c)
{
    if (!c) {
        return;
    }
    ReleaseTextures(c);
    SafeRelease(c->surface);
    SafeRelease(c->content);
    SafeRelease(c->background);
    SafeRelease(c->root);
    SafeRelease(c->target);
    if (c->dcomp) {
        c->dcomp->Commit();
    }
    SafeRelease(c->dcomp);
    SafeRelease(c->fence11);
    SafeRelease(c->fence12);
    SafeRelease(c->context11);
    SafeRelease(c->device11);
    SafeRelease(c->device12);
    SDL_free(c);
}

extern "C" D3D12Composition *D3D12_CreateComposition(void *hwnd, void *d3d12Device, unsigned int backgroundRgb)
{
    D3D12Composition *c = (D3D12Composition *)SDL_calloc(1, sizeof(D3D12Composition));
    if (!c) {
        return NULL;
    }
    c->hwnd = (HWND)hwnd;
    c->device12 = (ID3D12Device *)d3d12Device;
    c->device12->AddRef();

    if (!CreateD3D11Device(c) || !CreateSharedFence(c) || !CreateVisualTree(c, (HWND)hwnd, backgroundRgb)) {
        D3D12_DestroyComposition(c);
        return NULL;
    }
    return c;
}

extern "C" bool D3D12_SetCompositionTextures(D3D12Composition *c, void **d3d12Resources, unsigned int count)
{
    ReleaseTextures(c);
    if (count > MAX_COMPOSITION_TEXTURES) {
        return SDL_SetError("D3D12 composition: too many textures");
    }

    for (unsigned int i = 0; i < count; i += 1) {
        ID3D12Resource *resource = (ID3D12Resource *)d3d12Resources[i];
        HANDLE handle = NULL;
        HRESULT hr = c->device12->CreateSharedHandle(resource, NULL, GENERIC_ALL, NULL, &handle);
        if (FAILED(hr)) {
            ReleaseTextures(c);
            return Fail("CreateSharedHandle(texture)", hr);
        }
        hr = c->device11->OpenSharedResource1(handle, __uuidof(ID3D11Texture2D), (void **)&c->textures[i]);
        CloseHandle(handle);
        if (FAILED(hr)) {
            ReleaseTextures(c);
            return Fail("OpenSharedResource1", hr);
        }
        c->textureCount = i + 1;
    }

    D3D11_TEXTURE2D_DESC desc;
    c->textures[0]->GetDesc(&desc);
    c->width = desc.Width;
    c->height = desc.Height;
    return true;
}

extern "C" bool D3D12_PresentComposition(D3D12Composition *c, void *d3d12CommandQueue, unsigned int index)
{
    ID3D12CommandQueue *queue = (ID3D12CommandQueue *)d3d12CommandQueue;
    HRESULT hr;

    if (index >= c->textureCount) {
        return SDL_SetError("D3D12 composition: invalid texture index");
    }

    if (!c->surface || c->surfaceWidth != c->width || c->surfaceHeight != c->height) {
        SafeRelease(c->surface);
        hr = c->dcomp->CreateSurface(c->width, c->height, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_IGNORE, &c->surface);
        if (FAILED(hr)) {
            return Fail("CreateSurface", hr);
        }
        c->surfaceWidth = c->width;
        c->surfaceHeight = c->height;
    }

    // The D3D11 copy waits on the GPU for the D3D12 work that rendered the frame...
    c->fenceValue += 1;
    hr = queue->Signal(c->fence12, c->fenceValue);
    if (FAILED(hr)) {
        return Fail("ID3D12CommandQueue::Signal", hr);
    }
    c->context11->Wait(c->fence11, c->fenceValue);

    ID3D11Texture2D *destination = NULL;
    POINT offset;
    hr = c->surface->BeginDraw(NULL, __uuidof(ID3D11Texture2D), (void **)&destination, &offset);
    if (FAILED(hr)) {
        return Fail("BeginDraw", hr);
    }
    c->context11->CopySubresourceRegion(destination, 0, offset.x, offset.y, 0, c->textures[index], 0, NULL);
    destination->Release();

    // ...and later D3D12 work waits for the copy, so the texture is not rendered into again before it is read.
    c->fenceValue += 1;
    c->context11->Signal(c->fence11, c->fenceValue);
    c->context11->Flush();
    hr = queue->Wait(c->fence12, c->fenceValue);
    if (FAILED(hr)) {
        c->surface->EndDraw();
        return Fail("ID3D12CommandQueue::Wait", hr);
    }

    hr = c->surface->EndDraw();
    if (FAILED(hr)) {
        return Fail("EndDraw", hr);
    }
    c->content->SetContent(c->surface);
    c->content->SetOffsetX(0.0f);
    c->content->SetOffsetY(0.0f);
    hr = c->dcomp->Commit();
    if (FAILED(hr)) {
        return Fail("Commit", hr);
    }

    c->contentOrigin.x = 0;
    c->contentOrigin.y = 0;
    ClientToScreen(c->hwnd, &c->contentOrigin);
    c->contentOffset.x = 0;
    c->contentOffset.y = 0;
    c->hasContent = true;
    return true;
}

extern "C" void D3D12_KeepCompositionContentInPlace(D3D12Composition *c)
{
    if (!c->hasContent) {
        return;
    }

    // Visuals are positioned relative to the window, so when its client area moves (dragging the left or top
    // edge), the last frame would move along with it until the frame for the new size is committed. Offsetting
    // it back keeps it where it was on screen, like the content of a window resized from its right edge.
    // A plain move keeps the size and is not followed by a new frame, so the content has to move with the window.
    RECT client;
    GetClientRect(c->hwnd, &client);
    bool resized = (UINT)client.right != c->surfaceWidth || (UINT)client.bottom != c->surfaceHeight;

    POINT origin = { 0, 0 };
    ClientToScreen(c->hwnd, &origin);
    POINT offset = { 0, 0 };
    if (resized) {
        offset.x = c->contentOrigin.x - origin.x;
        offset.y = c->contentOrigin.y - origin.y;
    }
    if (offset.x == c->contentOffset.x && offset.y == c->contentOffset.y) {
        return;
    }

    c->content->SetOffsetX((float)offset.x);
    c->content->SetOffsetY((float)offset.y);
    c->contentOffset = offset;
    c->dcomp->Commit();
}

#endif
