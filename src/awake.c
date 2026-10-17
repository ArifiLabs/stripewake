/* Stripewake - keeps Windows awake from the tray. Plain C, Win32 only, no C runtime.
   The process sleeps in GetMessage; the only timer is one SetTimer at the end time (plus a 2 s input check while
   "Dim screen when away" is on). */
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <powrprof.h>
#include <intrin.h>
#include "resource.h"

#pragma function(memset, memcpy)
void *memset(void *d, int c, size_t n) { __stosb((unsigned char *)d, (unsigned char)c, n); return d; }
void *memcpy(void *d, const void *s, size_t n) { __movsb((unsigned char *)d, (const unsigned char *)s, n); return d; }

#define APP_NAME   L"Stripewake"
#define RUN_KEY    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE  L"Stripewake"
#define WM_TRAY    (WM_APP + 1)
#define TIMER_END  1
#define TIMER_ADD  2
#define TIMER_MENU 3  /* runs only while the menu is open, to tick its countdown */
#define TIMER_IDLE 4  /* "Dim screen when away" only: input check, 2 s; 250 ms while dimmed */
#define SEC        10000000ULL
#define MIN        (60 * SEC)

enum { ID_STATUS = 1000, ID_KEEP, ID_DISPLAY, ID_FOR30, ID_FOR60, ID_FOR120, ID_FOR240, ID_FOR480,
       ID_UNTIL, ID_LOGON, ID_CHECK, ID_EXIT, ID_LID0, ID_LID1, ID_LID2, ID_DYN, ID_INDEF, ID_WU, ID_NOREBOOT, ID_P1, ID_P3, ID_P7, ID_P14, ID_P21, ID_P28, ID_P35, ID_RESUME, ID_M0, ID_M1, ID_M2, ID_M3, ID_SS, ID_NODIM, ID_AWAYDIM,
       ID_DL30, ID_DL50, ID_DL70, ID_DM2, ID_DM5, ID_DM10, ID_DM15 };
static const DWORD DIM_PCT[] = {30, 50, 70};      /* ID_DL30.. : dim to this % of the current brightness */
static const DWORD DIM_MINS[] = {2, 5, 10, 15};   /* ID_DM2.. : minutes without input before dimming */
static const DWORD PAUSE_DAYS[] = {1, 3, 7, 14, 21, 28, 35};  /* ID_P1.. ; Settings offers up to 5 weeks */

static HINSTANCE g_inst;
static HWND g_hwnd;
static UINT g_taskbarCreated;
static HICON g_icon[3];
static BOOL g_on, g_display = TRUE, g_added, g_busy;
static ULONGLONG g_since, g_end;  /* UTC FILETIME units; g_end 0 = indefinite */
static ULONGLONG g_pick;
static UINT g_timerId;
static HMENU g_menu;  /* the open tray menu, for the live countdown */  /* menu item of the running timer: ID_FOR30..ID_FOR480 or ID_UNTIL; 0 = none */
static int g_addTries;
static WCHAR g_tip[256];
static BOOL g_lidPresent, g_lidTest, g_lidTripped;
static DWORD g_lidMode;  /* 0 = sleep as usual, 1 = keep running when plugged in, 2 = always */
typedef struct HOLD HOLD;
struct HOLD {
    const char *name;   /* for the log */
    const WCHAR *sect;  /* section in restore.ini */
    const GUID *sub, *set;
    BOOL held;
    DWORD mode;         /* lid mode while held */
    GUID plan;
};
static HOLD H_LID, H_LOCK, H_DISP;  /* defined with the power-plan holds below */
static BOOL g_dynOpt, g_dynHeld, g_dynTest;
static BOOL g_ssOpt = TRUE, g_ssHeld;  /* "Pause screen saver while ON" (on by default) and whether it is paused now */
static DWORD Atou(const WCHAR *s);  /* "Pause Dynamic Lock while ON" (opt-in) and whether it is paused now */
static const WCHAR *g_dynName = L"EnableGoodbye";  /* /dyntest swaps in a test value name */
static WCHAR g_buf[4096];
static BOOL WuPausedUntil(WCHAR *out);
static BOOL PresenceSettingExists(const HOLD *h);  /* the plan has a non-never value for it (so it needs holding) */
static BOOL WuNoReboot(void);
static int WuMode(void);
static const WCHAR *MODE_TIP[] = {L"", L"\nUpdates: ask before download", L"\nUpdates: ask before install",
                                  L"\nAutomatic updates off"};
static DWORD RegDw(HKEY root, LPCWSTR key, LPCWSTR val, DWORD def);

static ULONGLONG NowUtc(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

static void ToLocal(ULONGLONG t, SYSTEMTIME *lt)
{
    FILETIME ft;
    SYSTEMTIME st;
    ft.dwLowDateTime = (DWORD)t;
    ft.dwHighDateTime = (DWORD)(t >> 32);
    FileTimeToSystemTime(&ft, &st);
    SystemTimeToTzSpecificLocalTime(NULL, &st, lt);
}

static void Stamp(ULONGLONG t, WCHAR *out)
{
    SYSTEMTIME s;
    ToLocal(t, &s);
    wsprintfW(out, L"%04u-%02u-%02u %02u:%02u", s.wYear, s.wMonth, s.wDay, s.wHour, s.wMinute);
}

/* %LOCALAPPDATA%\Stripewake\<file>; creates the folder. */
static BOOL AppPath(WCHAR *path, const WCHAR *file)
{
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
    if (!n || n > MAX_PATH - 40) return FALSE;
    lstrcatW(path, L"\\Stripewake");
    CreateDirectoryW(path, NULL);
    lstrcatW(path, L"\\");
    lstrcatW(path, file);
    return TRUE;
}

static void Log(const char *what)
{
    WCHAR path[MAX_PATH];
    char line[300];
    SYSTEMTIME s;
    DWORD n, w;
    HANDLE h;

    if (!AppPath(path, L"state.log")) return;
    h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    GetLocalTime(&s);
    n = wsprintfA(line, "%04u-%02u-%02u %02u:%02u:%02u  %s\r\n",
                  s.wYear, s.wMonth, s.wDay, s.wHour, s.wMinute, s.wSecond, what);
    WriteFile(h, line, n, &w, NULL);
    CloseHandle(h);
}

/* "17:30" today, "2026-10-18 09:00" on another day. */
static void UntilText(ULONGLONG t, WCHAR *out)
{
    SYSTEMTIME e, n;
    ToLocal(t, &e);
    ToLocal(NowUtc(), &n);
    if (e.wYear == n.wYear && e.wMonth == n.wMonth && e.wDay == n.wDay) wsprintfW(out, L"%02u:%02u", e.wHour, e.wMinute);
    else wsprintfW(out, L"%04u-%02u-%02u %02u:%02u", e.wYear, e.wMonth, e.wDay, e.wHour, e.wMinute);
}

/* One line: "Stripewake: ON since ...", "Stripewake: ON - 1 h 23 m left (until 17:30)" or "Stripewake: OFF since ...".
   secs = TRUE adds seconds ("1 h 23 m 45 s"): the open menu ticks once a second. */
static void StatusTextEx(WCHAR *out, BOOL secs)
{
    WCHAR since[24], left[32];
    ULONGLONG now;
    DWORD t;

    Stamp(g_since, since);
    if (!g_on) { wsprintfW(out, L"Stripewake: OFF since %s", since); return; }
    if (!g_end) { wsprintfW(out, L"Stripewake: ON since %s", since); return; }
    now = NowUtc();
    if (secs) {
        t = now >= g_end ? 0 : (DWORD)((g_end - now + SEC - 1) / SEC);
        wsprintfW(left, L"%u h %02u m %02u s", t / 3600, t % 3600 / 60, t % 60);
    } else {
        t = now >= g_end ? 0 : (DWORD)((g_end - now + MIN - 1) / MIN);
        wsprintfW(left, L"%u h %02u m", t / 60, t % 60);
    }
    UntilText(g_end, since);
    wsprintfW(out, L"Stripewake: ON - %s left (until %s)", left, since);
}

static void StatusText(WCHAR *out) { StatusTextEx(out, FALSE); }

/* "Restart pending since 2026-10-16 17:30" when Windows has flagged a restart: the flag key's last-write time. */
static BOOL RestartPending(WCHAR *out)
{
    static const WCHAR *KEYS[] = {
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update\\RebootRequired",
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Component Based Servicing\\RebootPending"};
    FILETIME ft;
    HKEY k;
    int i;
    for (i = 0; i < 2; i++) {
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, KEYS[i], 0, KEY_READ, &k) != ERROR_SUCCESS) continue;
        if (RegQueryInfoKeyW(k, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, &ft) != ERROR_SUCCESS) {
            RegCloseKey(k);
            continue;
        }
        RegCloseKey(k);
        lstrcpyW(out, L"Restart pending since ");
        Stamp(((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime, out + lstrlenW(out));
        return TRUE;
    }
    return FALSE;
}

static void TipText(WCHAR *out)
{
    WCHAR pend[64];
    WCHAR since[24];
    StatusText(out);
    if (g_on && g_end) {
        Stamp(g_since, since);
        wsprintfW(out + lstrlenW(out), L"\nON since %s", since);
    }
    if (g_on && !g_display) lstrcatW(out, L"\nDisplay may sleep");
    if (g_dynHeld) lstrcatW(out, L"\nDynamic Lock paused");
    if (g_ssHeld) lstrcatW(out, L"\nScreen saver paused");
    if (WuPausedUntil(pend)) { lstrcatW(out, L"\nUpdates paused until "); lstrcatW(out, pend); }
    if (WuNoReboot()) lstrcatW(out, L"\nNo update restarts while signed in");
    lstrcatW(out, MODE_TIP[WuMode()]);
    if (RestartPending(pend)) { lstrcatW(out, L"\n"); lstrcatW(out, pend); }
    if (H_LID.held) lstrcatW(out, H_LID.mode == 2 ? L"\nLid on battery: heat, drain; off below 10%"
                                                 : L"\nLid shut: runs if plugged in");
}

static void Tray(DWORD msg)
{
    NOTIFYICONDATAW nid;
    memset(&nid, 0, sizeof nid);
    nid.cbSize = sizeof nid;
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = g_icon[!g_on ? 2 : g_end ? 1 : 0];
    TipText(g_tip);
    lstrcpynW(nid.szTip, g_tip, sizeof nid.szTip / sizeof nid.szTip[0]);
    if (msg == NIM_ADD) {
        g_added = Shell_NotifyIconW(NIM_ADD, &nid);
        if (!g_added) {  /* Explorer not ready yet at sign-in: bounded retry, stops on success. */
            if (++g_addTries <= 30) SetTimer(g_hwnd, TIMER_ADD, 2000, NULL);
            return;
        }
        KillTimer(g_hwnd, TIMER_ADD);
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    } else if (g_added) {
        Shell_NotifyIconW(msg, &nid);
        if (msg == NIM_DELETE) g_added = FALSE;
    }
}

static void Apply(void)
{
    /* The display is kept on by the plan hold (H_DISP), so the screen may still dim. The display request is the
       fallback when that hold is missing (another signed-in user holds the plan, or the plan cannot be written). */
    SetThreadExecutionState(g_on ? ES_CONTINUOUS | ES_SYSTEM_REQUIRED |
                                   (g_display && !H_DISP.held && PresenceSettingExists(&H_DISP) ? ES_DISPLAY_REQUIRED : 0)
                                 : ES_CONTINUOUS);
}

/* ---- Power-plan holds while ON, saved first and put back when ON ends ----
   lid:        lid close action -> "Do nothing" (laptops, when the user picks a keep-running mode)
   lockscreen: "display off after locking" -> never, so the screen stays lit at the lock screen
   The original values are saved to restore.ini BEFORE anything is written, so a crash or a forced stop is
   repaired at the next start (or by the uninstaller's /restore). One session at a time holds plan settings. */

static const GUID SUB_BUTTONS_G = {0x4f971e89, 0xeebd, 0x4455, {0xa8, 0xde, 0x9e, 0x59, 0x04, 0x0e, 0x73, 0x47}};
static const GUID LIDACTION_G   = {0x5ca83367, 0x6e45, 0x459f, {0xa2, 0x7b, 0x47, 0x6b, 0x1d, 0x01, 0xc9, 0x36}};
static const GUID SUB_VIDEO_H   = {0x7516b95f, 0xf776, 0x4464, {0x8c, 0x53, 0x06, 0x16, 0x7f, 0x40, 0xcc, 0x99}};
static const GUID CONLOCK_G     = {0x8ec4b3a5, 0x6868, 0x48c2, {0xbe, 0x75, 0x4f, 0x30, 0x44, 0xbe, 0x88, 0xa7}};
static const GUID ACTIVE_PLAN_G = {0x31f9f286, 0x5084, 0x42fe, {0xb7, 0x20, 0x2b, 0x02, 0x64, 0x99, 0x37, 0x63}};
static const GUID BATTERY_PCT_G = {0xa7ad8041, 0xb45a, 0x4cae, {0x87, 0xa3, 0xee, 0xcb, 0xb4, 0x68, 0xa9, 0xe1}};
static const GUID DISPLAY_G     = {0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};
static const char *LID_A[] = {"Do nothing", "Sleep", "Hibernate", "Shut down", "?"};
static const WCHAR *LID_W[] = {L"Do nothing", L"Sleep", L"Hibernate", L"Shut down", L"?"};
static const char *MODE_A[] = {"sleep as usual", "keep running when plugged in", "keep running always"};
#define BATTERY_NOTICE L"Below 10% battery Stripewake hands the lid back to Windows; at critical battery Windows hibernates no matter what."

static HOLD H_LID  = {"lid", L"lid", &SUB_BUTTONS_G, &LIDACTION_G, FALSE, 0, {0}};
static HOLD H_LOCK = {"lock-screen display", L"lockscreen", &SUB_VIDEO_H, &CONLOCK_G, FALSE, 0, {0}};
/* "Keep display on" = the plan's "turn off display after" -> never while ON. Not ES_DISPLAY_REQUIRED: that request
   also stops dimming, and dimming is wanted (saves power, dark rooms; moving the mouse brings it back). */
static const GUID VIDEOIDLE_G = {0x3c0bc021, 0xc8a8, 0x4e07, {0xa9, 0x73, 0x6b, 0x14, 0xcb, 0xcb, 0x2b, 0x7e}};
static HOLD H_DISP = {"display off", L"display", &SUB_VIDEO_H, &VIDEOIDLE_G, FALSE, 0, {0}};
/* Presence sensor (PCs with one): the two "display off when away / not looking" timeouts -> never while ON, so
   walking away does not blank the screen (dimming is left to Windows). Same save-first / put-back as the other plan holds. */
static const GUID SUB_PRES_H  = {0x8619b916, 0xe004, 0x4dd8, {0x9b, 0x66, 0xda, 0xe8, 0x6f, 0x80, 0x66, 0x98}};
static const GUID PRES_OFF_G  = {0x0a7d6ab6, 0xac83, 0x4ad1, {0x82, 0x82, 0xec, 0xa5, 0xb5, 0x83, 0x08, 0xf3}};
static const GUID PRES_LOFF_G = {0xee16691e, 0x6ab3, 0x4619, {0xbb, 0x48, 0x1c, 0x77, 0xc9, 0x35, 0x7e, 0x5a}};
static HOLD H_PRES[2] = {  /* display OFF only: dimming may still happen (President 19:2x) */
    {"presence: display off when away", L"presence-off", &SUB_PRES_H, &PRES_OFF_G, FALSE, 0, {0}},
    {"presence: display off when not looking", L"presence-look-off", &SUB_PRES_H, &PRES_LOFF_G, FALSE, 0, {0}}};

/* "Stop dimming too" (opt-in, off by default): the dim timeouts -> never while ON. */
static const GUID VIDEODIM_G  = {0x17aaa29b, 0x8b43, 0x4b94, {0xaa, 0xfe, 0x35, 0xf6, 0x4d, 0xaa, 0xf1, 0xee}};
static const GUID PRES_DIM_G  = {0xa79c8e0e, 0xf271, 0x482d, {0x8f, 0x8a, 0x5d, 0xb9, 0xa1, 0x83, 0x12, 0xde}};
static const GUID PRES_LDIM_G = {0xcf8c6097, 0x12b8, 0x4279, {0xbb, 0xdd, 0x44, 0x60, 0x1e, 0xe5, 0x20, 0x9d}};
static HOLD H_DIM[3] = {
    {"screen dim", L"dim", &SUB_VIDEO_H, &VIDEODIM_G, FALSE, 0, {0}},
    {"presence: dim when away", L"presence-dim", &SUB_PRES_H, &PRES_DIM_G, FALSE, 0, {0}},
    {"presence: dim when not looking", L"presence-look-dim", &SUB_PRES_H, &PRES_LDIM_G, FALSE, 0, {0}}};
static BOOL g_noDim;

static BOOL AnyPresHeld(void)
{
    int i;
    for (i = 0; i < 2; i++) if (H_PRES[i].held) return TRUE;
    for (i = 0; i < 3; i++) if (H_DIM[i].held) return TRUE;
    return FALSE;
}
static HANDLE g_planMutex;

static BOOL GuidEq(const GUID *a, const GUID *b)
{
    const DWORD *x = (const DWORD *)a, *y = (const DWORD *)b;
    return x[0] == y[0] && x[1] == y[1] && x[2] == y[2] && x[3] == y[3];
}

static void GuidStr(const GUID *g, WCHAR *out)
{
    wsprintfW(out, L"%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", g->Data1, g->Data2, g->Data3,
              g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6],
              g->Data4[7]);
}

static BOOL GuidParse(const WCHAR *s, GUID *g)
{
    BYTE b[16];
    int n = 0, i;
    for (; *s && n < 32; s++) {
        int v = *s >= L'0' && *s <= L'9' ? *s - L'0' : *s >= L'A' && *s <= L'F' ? *s - L'A' + 10
              : *s >= L'a' && *s <= L'f' ? *s - L'a' + 10 : -1;
        if (v < 0) { if (*s == L'-') continue; return FALSE; }
        if (n & 1) b[n / 2] = (BYTE)(b[n / 2] | v); else b[n / 2] = (BYTE)(v << 4);
        n++;
    }
    if (n != 32) return FALSE;
    g->Data1 = ((DWORD)b[0] << 24) | ((DWORD)b[1] << 16) | ((DWORD)b[2] << 8) | b[3];
    g->Data2 = (WORD)((b[4] << 8) | b[5]);
    g->Data3 = (WORD)((b[6] << 8) | b[7]);
    for (i = 0; i < 8; i++) g->Data4[i] = b[8 + i];
    return TRUE;
}

static DWORD Lid4(DWORD v) { return v > 3 ? 4 : v; }

/* "Sleep" for the lid, "30 s" / "never" for the lock-screen timeout. */
static void ValueText(const HOLD *h, DWORD v, char *out)
{
    if (h == &H_LID) lstrcpyA(out, LID_A[Lid4(v)]);
    else if (!v) lstrcpyA(out, "never");
    else wsprintfA(out, "%u s", v);
}

static BOOL HoldRead(const HOLD *h, const GUID *plan, DWORD *ac, DWORD *dc)
{
    return PowerReadACValueIndex(NULL, plan, h->sub, h->set, ac) == ERROR_SUCCESS &&
           PowerReadDCValueIndex(NULL, plan, h->sub, h->set, dc) == ERROR_SUCCESS;
}

static void PlanMutexDrop(void)
{
    if (g_planMutex && !H_LID.held && !H_LOCK.held && !H_DISP.held && !AnyPresHeld()) {
        CloseHandle(g_planMutex);
        g_planMutex = NULL;
    }
}

/* Put one hold's saved values back and remove its section. Safe to call at any time. */
static void HoldRestore(HOLD *h, const char *why)
{
    WCHAR path[MAX_PATH], gs[48], names[64];
    GUID plan, *active = NULL;
    DWORD ac, dc, err;
    char line[260], a[24], d[24];

    h->held = FALSE;
    if (!AppPath(path, L"restore.ini")) { PlanMutexDrop(); return; }
    if (!GetPrivateProfileStringW(h->sect, L"plan", L"", gs, 48, path)) { PlanMutexDrop(); return; }
    ac = GetPrivateProfileIntW(h->sect, L"ac", -1, path);
    dc = GetPrivateProfileIntW(h->sect, L"dc", -1, path);
    if (!GuidParse(gs, &plan) || ac == (DWORD)-1 || dc == (DWORD)-1) {
        WritePrivateProfileStringW(h->sect, NULL, NULL, path);
        wsprintfA(line, "%s restore: saved values were unreadable and were removed", h->name);
        Log(line);
    } else {
        err = PowerWriteACValueIndex(NULL, &plan, h->sub, h->set, ac);
        if (!err) err = PowerWriteDCValueIndex(NULL, &plan, h->sub, h->set, dc);
        if (err) {  /* keep the saved values so the next start retries */
            wsprintfA(line, "%s restore FAILED (error %u) - %s; saved values kept for the next start", h->name, err, why);
            Log(line);
            PlanMutexDrop();
            return;
        }
        if (PowerGetActiveScheme(NULL, &active) == ERROR_SUCCESS) {
            if (GuidEq(active, &plan)) PowerSetActiveScheme(NULL, active);  /* apply to the live plan */
            LocalFree(active);
        }
        WritePrivateProfileStringW(h->sect, NULL, NULL, path);
        ValueText(h, ac, a);
        ValueText(h, dc, d);
        wsprintfA(line, "%s restored: plugged in %s, on battery %s - %s", h->name, a, d, why);
        Log(line);
    }
    WritePrivateProfileStringW(NULL, NULL, NULL, path);
    if (GetPrivateProfileSectionNamesW(names, 64, path) == 0) DeleteFileW(path);
    PlanMutexDrop();
}

/* Save, verify the save, then write 0 to AC (and DC when dcToo). */
static void HoldApply(HOLD *h, BOOL dcToo)
{
    WCHAR path[MAX_PATH], gs[48], num[12];
    GUID *active = NULL;
    DWORD ac, dc, err;
    char line[260], a[24], d[24];

    if (!g_planMutex) {
        g_planMutex = CreateMutexW(NULL, FALSE, L"Global\\Stripewake.PlanOwner");
        if (!g_planMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
            if (g_planMutex) CloseHandle(g_planMutex);
            g_planMutex = NULL;
            wsprintfA(line, "%s hold skipped: another signed-in user's Stripewake holds power-plan settings", h->name);
            Log(line);
            return;
        }
    }
    if (PowerGetActiveScheme(NULL, &active) != ERROR_SUCCESS || !HoldRead(h, active, &ac, &dc) ||
        !AppPath(path, L"restore.ini")) {
        if (active) LocalFree(active);
        wsprintfA(line, "%s hold skipped: could not read the power plan", h->name);
        Log(line);
        PlanMutexDrop();
        return;
    }
    GuidStr(active, gs);
    WritePrivateProfileStringW(h->sect, L"plan", gs, path);
    wsprintfW(num, L"%u", ac);
    WritePrivateProfileStringW(h->sect, L"ac", num, path);
    wsprintfW(num, L"%u", dc);
    WritePrivateProfileStringW(h->sect, L"dc", num, path);
    wsprintfW(num, L"%u", g_lidMode);
    if (h == &H_LID) WritePrivateProfileStringW(h->sect, L"mode", num, path);
    WritePrivateProfileStringW(NULL, NULL, NULL, path);
    if ((DWORD)GetPrivateProfileIntW(h->sect, L"ac", -1, path) != ac ||
        (DWORD)GetPrivateProfileIntW(h->sect, L"dc", -1, path) != dc) {
        WritePrivateProfileStringW(h->sect, NULL, NULL, path);
        LocalFree(active);
        wsprintfA(line, "%s hold skipped: could not save the current setting", h->name);
        Log(line);
        PlanMutexDrop();
        return;
    }
    err = PowerWriteACValueIndex(NULL, active, h->sub, h->set, 0);
    if (!err && dcToo) err = PowerWriteDCValueIndex(NULL, active, h->sub, h->set, 0);
    if (!err) err = PowerSetActiveScheme(NULL, active);
    if (err) {
        LocalFree(active);
        wsprintfA(line, "%s hold FAILED (error %u); putting the saved setting back", h->name, err);
        Log(line);
        HoldRestore(h, "hold failed");
        return;
    }
    h->plan = *active;
    LocalFree(active);
    h->held = TRUE;
    h->mode = g_lidMode;
    ValueText(h, ac, a);
    ValueText(h, dc, d);
    wsprintfA(line, "%s hold on: plan %S was plugged in %s, on battery %s; now plugged in %s%s%s", h->name, gs, a, d,
              h == &H_LID ? "Do nothing" : "never", dcToo ? ", on battery " : "",
              dcToo ? (h == &H_LID ? "Do nothing" : "never") : "");
    Log(line);
}

/* ---- Dynamic Lock (locks the PC when a paired phone walks away): paused while ON when the user opts in ---- */
#define WINLOGON_KEY L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon"

static void IniTidy(const WCHAR *path)
{
    WCHAR names[64];
    WritePrivateProfileStringW(NULL, NULL, NULL, path);
    if (GetPrivateProfileSectionNamesW(names, 64, path) == 0) DeleteFileW(path);
}

static void DynRestore(const char *why)
{
    WCHAR path[MAX_PATH], name[48], val[16];
    DWORD v, err;
    char line[200];

    g_dynHeld = FALSE;
    if (!AppPath(path, L"restore.ini") || !GetPrivateProfileStringW(L"dynlock", L"name", L"", name, 48, path)) return;
    GetPrivateProfileStringW(L"dynlock", L"value", L"", val, 16, path);
    if (!lstrcmpW(val, L"absent")) {
        err = RegDeleteKeyValueW(HKEY_CURRENT_USER, WINLOGON_KEY, name);
        if (err == ERROR_FILE_NOT_FOUND) err = 0;
        wsprintfA(line, "Dynamic Lock restored: %S was absent, removed again - %s", name, why);
    } else {
        v = Atou(val);
        err = val[0] ? RegSetKeyValueW(HKEY_CURRENT_USER, WINLOGON_KEY, name, REG_DWORD, &v, sizeof v) : 1;
        wsprintfA(line, "Dynamic Lock restored: %S = %u - %s", name, v, why);
    }
    if (err) {
        wsprintfA(line, "Dynamic Lock restore FAILED (error %u) - %s; saved value kept for the next start", err, why);
        Log(line);
        return;
    }
    WritePrivateProfileStringW(L"dynlock", NULL, NULL, path);
    IniTidy(path);
    Log(line);
}

static void DynApply(void)
{
    WCHAR path[MAX_PATH], val[16], back[16];
    DWORD v = 0, zero = 0, cb = sizeof v, err;
    BOOL present;
    char line[200];

    if (!AppPath(path, L"restore.ini")) return;
    present = RegGetValueW(HKEY_CURRENT_USER, WINLOGON_KEY, g_dynName, RRF_RT_REG_DWORD, NULL, &v, &cb) == ERROR_SUCCESS;
    if (present) wsprintfW(val, L"%u", v); else lstrcpyW(val, L"absent");
    WritePrivateProfileStringW(L"dynlock", L"name", g_dynName, path);
    WritePrivateProfileStringW(L"dynlock", L"value", val, path);
    WritePrivateProfileStringW(NULL, NULL, NULL, path);
    GetPrivateProfileStringW(L"dynlock", L"value", L"", back, 16, path);
    if (lstrcmpW(back, val)) {
        WritePrivateProfileStringW(L"dynlock", NULL, NULL, path);
        IniTidy(path);
        Log("Dynamic Lock pause skipped: could not save the current value");
        return;
    }
    err = RegSetKeyValueW(HKEY_CURRENT_USER, WINLOGON_KEY, g_dynName, REG_DWORD, &zero, sizeof zero);
    if (err) {
        wsprintfA(line, "Dynamic Lock pause FAILED (error %u)", err);
        Log(line);
        DynRestore("pause failed");
        return;
    }
    g_dynHeld = TRUE;
    wsprintfA(line, "Dynamic Lock paused: %S was %S, now 0", g_dynName, val);
    Log(line);
}

/* ---- Windows Update, the user's own controls (opt-in, off by default) ----
   Pause: the same values Settings > Windows Update > "Pause updates" writes; Resume deletes them, as Settings does.
   No restart while signed in: Windows' own policy NoAutoRebootWithLoggedOnUsers; the old value is saved first.
   Both need admin rights, so each change runs this exe elevated (one Windows prompt) with a fixed switch. */
#define WU_UX  L"SOFTWARE\\Microsoft\\WindowsUpdate\\UX\\Settings"
#define WU_AU  L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate\\AU"
#define WU_NR  L"NoAutoRebootWithLoggedOnUsers"
#define SW_KEY L"Software\\Stripewake"
#define WU_NR_WARN L"Windows will not restart this PC on its own for updates while you are signed in. Updates that need a restart, security fixes included, wait until you restart.\n\nTurn this on?"
static const WCHAR *PAUSE_V[] = {L"PauseUpdatesStartTime", L"PauseUpdatesExpiryTime",
                                 L"PauseFeatureUpdatesStartTime", L"PauseFeatureUpdatesEndTime",
                                 L"PauseQualityUpdatesStartTime", L"PauseQualityUpdatesEndTime"};

static void Iso(ULONGLONG t, WCHAR *out)  /* "2026-10-23T22:15:00Z", the format Settings writes */
{
    FILETIME ft;
    SYSTEMTIME s;
    ft.dwLowDateTime = (DWORD)t;
    ft.dwHighDateTime = (DWORD)(t >> 32);
    FileTimeToSystemTime(&ft, &s);
    wsprintfW(out, L"%04u-%02u-%02uT%02u:%02u:%02uZ", s.wYear, s.wMonth, s.wDay, s.wHour, s.wMinute, s.wSecond);
}

/* TRUE while Windows updates are paused (by anyone); out = local end time. */
static BOOL WuPausedUntil(WCHAR *out)
{
    WCHAR cur[40], now[24];
    SYSTEMTIME u, l;
    FILETIME ft;
    DWORD cb = sizeof cur;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, WU_UX, L"PauseUpdatesExpiryTime", RRF_RT_REG_SZ, NULL, cur, &cb) != ERROR_SUCCESS)
        return FALSE;
    Iso(NowUtc(), now);
    if (lstrlenW(cur) < 19 || lstrcmpW(cur, now) <= 0) return FALSE;
    if (out) {
        memset(&u, 0, sizeof u);
        u.wYear = (WORD)Atou(cur); u.wMonth = (WORD)Atou(cur + 5); u.wDay = (WORD)Atou(cur + 8);
        u.wHour = (WORD)Atou(cur + 11); u.wMinute = (WORD)Atou(cur + 14);
        if (SystemTimeToFileTime(&u, &ft)) {
            ToLocal(((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime, &l);
            wsprintfW(out, L"%04u-%02u-%02u %02u:%02u", l.wYear, l.wMonth, l.wDay, l.wHour, l.wMinute);
        } else lstrcpyW(out, cur);
    }
    return TRUE;
}

static BOOL WuNoReboot(void) { return RegDw(HKEY_LOCAL_MACHINE, WU_AU, WU_NR, 0) == 1; }

/* Set one AU policy DWORD from a token: "absent" deletes it; otherwise a digit in allowed[]. */
static DWORD AuPut(const WCHAR *name, const WCHAR *tok, const WCHAR *allowed)
{
    HKEY k;
    DWORD err, d;
    if (!lstrcmpW(tok, L"absent")) {
        err = RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, WU_AU, name);
        return err == ERROR_FILE_NOT_FOUND ? 0 : err;
    }
    if (!tok[0] || tok[1]) return ERROR_INVALID_PARAMETER;
    for (; *allowed && *allowed != tok[0]; allowed++) {}
    if (!*allowed) return ERROR_INVALID_PARAMETER;
    d = tok[0] - L'0';
    if ((err = RegCreateKeyExW(HKEY_LOCAL_MACHINE, WU_AU, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL))) return err;
    err = RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&d, sizeof d);
    RegCloseKey(k);
    return err;
}

/* Elevated side: fixed actions only, never an arbitrary write. Returns a Win32 error code (0 = done). */
static DWORD PauseWrite(DWORD d)  /* Settings' own pause: start now, end in d days */
{
    WCHAR a[24], b[24];
    const WCHAR *v;
    HKEY k;
    DWORD err, i;
    ULONGLONG now = NowUtc();
    Iso(now, a);
    Iso(now + d * 24ULL * 60 * MIN, b);
    if ((err = RegCreateKeyExW(HKEY_LOCAL_MACHINE, WU_UX, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL))) return err;
    for (i = 0; i < 6 && !err; i++) {
        v = (i & 1) ? b : a;
        err = RegSetValueExW(k, PAUSE_V[i], 0, REG_SZ, (const BYTE *)v, (lstrlenW(v) + 1) * sizeof(WCHAR));
    }
    RegCloseKey(k);
    return err;
}

static DWORD PauseClear(void)  /* Settings' Resume */
{
    DWORD err = 0, e, i;
    for (i = 0; i < 6; i++) {
        e = RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, WU_UX, PAUSE_V[i]);
        if (e && e != ERROR_FILE_NOT_FOUND) err = e;
    }
    return err;
}

/* Elevated side: fixed actions only, never an arbitrary write. Returns a Win32 error code (0 = done).
   "set <NoAutoUpdate 0|1|absent> <AUOptions 2-5|absent> <pause: keep|0 = clear|1-35 days>"
   "noreboot <0|1|absent>" */
static DWORD WuHelper(const WCHAR *verb, const WCHAR *na, const WCHAR *ao, const WCHAR *pz)
{
    DWORD err, d = 0;
    BOOL keep;
    if (!lstrcmpW(verb, L"noreboot")) return AuPut(WU_NR, na, L"01");
    if (lstrcmpW(verb, L"set")) return ERROR_INVALID_PARAMETER;
    if ((lstrcmpW(na, L"absent") && (!na[0] || na[1] || (na[0] != L'0' && na[0] != L'1'))) ||
        (lstrcmpW(ao, L"absent") && (!ao[0] || ao[1] || ao[0] < L'2' || ao[0] > L'5')))
        return ERROR_INVALID_PARAMETER;
    keep = !lstrcmpW(pz, L"keep");
    if (!keep) {
        if (!pz[0] || pz[0] < L'0' || pz[0] > L'9') return ERROR_INVALID_PARAMETER;
        d = Atou(pz);
        if (d > 35 || (pz[1] && (pz[1] < L'0' || pz[1] > L'9' || pz[2]))) return ERROR_INVALID_PARAMETER;
    }
    err = AuPut(L"NoAutoUpdate", na, L"01");
    if (!err) err = AuPut(L"AUOptions", ao, L"2345");
    if (!err && !keep) err = d ? PauseWrite(d) : PauseClear();
    return err;
}

/* Run "Stripewake.exe /wu <verb> [arg]" elevated and wait. ERROR_CANCELLED = the user said No at the prompt. */
static DWORD Elevate(const WCHAR *params)
{
    SHELLEXECUTEINFOW s;
    WCHAR exe[MAX_PATH];
    DWORD code = ERROR_GEN_FAILURE;
    memset(&s, 0, sizeof s);
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return GetLastError();
    s.cbSize = sizeof s;
    s.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    s.hwnd = g_hwnd;
    s.lpVerb = L"runas";
    s.lpFile = exe;
    s.lpParameters = params;
    s.nShow = SW_HIDE;
    if (!ShellExecuteExW(&s)) return GetLastError();
    if (s.hProcess) {
        WaitForSingleObject(s.hProcess, 60000);
        GetExitCodeProcess(s.hProcess, &code);
        CloseHandle(s.hProcess);
    }
    return code;
}

static void WuLog(const char *what, DWORD err)
{
    char line[160];
    if (!err) Log(what);
    else if (err == ERROR_CANCELLED) { wsprintfA(line, "%s: cancelled at the Windows admin prompt", what); Log(line); }
    else { wsprintfA(line, "%s FAILED (error %u); nothing changed", what, err); Log(line); }
}

/* First time only: the warning, with a confirm. */
static BOOL WuConfirm(const WCHAR *flag, const WCHAR *text)
{
    DWORD one = 1;
    if (RegDw(HKEY_CURRENT_USER, SW_KEY, flag, 0)) return TRUE;
    if (MessageBoxW(NULL, text, APP_NAME L" - Windows Update", MB_OKCANCEL | MB_ICONWARNING | MB_SETFOREGROUND) != IDOK)
        return FALSE;
    RegSetKeyValueW(HKEY_CURRENT_USER, SW_KEY, flag, REG_DWORD, &one, sizeof one);
    return TRUE;
}

/* Windows' own pause limit on this PC (Insider builds allow less than the usual 35 days). */
static DWORD WuMaxDays(void)
{
    DWORD m = RegDw(HKEY_LOCAL_MACHINE, WU_UX, L"FlightSettingsMaxPauseDays", 35);
    return m && m < 35 ? m : 35;
}

static void WuToggleNoReboot(void)
{
    WCHAR prev[16], cmd[40];
    DWORD v, cb = sizeof v, err;
    if (WuNoReboot()) {  /* put back what was there before Stripewake turned it on */
        cb = sizeof prev;
        if (RegGetValueW(HKEY_CURRENT_USER, SW_KEY, L"WuNoRebootPrev", RRF_RT_REG_SZ, NULL, prev, &cb) != ERROR_SUCCESS ||
            prev[0] == L'1')
            lstrcpyW(prev, L"absent");
        wsprintfW(cmd, L"/wu noreboot %s", prev);
        err = Elevate(cmd);
        WuLog("no update restarts while signed in: off", err);
        if (!err) RegDeleteKeyValueW(HKEY_CURRENT_USER, SW_KEY, L"WuNoRebootPrev");
        return;
    }
    if (!WuConfirm(L"WuNoRebootWarned", WU_NR_WARN)) return;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, WU_AU, WU_NR, RRF_RT_REG_DWORD, NULL, &v, &cb) == ERROR_SUCCESS)
        wsprintfW(prev, L"%u", v);
    else lstrcpyW(prev, L"absent");
    RegSetKeyValueW(HKEY_CURRENT_USER, SW_KEY, L"WuNoRebootPrev", REG_SZ, prev, (lstrlenW(prev) + 1) * sizeof(WCHAR));
    err = Elevate(L"/wu noreboot 1");
    WuLog("no update restarts while signed in: on", err);
    if (err) RegDeleteKeyValueW(HKEY_CURRENT_USER, SW_KEY, L"WuNoRebootPrev");
}

/* Windows Update mode = Windows' own policies: 0 Windows default, 1 ask before download (AUOptions 2),
   2 download, ask before install (AUOptions 3), 3 automatic updates off (NoAutoUpdate 1). */
static const WCHAR *MODE_ARGS[] = {NULL, L"absent 2", L"absent 3", L"1 absent"};
static const char *MODE_NAMES[] = {"Windows default", "ask before download", "download, ask before install",
                                   "automatic updates off"};
#define WU_MODE_WARN L"Windows updates will not install or restart on their own until you set Windows Updates back to Automatic (a pause also ends by itself). Security fixes wait too.\n\nContinue?"

static void AuTok(const WCHAR *name, WCHAR *out)
{
    DWORD v, cb = sizeof v;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, WU_AU, name, RRF_RT_REG_DWORD, NULL, &v, &cb) == ERROR_SUCCESS) wsprintfW(out, L"%u", v);
    else lstrcpyW(out, L"absent");
}

static int WuMode(void)
{
    if (RegDw(HKEY_LOCAL_MACHINE, WU_AU, L"NoAutoUpdate", 0) == 1) return 3;
    switch (RegDw(HKEY_LOCAL_MACHINE, WU_AU, L"AUOptions", 0)) {
    case 2: return 1;
    case 3: return 2;
    }
    return 0;
}

/* The policy pair Windows had before Stripewake's first change ("absent absent" when none was saved). */
static void AuDefaultArgs(WCHAR *out)
{
    DWORD cb = 32 * sizeof(WCHAR);
    int i;
    if (RegGetValueW(HKEY_CURRENT_USER, SW_KEY, L"WuAuPrev", RRF_RT_REG_SZ, NULL, out, &cb) != ERROR_SUCCESS)
        lstrcpyW(out, L"absent absent");
    for (i = 1; i < 4; i++)  /* a saved pair that was itself a restricting mode would keep it on */
        if (!lstrcmpW(out, MODE_ARGS[i])) lstrcpyW(out, L"absent absent");
}

/* One state for Windows Updates, one Windows prompt per change:
   mode 0 + days 0 = automatic (Windows default); mode 0 + days N = paused N days, otherwise automatic;
   mode 1-3 = ask before download / ask before install / off, and any pause is cleared (the mode replaces it). */
static void WuChoose(int mode, DWORD days)
{
    WCHAR prev[32], na[12], ao[12], cmd[64], until[40];
    DWORD cb, err;
    char line[160];
    int cur = WuMode();
    if (!mode && !days && !cur && !WuPausedUntil(NULL)) return;  /* already automatic */
    if (mode || days) {
        if (!WuConfirm(L"WuModeWarned", WU_MODE_WARN)) return;
    }
    if (mode && cur == 0) {  /* first restricting mode: save what Windows had */
        cb = sizeof prev;
        if (RegGetValueW(HKEY_CURRENT_USER, SW_KEY, L"WuAuPrev", RRF_RT_REG_SZ, NULL, prev, &cb) != ERROR_SUCCESS) {
            AuTok(L"NoAutoUpdate", na);
            AuTok(L"AUOptions", ao);
            wsprintfW(prev, L"%s %s", na, ao);
            RegSetKeyValueW(HKEY_CURRENT_USER, SW_KEY, L"WuAuPrev", REG_SZ, prev, (lstrlenW(prev) + 1) * sizeof(WCHAR));
        }
    }
    if (mode) lstrcpyW(prev, MODE_ARGS[mode]);
    else AuDefaultArgs(prev);
    wsprintfW(cmd, L"/wu set %s %u", prev, days);
    err = Elevate(cmd);
    if (days) wsprintfA(line, "Windows Updates: paused for %u day(s), otherwise automatic", days);
    else wsprintfA(line, "Windows Updates: %s", MODE_NAMES[mode]);
    WuLog(line, err);
    if (err) {
        if (mode && cur == 0) RegDeleteKeyValueW(HKEY_CURRENT_USER, SW_KEY, L"WuAuPrev");
        return;
    }
    if (!mode) RegDeleteKeyValueW(HKEY_CURRENT_USER, SW_KEY, L"WuAuPrev");
    cb = sizeof until;  /* remember our pause end, so uninstall resumes only a pause Stripewake made */
    if (days && RegGetValueW(HKEY_LOCAL_MACHINE, WU_UX, L"PauseUpdatesExpiryTime", RRF_RT_REG_SZ, NULL, until, &cb) == ERROR_SUCCESS)
        RegSetKeyValueW(HKEY_CURRENT_USER, SW_KEY, L"WuPauseOurs", REG_SZ, until, (lstrlenW(until) + 1) * sizeof(WCHAR));
    else RegDeleteKeyValueW(HKEY_CURRENT_USER, SW_KEY, L"WuPauseOurs");
}

/* The Windows Updates status line, read from Windows each time:
   "Now: automatic", "Now: paused - 6 d 23 h 12 m 05 s left (until 2026-10-23 18:36)", "Now: off (no automatic updates)". */
static void WuStateText(WCHAR *out)
{
    static const WCHAR *NAMES[] = {L"automatic", L"ask before download", L"download, ask before install",
                                   L"off (no automatic updates)"};
    WCHAR cur[40], until[24];
    DWORD cb = sizeof cur, t;
    SYSTEMTIME u;
    FILETIME ft;
    ULONGLONG end, now = NowUtc();
    int mode = WuMode();
    if (mode || RegGetValueW(HKEY_LOCAL_MACHINE, WU_UX, L"PauseUpdatesExpiryTime", RRF_RT_REG_SZ, NULL, cur, &cb) != ERROR_SUCCESS ||
        lstrlenW(cur) < 19) {
        wsprintfW(out, L"Now: %s", NAMES[mode]);
        return;
    }
    memset(&u, 0, sizeof u);
    u.wYear = (WORD)Atou(cur); u.wMonth = (WORD)Atou(cur + 5); u.wDay = (WORD)Atou(cur + 8);
    u.wHour = (WORD)Atou(cur + 11); u.wMinute = (WORD)Atou(cur + 14); u.wSecond = (WORD)Atou(cur + 17);
    if (!SystemTimeToFileTime(&u, &ft)) { wsprintfW(out, L"Now: %s", NAMES[0]); return; }
    end = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    if (end <= now) { wsprintfW(out, L"Now: %s (the pause has ended)", NAMES[0]); return; }
    t = (DWORD)((end - now) / SEC);
    UntilText(end, until);
    wsprintfW(out, L"Now: paused - %u d %02u h %02u m %02u s left (until %s)", t / 86400, t % 86400 / 3600,
              t % 3600 / 60, t % 60, until);
}

/* Uninstall: undo only what Stripewake set. */
static void WuUndo(void)
{
    WCHAR ours[40], cur[40], prev[32], cmd[64];
    DWORD cb = sizeof ours, cb2 = sizeof cur;
    BOOL ourPause = FALSE, ourMode;
    if (RegGetValueW(HKEY_CURRENT_USER, SW_KEY, L"WuPauseOurs", RRF_RT_REG_SZ, NULL, ours, &cb) == ERROR_SUCCESS)
        ourPause = RegGetValueW(HKEY_LOCAL_MACHINE, WU_UX, L"PauseUpdatesExpiryTime", RRF_RT_REG_SZ, NULL, cur, &cb2) ==
                   ERROR_SUCCESS && !lstrcmpW(ours, cur);
    cb = sizeof prev;
    ourMode = RegGetValueW(HKEY_CURRENT_USER, SW_KEY, L"WuAuPrev", RRF_RT_REG_SZ, NULL, prev, &cb) == ERROR_SUCCESS && WuMode();
    if (ourPause || ourMode) {
        if (ourMode) AuDefaultArgs(prev);
        else { AuTok(L"NoAutoUpdate", ours); AuTok(L"AUOptions", cur); wsprintfW(prev, L"%s %s", ours, cur); }
        wsprintfW(cmd, L"/wu set %s %s", prev, ourPause ? L"0" : L"keep");
        WuLog("Windows Updates put back (uninstall)", Elevate(cmd));
    }
    RegDeleteKeyValueW(HKEY_CURRENT_USER, SW_KEY, L"WuPauseOurs");
    RegDeleteKeyValueW(HKEY_CURRENT_USER, SW_KEY, L"WuAuPrev");
    cb = sizeof prev;
    if (RegGetValueW(HKEY_CURRENT_USER, SW_KEY, L"WuNoRebootPrev", RRF_RT_REG_SZ, NULL, prev, &cb) == ERROR_SUCCESS) {
        if (prev[0] == L'1') lstrcpyW(prev, L"absent");
        wsprintfW(cmd, L"/wu noreboot %s", prev);
        if (WuNoReboot()) WuLog("no update restarts while signed in: off (uninstall)", Elevate(cmd));
        RegDeleteKeyValueW(HKEY_CURRENT_USER, SW_KEY, L"WuNoRebootPrev");
    }
}

/* ---- Screen saver: paused while ON (keep-awake requests do not stop a screen saver) ----
   Session-only change (no SPIF_UPDATEINIFILE), so your saved setting is never rewritten; restore.ini records the
   pause so a crash is repaired at the next start. A screen saver set by your organization's policy wins; reported. */
static void SsRestore(const char *why)
{
    WCHAR path[MAX_PATH];
    char line[160];
    g_ssHeld = FALSE;
    if (!AppPath(path, L"restore.ini") || GetPrivateProfileIntW(L"screensaver", L"active", 0, path) != 1) return;
    SystemParametersInfoW(SPI_SETSCREENSAVEACTIVE, TRUE, NULL, SPIF_SENDCHANGE);
    WritePrivateProfileStringW(L"screensaver", NULL, NULL, path);
    IniTidy(path);
    wsprintfA(line, "screen saver back on - %s", why);
    Log(line);
}

static void SsApply(void)
{
    WCHAR path[MAX_PATH];
    BOOL on = FALSE;
    if (!SystemParametersInfoW(SPI_GETSCREENSAVEACTIVE, 0, &on, 0) || !on || !AppPath(path, L"restore.ini")) return;
    WritePrivateProfileStringW(L"screensaver", L"active", L"1", path);
    WritePrivateProfileStringW(NULL, NULL, NULL, path);
    if (GetPrivateProfileIntW(L"screensaver", L"active", 0, path) != 1) { Log("screen saver pause skipped: could not save"); return; }
    if (!SystemParametersInfoW(SPI_SETSCREENSAVEACTIVE, FALSE, NULL, SPIF_SENDCHANGE)) {
        Log("screen saver pause FAILED");
        SsRestore("pause failed");
        return;
    }
    g_ssHeld = TRUE;
    Log("screen saver paused while ON");
}

static void RestoreAll(const char *why)
{
    int i;
    HoldRestore(&H_LID, why);
    HoldRestore(&H_LOCK, why);
    HoldRestore(&H_DISP, why);
    for (i = 0; i < 2; i++) HoldRestore(&H_PRES[i], why);
    for (i = 0; i < 3; i++) HoldRestore(&H_DIM[i], why);
    DynRestore(why);
    SsRestore(why);
}

/* ---- Dim when away (opt-in): external monitors over DDC/CI and a built-in laptop panel, to DimLevel % (30/50/70, default 50) of their
   brightness after DimAwayMins (2/5/10/15, default 5) without input, back at the first input. A monitor keeps its brightness in its own memory, so each value is
   saved to dim.ini BEFORE it is lowered and removed only after it is put back: a crash, a monitor that drops off in
   standby or a waking monitor that does not answer yet is put back later, and a saved monitor is never dimmed twice.
   DDC calls take tens to hundreds of ms, so they all run on one worker thread; the window only checks input. */
typedef struct { HANDLE h; WCHAR desc[128]; } PHYSMON;  /* PHYSICAL_MONITOR */
typedef BOOL (WINAPI *DDC_NUM)(HMONITOR, DWORD *);
typedef BOOL (WINAPI *DDC_GETPM)(HMONITOR, DWORD, PHYSMON *);
typedef BOOL (WINAPI *DDC_GETB)(HANDLE, DWORD *, DWORD *, DWORD *);
typedef BOOL (WINAPI *DDC_SETB)(HANDLE, DWORD);
typedef BOOL (WINAPI *DDC_FREE)(DWORD, PHYSMON *);
static DDC_NUM pDdcNum;
static DDC_GETPM pDdcGetPm;
static DDC_GETB pDdcGetB;
static DDC_SETB pDdcSetB;
static DDC_FREE pDdcFree;
static BOOL g_awayOpt, g_dimmed;          /* the option; dimming asked for (window side) */
static DWORD g_awaySecs = 300, g_dimInput;  /* idle seconds before dimming; last-input time when it dimmed */
static DWORD g_dimPct = 50, g_dimMins = 5;  /* the menu choices */
static HANDLE g_ddcEvt, g_ddcThread;
static volatile LONG g_dimWant, g_ddcQuit, g_ddcReq, g_ddcDone;  /* request counters: bounded waits at exit and sleep */
static DWORD g_ddcLeft;  /* worker: monitors present but not put back in the last pass */

/* dxva2 is loaded on first use, so the exe runs (and stays small) where DDC is missing. */
static BOOL DdcLoad(void)
{
    static HMODULE m;
    if (pDdcSetB) return TRUE;
    if (!m && !(m = LoadLibraryExW(L"dxva2.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32))) return FALSE;
    pDdcNum = (DDC_NUM)GetProcAddress(m, "GetNumberOfPhysicalMonitorsFromHMONITOR");
    pDdcGetPm = (DDC_GETPM)GetProcAddress(m, "GetPhysicalMonitorsFromHMONITOR");
    pDdcGetB = (DDC_GETB)GetProcAddress(m, "GetMonitorBrightness");
    pDdcFree = (DDC_FREE)GetProcAddress(m, "DestroyPhysicalMonitors");
    if (!pDdcNum || !pDdcGetPm || !pDdcGetB || !pDdcFree) return FALSE;
    pDdcSetB = (DDC_SETB)GetProcAddress(m, "SetMonitorBrightness");
    return pDdcSetB != NULL;
}

/* The monitor's own id (from its EDID, stable across restarts and replugging), else "\\.\DISPLAY2#0". */
static void DdcKey(const MONITORINFOEXW *mi, DWORD j, WCHAR *key)
{
    DISPLAY_DEVICEW dd;
    memset(&dd, 0, sizeof dd);
    dd.cb = sizeof dd;
    if (EnumDisplayDevicesW(mi->szDevice, j, &dd, EDD_GET_DEVICE_INTERFACE_NAME) && dd.DeviceID[0])
        lstrcpynW(key, dd.DeviceID, 200);
    else wsprintfW(key, L"%s#%u", mi->szDevice, j);
}

static BOOL CALLBACK DdcMonitor(HMONITOR hm, HDC dc, LPRECT rc, LPARAM dim)
{
    PHYSMON pm[4];
    MONITORINFOEXW mi;
    WCHAR path[MAX_PATH], key[200], num[12];
    DWORD n = 0, j, lo, cur, hi, low, saved;
    char line[220];
    (void)dc; (void)rc;
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(hm, (MONITORINFO *)&mi) || !AppPath(path, L"dim.ini")) return TRUE;
    if (!pDdcNum(hm, &n) || !n) return TRUE;
    if (n > 4) n = 4;
    if (!pDdcGetPm(hm, n, pm)) return TRUE;
    for (j = 0; j < n; j++) {
        DdcKey(&mi, j, key);
        saved = GetPrivateProfileIntW(L"dim", key, -1, path);
        if (!dim) {  /* put back only what was saved; the saved value goes only once it is set */
            if (saved == (DWORD)-1) continue;
            if (pDdcSetB(pm[j].h, saved)) {
                WritePrivateProfileStringW(L"dim", key, NULL, path);
                wsprintfA(line, "away dim: %S brightness back to %u", pm[j].desc, saved);
                Log(line);
            } else g_ddcLeft++;
            continue;
        }
        if (saved != (DWORD)-1 || !pDdcGetB(pm[j].h, &lo, &cur, &hi)) continue;  /* already dimmed, or no DDC: skip */
        low = cur * g_dimPct / 100;
        if (low < lo) low = lo;  /* never below the monitor's own minimum */
        if (low >= cur) continue;
        wsprintfW(num, L"%u", cur);
        WritePrivateProfileStringW(L"dim", key, num, path);
        WritePrivateProfileStringW(NULL, NULL, NULL, path);
        if ((DWORD)GetPrivateProfileIntW(L"dim", key, -1, path) != cur) {  /* not saved: do not touch it */
            WritePrivateProfileStringW(L"dim", key, NULL, path);
            continue;
        }
        if (!pDdcSetB(pm[j].h, low)) { WritePrivateProfileStringW(L"dim", key, NULL, path); continue; }
        wsprintfA(line, "away dim: %S brightness %u -> %u", pm[j].desc, cur, low);
        Log(line);
    }
    pDdcFree(n, pm);
    return TRUE;
}

/* Built-in laptop panel: Windows' panel brightness control (\\.\LCD), levels 0-100 for plugged in and battery.
   Same rule: save "ac*256+dc" first, set the nearest supported level at or above the target, put it back after. */
#define IOCTL_VIDEO_QUERY_SUPPORTED_BRIGHTNESS 0x230494  /* CTL_CODE(FILE_DEVICE_VIDEO, 0x125, BUFFERED, ANY) */
#define IOCTL_VIDEO_QUERY_DISPLAY_BRIGHTNESS   0x230498  /* 0x126 */
#define IOCTL_VIDEO_SET_DISPLAY_BRIGHTNESS     0x23049C  /* 0x127 */
typedef struct { UCHAR policy, ac, dc; } PANELBRI;  /* DISPLAY_BRIGHTNESS; policy 3 = both */

static UCHAR LcdLevel(const UCHAR *lv, DWORD n, DWORD cur)
{
    DWORD t = cur * g_dimPct / 100, i, best = cur;
    if (!n) return (UCHAR)t;
    for (i = 0; i < n; i++) if (lv[i] >= t && lv[i] < best) best = lv[i];
    return (UCHAR)best;
}

static void LcdPass(BOOL dim, const WCHAR *path)
{
    PANELBRI b;
    UCHAR lv[256];
    DWORD n = 0, saved, ac, dc;
    WCHAR num[12];
    char line[120];
    HANDLE h = CreateFileW(L"\\\\.\\LCD", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;  /* no built-in panel: skip */
    saved = GetPrivateProfileIntW(L"dim", L"lcd", -1, path);
    if (!dim) {
        if (saved != (DWORD)-1) {
            b.policy = 3; b.ac = (UCHAR)(saved >> 8); b.dc = (UCHAR)saved;
            if (DeviceIoControl(h, IOCTL_VIDEO_SET_DISPLAY_BRIGHTNESS, &b, sizeof b, NULL, 0, &n, NULL)) {
                WritePrivateProfileStringW(L"dim", L"lcd", NULL, path);
                wsprintfA(line, "away dim: built-in screen brightness back to %u (battery %u)", b.ac, b.dc);
                Log(line);
            } else g_ddcLeft++;
        }
    } else if (saved == (DWORD)-1 &&
               DeviceIoControl(h, IOCTL_VIDEO_QUERY_DISPLAY_BRIGHTNESS, NULL, 0, &b, sizeof b, &n, NULL)) {
        ac = b.ac; dc = b.dc;
        if (!DeviceIoControl(h, IOCTL_VIDEO_QUERY_SUPPORTED_BRIGHTNESS, NULL, 0, lv, sizeof lv, &n, NULL)) n = 0;
        b.policy = 3; b.ac = LcdLevel(lv, n, ac); b.dc = LcdLevel(lv, n, dc);
        if (b.ac < ac || b.dc < dc) {
            wsprintfW(num, L"%u", ac << 8 | dc);
            WritePrivateProfileStringW(L"dim", L"lcd", num, path);
            WritePrivateProfileStringW(NULL, NULL, NULL, path);
            if ((DWORD)GetPrivateProfileIntW(L"dim", L"lcd", -1, path) != (ac << 8 | dc) ||
                !DeviceIoControl(h, IOCTL_VIDEO_SET_DISPLAY_BRIGHTNESS, &b, sizeof b, NULL, 0, &n, NULL))
                WritePrivateProfileStringW(L"dim", L"lcd", NULL, path);
            else {
                wsprintfA(line, "away dim: built-in screen brightness %u -> %u (battery %u -> %u)", ac, b.ac, dc, b.dc);
                Log(line);
            }
        }
    }
    CloseHandle(h);
}

/* One pass over all screens: dim (save first) or put back. dim.ini goes when nothing is left in it.
   ponytail: a saved value of a monitor that never comes back stays in dim.ini; harmless, nothing matches it. */
static void DdcPass(BOOL dim)
{
    WCHAR path[MAX_PATH], s[4];
    g_ddcLeft = 0;
    if (!AppPath(path, L"dim.ini") || (!dim && GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)) return;
    if (DdcLoad()) EnumDisplayMonitors(NULL, NULL, DdcMonitor, dim);
    LcdPass(dim, path);
    WritePrivateProfileStringW(NULL, NULL, NULL, path);
    if (!GetPrivateProfileSectionW(L"dim", s, 4, path)) DeleteFileW(path);
}

static DWORD WINAPI DdcThread(void *p)
{
    LONG seq;
    int tries;
    (void)p;
    for (;;) {
        WaitForSingleObject(g_ddcEvt, INFINITE);
        seq = g_ddcReq;
        for (tries = 0;; tries++) {  /* a monitor just woken by input may not answer DDC for a second or two */
            LONG dim = g_dimWant;
            DdcPass(dim);
            if (dim || !g_ddcLeft || tries >= 10 || g_ddcQuit || g_dimWant) break;
            Sleep(1000);
        }
        InterlockedExchange(&g_ddcDone, seq);
        if (g_ddcQuit && seq == g_ddcReq) return 0;  /* exit: only after the last request (the put-back) is done */
    }
}

static void DdcRequest(BOOL dim)
{
    InterlockedExchange(&g_dimWant, dim);
    InterlockedIncrement(&g_ddcReq);
    if (g_ddcEvt) SetEvent(g_ddcEvt);
}

/* Bounded wait until the worker has done the latest request (or has ended). */
static void DdcWait(DWORD ms)
{
    LONG seq = g_ddcReq;
    DWORD t0 = GetTickCount();
    while (g_ddcThread && g_ddcDone - seq < 0 && GetTickCount() - t0 < ms &&
           WaitForSingleObject(g_ddcThread, 20) == WAIT_TIMEOUT) {}
}

/* Brightness back: at input, option off, display change, sleep, exit. */
static void AwayUndim(void)
{
    if (g_dimmed) {
        g_dimmed = FALSE;
        if (g_awayOpt) SetTimer(g_hwnd, TIMER_IDLE, 2000, NULL);
    }
    DdcRequest(FALSE);
}

static void AwayArm(void)
{
    g_awaySecs = RegDw(HKEY_CURRENT_USER, L"Software\\Stripewake", L"DimAwaySecs", g_dimMins * 60);  /* hidden test value */
    if (g_awaySecs < 5) g_awaySecs = 5;
    if (g_awayOpt) SetTimer(g_hwnd, TIMER_IDLE, g_dimmed ? 250 : 2000, NULL);
    else {
        KillTimer(g_hwnd, TIMER_IDLE);
        AwayUndim();
    }
}

static void AwayTick(void)
{
    LASTINPUTINFO li;
    QUERY_USER_NOTIFICATION_STATE q;
    li.cbSize = sizeof li;
    if (!g_awayOpt || !g_ddcThread || !GetLastInputInfo(&li)) return;
    if (g_dimmed) {
        if (li.dwTime != g_dimInput) { Log("away dim: input, putting brightness back"); AwayUndim(); }
        return;
    }
    if ((GetTickCount() - li.dwTime) / 1000 < g_awaySecs) return;
    if (SHQueryUserNotificationState(&q) == S_OK &&
        (q == QUNS_BUSY || q == QUNS_RUNNING_D3D_FULL_SCREEN || q == QUNS_PRESENTATION_MODE))
        return;  /* a full-screen video, game or presentation is not "away" */
    g_dimmed = TRUE;
    g_dimInput = li.dwTime;
    Log("away dim: no input, dimming");
    DdcRequest(TRUE);
    SetTimer(g_hwnd, TIMER_IDLE, 250, NULL);
}

/* While ON: the lock screen stays lit; the lid is held on a PC with a lid when the user picked a keep-running mode. */
/* Only hold a presence timeout this PC's power plan actually has (skip quietly on PCs without the setting). */
static BOOL PresenceSettingExists(const HOLD *h)
{
    GUID *active = NULL;
    DWORD ac, dc;
    BOOL ok = FALSE;
    if (PowerGetActiveScheme(NULL, &active) == ERROR_SUCCESS) {
        ok = HoldRead(h, active, &ac, &dc) && (ac || dc);  /* already never on both = nothing to hold */
        LocalFree(active);
    }
    return ok;
}

static void HoldsUpdate(const char *why)
{
    int i;
    BOOL wantLid = (g_lidPresent || g_lidTest) && g_on && g_lidMode && !g_lidTripped;
    if (H_LID.held && (!wantLid || H_LID.mode != g_lidMode)) HoldRestore(&H_LID, why);
    if (H_LOCK.held && !g_on) HoldRestore(&H_LOCK, why);
    if (g_dynHeld && !(g_on && g_dynOpt)) DynRestore(why);
    if (g_on && g_dynOpt && !g_dynHeld) DynApply();
    if (g_ssHeld && !(g_on && g_ssOpt)) SsRestore(why);
    if (g_on && g_ssOpt && !g_ssHeld) SsApply();
    if (g_on && !H_LOCK.held) HoldApply(&H_LOCK, TRUE);
    if (H_DISP.held && !(g_on && g_display)) HoldRestore(&H_DISP, why);
    if (g_on && g_display && !H_DISP.held && PresenceSettingExists(&H_DISP)) HoldApply(&H_DISP, TRUE);
    for (i = 0; i < 3; i++) {
        BOOL want = g_on && g_display && (g_noDim || g_awayOpt);  /* Dim when away replaces Windows' own dim */
        if (H_DIM[i].held && !want) HoldRestore(&H_DIM[i], why);
        if (want && !H_DIM[i].held && PresenceSettingExists(&H_DIM[i])) HoldApply(&H_DIM[i], TRUE);
    }
    Apply();  /* falls back to the display request only if the display hold could not be made */
    for (i = 0; i < 2; i++) {
        if (H_PRES[i].held && !g_on) HoldRestore(&H_PRES[i], why);
        if (g_on && !H_PRES[i].held && PresenceSettingExists(&H_PRES[i])) HoldApply(&H_PRES[i], TRUE);
    }
    if (wantLid && !H_LID.held) HoldApply(&H_LID, g_lidMode == 2);
}

static void ArmTimer(void)
{
    ULONGLONG now = NowUtc(), ms = g_end > now ? (g_end - now) / 10000 + 1 : 1;
    if (ms > 0x7FFFFFF0) ms = 0x7FFFFFF0;  /* about 24 days; WM_TIMER re-arms for the rest */
    SetTimer(g_hwnd, TIMER_END, (UINT)ms, NULL);
}

static void SetOn(BOOL on, ULONGLONG end, const char *why)
{
    char line[160];
    SYSTEMTIME e;

    KillTimer(g_hwnd, TIMER_END);
    g_timerId = 0;  /* Command() sets it again for a menu timer */
    g_on = on;
    g_end = on ? end : 0;
    g_since = NowUtc();
    Apply();
    if (g_end) ArmTimer();

    if (!on) lstrcpyA(line, "OFF");
    else if (!g_end) lstrcpyA(line, "ON (indefinite)");
    else {
        ToLocal(g_end, &e);
        wsprintfA(line, "ON until %04u-%02u-%02u %02u:%02u:%02u", e.wYear, e.wMonth, e.wDay, e.wHour, e.wMinute,
                  e.wSecond);
    }
    if (on) lstrcatA(line, g_display ? ", display on" : ", display may sleep");
    wsprintfA(line + lstrlenA(line), " - %s", why);
    Log(line);
    if (on) g_lidTripped = FALSE;
    HoldsUpdate(why);
    Tray(NIM_MODIFY);
}

/* 0 = no autostart, 1 = this user (HKCU), 2 = all users (HKLM, set by the installer). */
static int LogonState(void)
{
    WCHAR v[MAX_PATH + 4];
    DWORD cb = sizeof v;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, RUN_KEY, RUN_VALUE, RRF_RT_REG_SZ, NULL, v, &cb) == ERROR_SUCCESS) return 2;
    cb = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE, RRF_RT_REG_SZ, NULL, v, &cb) == ERROR_SUCCESS) return 1;
    return 0;
}

static void ToggleLogon(void)
{
    WCHAR cmd[MAX_PATH + 4];
    DWORD n;
    int s = LogonState();
    if (s == 2) return;
    if (s == 1) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE);
        Log("start at logon: off (this user)");
        return;
    }
    cmd[0] = L'"';
    n = GetModuleFileNameW(NULL, cmd + 1, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    lstrcatW(cmd, L"\"");
    if (RegSetKeyValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE, REG_SZ, cmd,
                        (lstrlenW(cmd) + 1) * sizeof(WCHAR)) == ERROR_SUCCESS)
        Log("start at logon: on (this user)");
}

/* ---- Status check: read-only report of lock and blank-screen risks ---- */

static DWORD RegDw(HKEY root, LPCWSTR key, LPCWSTR val, DWORD def)
{
    DWORD v, cb = sizeof v;
    return RegGetValueW(root, key, val, RRF_RT_REG_DWORD, NULL, &v, &cb) == ERROR_SUCCESS ? v : def;
}

static BOOL RegSz(HKEY root, LPCWSTR key, LPCWSTR val, WCHAR *out, DWORD cch)
{
    DWORD cb = cch * sizeof(WCHAR);
    out[0] = 0;
    return RegGetValueW(root, key, val, RRF_RT_REG_SZ, NULL, out, &cb) == ERROR_SUCCESS && out[0];
}

static BOOL KeyExists(LPCWSTR key)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &k) != ERROR_SUCCESS) return FALSE;
    RegCloseKey(k);
    return TRUE;
}

static DWORD Atou(const WCHAR *s)
{
    DWORD v = 0;
    while (*s >= L'0' && *s <= L'9') v = v * 10 + (*s++ - L'0');
    return v;
}

static void Secs(DWORD s, WCHAR *out)
{
    if (!s) lstrcpyW(out, L"never");
    else if (s < 60) wsprintfW(out, L"%u s", s);
    else if (s % 60) wsprintfW(out, L"%u min %u s", s / 60, s % 60);
    else if (s % 3600 || s < 3600) wsprintfW(out, L"%u min", s / 60);
    else wsprintfW(out, L"%u h", s / 3600);
}

/* One row of the check window: level 2 = can still interrupt, 1 = worth knowing, 0 = fine; fix = what Change it does
   ("#dyn" / "#ss" / "#noreboot" = Stripewake turns it on itself, else a Windows setting to open); cat = its group. */
enum { CAT_LOCK, CAT_SCREEN, CAT_SLEEP, CAT_UPDATE, CAT_LID, CAT_N };
static const WCHAR *CAT_NAME[CAT_N] = {L"Locking", L"Screen", L"Sleep", L"Windows Update", L"Laptop lid"};
typedef struct { int lvl, cat; WCHAR text[220]; const WCHAR *fix; } CHECKROW;
static CHECKROW g_rows[48];
static int g_nrows, g_cat;
static DWORD g_risks;
static HFONT BoldMenuFont(void);
static void Command(UINT id);

static void AddF(const WCHAR *tag, const WCHAR *text, const WCHAR *fix)
{
    if (g_nrows >= (int)(sizeof g_rows / sizeof g_rows[0])) return;
    g_rows[g_nrows].lvl = tag[0] == L'R' ? 2 : tag[0] == L'n' ? 1 : 0;
    g_rows[g_nrows].cat = g_cat;
    lstrcpynW(g_rows[g_nrows].text, text, 220);
    g_rows[g_nrows].fix = fix;
    g_nrows++;
}

static void Add(const WCHAR *tag, const WCHAR *text) { AddF(tag, text, NULL); }

static const GUID SUB_NONE_G   = {0xfea3413e, 0x7e05, 0x4911, {0x9a, 0x71, 0x70, 0x03, 0x31, 0xf1, 0xc2, 0x94}};
static const GUID SUB_VIDEO_G  = {0x7516b95f, 0xf776, 0x4464, {0x8c, 0x53, 0x06, 0x16, 0x7f, 0x40, 0xcc, 0x99}};
static const GUID SUB_SLEEP_G  = {0x238c9fa8, 0x0aad, 0x41ed, {0x83, 0xf4, 0x97, 0xbe, 0x24, 0x2c, 0x8f, 0x20}};
static const GUID SUB_PRES_G   = {0x8619b916, 0xe004, 0x4dd8, {0x9b, 0x66, 0xda, 0xe8, 0x6f, 0x80, 0x66, 0x98}};

/* kind: 0 = sleep/hibernate Stripewake stops while ON, 5 = screen off (held at never while ON), 6 = dimming (left on
   purpose), 1 = must be yes, 2 = presence sensor, 3 = password after sleep, 4 = unattended-wake sleep, 7 = lock screen.
   Rows show in this order inside each group. */
static const struct { const WCHAR *name; const GUID *sub; GUID set; int kind, cat; } POWER[] = {
    {L"Screen turns off after", &SUB_VIDEO_G, {0x3c0bc021, 0xc8a8, 0x4e07, {0xa9, 0x73, 0x6b, 0x14, 0xcb, 0xcb, 0x2b, 0x7e}}, 5, CAT_SCREEN},
    {L"Screen dims after", &SUB_VIDEO_G, {0x17aaa29b, 0x8b43, 0x4b94, {0xaa, 0xfe, 0x35, 0xf6, 0x4d, 0xaa, 0xf1, 0xee}}, 6, CAT_SCREEN},
    {L"Screen turns off at the lock screen", &SUB_VIDEO_G, {0x8ec4b3a5, 0x6868, 0x48c2, {0xbe, 0x75, 0x4f, 0x30, 0x44, 0xbe, 0x88, 0xa7}}, 7, CAT_SCREEN},
    {L"Presence sensor: screen off when you walk away", &SUB_PRES_G, {0x0a7d6ab6, 0xac83, 0x4ad1, {0x82, 0x82, 0xec, 0xa5, 0xb5, 0x83, 0x08, 0xf3}}, 2, CAT_SCREEN},
    {L"Presence sensor: screen off when you look away", &SUB_PRES_G, {0xee16691e, 0x6ab3, 0x4619, {0xbb, 0x48, 0x1c, 0x77, 0xc9, 0x35, 0x7e, 0x5a}}, 2, CAT_SCREEN},
    {L"Presence sensor: screen dims when you walk away", &SUB_PRES_G, {0xa79c8e0e, 0xf271, 0x482d, {0x8f, 0x8a, 0x5d, 0xb9, 0xa1, 0x83, 0x12, 0xde}}, 2, CAT_SCREEN},
    {L"Presence sensor: screen dims when you look away", &SUB_PRES_G, {0xcf8c6097, 0x12b8, 0x4279, {0xbb, 0xdd, 0x44, 0x60, 0x1e, 0xe5, 0x20, 0x9d}}, 2, CAT_SCREEN},
    {L"PC sleeps after", &SUB_SLEEP_G, {0x29f6c1db, 0x86da, 0x48c5, {0x9f, 0xdb, 0xf2, 0xb6, 0x7b, 0x1f, 0x44, 0xda}}, 0, CAT_SLEEP},
    {L"PC hibernates after", &SUB_SLEEP_G, {0x9d7815a6, 0x7ee4, 0x497e, {0x88, 0x88, 0x51, 0x5a, 0x05, 0xf0, 0x23, 0x64}}, 0, CAT_SLEEP},
    {L"Sleep after Windows wakes the PC on its own", &SUB_SLEEP_G, {0x7bc4a2f9, 0xd8fc, 0x4469, {0xb0, 0x7b, 0x33, 0xeb, 0x78, 0x5a, 0xac, 0xa0}}, 4, CAT_SLEEP},
    {L"Windows lets apps keep the PC awake", &SUB_SLEEP_G, {0xa4b195f5, 0x8225, 0x47d8, {0x80, 0x12, 0x9d, 0x41, 0x36, 0x97, 0x86, 0xe2}}, 1, CAT_SLEEP},
    {L"Password needed after sleep", &SUB_NONE_G, {0x0e796bdb, 0x100d, 0x47d6, {0xa2, 0xd5, 0xf7, 0xd2, 0xda, 0xa5, 0x1f, 0x51}}, 3, CAT_LOCK},
};
#define P_PRES_OFF 3   /* POWER rows held as H_PRES[0] / H_PRES[1] */
#define P_PRES_LOFF 4

/* TRUE while a running Stripewake holds this power-plan setting (its restore.ini section is the record). */
static BOOL HeldInIni(const WCHAR *sect)
{
    WCHAR path[MAX_PATH], gs[48];
    return AppPath(path, L"restore.ini") && GetPrivateProfileStringW(sect, L"plan", L"", gs, 48, path) > 0;
}

/* The saved plugged-in value of a held setting (from restore.ini). */
static DWORD IniAc(const WCHAR *sect)
{
    WCHAR path[MAX_PATH];
    return AppPath(path, L"restore.ini") ? GetPrivateProfileIntW(sect, L"ac", 0, path) : 0;
}

/* Lid lines. The restore file is the record of an active hold, so /check works from a second process too. */
static void LidStatus(void)
{
    WCHAR path[MAX_PATH], t[220];
    GUID *active = NULL;
    DWORD ac, dc, mode;

    g_cat = CAT_LID;
    if (!g_lidPresent && !g_lidTest) { Add(L"ok", L"This PC has no lid, so the laptop lid options are hidden."); return; }
    if (AppPath(path, L"restore.ini") && GetPrivateProfileIntW(L"lid", L"ac", -1, path) != (UINT)-1) {
        ac = GetPrivateProfileIntW(L"lid", L"ac", 4, path);
        dc = GetPrivateProfileIntW(L"lid", L"dc", 4, path);
        mode = GetPrivateProfileIntW(L"lid", L"mode", 1, path);
        wsprintfW(t, L"Closing the lid: keeps running %s while ON - your setting (plugged in %s, on battery %s) comes back after.",
                  mode == 2 ? L"always (battery: heat, drain)" : L"when plugged in", LID_W[Lid4(ac)], LID_W[Lid4(dc)]);
        Add(mode == 2 ? L"note" : L"ok", t);
        return;
    }
    if (PowerGetActiveScheme(NULL, &active) == ERROR_SUCCESS) {
        if (HoldRead(&H_LID, active, &ac, &dc)) {
            wsprintfW(t, L"Closing the lid: plugged in %s, on battery %s. Menu: Lid closed > keep running.",
                      LID_W[Lid4(ac)], LID_W[Lid4(dc)]);
            Add(ac || dc ? L"note" : L"ok", t);
        }
        LocalFree(active);
    }
}

static void StatusCheck(void)
{
    static const WCHAR WINLOGON[] = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";
    static const WCHAR POLICY[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System";
    static const WCHAR DESK[] = L"Control Panel\\Desktop";
    static const WCHAR DESKPOL[] = L"Software\\Policies\\Microsoft\\Windows\\Control Panel\\Desktop";
    WCHAR t[220], v[24], v2[24], exe[MAX_PATH], active[8], secure[8], timeout[16];
    GUID *scheme = NULL;
    DWORD d, i, risks = 0;
    HKEY ss;

    g_nrows = 0;

    /* ---- Locking ---- */
    g_cat = CAT_LOCK;
    if (g_dynHeld) Add(L"ok", L"Phone walk-away lock (Dynamic Lock): paused while Stripewake is ON.");
    else if (RegDw(HKEY_CURRENT_USER, WINLOGON, L"EnableGoodbye", 0)) {
        if (g_dynOpt) Add(L"ok", L"Phone walk-away lock (Dynamic Lock): on - Stripewake pauses it whenever it is ON.");
        else {
            AddF(L"RISK", L"Phone walk-away lock (Dynamic Lock): on - Windows locks when your phone leaves. Stripewake can pause it.", L"#dyn");
            risks++;
        }
    } else Add(L"ok", L"Phone walk-away lock (Dynamic Lock): off.");

    d = RegDw(HKEY_LOCAL_MACHINE, POLICY, L"InactivityTimeoutSecs", 0);
    if (d) {
        Secs(d, v);
        wsprintfW(t, L"Company idle-lock policy: locks after %s idle - only your IT admin can exempt this PC.", v);
        Add(L"RISK", t);
        risks++;
    } else Add(L"ok", L"Company idle-lock policy: none.");

    /* A policy screen saver overrides the user one. Keep-awake requests do not stop a screen saver. */
    ss = RegSz(HKEY_CURRENT_USER, DESKPOL, L"ScreenSaveActive", active, 8) ? HKEY_CURRENT_USER : NULL;
    if (ss) {
        RegSz(ss, DESKPOL, L"ScreenSaverIsSecure", secure, 8);
        RegSz(ss, DESKPOL, L"ScreenSaveTimeOut", timeout, 16);
        RegSz(ss, DESKPOL, L"SCRNSAVE.EXE", exe, MAX_PATH);
    } else {
        RegSz(HKEY_CURRENT_USER, DESK, L"ScreenSaveActive", active, 8);
        RegSz(HKEY_CURRENT_USER, DESK, L"ScreenSaverIsSecure", secure, 8);
        RegSz(HKEY_CURRENT_USER, DESK, L"ScreenSaveTimeOut", timeout, 16);
        RegSz(HKEY_CURRENT_USER, DESK, L"SCRNSAVE.EXE", exe, MAX_PATH);
    }
    d = Atou(timeout);
    if (ss && active[0] == L'1' && d) {
        Secs(d, v);
        wsprintfW(t, L"Company screen saver policy: starts after %s%s - Stripewake cannot pause it.", v,
                  secure[0] == L'1' ? L" and locks" : L"");
        Add(L"RISK", t);
        risks++;
    } else if (g_ssHeld) Add(L"ok", L"Screen saver: paused while Stripewake is ON.");
    else if (active[0] == L'1' && exe[0] && d) {
        Secs(d, v);
        wsprintfW(t, L"Screen saver: starts after %s%s - %s", v, secure[0] == L'1' ? L" and locks" : L"",
                  g_ssOpt ? L"Stripewake pauses it whenever it is ON." : L"Stripewake can pause it.");
        AddF(g_ssOpt ? L"ok" : L"RISK", t, g_ssOpt ? L"ms-settings:lockscreen" : L"#ss");
        if (!g_ssOpt) risks++;
    } else Add(L"ok", L"Screen saver: none set.");

    /* ---- Windows Update ---- */
    g_cat = CAT_UPDATE;
    if (KeyExists(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update\\RebootRequired") ||
        KeyExists(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Component Based Servicing\\RebootPending")) {
        AddF(WuNoReboot() ? L"note" : L"RISK", WuNoReboot()
             ? L"Update restart: one is waiting - Windows will not do it while you are signed in. Restart when it suits you."
             : L"Update restart: one is waiting and Windows can do it on its own. Stripewake can block it while you are signed in.",
             WuNoReboot() ? L"ms-settings:windowsupdate" : L"#noreboot");
        if (!WuNoReboot()) risks++;
    } else Add(L"ok", L"Update restart: none waiting.");
    if (WuNoReboot()) Add(L"ok", L"Restarts for updates while you are signed in: blocked.");
    else AddF(L"note", L"Restarts for updates while you are signed in: allowed outside your active hours. Stripewake can block them.",
              L"#noreboot");
    {
        WCHAR st[160];
        WuStateText(st);
        wsprintfW(t, L"Updates: %s.", st + 5);  /* "Now: ..." without the prefix */
        Add(WuMode() || WuPausedUntil(NULL) ? L"note" : L"ok", t);
    }

    /* ---- Screen and Sleep (power plan, plugged-in values) ---- */
    if (PowerGetActiveScheme(NULL, &scheme) == ERROR_SUCCESS) {
        for (i = 0; i < sizeof POWER / sizeof POWER[0]; i++) {
            if (PowerReadACValueIndex(NULL, scheme, POWER[i].sub, &POWER[i].set, &d) != ERROR_SUCCESS) continue;
            g_cat = POWER[i].cat;
            Secs(d, v);
            switch (POWER[i].kind) {
            case 1:
                if (d) {
                    wsprintfW(t, L"%s: yes (Stripewake relies on this to keep the PC awake).", POWER[i].name);
                    Add(L"ok", t);
                } else {
                    wsprintfW(t, L"%s: no - Stripewake cannot keep the PC awake until this is Yes.", POWER[i].name);
                    AddF(L"RISK", t, L"powercfg.cpl");
                    risks++;
                }
                break;
            case 3:
                wsprintfW(t, L"%s: %s (only matters after the PC sleeps, which Stripewake prevents).", POWER[i].name, d ? L"yes" : L"no");
                AddF(L"ok", t, L"ms-settings:signinoptions");
                break;
            case 0:
                if (d) wsprintfW(t, L"%s: %s - Stripewake stops it while ON.", POWER[i].name, v);
                else wsprintfW(t, L"%s: never.", POWER[i].name);
                AddF(L"ok", t, L"ms-settings:powersleep");
                break;
            case 5:  /* screen off: held at never while ON */
                if (HeldInIni(H_DISP.sect)) {
                    Secs(IniAc(H_DISP.sect), v2);
                    wsprintfW(t, L"%s: never while ON (back to %s after).", POWER[i].name, v2);
                } else if (d) wsprintfW(t, L"%s: %s - Stripewake keeps it on while ON.", POWER[i].name, v);
                else wsprintfW(t, L"%s: never.", POWER[i].name);
                AddF(L"ok", t, L"ms-settings:powersleep");
                break;
            case 6:  /* dimming: allowed unless "Stop dimming too" */
                if (HeldInIni(H_DIM[0].sect)) {
                    Secs(IniAc(H_DIM[0].sect), v2);
                    wsprintfW(t, L"%s: never while ON (held by Stripewake; back to %s after).", POWER[i].name, v2);
                } else if (d) wsprintfW(t, L"%s: %s (allowed: saves power; moving the mouse brings it back).", POWER[i].name, v);
                else wsprintfW(t, L"%s: never.", POWER[i].name);
                AddF(L"ok", t, L"ms-settings:powersleep");
                break;
            case 7:  /* lock-screen display */
                if (HeldInIni(H_LOCK.sect)) {
                    Secs(IniAc(H_LOCK.sect), v2);
                    wsprintfW(t, L"%s: never while ON (back to %s after).", POWER[i].name, v2);
                } else wsprintfW(t, L"%s: %s%s", POWER[i].name, v, d ? L" - Stripewake keeps it on while ON." : L".");
                AddF(L"ok", t, L"ms-settings:powersleep");
                break;
            case 4:
                wsprintfW(t, L"%s: %s (not while Stripewake keeps it awake).", POWER[i].name, v);
                AddF(L"ok", t, L"ms-settings:powersleep");
                break;
            default: {  /* presence sensor: the screen-off pair is held at never while ON; dimming is allowed */
                int hp = i == P_PRES_OFF ? 0 : i == P_PRES_LOFF ? 1 : -1;
                if (hp >= 0 && HeldInIni(H_PRES[hp].sect)) {
                    Secs(IniAc(H_PRES[hp].sect), v2);
                    wsprintfW(t, L"%s: never while ON (back to %s after).", POWER[i].name, v2);
                } else if (!d) wsprintfW(t, L"%s: never.", POWER[i].name);
                else if (hp >= 0) wsprintfW(t, L"%s: %s - Stripewake holds it at never while ON.", POWER[i].name, v);
                else wsprintfW(t, L"%s: %s (allowed: dims only, the screen stays on).", POWER[i].name, v);
                AddF(L"ok", t, L"ms-settings:presence");
            }
            }
        }
        LocalFree(scheme);
    } else { g_cat = CAT_SLEEP; Add(L"note", L"Could not read the active power plan."); }

    LidStatus();
    g_risks = risks;
}

/* ---- The check window: grouped (Locking, Screen, Sleep, Windows Update, Laptop lid), worst first in each ---- */
static const WCHAR *LVL_TEXT[] = {L"OK", L"Info", L"Action needed"};
static const COLORREF LVL_COLOR[] = {RGB(0, 120, 60), RGB(170, 95, 0), RGB(190, 20, 20)};

static void CheckFill(HWND dlg)
{
    HWND lv = GetDlgItem(dlg, IDC_LIST);
    LVITEMW it;
    LVGROUP g;
    WCHAR head[160], gname[64];
    int lvl, i, n = 0, cat, cnt, bad;
    StatusCheck();
    ListView_DeleteAllItems(lv);
    ListView_RemoveAllGroups(lv);
    ListView_EnableGroupView(lv, TRUE);
    for (cat = 0; cat < CAT_N; cat++) {
        for (cnt = bad = i = 0; i < g_nrows; i++)
            if (g_rows[i].cat == cat) { cnt++; if (g_rows[i].lvl == 2) bad++; }
        if (!cnt) continue;
        if (bad) wsprintfW(gname, L"%s - %u need%s action", CAT_NAME[cat], bad, bad == 1 ? L"s" : L"");
        else lstrcpyW(gname, CAT_NAME[cat]);
        memset(&g, 0, sizeof g);
        g.cbSize = sizeof g;
        g.mask = LVGF_HEADER | LVGF_GROUPID | LVGF_STATE;
        g.pszHeader = gname;
        g.iGroupId = cat;
        g.stateMask = LVGS_COLLAPSIBLE;
        g.state = LVGS_COLLAPSIBLE;
        ListView_InsertGroup(lv, -1, &g);
        for (lvl = 2; lvl >= 0; lvl--)
            for (i = 0; i < g_nrows; i++) {
                if (g_rows[i].cat != cat || g_rows[i].lvl != lvl) continue;
                memset(&it, 0, sizeof it);
                it.mask = LVIF_TEXT | LVIF_PARAM | LVIF_GROUPID;
                it.iItem = n++;
                it.pszText = (WCHAR *)LVL_TEXT[lvl];
                it.lParam = i;
                it.iGroupId = cat;
                ListView_InsertItem(lv, &it);
                ListView_SetItemText(lv, it.iItem, 1, g_rows[i].text);
                ListView_SetItemText(lv, it.iItem, 2, !g_rows[i].fix || !lvl ? L"" : g_rows[i].fix[0] == L'#' ? L"Turn on in Stripewake" : L"Open Windows setting");
            }
    }
    if (g_risks) wsprintfW(head, L"%u thing%s can still interrupt Stripewake. Select %s and click Turn on or Open setting.",
                           g_risks, g_risks == 1 ? L"" : L"s", g_risks == 1 ? L"it" : L"each");
    else lstrcpyW(head, L"Nothing can interrupt Stripewake right now.");
    SetDlgItemTextW(dlg, IDC_HEAD, head);
    SendDlgItemMessageW(dlg, IDC_HEAD, WM_SETFONT, (WPARAM)BoldMenuFont(), TRUE);
    EnableWindow(GetDlgItem(dlg, IDC_FIX), FALSE);
}

static void CheckFix(HWND dlg)
{
    HWND lv = GetDlgItem(dlg, IDC_LIST);
    LVITEMW it;
    int sel = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
    if (sel < 0) return;
    memset(&it, 0, sizeof it);
    it.mask = LVIF_PARAM;
    it.iItem = sel;
    ListView_GetItem(lv, &it);
    if (!g_rows[it.lParam].fix) return;
    if (g_rows[it.lParam].fix[0] == L'#') {  /* something Stripewake itself can turn on: do it, then re-check */
        const WCHAR *a = g_rows[it.lParam].fix + 1;
        Command(!lstrcmpW(a, L"dyn") ? ID_DYN : !lstrcmpW(a, L"ss") ? ID_SS : ID_NOREBOOT);
        CheckFill(dlg);
        return;
    }
    SetWindowPos(dlg, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);  /* let Settings show above */
    ShellExecuteW(dlg, L"open", g_rows[it.lParam].fix, NULL, NULL, SW_SHOWNORMAL);
}

/* Lay the window out for its current size (any screen size or scaling): list fills, buttons stay at the bottom. */
static int Dlu(HWND dlg, int v, BOOL vert)
{
    RECT r = {0, 0, v, v};
    MapDialogRect(dlg, &r);
    return vert ? r.bottom : r.right;
}

static void CheckLayout(HWND dlg)
{
    RECT cr, lr;
    HWND lv = GetDlgItem(dlg, IDC_LIST);
    int w, h, m, top, bh, bw, gap, col0, col2;
    GetClientRect(dlg, &cr);
    w = cr.right; h = cr.bottom;
    m = Dlu(dlg, 8, FALSE); gap = Dlu(dlg, 8, TRUE); top = Dlu(dlg, 44, TRUE);
    bh = Dlu(dlg, 14, TRUE); bw = Dlu(dlg, 64, FALSE);
    MoveWindow(GetDlgItem(dlg, IDC_HEAD), m, Dlu(dlg, 8, TRUE), w - 2 * m, Dlu(dlg, 12, TRUE), TRUE);
    MoveWindow(GetDlgItem(dlg, IDC_DESC), m, Dlu(dlg, 22, TRUE), w - 2 * m, Dlu(dlg, 18, TRUE), TRUE);
    MoveWindow(lv, m, top, w - 2 * m, h - top - bh - 2 * gap, TRUE);
    MoveWindow(GetDlgItem(dlg, IDC_FIX), m, h - gap - bh, bw, bh, TRUE);
    MoveWindow(GetDlgItem(dlg, IDC_RECHECK), m + bw + Dlu(dlg, 4, FALSE), h - gap - bh, bw, bh, TRUE);
    MoveWindow(GetDlgItem(dlg, IDCANCEL), w - m - bw, h - gap - bh, bw, bh, TRUE);
    GetClientRect(lv, &lr);
    col0 = Dlu(dlg, 52, FALSE); col2 = Dlu(dlg, 72, FALSE);
    ListView_SetColumnWidth(lv, 0, col0);
    ListView_SetColumnWidth(lv, 2, col2);
    ListView_SetColumnWidth(lv, 1, lr.right - col0 - col2 > 80 ? lr.right - col0 - col2 : 80);
}

/* Never larger than the screen it opens on (small laptops, 1366x768, 125-150% scaling): shrink and centre. */
static void CheckFitScreen(HWND dlg)
{
    MONITORINFO mi;
    RECT wr, wa;
    int w, h;
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(MonitorFromWindow(dlg, MONITOR_DEFAULTTONEAREST), &mi)) return;
    wa = mi.rcWork;
    GetWindowRect(dlg, &wr);
    w = wr.right - wr.left; h = wr.bottom - wr.top;
    if (w > wa.right - wa.left - 16) w = wa.right - wa.left - 16;
    if (h > wa.bottom - wa.top - 16) h = wa.bottom - wa.top - 16;
    SetWindowPos(dlg, NULL, wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

static INT_PTR CALLBACK CheckProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_SIZE) { CheckLayout(dlg); return TRUE; }
    if (msg == WM_GETMINMAXINFO) {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = Dlu(dlg, 300, FALSE);
        mm->ptMinTrackSize.y = Dlu(dlg, 160, TRUE);
        return TRUE;
    }
    static const WCHAR *COLS[] = {L"Status", L"What", L"Change it"};
    LVCOLUMNW c;
    int i;
    if (msg == WM_INITDIALOG) {
        HWND lv = GetDlgItem(dlg, IDC_LIST);
        ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
        for (i = 0; i < 3; i++) {
            memset(&c, 0, sizeof c);
            c.mask = LVCF_TEXT | LVCF_WIDTH;
            c.pszText = (WCHAR *)COLS[i];
            c.cx = 100;  /* real widths come from CheckLayout */
            ListView_InsertColumn(lv, i, &c);
        }
        {  /* the tiger for the current state (awake / timer / off), same as the tray */
            int s = !g_on ? 2 : g_end ? 1 : 0;
            if (!g_hwnd) s = HeldInIni(H_LOCK.sect) ? 0 : 2;  /* /check from a second process: the running copy holds the lock screen while ON */
            static const WORD IDS[] = {IDI_ON, IDI_TIMER, IDI_OFF};
            SendMessageW(dlg, WM_SETICON, ICON_SMALL, (LPARAM)g_icon[s]);
            SendMessageW(dlg, WM_SETICON, ICON_BIG, (LPARAM)LoadIconW(g_inst, MAKEINTRESOURCEW(IDS[s])));
        }
        CheckFill(dlg);
        CheckFitScreen(dlg);
        CheckLayout(dlg);
        SetWindowPos(dlg, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);  /* stays on top of other windows */
        return TRUE;
    }
    if (msg == WM_ACTIVATE && LOWORD(wp) != WA_INACTIVE) {  /* back on top after a Fix let Settings come first */
        SetWindowPos(dlg, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return FALSE;
    }
    if (msg == WM_NOTIFY && ((NMHDR *)lp)->idFrom == IDC_LIST) {
        NMHDR *h = (NMHDR *)lp;
        if (h->code == NM_CUSTOMDRAW) {  /* colour the Status column by level */
            NMLVCUSTOMDRAW *cd = (NMLVCUSTOMDRAW *)lp;
            LRESULT res = CDRF_DODEFAULT;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) res = CDRF_NOTIFYITEMDRAW;
            else if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) res = CDRF_NOTIFYSUBITEMDRAW;
            else if (cd->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
                int row = (int)cd->nmcd.lItemlParam;
                cd->clrText = cd->iSubItem == 0 ? LVL_COLOR[g_rows[row].lvl]
                            : cd->iSubItem == 2 ? RGB(0, 90, 180) : GetSysColor(COLOR_WINDOWTEXT);
                res = CDRF_NEWFONT;
            }
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, res);
            return TRUE;
        }
        if (h->code == LVN_ITEMCHANGED) {
            int sel = ListView_GetNextItem(h->hwndFrom, -1, LVNI_SELECTED);
            LVITEMW it;
            BOOL can = FALSE;
            const WCHAR *fx = NULL;
            if (sel >= 0) {
                memset(&it, 0, sizeof it);
                it.mask = LVIF_PARAM;
                it.iItem = sel;
                ListView_GetItem(h->hwndFrom, &it);
                fx = g_rows[it.lParam].fix;
                can = fx != NULL;
            }
            EnableWindow(GetDlgItem(dlg, IDC_FIX), can);
            SetDlgItemTextW(dlg, IDC_FIX, fx && fx[0] == L'#' ? L"Turn on" : L"Open setting");
        }
        if (h->code == NM_DBLCLK) CheckFix(dlg);
        return FALSE;
    }
    if (msg != WM_COMMAND) return FALSE;
    switch (LOWORD(wp)) {
    case IDC_FIX: CheckFix(dlg); return TRUE;
    case IDC_RECHECK: CheckFill(dlg); return TRUE;
    case IDCANCEL: case IDOK: EndDialog(dlg, 0); return TRUE;
    }
    return FALSE;
}

static void CheckWindow(void)
{
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);
    if (!g_icon[0]) {  /* /check runs before the tray loads them */
        LoadIconMetric(g_inst, MAKEINTRESOURCEW(IDI_ON), LIM_SMALL, &g_icon[0]);
        LoadIconMetric(g_inst, MAKEINTRESOURCEW(IDI_TIMER), LIM_SMALL, &g_icon[1]);
        LoadIconMetric(g_inst, MAKEINTRESOURCEW(IDI_OFF), LIM_SMALL, &g_icon[2]);
    }
    DialogBoxW(g_inst, MAKEINTRESOURCEW(IDD_CHECK), NULL, CheckProc);
}

/* ---- "Until a time..." dialog ---- */

static INT_PTR CALLBACK UntilProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    SYSTEMTIME st, day, utc;
    FILETIME ft;
    HWND pick = GetDlgItem(dlg, IDC_TIME), date = GetDlgItem(dlg, IDC_DATE);
    (void)lp;

    if (msg == WM_INITDIALOG) {
        ToLocal(NowUtc() + 60 * MIN, &st);
        st.wSecond = st.wMilliseconds = 0;
        SendMessageW(pick, DTM_SETFORMATW, 0, (LPARAM)L"HH:mm");
        SendMessageW(pick, DTM_SETSYSTEMTIME, GDT_VALID, (LPARAM)&st);
        SendMessageW(date, DTM_SETSYSTEMTIME, GDT_VALID, (LPARAM)&st);
        return TRUE;
    }
    if (msg != WM_COMMAND) return FALSE;
    if (LOWORD(wp) == IDOK) {
        SendMessageW(pick, DTM_GETSYSTEMTIME, 0, (LPARAM)&st);
        SendMessageW(date, DTM_GETSYSTEMTIME, 0, (LPARAM)&day);
        day.wHour = st.wHour;
        day.wMinute = st.wMinute;
        day.wSecond = day.wMilliseconds = 0;
        TzSpecificLocalTimeToSystemTime(NULL, &day, &utc);
        SystemTimeToFileTime(&utc, &ft);
        g_pick = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        if (g_pick <= NowUtc()) {
            MessageBoxW(dlg, L"That date and time has already passed. Pick a later one.", APP_NAME,
                        MB_OK | MB_ICONINFORMATION);
            return TRUE;
        }
        EndDialog(dlg, IDOK);
        return TRUE;
    }
    if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
    return FALSE;
}

/* ---- Menu ---- */

static void Command(UINT id)
{
    static const DWORD mins[] = {30, 60, 120, 240, 480};
    char why[64];

    switch (id) {
    case ID_KEEP: SetOn(!g_on, 0, "menu"); break;
    case ID_INDEF: SetOn(TRUE, 0, "keep awake indefinitely"); break;
    case ID_DISPLAY:
        g_display = !g_display;
        Log(g_display ? "keep display on: on" : "keep display on: off (system awake, screen may sleep)");
        HoldsUpdate("display setting changed");
        Tray(NIM_MODIFY);
        break;
    case ID_FOR30: case ID_FOR60: case ID_FOR120: case ID_FOR240: case ID_FOR480:
        wsprintfA(why, "timer %u min", mins[id - ID_FOR30]);
        SetOn(TRUE, NowUtc() + mins[id - ID_FOR30] * MIN, why);
        g_timerId = id;
        break;
    case ID_UNTIL:
        if (DialogBoxW(g_inst, MAKEINTRESOURCEW(IDD_UNTIL), g_hwnd, UntilProc) == IDOK)
        {
            SetOn(TRUE, g_pick, "timer until a time");
            g_timerId = ID_UNTIL;
        }
        break;
    case ID_LID0: case ID_LID1: case ID_LID2:
        g_lidMode = id - ID_LID0;
        if (!g_lidTest)
            RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Stripewake", L"LidMode", REG_DWORD, &g_lidMode,
                            sizeof g_lidMode);
        wsprintfA(why, "lid closed: %s", MODE_A[g_lidMode]);
        Log(why);
        if (g_lidMode && !RegDw(HKEY_CURRENT_USER, L"Software\\Stripewake", L"LidNoticeShown", 0)) {
            DWORD one = 1;  /* shown once, the first time a keep-running mode is picked */
            MessageBoxW(NULL, BATTERY_NOTICE, APP_NAME L" - lid", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
            if (!g_lidTest)
                RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Stripewake", L"LidNoticeShown", REG_DWORD, &one, sizeof one);
        }
        g_lidTripped = FALSE;
        HoldsUpdate("lid setting changed");
        Tray(NIM_MODIFY);
        break;
    case ID_DYN:
        g_dynOpt = !g_dynOpt;
        if (!g_dynTest)
            RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Stripewake", L"PauseDynamicLock", REG_DWORD, &g_dynOpt,
                            sizeof g_dynOpt);
        Log(g_dynOpt ? "pause Dynamic Lock while ON: on" : "pause Dynamic Lock while ON: off");
        HoldsUpdate("Dynamic Lock setting changed");
        Tray(NIM_MODIFY);
        break;
    case ID_NODIM:
        g_noDim = !g_noDim;
        RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Stripewake", L"StopDimming", REG_DWORD, &g_noDim, sizeof g_noDim);
        Log(g_noDim ? "stop dimming too: on" : "stop dimming too: off (dimming allowed)");
        HoldsUpdate("dimming setting changed");
        Tray(NIM_MODIFY);
        break;
    case ID_AWAYDIM:
        g_awayOpt = !g_awayOpt;
        RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Stripewake", L"DimWhenAway", REG_DWORD, &g_awayOpt, sizeof g_awayOpt);
        Log(g_awayOpt ? "dim when away: on" : "dim when away: off");
        AwayArm();
        HoldsUpdate("dim when away setting changed");
        Tray(NIM_MODIFY);
        break;
    case ID_DL30: case ID_DL50: case ID_DL70:  /* takes effect at the next dim */
        g_dimPct = DIM_PCT[id - ID_DL30];
        RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Stripewake", L"DimLevel", REG_DWORD, &g_dimPct, sizeof g_dimPct);
        wsprintfA(why, "dim screen when away: to %u%%", g_dimPct);
        Log(why);
        break;
    case ID_DM2: case ID_DM5: case ID_DM10: case ID_DM15:
        g_dimMins = DIM_MINS[id - ID_DM2];
        RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Stripewake", L"DimAwayMins", REG_DWORD, &g_dimMins, sizeof g_dimMins);
        wsprintfA(why, "dim screen when away: after %u min", g_dimMins);
        Log(why);
        AwayArm();
        break;
    case ID_SS:
        g_ssOpt = !g_ssOpt;
        RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Stripewake", L"PauseScreenSaver", REG_DWORD, &g_ssOpt, sizeof g_ssOpt);
        Log(g_ssOpt ? "pause screen saver while ON: on" : "pause screen saver while ON: off");
        HoldsUpdate("screen saver setting changed");
        Tray(NIM_MODIFY);
        break;
    case ID_LOGON: ToggleLogon(); break;
    case ID_CHECK: CheckWindow(); break;
    case ID_P1: case ID_P3: case ID_P7: case ID_P14: case ID_P21: case ID_P28: case ID_P35:
        WuChoose(0, PAUSE_DAYS[id - ID_P1]);
        Tray(NIM_MODIFY);
        break;
    case ID_M0: case ID_M1: case ID_M2: case ID_M3: WuChoose((int)(id - ID_M0), 0); Tray(NIM_MODIFY); break;
    case ID_NOREBOOT: WuToggleNoReboot(); Tray(NIM_MODIFY); break;
    case ID_EXIT: DestroyWindow(g_hwnd); break;
    }
}

static void ShowMenu(int x, int y)
{
    static const WCHAR *LABELS[] = {L"30 minutes", L"1 hour", L"2 hours", L"4 hours", L"8 hours"};
    HMENU m = CreatePopupMenu(), sub = CreatePopupMenu(), lid;
    int logon = LogonState(), i;
    UINT cmd;
    WCHAR label[80], until[40] = L"", t[24], wu[40];

    StatusTextEx(g_buf, TRUE);
    AppendMenuW(m, MF_STRING, ID_STATUS, g_buf);
    SetMenuDefaultItem(m, ID_STATUS, FALSE);  /* bold and readable; clicking it does nothing */
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    /* Keep Awake > Indefinite, the intervals, Pick until...; the running choice is checked, a timer shows its end. */
    if (g_on && g_end) {
        UntilText(g_end, t);
        wsprintfW(until, L" (until %s)", t);
    }
    AppendMenuW(sub, MF_STRING | (g_on && !g_end ? MF_CHECKED : 0), ID_INDEF, L"Indefinite");
    AppendMenuW(sub, MF_SEPARATOR, 0, NULL);
    for (i = 0; i < 5; i++) {
        lstrcpyW(label, LABELS[i]);
        if (g_on && g_timerId == (UINT)(ID_FOR30 + i)) lstrcatW(label, until);
        AppendMenuW(sub, MF_STRING | (g_on && g_timerId == (UINT)(ID_FOR30 + i) ? MF_CHECKED : 0), ID_FOR30 + i, label);
    }
    AppendMenuW(sub, MF_SEPARATOR, 0, NULL);
    if (g_on && g_timerId == ID_UNTIL) wsprintfW(label, L"Until %s (pick another...)", t);
    else lstrcpyW(label, L"Pick until...");
    AppendMenuW(sub, MF_STRING | (g_on && g_timerId == ID_UNTIL ? MF_CHECKED : 0), ID_UNTIL, label);
    AppendMenuW(sub, MF_SEPARATOR, 0, NULL);
    AppendMenuW(sub, MF_STRING | (!g_on ? MF_CHECKED : 0), ID_KEEP, g_on ? L"Turn off" : L"Off");
    AppendMenuW(m, MF_POPUP | (g_on ? MF_CHECKED : 0), (UINT_PTR)sub, L"Keep Awake");
    {  /* Display > Never turn the screen off (+ Windows' own dim), then Dim when away with its level and idle time */
        HMENU d = CreatePopupMenu();
        AppendMenuW(d, MF_STRING | (g_display ? MF_CHECKED : 0), ID_DISPLAY, L"Never turn the screen off");
        AppendMenuW(d, MF_STRING | (g_noDim || g_awayOpt ? MF_CHECKED : 0) | (g_display && !g_awayOpt ? 0 : MF_GRAYED), ID_NODIM,
                    L"    Also stop Windows dimming (laptop screens)");
        AppendMenuW(d, MF_SEPARATOR, 0, NULL);
        AppendMenuW(d, MF_STRING | (g_awayOpt ? MF_CHECKED : 0), ID_AWAYDIM, L"Dim when away (screen stays on)");
        AppendMenuW(d, MF_SEPARATOR, 0, NULL);
        for (i = 0; i < 3; i++) {
            wsprintfW(label, L"    Dim to %u%% of current brightness", DIM_PCT[i]);
            AppendMenuW(d, MF_STRING, ID_DL30 + i, label);
            if (DIM_PCT[i] == g_dimPct) CheckMenuRadioItem(d, ID_DL30, ID_DL70, ID_DL30 + i, MF_BYCOMMAND);
        }
        AppendMenuW(d, MF_SEPARATOR, 0, NULL);
        for (i = 0; i < 4; i++) {
            wsprintfW(label, L"    After %u minutes away", DIM_MINS[i]);
            AppendMenuW(d, MF_STRING, ID_DM2 + i, label);
            if (DIM_MINS[i] == g_dimMins) CheckMenuRadioItem(d, ID_DM2, ID_DM15, ID_DM2 + i, MF_BYCOMMAND);
        }
        AppendMenuW(m, MF_POPUP, (UINT_PTR)d, L"Display");
    }
    if (g_lidPresent || g_lidTest) {  /* laptops only */
        lid = CreatePopupMenu();
        AppendMenuW(lid, MF_STRING, ID_LID0, L"Sleep as usual");
        AppendMenuW(lid, MF_STRING, ID_LID1, L"Keep running when plugged in");
        AppendMenuW(lid, MF_STRING, ID_LID2, L"Keep running always (battery: heat, drain)");
        CheckMenuRadioItem(lid, ID_LID0, ID_LID2, ID_LID0 + g_lidMode, MF_BYCOMMAND);
        AppendMenuW(lid, MF_SEPARATOR, 0, NULL);
        AppendMenuW(lid, MF_STRING | MF_GRAYED, ID_STATUS, BATTERY_NOTICE);
        AppendMenuW(m, MF_POPUP, (UINT_PTR)lid, L"Lid closed");
    }
    AppendMenuW(m, MF_STRING | (g_dynOpt ? MF_CHECKED : 0), ID_DYN, L"Pause Dynamic Lock while ON");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (logon ? MF_CHECKED : 0) | (logon == 2 ? MF_GRAYED : 0), ID_LOGON,
                logon == 2 ? L"Start at logon (all users)" : L"Start at logon");
    {  /* Windows Updates > one state (Automatic / Paused / Ask before download / Ask before install / Off),
          then the restart choice, then what Windows reports. Every change is one Windows prompt. */
        static const WCHAR *PL[] = {L"1 day", L"3 days", L"1 week", L"2 weeks", L"3 weeks", L"4 weeks", L"5 weeks"};
        HMENU w = CreatePopupMenu(), p = CreatePopupMenu();
        DWORD max = WuMaxDays();
        int mode = WuMode();
        BOOL paused = !mode && WuPausedUntil(t);
        WuStateText(wu);  /* "Now: paused - 6 d 23 h left (until ...)", read from Windows, live while open */
        /* Bold status line, drawn by Stripewake: its size is fixed once (WM_MEASUREITEM, widest possible text) and
           each second only repaints it, so Windows never re-places the open submenu (the 19:0x jump). */
        AppendMenuW(w, MF_OWNERDRAW, ID_WU, NULL);
        AppendMenuW(w, MF_SEPARATOR, 0, NULL);
        AppendMenuW(w, MF_STRING, ID_M0, L"Automatic (Windows default)");
        for (i = 0; i < 7 && PAUSE_DAYS[i] <= max; i++) {  /* actions: each click restarts the pause from now */
            wsprintfW(label, L"%s from now", PL[i]);
            AppendMenuW(p, MF_STRING, ID_P1 + i, label);
        }
        if (max < 35) {
            wsprintfW(wu, L"Windows allows up to %u days on this PC", max);
            AppendMenuW(p, MF_STRING | MF_GRAYED, ID_STATUS, wu);
        }
        AppendMenuW(w, MF_POPUP, (UINT_PTR)p, paused ? L"Pause again for" : L"Pause for");
        AppendMenuW(w, MF_STRING, ID_M1, L"Ask before download");
        AppendMenuW(w, MF_STRING, ID_M2, L"Download, ask before install");
        AppendMenuW(w, MF_STRING, ID_M3, L"Off (no automatic updates)");
        if (!paused) CheckMenuRadioItem(w, ID_M0, ID_M3, ID_M0 + mode, MF_BYCOMMAND);
        AppendMenuW(w, MF_SEPARATOR, 0, NULL);
        AppendMenuW(w, MF_STRING | (WuNoReboot() ? MF_CHECKED : 0), ID_NOREBOOT, L"No restarts while I'm signed in");
        if (RestartPending(wu)) {
            AppendMenuW(w, MF_SEPARATOR, 0, NULL);
            AppendMenuW(w, MF_STRING | MF_GRAYED, ID_STATUS, wu);
        }
        AppendMenuW(m, MF_POPUP | (mode || paused || WuNoReboot() ? MF_CHECKED : 0), (UINT_PTR)w, L"Windows Updates");
    }
    AppendMenuW(m, MF_STRING, ID_CHECK, L"What can still interrupt Stripewake?");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");

    SetForegroundWindow(g_hwnd);
    g_menu = m;
    if ((g_on && g_end) || WuPausedUntil(NULL)) SetTimer(g_hwnd, TIMER_MENU, 1000, NULL);
    cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN |
                           (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN),
                           x, y, g_hwnd, NULL);
    KillTimer(g_hwnd, TIMER_MENU);
    g_menu = NULL;
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (cmd) Command(cmd);
}

/* The menu font, bold, made once. */
static HFONT BoldMenuFont(void)
{
    static HFONT f;
    NONCLIENTMETRICSW ncm;
    if (f) return f;
    memset(&ncm, 0, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0)) return (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    ncm.lfMenuFont.lfWeight = FW_BOLD;
    f = CreateFontIndirectW(&ncm.lfMenuFont);
    return f ? f : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

/* Repaint this thread's open menu windows (class #32768) so a changed item text shows. */
static BOOL CALLBACK RedrawMenus(HWND h, LPARAM lp)
{
    WCHAR cls[16];
    (void)lp;
    if (GetClassNameW(h, cls, 16) && !lstrcmpW(cls, L"#32768")) InvalidateRect(h, NULL, TRUE);
    return TRUE;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    WCHAR tip[256];

    if (msg == g_taskbarCreated && msg) {  /* Explorer restarted: put the icon back. */
        g_added = FALSE;
        g_addTries = 0;
        Tray(NIM_ADD);
        return 0;
    }
    switch (msg) {
    case WM_TRAY:
        switch (LOWORD(lp)) {
        case WM_MOUSEMOVE:
        case NIN_POPUPOPEN:  /* hover: refresh the countdown, only when the text changes */
            TipText(tip);
            if (lstrcmpW(tip, g_tip)) Tray(NIM_MODIFY);
            break;
        case NIN_SELECT:
        case NIN_KEYSELECT:
            if (!g_busy) SetOn(!g_on, 0, "click");
            break;
        case WM_CONTEXTMENU:
            if (g_busy) break;
            g_busy = TRUE;
            ShowMenu((short)LOWORD(wp), (short)HIWORD(wp));
            g_busy = FALSE;
            break;
        }
        return 0;
    case WM_MEASUREITEM: {  /* the owner-drawn Windows Updates status line: widest text it can ever show, in bold */
        MEASUREITEMSTRUCT *mis = (MEASUREITEMSTRUCT *)lp;
        HDC dc;
        HGDIOBJ old;
        SIZE sz;
        static const WCHAR WIDEST[] = L"Now: paused - 35 d 23 h 59 m 59 s left (until 2026-12-31 23:59)";
        if (mis->CtlType != ODT_MENU || mis->itemID != ID_WU) break;
        dc = GetDC(hwnd);
        old = SelectObject(dc, BoldMenuFont());
        GetTextExtentPoint32W(dc, WIDEST, lstrlenW(WIDEST), &sz);
        SelectObject(dc, old);
        ReleaseDC(hwnd, dc);
        mis->itemWidth = (UINT)sz.cx + 8;
        mis->itemHeight = (UINT)sz.cy + sz.cy / 2;
        return TRUE;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lp;
        WCHAR text[160];
        RECT r;
        HGDIOBJ old;
        if (dis->CtlType != ODT_MENU || dis->itemID != ID_WU) break;
        FillRect(dis->hDC, &dis->rcItem, GetSysColorBrush(COLOR_MENU));
        WuStateText(text);
        r = dis->rcItem;
        r.left += GetSystemMetrics(SM_CXMENUCHECK) + 6;
        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, GetSysColor(COLOR_MENUTEXT));
        old = SelectObject(dis->hDC, BoldMenuFont());
        DrawTextW(dis->hDC, text, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        SelectObject(dis->hDC, old);
        return TRUE;
    }
    case WM_TIMER:
        if (wp == TIMER_ADD) { KillTimer(hwnd, TIMER_ADD); Tray(NIM_ADD); return 0; }
        if (wp == TIMER_IDLE) { AwayTick(); return 0; }
        if (wp == TIMER_MENU) {
            if (g_menu) {
                WCHAR t[160];
                /* text only (MIIM_STRING keeps the bold default state, so the item width never changes and the
                   open menus never re-lay out under the mouse); found in the Windows Updates submenu by id */
                MENUITEMINFOW mi;
                memset(&mi, 0, sizeof mi);
                mi.cbSize = sizeof mi;
                mi.fMask = MIIM_STRING;
                mi.dwTypeData = t;
                StatusTextEx(t, TRUE);
                SetMenuItemInfoW(g_menu, ID_STATUS, FALSE, &mi);
                /* the Windows Updates line is owner-drawn: the repaint below redraws it with the fresh time,
                   no item change, so its open submenu never moves */
                EnumThreadWindows(GetCurrentThreadId(), RedrawMenus, 0);
            }
            return 0;
        }
        if (wp == TIMER_END) {
            if (g_on && g_end && NowUtc() >= g_end) SetOn(FALSE, 0, "timer ended");
            else if (g_on && g_end) ArmTimer();
            else KillTimer(hwnd, TIMER_END);
        }
        return 0;
    case WM_TIMECHANGE:
        if (g_on && g_end) ArmTimer();
        return 0;
    case WM_DISPLAYCHANGE:  /* monitors added, removed or rearranged: brightness back first (dims again if still away) */
        AwayUndim();
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_APMSUSPEND) { AwayUndim(); DdcWait(2500); }
        if (wp == PBT_APMRESUMEAUTOMATIC) AwayUndim();  /* anything the sleep cut short */
        if (wp == PBT_APMRESUMEAUTOMATIC && g_on && g_end) {
            if (NowUtc() >= g_end) SetOn(FALSE, 0, "timer ended (after resume)");
            else ArmTimer();
        }
        if (wp == PBT_POWERSETTINGCHANGE && lp) {
            POWERBROADCAST_SETTING *ps = (POWERBROADCAST_SETTING *)lp;
            if (GuidEq(&ps->PowerSetting, &ACTIVE_PLAN_G) && ps->DataLength >= sizeof(GUID) &&
                ((H_LID.held && !GuidEq((const GUID *)ps->Data, &H_LID.plan)) ||
                 (H_LOCK.held && !GuidEq((const GUID *)ps->Data, &H_LOCK.plan)))) {  /* old plan back, hold the new */
                RestoreAll("power plan changed");
                HoldsUpdate("power plan changed");
                Tray(NIM_MODIFY);
            }
            if (GuidEq(&ps->PowerSetting, &DISPLAY_G) && ps->DataLength >= sizeof(DWORD)) {  /* record only */
                DWORD v = *(const DWORD *)ps->Data;
                Log(v == 0 ? "display off" : v == 2 ? "display dimmed" : "display on");
            }
            if (GuidEq(&ps->PowerSetting, &BATTERY_PCT_G) && ps->DataLength >= sizeof(DWORD) && H_LID.held &&
                H_LID.mode == 2 && *(const DWORD *)ps->Data < 10) {
                g_lidTripped = TRUE;  /* stays off until Stripewake is turned ON again or the lid setting changes */
                HoldRestore(&H_LID, "battery below 10%");
                Tray(NIM_MODIFY);
            }
        }
        return TRUE;
    case WM_ENDSESSION:
        if (wp) {
            BOOL closeApp = (lp & ENDSESSION_CLOSEAPP) != 0;  /* an installer (Restart Manager) asks us to exit */
            InterlockedExchange(&g_ddcQuit, 1);
            AwayUndim();
            DdcWait(5000);
            RestoreAll(closeApp ? "closed for an update" : "sign-out or shutdown");
            Log(closeApp ? "OFF - closed for an update" : "OFF - sign-out or shutdown");
            Tray(NIM_DELETE);
            if (closeApp) DestroyWindow(hwnd);
        }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_IDLE);
        InterlockedExchange(&g_ddcQuit, 1);
        AwayUndim();
        DdcWait(5000);
        RestoreAll("exit");
        SetThreadExecutionState(ES_CONTINUOUS);
        Log("OFF - exit");
        Tray(NIM_DELETE);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* Switches: "/check" shows the status check and exits; "/restore" puts back saved settings and exits
   (the uninstaller runs it). Test hooks: "/for <seconds>" starts with a short timer; "/lidtest <0|1|2>" forces
   the lid code path with that lid mode on a PC without a lid (the mode is not saved). */
static const WCHAR *Switch(const WCHAR *name)
{
    const WCHAR *c = GetCommandLineW();
    int i;
    for (; *c; c++) {
        if (*c != L'/') continue;
        for (i = 0; name[i] && c[i + 1] == name[i]; i++) {}
        if (!name[i] && (c[i + 1] == L' ' || !c[i + 1])) return c + i + 1;
    }
    return NULL;
}

void WinMainCRTStartup(void)
{
    INITCOMMONCONTROLSEX icc;
    WNDCLASSW wc;
    MSG msg;
    DWORD secs = 0;
    const WCHAR *arg;
    char why[40];
    SYSTEM_POWER_CAPABILITIES caps;

    g_inst = GetModuleHandleW(NULL);
    memset(&caps, 0, sizeof caps);
    g_lidPresent = GetPwrCapabilities(&caps) && caps.LidPresent;
    arg = Switch(L"lidtest");
    if (arg) {
        g_lidTest = TRUE;
        g_lidMode = *arg ? Atou(arg + 1) : 1;
        if (g_lidMode > 2) g_lidMode = 1;
    } else {
        g_lidMode = RegDw(HKEY_CURRENT_USER, L"Software\\Stripewake", L"LidMode", 0);
        if (g_lidMode > 2) g_lidMode = 0;
    }
    /* the screen saver is always paused while ON (President 19:5x: part of being awake, not an option) */
    g_noDim = RegDw(HKEY_CURRENT_USER, L"Software\\Stripewake", L"StopDimming", 0) != 0;      /* off by default: dimming allowed */
    g_dynOpt = RegDw(HKEY_CURRENT_USER, L"Software\\Stripewake", L"PauseDynamicLock", 1) != 0;  /* on by default */
    g_awayOpt = RegDw(HKEY_CURRENT_USER, L"Software\\Stripewake", L"DimWhenAway", 0) != 0;  /* off by default */
    g_dimPct = RegDw(HKEY_CURRENT_USER, L"Software\\Stripewake", L"DimLevel", 50);
    if (g_dimPct != 30 && g_dimPct != 70) g_dimPct = 50;
    g_dimMins = RegDw(HKEY_CURRENT_USER, L"Software\\Stripewake", L"DimAwayMins", 5);
    if (g_dimMins != 2 && g_dimMins != 10 && g_dimMins != 15) g_dimMins = 5;
    if (Switch(L"dyntest")) {  /* test hook: opt in and use a harmless test value instead of EnableGoodbye */
        g_dynTest = g_dynOpt = TRUE;
        g_dynName = L"StripewakeDynLockTest";
    }
    if (Switch(L"check")) { CheckWindow(); ExitProcess(0); }
    arg = Switch(L"wu");  /* the elevated side of the Windows Update toggles: "/wu pause|resume|noreboot <0|1|absent>" */
    if (arg) {
        WCHAR tok[4][16];
        int n, j;
        for (j = 0; j < 4; j++) {
            while (*arg == L' ') arg++;
            for (n = 0; n < 15 && arg[n] && arg[n] != L' '; n++) tok[j][n] = arg[n];
            tok[j][n] = 0;
            arg += n;
        }
        ExitProcess(WuHelper(tok[0], tok[1], tok[2], tok[3]));
    }
    if (Switch(L"restore")) { RestoreAll("restore requested (uninstall)"); DdcPass(FALSE); WuUndo(); ExitProcess(0); }
    CreateMutexW(NULL, TRUE, L"Local\\Stripewake.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) ExitProcess(0);
    RestoreAll("recovered at start: the last run did not put the setting back");  /* crash safety */

    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_DATE_CLASSES;
    InitCommonControlsEx(&icc);
    LoadIconMetric(g_inst, MAKEINTRESOURCEW(IDI_ON), LIM_SMALL, &g_icon[0]);
    LoadIconMetric(g_inst, MAKEINTRESOURCEW(IDI_TIMER), LIM_SMALL, &g_icon[1]);
    LoadIconMetric(g_inst, MAKEINTRESOURCEW(IDI_OFF), LIM_SMALL, &g_icon[2]);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_inst;
    wc.lpszClassName = L"StripewakeTray";
    RegisterClassW(&wc);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    /* A hidden top-level window, not HWND_MESSAGE: message-only windows miss the TaskbarCreated broadcast. */
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, APP_NAME, WS_POPUP, 0, 0, 0, 0, NULL, NULL, g_inst, NULL);
    if (!g_hwnd) ExitProcess(1);
    ChangeWindowMessageFilterEx(g_hwnd, g_taskbarCreated, MSGFLT_ALLOW, NULL);
    RegisterPowerSettingNotification(g_hwnd, &DISPLAY_G, DEVICE_NOTIFY_WINDOW_HANDLE);  /* display on/off/dim log */
    RegisterPowerSettingNotification(g_hwnd, &ACTIVE_PLAN_G, DEVICE_NOTIFY_WINDOW_HANDLE);  /* plan switches */
    if (g_lidPresent || g_lidTest)  /* event-driven battery level, no polling */
        RegisterPowerSettingNotification(g_hwnd, &BATTERY_PCT_G, DEVICE_NOTIFY_WINDOW_HANDLE);

    arg = Switch(L"for");
    if (arg && *arg) secs = Atou(arg + 1);
    if (secs) wsprintfA(why, "start (test timer %u s)", secs);
    SetOn(TRUE, secs ? NowUtc() + secs * SEC : 0, secs ? why : "start");
    Tray(NIM_ADD);
    g_ddcEvt = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (g_ddcEvt) g_ddcThread = CreateThread(NULL, 0, DdcThread, NULL, 0, NULL);
    DdcRequest(FALSE);  /* crash safety: brightness a previous run lowered goes back (nothing to do without dim.ini) */
    AwayArm();

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    ExitProcess(0);
}
