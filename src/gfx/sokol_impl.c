// SPDX-License-Identifier: LGPL-2.0-or-later
//
// The single sokol_gfx implementation TU (non-Apple). Compiled with the GLCORE
// backend on Linux and the D3D11 backend on Windows; the Apple build uses the
// Objective-C .m sibling with Metal. Exactly one TU defines SOKOL_IMPL.
//
// The engine issues only sokol calls; the host creates the device/context and
// hands in the swapchain (sg_environment / sg_swapchain), so toe stays
// window-system-context-free. On Windows that swapchain is a DXGI one and the
// GPU path is native D3D11 — no GL, no ANGLE, no translation layer.

#if defined(_WIN32)
#define SOKOL_D3D11
#else
#define SOKOL_GLCORE
#endif
#define SOKOL_IMPL
#include "sokol/sokol_gfx.h"
#include "sokol/sokol_log.h"
