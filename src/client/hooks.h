#pragma once

namespace fc::hooks {

// Installs the frame hook via MinHook.
//
// Minecraft draws with OpenGL through LWJGL/GLFW, and GLFW hands each finished
// frame to Windows with gdi32's SwapBuffers -- there is no DXGI swap chain
// anywhere in the process. That call is the equivalent of a Present, and the
// only place an overlay can draw into the frame before it reaches the screen.
bool install();
void uninstall();

// The address we hooked, for logging and for the self-test.
void* swap_target();

} // namespace fc::hooks
