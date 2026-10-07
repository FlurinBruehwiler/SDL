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

#ifndef SDL_gpu_d3d12_composition_h_
#define SDL_gpu_d3d12_composition_h_

/* Presents D3D12 frames through a DirectComposition surface instead of a DXGI swapchain.
 *
 * DWM shows a flip-model swapchain's frames independently of window geometry changes, so a
 * window being resized shows stale, offset frames. DirectComposition surface updates are
 * applied together with the Commit that publishes them, which keeps the content in step with
 * the window. DirectComposition surfaces can only be drawn by D3D11, so each frame is rendered
 * into a shared D3D12 texture and copied into the surface by a D3D11 device on the same adapter.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct D3D12Composition D3D12Composition;

D3D12Composition *D3D12_CreateComposition(void *hwnd, void *d3d12Device, unsigned int backgroundRgb);

bool D3D12_SetCompositionTextures(D3D12Composition *composition, void **d3d12Resources, unsigned int count);

bool D3D12_PresentComposition(D3D12Composition *composition, void *d3d12CommandQueue, unsigned int index);

void D3D12_KeepCompositionContentInPlace(D3D12Composition *composition);

void D3D12_DestroyComposition(D3D12Composition *composition);

#ifdef __cplusplus
}
#endif

#endif
