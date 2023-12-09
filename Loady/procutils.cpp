#include "procutils.h"




namespace processutils
{

    inline PEB* PebBaseAddress(HANDLE hProcess)
    {

        if (hProcess == INVALID_HANDLE_VALUE)
            return nullptr;

        ImportUtils utils(GetModuleHandleW(L"ntdll.dll"));

        auto ptrNtQueryInformationProcess = utils.DynamicImporter<prototypes::fpNtQueryInformationProcess>("NtQueryInformationProcess");

        if (!ptrNtQueryInformationProcess)
            return nullptr;

        utils.~ImportUtils();

        PROCESS_BASIC_INFORMATION pbi{};

        NTSTATUS status = ptrNtQueryInformationProcess(hProcess, ProcessBasicInformation, &pbi, sizeof(pbi), NULL);

        if (!NT_SUCCESS(status))
            return nullptr;

        return pbi.PebBaseAddress;
    }


    PVOID GetProcessHeapAddress(HANDLE hProcess)
    {

        PEB* pebBase = PebBaseAddress(hProcess);

        PVOID processHeapAddress = (PVOID)((char*)pebBase + 0x30);
        PVOID processHeap = nullptr;
        SIZE_T bytesRead;

        BOOL preadResult = ReadProcessMemory(hProcess, processHeapAddress, &processHeap, sizeof(processHeap), &bytesRead);
        return (preadResult && bytesRead == sizeof(processHeap)) ? processHeap : nullptr;
    }


    int GetHandleCount(DWORD pid, int type)
    {

        PSYSTEM_HANDLE_INFORMATION buffer{};
        ULONG bufferSize = 0xffffff;
        buffer = (PSYSTEM_HANDLE_INFORMATION)malloc(bufferSize);
        NTSTATUS status;

        ImportUtils utils(GetModuleHandleW(L"ntdll.dll"));

        auto NtQuerySystemInformation = utils.DynamicImporter<prototypes::fpNtQuerySystemInformation>("NtQuerySystemInformation");
        status = NtQuerySystemInformation(0x10, buffer, bufferSize, NULL);
        utils.~ImportUtils();

        if (!NT_SUCCESS(status))
        {
            free(buffer);
            return -1;
        }

        const PVOID ProcAddress = nullptr;
        int count = 0;

        for (ULONG i = 0; i <= buffer->HandleCount; i++)
        {
            if ((buffer->Handles[i].ProcessId == pid))
                if (buffer->Handles[i].ObjectTypeNumber == type)
                    count += 1;
        }

        free(buffer);
        return count;
    }


    LPTSTR GetProcessSid(HANDLE hProcess)
    {

        HANDLE hToken = nullptr;
        ImportUtils utils(GetModuleHandleW(L"ntdll.dll"));

        auto NtOpenProcessToken = utils.DynamicImporter<prototypes::fpNtOpenProcessToken>("NtOpenProcessToken");

        if (!NT_SUCCESS(NtOpenProcessToken(hProcess, TOKEN_QUERY, &hToken)))
            return nullptr;

        std::unique_ptr<void, decltype(&CloseHandle)> tokenGuard(hToken, CloseHandle);

        auto NtQueryInformationToken = utils.DynamicImporter<prototypes::fpNtQueryInformationToken>("NtQueryInformationToken");

        DWORD dwSize = 0;
        NtQueryInformationToken(hToken, TokenUser, NULL, 0, &dwSize);

        utils.~ImportUtils();

        auto buffer = std::make_unique<BYTE[]>(dwSize);

        if (!NT_SUCCESS(NtQueryInformationToken(hToken, TokenUser, buffer.get(), dwSize, &dwSize)))
            return nullptr;

        LPTSTR sidString = nullptr;

        if (!ConvertSidToStringSidW(reinterpret_cast<PTOKEN_USER>(buffer.get())->User.Sid, &sidString))
            return nullptr;

        std::unique_ptr<TCHAR, decltype(&LocalFree)> sidGuard(sidString, LocalFree);

        return _tcsdup(sidString);
    }


    int IsTokenPresent(HANDLE hToken, const wchar_t* privilegeType)
    {
        
        int fail = -1;
        NTSTATUS status;

        ImportUtils utils(GetModuleHandleW(L"ntdll.dll"));

        auto NtPrivilegeCheck = utils.DynamicImporter<prototypes::fpNtPrivilegeCheck>("NtPrivilegeCheck");

        if (!NtPrivilegeCheck)
            return fail;

        utils.~ImportUtils();

        LUID luid;

        if (!LookupPrivilegeValueW(nullptr, privilegeType, &luid))
            return fail;

        PRIVILEGE_SET requiredPrivileges{};
        requiredPrivileges.PrivilegeCount = 1;
        requiredPrivileges.Control = PRIVILEGE_SET_ALL_NECESSARY;

        LUID_AND_ATTRIBUTES privilegeArray[1];
        privilegeArray[0].Luid = luid;
        privilegeArray[0].Attributes = SE_PRIVILEGE_ENABLED;

        requiredPrivileges.Privilege[0] = privilegeArray[0];

        BOOLEAN hasPrivilege;
        status = NtPrivilegeCheck(hToken, &requiredPrivileges, &hasPrivilege);

        return (NT_SUCCESS(status) && hasPrivilege) ? 1 : 0;
    }


    inline ModuleInfo MainModuleInfoEx(HANDLE hProcess)
    {
        ModuleInfo mainModuleInfo{};

        if (!hProcess)
            return mainModuleInfo;

        HMODULE modules[1024];
        DWORD bytesNeeded;

        if (K32EnumProcessModules(hProcess, modules, sizeof(modules), &bytesNeeded))
        {
            TCHAR moduleName[MAX_PATH];

            if (GetModuleFileNameEx(hProcess, modules[0], moduleName, MAX_PATH))
            {
                if (_tcsstr(moduleName, TEXT(".exe")))
                {
                    MODULEINFO moduleInfo{};
                    if (GetModuleInformation(hProcess, modules[0], &moduleInfo, sizeof(moduleInfo)))
                    {
                        mainModuleInfo.baseAddress = moduleInfo.lpBaseOfDll;
                        mainModuleInfo.size = moduleInfo.SizeOfImage;
                    }
                }
            }
        }

        return mainModuleInfo;
    }




    inline int ThreadStartedSuspended(HANDLE hThread)
    {

        ImportUtils utils(GetModuleHandleW(L"ntdll.dll"));
        auto NtQueryInformationThread = utils.DynamicImporter<prototypes::fpNtQueryInformationThread>("NtQueryInformationThread");

        if (!NtQueryInformationThread)
            return -1;
       

        THREAD_BASIC_INFORMATION tbi{};
        NTSTATUS status = NtQueryInformationThread(hThread, (THREADINFOCLASS)0x00, &tbi, sizeof(tbi), nullptr);

        if (!NT_SUCCESS(status))
            return -1;

        return (tbi.CreateFlags & 0x00000001) ? 1 : 0;
    }


    int GetMainThreadState(DWORD pid)
    {

        HANDLE hMainThread = NULL;
        FILETIME earliestCreationTime{};
        int result = 0;

        HANDLE hThreadSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, pid);

        if (hThreadSnapshot != INVALID_HANDLE_VALUE)
        {
            THREADENTRY32 te32{};
            te32.dwSize = sizeof(THREADENTRY32);

            if (Thread32First(hThreadSnapshot, &te32))
            {
                do
                {
                    if (te32.th32OwnerProcessID == pid)
                    {
                        HANDLE hThread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te32.th32ThreadID);

                        if (hThread)
                        {
                            FILETIME creationTime, exitTime, kernelTime, userTime;

                            if (GetThreadTimes(hThread, &creationTime, &exitTime, &kernelTime, &userTime))
                            {
                                LONG comparisonResult = CompareFileTime(&creationTime, &earliestCreationTime);

                                if (hMainThread == NULL || comparisonResult == -1)
                                {
                                    earliestCreationTime = creationTime;

                                    if (hMainThread)
                                        CloseHandle(hMainThread);

                                    hMainThread = hThread;
                                }
                                else
                                    CloseHandle(hThread);
                            }
                        }
                    }
                } while (Thread32Next(hThreadSnapshot, &te32));
            }
            CloseHandle(hThreadSnapshot);
        }

        if (hMainThread)
        {
            result = ThreadStartedSuspended(hMainThread);
            CloseHandle(hMainThread);
        }

        return result;
    }


    int GetHiddenThreadCount(DWORD pid)
    {
        
        ImportUtils utils(GetModuleHandleW(L"ntdll.dll"));
        auto NtQueryInformationThread = utils.DynamicImporter<prototypes::fpNtQueryInformationThread>("NtQueryInformationThread");

        THREADENTRY32 te32{};
        int hiddenThreadCount = 0;

        HANDLE hThreadSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, pid);

        if (Thread32First(hThreadSnapshot, &te32))
        {
            do
            {
                ULONG threadHidden = 0;
                HANDLE hThread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te32.th32ThreadID);

                if (hThread)
                {
                    NTSTATUS status = NtQueryInformationThread(hThread, (THREADINFOCLASS)17, &threadHidden, sizeof(threadHidden), nullptr);

                    if (NT_SUCCESS(status) && threadHidden)                   
                        hiddenThreadCount++;  

                    CloseHandle(hThread);
                }
            } while (Thread32Next(hThreadSnapshot, &te32));
        }

        CloseHandle(hThreadSnapshot);
        return hiddenThreadCount;
    }


    int GetIoCounts(HANDLE hProcess)
    {

        ImportUtils utils(GetModuleHandleW(L"ntdll.dll"));
        auto NtQueryInformationProcess = utils.DynamicImporter<prototypes::fpNtQueryInformationProcess>("NtQueryInformationProcess");

        IO_COUNTERS ioCounters{};
        NTSTATUS status = NtQueryInformationProcess(hProcess, (PROCESSINFOCLASS)2, &ioCounters, sizeof(ioCounters), nullptr);

        return (NT_SUCCESS(status)) ? ioCounters.WriteTransferCount / 1024 / 1024 : -1;
    }


    ProcessGenericInfo ProcessInfoQueryGeneric(const wchar_t* section, HANDLE hProcess)
    {

        ProcessGenericInfo sectionInfo{};
        sectionInfo.sectionFound = false;

        if (!hProcess)
            return sectionInfo;


        ModuleInfo moduleInfo = MainModuleInfoEx(hProcess);
        sectionInfo.mainModuleAddress = moduleInfo.baseAddress;
        sectionInfo.mainModuleSize = moduleInfo.size;

        ImportUtils utils(GetModuleHandleW(L"ntdll.dll"));
        auto NtQueryVirtualMemory = utils.DynamicImporter<prototypes::fpNtQueryVirtualMemory>("NtQueryVirtualMemory");

        MEMORY_BASIC_INFORMATION mbi{};
        SIZE_T returnLength;

        NTSTATUS status = NtQueryVirtualMemory(hProcess, moduleInfo.baseAddress, MemoryBasicInformation, &mbi, sizeof(mbi), &returnLength);

        if (NT_SUCCESS(status))
        {
            if (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))
            {
                PIMAGE_DOS_HEADER DosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(sectionInfo.mainModuleAddress);

                if (DosHeader != nullptr && DosHeader->e_magic == IMAGE_DOS_SIGNATURE)
                {
                    PIMAGE_NT_HEADERS NtHeaders = reinterpret_cast<PIMAGE_NT_HEADERS>(reinterpret_cast<BYTE*>(sectionInfo.mainModuleAddress) + DosHeader->e_lfanew);
                    sectionInfo.sectionFound = true;
                }               
            }          
        }

        return sectionInfo;
    }


}