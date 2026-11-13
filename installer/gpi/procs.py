"""Is Total Overdose running from this game folder? (Windows process list through psapi, stdlib ctypes only.)

The installer refuses every install / restore step while a TOD.exe or TotalOverdose.exe runs from the selected game
folder. A TOD process whose path cannot be read (access denied) counts as running there: better refuse than write
under a running game.
"""
import ctypes
import os
import sys

GAME_EXES = ('tod.exe', 'totaloverdose.exe')


def list_processes():
    """[(pid, image path or None, exe name lower)] of every process (Windows); [] elsewhere."""
    if sys.platform != 'win32':
        return []
    from ctypes import wintypes
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    psapi = ctypes.WinDLL('psapi', use_last_error=True)
    arr = (wintypes.DWORD * 8192)()
    needed = wintypes.DWORD()
    if not psapi.EnumProcesses(ctypes.byref(arr), ctypes.sizeof(arr), ctypes.byref(needed)):
        return []
    n = needed.value // ctypes.sizeof(wintypes.DWORD)
    OpenProcess = k32.OpenProcess
    OpenProcess.restype = wintypes.HANDLE
    OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    Query = k32.QueryFullProcessImageNameW
    Query.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    Close = k32.CloseHandle
    Close.argtypes = [wintypes.HANDLE]
    out = []
    for i in range(n):
        pid = arr[i]
        if not pid:
            continue
        h = OpenProcess(0x1000, False, pid)        # PROCESS_QUERY_LIMITED_INFORMATION
        path = None
        if h:
            buf = ctypes.create_unicode_buffer(1024)
            size = wintypes.DWORD(1024)
            if Query(h, 0, buf, ctypes.byref(size)):
                path = buf.value
            Close(h)
        name = os.path.basename(path).lower() if path else None
        out.append((pid, path, name))
    if not any(p[1] is None for p in out):
        return out
    # names of the processes we could not open (tasklist sees them all)
    try:
        import subprocess
        txt = subprocess.run(['tasklist', '/FO', 'CSV', '/NH'], capture_output=True, text=True,
                             creationflags=0x08000000).stdout
        names = {}
        for ln in txt.splitlines():
            parts = [p.strip('"') for p in ln.split('","')]
            if len(parts) > 1 and parts[1].isdigit():
                names[int(parts[1])] = parts[0].strip('"').lower()
        out = [(pid, path, name or names.get(pid)) for pid, path, name in out]
    except Exception:
        pass
    return out


def game_processes(game_dir, lister=list_processes):
    """The TOD processes that block an install into game_dir: [(pid, path or None)]."""
    g = os.path.normcase(os.path.abspath(game_dir)) if game_dir else None
    hits = []
    for pid, path, name in lister():
        if name not in GAME_EXES:
            continue
        if path is None:
            hits.append((pid, None))
            continue
        d = os.path.normcase(os.path.dirname(os.path.abspath(path)))
        if g is None or d == g:
            hits.append((pid, path))
    return hits


def any_game_process(lister=list_processes):
    return [(pid, path) for pid, path, name in lister() if name in GAME_EXES]
