/* Longshot Co-op loader: a winmm.dll beside bin\longshot.exe.

   Windows looks for winmm.dll beside the exe first (it is not one of the KnownDLLs), so the game loads this one. Every
   winmm export jumps to the real System32\winmm.dll (winmm_stubs.S, slots filled here), and once the process has finished
   loading its modules, a thread of ours loads the mod itself: coop\LongshotCoop.dll beside this file. Nothing of the game is
   changed on disk; deleting winmm.dll and the coop folder removes the mod.

   LONGSHOT_COOP=0 in the environment keeps the mod out (the game runs as shipped). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

#include "winmm_names.h"

void *lsc_real[LSC_WINMM_COUNT];   /* (the stubs jump through these: winmm_stubs.S) */

static HMODULE self_module;

/* (an export no one has a real one for: there is none in this Windows' winmm) */
static void missing_export(void) {
    MessageBoxA(NULL, "A winmm function this Windows does not have was called (Longshot Co-op loader).", "Longshot Co-op",
                MB_ICONERROR);
    ExitProcess(1);
}

static void resolve_real(void) {
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (!n || n > MAX_PATH - 16) return;
    wcscat(path, L"\\winmm.dll");
    HMODULE real = LoadLibraryW(path);
    for (int i = 0; i < LSC_WINMM_COUNT; i++) {
        FARPROC p = real ? GetProcAddress(real, lsc_winmm_names[i]) : NULL;
        lsc_real[i] = p ? (void *)p : (void *)missing_export;
    }
}

static DWORD WINAPI load_mod(LPVOID unused) {
    (void)unused;
    char env[8];
    DWORD e = GetEnvironmentVariableA("LONGSHOT_COOP", env, sizeof env);
    if (e && e < sizeof env && env[0] == '0') return 0;
    wchar_t path[MAX_PATH + 64];
    DWORD n = GetModuleFileNameW(self_module, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return 0;
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash) return 0;
    wcscpy(slash + 1, L"coop\\LongshotCoop.dll");
    /* (its own folder first for its dependencies) */
    HMODULE mod = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!mod) {
        DWORD err = GetLastError();
        if (err != ERROR_MOD_NOT_FOUND && err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) {
            char text[256];
            wsprintfA(text, "Longshot Co-op could not be loaded (error %lu). The game starts without it.", err);
            MessageBoxA(NULL, text, "Longshot Co-op", MB_ICONWARNING);
        }
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        self_module = inst;
        DisableThreadLibraryCalls(inst);
        resolve_real();
        /* (the mod is loaded off the loader lock: the thread starts once the process's DLLs are initialized) */
        HANDLE t = CreateThread(NULL, 0, load_mod, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
