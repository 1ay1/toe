// SPDX-License-Identifier: LGPL-2.0-or-later
//
// The single sokol_gfx implementation TU (Linux). Compiled with the GLCORE
// backend; the Apple build uses the Objective-C .m sibling with Metal. Exactly
// one TU defines SOKOL_IMPL.
//
// The engine issues only sokol calls; the host creates the GL context and hands
// in the swapchain (sg_environment / sg_swapchain), so toe stays
// window-system-context-free.

#define SOKOL_GLCORE
#define SOKOL_IMPL
#include "sokol/sokol_gfx.h"
#include "sokol/sokol_log.h"
