// A self-contained smoke test for the parts of the client that normally only
// run inside a live game: the OpenGL overlay, the glass-blur shader pipeline and
// the menu renderer. It creates a real window + WGL context, drives the same
// entry points the game's SwapBuffers hook does, and reports how far it got.
//
// Exposed as `wiener_client_selftest` so a host process can call it without
// Minecraft being present.

#include <windows.h>
#include <GL/gl.h>

#include "imgui_internal.h"      // input state, for the click probe below

#include "client/overlay.h"
#include "client/hooks.h"
#include "render/gl.h"
#include "render/world.h"
#include "ui/menu.h"
#include "ui/theme.h"
#include "modules/module.h"
#include "modules/visuals.h"
#include "common/log.h"

#include <cmath>
#include <cfloat>
#include <vector>
#include <algorithm>

using namespace fc;

namespace {

constexpr int kWidth  = 800;
constexpr int kHeight = 500;

struct GLWindow {
    HWND  wnd = nullptr;
    HDC   dc  = nullptr;
    HGLRC rc  = nullptr;
};

bool make_gl_window(GLWindow& out) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    // This window is a throwaway off-screen target for the GL context, not the
    // client's own window, so it carries its own name rather than the brand.
    wc.lpszClassName = L"wiener_client_selftest";
    RegisterClassExW(&wc);

    out.wnd = CreateWindowExW(0, wc.lpszClassName, L"wiener_client_selftest", WS_POPUP,
                              0, 0, kWidth, kHeight, nullptr, nullptr, wc.hInstance,
                              nullptr);
    if (!out.wnd) return false;

    out.dc = GetDC(out.wnd);
    if (!out.dc) return false;

    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;

    const int fmt = ChoosePixelFormat(out.dc, &pfd);
    if (!fmt || !SetPixelFormat(out.dc, fmt, &pfd)) return false;

    out.rc = wglCreateContext(out.dc);
    if (!out.rc || !wglMakeCurrent(out.dc, out.rc)) return false;
    return true;
}

void destroy_gl_window(GLWindow& w) {
    wglMakeCurrent(nullptr, nullptr);
    if (w.rc) wglDeleteContext(w.rc);
    if (w.dc) ReleaseDC(w.wnd, w.dc);
    if (w.wnd) DestroyWindow(w.wnd);
    w = GLWindow{};
}

// Rebuilds the lParam Windows would hand a window procedure for a key event.
// Bits 16..23 carry the scan code, bit 30 flags auto-repeat, bit 31 a key-up.
LPARAM key_lparam(UINT scan, bool repeat, bool up) {
    LPARAM lp = (LPARAM)(scan & 0xFF) << 16;
    lp |= 1;
    if (repeat) lp |= 1 << 30;
    if (up)     lp |= 1 << 31;
    return lp;
}

// Drives the real window procedure to confirm which key opens the menu. The
// toggle key is Right Shift, so Insert and Left Shift must do nothing and a
// held (auto-repeating) key must not strobe the menu open and shut.
// Returns 0 on success, otherwise the index of the first failed case.
int check_toggle_key(HWND wnd) {
    auto press = [&](UINT vk, UINT scan, bool repeat, bool up) {
        SendMessageW(wnd, up ? WM_KEYUP : WM_KEYDOWN, vk, key_lparam(scan, repeat, up));
    };

    ui::close_menu();

    press(VK_INSERT, 0x52, false, false);          // 1: Insert must not open
    if (ui::menu_open()) return 1;

    press(VK_SHIFT, 0x2A, false, false);           // 2: Left Shift must not open
    if (ui::menu_open()) return 2;

    press(VK_SHIFT, 0x36, false, false);           // 3: Right Shift opens
    if (!ui::menu_open()) return 3;

    press(VK_SHIFT, 0x36, true, false);            // 4: auto-repeat must not close
    if (!ui::menu_open()) return 4;

    press(VK_SHIFT, 0x36, false, true);            // 5: release, then press again
    press(VK_SHIFT, 0x36, false, false);           //    closes
    if (ui::menu_open()) return 5;

    ui::close_menu();
    return 0;
}

// The world -> screen maths ESP depends on, checked against points whose answer
// is known by hand. A camera at the origin faces +Z at yaw 0; because yaw grows
// clockwise, the camera's right is -X, so a point at -X must land right of
// centre. Returns 0 on success, otherwise the index of the first failed case.
int check_world_math() {
    render::Camera cam;
    cam.pos = render::Vec3{ 0.0, 0.0, 0.0 };
    cam.yaw = 0.0f;
    cam.pitch = 0.0f;
    cam.fov_y = 70.0f;
    const float w = 800.0f, h = 500.0f;

    float sx = 0.0f, sy = 0.0f;

    if (!render::project(cam, render::Vec3{ 0, 0, 10 }, w, h, &sx, &sy)) return 1;
    if (std::fabs(sx - w * 0.5f) > 0.5f || std::fabs(sy - h * 0.5f) > 0.5f) return 2;

    if (!render::project(cam, render::Vec3{ -1, 0, 10 }, w, h, &sx, &sy)) return 3;
    if (sx <= w * 0.5f) return 4;

    if (!render::project(cam, render::Vec3{ 0, 1, 10 }, w, h, &sx, &sy)) return 5;
    if (sy >= h * 0.5f) return 6;

    if (render::project(cam, render::Vec3{ 0, 0, -10 }, w, h, &sx, &sy)) return 7;

    const render::Rect2 r = render::project_box(
        cam, render::Vec3{ -0.3, 0.0, 9.7 }, render::Vec3{ 0.3, 1.8, 10.3 }, w, h);
    if (!r.visible) return 8;
    if (!(r.x1 > r.x0 && r.y1 > r.y0)) return 9;
    // a 1.8 block tall box ten blocks away cannot cover the screen
    if ((r.y1 - r.y0) > h * 0.5f) return 10;

    float yaw = 0.0f, pitch = 0.0f;
    render::aim_angles(render::Vec3{ 0, 1, 0 }, render::Vec3{ -10, 1, 0 }, &yaw, &pitch);
    if (std::fabs(yaw - 90.0f) > 0.01f) return 11;
    if (std::fabs(pitch) > 0.01f) return 12;

    render::aim_angles(render::Vec3{ 0, 0, 0 }, render::Vec3{ 0, 5, 10 }, &yaw, &pitch);
    if (std::fabs(yaw) > 0.01f) return 13;
    if (!(pitch < -20.0f && pitch > -40.0f)) return 14;

    // 170 -> -170 must go forwards through 180, not backwards through 0
    if (std::fabs(render::approach_angle(170.0f, -170.0f, 5.0f) - 175.0f) > 0.01f)
        return 15;
    return 0;
}

// Clicks a point in the menu, through the real overlay frame. The module rows
// are InvisibleButtons, so a click only lands when hover, capture and the item
// rect all agree -- which is exactly what was broken when the menu opened but
// nothing could be toggled. Synthetic ImGui input events go in before the
// frame, the way the window procedure would deliver them.
//
// Also covers the two mouse buttons disagreeing on purpose: a left click flips
// the module, a right click opens its settings, and neither may do the other's
// job -- the settings used to hang off a 22px chevron button of their own.
int check_row_click(GLWindow& win) {
    const ui::FrameLayout lo = ui::last_layout();
    if (!lo.valid) return 1;

    auto& combat = g_modules.in(Category::Combat);
    if (combat.empty()) return 2;
    Module* m = combat[0];        // first row of the first tab

    // A click is delivered the way Windows delivers it, so this covers the whole
    // chain the user actually pulls: the window procedure, the ImGui backend and
    // the widget hit test -- not just ImGui's own event queue.
    // ImGui trickles queued input events (io.ConfigInputTrickleEventQueue), so a
    // press landing on the same frame as the move that positions the cursor is
    // not guaranteed. Hold the button down across several frames, the way a real
    // click does, then release it the same way.
    // `switch_held`, when asked for, reports whether the first row's switch (and
    // not the row behind it) owned the press during the held frames.
    auto click_at = [&](float x, float y, bool right = false,
                        bool* switch_held = nullptr) {
        const LPARAM lp = MAKELPARAM((int)x, (int)y);
        const UINT down = right ? WM_RBUTTONDOWN : WM_LBUTTONDOWN;
        const UINT up   = right ? WM_RBUTTONUP   : WM_LBUTTONUP;
        SendMessageW(win.wnd, WM_MOUSEMOVE, 0, lp);
        SendMessageW(win.wnd, down, right ? MK_RBUTTON : MK_LBUTTON, lp);
        for (int i = 0; i < 3; ++i) overlay::frame(win.dc);
        if (switch_held) *switch_held = ui::last_layout().switch_active;
        FC_INFO("selftest", "  %s press at (%.0f,%.0f): mouse=(%.0f,%.0f) down=(%d,%d) "
                            "out_of_window=%d hovered=%s active=%u",
                right ? "right" : "left",
                x, y, ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y,
                ImGui::GetIO().MouseDown[0] ? 1 : 0, ImGui::GetIO().MouseDown[1] ? 1 : 0,
                ImGui::IsMousePosValid() ? 0 : 1,
                GImGui->HoveredWindow ? GImGui->HoveredWindow->Name : "(none)",
                (unsigned)GImGui->ActiveId);
        SendMessageW(win.wnd, up, 0, lp);
        for (int i = 0; i < 3; ++i) overlay::frame(win.dc);
    };

    const bool was = m->enabled;
    const float swx = (lo.switch_min.x + lo.switch_max.x) * 0.5f;
    const float swy = (lo.switch_min.y + lo.switch_max.y) * 0.5f;
    const float bodyx = (lo.row_min.x + 60.0f + lo.switch_min.x) * 0.5f;
    const float bodyy = (lo.row_min.y + lo.row_max.y) * 0.5f;
    FC_INFO("selftest", "click probe: row=(%.0f,%.0f)-(%.0f,%.0f) body=(%.0f,%.0f) "
                         "switch=(%.0f,%.0f)",
            lo.row_min.x, lo.row_min.y, lo.row_max.x, lo.row_max.y,
            bodyx, bodyy, swx, swy);

    // The switch is the hit target that existed first, so check it first: if it
    // does not work either, the problem is the input path, not the row logic.
    //
    // Toggling is not enough on its own: a click at the switch position also
    // lands on the row, whose own path flips the module, so "the module toggled"
    // would pass even with the switch completely covered. The switch has to be
    // the widget that took the press, which is what `switch_held` reads.
    bool switch_held = false;
    click_at(swx, swy, false, &switch_held);
    if (m->enabled == was) return 5;
    if (!switch_held) return 12;         // the row swallowed the switch's click
    click_at(swx, swy, false, &switch_held);
    if (m->enabled != was) return 6;
    if (!switch_held) return 13;

    // Clicking the row body must toggle it too, exactly once per click.
    click_at(bodyx, bodyy);
    if (m->enabled == was) return 3;
    click_at(bodyx, bodyy);
    if (m->enabled != was) return 4;

    // The right button on the same card opens the settings and nothing else.
    // Come back to a collapsed row first so the two clicks below mean the same
    // thing whatever state the menu opened in.
    if (m->expanded) {
        click_at(bodyx, bodyy, true);
        if (m->expanded) return 10;
    }
    click_at(bodyx, bodyy, true);
    if (!m->expanded) return 7;          // right click did not open the settings
    if (m->enabled != was) return 8;     // it flipped the module as well
    click_at(bodyx, bodyy, true);
    if (m->expanded) return 9;           // and the second one did not close them
    if (m->enabled != was) return 11;    // closing them flipped the module too

    // Park the cursor off the window so the later checks see no hover.
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    overlay::frame(win.dc);
    return 0;
}

// --- catalog wiring ---------------------------------------------------------
// The catalog is the contract between the UI and the behaviour: every module
// either has something attached or says why it does not. Before this check
// existed the client shipped 30 rows of which a handful did anything, and neither
// the UI nor the log said which -- which is exactly how "nothing works" happens.
int check_catalog_wiring() {
    // Behaviour that lives in the client's own UI (the HUD and the menu) instead
    // of in a per-frame callback.
    const char* kUiDriven[] = { "Watermark", "ArrayList", "Blur", "Animations",
                                "Theme" };
    auto ui_driven = [&](const std::string& name) {
        for (const char* n : kUiDriven) if (name == n) return true;
        return false;
    };

    int callbacks = 0;
    for (const auto& m : g_modules.all()) {
        const bool has_cb = (bool)(m.on_enable || m.on_disable || m.on_tick ||
                                   m.on_render);
        if (has_cb) ++callbacks;

        if (m.name.empty() || m.desc.empty()) return 1;
        if (m.todo && has_cb) return 2;                 // says WIP but has behaviour
        if (!m.implemented && has_cb) return 3;         // behaviour nobody declared
        if (m.implemented && !has_cb && !ui_driven(m.name)) return 4;
        if (!m.implemented && !m.todo) return 5;        // silent placeholder
        if (m.todo && !*m.todo) return 6;
    }

    const int ui = (int)(sizeof(kUiDriven) / sizeof(kUiDriven[0]));
    if (g_modules.count() - unimplemented_count() != callbacks + ui) return 7;

    // The modules the user is being asked to trust, and what each must own.
    struct Want { const char* name; bool on, off, tick, render; };
    static const Want kWant[] = {
        { "ESP",         false, false, false, true  },
        { "Aim Assist",  false, false, true,  false },
        { "Nametags",    false, false, false, true  },
        { "Tracers",     false, false, false, true  },
        { "Fullbright",  true,  true,  false, false },
        { "Zoom",        true,  true,  false, false },
        { "Auto Jump",   true,  true,  false, false },
        { "Sprint",      false, true,  true,  false },
        { "Anti AFK",    false, false, true,  false },
        // Combat. Auto Clicker holds a key, so it has to give it back; Reach
        // writes an attribute, so it has to be able to restore it; Velocity is a
        // pure per-frame write and owns nothing, and Triggerbot only ever calls
        // the game's click, so it owns nothing either.
        { "Auto Clicker", false, true,  true,  false },
        { "Triggerbot",   false, false, true,  false },
        { "Velocity",     false, false, true,  false },
        { "Reach",        true,  true,  true,  false },
        // Criticals holds the jump key, so it gives it back on the way out. Step,
        // Speed and Fly each write something the game owns (an attribute, or the
        // abilities flags) and therefore restore it. Fast Place only writes
        // MinecraftClient's cooldown every frame and owns nothing.
        { "Criticals",    false, true,  true,  false },
        { "Step",         true,  true,  true,  false },
        { "Speed",        true,  true,  true,  false },
        { "Fly",          true,  true,  true,  false },
        { "Fast Place",   false, false, true,  false },
        // Auto Totem performs its action through the game's own inventory click,
        // which has already happened by the time the call returns, so it holds
        // nothing and has nothing to give back on the way out.
        { "Auto Totem",   false, false, true,  false },
    };
    for (const Want& w : kWant) {
        Module* m = g_modules.find(w.name);
        if (!m || !m->implemented) return 10;
        if (!!m->on_enable != w.on || !!m->on_disable != w.off ||
            !!m->on_tick != w.tick || !!m->on_render != w.render)
            return 11;
    }

    // The implemented set is a documented list (README's module table), so check
    // the names themselves rather than only the count: adding or dropping one
    // silently is how the two drift apart.
    static const char* kImplemented[] = {
        "Aim Assist", "Auto Clicker", "Triggerbot", "Velocity", "Reach", "Criticals",
        "Sprint", "Auto Jump", "Step", "Speed", "Fly", "Anti AFK",
        "Fullbright", "Zoom", "ESP", "Nametags", "Tracers", "Fast Place",
        "Auto Totem",
        "Watermark", "ArrayList", "Blur", "Animations", "Theme",
    };
    for (const char* name : kImplemented) {
        Module* m = g_modules.find(name);
        if (!m) return 20;
        if (!m->implemented) return 21;
        if (m->todo) return 22;
    }
    for (const auto& m : g_modules.all()) {
        const bool expected = std::any_of(
            std::begin(kImplemented), std::end(kImplemented),
            [&](const char* n) { return m.name == n; });
        if (m.implemented != expected) return 23;
    }

    // Enabling a placeholder has to say so out loud. The switch still flips -- the
    // row exists and its state is honest -- but the log line is the half of the
    // contract that stops "nothing happened" from being a mystery, so flipping one
    // here puts it in every self-test log.
    Module* placeholder = nullptr;
    for (auto& m : g_modules.all())
        if (!m.implemented) { placeholder = &m; break; }
    if (!placeholder) return 12;
    g_modules.set_enabled(nullptr, *placeholder, true);
    if (!placeholder->enabled) return 13;
    g_modules.set_enabled(nullptr, *placeholder, false);
    if (placeholder->enabled) return 14;
    return 0;
}

// The handful of pure functions the new modules added. They are the parts of a
// module that can be wrong without the game being present at all.
int check_pure_helpers() {
    if (visuals::nametag_text("Notch", 20.0f, 20.0f, 12.4f, true, true) !=
        "Notch   20/20 hp   12 m") return 1;
    if (visuals::nametag_text("Notch", 8.0f, 0.0f, 3.0f, true, false) !=
        "Notch   8 hp") return 2;
    if (visuals::nametag_text("Notch", 20.0f, 20.0f, 3.0f, false, true) !=
        "Notch   3 m") return 3;
    if (visuals::nametag_text("", 20.0f, 20.0f, 0.0f, false, true) != "0 m")
        return 4;
    if (!visuals::nametag_text("", 0.0f, 0.0f, 0.0f, false, false).empty())
        return 5;

    // Two degrees, alternating with the clock, so the view comes back to where it
    // started instead of creeping away.
    if (std::fabs(visuals::afk_nudge(0.0f, 0.25) - 2.0f) > 0.001f) return 6;
    if (std::fabs(visuals::afk_nudge(0.0f, 1.25) + 2.0f) > 0.001f) return 7;
    // ...and it wraps instead of running past the yaw range.
    if (std::fabs(visuals::afk_nudge(179.0f, 0.25) + 179.0f) > 0.001f) return 8;
    if (std::fabs(visuals::afk_nudge(-179.0f, 1.25) - 179.0f) > 0.001f) return 9;

    if (std::fabs(render::normalize_yaw(190.0f) + 170.0f) > 0.001f) return 10;
    if (std::fabs(render::normalize_yaw(-190.0f) - 170.0f) > 0.001f) return 11;
    if (std::fabs(render::normalize_yaw(180.0f) - 180.0f) > 0.001f) return 12;
    if (std::fabs(render::normalize_yaw(540.0f) - 180.0f) > 0.001f) return 13;
    // -540 and +180 are the same direction: the range is (-180, 180].
    if (std::fabs(render::normalize_yaw(-540.0f) - 180.0f) > 0.001f) return 14;

    // Auto Clicker: a half-duty pulse at the requested rate, with rates outside
    // the client's 20 ticks a second clamped rather than turning into a period
    // that cannot happen.
    if (!visuals::click_pulse(0.00, 10.0))   return 15;   // first half of the period
    if (visuals::click_pulse(0.06, 10.0))    return 16;   // second half
    if (!visuals::click_pulse(0.14, 10.0))   return 17;   // period wraps
    if (!visuals::click_pulse(0.00, 0.0))    return 18;   // clamped up to 1 cps
    if (visuals::click_pulse(0.93, 1000.0))  return 19;   // clamped down to 20 cps
    if (visuals::click_pulse(std::nan(""), 10.0)) return 20;

    // Velocity: the damping window is the few ticks after a hit, and a request
    // that would change nothing leaves the player alone instead of writing the
    // same velocity back every frame.
    if (visuals::knockback_scale(0, 10, 0.0f, 0.0f).active)  return 21;  // not hit
    if (!visuals::knockback_scale(10, 10, 0.0f, 0.0f).active) return 22; // hit tick
    if (!visuals::knockback_scale(6, 10, 0.5f, 0.25f).active) return 23;
    if (visuals::knockback_scale(5, 10, 0.0f, 0.0f).active)  return 24;  // just outside
    if (visuals::knockback_scale(-1, 10, 0.0f, 0.0f).active) return 25;
    if (visuals::knockback_scale(10, 0, 0.0f, 0.0f).active)  return 26;  // no counter
    if (visuals::knockback_scale(10, 10, 1.0f, 1.0f).active) return 27;  // no-op

    const visuals::KnockbackScale clamped =
        visuals::knockback_scale(10, 10, -3.0f, 7.0f);
    if (!clamped.active || clamped.horizontal != 0.0f || clamped.vertical != 1.0f)
        return 28;
    const visuals::KnockbackScale half = visuals::knockback_scale(9, 10, 0.5f, 0.25f);
    if (!half.active || half.horizontal != 0.5f || half.vertical != 0.25f) return 29;

    // Triggerbot: who may be clicked on, and when the click goes out.
    if (visuals::triggerbot_accepts(false, false, true, true))  return 30;  // a mob
    if (visuals::triggerbot_accepts(true, true, true, true))    return 31;  // unseen
    if (!visuals::triggerbot_accepts(true, true, true, false))  return 32;
    if (!visuals::triggerbot_accepts(false, false, false, true)) return 33;
    if (visuals::triggerbot_accepts(false, true, false, true))  return 34;

    // Nothing is clicked without a target, and nothing is clicked while the game
    // would drop the click.
    if (visuals::triggerbot_fire(false, true, 10.0, 10.0, 0.0, 0.0))  return 35;
    if (visuals::triggerbot_fire(true, false, 10.0, 10.0, 0.0, 0.0))  return 36;
    // No delay means no delay on either clock.
    if (!visuals::triggerbot_fire(true, true, 0.0, 0.0, 0.0, 0.0))    return 37;
    // 90 ms of reaction and 100 ms between clicks, exactly: the clock has to have
    // reached the wait, not merely be near it.
    if (visuals::triggerbot_fire(true, true, 0.089, 1.0, 0.09, 0.1))  return 38;
    if (!visuals::triggerbot_fire(true, true, 0.09, 1.0, 0.09, 0.1))  return 39;
    if (visuals::triggerbot_fire(true, true, 1.0, 0.099, 0.09, 0.1))  return 40;
    if (!visuals::triggerbot_fire(true, true, 1.0, 0.1, 0.09, 0.1))   return 41;
    // A delay that is not a positive number is no delay -- including a slider that
    // somehow reads NaN, which must not silence the module for good.
    if (!visuals::triggerbot_fire(true, true, 0.0, 0.0, -1.0, -1.0))  return 42;
    if (!visuals::triggerbot_fire(true, true, 0.0, 0.0, std::nan(""), 0.0))
        return 43;
    // ...but a *clock* that reads NaN has not reached a real wait.
    if (visuals::triggerbot_fire(true, true, std::nan(""), 1.0, 0.09, 0.0))
        return 44;
    if (visuals::triggerbot_fire(true, true, 1.0, std::nan(""), 0.0, 0.1))
        return 45;

    // Auto Totem: the swap goes out only while the offhand is holding something
    // that is not a totem, and only once the module's own delay has elapsed.
    if (visuals::totem_swap_due(true, 1.0, 0.0))   return 46;  // already holding one
    if (visuals::totem_swap_due(true, 0.0, 1.0))   return 47;
    if (!visuals::totem_swap_due(false, 0.0, 0.0)) return 48;  // no delay is no delay
    // The default 100 ms delay, on both sides of the boundary: the clock has to
    // have reached the wait, not be near it.
    if (visuals::totem_swap_due(false, 0.099, 0.1)) return 49;
    if (!visuals::totem_swap_due(false, 0.1, 0.1))  return 50;
    // The same two conventions the triggerbot's clocks use: a delay that is not a
    // positive number is no delay, and a clock that reads NaN has not reached one.
    if (!visuals::totem_swap_due(false, 0.0, -1.0))  return 51;
    if (!visuals::totem_swap_due(false, 0.0, std::nan(""))) return 52;
    if (visuals::totem_swap_due(false, std::nan(""), 0.1))  return 53;

    // Auto Totem's log line: a *changed* reason is worth one, the same reason is
    // not, and "no reason" -- what a successful swap passes -- is not a reason at
    // all. Case 55 is the crash this module shipped with, written as the test it
    // should have been: the code compared with strcmp(said, what), msvcrt's strcmp
    // dereferences the null, and the game died on the render thread the second
    // time the module acted. Here it is a plain call, so the old code does not
    // pass by luck -- it takes the selftest down with it.
    if (!visuals::reason_is_new(nullptr, "the offhand holds a totem")) return 54;
    if (visuals::reason_is_new("the offhand holds a totem", nullptr))    return 55;
    if (visuals::reason_is_new("the offhand holds a totem",
                               "the offhand holds a totem"))            return 56;
    if (!visuals::reason_is_new("the offhand holds a totem",
                                "no totem in the player's inventory")) return 57;
    return 0;
}

// Pink, as the palette is meant to read: red leads the other two channels, and
// the blue channel leads green. Violet (139,92,246) fails the first test, cyan
// (34,211,238) fails both, so this is not a test any of the other presets pass by
// accident.
bool is_pink(ImU32 c) {
    const ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    return v.x > 0.75f && v.x >= v.z && v.z > v.y;
}

// The three Client modules whose behaviour lives in the UI rather than in a
// callback. Each has to change what the client actually draws, or its setting is
// decoration -- which is what the whole catalog used to be. Runs with the menu
// open, so the blur has something to build. Returns 0 on success.
int check_ui_modules(GLWindow& win) {
    Module* blur  = g_modules.find("Blur");
    Module* theme_mod = g_modules.find("Theme");
    Module* anims = g_modules.find("Animations");
    if (!blur || !theme_mod || !anims) return 1;
    if (!blur->implemented || !theme_mod->implemented || !anims->implemented) return 2;

    const bool was_blur = blur->enabled;
    const bool was_theme = theme_mod->enabled;
    const bool was_anims = anims->enabled;

    auto frame = [&]() {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        overlay::frame(win.dc);
    };

    // --- Blur gates the frosted backdrop ------------------------------------
    blur->enabled = true;
    frame();
    const bool had_backdrop = ui::backdrop() != 0;
    blur->enabled = false;
    frame();
    const bool no_backdrop = ui::backdrop() == 0;

    // --- Theme drives the accent palette ------------------------------------
    Setting* accent = theme_mod->find("Accent");
    if (!accent) return 3;
    const int was_accent = accent->mode;
    theme_mod->enabled = true;
    accent->mode = 0;
    frame();
    const ImU32 first = theme::P.accent;          // Pink, the shipped default
    // "Pink" is a claim about the pixels, so it is checked as one: red leads,
    // and blue leads green, which the violet/cyan reference palette does not
    // satisfy. Everything the client draws pulls its colour from here, so a
    // palette that drifted back to violet would repaint the whole UI.
    const bool pink = is_pink(first) && is_pink(theme::P.accent2);
    accent->mode = 1;
    frame();
    // Read while mode 1 is still applied: the restore below puts mode 0 back, and
    // sampling after it would only ever report the first colour twice.
    const ImU32 second = theme::P.accent;
    const bool switched = second != first && !is_pink(second);

    // --- Animations is the menu's easing speed ------------------------------
    Setting* speed = anims->find("Speed");
    if (!speed) return 4;
    const float was_speed = speed->num;
    anims->enabled = false;
    const float fallback = ui::menu_anim_speed();
    anims->enabled = true;
    speed->num = 30.0f;
    const float from_module = ui::menu_anim_speed();

    // Put everything back the way it was before reporting.
    accent->mode = was_accent;
    theme_mod->enabled = was_theme;
    speed->num = was_speed;
    anims->enabled = was_anims;
    blur->enabled = was_blur;
    frame();

    const ImVec4 a0 = ImGui::ColorConvertU32ToFloat4(first);
    const ImVec4 a1 = ImGui::ColorConvertU32ToFloat4(second);
    FC_INFO("selftest", "shipped accent rgb(%d,%d,%d) %s; mode 1 is rgb(%d,%d,%d)",
            (int)(a0.x * 255.0f), (int)(a0.y * 255.0f), (int)(a0.z * 255.0f),
            pink ? "(pink)" : "(NOT pink)",
            (int)(a1.x * 255.0f), (int)(a1.y * 255.0f), (int)(a1.z * 255.0f));
    if (!pink) return 10;                            // the palette is not pink
    if (!had_backdrop) return 5;                     // Blur on drew no backdrop
    if (!no_backdrop) return 6;                      // Blur off kept the backdrop
    if (!switched) return 7;                         // the accent selector did nothing
    if (std::fabs(fallback - 20.0f) > 0.01f) return 8;      // default speed changed
    if (std::fabs(from_module - 30.0f) > 0.01f) return 9;   // slider did not reach it
    return 0;
}

// Reads the back buffer. ImGui draws top-down while glReadPixels hands back
// rows bottom-up, so every helper below flips the y axis.
void read_frame(std::vector<unsigned char>& px) {
    px.assign((size_t)kWidth * kHeight * 4, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
}

// Counts pixels the overlay actually wrote. The window is cleared to black
// first, so anything above that came out of ImGui -- through the blur, the
// shaders and the renderer. A broken GL path renders nothing and this catches
// it, which a "did init return true" check never would.
int lit_pixels(const std::vector<unsigned char>& px) {
    int lit = 0;
    for (size_t i = 0; i < px.size(); i += 4) {
        if (px[i] > 12 || px[i + 1] > 12 || px[i + 2] > 12) ++lit;
    }
    return lit;
}

// Counts near-white pixels inside an ImGui-space rect. The glass panel is a
// dark navy and every control on top of it carries bright text or a bright
// border, so a rect with no lit pixel means the widget layer was painted over.
int bright_pixels_in(const std::vector<unsigned char>& px, ImVec2 lo, ImVec2 hi,
                     unsigned char min_channel) {
    const int x0 = (int)(lo.x < 0 ? 0 : lo.x), x1 = (int)(hi.x < kWidth ? hi.x : kWidth);
    const int y0 = (int)(lo.y < 0 ? 0 : lo.y), y1 = (int)(hi.y < kHeight ? hi.y : kHeight);

    int lit = 0;
    for (int y = y0; y < y1; ++y) {
        const int row = kHeight - 1 - y;
        for (int x = x0; x < x1; ++x) {
            const unsigned char* p = &px[((size_t)row * kWidth + x) * 4];
            if (p[0] > min_channel || p[1] > min_channel || p[2] > min_channel) ++lit;
        }
    }
    return lit;
}

// The settings panel of the *last* row on a page. That is the layout that aborted
// the client in game: the panel is painted by hand and positioned with
// SetCursorScreenPos(), which grows nothing by itself, and ImGui escalates exactly
// that when it is the last thing a window does in a frame --
// "Code uses SetCursorPos()/SetCursorScreenPos() to extend window/parent
// boundaries. Please submit an item e.g. Dummy() afterwards". Only a panel on the
// last row leaves it as the frame's final act, which is why opening the settings
// of the row at the bottom of a long page was the crash.
//
// The right-click -> `expanded` gesture is covered by check_row_click; what is
// exercised here is the state that gesture produces on the last row, in both
// directions, plus the number that makes it safe: the content child has to report
// room for the panel, or neither the panel nor anything after it can be scrolled
// to. Returns 0 on success.
int check_last_row_panel(GLWindow& win) {
    auto& combat = g_modules.in(Category::Combat);
    if (combat.empty()) return 1;
    Module* last = combat.back();          // registration order == row order
    if (!last || last->settings.empty()) return 2;

    const bool was = last->expanded;
    auto render = [&](bool expanded) {
        last->expanded = expanded;
        for (int i = 0; i < 3; ++i) {
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            overlay::frame(win.dc);        // EndChild() checks the layout in here
            Sleep(16);
        }
        return ui::last_layout().content_h;
    };

    const float collapsed = render(false);
    const float expanded  = render(true);
    last->expanded = was;
    render(was);

    // The panel's drawn height by the menu's own rule (34px a setting plus its
    // padding), less a few pixels of slack so the check is about the reservation
    // and not about rounding.
    const float panel = (float)last->settings.size() * 34.0f + 20.0f;
    FC_INFO("selftest", "  %s: content reports %.0f px plain, %.0f px with its "
                         "settings open",
            last->name.c_str(), collapsed, expanded);

    if (collapsed <= 0.0f) return 3;                    // never reported at all
    if (expanded < collapsed + panel) return 4;         // the panel was not reserved
    return 0;
}

// The HUD modules are modules like any other, so their settings have to do
// something. The watermark's corner was stored and then ignored: the plate was
// always painted top-left no matter what the selector said. Returns 0 on success.
int check_hud_corner(GLWindow& win) {
    Module* watermark = g_modules.find("Watermark");
    if (!watermark) return 1;
    if (!watermark->implemented) return 2;
    Setting* corner = watermark->find("Corner");
    if (!corner) return 3;

    const bool was_on = watermark->enabled;
    const int  was_corner = corner->mode;
    watermark->enabled = true;

    const ImVec2 tl0(0, 0), tl1(300.0f, 60.0f);
    const ImVec2 br0((float)kWidth - 320.0f, (float)kHeight - 60.0f);
    const ImVec2 br1((float)kWidth, (float)kHeight);

    std::vector<unsigned char> px;
    auto ink_at = [&](int mode, ImVec2 lo, ImVec2 hi) {
        corner->mode = mode;
        for (int i = 0; i < 3; ++i) {
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            overlay::frame(win.dc);
        }
        read_frame(px);
        return bright_pixels_in(px, lo, hi, 120);
    };

    const int tl_ink      = ink_at(0, tl0, tl1);      // Corner = TL
    const int tl_stale_br = ink_at(0, br0, br1);
    const int br_ink      = ink_at(3, br0, br1);      // Corner = BR
    const int br_stale_tl = ink_at(3, tl0, tl1);

    corner->mode = was_corner;
    watermark->enabled = was_on;

    if (tl_ink < 20) return 4;          // nothing drawn at the top-left
    if (br_ink < 20) return 5;          // nothing drawn at the bottom-right
    if (br_stale_tl > 8) return 6;      // still painting at the old corner
    if (tl_stale_br > 8) return 7;      // ...or painting bottom-right by mistake
    return 0;
}

} // namespace

extern "C" __declspec(dllexport) int wiener_client_selftest() {
    FC_INFO("selftest", "starting overlay self-test");

    // The catalog is registered by the worker thread DllMain starts, so wait for
    // it rather than racing it: every check below reads g_modules.
    for (int i = 0; i < 200 && g_modules.count() == 0; ++i) Sleep(10);
    if (g_modules.count() == 0) {
        FC_ERROR("selftest", "the module catalog was never registered");
        return 14;
    }

    const int wiringfail = check_catalog_wiring();
    if (wiringfail) {
        FC_ERROR("selftest", "catalog wiring failed at case %d "
                              "(%d modules, %d placeholders)",
                 wiringfail, g_modules.count(), unimplemented_count());
        return 15;
    }
    FC_INFO("selftest", "catalog ok: %d modules, %d with behaviour, %d placeholders",
            g_modules.count(), g_modules.count() - unimplemented_count(),
            unimplemented_count());

    const int helperfail = check_pure_helpers();
    if (helperfail) {
        FC_ERROR("selftest", "module helper check failed at case %d", helperfail);
        return 16;
    }
    FC_INFO("selftest", "nametag text, anti-idle nudge, yaw wrapping, click pulse and "
                        "knockback window check out");

    GLWindow win;
    if (!make_gl_window(win)) {
        FC_ERROR("selftest", "window/WGL context creation failed (%lu)",
                 (unsigned long)GetLastError());
        return 1;
    }

    if (!overlay::init(win.dc)) {
        FC_ERROR("selftest", "overlay init failed");
        destroy_gl_window(win);
        return 2;
    }
    FC_INFO("selftest", "overlay up on GL %s", fc::gpu::gl_version());

    const int keyfail = check_toggle_key(win.wnd);
    if (keyfail) {
        FC_ERROR("selftest", "toggle-key check failed at case %d", keyfail);
        overlay::shutdown();
        destroy_gl_window(win);
        return 3;
    }
    FC_INFO("selftest", "toggle key is Right Shift (Insert and Left Shift inert)");

    const int mathfail = check_world_math();
    if (mathfail) {
        FC_ERROR("selftest", "world->screen maths failed at case %d", mathfail);
        overlay::shutdown();
        destroy_gl_window(win);
        return 9;
    }
    FC_INFO("selftest", "projection and aim angles check out");

    // Render with the menu open so the frame grab, the blur passes and every
    // widget code path execute. Realistic spacing matters: ImGui derives its
    // frame delta from the clock, and the open animation is time based.
    ui::open_menu();
    std::vector<unsigned char> px;
    for (int i = 0; i < 12; ++i) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        overlay::frame(win.dc);
        Sleep(16);
    }
    read_frame(px);
    const int lit = lit_pixels(px);

    // The controls must be visible *on top of* the glass panel. Everything the
    // menu draws as chrome lives on ImGui's background draw list and the
    // widgets on the window layer; if any of that ordering regresses the menu
    // still opens and still reports pixels, but the rail and the module rows
    // are hidden behind the panel -- which is just how it looked before.
    const ui::FrameLayout& layout = ui::last_layout();

    // Thresholds are picked just above what each control's own background can
    // reach, so these count glyph ink rather than a widget existing at all. The
    // plain panel is ~33/255 and the selected sidebar entry's translucent accent
    // gradient tops out around 170, while its label is white and the search
    // hint is 185 -- so a rect covered by the panel chrome scores zero.
    const int rail_ink = layout.valid
        ? bright_pixels_in(px, layout.rail_item_min, layout.rail_item_max, 200)
        : 0;
    const int search_ink = bright_pixels_in(px, layout.search_min, layout.search_max, 165);

    const bool blurred = ui::backdrop() != 0;

    // --- the modules that live in the UI -----------------------------------
    const int uifail = check_ui_modules(win);

    // --- clicking a row toggles its module ---------------------------------
    // Runs while the menu is still open, using the layout read back above.
    const int clickfail = check_row_click(win);

    // --- opening the settings of the row at the bottom of the page ---------
    const int panelfail = check_last_row_panel(win);
    if (panelfail) {
        FC_ERROR("selftest", "the last row's settings broke the content layout "
                              "(case %d)", panelfail);
        overlay::shutdown();
        destroy_gl_window(win);
        return 23;
    }
    FC_INFO("selftest", "the bottom row's settings fit the content region");

    // --- ESP, with no game to read -----------------------------------------
    // Let the open animation finish first: while it fades, the menu panel is
    // still painted over the world and would dim the boxes being counted.
    ui::close_menu();
    for (int i = 0; i < 20; ++i) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        overlay::frame(win.dc);
        Sleep(16);
    }

    Module* esp = nullptr;
    for (auto& m : g_modules.all())
        if (m.name == "ESP") esp = &m;

    render::Camera cam;
    cam.pos = render::Vec3{ 0.0, 0.0, 0.0 };
    cam.yaw = 0.0f;
    cam.pitch = 0.0f;
    cam.fov_y = 70.0f;

    visuals::Target dummy;
    dummy.lo = render::Vec3{ -0.3, 0.0, 9.7 };
    dummy.hi = render::Vec3{ 0.3, 1.8, 10.3 };
    dummy.centre = render::Vec3{ 0.0, 0.9, 10.0 };
    dummy.aim = render::Vec3{ 0.0, 1.5, 10.0 };
    dummy.health = 20.0f;
    dummy.max_health = 20.0f;
    dummy.distance = 10.0f;

    const render::Rect2 expected =
        render::project_box(cam, dummy.lo, dummy.hi, (float)kWidth, (float)kHeight);

    int esp_ink = -1;
    int esp_drawn = -1;
    int esp_idle_drawn = -1;
    int nametags_drawn = -1;
    int tracers_drawn = -1;
    int nametag_ink = -1;
    int idle_nametags = -1;
    int idle_tracers = -1;

    Module* nametags = g_modules.find("Nametags");
    Module* tracers  = g_modules.find("Tracers");

    if (esp && nametags && tracers) {
        dummy.name = "Tester";
        visuals::set_test_view(true, cam);
        visuals::set_test_targets(std::vector<visuals::Target>{ dummy });
        g_modules.set_enabled(nullptr, *esp, true);
        g_modules.set_enabled(nullptr, *nametags, true);
        g_modules.set_enabled(nullptr, *tracers, true);

        glClear(GL_COLOR_BUFFER_BIT);
        overlay::frame(win.dc);

        std::vector<unsigned char> esp_px;
        read_frame(esp_px);
        esp_drawn = visuals::last_drawn();
        nametags_drawn = visuals::last_nametags();
        tracers_drawn = visuals::last_tracers();
        if (expected.visible) {
            esp_ink = bright_pixels_in(esp_px, ImVec2(expected.x0, expected.y0),
                                       ImVec2(expected.x1, expected.y1), 120);
            // The name plate sits just above the box, so its ink belongs in the
            // band above the projection rather than inside it.
            nametag_ink = bright_pixels_in(esp_px,
                                           ImVec2(expected.x0 - 100.0f,
                                                  expected.y0 - 60.0f),
                                           ImVec2(expected.x1 + 100.0f,
                                                  expected.y0 - 2.0f), 120);
        }

        // Drop the synthetic view: with no game to read either, none of the three
        // may draw -- no invented players, tags or lines.
        visuals::set_test_view(false, cam);
        visuals::set_test_targets(std::vector<visuals::Target>{});
        glClear(GL_COLOR_BUFFER_BIT);
        overlay::frame(win.dc);
        esp_idle_drawn = visuals::last_drawn();
        idle_nametags = visuals::last_nametags();
        idle_tracers = visuals::last_tracers();

        g_modules.set_enabled(nullptr, *nametags, false);
        g_modules.set_enabled(nullptr, *tracers, false);
        g_modules.set_enabled(nullptr, *esp, false);
    }

    // Must run before the overlay is shut down: it needs live GL frames, which is
    // exactly the sort of ordering a test gets wrong on its own.
    const int hudfail = check_hud_corner(win);
    if (hudfail) {
        FC_ERROR("selftest", "the watermark corner setting had no effect (case %d)",
                 hudfail);
        overlay::shutdown();
        destroy_gl_window(win);
        return 20;
    }
    FC_INFO("selftest", "watermark follows its corner setting");

    const GLenum glerr = glGetError();
    const void* target = hooks::swap_target();

    overlay::shutdown();
    destroy_gl_window(win);

    const int total = kWidth * kHeight;
    const int percent = total ? (lit * 100) / total : 0;
    FC_INFO("selftest", "frame hook target=%p, %d%% of the window was drawn",
            target, percent);

    if (!blurred) {
        FC_ERROR("selftest", "glass blur produced no backdrop texture");
        return 4;
    }
    if (uifail) {
        FC_ERROR("selftest", "a UI module's setting had no effect, or the palette "
                              "is not the shipped pink (case %d)", uifail);
        return 22;
    }
    if (clickfail) {
        FC_ERROR("selftest", "the row mouse buttons did not behave (case %d)",
                 clickfail);
        return 13;
    }
    if (percent < 30) {
        FC_ERROR("selftest", "overlay drew almost nothing (%d%% of the window)",
                 percent);
        return 5;
    }
    if (!layout.valid) {
        FC_ERROR("selftest", "the menu laid out no sidebar entries");
        return 7;
    }
    if (rail_ink < 20 || search_ink < 20) {
        FC_ERROR("selftest",
                 "the widget layer is hidden behind the glass panel "
                 "(sidebar=%d, search=%d bright pixels)",
                 rail_ink, search_ink);
        return 8;
    }
    if (glerr != GL_NO_ERROR) {
        FC_ERROR("selftest", "GL error 0x%04X during overlay frames", (unsigned)glerr);
        return 6;
    }
    if (!esp) {
        FC_ERROR("selftest", "the ESP module is missing from the catalog");
        return 10;
    }
    if (!nametags || !tracers) {
        FC_ERROR("selftest", "Nametags or Tracers is missing from the catalog");
        return 21;
    }
    if (!expected.visible || esp_ink < 20 || esp_drawn != 1) {
        FC_ERROR("selftest",
                 "ESP did not draw its box (ink=%d pixels, drawn=%d, rect=%s)",
                 esp_ink, esp_drawn, expected.visible ? "on screen" : "off screen");
        return 11;
    }
    if (esp_idle_drawn != 0) {
        FC_ERROR("selftest", "ESP drew %d boxes with no game and no test view",
                 esp_idle_drawn);
        return 12;
    }
    if (nametags_drawn != 1 || nametag_ink < 20) {
        FC_ERROR("selftest", "Nametags drew %d plate(s) (%d ink pixels above the "
                              "box); expected one plate with text",
                 nametags_drawn, nametag_ink);
        return 17;
    }
    if (tracers_drawn != 1) {
        FC_ERROR("selftest", "Tracers drew %d line(s); expected one", tracers_drawn);
        return 18;
    }
    if (idle_nametags != 0 || idle_tracers != 0) {
        FC_ERROR("selftest", "with no game and no test view the HUD modules drew "
                              "%d plate(s) and %d line(s)",
                 idle_nametags, idle_tracers);
        return 19;
    }

    FC_INFO("selftest", "ESP drew %d box (%d ink pixels where the projection says "
                         "it should be), and nothing without a world",
            esp_drawn, esp_ink);
    FC_INFO("selftest", "Nametags drew %d plate (%d ink pixels) and Tracers %d line",
            nametags_drawn, nametag_ink, tracers_drawn);
    FC_INFO("selftest", "widgets sit on top of the glass (sidebar=%d, search=%d "
                         "bright pixels)",
            rail_ink, search_ink);
    FC_INFO("selftest", "a left click on a row and on its switch each toggle the "
                         "module exactly once, and a right click opens its settings "
                         "without touching it");
    FC_INFO("selftest", "overlay self-test complete (blur ok, %d%% drawn)", percent);
    return 0;
}
