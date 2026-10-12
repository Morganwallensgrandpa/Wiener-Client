#include "client/core.h"
#include "client/hooks.h"
#include "jni/jvm.h"
#include "modules/module.h"
#include "common/brand.h"
#include "common/log.h"

#include <windows.h>
#include <vector>
#include <string>
#include <mutex>

namespace fc::core {

namespace {
bool g_started = false;
std::mutex g_start_mutex;
long g_frame = 0;

struct BindState { int key; bool down; };
std::vector<BindState> g_binds;

// The DLL's own path and file timestamp. "Is the game running the build I just
// made?" was unanswerable from the log, and answering it cost a round trip;
// __DATE__/__TIME__ alone would not fix that because it only moves when this
// file is recompiled.
void log_module_stamp() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&log_module_stamp), &self))
        return;

    wchar_t path[MAX_PATH] = {0};
    if (!GetModuleFileNameW(self, path, MAX_PATH)) return;

    const wchar_t* name = path;
    for (const wchar_t* c = path; *c; ++c) if (*c == L'\\' || *c == L'/') name = c + 1;
    char file[128] = {0};
    for (int i = 0; name[i] && i < 127; ++i) file[i] = (char)name[i];

    HANDLE h = CreateFileW(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        FC_INFO("core", "module %s (file time unavailable)", file);
        return;
    }

    FILETIME ft{};
    FILETIME local{};
    SYSTEMTIME st{};
    // Local time, so the stamp matches what Explorer shows for the file.
    if (GetFileTime(h, nullptr, nullptr, &ft) &&
        FileTimeToLocalFileTime(&ft, &local) &&
        FileTimeToSystemTime(&local, &st)) {
        FC_INFO("core", "module %s, written %04u-%02u-%02u %02u:%02u:%02u", file,
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    }
    CloseHandle(h);
}

void poll_keybinds(void* env) {
    for (auto& m : g_modules.all()) {
        if (!m.key) continue;
        const bool down = (GetAsyncKeyState(m.key) & 0x8000) != 0;
        BindState* st = nullptr;
        for (auto& b : g_binds) if (b.key == m.key) st = &b;
        if (!st) { g_binds.push_back({m.key, down}); continue; }
        if (down && !st->down) g_modules.toggle(env, m);
        st->down = down;
    }
}
} // namespace

void* env() { return jni::env(); }

void start() {
    // Called from the DllMain worker thread, but make it safe to call from
    // anywhere (and only once) in case a host or a test drives it too.
    std::lock_guard<std::mutex> lock(g_start_mutex);
    if (g_started) return;
    g_started = true;

    log_set_file(kClientLog);
    log_set_level(LogLevel::Debug);
    FC_INFO("core", "%s 1.0.0 booting for 1.21.11 (built %s %s)",
            kClientName, __DATE__, __TIME__);
    log_module_stamp();

    register_catalog();
    FC_INFO("core", "catalog ready: %d modules", g_modules.count());

    if (!hooks::install())
        FC_ERROR("core", "failed to install the frame hook");
}

void stop() {
    if (!g_started) return;
    hooks::uninstall();
    jni::detach();
    g_started = false;
}

void on_render_frame() {
    ++g_frame;

    if (!jni::ready()) {
        jni::attach();
    }
    if (jni::ready()) {
        // Re-reading the player/world is comparatively expensive; four times a
        // second is plenty for a HUD.
        if ((g_frame % 15) == 0) jni::refresh();
        // Reports which mappings resolve, and stops as soon as the answer is
        // complete. This is what turns "the module did nothing" into a line in
        // the log that names the member that did not resolve.
        if ((g_frame % 60) == 0) jni::self_check_on_change();
    }

    void* e = jni::env();
    g_modules.tick(e);
    poll_keybinds(e);
}

void draw_modules(float w, float h) {
    g_modules.render(jni::env(), w, h);
}

} // namespace fc::core
