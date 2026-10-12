#include <windows.h>
#include "client/core.h"
#include "common/log.h"

namespace {
DWORD WINAPI boot(LPVOID) {
    fc::core::start();
    return 0;
}
} // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(inst);
            // Never do work directly in DllMain: the loader lock is held. Hand
            // off to a worker thread instead.
            HANDLE t = CreateThread(nullptr, 0, boot, nullptr, 0, nullptr);
            if (t) CloseHandle(t);
            break;
        }
        case DLL_PROCESS_DETACH:
            // Best-effort cleanup; the process may be tearing down already.
            break;
        default:
            break;
    }
    return TRUE;
}
