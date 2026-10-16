#include <Windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <print>        // C++23 std::println
#include <chrono>
#include <vector>

#pragma comment(lib, "psapi.lib")

#include "Generators/CppGenerator.h"
#include "Generators/MappingGenerator.h"
#include "Generators/IDAMappingGenerator.h"
#include "Generators/DumpspaceGenerator.h"
#include "Generators/Generator.h"

typedef NTSTATUS(NTAPI* pfnNtQueryInformationThread)(
	HANDLE ThreadHandle,
	ULONG ThreadInformationClass,
	PVOID ThreadInformation,
	ULONG ThreadInformationLength,
	PULONG ReturnLength
	);

// ---------------------- 控制台初始化（兼容 std::println） ----------------------
void SetupConsole()
{
	// 如果控制台不存在则分配；若游戏已分配过则直接附加上去
	if (!AllocConsole())
	{
		AttachConsole(ATTACH_PARENT_PROCESS);
	}

	FILE* Dummy = nullptr;
	// 务必使用 "CONOUT$" 和 "CONIN$"（带 $），同时重定向 stdout 和 stderr
	freopen_s(&Dummy, "CONOUT$", "w", stdout);
	freopen_s(&Dummy, "CONOUT$", "w", stderr);
	freopen_s(&Dummy, "CONIN$", "r", stdin);

	// 关键：更新 Windows 原生句柄，让 std::println 的 WriteConsoleW 能直接生效
	HANDLE hStdOut = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, OPEN_EXISTING, 0, NULL);
	HANDLE hStdErr = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, OPEN_EXISTING, 0, NULL);
	HANDLE hStdIn = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, OPEN_EXISTING, 0, NULL);

	SetStdHandle(STD_OUTPUT_HANDLE, hStdOut);
	SetStdHandle(STD_ERROR_HANDLE, hStdErr);
	SetStdHandle(STD_INPUT_HANDLE, hStdIn);

	// 支持 UTF-8 输出
	SetConsoleOutputCP(CP_UTF8);
	SetConsoleCP(CP_UTF8);

	// 关闭缓冲区
	setvbuf(stdout, nullptr, _IONBF, 0);
	setvbuf(stderr, nullptr, _IONBF, 0);
}

// ---------------------- 精确冻结游戏线程 ----------------------
std::vector<HANDLE> SuspendGameThreadsOnly()
{
	std::vector<HANDLE> suspendedThreads;
	DWORD currentProcessId = GetCurrentProcessId();
	DWORD currentThreadId = GetCurrentThreadId();

	MODULEINFO modInfo = { 0 };
	HMODULE hMainModule = GetModuleHandle(NULL);
	if (!GetModuleInformation(GetCurrentProcess(), hMainModule, &modInfo, sizeof(modInfo)))
	{
		return suspendedThreads;
	}
	uintptr_t gameBase = (uintptr_t)modInfo.lpBaseOfDll;
	uintptr_t gameEnd = gameBase + modInfo.SizeOfImage;

	static auto NtQueryInfoThread = (pfnNtQueryInformationThread)GetProcAddress(
		GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread"
	);

	HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (hSnapshot == INVALID_HANDLE_VALUE)
	{
		return suspendedThreads;
	}

	THREADENTRY32 te;
	te.dwSize = sizeof(THREADENTRY32);

	if (Thread32First(hSnapshot, &te))
	{
		do
		{
			if (te.th32OwnerProcessID == currentProcessId && te.th32ThreadID != currentThreadId)
			{
				HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
				if (!hThread)
					continue;

				PVOID startAddress = nullptr;
				bool isGameThread = false;

				if (NtQueryInfoThread)
				{
					if (NtQueryInfoThread(hThread, 9, &startAddress, sizeof(startAddress), nullptr) == 0)
					{
						uintptr_t addr = (uintptr_t)startAddress;
						// 仅冻结游戏主程序代码段内的线程
						if (addr >= gameBase && addr < gameEnd)
						{
							isGameThread = true;
						}
					}
				}

				if (isGameThread)
				{
					if (SuspendThread(hThread) != (DWORD)-1)
					{
						suspendedThreads.push_back(hThread);
						continue;
					}
				}

				CloseHandle(hThread);
			}
		} while (Thread32Next(hSnapshot, &te));
	}

	CloseHandle(hSnapshot);
	return suspendedThreads;
}

void ResumeOtherThreads(std::vector<HANDLE>& threads)
{
	for (HANDLE hThread : threads)
	{
		if (hThread && hThread != INVALID_HANDLE_VALUE)
		{
			ResumeThread(hThread);
			CloseHandle(hThread);
		}
	}
	threads.clear();
}

// --------------------------------------------------------------------

DWORD MainThread(HMODULE Module)
{
	// 初始化控制台并绑定
	SetupConsole();

	std::println("Initializing [Dumper-7]...");

	Settings::Config::Load();
	Settings::Config::DelayDumperStart();

	std::println("Started Generation [Dumper-7]!");
	auto DumpStartTime = std::chrono::high_resolution_clock::now();

	Generator::InitEngineCore();
	Generator::InitInternal();

	if (Settings::Generator::GameName.empty() && Settings::Generator::GameVersion.empty())
	{
		FString Name;
		FString Version;
		UEClass Kismet = ObjectArray::FindClassFast("KismetSystemLibrary");
		UEFunction GetGameName = Kismet.GetFunction("KismetSystemLibrary", "GetGameName");
		UEFunction GetEngineVersion = Kismet.GetFunction("KismetSystemLibrary", "GetEngineVersion");

		Kismet.ProcessEvent(GetGameName, &Name);
		Kismet.ProcessEvent(GetEngineVersion, &Version);

		Settings::Generator::GameName = Name.ToString();
		Settings::Generator::GameVersion = Version.ToString();
	}

	std::println("GameName: {}", Settings::Generator::GameName);
	std::println("GameVersion: {}", Settings::Generator::GameVersion);
	std::println("FolderName: {}-{}\n", Settings::Generator::GameVersion, Settings::Generator::GameName);

	// 冻结游戏模块线程
	std::println("Suspending Game-only threads to prevent crashes...");
	std::vector<HANDLE> suspendedThreads = SuspendGameThreadsOnly();
	std::println("Successfully suspended {} game threads.\n", suspendedThreads.size());

	// 开始生成 SDK
	Generator::Generate<CppGenerator>();
	Generator::Generate<MappingGenerator>();
	Generator::Generate<IDAMappingGenerator>();
	Generator::Generate<DumpspaceGenerator>();

	// 恢复线程
	std::println("\nResuming game threads...");
	ResumeOtherThreads(suspendedThreads);
	std::println("All threads resumed.");

	auto DumpFinishTime = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double, std::milli> DumpTime = DumpFinishTime - DumpStartTime;

	std::println("\n\nGenerating SDK took ({:.2f}ms)\n\n", DumpTime.count());

	if (Settings::Debug::bExecuteSDKTestScript)
	{
		CppGenerator::ExecuteSDKCompilationTestScript();
	}

	std::println("\nPress F6 to unload\n");

	while (true)
	{
		if (GetAsyncKeyState(VK_F6) & 1)
		{
			fclose(stdout);
			fclose(stderr);
			fclose(stdin);
			FreeConsole();

			FreeLibraryAndExitThread(Module, 0);
		}

		Sleep(100);
	}

	return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		CreateThread(0, 0, (LPTHREAD_START_ROUTINE)MainThread, hModule, 0, 0);
		break;
	}

	return TRUE;
}