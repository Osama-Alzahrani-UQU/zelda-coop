// Longshot Co-op Setup: installs the mod into a Longshot game folder (and removes it). One file: the mod's two DLLs ride in
// it as resources. The game's own files are never changed: the setup adds bin\winmm.dll (the loader) and
// bin\coop\LongshotCoop.dll, and takes away only those.
//
//   LongshotCoop-Setup.exe                                  the window
//   LongshotCoop-Setup.exe /silent /install <game folder>   (tests, scripts) exit 0 when done
//   LongshotCoop-Setup.exe /silent /uninstall <game folder> [/removesettings]
//
// Exit codes: 0 done, 1 bad arguments, 2 not a Longshot game folder, 3 the game is running, 4 a file could not be written.
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "resource.h"

#define MAX_GAMES 32

enum { EXIT_DONE = 0, EXIT_ARGS = 1, EXIT_NOT_GAME = 2, EXIT_RUNNING = 3, EXIT_WRITE = 4 };

static wchar_t g_games[MAX_GAMES][MAX_PATH];
static int g_game_count = 0;
static HWND g_list, g_status, g_remove_settings, g_install_btn, g_uninstall_btn;
static HFONT g_font, g_font_bold, g_font_title;
static UINT g_dpi = 96;

// ---------------------------------------------------------------- files and folders

static int file_exists(const wchar_t* path) {
    const DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int dir_exists(const wchar_t* path) {
    const DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void join(wchar_t* out, const wchar_t* a, const wchar_t* b) {
    _snwprintf(out, MAX_PATH, L"%ls\\%ls", a, b);
    out[MAX_PATH - 1] = 0;
}

static void trim_slash(wchar_t* p) {
    size_t n = wcslen(p);
    while (n > 3 && (p[n - 1] == L'\\' || p[n - 1] == L'/')) p[--n] = 0;
}

// a Longshot game build: bin\longshot.exe (or longshot.exe)
static int is_game(const wchar_t* folder) {
    wchar_t p[MAX_PATH];
    join(p, folder, L"bin\\longshot.exe");
    if (file_exists(p)) return 1;
    join(p, folder, L"longshot.exe");
    if (file_exists(p)) return 1;
    return 0;
}

// a folder the player picked: the game folder itself, its bin folder, or a launcher data folder with one build
static int game_from_pick(const wchar_t* picked, wchar_t* out) {
    wchar_t p[MAX_PATH];
    wcsncpy(p, picked, MAX_PATH - 1);
    p[MAX_PATH - 1] = 0;
    trim_slash(p);
    if (is_game(p)) return wcscpy(out, p), 1;
    // (its bin folder)
    wchar_t* slash = wcsrchr(p, L'\\');
    if (slash && !_wcsicmp(slash + 1, L"bin")) {
        *slash = 0;
        if (is_game(p)) return wcscpy(out, p), 1;
        *slash = L'\\';
    }
    // (a folder of builds: the first one)
    wchar_t pattern[MAX_PATH];
    join(pattern, p, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int found = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        wchar_t sub[MAX_PATH];
        join(sub, p, fd.cFileName);
        if (is_game(sub)) found = 1, wcscpy(out, sub);
    } while (!found && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

static void add_game(const wchar_t* folder) {
    wchar_t full[MAX_PATH];
    if (!GetFullPathNameW(folder, MAX_PATH, full, NULL)) return;
    trim_slash(full);
    if (!is_game(full) || g_game_count >= MAX_GAMES) return;
    for (int i = 0; i < g_game_count; i++)
        if (!_wcsicmp(g_games[i], full)) return;
    wcscpy(g_games[g_game_count++], full);
}

static wchar_t* read_text(const wchar_t* path, size_t* chars) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(f, &size) || size.QuadPart > 4 * 1024 * 1024) {
        CloseHandle(f);
        return NULL;
    }
    char* bytes = (char*)malloc((size_t)size.QuadPart + 1);
    DWORD got = 0;
    if (!bytes || !ReadFile(f, bytes, (DWORD)size.QuadPart, &got, NULL)) {
        free(bytes);
        CloseHandle(f);
        return NULL;
    }
    CloseHandle(f);
    bytes[got] = 0;
    const char* start = bytes;
    if (got >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF) start += 3;
    const int n = MultiByteToWideChar(CP_UTF8, 0, start, -1, NULL, 0);
    wchar_t* text = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)(n + 1));
    if (text) MultiByteToWideChar(CP_UTF8, 0, start, -1, text, n);
    free(bytes);
    if (chars && text) *chars = wcslen(text);
    return text;
}

// every "key": "string" value of a JSON text (the launcher's builds.json and settings.json: flat enough for this)
static void json_strings(const wchar_t* text, const wchar_t* key, void (*each)(const wchar_t* value)) {
    wchar_t quoted[64];
    _snwprintf(quoted, 64, L"\"%ls\"", key);
    const wchar_t* p = text;
    while ((p = wcsstr(p, quoted)) != NULL) {
        p += wcslen(quoted);
        while (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n') p++;
        if (*p != L':') continue;
        p++;
        while (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n') p++;
        if (*p != L'"') continue;
        p++;
        wchar_t value[MAX_PATH];
        size_t n = 0;
        while (*p && *p != L'"' && n < MAX_PATH - 1) {
            if (*p == L'\\' && p[1]) {
                p++;
                if (*p == L'n') value[n++] = L'\n';
                else if (*p == L't') value[n++] = L'\t';
                else if (*p == L'u' && p[1] && p[2] && p[3] && p[4]) {
                    wchar_t hex[5] = {p[1], p[2], p[3], p[4], 0};
                    value[n++] = (wchar_t)wcstoul(hex, NULL, 16);
                    p += 4;
                } else value[n++] = *p;
                p++;
            } else {
                value[n++] = *p++;
            }
        }
        value[n] = 0;
        if (n) each(value);
    }
}

static void each_build_folder(const wchar_t* folder) { add_game(folder); }

static void scan_builds_dir(const wchar_t* builds) {
    wchar_t pattern[MAX_PATH];
    join(pattern, builds, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        wchar_t sub[MAX_PATH];
        join(sub, builds, fd.cFileName);
        add_game(sub);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void each_builds_setting(const wchar_t* folder) { scan_builds_dir(folder); }

// a launcher data folder: its builds.json, its builds folder, and the builds folder its settings name
static void scan_data_dir(const wchar_t* data) {
    if (!dir_exists(data)) return;
    wchar_t p[MAX_PATH];
    join(p, data, L"builds.json");
    wchar_t* text = read_text(p, NULL);
    if (text) {
        json_strings(text, L"folder", each_build_folder);
        free(text);
    }
    join(p, data, L"settings.json");
    text = read_text(p, NULL);
    if (text) {
        json_strings(text, L"buildsFolder", each_builds_setting);
        free(text);
    }
    join(p, data, L"builds");
    scan_builds_dir(p);
}

// the launcher's portable copies: a LongshotLauncher-data folder beside LongshotLauncher*.exe in a folder
static void scan_folder_for_launchers(const wchar_t* dir) {
    if (!dir_exists(dir)) return;
    wchar_t p[MAX_PATH];
    join(p, dir, L"LongshotLauncher-data");
    scan_data_dir(p);
    wchar_t pattern[MAX_PATH];
    join(pattern, dir, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {   // (one level down: Downloads\LongshotLauncher\LongshotLauncher-data, Games\Longshot\...)
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        if (_wcsnicmp(fd.cFileName, L"Longshot", 8) != 0) continue;
        wchar_t sub[MAX_PATH], data[MAX_PATH];
        join(sub, dir, fd.cFileName);
        join(data, sub, L"LongshotLauncher-data");
        scan_data_dir(data);
        if (!_wcsicmp(fd.cFileName, L"LongshotLauncher-data")) scan_data_dir(sub);
        join(data, sub, L"builds");
        scan_builds_dir(data);
        add_game(sub);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void find_games(void) {
    g_game_count = 0;
    wchar_t base[MAX_PATH], p[MAX_PATH];
    const wchar_t* homes[] = {L"Downloads", L"Desktop", L"Documents", L"Games", L""};
    if (GetEnvironmentVariableW(L"USERPROFILE", base, MAX_PATH)) {
        for (int i = 0; i < 5; i++) {
            if (homes[i][0]) join(p, base, homes[i]);
            else wcscpy(p, base);
            scan_folder_for_launchers(p);
        }
        join(p, base, L"Games\\Longshot");
        scan_builds_dir(p);
        add_game(p);
    }
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) {
        join(p, base, L"LongshotLauncher");
        scan_data_dir(p);
        join(p, base, L"LongshotLauncher\\LongshotLauncher-data");
        scan_data_dir(p);
    }
    if (GetEnvironmentVariableW(L"APPDATA", base, MAX_PATH)) {
        join(p, base, L"LongshotLauncher");
        scan_data_dir(p);
    }
    // (beside this setup, and the folders above it)
    wchar_t self[MAX_PATH];
    if (GetModuleFileNameW(NULL, self, MAX_PATH)) {
        for (int up = 0; up < 3; up++) {
            wchar_t* slash = wcsrchr(self, L'\\');
            if (!slash) break;
            *slash = 0;
            scan_folder_for_launchers(self);
            add_game(self);
        }
    }
    // (the drives' roots: D:\Games\Longshot ...)
    for (wchar_t drive = L'C'; drive <= L'Z'; drive++) {
        wchar_t root[8] = {drive, L':', L'\\', 0};
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        root[2] = 0;
        join(p, root, L"Games");
        scan_folder_for_launchers(p);
        scan_folder_for_launchers(root);
    }
}

// ---------------------------------------------------------------- the payload

static const void* payload(int id, DWORD* size) {
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
    if (!r) return NULL;
    HGLOBAL g = LoadResource(NULL, r);
    *size = SizeofResource(NULL, r);
    return g ? LockResource(g) : NULL;
}

// a file of ours: the loader names "LONGSHOT_COOP", the mod its log "coop_log"
static int file_has(const wchar_t* path, const char* marker, int wide) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER size;
    int found = 0;
    if (GetFileSizeEx(f, &size) && size.QuadPart < 64 * 1024 * 1024) {
        char* bytes = (char*)malloc((size_t)size.QuadPart);
        DWORD got = 0;
        if (bytes && ReadFile(f, bytes, (DWORD)size.QuadPart, &got, NULL)) {
            char pat[128];
            size_t n = 0;
            for (const char* m = marker; *m && n + 2 < sizeof pat; m++) {
                pat[n++] = *m;
                if (wide) pat[n++] = 0;
            }
            for (DWORD i = 0; i + n <= got && !found; i++)
                if (!memcmp(bytes + i, pat, n)) found = 1;
        }
        free(bytes);
    }
    CloseHandle(f);
    return found;
}

static int write_payload(int id, const wchar_t* path) {
    DWORD size = 0;
    const void* data = payload(id, &size);
    if (!data || !size) return 0;
    wchar_t tmp[MAX_PATH];
    _snwprintf(tmp, MAX_PATH, L"%ls.new", path);
    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD put = 0;
    const BOOL ok = WriteFile(f, data, size, &put, NULL) && put == size && FlushFileBuffers(f);
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp);
        return 0;
    }
    return 1;
}

// the game running from this folder (its longshot.exe or bin\longshot.exe)
static int game_running(const wchar_t* folder) {
    wchar_t exe[MAX_PATH], exe2[MAX_PATH];
    join(exe, folder, L"bin\\longshot.exe");
    join(exe2, folder, L"longshot.exe");
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof pe;
    int running = 0;
    for (BOOL more = Process32FirstW(snap, &pe); more && !running; more = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"longshot.exe")) continue;
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!p) {
            running = 1;   // (one we cannot look into: better safe)
            continue;
        }
        wchar_t path[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, path, &n)) {
            if (!_wcsicmp(path, exe) || !_wcsicmp(path, exe2)) running = 1;
        }
        CloseHandle(p);
    }
    CloseHandle(snap);
    return running;
}

static int installed(const wchar_t* folder) {
    wchar_t p[MAX_PATH];
    join(p, folder, L"bin\\coop\\LongshotCoop.dll");
    return file_exists(p);
}

static int do_install(const wchar_t* folder, wchar_t* why, size_t why_cap) {
    if (!is_game(folder)) return _snwprintf(why, why_cap, L"Not a Longshot game folder (longshot.exe missing): %ls", folder), EXIT_NOT_GAME;
    if (game_running(folder)) return _snwprintf(why, why_cap, L"Close the game (longshot.exe) first."), EXIT_RUNNING;
    wchar_t bin[MAX_PATH], coop[MAX_PATH], loader[MAX_PATH], mod[MAX_PATH], backup[MAX_PATH];
    join(bin, folder, L"bin");
    CreateDirectoryW(bin, NULL);
    join(coop, bin, L"coop");
    join(loader, bin, L"winmm.dll");
    join(mod, coop, L"LongshotCoop.dll");
    CreateDirectoryW(coop, NULL);
    if (!dir_exists(coop)) return _snwprintf(why, why_cap, L"Could not make the folder %ls", coop), EXIT_WRITE;
    // (a winmm.dll that is not ours, another mod's: kept aside, given back on Uninstall)
    if (file_exists(loader) && !file_has(loader, "LONGSHOT_COOP", 0)) {
        join(backup, bin, L"winmm.dll.before-coop");
        if (!file_exists(backup)) MoveFileExW(loader, backup, MOVEFILE_REPLACE_EXISTING);
    }
    if (!write_payload(IDR_COOP_DLL, mod)) return _snwprintf(why, why_cap, L"Could not write %ls", mod), EXIT_WRITE;
    if (!write_payload(IDR_WINMM_DLL, loader)) return _snwprintf(why, why_cap, L"Could not write %ls", loader), EXIT_WRITE;
    _snwprintf(why, why_cap, L"Installed successfully into %ls", folder);
    return EXIT_DONE;
}

static int do_uninstall(const wchar_t* folder, int remove_settings, wchar_t* why, size_t why_cap) {
    if (!is_game(folder)) return _snwprintf(why, why_cap, L"Not a Longshot game folder: %ls", folder), EXIT_NOT_GAME;
    if (game_running(folder)) return _snwprintf(why, why_cap, L"Close the game first."), EXIT_RUNNING;
    wchar_t bin[MAX_PATH], coop[MAX_PATH], loader[MAX_PATH], mod[MAX_PATH], backup[MAX_PATH];
    join(bin, folder, L"bin");
    join(coop, bin, L"coop");
    join(loader, bin, L"winmm.dll");
    join(mod, coop, L"LongshotCoop.dll");
    join(backup, bin, L"winmm.dll.before-coop");
    if (file_exists(loader) && file_has(loader, "LONGSHOT_COOP", 0) && !DeleteFileW(loader))
        return _snwprintf(why, why_cap, L"Could not remove %ls", loader), EXIT_WRITE;
    if (file_exists(backup) && !file_exists(loader)) MoveFileExW(backup, loader, 0);
    if (file_exists(mod) && !DeleteFileW(mod)) return _snwprintf(why, why_cap, L"Could not remove %ls", mod), EXIT_WRITE;
    if (remove_settings && dir_exists(coop)) {
        // (the mod's own files only: its settings, logs and test files)
        const wchar_t* patterns[] = {L"coop_settings*.ini", L"coop_log*.txt", L"probe*.txt", L"probe_on"};
        for (int i = 0; i < 4; i++) {
            wchar_t pattern[MAX_PATH];
            join(pattern, coop, patterns[i]);
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW(pattern, &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                wchar_t f[MAX_PATH];
                join(f, coop, fd.cFileName);
                DeleteFileW(f);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    RemoveDirectoryW(coop);   // (only when empty)
    _snwprintf(why, why_cap, L"Removed from %ls%ls", folder, remove_settings ? L" (settings too)" : L"");
    return EXIT_DONE;
}

// ---------------------------------------------------------------- the window

static int S(int v) { return MulDiv(v, (int)g_dpi, 96); }

static void make_fonts(void) {
    if (g_font) DeleteObject(g_font), DeleteObject(g_font_bold), DeleteObject(g_font_title);
    g_font = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_font_bold = CreateFontW(-S(15), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_font_title = CreateFontW(-S(22), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

static void set_status(const wchar_t* text) { SetWindowTextW(g_status, text); }

static void fill_list(void) {
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < g_game_count; i++) {
        wchar_t row[MAX_PATH + 64];
        _snwprintf(row, MAX_PATH + 64, L"%ls   %ls", installed(g_games[i]) ? L"[Installed]" : L"[ ]", g_games[i]);
        SendMessageW(g_list, LB_ADDSTRING, 0, (LPARAM)row);
    }
    if (g_game_count) SendMessageW(g_list, LB_SETCURSEL, 0, 0);
    const BOOL any = g_game_count > 0;
    EnableWindow(g_install_btn, any);
    EnableWindow(g_uninstall_btn, any);
    if (!any) set_status(L"Click Browse... to select the folder containing longshot.exe");
    else set_status(L"Select your game folder, then click Install.");
}

static void browse(HWND owner) {
    IFileOpenDialog* dlg = NULL;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void**)&dlg))) return;
    DWORD opts = 0;
    dlg->lpVtbl->GetOptions(dlg, &opts);
    dlg->lpVtbl->SetOptions(dlg, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->lpVtbl->SetTitle(dlg, L"Select the folder containing longshot.exe");
    if (SUCCEEDED(dlg->lpVtbl->Show(dlg, owner))) {
        IShellItem* item = NULL;
        if (SUCCEEDED(dlg->lpVtbl->GetResult(dlg, &item))) {
            wchar_t* path = NULL;
            if (SUCCEEDED(item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &path))) {
                wchar_t game[MAX_PATH];
                if (game_from_pick(path, game)) {
                    add_game(game);
                    fill_list();
                    for (int i = 0; i < g_game_count; i++)
                        if (!_wcsicmp(g_games[i], game)) SendMessageW(g_list, LB_SETCURSEL, (WPARAM)i, 0);
                    set_status(L"Folder selected. Click Install to proceed.");
                } else {
                    MessageBoxW(owner, L"Please select the folder containing the launcher (longshot.exe).\n\n"
                                       L"\x064A\x0631\x062C\x0649 \x0627\x062E\x062A\x064A\x0627\x0631 \x0627\x0644\x0645\x062C\x0644\x062F \x0627\x0644\x0630\x064A \x064A\x062D\x062A\x0648\x064A \x0639\x0644\x0649 \x0645\x0634\x063A\x0644 \x0627\x0644\x0644\x0639\x0628\x0629 (longshot.exe).",
                                L"Longshot Co-op Setup", MB_ICONWARNING);
                }
                CoTaskMemFree(path);
            }
            item->lpVtbl->Release(item);
        }
    }
    dlg->lpVtbl->Release(dlg);
}

static void act(HWND owner, int install) {
    const int sel = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= g_game_count) {
        set_status(L"Pick a game folder first.");
        return;
    }
    wchar_t why[MAX_PATH + 128];
    const int r = install ? do_install(g_games[sel], why, MAX_PATH + 128)
                          : do_uninstall(g_games[sel], SendMessageW(g_remove_settings, BM_GETCHECK, 0, 0) == BST_CHECKED, why, MAX_PATH + 128);
    if (r == EXIT_RUNNING) {
        MessageBoxW(owner, L"The game is currently running from this folder: please close longshot.exe first, then try again.",
                    L"Longshot Co-op Setup", MB_ICONWARNING);
    }
    fill_list();
    SendMessageW(g_list, LB_SETCURSEL, (WPARAM)sel, 0);
    set_status(why);
    if (r == EXIT_DONE && install)
        MessageBoxW(owner, L"Longshot Co-op installed successfully!\n"
                           L"Developer: Osama-Alzahrani-UQU\n"
                           L"GitHub: https://github.com/Osama-Alzahrani-UQU\n\n"
                           L"[Important Requirements]\n"
                           L"1. Radmin VPN: All players MUST join the EXACT SAME network room to play!\n"
                           L"2. Launcher: Start the game ONLY from longshot.exe (emulators are unsupported).\n"
                           L"3. Save Sync: All players must be at the exact same location in their saves.\n"
                           L"4. Beta Notice: Known issues exist and active improvements are underway.",
                    L"Longshot Co-op Setup", MB_ICONINFORMATION);
}

static HWND label(HWND parent, const wchar_t* text, int x, int y, int w, int h, HFONT font, int rtl) {
    HWND c = CreateWindowExW(rtl ? WS_EX_RTLREADING | WS_EX_RIGHT : 0, L"STATIC", text, WS_CHILD | WS_VISIBLE | (rtl ? SS_RIGHT : SS_LEFT),
                             S(x), S(y), S(w), S(h), parent, NULL, NULL, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

static HWND button(HWND parent, const wchar_t* text, int id, int x, int y, int w, int h, DWORD style) {
    HWND c = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, S(x), S(y), S(w), S(h), parent,
                             (HMENU)(INT_PTR)id, NULL, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

enum { ID_LIST = 100, ID_BROWSE, ID_INSTALL, ID_UNINSTALL, ID_REMOVE_SETTINGS, ID_CLOSE, ID_GITHUB, ID_PAYPAL };

static void build_ui(HWND w) {
    make_fonts();
    label(w, L"Longshot Co-op (Zelda Multiplayer Mod)", 20, 12, 600, 30, g_font_title, 0);
    
    label(w, L"Developer: Osama-Alzahrani-UQU  |  GitHub: https://github.com/Osama-Alzahrani-UQU", 20, 44, 600, 20, g_font_bold, 0);

    label(w, L"Notice: Works EXCLUSIVELY on Longshot Launcher (longshot.exe). Standalone emulators are unsupported.", 20, 68, 600, 20, g_font, 0);
    label(w, L"Mandatory: All players MUST join the SAME Radmin VPN network room with synced save locations.", 20, 90, 600, 20, g_font, 0);

    g_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                             S(20), S(114), S(600), S(110), w, (HMENU)ID_LIST, NULL, NULL);
    SendMessageW(g_list, WM_SETFONT, (WPARAM)g_font, TRUE);

    button(w, L"Browse...", ID_BROWSE, 20, 232, 115, 32, BS_PUSHBUTTON);
    button(w, L"GitHub: Osama-Alzahrani-UQU", ID_GITHUB, 142, 232, 190, 32, BS_PUSHBUTTON);
    g_install_btn = button(w, L"Install", ID_INSTALL, 340, 232, 134, 32, BS_DEFPUSHBUTTON);
    g_uninstall_btn = button(w, L"Uninstall", ID_UNINSTALL, 482, 232, 138, 32, BS_PUSHBUTTON);

    g_remove_settings = button(w, L"Also remove configuration settings on uninstall",
                               ID_REMOVE_SETTINGS, 20, 272, 600, 24, BS_AUTOCHECKBOX);

    g_status = label(w, L"", 20, 302, 600, 36, g_font_bold, 0);

    label(w, L"All rights reserved (C) 2026 Osama-Alzahrani-UQU. All players MUST join the same Radmin VPN.", 20, 344, 600, 20, g_font, 0);
    label(w, L"Beta Build: Known issues under active development. Feedback and bug reports are welcome.", 20, 366, 600, 20, g_font, 0);

    button(w, L"Support / Donate (PayPal)", ID_PAYPAL, 20, 396, 190, 30, BS_PUSHBUTTON);
    button(w, L"Close", ID_CLOSE, 500, 396, 120, 30, BS_PUSHBUTTON);

    find_games();
    fill_list();
}

static LRESULT CALLBACK wnd_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_dpi = GetDpiForWindow(w);
        build_ui(w);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_BROWSE: browse(w); break;
        case ID_GITHUB:
            ShellExecuteW(w, L"open", L"https://github.com/Osama-Alzahrani-UQU", NULL, NULL, SW_SHOWNORMAL);
            break;
        case ID_PAYPAL:
            ShellExecuteW(w, L"open", L"https://paypal.me/onlinerp53", NULL, NULL, SW_SHOWNORMAL);
            break;
        case ID_INSTALL: act(w, 1); break;
        case ID_UNINSTALL: act(w, 0); break;
        case ID_CLOSE: DestroyWindow(w); break;
        case ID_LIST:
            if (HIWORD(wp) == LBN_DBLCLK) act(w, 1);
            break;
        }
        return 0;
    case WM_DPICHANGED: {
        // (moved to a screen of another scale: the window rebuilt at the new size)
        g_dpi = HIWORD(wp);
        const RECT* r = (const RECT*)lp;
        SetWindowPos(w, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        HWND child;
        while ((child = GetWindow(w, GW_CHILD)) != NULL) DestroyWindow(child);
        build_ui(w);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

static int run_window(HINSTANCE inst) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"LongshotCoopSetup";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);
    const UINT dpi = GetDpiForSystem();
    RECT r = {0, 0, MulDiv(640, (int)dpi, 96), MulDiv(475, (int)dpi, 96)};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
    HWND w = CreateWindowExW(0, wc.lpszClassName, L"Longshot Co-op Setup - Osama-Alzahrani-UQU", style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                             r.bottom - r.top, NULL, NULL, inst, NULL);
    if (!w) return 1;
    ShowWindow(w, SW_SHOWNORMAL);
    UpdateWindow(w);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(w, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    CoUninitialize();
    return 0;
}

// ---------------------------------------------------------------- the command line

// a switch, written /name, -name or --name
static int is_switch(const wchar_t* arg, const wchar_t* name) {
    if (*arg == L'/') arg++;
    else if (arg[0] == L'-' && arg[1] == L'-') arg += 2;
    else if (*arg == L'-') arg++;
    else return 0;
    return !_wcsicmp(arg, name);
}

static int run_silent(int argc, wchar_t** argv) {
    int install = -1, remove_settings = 0;
    const wchar_t* folder = NULL;
    for (int i = 1; i < argc; i++) {
        if (is_switch(argv[i], L"silent")) continue;
        if (is_switch(argv[i], L"install") && i + 1 < argc) install = 1, folder = argv[++i];
        else if (is_switch(argv[i], L"uninstall") && i + 1 < argc) install = 0, folder = argv[++i];
        else if (is_switch(argv[i], L"removesettings")) remove_settings = 1;
        else return EXIT_ARGS;
    }
    if (install < 0 || !folder) return EXIT_ARGS;
    wchar_t game[MAX_PATH], why[MAX_PATH + 128];
    if (!game_from_pick(folder, game)) wcsncpy(game, folder, MAX_PATH - 1), game[MAX_PATH - 1] = 0;
    const int r = install ? do_install(game, why, MAX_PATH + 128) : do_uninstall(game, remove_settings, why, MAX_PATH + 128);
    // (the result for whoever started it, on its console when there is one)
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        char line[1024];
        const int n = WideCharToMultiByte(CP_UTF8, 0, why, -1, line, sizeof line - 2, NULL, NULL);
        if (n > 0) {
            line[n - 1] = '\n';
            DWORD put;
            WriteFile(out, line, (DWORD)n, &put, NULL);
        }
        FreeConsole();
    }
    return r;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show) {
    (void)prev, (void)cmd, (void)show;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int silent = 0;
    for (int i = 1; argv && i < argc; i++)
        if (is_switch(argv[i], L"silent")) silent = 1;
    const int r = silent ? run_silent(argc, argv) : run_window(inst);
    if (argv) LocalFree(argv);
    return r;
}
