// Longshot Co-op: the mod's start (loaded by the winmm.dll beside bin\longshot.exe, off the loader lock).
#include "coop.h"
#include "runtime.h"

namespace coop {
namespace {

DWORD WINAPI start(LPVOID) {
    log("Longshot Co-op starting (game folder %s)", narrow(game_dir()).c_str());
    if (!runtime_init()) {
        log("the mod stays off: the runtime is not one it knows");
        return 0;
    }
    // (the renderer's DLL is loaded by the runtime once its window is up: hooked as soon as it is there)
    for (int i = 0; i < 6000 && !GetModuleHandleW(L"longshot_vk.dll"); i++) Sleep(10);
    runtime_vk_init();
    input_window_hook();
    log("Longshot Co-op ready (overlay %s)", rt().vk_ready ? "on" : "off");
    return 0;
}

}  // namespace
}  // namespace coop

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        HANDLE t = CreateThread(nullptr, 0, coop::start, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
