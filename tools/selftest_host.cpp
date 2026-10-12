// Loads "Wiener Client.dll" and calls wiener_client_selftest(). Used to verify
// the injected payload without launching Minecraft.
#include <windows.h>
#include <cstdio>
#include <cstring>

using SelftestFn = int (*)();

namespace {

// The DLL next to this executable, unless argv[1] names another one. Resolving
// it from the executable's own directory means the check runs against the build
// in build/bin whatever the shell's working directory happens to be -- loading a
// stale copy from somewhere else would report a pass for code nobody ran.
const char* dll_next_to_us() {
    static char path[MAX_PATH] = {0};
    if (path[0]) return path;

    char exe[MAX_PATH] = {0};
    if (!GetModuleFileNameA(nullptr, exe, MAX_PATH)) return "Wiener Client.dll";
    char* slash = strrchr(exe, '\\');
    if (!slash) return "Wiener Client.dll";
    *(slash + 1) = 0;

    snprintf(path, sizeof(path), "%sWiener Client.dll", exe);
    return path;
}

} // namespace

int main(int argc, char** argv) {
    const char* dll = (argc > 1) ? argv[1] : dll_next_to_us();

    std::printf("loading %s\n", dll);
    HMODULE mod = LoadLibraryA(dll);
    if (!mod) {
        std::printf("LoadLibrary failed (%lu)\n", GetLastError());
        return 1;
    }

    auto fn = reinterpret_cast<SelftestFn>(GetProcAddress(mod, "wiener_client_selftest"));
    if (!fn) {
        std::printf("export wiener_client_selftest not found\n");
        return 2;
    }

    const int rc = fn();
    std::printf("wiener_client_selftest returned %d\n", rc);

    // DllMain kicks off its own worker thread. Let it finish (it installs the
    // swap hook) before this process starts tearing down, otherwise the crate
    // gets unmapped out from under it.
    Sleep(2500);

    FreeLibrary(mod);
    return rc;
}
