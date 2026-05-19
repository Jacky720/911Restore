// dllmain.cpp : Defines the entry point for the DLL application.
#include "pch.h"
#include "helper.hpp"
#include <filesystem>
//#include <libloaderapi.h>
#include <mutex>

extern void Mod();


std::mutex mainThreadFinishedMutex;
std::condition_variable mainThreadFinishedVar;
bool mainThreadFinished = false;
inline std::string sExeName;
inline std::filesystem::path sExePath;

DWORD __stdcall DetachThread(void* module)
{
    FreeLibraryAndExitThread(static_cast<HMODULE>(module), 0);
}

DWORD __stdcall Main(void*)
{
    // Get game name and exe path
    WCHAR exePath[_MAX_PATH] = { 0 };
    GetModuleFileNameW(baseModule, exePath, MAX_PATH);
    sExePath = exePath;
    sExeName = sExePath.filename().string();
    sExePath = sExePath.remove_filename();

    Mod();
    // Signal any threads (e.g., the memset hook) that are waiting for initialization to finish.
    {
        std::lock_guard lock(mainThreadFinishedMutex);
        mainThreadFinished = true;
    }
    mainThreadFinishedVar.notify_all();
    return TRUE;
}

std::mutex memsetHookMutex;
bool memsetHookCalled = false;
static void* (__cdecl* memset_Fn)(void* Dst, int Val, size_t Size); // Pointer to the next function in the memset chain (could be another hook or the real CRT memset).
static void* __cdecl memset_Hook(void* Dst, int Val, size_t Size) // Our memset hook, which blocks the game's main thread until initialization is complete.
{
    std::lock_guard lock(memsetHookMutex);

    if (!memsetHookCalled)
    {
        memsetHookCalled = true;

        // Restore the original (or previously-hooked) memset in the IAT.
        // This ensures future memset calls bypass our hook and run at full speed.
        Memory::WriteIAT(baseModule, "VCRUNTIME140.dll", "memset", memset_Fn);

        // Block the current thread here until our main initialization is complete.
        std::unique_lock finishedLock(mainThreadFinishedMutex);
        mainThreadFinishedVar.wait(finishedLock, []
            {
                return mainThreadFinished;
            });
    }

    // Forward the memset call to the next function (another hook or the real memset).
    return reinterpret_cast<decltype(memset_Fn)>(memset_Fn)(Dst, Val, Size);
}



BOOL APIENTRY DllMain(HMODULE hModule,
    DWORD  ul_reason_for_call,
    LPVOID lpReserved
)
{
    if (ul_reason_for_call != DLL_PROCESS_ATTACH)
    {
        return TRUE;
    }

    DisableThreadLibraryCalls(hModule);

    WCHAR exePath[_MAX_PATH] = { 0 };
    GetModuleFileNameW(baseModule, exePath, MAX_PATH);

    const std::filesystem::path currentExePath = exePath;
    const std::string currentExeName = currentExePath.filename().string();

    if (_stricmp(currentExeName.c_str(), "METAL GEAR SOLID2.exe") != 0)
    {
        if (const HANDLE detachHandle = CreateThread(nullptr, 0, DetachThread, hModule, 0, nullptr))
        {
            CloseHandle(detachHandle);
        }

        return TRUE;
    }

    if (GetModuleHandleA("VCRUNTIME140.dll"))
    {
        // Read the current IAT entry for memset in the base module.
        // Note: it may already point to another mod's hook if they loaded first.
        void* currentIATMemset = Memory::ReadIAT(baseModule, "VCRUNTIME140.dll", "memset");

        // Save the current pointer so we can call it later (chaining to the next hook or real memset).
        memset_Fn = reinterpret_cast<decltype(memset_Fn)>(currentIATMemset);

        // Overwrite the IAT entry with our memset_Hook, so our code intercepts memset calls.
        // We always overwrite unconditionally to ensure our hook is active.
        // This will prevent other mods that also hook memset from unpausing the main thread before our Main() has finished.

        Memory::WriteIAT(baseModule, "VCRUNTIME140.dll", "memset", &memset_Hook);
    }

    // Create our main thread, which runs the initialization logic.
    if (const HANDLE mainHandle = CreateThread(nullptr, 0, Main, nullptr, CREATE_SUSPENDED, nullptr))
    {
        SetThreadPriority(mainHandle, THREAD_PRIORITY_TIME_CRITICAL); // Give our thread higher priority than the game's.
        ResumeThread(mainHandle);
        CloseHandle(mainHandle);
    }

    return TRUE;
}
