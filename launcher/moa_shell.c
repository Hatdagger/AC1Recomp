/* "Armored Core Moa.exe": the one file a player starts.
 *
 * The release folder is laid out for the player, not for the build:
 *
 *   Armored Core Moa.exe      this program
 *   (disc images)             the player's own .cue/.bin, .chd or .iso
 *   Memory Card/              memory cards
 *   Mods/                     installed mods; Mods/new is the drop folder
 *   RecompData/               the game itself and everything it needs
 *
 * This program starts RecompData/ArmoredCoreMasterOfArena.exe and tells it
 * where the player's folders are, then waits for it and passes on its exit
 * code, so a front end that started this program sees the game's lifetime.
 * Arguments are passed through untouched.
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <wchar.h>

#define GAME_DIR L"RecompData"
#define GAME_EXE L"ArmoredCoreMasterOfArena.exe"

static void fail(const wchar_t* what, const wchar_t* detail) {
    wchar_t text[1024];
    _snwprintf(text, 1023, L"%ls\n\n%ls", what, detail ? detail : L"");
    text[1023] = 0;
    MessageBoxW(NULL, text, L"Armored Core: Master of Arena", MB_OK | MB_ICONERROR);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR args, int show) {
    static wchar_t root[32768], path[32768], command[32768];
    (void)inst; (void)prev; (void)show;

    DWORD n = GetModuleFileNameW(NULL, root, 32767);
    if (n == 0 || n >= 32767) { fail(L"Could not find this program's folder.", NULL); return 1; }
    wchar_t* slash = wcsrchr(root, L'\\');
    if (!slash) { fail(L"Could not find this program's folder.", root); return 1; }
    *slash = 0;

    /* The player's folders. Created here so an empty copy of the release
     * still has somewhere to put a card or a mod. */
    _snwprintf(path, 32767, L"%ls\\Memory Card", root);
    CreateDirectoryW(path, NULL);
    static wchar_t cards[32768];
    wcscpy(cards, path);
    _snwprintf(path, 32767, L"%ls\\Mods", root);
    CreateDirectoryW(path, NULL);
    SetEnvironmentVariableW(L"PSX_MODS_DIR", path);
    _snwprintf(path, 32767, L"%ls\\Mods\\new", root);
    CreateDirectoryW(path, NULL);
    SetEnvironmentVariableW(L"PSX_DISC_SEARCH_DIR", root);

    static wchar_t game_dir[32768];
    _snwprintf(game_dir, 32767, L"%ls\\" GAME_DIR, root);
    _snwprintf(path, 32767, L"%ls\\" GAME_EXE, game_dir);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        fail(L"The game files are missing. Keep the RecompData folder next to "
             L"this program.", path);
        return 1;
    }
    _snwprintf(command, 32767, L"\"%ls\" --memcard-dir \"%ls\"%ls%ls", path, cards,
               (args && args[0]) ? L" " : L"", (args && args[0]) ? args : L"");
    command[32767] = 0;

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si);
    ZeroMemory(&pi, sizeof pi);
    si.cb = sizeof si;
    if (!CreateProcessW(path, command, NULL, NULL, FALSE, 0, NULL, game_dir, &si, &pi)) {
        fail(L"The game could not be started.", path);
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}
