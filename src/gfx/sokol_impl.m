// SPDX-License-Identifier: LGPL-2.0-or-later
//
// The single sokol_gfx implementation TU. On Apple this MUST be Objective-C
// (Metal uses ObjC APIs), so it's a .m; the Linux build compiles a .c sibling
// with the GL backend. Exactly one TU defines SOKOL_IMPL.
//
// The engine issues only sokol calls; the host creates the Metal device /
// swapchain and hands them in (sg_environment / sg_swapchain), so toe stays
// window-system-context-free — the same division it had with GL.

#define SOKOL_METAL
#define SOKOL_IMPL
#include "sokol/sokol_gfx.h"
#include "sokol/sokol_log.h"
