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

#include "SDL_gpu_d3d12_bitblt.h"

#define MAX_PRESENTER_TEXTURES 3

struct D3D12BitbltDevice
{
    ID3D12Device *device12;
    ID3D12Fence *fence12;
    ID3D11Device5 *device11;
    ID3D11DeviceContext4 *context11;
    ID3D11Fence *fence11;
    UINT64 fenceValue;
    IDXGIFactory2 *factory;
};

struct D3D12BitbltPresenter
{
    D3D12BitbltDevice *device;
    HWND hwnd;

    IDXGISwapChain1 *swapchain;
    UINT swapchainWidth;
    UINT swapchainHeight;

    ID3D11Texture2D *textures[MAX_PRESENTER_TEXTURES];
    unsigned int textureCount;
    UINT width;
    UINT height;
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
    SDL_SetError("D3D12 bitblt presentation: %s failed (0x%08lX)", what, (unsigned long)hr);
    return false;
}

static bool CreateD3D11Device(D3D12BitbltDevice *d)
{
    typedef HRESULT(WINAPI * CreateDXGIFactory1Fn)(REFIID, void **);
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
    if (!dxgi || !d3d11) {
        SDL_SetError("D3D12 bitblt presentation: could not load dxgi.dll or d3d11.dll");
        return false;
    }
    CreateDXGIFactory1Fn createFactory = (CreateDXGIFactory1Fn)GetProcAddress(dxgi, "CreateDXGIFactory1");
    PFN_D3D11_CREATE_DEVICE createDevice = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(d3d11, "D3D11CreateDevice");
    if (!createFactory || !createDevice) {
        SDL_SetError("D3D12 bitblt presentation: missing CreateDXGIFactory1 or D3D11CreateDevice");
        return false;
    }

    IDXGIFactory4 *factory = NULL;
    HRESULT hr = createFactory(__uuidof(IDXGIFactory4), (void **)&factory);
    if (FAILED(hr)) {
        return Fail("CreateDXGIFactory1", hr);
    }
    IDXGIAdapter *adapter = NULL;
    hr = factory->EnumAdapterByLuid(d->device12->GetAdapterLuid(), __uuidof(IDXGIAdapter), (void **)&adapter);
    if (FAILED(hr)) {
        factory->Release();
        return Fail("EnumAdapterByLuid", hr);
    }
    hr = factory->QueryInterface(__uuidof(IDXGIFactory2), (void **)&d->factory);
    factory->Release();
    if (FAILED(hr)) {
        adapter->Release();
        return Fail("QueryInterface(IDXGIFactory2)", hr);
    }

    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    hr = createDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &level, 1, D3D11_SDK_VERSION, &device, NULL, &context);
    adapter->Release();
    if (FAILED(hr)) {
        return Fail("D3D11CreateDevice", hr);
    }
    hr = device->QueryInterface(__uuidof(ID3D11Device5), (void **)&d->device11);
    device->Release();
    if (FAILED(hr)) {
        context->Release();
        return Fail("QueryInterface(ID3D11Device5)", hr);
    }
    hr = context->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&d->context11);
    context->Release();
    if (FAILED(hr)) {
        return Fail("QueryInterface(ID3D11DeviceContext4)", hr);
    }
    return true;
}

static bool CreateSharedFence(D3D12BitbltDevice *d)
{
    HRESULT hr = d->device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), (void **)&d->fence12);
    if (FAILED(hr)) {
        return Fail("ID3D12Device::CreateFence", hr);
    }
    HANDLE handle = NULL;
    hr = d->device12->CreateSharedHandle(d->fence12, NULL, GENERIC_ALL, NULL, &handle);
    if (FAILED(hr)) {
        return Fail("CreateSharedHandle(fence)", hr);
    }
    hr = d->device11->OpenSharedFence(handle, __uuidof(ID3D11Fence), (void **)&d->fence11);
    CloseHandle(handle);
    if (FAILED(hr)) {
        return Fail("OpenSharedFence", hr);
    }
    return true;
}

extern "C" void D3D12_DestroyBitbltDevice(D3D12BitbltDevice *d)
{
    if (!d) {
        return;
    }
    SafeRelease(d->factory);
    SafeRelease(d->fence11);
    SafeRelease(d->fence12);
    SafeRelease(d->context11);
    SafeRelease(d->device11);
    SafeRelease(d->device12);
    SDL_free(d);
}

extern "C" D3D12BitbltDevice *D3D12_CreateBitbltDevice(void *d3d12Device)
{
    D3D12BitbltDevice *d = (D3D12BitbltDevice *)SDL_calloc(1, sizeof(D3D12BitbltDevice));
    if (!d) {
        return NULL;
    }
    d->device12 = (ID3D12Device *)d3d12Device;
    d->device12->AddRef();

    if (!CreateD3D11Device(d) || !CreateSharedFence(d)) {
        D3D12_DestroyBitbltDevice(d);
        return NULL;
    }
    return d;
}

static void ReleaseTextures(D3D12BitbltPresenter *p)
{
    for (unsigned int i = 0; i < p->textureCount; i += 1) {
        SafeRelease(p->textures[i]);
    }
    p->textureCount = 0;
}

extern "C" void D3D12_DestroyBitbltPresenter(D3D12BitbltPresenter *p)
{
    if (!p) {
        return;
    }
    ReleaseTextures(p);
    SafeRelease(p->swapchain);
    SDL_free(p);
}

extern "C" D3D12BitbltPresenter *D3D12_CreateBitbltPresenter(D3D12BitbltDevice *device, void *hwnd)
{
    D3D12BitbltPresenter *p = (D3D12BitbltPresenter *)SDL_calloc(1, sizeof(D3D12BitbltPresenter));
    if (!p) {
        return NULL;
    }
    p->device = device;
    p->hwnd = (HWND)hwnd;

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 1;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;

    HRESULT hr = device->factory->CreateSwapChainForHwnd(device->device11, p->hwnd, &desc, NULL, NULL, &p->swapchain);
    if (FAILED(hr)) {
        Fail("CreateSwapChainForHwnd", hr);
        D3D12_DestroyBitbltPresenter(p);
        return NULL;
    }
    device->factory->MakeWindowAssociation(p->hwnd, DXGI_MWA_NO_WINDOW_CHANGES);

    p->swapchain->GetDesc1(&desc);
    p->swapchainWidth = desc.Width;
    p->swapchainHeight = desc.Height;
    return p;
}

extern "C" bool D3D12_SetBitbltPresenterTextures(D3D12BitbltPresenter *p, void **d3d12Resources, unsigned int count)
{
    D3D12BitbltDevice *d = p->device;

    ReleaseTextures(p);
    if (count > MAX_PRESENTER_TEXTURES) {
        return SDL_SetError("D3D12 bitblt presentation: too many textures");
    }

    for (unsigned int i = 0; i < count; i += 1) {
        ID3D12Resource *resource = (ID3D12Resource *)d3d12Resources[i];
        HANDLE handle = NULL;
        HRESULT hr = d->device12->CreateSharedHandle(resource, NULL, GENERIC_ALL, NULL, &handle);
        if (FAILED(hr)) {
            ReleaseTextures(p);
            return Fail("CreateSharedHandle(texture)", hr);
        }
        hr = d->device11->OpenSharedResource1(handle, __uuidof(ID3D11Texture2D), (void **)&p->textures[i]);
        CloseHandle(handle);
        if (FAILED(hr)) {
            ReleaseTextures(p);
            return Fail("OpenSharedResource1", hr);
        }
        p->textureCount = i + 1;
    }

    D3D11_TEXTURE2D_DESC desc;
    p->textures[0]->GetDesc(&desc);
    p->width = desc.Width;
    p->height = desc.Height;
    return true;
}

extern "C" bool D3D12_PresentBitblt(D3D12BitbltPresenter *p, void *d3d12CommandQueue, unsigned int index, unsigned int syncInterval)
{
    D3D12BitbltDevice *d = p->device;
    ID3D12CommandQueue *queue = (ID3D12CommandQueue *)d3d12CommandQueue;
    HRESULT hr;

    if (index >= p->textureCount) {
        return SDL_SetError("D3D12 bitblt presentation: invalid texture index");
    }

    if (p->swapchainWidth != p->width || p->swapchainHeight != p->height) {
        hr = p->swapchain->ResizeBuffers(0, p->width, p->height, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) {
            return Fail("ResizeBuffers", hr);
        }
        p->swapchainWidth = p->width;
        p->swapchainHeight = p->height;
    }

    // The D3D11 copy waits on the GPU for the D3D12 work that rendered the frame...
    d->fenceValue += 1;
    hr = queue->Signal(d->fence12, d->fenceValue);
    if (FAILED(hr)) {
        return Fail("ID3D12CommandQueue::Signal", hr);
    }
    d->context11->Wait(d->fence11, d->fenceValue);

    ID3D11Texture2D *backbuffer = NULL;
    hr = p->swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&backbuffer);
    if (FAILED(hr)) {
        return Fail("GetBuffer", hr);
    }
    d->context11->CopyResource(backbuffer, p->textures[index]);
    backbuffer->Release();

    // ...and later D3D12 work waits for the copy, so the texture is not rendered into again before it is read.
    d->fenceValue += 1;
    d->context11->Signal(d->fence11, d->fenceValue);
    hr = queue->Wait(d->fence12, d->fenceValue);
    if (FAILED(hr)) {
        return Fail("ID3D12CommandQueue::Wait", hr);
    }

    hr = p->swapchain->Present(syncInterval, 0);
    if (FAILED(hr)) {
        return Fail("Present", hr);
    }
    return true;
}

#endif
