#pragma once
#include "ProcMonCommon.h"
#include "FastMutex.h"

#define DRIVER_TAG 'nmrp' /*Up to 4 characters*/
#define DRIVER_PREFIX "ProcMon: "

struct FullEventData {
	LIST_ENTRY Link;
	EventData Data;
};

struct ProcMonState {
	LIST_ENTRY Head;
	ULONG Count;
	FastMutex Lock;
};