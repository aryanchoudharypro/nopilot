#include <windows.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <sddl.h>
#include <intrin.h>

// The Copilot key arrives as LWin down, LShift down, F23 down, and releases as F23 up, LShift up, LWin up.
// A Windows or Shift press belongs to the Copilot key when it is one of the two key events directly
// before F23 and no older than this; anything else is a key the user is holding.
const DWORD COPILOT_WINDOW_MS = 50;

struct Modifier {
	WORD vk;
	WORD scan;
	DWORD flags;
	bool down;
	DWORD downTime;
	DWORD downSeq;
	bool userHeld;
	bool swallowUp;
	DWORD swallowUntil;
};

Modifier mods[2] = {
	{ VK_LSHIFT, 0x2A, 0, false, 0, 0, false, false, 0 },
	{ VK_LWIN, 0x5B, KEYEVENTF_EXTENDEDKEY, false, 0, 0, false, false, 0 },
};

HHOOK hHook = NULL;
HANDLE hStart = NULL;
HANDLE hStop = NULL;
DWORD mainThreadId = 0;
bool held = false;
// Counts physical key events, so the hook can tell which presses came directly before F23.
DWORD eventSeq = 0;
// Bumped whenever a hold starts or its repeat ends, so repeat ticks already queued are ignored.
volatile LONG generation = 0;

const UINT WM_REPEAT_TICK = WM_APP + 1;

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
		LONG gen = generation;
		double next = NowMs() + RepeatDelayMs();
		for (;;) {
			double wait = next - NowMs();
			if (WaitForSingleObject(hStop, wait > 0 ? (DWORD)(wait + 0.5) : 0) != WAIT_TIMEOUT) break;
			if (generation != gen) {
				// A new hold began before this loop saw the previous one end.
				gen = generation;
				next = NowMs() + RepeatDelayMs();
				continue;
			}
			// Keys are only ever injected from the hook thread; injecting here could block against the hook.
			PostThreadMessage(mainThreadId, WM_REPEAT_TICK, (WPARAM)gen, 0);
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
			eventSeq++;

			// Like hardware typematic, pressing any other key ends the repeat for the rest of this hold.
			if (held && !up && pKbd->vkCode != VK_F23) {
				InterlockedIncrement(&generation);
				SetEvent(hStop);
			}

			for (Modifier& m : mods) {
				if (pKbd->vkCode != m.vk) continue;
				if (!up) {
					if (!m.down) {
						m.down = true;
						m.downTime = pKbd->time;
						m.downSeq = eventSeq;
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
						bool fromCopilot = m.down && (eventSeq - m.downSeq) <= 2
							&& (pKbd->time - m.downTime) <= COPILOT_WINDOW_MS;
						m.userHeld = m.down && !fromCopilot;
						m.swallowUp = false;
						if (fromCopilot) SendKey(m.vk, m.scan, m.flags | KEYEVENTF_KEYUP);
					}
					held = true;
					InterlockedIncrement(&generation);
					SendRCtrl(false);
					ResetEvent(hStop);
					SetEvent(hStart);
				}
			} else {
				SetEvent(hStop);
				InterlockedIncrement(&generation);
				held = false;
				SendRCtrl(true);
				for (Modifier& m : mods) {
					if (m.userHeld) {
						m.swallowUp = true;
						m.swallowUntil = pKbd->time + COPILOT_WINDOW_MS;
					}
				}
			}
			return 1;
		}
	}
	return CallNextHookEx(hHook, nCode, wParam, lParam);
}

const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* RUN_VALUE = L"nopilot";
const wchar_t* TASK_NAME = L"nopilot";
const wchar_t* TASK_TREE_KEY = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Schedule\\TaskCache\\Tree\\nopilot";
const wchar_t* MUTEX_NAME = L"Local\\nopilot";
const wchar_t* QUIT_EVENT_NAME = L"Local\\nopilotQuit";

// The running instance may be elevated, and a non-elevated nopilot must still be able to find and stop it.
SECURITY_ATTRIBUTES* OpenToAll() {
	static SECURITY_DESCRIPTOR sd;
	static SECURITY_ATTRIBUTES sa;
	InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
	SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
	sa.nLength = sizeof(sa);
	sa.lpSecurityDescriptor = &sd;
	sa.bInheritHandle = FALSE;
	return &sa;
}

bool OwnPath(wchar_t* path) {
	DWORD len = GetModuleFileNameW(NULL, path, MAX_PATH);
	return len && len < MAX_PATH;
}

void InstallRunEntry() {
	wchar_t path[MAX_PATH];
	if (!OwnPath(path)) return;
	wchar_t command[MAX_PATH + 3];
	wsprintfW(command, L"\"%s\"", path);
	RegSetKeyValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE, REG_SZ, command,
		(DWORD)((lstrlenW(command) + 1) * sizeof(wchar_t)));
}

void RemoveRunEntry() {
	RegDeleteKeyValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE);
}

void QuitRunningInstance() {
	HANDLE hQuit = OpenEventW(EVENT_MODIFY_STATE, FALSE, QUIT_EVENT_NAME);
	if (hQuit) {
		SetEvent(hQuit);
		CloseHandle(hQuit);
	}
}

void WaitForInstanceExit() {
	for (int i = 0; i < 60; i++) {
		HANDLE hMutex = OpenMutexW(SYNCHRONIZE, FALSE, MUTEX_NAME);
		if (!hMutex) {
			if (GetLastError() == ERROR_FILE_NOT_FOUND) return;
		} else {
			CloseHandle(hMutex);
		}
		Sleep(50);
	}
}

bool IsElevated() {
	HANDLE hToken;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) return false;
	TOKEN_ELEVATION elevation;
	DWORD size = 0;
	bool elevated = GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &size)
		&& elevation.TokenIsElevated;
	CloseHandle(hToken);
	return elevated;
}

bool CurrentUserSid(wchar_t* out, int capacity) {
	static BYTE buffer[256];
	HANDLE hToken;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) return false;
	DWORD size = 0;
	bool ok = GetTokenInformation(hToken, TokenUser, buffer, sizeof(buffer), &size) != 0;
	CloseHandle(hToken);
	if (!ok) return false;
	wchar_t* sid = NULL;
	if (!ConvertSidToStringSidW(((TOKEN_USER*)buffer)->User.Sid, &sid)) return false;
	ok = lstrlenW(sid) < capacity;
	if (ok) lstrcpyW(out, sid);
	LocalFree(sid);
	return ok;
}

// Runs this executable again with administrator rights (one UAC prompt) and returns its exit code.
DWORD RunElevated(const wchar_t* params) {
	wchar_t path[MAX_PATH];
	if (!OwnPath(path)) return (DWORD)-1;
	SHELLEXECUTEINFOW info = {};
	info.cbSize = sizeof(info);
	info.fMask = SEE_MASK_NOCLOSEPROCESS;
	info.lpVerb = L"runas";
	info.lpFile = path;
	info.lpParameters = params;
	info.nShow = SW_HIDE;
	if (!ShellExecuteExW(&info) || !info.hProcess) return (DWORD)-1;
	WaitForSingleObject(info.hProcess, INFINITE);
	DWORD code = (DWORD)-1;
	GetExitCodeProcess(info.hProcess, &code);
	CloseHandle(info.hProcess);
	return code;
}

DWORD RunSchtasks(const wchar_t* arguments) {
	static wchar_t command[1024];
	wchar_t system[MAX_PATH];
	UINT len = GetSystemDirectoryW(system, MAX_PATH);
	if (!len || len >= MAX_PATH) return (DWORD)-1;
	if (lstrlenW(system) + lstrlenW(arguments) + 20 >= 1024) return (DWORD)-1;
	lstrcpyW(command, L"\"");
	lstrcatW(command, system);
	lstrcatW(command, L"\\schtasks.exe\" ");
	lstrcatW(command, arguments);

	STARTUPINFOW startup = {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process = {};
	if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process))
		return (DWORD)-1;
	WaitForSingleObject(process.hProcess, INFINITE);
	DWORD code = (DWORD)-1;
	GetExitCodeProcess(process.hProcess, &code);
	CloseHandle(process.hProcess);
	CloseHandle(process.hThread);
	return code;
}

static wchar_t taskXml[4096];

void AppendXml(const wchar_t* text) {
	if (lstrlenW(taskXml) + lstrlenW(text) < 4000) lstrcatW(taskXml, text);
}

void AppendXmlEscaped(const wchar_t* text) {
	for (; *text; text++) {
		wchar_t one[2] = { *text, 0 };
		if (*text == L'&') AppendXml(L"&amp;");
		else if (*text == L'<') AppendXml(L"&lt;");
		else if (*text == L'>') AppendXml(L"&gt;");
		else AppendXml(one);
	}
}

bool TaskExists() {
	HKEY hKey;
	LSTATUS status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, TASK_TREE_KEY, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &hKey);
	if (status == ERROR_SUCCESS) RegCloseKey(hKey);
	// Without administrator rights the key may exist but be unreadable.
	return status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND;
}

// Registers a scheduled task that starts nopilot with administrator rights whenever the given user signs in,
// then starts it. Must be called elevated.
DWORD InstallTask(const wchar_t* sid) {
	wchar_t path[MAX_PATH];
	if (!OwnPath(path)) return (DWORD)-1;

	taskXml[0] = 0;
	AppendXml(L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
		L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
		L"<RegistrationInfo><Description>Turns the Copilot key into a Right Ctrl key.</Description></RegistrationInfo>\r\n"
		L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>");
	AppendXmlEscaped(sid);
	AppendXml(L"</UserId></LogonTrigger></Triggers>\r\n"
		L"<Principals><Principal id=\"Author\"><UserId>");
	AppendXmlEscaped(sid);
	AppendXml(L"</UserId><LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>\r\n"
		L"<Settings>"
		L"<MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
		L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
		L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
		L"<AllowHardTerminate>true</AllowHardTerminate>"
		L"<StartWhenAvailable>false</StartWhenAvailable>"
		L"<AllowStartOnDemand>true</AllowStartOnDemand>"
		L"<Enabled>true</Enabled>"
		L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit>"
		L"<Priority>4</Priority>"
		L"</Settings>\r\n"
		L"<Actions Context=\"Author\"><Exec><Command>");
	AppendXmlEscaped(path);
	AppendXml(L"</Command></Exec></Actions>\r\n</Task>\r\n");

	wchar_t xmlPath[MAX_PATH];
	DWORD len = GetTempPathW(MAX_PATH, xmlPath);
	if (!len || len + 20 >= MAX_PATH) return (DWORD)-1;
	lstrcatW(xmlPath, L"nopilot-task.xml");

	HANDLE hFile = CreateFileW(xmlPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) return (DWORD)-1;
	const wchar_t bom = 0xFEFF;
	DWORD written;
	WriteFile(hFile, &bom, sizeof(bom), &written, NULL);
	WriteFile(hFile, taskXml, (DWORD)(lstrlenW(taskXml) * sizeof(wchar_t)), &written, NULL);
	CloseHandle(hFile);

	static wchar_t arguments[512];
	wsprintfW(arguments, L"/Create /TN %s /XML \"%s\" /F", TASK_NAME, xmlPath);
	DWORD code = RunSchtasks(arguments);
	DeleteFileW(xmlPath);
	if (code != 0) return code;

	QuitRunningInstance();
	WaitForInstanceExit();
	wsprintfW(arguments, L"/Run /TN %s", TASK_NAME);
	RunSchtasks(arguments);
	return 0;
}

DWORD DeleteTask() {
	static wchar_t arguments[128];
	wsprintfW(arguments, L"/Delete /TN %s /F", TASK_NAME);
	return RunSchtasks(arguments);
}

bool StartsWith(const wchar_t* text, const wchar_t* prefix) {
	while (*prefix) {
		if (*text++ != *prefix++) return false;
	}
	return true;
}

DWORD WINAPI QuitWatchThread(LPVOID param) {
	HANDLE hQuit = CreateEventW(OpenToAll(), TRUE, FALSE, QUIT_EVENT_NAME);
	if (hQuit) {
		WaitForSingleObject(hQuit, INFINITE);
		PostThreadMessage((DWORD)(DWORD_PTR)param, WM_QUIT, 0, 0);
	}
	return 0;
}

int Run(HINSTANCE hInstance, const wchar_t* args) {
	const wchar_t* INSTALL_TASK = L"--install-task ";

	if (lstrcmpiW(args, L"--quit") == 0) {
		QuitRunningInstance();
		return 0;
	}
	if (lstrcmpiW(args, L"--uninstall") == 0) {
		RemoveRunEntry();
		QuitRunningInstance();
		if (!TaskExists()) return 0;
		return (int)(IsElevated() ? DeleteTask() : RunElevated(L"--uninstall"));
	}
	if (StartsWith(args, INSTALL_TASK)) {
		return (int)InstallTask(args + lstrlenW(INSTALL_TASK));
	}
	if (lstrcmpiW(args, L"--install") == 0) {
		// Preferred: a scheduled task that starts nopilot elevated, so it also works in administrator windows.
		// If the user declines the UAC prompt, fall back to a plain per-user start at sign-in.
		wchar_t sid[128];
		if (CurrentUserSid(sid, 128)) {
			DWORD code;
			if (IsElevated()) {
				code = InstallTask(sid);
			} else {
				wchar_t params[160];
				lstrcpyW(params, INSTALL_TASK);
				lstrcatW(params, sid);
				code = RunElevated(params);
			}
			if (code == 0) {
				RemoveRunEntry();
				return 0;
			}
		}
		InstallRunEntry();
	}

	HANDLE hMutex = CreateMutexW(OpenToAll(), FALSE, MUTEX_NAME);
	if (!hMutex || GetLastError() == ERROR_ALREADY_EXISTS) return 0;

	MSG msg;
	PeekMessage(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);
	mainThreadId = GetCurrentThreadId();
	CreateThread(NULL, 0, QuitWatchThread, (LPVOID)(DWORD_PTR)mainThreadId, 0, NULL);

	timeBeginPeriod(1);
	hStart = CreateEvent(NULL, FALSE, FALSE, NULL);
	hStop = CreateEvent(NULL, TRUE, TRUE, NULL);
	CreateThread(NULL, 0, RepeatThread, NULL, 0, NULL);

	hHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInstance, 0);
	while (GetMessage(&msg, NULL, 0, 0)) {
		if (msg.hwnd == NULL && msg.message == WM_REPEAT_TICK) {
			if (held && (LONG)msg.wParam == generation) SendRCtrl(false);
			continue;
		}
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
	UnhookWindowsHookEx(hHook);

	SetEvent(hStop);
	if (held) {
		held = false;
		SendRCtrl(true);
	}
	timeEndPeriod(1);
	return 0;
}

// The program is built without the C runtime to keep the executable tiny, so it provides its own entry point
// and the few runtime pieces the compiler may still ask for.
extern "C" int _fltused = 0;

#pragma function(memset)
extern "C" void* memset(void* dest, int value, size_t count) {
	__stosb((unsigned char*)dest, (unsigned char)value, count);
	return dest;
}

#pragma function(memcpy)
extern "C" void* memcpy(void* dest, const void* src, size_t count) {
	__movsb((unsigned char*)dest, (const unsigned char*)src, count);
	return dest;
}

void WINAPI Entry() {
	const wchar_t* args = GetCommandLineW();
	if (*args == L'"') {
		args++;
		while (*args && *args != L'"') args++;
		if (*args) args++;
	} else {
		while (*args && *args != L' ' && *args != L'\t') args++;
	}
	while (*args == L' ' || *args == L'\t') args++;
	ExitProcess(Run(GetModuleHandleW(NULL), args));
}
