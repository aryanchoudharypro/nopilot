#include <windows.h>
#include <mmsystem.h>

// The Copilot key arrives as LWin down, LShift down, F23 down with the same timestamp,
// and F23 up, LShift up, LWin up within a few milliseconds of each other on release.
const DWORD COPILOT_DOWN_WINDOW_MS = 20;
const DWORD COPILOT_UP_WINDOW_MS = 40;

struct Modifier {
    WORD vk;
    WORD scan;
    DWORD flags;
    bool down;
    DWORD downTime;
    bool userHeld;
    bool swallowUp;
    DWORD swallowUntil;
};

Modifier mods[2] = {
    { VK_LSHIFT, 0x2A, 0, false, 0, false, false, 0 },
    { VK_LWIN, 0x5B, KEYEVENTF_EXTENDEDKEY, false, 0, false, false, 0 },
};

HHOOK hHook = NULL;
CRITICAL_SECTION lock;
HANDLE hStart = NULL;
HANDLE hStop = NULL;
bool held = false;

void SendKey(WORD vk, WORD scan, DWORD flags) {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.wScan = scan;
    input.ki.dwFlags = flags;
    SendInput(1, &input, sizeof(INPUT));
}

void SendRCtrl(bool up) {
    SendKey(VK_RCONTROL, 0x1D, KEYEVENTF_EXTENDEDKEY | (up ? KEYEVENTF_KEYUP : 0));
}

double RepeatDelayMs() {
    int delay = 1;
    SystemParametersInfo(SPI_GETKEYBOARDDELAY, 0, &delay, 0);
    return (delay + 1) * 250.0;
}

double RepeatIntervalMs() {
    DWORD speed = 31;
    SystemParametersInfo(SPI_GETKEYBOARDSPEED, 0, &speed, 0);
    return 1000.0 / (2.5 + speed * 27.5 / 31.0);
}

double NowMs() {
    static LARGE_INTEGER freq = {};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart * 1000.0 / freq.QuadPart;
}

DWORD WINAPI RepeatThread(LPVOID) {
    for (;;) {
        WaitForSingleObject(hStart, INFINITE);
        double interval = RepeatIntervalMs();
        double next = NowMs() + RepeatDelayMs();
        for (;;) {
            double wait = next - NowMs();
            if (WaitForSingleObject(hStop, wait > 0 ? (DWORD)(wait + 0.5) : 0) != WAIT_TIMEOUT) break;
            EnterCriticalSection(&lock);
            if (held) SendRCtrl(false);
            LeaveCriticalSection(&lock);
            next += interval;
        }
    }
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* pKbd = (KBDLLHOOKSTRUCT*)lParam;
        bool up = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
        bool injected = (pKbd->flags & LLKHF_INJECTED) != 0;

        if (!injected) {
            // Like hardware typematic, pressing any other key ends the repeat for the rest of this hold.
            if (held && !up && pKbd->vkCode != VK_F23) SetEvent(hStop);

            for (Modifier& m : mods) {
                if (pKbd->vkCode != m.vk) continue;
                if (!up) {
                    if (!m.down) {
                        m.down = true;
                        m.downTime = pKbd->time;
                    }
                } else if (m.swallowUp && (LONG)(m.swallowUntil - pKbd->time) >= 0) {
                    // The Copilot key's own release of a modifier the user is still holding.
                    m.swallowUp = false;
                    return 1;
                } else {
                    m.down = false;
                    m.userHeld = false;
                    m.swallowUp = false;
                }
            }
        }

        if (pKbd->vkCode == VK_F23) {
            if (!up) {
                if (!held) {
                    for (Modifier& m : mods) {
                        bool fromCopilot = m.down && (pKbd->time - m.downTime) <= COPILOT_DOWN_WINDOW_MS;
                        m.userHeld = m.down && !fromCopilot;
                        m.swallowUp = false;
                        if (fromCopilot) SendKey(m.vk, m.scan, m.flags | KEYEVENTF_KEYUP);
                    }
                    EnterCriticalSection(&lock);
                    held = true;
                    SendRCtrl(false);
                    LeaveCriticalSection(&lock);
                    ResetEvent(hStop);
                    SetEvent(hStart);
                }
            } else {
                SetEvent(hStop);
                EnterCriticalSection(&lock);
                held = false;
                SendRCtrl(true);
                LeaveCriticalSection(&lock);
                for (Modifier& m : mods) {
                    if (m.userHeld) {
                        m.swallowUp = true;
                        m.swallowUntil = pKbd->time + COPILOT_UP_WINDOW_MS;
                    }
                }
            }
            return 1;
        }
    }
    return CallNextHookEx(hHook, nCode, wParam, lParam);
}

const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* RUN_VALUE = L"CopilotToRightCtrl";
const wchar_t* MUTEX_NAME = L"Local\\CopilotToRightCtrl";
const wchar_t* QUIT_EVENT_NAME = L"Local\\CopilotToRightCtrlQuit";

void InstallAutostart() {
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (!len || len >= MAX_PATH) return;
    wchar_t command[MAX_PATH + 3];
    wsprintfW(command, L"\"%s\"", path);
    RegSetKeyValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE, REG_SZ, command,
        (DWORD)((lstrlenW(command) + 1) * sizeof(wchar_t)));
}

void UninstallAutostart() {
    RegDeleteKeyValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE);
}

void QuitRunningInstance() {
    HANDLE hQuit = OpenEventW(EVENT_MODIFY_STATE, FALSE, QUIT_EVENT_NAME);
    if (hQuit) {
        SetEvent(hQuit);
        CloseHandle(hQuit);
    }
}

DWORD WINAPI QuitWatchThread(LPVOID param) {
    HANDLE hQuit = CreateEventW(NULL, TRUE, FALSE, QUIT_EVENT_NAME);
    if (hQuit) {
        WaitForSingleObject(hQuit, INFINITE);
        PostThreadMessage((DWORD)(DWORD_PTR)param, WM_QUIT, 0, 0);
    }
    return 0;
}

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    if (lstrcmpiA(lpCmdLine, "--quit") == 0) {
        QuitRunningInstance();
        return 0;
    }
    if (lstrcmpiA(lpCmdLine, "--uninstall") == 0) {
        UninstallAutostart();
        QuitRunningInstance();
        return 0;
    }
    if (lstrcmpiA(lpCmdLine, "--install") == 0) InstallAutostart();

    CreateMutexW(NULL, FALSE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    MSG msg;
    PeekMessage(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);
    CreateThread(NULL, 0, QuitWatchThread, (LPVOID)(DWORD_PTR)GetCurrentThreadId(), 0, NULL);

    timeBeginPeriod(1);
    InitializeCriticalSection(&lock);
    hStart = CreateEvent(NULL, FALSE, FALSE, NULL);
    hStop = CreateEvent(NULL, TRUE, TRUE, NULL);
    CreateThread(NULL, 0, RepeatThread, NULL, 0, NULL);

    hHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInstance, 0);
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    UnhookWindowsHookEx(hHook);

    SetEvent(hStop);
    EnterCriticalSection(&lock);
    if (held) {
        held = false;
        SendRCtrl(true);
    }
    LeaveCriticalSection(&lock);
    timeEndPeriod(1);
    return 0;
}
