// SAC Anti Cheat.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <iostream>
#include <conio.h>
#include <windows.h>
#include <stdio.h>
#include <tchar.h>



EXCEPTION_DISPOSITION ExceptionRoutine(
	PEXCEPTION_RECORD ExceptionRecord,
	PVOID             EstablisherFrame,
	PCONTEXT          ContextRecord,
	PVOID             DispatcherContext)
{
	if (EXCEPTION_INVALID_HANDLE == ExceptionRecord->ExceptionCode)
	{
		std::cout << "Stop debugging program!" << std::endl;
		exit(-1);
	}
	return ExceptionContinueExecution;
}

#define FLG_HEAP_ENABLE_TAIL_CHECK   0x10
#define FLG_HEAP_ENABLE_FREE_CHECK   0x20
#define FLG_HEAP_VALIDATE_PARAMETERS 0x40
#define NT_GLOBAL_FLAG_DEBUGGED (FLG_HEAP_ENABLE_TAIL_CHECK | FLG_HEAP_ENABLE_FREE_CHECK | FLG_HEAP_VALIDATE_PARAMETERS)

PVOID GetPEB64()
{
	PVOID pPeb = 0;
#ifndef _WIN64
	// 1. There are two copies of PEB - PEB64 and PEB32 in WOW64 process
	// 2. PEB64 follows after PEB32
	// 3. This is true for versions lower than Windows 8, else __readfsdword returns address of real PEB64
	if (IsWin8OrHigher())
	{
		BOOL isWow64 = FALSE;
		typedef BOOL(WINAPI *pfnIsWow64Process)(HANDLE hProcess, PBOOL isWow64);
		pfnIsWow64Process fnIsWow64Process = (pfnIsWow64Process)
			GetProcAddress(GetModuleHandleA("Kernel32.dll"), "IsWow64Process");
		if (fnIsWow64Process(GetCurrentProcess(), &isWow64))
		{
			if (isWow64)
			{
				pPeb = (PVOID)__readfsdword(0x0C * sizeof(PVOID));
				pPeb = (PVOID)((PBYTE)pPeb + 0x1000);
			}
		}
	}
#endif
	return pPeb;
}


// Current PEB for 64bit and 32bit processes accordingly
PVOID GetPEB()
{
	return (PVOID)__readgsqword(0x0C * sizeof(PVOID));
}


PIMAGE_NT_HEADERS GetImageNtHeaders(PBYTE pImageBase)
{
	PIMAGE_DOS_HEADER pImageDosHeader = (PIMAGE_DOS_HEADER)pImageBase;
	return (PIMAGE_NT_HEADERS)(pImageBase + pImageDosHeader->e_lfanew);
}
PIMAGE_SECTION_HEADER FindRDataSection(PBYTE pImageBase)
{
	static const std::string rdata = ".rdata";
	PIMAGE_NT_HEADERS pImageNtHeaders = GetImageNtHeaders(pImageBase);
	PIMAGE_SECTION_HEADER pImageSectionHeader = IMAGE_FIRST_SECTION(pImageNtHeaders);
	int n = 0;
	for (; n < pImageNtHeaders->FileHeader.NumberOfSections; ++n)
	{
		if (rdata == (char*)pImageSectionHeader[n].Name)
		{
			break;
		}
	}
	return &pImageSectionHeader[n];
}
#pragma warning(disable : 4996)
WORD GetVersionWord()
{
	OSVERSIONINFO verInfo = { sizeof(OSVERSIONINFO) };
	GetVersionEx(&verInfo);
	return MAKEWORD(verInfo.dwMinorVersion, verInfo.dwMajorVersion);
}
BOOL IsWin8OrHigher() { return GetVersionWord() >= _WIN32_WINNT_WIN8; }
BOOL IsVistaOrHigher() { return GetVersionWord() >= _WIN32_WINNT_VISTA; }

int GetHeapFlagsOffset(bool x64)
{
	return x64 ?
		IsVistaOrHigher() ? 0x70 : 0x14 : //x64 offsets
		IsVistaOrHigher() ? 0x40 : 0x0C; //x86 offsets
}
int GetHeapForceFlagsOffset(bool x64)
{
	return x64 ?
		IsVistaOrHigher() ? 0x74 : 0x18 : //x64 offsets
		IsVistaOrHigher() ? 0x44 : 0x10; //x86 offsets
}

typedef NTSTATUS(NTAPI *pfnNtQueryInformationProcess)(
	_In_      HANDLE           ProcessHandle,
	_In_      UINT             ProcessInformationClass,
	_Out_     PVOID            ProcessInformation,
	_In_      ULONG            ProcessInformationLength,
	_Out_opt_ PULONG           ReturnLength
	);
const UINT ProcessDebugPort = 7;

DWORD CalcFuncCrc(PUCHAR funcBegin, PUCHAR funcEnd)
{
	DWORD crc = 0;
	for (; funcBegin < funcEnd; ++funcBegin)
	{
		crc += *funcBegin;
	}
	return crc;
}
#pragma auto_inline(off)
VOID DebuggeeFunction()
{
	int calc = 0;
	calc += 2;
	calc <<= 8;
	calc -= 3;
}
VOID DebuggeeFunctionEnd()
{
};
#pragma auto_inline(on)
DWORD g_origCrc = 0x2bd0;

typedef NTSTATUS(NTAPI *pfnNtSetInformationThread)(
	_In_ HANDLE ThreadHandle,
	_In_ ULONG  ThreadInformationClass,
	_In_ PVOID  ThreadInformation,
	_In_ ULONG  ThreadInformationLength
	);
const ULONG ThreadHideFromDebugger = 0x11;

const int g_doSmthExecutionTime = 1050;
void DoSmth()
{
	Sleep(1000);
}
#define JUNK_CODE_ONE        \
    __asm{push eax}            \
    __asm{xor eax, eax}        \
    __asm{setpo al}            \
    __asm{push edx}            \
    __asm{xor edx, eax}        \
    __asm{sal edx, 2}        \
    __asm{xchg eax, edx}    \
    __asm{pop edx}            \
    __asm{or eax, ecx}        \
    __asm{pop eax}

void SetupMain()
{
	HMODULE hNtDll = LoadLibrary(TEXT("ntdll.dll"));
	pfnNtSetInformationThread NtSetInformationThread = (pfnNtSetInformationThread)
		GetProcAddress(hNtDll, "NtSetInformationThread");
	NTSTATUS status = NtSetInformationThread(GetCurrentThread(),
		ThreadHideFromDebugger, NULL, 0);

	DWORD OldProtect = 0;


	STARTUPINFO info = { sizeof(info) };
	PROCESS_INFORMATION processInfo;

	while (1)
	{


		SYSTEMTIME sysTimeStart;
		SYSTEMTIME sysTimeEnd;
		FILETIME timeStart, timeEnd;
		HANDLE hProcess = NULL;
		DEBUG_EVENT de;
		PROCESS_INFORMATION pi;
		STARTUPINFO si;
		ZeroMemory(&pi, sizeof(PROCESS_INFORMATION));
		ZeroMemory(&si, sizeof(STARTUPINFO));
		ZeroMemory(&de, sizeof(DEBUG_EVENT));

		GetStartupInfo(&si);

		// Create the copy of ourself
		CreateProcess(NULL, GetCommandLine(), NULL, NULL, FALSE,
			DEBUG_PROCESS, NULL, NULL, &si, &pi);

		// Continue execution
		ContinueDebugEvent(pi.dwProcessId, pi.dwThreadId, DBG_CONTINUE);

		// Wait for an event
		WaitForDebugEvent(&de, INFINITE);
		GetSystemTime(&sysTimeStart);
		DoSmth();
		GetSystemTime(&sysTimeEnd);
		SystemTimeToFileTime(&sysTimeStart, &timeStart);
		SystemTimeToFileTime(&sysTimeEnd, &timeEnd);
		double timeExecution = (timeEnd.dwLowDateTime - timeStart.dwLowDateTime) / 10000.0;
		if (timeExecution < g_doSmthExecutionTime)
		{

			if (IsDebuggerPresent())
			{
				std::cout << "Stop debugging program! 13" << std::endl;
				exit(-1);
			}
			BOOL isDebuggerPresent = FALSE;
			if (CheckRemoteDebuggerPresent(GetCurrentProcess(), &isDebuggerPresent))
			{
				if (isDebuggerPresent)
				{
					std::cout << "Stop debugging program! 12" << std::endl;
					exit(-1);
				}
			}
			PVOID pPeb = GetPEB();
			PVOID pPeb64 = GetPEB64();
			DWORD offsetNtGlobalFlag = 0;
			offsetNtGlobalFlag = 0xBC;
			DWORD NtGlobalFlag = *(PDWORD)((PBYTE)pPeb + offsetNtGlobalFlag);
			if (NtGlobalFlag & NT_GLOBAL_FLAG_DEBUGGED)
			{
				std::cout << "Stop debugging program! 11" << std::endl;
				exit(-1);
			}
			if (pPeb64)
			{
				DWORD NtGlobalFlagWow64 = *(PDWORD)((PBYTE)pPeb64 + 0xBC);
				if (NtGlobalFlagWow64 & NT_GLOBAL_FLAG_DEBUGGED)
				{
					std::cout << "Stop debugging program! 10" << std::endl;
					exit(-1);
				}
			}
			PBYTE pImageBase = (PBYTE)GetModuleHandle(NULL);
			PIMAGE_NT_HEADERS pImageNtHeaders = GetImageNtHeaders(pImageBase);
			PIMAGE_LOAD_CONFIG_DIRECTORY pImageLoadConfigDirectory = (PIMAGE_LOAD_CONFIG_DIRECTORY)(pImageBase
				+ pImageNtHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG].VirtualAddress);
			if (pImageLoadConfigDirectory->GlobalFlagsClear != 0)
			{
				std::cout << "Stop debugging program! 9" << std::endl;
				exit(-1);
			}
			HANDLE hExecutable = INVALID_HANDLE_VALUE;
			HANDLE hExecutableMapping = NULL;
			PBYTE pMappedImageBase = NULL;
			__try
			{
				PBYTE pImageBase = (PBYTE)GetModuleHandle(NULL);
				PIMAGE_SECTION_HEADER pImageSectionHeader = FindRDataSection(pImageBase);
				TCHAR pszExecutablePath[MAX_PATH];
				DWORD dwPathLength = GetModuleFileName(NULL, pszExecutablePath, MAX_PATH);
				if (0 == dwPathLength) __leave;
				hExecutable = CreateFile(pszExecutablePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
				if (INVALID_HANDLE_VALUE == hExecutable) __leave;
				hExecutableMapping = CreateFileMapping(hExecutable, NULL, PAGE_READONLY, 0, 0, NULL);
				if (NULL == hExecutableMapping) __leave;
				pMappedImageBase = (PBYTE)MapViewOfFile(hExecutableMapping, FILE_MAP_READ, 0, 0,
					pImageSectionHeader->PointerToRawData + pImageSectionHeader->SizeOfRawData);
				if (NULL == pMappedImageBase) __leave;
				PIMAGE_NT_HEADERS pImageNtHeaders = GetImageNtHeaders(pMappedImageBase);
				PIMAGE_LOAD_CONFIG_DIRECTORY pImageLoadConfigDirectory = (PIMAGE_LOAD_CONFIG_DIRECTORY)(pMappedImageBase
					+ (pImageSectionHeader->PointerToRawData
						+ (pImageNtHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG].VirtualAddress - pImageSectionHeader->VirtualAddress)));
				if (pImageLoadConfigDirectory->GlobalFlagsClear != 0)
				{
					std::cout << "Stop debugging program! 8" << std::endl;
					exit(-1);
				}
			}
			__finally
			{
				if (NULL != pMappedImageBase)
					UnmapViewOfFile(pMappedImageBase);
				if (NULL != hExecutableMapping)
					CloseHandle(hExecutableMapping);
				if (INVALID_HANDLE_VALUE != hExecutable)
					CloseHandle(hExecutable);
			}
			PVOID tpPeb = GetPEB();
			PVOID tpPeb64 = GetPEB64();
			PVOID heap = 0;
			DWORD offsetProcessHeap = 0;
			PDWORD heapFlagsPtr = 0, heapForceFlagsPtr = 0;
			BOOL x64 = FALSE;
			x64 = TRUE;
			offsetProcessHeap = 0x30;

			heap = (PVOID)*(PDWORD_PTR)((PBYTE)tpPeb + offsetProcessHeap);
			heapFlagsPtr = (PDWORD)((PBYTE)heap + GetHeapFlagsOffset(x64));
			heapForceFlagsPtr = (PDWORD)((PBYTE)heap + GetHeapForceFlagsOffset(x64));
			if (*heapFlagsPtr & ~HEAP_GROWABLE || *heapForceFlagsPtr != 0)
			{
				std::cout << "Stop debugging program! 7" << std::endl;
				exit(-1);
			}
			if (tpPeb64)
			{
				heap = (PVOID)*(PDWORD_PTR)((PBYTE)tpPeb64 + 0x30);
				heapFlagsPtr = (PDWORD)((PBYTE)heap + GetHeapFlagsOffset(true));
				heapForceFlagsPtr = (PDWORD)((PBYTE)heap + GetHeapForceFlagsOffset(true));
				if (*heapFlagsPtr & ~HEAP_GROWABLE || *heapForceFlagsPtr != 0)
				{
					std::cout << "Stop debugging program! 6" << std::endl;
					exit(-1);
				}
			}
			BOOL ttisDebuggerPresent = FALSE;
			if (CheckRemoteDebuggerPresent(GetCurrentProcess(), &ttisDebuggerPresent))
			{
				if (ttisDebuggerPresent)
				{
					std::cout << "Stop debugging program! 5" << std::endl;
					exit(-1);
				}
			}
			pfnNtQueryInformationProcess NtQueryInformationProcess = NULL;
			NTSTATUS status;
			DWORD tisDebuggerPresent = 0;
			HMODULE hNtDll = LoadLibrary(TEXT("ntdll.dll"));

			if (NULL != hNtDll)
			{
				NtQueryInformationProcess = (pfnNtQueryInformationProcess)GetProcAddress(hNtDll, "NtQueryInformationProcess");
				if (NULL != NtQueryInformationProcess)
				{
					status = NtQueryInformationProcess(
						GetCurrentProcess(),
						ProcessDebugPort,
						&tisDebuggerPresent,
						sizeof(DWORD),
						NULL);
					if (status == 0x00000000 && tisDebuggerPresent != 0)
					{
						std::cout << "Stop debugging program! 4" << std::endl;
						exit(-1);
					}
				}
			}
			SYSTEMTIME sysTimeStart;
			SYSTEMTIME sysTimeEnd;
			FILETIME timeStart, timeEnd;
			LPWSTR ProcessId = (LPWSTR)GetCurrentProcessId();
			if (CreateProcess(L"Snake Game.exe", NULL, NULL, NULL, TRUE, 0, NULL, NULL, &info, &processInfo))
			{

				GetSystemTime(&sysTimeStart);
				DoSmth();
				GetSystemTime(&sysTimeEnd);
				SystemTimeToFileTime(&sysTimeStart, &timeStart);
				SystemTimeToFileTime(&sysTimeEnd, &timeEnd);
				double timeExecution = (timeEnd.dwLowDateTime - timeStart.dwLowDateTime) / 10000.0;
				if (timeExecution < g_doSmthExecutionTime)
				{

					WaitForSingleObject(processInfo.hProcess, INFINITE);
					CloseHandle(processInfo.hProcess);
					CloseHandle(processInfo.hThread);
				}
				else
				{
					std::cout << "Stop debugging program! 2" << std::endl;
					exit(-1);
				}
			}
		}
		else
		{
			std::cout << "Stop debugging program! 1" << std::endl;
			exit(-1);
		}
	}

}

int main()
{
	SYSTEMTIME sysTimeStart;
	SYSTEMTIME sysTimeEnd;
	FILETIME timeStart, timeEnd;
	HMODULE hNtDll = LoadLibrary(TEXT("ntdll.dll"));
	pfnNtSetInformationThread NtSetInformationThread = (pfnNtSetInformationThread)
		GetProcAddress(hNtDll, "NtSetInformationThread");
	NTSTATUS status = NtSetInformationThread(GetCurrentThread(),
		ThreadHideFromDebugger, NULL, 0);

	if (IsDebuggerPresent())
	{
		std::cout << "Stop debugging program! 13" << std::endl;
		exit(-1);
	}
	BOOL tisDebuggerPresent = FALSE;
	if (CheckRemoteDebuggerPresent(GetCurrentProcess(), &tisDebuggerPresent))
	{
		if (tisDebuggerPresent)
		{
			std::cout << "Stop debugging program! 12" << std::endl;
			exit(-1);
		}
	}
	PVOID pPeb = GetPEB();
	PVOID pPeb64 = GetPEB64();
	DWORD offsetNtGlobalFlag = 0;
	offsetNtGlobalFlag = 0xBC;
	DWORD NtGlobalFlag = *(PDWORD)((PBYTE)pPeb + offsetNtGlobalFlag);
	if (NtGlobalFlag & NT_GLOBAL_FLAG_DEBUGGED)
	{
		std::cout << "Stop debugging program! 11" << std::endl;
		exit(-1);
	}
	if (pPeb64)
	{
		DWORD NtGlobalFlagWow64 = *(PDWORD)((PBYTE)pPeb64 + 0xBC);
		if (NtGlobalFlagWow64 & NT_GLOBAL_FLAG_DEBUGGED)
		{
			std::cout << "Stop debugging program! 10" << std::endl;
			exit(-1);
		}
	}
	PBYTE pImageBase = (PBYTE)GetModuleHandle(NULL);
	PIMAGE_NT_HEADERS pImageNtHeaders = GetImageNtHeaders(pImageBase);
	PIMAGE_LOAD_CONFIG_DIRECTORY pImageLoadConfigDirectory = (PIMAGE_LOAD_CONFIG_DIRECTORY)(pImageBase
		+ pImageNtHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG].VirtualAddress);
	if (pImageLoadConfigDirectory->GlobalFlagsClear != 0)
	{
		std::cout << "Stop debugging program! 9" << std::endl;
		exit(-1);
	}
	HANDLE hExecutable = INVALID_HANDLE_VALUE;
	HANDLE hExecutableMapping = NULL;
	PBYTE pMappedImageBase = NULL;
	__try
	{
		PBYTE pImageBase = (PBYTE)GetModuleHandle(NULL);
		PIMAGE_SECTION_HEADER pImageSectionHeader = FindRDataSection(pImageBase);
		TCHAR pszExecutablePath[MAX_PATH];
		DWORD dwPathLength = GetModuleFileName(NULL, pszExecutablePath, MAX_PATH);
		if (0 == dwPathLength) __leave;
		hExecutable = CreateFile(pszExecutablePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
		if (INVALID_HANDLE_VALUE == hExecutable) __leave;
		hExecutableMapping = CreateFileMapping(hExecutable, NULL, PAGE_READONLY, 0, 0, NULL);
		if (NULL == hExecutableMapping) __leave;
		pMappedImageBase = (PBYTE)MapViewOfFile(hExecutableMapping, FILE_MAP_READ, 0, 0,
			pImageSectionHeader->PointerToRawData + pImageSectionHeader->SizeOfRawData);
		if (NULL == pMappedImageBase) __leave;
		PIMAGE_NT_HEADERS pImageNtHeaders = GetImageNtHeaders(pMappedImageBase);
		PIMAGE_LOAD_CONFIG_DIRECTORY pImageLoadConfigDirectory = (PIMAGE_LOAD_CONFIG_DIRECTORY)(pMappedImageBase
			+ (pImageSectionHeader->PointerToRawData
				+ (pImageNtHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG].VirtualAddress - pImageSectionHeader->VirtualAddress)));
		if (pImageLoadConfigDirectory->GlobalFlagsClear != 0)
		{
			std::cout << "Stop debugging program! 8" << std::endl;
			exit(-1);
		}
	}
	__finally
	{
		if (NULL != pMappedImageBase)
			UnmapViewOfFile(pMappedImageBase);
		if (NULL != hExecutableMapping)
			CloseHandle(hExecutableMapping);
		if (INVALID_HANDLE_VALUE != hExecutable)
			CloseHandle(hExecutable);
	}
	PVOID tpPeb = GetPEB();
	PVOID tpPeb64 = GetPEB64();
	PVOID heap = 0;
	DWORD offsetProcessHeap = 0;
	PDWORD heapFlagsPtr = 0, heapForceFlagsPtr = 0;
	BOOL x64 = FALSE;
	x64 = TRUE;
	offsetProcessHeap = 0x30;
	heap = (PVOID)*(PDWORD_PTR)((PBYTE)tpPeb + offsetProcessHeap);
	heapFlagsPtr = (PDWORD)((PBYTE)heap + GetHeapFlagsOffset(x64));
	heapForceFlagsPtr = (PDWORD)((PBYTE)heap + GetHeapForceFlagsOffset(x64));
	if (*heapFlagsPtr & ~HEAP_GROWABLE || *heapForceFlagsPtr != 0)
	{
		std::cout << "Stop debugging program! 7" << std::endl;
		exit(-1);
	}
	if (tpPeb64)
	{
		heap = (PVOID)*(PDWORD_PTR)((PBYTE)tpPeb64 + 0x30);
		heapFlagsPtr = (PDWORD)((PBYTE)heap + GetHeapFlagsOffset(true));
		heapForceFlagsPtr = (PDWORD)((PBYTE)heap + GetHeapForceFlagsOffset(true));
		if (*heapFlagsPtr & ~HEAP_GROWABLE || *heapForceFlagsPtr != 0)
		{
			std::cout << "Stop debugging program! 6" << std::endl;
			exit(-1);
		}
	}
	BOOL ttisDebuggerPresent = FALSE;
	if (CheckRemoteDebuggerPresent(GetCurrentProcess(), &ttisDebuggerPresent))
	{
		if (ttisDebuggerPresent)
		{
			std::cout << "Stop debugging program! 5" << std::endl;
			exit(-1);
		}
	}
	pfnNtQueryInformationProcess NtQueryInformationProcess = NULL;
	NTSTATUS ttstatus;
	DWORD tttisDebuggerPresent = 0;
	HMODULE thNtDll = LoadLibrary(TEXT("ntdll.dll"));
	SetupMain();
	if (NULL != thNtDll)
	{
		NtQueryInformationProcess = (pfnNtQueryInformationProcess)GetProcAddress(thNtDll, "NtQueryInformationProcess");
		if (NULL != NtQueryInformationProcess)
		{
			ttstatus = NtQueryInformationProcess(
				GetCurrentProcess(),
				ProcessDebugPort,
				&tttisDebuggerPresent,
				sizeof(DWORD),
				NULL);
			if (ttstatus == 0x00000000 && tttisDebuggerPresent != 0)
			{
				std::cout << "Stop debugging program! 4" << std::endl;
				exit(-1);
			}
		}

	}

	
	return 0;

}

