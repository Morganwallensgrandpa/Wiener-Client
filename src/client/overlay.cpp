#include "client/overlay.h"
#include "client/core.h"
#include "modules/module.h"
#include "render/blur.h"
#include "render/gl.h"
#include "ui/menu.h"
#include "ui/theme.h"
#include "common/log.h"

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_win32.h"

// Must be declared at global scope: the backend exports this with C linkage.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace fc::overlay {

namespace {
HWND    g_hwnd = nullptr;
WNDPROC g_orig_wndproc = nullptr;
bool    g_init = false;
bool    g_fonts_loaded = false;
gpu::GlassBlur g_blur;

void apply_style() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.0f;
    s.ChildRounding = 12.0f;
    s.FrameRounding = 9.0f;
    s.GrabRounding = 9.0f;
    s.PopupRounding = 12.0f;
    s.ScrollbarRounding = 12.0f;
    s.WindowBorderSize = 0.0f;
    s.FrameBorderSize = 0.0f;
    s.ScrollbarSize = 10.0f;
    s.WindowPadding = ImVec2(14, 14);
    s.ItemSpacing = ImVec2(10, 10);
    s.Colors[ImGuiCol_Text] = ImVec4(0.93f, 0.94f, 1.00f, 1.00f);
    s.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    s.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(1, 1, 1, 0.18f);
    s.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1, 1, 1, 0.30f);
    s.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(1, 1, 1, 0.40f);
    s.Colors[ImGuiCol_CheckMark] = ImVec4(1, 1, 1, 1);
    s.Colors[ImGuiCol_SliderGrab] = ImVec4(1, 1, 1, 1);
    s.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.55f, 0.42f, 1.0f, 0.45f);
}

LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    // Toggle key is handled before anything else so it works even with the menu
    // closed and the game holding input.
    //
    // Right Shift, not Insert. Windows never delivers VK_RSHIFT through
    // WM_KEYDOWN -- it reports the generic VK_SHIFT for either side and puts the
    // scan code in bits 16..23 of lParam, where right shift is 0x36 (left is
    // 0x2A). Bit 30 is set on auto-repeat, so ignore repeats and require a real
    // press-to-release; otherwise holding the key strobes the menu open/shut.
    const bool rshift = w == VK_SHIFT && ((l >> 16) & 0xFF) == 0x36;
    const bool repeat = (l & (1 << 30)) != 0;
    if (m == WM_KEYDOWN && rshift && !repeat) {
        ui::toggle_menu();
        return 0;
    }

    if (ui::menu_open()) {
        ImGui_ImplWin32_WndProcHandler(h, m, w, l);
        switch (m) {
            case WM_LBUTTONDOWN: case WM_LBUTTONUP:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP:
            case WM_MBUTTONDOWN: case WM_MBUTTONUP:
            case WM_XBUTTONDOWN: case WM_XBUTTONUP:
            case WM_MOUSEMOVE:   case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
            case WM_KEYDOWN:     case WM_KEYUP:
            case WM_CHAR:        case WM_SYSKEYDOWN: case WM_SYSKEYUP:
            case WM_INPUT:
                return 1;   // keep the game from seeing menu input
            default: break;
        }
    }
    return CallWindowProcW(g_orig_wndproc, h, m, w, l);
}
} // namespace

bool initialized() { return g_init; }

bool init(HDC dc) {
    if (g_init) return true;
    if (!dc) return false;

    // GLFW hands us the window's own device context on every frame, so this is
    // the window we subclass -- and the one we read the size from.
    g_hwnd = WindowFromDC(dc);
    if (!g_hwnd) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    theme::init();
    apply_style();

    if (!ImGui_ImplWin32_Init(g_hwnd)) {
        FC_ERROR("overlay", "ImGui_ImplWin32_Init failed");
        return false;
    }
    // nullptr selects "#version 130", which the GL 3.3 context the game gives
    // us handles comfortably. The loader is initialised in here, so this must
    // come before any of our own GL calls.
    if (!ImGui_ImplOpenGL3_Init(nullptr)) {
        FC_ERROR("overlay", "ImGui_ImplOpenGL3_Init failed (no usable GL context)");
        return false;
    }

    if (!g_blur.init())
        FC_WARN("overlay", "glass blur unavailable; falling back to a flat panel");

    // Hook the window procedure so we can read input and swallow it when open.
    g_orig_wndproc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC,
                                                (LONG_PTR)wndproc);

    g_init = true;
    FC_INFO("overlay", "initialised (hwnd=%p gl=%s)", (void*)g_hwnd, gpu::gl_version());
    return true;
}

void frame(HDC dc) {
    (void)dc;
    if (!g_init) return;

    // The drawable is the window's client area; that is what the game sizes its
    // frame buffer to, so ImGui's coordinate space matches the game's.
    RECT rc{};
    if (!GetClientRect(g_hwnd, &rc)) return;
    const int W = rc.right - rc.left;
    const int H = rc.bottom - rc.top;
    if (W <= 0 || H <= 0) return;

    // --- blur the world behind the menu ---------------------------------
    // The frosted backdrop is the Blur module's behaviour: switching it off leaves
    // the menu its flat panel, and its Radius setting is the blur spread.
    Module* blur_module = g_modules.find("Blur");
    const bool blur_on = blur_module && blur_module->enabled;
    float blur_radius = 1.6f;
    if (blur_on) {
        if (Setting* s = blur_module->find("Radius"))
            if (s->num > 0.0f) blur_radius = s->num;
    }

    if (blur_on && (ui::menu_alpha() > 0.01f || ui::menu_open())) {
        const uint32_t tex = g_blur.blit_backdrop(W, H, blur_radius);
        ui::set_backdrop(tex ? (ImTextureID)(uintptr_t)tex : (ImTextureID)0);
    } else {
        ui::set_backdrop((ImTextureID)0);
    }

    // --- ImGui frame ----------------------------------------------------
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplWin32_NewFrame();

    if (!g_fonts_loaded) {
        theme::load_fonts(false);
        g_fonts_loaded = true;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)W, (float)H);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

    // While the menu is open the game must not keep the cursor locked.
    if (ui::menu_open()) ClipCursor(nullptr);

    ImGui::NewFrame();
    theme::tick();
    // Applied before anything draws: the HUD, the world modules and the menu all
    // use the accent colour this picks.
    ui::sync_module_style();

    core::on_render_frame();
    // Module visuals (ESP) belong to the world, so they are submitted before the
    // menu's glass panel and end up underneath it.
    core::draw_modules((float)W, (float)H);

    const ImVec2 display((float)W, (float)H);
    if (ui::menu_open() || ui::menu_alpha() > 0.01f)
        ui::draw(core::env(), display);
    ui::draw_hud(display);

    ImGui::Render();

    // One line per second while the menu is open. It records the drawable size
    // the overlay believed in and what ImGui handed back, so a menu that opens
    // with no visible controls can be diagnosed from the log instead of guessed
    // at from a screenshot.
    {
        // Log the first frame of every open as well as one line per second: the
        // first frame is the one that shows what the menu looked like before any
        // animation settled.
        static double last_diag = -100.0;
        static bool   was_open = false;
        const bool open = ui::menu_open();
        if (open && !was_open) last_diag = -100.0;
        was_open = open;
        const double now = ImGui::GetTime();
        if (open && now - last_diag >= 1.0) {
            last_diag = now;
            GLint viewport[4] = {0, 0, 0, 0};
            GLint scissor[4] = {0, 0, 0, 0};
            glGetIntegerv(GL_VIEWPORT, viewport);
            glGetIntegerv(GL_SCISSOR_BOX, scissor);
            FC_INFO("overlay",
                    "gl: client=%dx%d viewport=%dx%d@(%d,%d) scissor=%dx%d@(%d,%d)",
                    W, H, viewport[2], viewport[3], viewport[0], viewport[1],
                    scissor[2], scissor[3], scissor[0], scissor[1]);
            ui::log_diagnostics();
        }
    }

    // We are in the middle of the game's frame: the game may still have one of
    // its own framebuffers bound, so draw into the window explicitly.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void shutdown() {
    if (g_orig_wndproc && g_hwnd) {
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_orig_wndproc);
        g_orig_wndproc = nullptr;
    }
    if (g_init) {
        g_blur.release();
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    gpu::gl_unload();

    g_init = false;
    g_fonts_loaded = false;
    g_hwnd = nullptr;
}

} // namespace fc::overlay
