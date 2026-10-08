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

#ifndef SDL_gpu_d3d12_bitblt_h_
#define SDL_gpu_d3d12_bitblt_h_

/* Presents D3D12 frames through a bitblt-model DXGI swapchain instead of a flip-model one.
 *
 * DWM shows a flip-model swapchain's frames independently of window geometry changes, so a
 * window being resized shows stale frames pinned to its top-left corner. A bitblt-model
 * swapchain presents into the window's redirection surface, which DWM keeps in step with
 * resizes, the same as GDI content. D3D12 only supports flip-model swapchains, so each frame
 * is rendered into a shared D3D12 texture and copied into the back buffer of a D3D11 bitblt
 * swapchain by a D3D11 device on the same adapter.
 *
 * The D3D11 device belongs to the GPU device and is created with it when the
 * SDL_GPU_D3D12_BITBLT hint is set, so its cost lands wherever the GPU device is created
 * (often a background thread) instead of when a window is claimed.
 */

#define SDL_HINT_GPU_D3D12_BITBLT "SDL_GPU_D3D12_BITBLT"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct D3D12BitbltDevice D3D12BitbltDevice;
typedef struct D3D12BitbltPresenter D3D12BitbltPresenter;

D3D12BitbltDevice *D3D12_CreateBitbltDevice(void *d3d12Device);

void D3D12_DestroyBitbltDevice(D3D12BitbltDevice *device);

D3D12BitbltPresenter *D3D12_CreateBitbltPresenter(D3D12BitbltDevice *device, void *hwnd);

bool D3D12_SetBitbltPresenterTextures(D3D12BitbltPresenter *presenter, void **d3d12Resources, unsigned int count);

bool D3D12_PresentBitblt(D3D12BitbltPresenter *presenter, void *d3d12CommandQueue, unsigned int index, unsigned int syncInterval);

void D3D12_DestroyBitbltPresenter(D3D12BitbltPresenter *presenter);

#ifdef __cplusplus
}
#endif

#endif
