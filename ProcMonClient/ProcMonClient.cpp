// ProcMonClient.cpp : This file contains the 'main' function. Program execution begins and ends there.
//
#include <Windows.h>
#include <iostream>
#include <string>
#include "../ProcMonDriver/ProcMonCommon.h"

void DisplayTime(ULONG64 time_stamp)
{
    auto file_time = *(FILETIME*)&time_stamp;
    // Convert to local time
    FileTimeToLocalFileTime(&file_time, &file_time);
    SYSTEMTIME sys_time;
    // Convert to system time
    FileTimeToSystemTime(&file_time, &sys_time);
    printf("%02d:%02d:%02d.%03d  ", sys_time.wHour, sys_time.wMinute, sys_time.wSecond, sys_time.wMilliseconds);
}

void DisplayData(const BYTE* buffer, DWORD size)
{

    while(size>0)
    {
        auto data = (EventData*)buffer;
        auto& header = data->Header;
        DisplayTime(header.TimesTamp);
        switch (header.type)
        {
        case EventType::ProcessCreate:
        {
            auto& p_create = data->ProcessCreate;
            printf("Event: ProcessCreation, PID: %u, PPID: %u, CPID: %u,  CmdLine: %ws\n",
                p_create.ProcessId,
                p_create.ParentProcessId,
                p_create.CreatingProcessId,
                std::wstring(p_create.CommandLine, p_create.CommandLineLength).c_str());
            break;
        }
        case EventType::ProcessExit:
        {
            auto& p_exit = data->ProcessExit;
            printf("Event: ProcessExit, PID: %u, Exit code: %u\n", p_exit.ProcessId, p_exit.ExitCode);
            break;
        }
        case EventType::ThreadCreate:
        {
            auto& t_create = data->ThreadCreate;
            printf("Event: ThreadCreation, TID: %u, PID: %u\n",
                t_create.ThreadId,
                t_create.ProcessId);
            break;
        }
        case EventType::ThreadExit:
        {
            auto& t_exit = data->ThreadExit;
            printf("Event: ThreadExit, TID: %u, PID: %u, Exit code: %u\n",
                t_exit.ThreadId,
                t_exit.ProcessId,
                t_exit.ExitCode);
            break;
        }
        case EventType::ProcessObject:
        {
            auto& ObProc = data->ObjectNotify;
            auto OpStr = ObProc.OP == OpType::OpHandleCreate ? "Handle Create" : "Handle Duplicate";
            printf("Event: ProcessObject, OP: %s, PID: %u\n",
                OpStr,
                ObProc.Id);
            break;
        }
        case EventType::ThreadObject:
        {
            auto& ObThread = data->ObjectNotify;
            auto OpStr = ObThread.OP == OpType::OpHandleCreate ? "Handle Create" : "Handle Duplicate";
            printf("Event: ThreadObject, OP: %s, TID: %u\n",
                OpStr,
                ObThread.Id);
            break;
        }
        }
        buffer += data->Header.Size;
        size -= data->Header.Size;
    }
}

int main()
{
    HANDLE device_handle = CreateFileW(L"\\\\.\\ProcMonDevice", GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (device_handle == INVALID_HANDLE_VALUE)
    {
        printf("Could not create ProcMon device.(%u)\n", GetLastError());
        return 1;
    }

    for (;;)
    {
        BYTE buffer[1<<12]; // 64 KB
        DWORD bytes_red;
        auto status = ReadFile(device_handle, buffer, sizeof(buffer), &bytes_red, nullptr);
        if (!status)
        {
            printf("Failed reading from procmon device. (%u)", GetLastError());
        }
        else {
            DisplayData(buffer, bytes_red);
        }
        Sleep(1000);
    }


    CloseHandle(device_handle);
    return 0;
}
