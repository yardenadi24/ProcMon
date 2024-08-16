#pragma once

enum class EventType {
	ProcessCreate,
	ProcessExit,
	ThreadCreate,
	ThreadExit,
	ProcessObject,
	ThreadObject
};

enum class OpType
{
	OpHandleCreate,
	OpHandleDuplicate
};

struct EventHeader
{
	EventType type;
	ULONG Size;
	ULONG64 TimesTamp;
};

struct ProcessCreateInfo
{
	ULONG ProcessId;
	ULONG ParentProcessId;
	ULONG CreatingProcessId;
	ULONG CommandLineLength;
	WCHAR CommandLine[1];
};

struct ProcessExitInfo {
	ULONG ProcessId;
	ULONG ExitCode;
};

struct ThreadCreateInfo
{
	ULONG ThreadId;
	ULONG ProcessId;
};

struct ThreadExitInfo {
	ULONG ThreadId;
	ULONG ProcessId;
	ULONG ExitCode;
};


struct ObjectNotifyInfo {
	OpType OP;
	ULONG Id;
};

struct EventData {
	EventHeader Header;
	union {
		ProcessCreateInfo ProcessCreate;
		ProcessExitInfo ProcessExit;
		ThreadCreateInfo ThreadCreate;
		ThreadExitInfo ThreadExit;
		ObjectNotifyInfo ObjectNotify;
	};
};