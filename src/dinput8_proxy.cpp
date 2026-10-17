#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <string>

static HMODULE g_self = nullptr;
static HMODULE g_real = nullptr;

static std::wstring ModuleDirectory(HMODULE module)
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(module, path, MAX_PATH);
    std::wstring s(path);
    const size_t pos = s.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : s.substr(0, pos);
}

static std::wstring DataDirectory()
{
    std::wstring dir = ModuleDirectory(g_self) + L"\\CCD_TTS";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}


static void ResetProxyLog()
{
    const std::wstring path = DataDirectory() + L"\\CCD_TTS_proxy.log";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

static void AppendProxyLog(const wchar_t* text)
{
    const std::wstring path = DataDirectory() + L"\\CCD_TTS_proxy.log";
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char line[512] = {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, line, sizeof(line) - 3, nullptr, nullptr);
    if (n > 1) {
        DWORD written = 0;
        WriteFile(h, line, static_cast<DWORD>(n - 1), &written, nullptr);
        WriteFile(h, "\r\n", 2, &written, nullptr);
    }
    CloseHandle(h);
}

static HMODULE EnsureRealDInput8()
{
    if (g_real) return g_real;

    wchar_t sysdir[MAX_PATH] = {};
    if (!GetSystemDirectoryW(sysdir, MAX_PATH)) return nullptr;

    std::wstring path(sysdir);
    path += L"\\dinput8.dll";
    HMODULE mod = LoadLibraryW(path.c_str());
    if (mod) {
        InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(&g_real), mod, nullptr);
        if (g_real != mod) FreeLibrary(mod);
    }
    return g_real;
}

static DWORD WINAPI InitThread(LPVOID)
{
    ResetProxyLog();

    const std::wstring tts = ModuleDirectory(g_self) + L"\\CCD_TTS.dll";
    if (!LoadLibraryW(tts.c_str())) {
        wchar_t msg[256] = {};
        _snwprintf_s(msg, _countof(msg), _TRUNCATE,
                     L"ERROR: Failed to load CCD_TTS.dll (GetLastError=%lu)", GetLastError());
        AppendProxyLog(msg);
    } else {
        AppendProxyLog(L"CCD_TTS.dll loaded");
    }
    return 0;
}

extern "C" HRESULT WINAPI Proxy_DirectInput8Create(
    HINSTANCE hinst, DWORD version, REFIID riidltf, LPVOID* out, LPUNKNOWN outer)
{
    using Fn = HRESULT (WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    HMODULE real = EnsureRealDInput8();
    if (!real) return HRESULT_FROM_WIN32(GetLastError());
    auto fn = reinterpret_cast<Fn>(GetProcAddress(real, "DirectInput8Create"));
    return fn ? fn(hinst, version, riidltf, out, outer) : E_NOINTERFACE;
}

extern "C" HRESULT WINAPI Proxy_DllCanUnloadNow()
{
    using Fn = HRESULT (WINAPI*)();
    HMODULE real = EnsureRealDInput8();
    auto fn = real ? reinterpret_cast<Fn>(GetProcAddress(real, "DllCanUnloadNow")) : nullptr;
    return fn ? fn() : S_FALSE;
}

extern "C" HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID iid, LPVOID* out)
{
    using Fn = HRESULT (WINAPI*)(REFCLSID, REFIID, LPVOID*);
    HMODULE real = EnsureRealDInput8();
    auto fn = real ? reinterpret_cast<Fn>(GetProcAddress(real, "DllGetClassObject")) : nullptr;
    return fn ? fn(clsid, iid, out) : CLASS_E_CLASSNOTAVAILABLE;
}

extern "C" HRESULT WINAPI Proxy_DllRegisterServer()
{
    using Fn = HRESULT (WINAPI*)();
    HMODULE real = EnsureRealDInput8();
    auto fn = real ? reinterpret_cast<Fn>(GetProcAddress(real, "DllRegisterServer")) : nullptr;
    return fn ? fn() : E_NOTIMPL;
}

extern "C" HRESULT WINAPI Proxy_DllUnregisterServer()
{
    using Fn = HRESULT (WINAPI*)();
    HMODULE real = EnsureRealDInput8();
    auto fn = real ? reinterpret_cast<Fn>(GetProcAddress(real, "DllUnregisterServer")) : nullptr;
    return fn ? fn() : E_NOTIMPL;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = instance;
        DisableThreadLibraryCalls(instance);
        HANDLE h = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }
    return TRUE;
}
