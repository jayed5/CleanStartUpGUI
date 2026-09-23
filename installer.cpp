// CleanStart Installer - native Win32 GUI installer, fully self-contained
// (no CleanStart.bat needed). Modes:
//   CleanStartInstaller.exe            -> GUI
//   CleanStartInstaller.exe /run       -> run cleanup once (no GUI)
//   CleanStartInstaller.exe /install   -> register scheduled task (elevates itself)
//   CleanStartInstaller.exe /uninstall -> remove task + startup entry (elevates itself)
//   CleanStartInstaller.exe /clearlogs -> delete all logs in C:\CleanLogs
// Build: g++ -municode -mwindows -O2 -std=c++17 -static ... installer.cpp installer.rc ...
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <string>
#include <thread>
#include <atomic>

#pragma comment(lib, "comctl32.lib")

#define IDC_STATUS_LABEL   1001
#define IDC_STATUS_VALUE   1002
#define IDC_INSTALL_BTN    1003
#define IDC_UNINSTALL_BTN  1004
#define IDC_RUNNOW_BTN     1005
#define IDC_LOGS_BTN       1006
#define IDC_LOG_LIST       1007
#define IDC_INFO_TEXT      1008
#define IDC_PROGRESS       1009
#define IDC_REFRESH_BTN    1010
#define IDC_EXIT_BTN       1011
#define IDC_CLEARLOGS_BTN  1012

#define WM_WORK_DONE   (WM_APP + 1)   // background action finished
#define WM_STATUS      (WM_APP + 2)   // lParam = InstallStatus* (heap, receiver deletes)

#define TIMER_STATUS_POLL  2000
#define TIMER_PROGRESS     2001   // manual marquee animation (works without comctl32 v6)
#define PROGRESS_RANGE     100
#define PROGRESS_BLOCK     18     // animated block width, % of range
#define PROGRESS_STEP      4      // px per tick
#define WINDOW_W 640
#define WINDOW_H 452

static HWND g_hMain = nullptr;
static HWND g_hStatusValue = nullptr;
static HWND g_hInfoText = nullptr;
static HWND g_hLogList = nullptr;
static HWND g_hProgress = nullptr;
static HFONT g_hFont = nullptr;
static HFONT g_hFontBold = nullptr;
static std::atomic<bool> g_busy{ false };
static int g_pollTicks = 0;

// ---------------- helpers ----------------
static bool FileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring GetSelfPath() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

static std::wstring GetStartupDir() {
    PWSTR p = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Startup, 0, nullptr, &p))) {
        result = p;
        CoTaskMemFree(p);
    }
    return result;
}

static bool IsElevated() {
    BOOL elevated = FALSE;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elev{};
        DWORD cb = sizeof(elev);
        if (GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &cb))
            elevated = elev.TokenIsElevated;
        CloseHandle(token);
    }
    return elevated != FALSE;
}

// Run a process hidden and wait (with timeout). Returns exit code or -1.
static int RunHidden(const std::wstring& exe, const std::wstring& args, DWORD timeoutMs) {
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;
    DWORD wait = WaitForSingleObject(pi.hProcess, timeoutMs);
    int code = -1;
    if (wait == WAIT_OBJECT_0) {
        DWORD c = static_cast<DWORD>(-1);
        GetExitCodeProcess(pi.hProcess, &c);
        code = static_cast<int>(c);
    } else {
        TerminateProcess(pi.hProcess, 1);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

// Relaunch self elevated with the given argument; does NOT wait (no hang).
static bool RelaunchElevated(const std::wstring& arg) {
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOASYNC;   // don't wait on the calling thread
    sei.lpFile = GetSelfPath().c_str();
    sei.lpParameters = arg.c_str();
    sei.lpVerb = L"runas";
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) != FALSE;
}

// ---------------- status ----------------
struct InstallStatus {
    bool task = false;
    bool startupFile = false;
    bool elevated = false;
};

static bool TaskExists() {
    return RunHidden(L"schtasks.exe", L"/Query /TN CleanStart", 10000) == 0;
}

static InstallStatus CheckStatus() {
    InstallStatus s;
    s.task = TaskExists();
    s.startupFile = FileExists(GetStartupDir() + L"\\CleanStart.bat");
    s.elevated = IsElevated();
    return s;
}

// Worker-side: compute status and hand it to the GUI thread.
static void PostStatusToUI() {
    InstallStatus* s = new InstallStatus(CheckStatus());
    PostMessageW(g_hMain, WM_STATUS, 0, reinterpret_cast<LPARAM>(s));
}

// ---------------- native cleanup ----------------
static void CleanDir(const std::wstring& dir, FILE* log, unsigned long long& bytes, int& count) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring full = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // remove subdirectory tree (best effort)
            SHFILEOPSTRUCTW op{};
            op.hwnd = nullptr;
            op.wFunc = FO_DELETE;
            op.pFrom = (full + L'\0').c_str();
            op.fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
            if (SHFileOperationW(&op) == 0) count++;
            else fwprintf(log, L"  dir kept (in use): %s\n", full.c_str());
        } else {
            unsigned long long sz = (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            if (SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL) && DeleteFileW(full.c_str())) {
                bytes += sz;
                count++;
            } else {
                fwprintf(log, L"  file kept (in use): %s\n", full.c_str());
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void DeleteThumbCaches(FILE* log, unsigned long long& bytes, int& count) {
    PWSTR appData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appData))) return;
    std::wstring dir = std::wstring(appData) + L"\\Microsoft\\Windows\\Explorer";
    CoTaskMemFree(appData);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\thumbcache_*.db").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring full = dir + L"\\" + fd.cFileName;
        unsigned long long sz = (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        if (DeleteFileW(full.c_str())) { bytes += sz; count++; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void PurgeOldLogs() {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(L"C:\\CleanLogs\\clean_log_*.txt", &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    const unsigned long long threeDays = 3ULL * 24 * 60 * 60 * 10000000ULL;
    unsigned long long nowULL = (static_cast<unsigned long long>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    do {
        unsigned long long ft = (static_cast<unsigned long long>(fd.ftLastWriteTime.dwHighDateTime) << 32)
                               | fd.ftLastWriteTime.dwLowDateTime;
        if (nowULL - ft > threeDays)
            DeleteFileW((std::wstring(L"C:\\CleanLogs\\") + fd.cFileName).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Full cleanup pass; returns 0 on success. Safe to call from any thread.
static int CleanupNow() {
    CreateDirectoryW(L"C:\\CleanLogs", nullptr);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[128];
    swprintf(name, 128, L"C:\\CleanLogs\\clean_log_%04u-%02u-%02u_%02u-%02u-%02u.txt",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    FILE* log = _wfopen(name, L"w");
    if (!log) return -1;

    unsigned long long bytes = 0;
    int count = 0;
    fwprintf(log, L"CleanStart - start %04u-%02u-%02u %02u:%02u:%02u\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    wchar_t tempPath[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring temp = tempPath;
    if (!temp.empty() && temp.back() == L'\\') temp.pop_back();
    fwprintf(log, L"[1/5] Clearing %s\n", temp.c_str());
    CleanDir(temp, log, bytes, count);

    fwprintf(log, L"[2/5] Clearing Windows temp\n");
    wchar_t winDir[MAX_PATH]{};
    GetWindowsDirectoryW(winDir, MAX_PATH);
    CleanDir(std::wstring(winDir) + L"\\Temp", log, bytes, count);

    fwprintf(log, L"[3/5] Rebuilding thumbnail cache\n");
    DeleteThumbCaches(log, bytes, count);

    fwprintf(log, L"[4/5] Flushing DNS cache\n");
    RunHidden(L"ipconfig.exe", L"/flushdns", 30000);

    fwprintf(log, L"[5/5] Removing logs older than 3 days\n");
    PurgeOldLogs();

    fwprintf(log, L"Done. Deleted %d items, %llu bytes freed.\n", count, bytes);
    fclose(log);
    return 0;
}

static void ClearAllLogs() {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(L"C:\\CleanLogs\\*.*", &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            DeleteFileW((std::wstring(L"C:\\CleanLogs\\") + fd.cFileName).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// ---------------- install / uninstall (native) ----------------
static bool InstallTask() {
    std::wstring self = GetSelfPath();
    std::wstring args = L"/Create /F /TN CleanStart /SC ONLOGON /RL HIGHEST /TR \"\\\"" + self + L"\\\" /run\"";
    return RunHidden(L"schtasks.exe", args, 30000) == 0;
}

static bool UninstallAll() {
    RunHidden(L"schtasks.exe", L"/Delete /F /TN CleanStart", 15000);
    DeleteFileW((GetStartupDir() + L"\\CleanStart.bat").c_str());
    return !TaskExists();
}

// ---------------- UI ----------------
static void SetBusy(bool busy) {
    g_busy = busy;
    EnableWindow(GetDlgItem(g_hMain, IDC_INSTALL_BTN), !busy);
    EnableWindow(GetDlgItem(g_hMain, IDC_UNINSTALL_BTN), !busy);
    EnableWindow(GetDlgItem(g_hMain, IDC_RUNNOW_BTN), !busy);
    EnableWindow(GetDlgItem(g_hMain, IDC_CLEARLOGS_BTN), !busy);
    EnableWindow(GetDlgItem(g_hMain, IDC_REFRESH_BTN), !busy);
    // Manual marquee: classic PBM_SETMARQUEE needs comctl32 v6 (manifest),
    // which we don't embed (classic look). Animate the position ourselves.
    if (busy) {
        SendMessage(g_hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, PROGRESS_RANGE));
        SendMessage(g_hProgress, PBM_SETPOS, 0, 0);
        SetTimer(g_hMain, TIMER_PROGRESS, 60, nullptr);
    } else {
        KillTimer(g_hMain, TIMER_PROGRESS);
        SendMessage(g_hProgress, PBM_SETPOS, 0, 0);
    }
}

static void UpdateStatusUI(const InstallStatus& s) {
    wchar_t buf[128];
    if (s.task && s.startupFile)
        swprintf(buf, 128, L"Installed (task + startup folder)");
    else if (s.task)
        swprintf(buf, 128, L"Installed (scheduled task)");
    else if (s.startupFile)
        swprintf(buf, 128, L"Installed (startup folder)");
    else
        swprintf(buf, 128, L"Not installed");
    SetWindowTextW(g_hStatusValue, buf);
    InvalidateRect(g_hStatusValue, nullptr, TRUE);

    std::wstring info;
    if (!s.elevated)
        info += L"[i] Actions will ask for Administrator rights (UAC prompt).";
    else
        info += L"[i] Running as Administrator. Click Install to register cleanup at logon.";
    SetWindowTextW(g_hInfoText, info.c_str());

    ListView_DeleteAllItems(g_hLogList);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(L"C:\\CleanLogs\\clean_log_*.txt", &fd);
    if (h != INVALID_HANDLE_VALUE) {
        int row = 0;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                LVITEMW item{};
                item.mask = LVIF_TEXT;
                item.iItem = row;
                item.pszText = fd.cFileName;
                ListView_InsertItem(g_hLogList, &item);
                SYSTEMTIME st;
                FileTimeToSystemTime(&fd.ftLastWriteTime, &st);
                wchar_t date[64];
                swprintf(date, 64, L"%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
                ListView_SetItemText(g_hLogList, row, 1, date);
                unsigned long long size = (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
                wchar_t sz[32];
                if (size < 1024) swprintf(sz, 32, L"%llu B", size);
                else if (size < 1024 * 1024) swprintf(sz, 32, L"%.1f KB", size / 1024.0);
                else swprintf(sz, 32, L"%.1f MB", size / (1024.0 * 1024.0));
                ListView_SetItemText(g_hLogList, row, 2, sz);
                ++row;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

// Poll status for a while after an async elevated action completes.
static void StartStatusPolling() {
    g_pollTicks = 12;                       // ~12 s of polling
    SetTimer(g_hMain, TIMER_STATUS_POLL, 1000, nullptr);
}

// ---------------- actions (all off the UI thread) ----------------
static void DoInstall() {
    if (IsElevated()) {
        SetBusy(true);
        std::thread([] {
            InstallTask();
            PostMessageW(g_hMain, WM_WORK_DONE, 0, 0);
        }).detach();
    } else {
        RelaunchElevated(L"/install");      // no wait -> no hang; UAC prompt appears
        StartStatusPolling();
    }
}

static void DoUninstall() {
    if (IsElevated()) {
        SetBusy(true);
        std::thread([] {
            UninstallAll();
            PostMessageW(g_hMain, WM_WORK_DONE, 0, 0);
        }).detach();
    } else {
        RelaunchElevated(L"/uninstall");
        StartStatusPolling();
    }
}

static void DoRunNow() {
    SetBusy(true);
    std::thread([] {
        CleanupNow();
        PostMessageW(g_hMain, WM_WORK_DONE, 0, 0);
    }).detach();
}

static void DoClearLogs() {
    SetBusy(true);
    std::thread([] {
        ClearAllLogs();
        PostMessageW(g_hMain, WM_WORK_DONE, 0, 0);
    }).detach();
}

static void CreateControls(HWND hwnd) {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_hFont = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfWeight = FW_BOLD;
    g_hFontBold = CreateFontIndirectW(&ncm.lfMessageFont);

    auto makeLabel = [&](const wchar_t* text, int x, int y, int w, int h, bool bold = false) {
        HWND hCtl = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
            x, y, w, h, hwnd, nullptr, nullptr, nullptr);
        SendMessage(hCtl, WM_SETFONT, reinterpret_cast<WPARAM>(bold ? g_hFontBold : g_hFont), TRUE);
        return hCtl;
    };
    auto makeButton = [&](const wchar_t* text, int id, int x, int y, int w, int h) {
        HWND hCtl = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            x, y, w, h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
        SendMessage(hCtl, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFont), TRUE);
        return hCtl;
    };

    makeLabel(L"CleanStart Installer", 20, 15, 400, 30, true);

    makeLabel(L"Status:", 20, 55, 80, 20);
    g_hStatusValue = makeLabel(L"Checking...", 105, 55, 400, 20, true);

    g_hInfoText = makeLabel(L"", 20, 85, 580, 40);

    makeButton(L"Install", IDC_INSTALL_BTN, 20, 135, 130, 34);
    makeButton(L"Uninstall", IDC_UNINSTALL_BTN, 165, 135, 130, 34);
    makeButton(L"Run cleanup now", IDC_RUNNOW_BTN, 310, 135, 160, 34);
    makeButton(L"Open logs folder", IDC_LOGS_BTN, 485, 135, 135, 34);

    g_hProgress = CreateWindowExW(0, PROGRESS_CLASS, nullptr,
        WS_CHILD | WS_VISIBLE | PBS_MARQUEE, 20, 185, 600, 18, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PROGRESS)), nullptr, nullptr);
    SendMessage(g_hProgress, PBM_SETMARQUEE, FALSE, 0);

    makeLabel(L"Cleanup logs (C:\\CleanLogs):", 20, 218, 400, 20);
    g_hLogList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEW, nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
        20, 242, 600, 150, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LOG_LIST)), nullptr, nullptr);
    ListView_SetExtendedListViewStyle(g_hLogList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.pszText = const_cast<LPWSTR>(L"Log file");
    col.cx = 350; col.iSubItem = 0;
    ListView_InsertColumn(g_hLogList, 0, &col);
    col.pszText = const_cast<LPWSTR>(L"Last run");
    col.cx = 160; col.iSubItem = 1;
    ListView_InsertColumn(g_hLogList, 1, &col);
    col.pszText = const_cast<LPWSTR>(L"Size");
    col.cx = 86; col.iSubItem = 2;
    ListView_InsertColumn(g_hLogList, 2, &col);

    makeButton(L"Refresh", IDC_REFRESH_BTN, 20, 404, 100, 30);
    makeButton(L"Clear logs", IDC_CLEARLOGS_BTN, 130, 404, 100, 30);
    makeButton(L"Exit", IDC_EXIT_BTN, 240, 404, 100, 30);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_hMain = hwnd;
        CreateControls(hwnd);
        SetBusy(true);
        // status check involves schtasks -> do it off the UI thread on startup too
        std::thread([] { PostStatusToUI(); PostMessageW(g_hMain, WM_WORK_DONE, 0, 0); }).detach();
        return 0;
    case WM_TIMER:
        if (wParam == TIMER_PROGRESS) {
            // manual marquee: jump a block across the bar and wrap around
            static int pos = 0;
            pos += PROGRESS_STEP;
            if (pos > PROGRESS_RANGE) pos = -PROGRESS_BLOCK;   // restart from the left
            int clamped = pos < 0 ? 0 : pos;
            SendMessage(g_hProgress, PBM_SETPOS, clamped, 0);
            return 0;
        }
        if (wParam == TIMER_STATUS_POLL) {
            std::thread([] { PostStatusToUI(); }).detach();
            if (--g_pollTicks <= 0) KillTimer(g_hMain, TIMER_STATUS_POLL);
            return 0;
        }
        break;
    case WM_WORK_DONE:
        SetBusy(false);
        std::thread([] { PostStatusToUI(); }).detach();
        return 0;
    case WM_STATUS: {
        // Apply status on the GUI thread (worker threads must not touch controls)
        InstallStatus* s = reinterpret_cast<InstallStatus*>(lParam);
        if (s) {
            UpdateStatusUI(*s);
            delete s;
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        if (reinterpret_cast<HWND>(lParam) == g_hStatusValue) {
            static HBRUSH hbr = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
            wchar_t buf[128];
            GetWindowTextW(g_hStatusValue, buf, 128);
            bool installed = wcsstr(buf, L"Not installed") == nullptr;
            SetTextColor(hdc, installed ? RGB(0, 128, 0) : RGB(96, 96, 96));
            SetBkColor(hdc, GetSysColor(COLOR_BTNFACE));
            return reinterpret_cast<LRESULT>(hbr);
        }
        SetBkMode(hdc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_INSTALL_BTN: if (!g_busy) DoInstall(); break;
        case IDC_UNINSTALL_BTN: if (!g_busy) DoUninstall(); break;
        case IDC_RUNNOW_BTN: if (!g_busy) DoRunNow(); break;
        case IDC_CLEARLOGS_BTN: if (!g_busy) DoClearLogs(); break;
        case IDC_LOGS_BTN:
            CreateDirectoryW(L"C:\\CleanLogs", nullptr);
            ShellExecuteW(hwnd, L"open", L"C:\\CleanLogs", nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case IDC_REFRESH_BTN:
            if (!g_busy) {
                SetBusy(true);
                std::thread([] { PostStatusToUI(); PostMessageW(g_hMain, WM_WORK_DONE, 0, 0); }).detach();
            }
            break;
        case IDC_EXIT_BTN: DestroyWindow(hwnd); break;
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int nCmdShow) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // ---- command-line modes (used by the scheduled task & self-elevation) ----
    std::wstring cmd = lpCmdLine ? lpCmdLine : L"";
    // strip quotes/spaces
    auto trim = [](std::wstring s) {
        while (!s.empty() && (s.front() == L'"' || s.front() == L' ')) s.erase(s.begin());
        while (!s.empty() && (s.back() == L'"' || s.back() == L' ' || s.back() == L'\r' || s.back() == L'\n')) s.pop_back();
        return s;
    };
    std::wstring arg = trim(cmd);

    if (arg == L"/run") {
        CleanupNow();
        CoUninitialize();
        return 0;
    }
    if (arg == L"/install") {
        InstallTask();
        CoUninitialize();
        return 0;
    }
    if (arg == L"/uninstall") {
        UninstallAll();
        CoUninitialize();
        return 0;
    }
    if (arg == L"/clearlogs") {
        ClearAllLogs();
        CoUninitialize();
        return 0;
    }

    // ---- GUI ----
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"CleanStartInstallerWnd";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    if (!wc.hIconSm) wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    // Outer window size so the CLIENT area is exactly WINDOW_W x WINDOW_H
    RECT rc{ 0, 0, WINDOW_W, WINDOW_H };
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    int winW = rc.right - rc.left, winH = rc.bottom - rc.top;

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int x = (work.right - winW) / 2, y = (work.bottom - winH) / 2;

    HWND hwnd = CreateWindowExW(0, L"CleanStartInstallerWnd",
        L"CleanStart Installer | https://github.com/jayed5",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        x, y, winW, winH, nullptr, nullptr, hInstance, nullptr);

    HICON hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    if (hIcon) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hIcon));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(wc.hIconSm));
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return 0;
}
