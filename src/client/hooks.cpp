#include "client/hooks.h"
#include "client/overlay.h"
#include "common/log.h"
#include "ui/menu.h"

#include <windows.h>
#include "MinHook.h"

namespace fc::hooks {

namespace {

using SwapFn         = BOOL(WINAPI*)(HDC);
using SetInputModeFn = void (*)(void* window, int mode, int value);
using SetCursorPosFn = void (*)(void* window, double x, double y);
using GetInputModeFn = int (*)(void* window, int mode);
using GetContextFn   = void* (*)();

SwapFn         g_swap = nullptr;
SetInputModeFn g_real_set_input_mode = nullptr;
SetCursorPosFn g_real_set_cursor_pos = nullptr;
void*          g_target = nullptr;
bool           g_installed = false;

// --- GLFW cursor handling ---------------------------------------------------
//
// A game that has grabbed the mouse recentres the pointer every frame and reads
// it as raw deltas, which leaves ImGui with a cursor pinned to the middle of
// the window -- the menu would open but no button could be clicked. So while
// the menu is up we hand the mouse back to Windows, and we stop the game from
// re-grabbing it.

// GLFW constants (from glfw3.h; we do not link GLFW ourselves).
constexpr int kGlfwCursor         = 0x00033001;
constexpr int kGlfwCursorNormal   = 0x00034001;
constexpr int kGlfwCursorHidden   = 0x00034002;
constexpr int kGlfwCursorDisabled = 0x00034003;

void* g_glfw_window = nullptr;
int   g_game_cursor_mode = 0;   // what the game last asked for (0 = not seen)
bool  g_cursor_overridden = false;

void hooked_set_input_mode(void* window, int mode, int value) {
    if (!g_real_set_input_mode) return;
    if (mode == kGlfwCursor) {
        // Remember the game's intent so we can hand the grab back on close.
        g_game_cursor_mode = value;
        if (window) g_glfw_window = window;
        if (ui::menu_open() && value != kGlfwCursorNormal) {
            g_real_set_input_mode(window, mode, kGlfwCursorNormal);
            g_cursor_overridden = true;
            return;
        }
    }
    g_real_set_input_mode(window, mode, value);
}

void hooked_set_cursor_pos(void* window, double x, double y) {
    if (!g_real_set_cursor_pos) return;
    if (ui::menu_open() && g_cursor_overridden) return;   // let the pointer move
    g_real_set_cursor_pos(window, x, y);
}

// Hands the pointer to Windows when the menu opens and back to the game when it
// closes. Cheap enough to call every frame: it only acts on the transition.
void set_menu_cursor() {
    const bool open = ui::menu_open();
    if (open == g_cursor_overridden) return;
    if (!g_real_set_input_mode || !g_glfw_window) return;

    g_cursor_overridden = open;
    if (open) {
        g_real_set_input_mode(g_glfw_window, kGlfwCursor, kGlfwCursorNormal);
    } else {
        const int mode = (g_game_cursor_mode == kGlfwCursorHidden ||
                          g_game_cursor_mode == kGlfwCursorDisabled)
                             ? g_game_cursor_mode
                             : kGlfwCursorDisabled;
        g_real_set_input_mode(g_glfw_window, kGlfwCursor, mode);
    }
}

// Installed lazily from the first frame we render, at which point the game has
// definitely loaded glfw.dll and created its window.
void install_glfw_hooks() {
    static bool attempted = false;
    if (attempted) return;
    attempted = true;

    HMODULE glfw = GetModuleHandleA("glfw.dll");
    if (!glfw) {
        FC_WARN("hooks", "glfw.dll not loaded; the cursor will stay grabbed while "
                         "the menu is open");
        return;
    }

    auto set_input_mode = (void*)GetProcAddress(glfw, "glfwSetInputMode");
    auto set_cursor_pos = (void*)GetProcAddress(glfw, "glfwSetCursorPos");
    auto get_input_mode = (GetInputModeFn)GetProcAddress(glfw, "glfwGetInputMode");
    auto get_context    = (GetContextFn)GetProcAddress(glfw, "glfwGetCurrentContext");

    if (!set_input_mode || !set_cursor_pos) {
        FC_WARN("hooks", "glfw entry points missing; cursor handling disabled");
        return;
    }

    if (MH_CreateHook(set_input_mode, (void*)&hooked_set_input_mode,
                      (void**)&g_real_set_input_mode) != MH_OK ||
        MH_CreateHook(set_cursor_pos, (void*)&hooked_set_cursor_pos,
                      (void**)&g_real_set_cursor_pos) != MH_OK) {
        FC_WARN("hooks", "could not hook GLFW cursor calls");
        return;
    }
    MH_EnableHook(MH_ALL_HOOKS);

    // We are called after the game grabbed the mouse, so its original call is
    // long gone -- ask GLFW what the current mode is and adopt that as the
    // game's intent.
    if (get_context) g_glfw_window = get_context();
    if (get_input_mode && g_glfw_window)
        g_game_cursor_mode = get_input_mode(g_glfw_window, kGlfwCursor);

    FC_INFO("hooks", "glfw cursor hooks installed (window=%p mode=0x%04X)",
            g_glfw_window, (unsigned)g_game_cursor_mode);
}

BOOL WINAPI hk_swap(HDC dc) {
    if (!overlay::initialized()) overlay::init(dc);
    install_glfw_hooks();
    set_menu_cursor();

    overlay::frame(dc);
    return g_swap(dc);
}

} // namespace

void* swap_target() { return g_target; }

bool install() {
    if (g_installed) return true;

    HMODULE gdi = GetModuleHandleA("gdi32.dll");
    if (!gdi) gdi = LoadLibraryA("gdi32.dll");

    g_target = gdi ? (void*)GetProcAddress(gdi, "SwapBuffers") : nullptr;
    if (!g_target) {
        FC_ERROR("hooks", "could not resolve SwapBuffers (gdi32 %p)", (void*)gdi);
        return false;
    }

    if (MH_Initialize() != MH_OK && MH_ERROR_ALREADY_INITIALIZED) {
        FC_ERROR("hooks", "MH_Initialize failed");
        return false;
    }
    if (MH_CreateHook(g_target, (void*)&hk_swap, (void**)&g_swap) != MH_OK) {
        FC_ERROR("hooks", "failed to hook SwapBuffers");
        return false;
    }
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        FC_ERROR("hooks", "MH_EnableHook failed");
        return false;
    }

    g_installed = true;
    FC_INFO("hooks", "installed (SwapBuffers=%p)", g_target);
    return true;
}

void uninstall() {
    if (!g_installed) return;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_installed = false;
}

} // namespace fc::hooks
