// dsound_proxy.cpp - the TODGameplay loader: a dsound.dll proxy for the GOG TOD.exe. TOD.exe imports DSOUND.dll
// (ordinal 1, DirectSoundCreate), and DSOUND is not a KnownDLL, so a dsound.dll next to TOD.exe is loaded instead of
// the system one. This proxy
//   1. forwards all 12 exports of the system dsound.dll (same ordinals) to the real DLL, resolved on first use;
//   2. loads TODGameplay.dll from the exe's folder (full path) when the process starts.
// It never touches the game's memory itself. It coexists with the dinput8.dll proxy (renderer bridge) and the d3d9.dll
// proxy (TOD HD): different DLL names, no shared hook sites.
// Optional, TODGameplay.ini [loader]: chain_dsound = <file in the exe folder> loads another dsound.dll (e.g. a DSOAL
// build renamed) instead of the system one; load = 0 forwards only (TODGameplay.dll is not loaded).
#include <windows.h>
#include <stdio.h>
#include <string.h>

static char g_dir[MAX_PATH];
static HMODULE g_real = nullptr;
static volatile LONG g_resolved = 0;
static const char* kNames[12] = {"DirectSoundCreate", "DirectSoundEnumerateA", "DirectSoundEnumerateW", "DllCanUnloadNow",
                                 "DllGetClassObject", "DirectSoundCaptureCreate", "DirectSoundCaptureEnumerateA",
                                 "DirectSoundCaptureEnumerateW", "GetDeviceID", "DirectSoundFullDuplexCreate",
                                 "DirectSoundCreate8", "DirectSoundCaptureCreate8"};
extern "C" { FARPROC g_fn[12]; }

static void log_line(const char* msg) {
    char p[MAX_PATH + 32]; sprintf_s(p, "%s\\TODGameplay.log", g_dir);
    FILE* f = nullptr; if (fopen_s(&f, p, "a") != 0 || !f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "%02d:%02d:%02d.%03d dsound loader: %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
    fclose(f);
}
// only reachable if the real DLL lacks an export (never with the Windows 7..11 dsound.dll): fail without crashing
extern "C" __declspec(naked) void missing_export() {
    __asm mov eax, 0x80004001
    __asm ret
}
extern "C" void __cdecl resolve_all() {
    if (g_resolved) return;
    char ini[MAX_PATH + 32]; sprintf_s(ini, "%s\\TODGameplay.ini", g_dir);
    char chain[MAX_PATH] = "";
    GetPrivateProfileStringA("loader", "chain_dsound", "", chain, MAX_PATH, ini);
    if (chain[0]) {
        char p[MAX_PATH * 2]; sprintf_s(p, "%s\\%s", g_dir, chain);
        g_real = LoadLibraryA(p);
        char m[MAX_PATH * 2 + 64]; sprintf_s(m, "chain_dsound %s %s", p, g_real ? "loaded" : "NOT FOUND, using the system dsound.dll");
        log_line(m);
    }
    if (!g_real) {
        char p[MAX_PATH]; GetSystemDirectoryA(p, MAX_PATH); strcat_s(p, "\\dsound.dll");
        g_real = LoadLibraryA(p);
    }
    for (int i = 0; i < 12; ++i) {
        FARPROC f = g_real ? GetProcAddress(g_real, kNames[i]) : nullptr;
        g_fn[i] = f ? f : (FARPROC)&missing_export;
    }
    InterlockedExchange(&g_resolved, 1);
}

#define FWD(i, name) \
    extern "C" __declspec(naked) void Fwd_##name() { \
        __asm pushad __asm call resolve_all __asm popad __asm jmp dword ptr [g_fn + 4 * i] \
    }
FWD(0, DirectSoundCreate)
FWD(1, DirectSoundEnumerateA)
FWD(2, DirectSoundEnumerateW)
FWD(3, DllCanUnloadNow)
FWD(4, DllGetClassObject)
FWD(5, DirectSoundCaptureCreate)
FWD(6, DirectSoundCaptureEnumerateA)
FWD(7, DirectSoundCaptureEnumerateW)
FWD(8, GetDeviceID)
FWD(9, DirectSoundFullDuplexCreate)
FWD(10, DirectSoundCreate8)
FWD(11, DirectSoundCaptureCreate8)

// HD effects (hdfx/README.md): the TOD HD d3d9.dll writes its logs to TODHD_LOG_DIR, else to a folder compiled into it,
// and it refuses a folder at or under the game folder. Point it at
// %LOCALAPPDATA%\TODGameplay\hdfx_logs (or [loader] hdfx_log_dir) before anything in the process logs: this DllMain runs
// before TOD.exe's entry point, and the d3d9.dll reads the variable at its first log line (TOD.exe's first USER32
// call or Direct3DCreate9, both later). A TODHD_LOG_DIR that is already set is left alone. Kernel32 only.
static void set_hdfx_log_dir(const char* ini) {
    char cur[8];
    if (GetEnvironmentVariableA("TODHD_LOG_DIR", cur, sizeof cur)) return;
    char dir[MAX_PATH] = "";
    GetPrivateProfileStringA("loader", "hdfx_log_dir", "", dir, MAX_PATH, ini);
    if (!dir[0]) {
        char base[MAX_PATH];
        DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH);
        if (!n || n >= MAX_PATH - 40) return;
        sprintf_s(dir, "%s\\TODGameplay\\hdfx_logs", base);
    }
    for (char* p = dir + 3; *p; ++p)                  // every folder of the path (the d3d9.dll makes only the leaf)
        if (*p == '\\' || *p == '/') { char c = *p; *p = 0; CreateDirectoryA(dir, nullptr); *p = c; }
    CreateDirectoryA(dir, nullptr);
    SetEnvironmentVariableA("TODHD_LOG_DIR", dir);
}

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        GetModuleFileNameA(nullptr, g_dir, MAX_PATH);
        char* sl = strrchr(g_dir, '\\'); if (sl) *sl = 0;
        char ini[MAX_PATH + 32]; sprintf_s(ini, "%s\\TODGameplay.ini", g_dir);
        set_hdfx_log_dir(ini);
        if (GetPrivateProfileIntA("loader", "load", 1, ini)) {
            char p[MAX_PATH + 32]; sprintf_s(p, "%s\\TODGameplay.dll", g_dir);
            if (!LoadLibraryA(p)) log_line("TODGameplay.dll not found next to the exe: forwarding only");
        }
    }
    return TRUE;
}
