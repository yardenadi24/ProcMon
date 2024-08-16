#include "pch.h"
#include "ProcMon.h"
#include "Locker.h"


ProcMonState g_ProcMonState;
ULONG g_MaxProcessCount = 100;
FastMutex g_Lock;

PVOID pCBRegistrationHandle = NULL;
bool gUnloading = FALSE;
void UnloadProcMon(PDRIVER_OBJECT);
NTSTATUS DeviceCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS ProcMonRead(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS CompleteIoRequest(PIRP Irp, NTSTATUS status = STATUS_SUCCESS, ULONG_PTR written = 0);
VOID OnProcessCallback(PEPROCESS Process, HANDLE ProcessId, PPS_CREATE_NOTIFY_INFO CreateInfo);
VOID OnThreadCallback(HANDLE ProcessId, HANDLE ThreadId, BOOLEAN Create);
void AddData(FullEventData*);
OB_PREOP_CALLBACK_STATUS OnPreOpenOp(PVOID RegistrationContext,POB_PRE_OPERATION_INFORMATION OperationInformation);
extern "C"
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
	UNREFERENCED_PARAMETER(RegistryPath);
	KdPrint((DRIVER_PREFIX"DriverObject (0x%p)\n", DriverObject));
	
	UNICODE_STRING DeviceName = RTL_CONSTANT_STRING(L"\\Device\\ProcMonDevice");
	UNICODE_STRING SymlinkName = RTL_CONSTANT_STRING(L"\\??\\ProcMonDevice");
	NTSTATUS status;
	PDEVICE_OBJECT device_object = nullptr;
	bool symlink_created = false;
	bool ps_creation_callback_created = false;
	bool td_creation_callback_created = false;
	bool ob_op_callback_created = false;
	//bool reg_op_callback_created = false;
	do {
		// Create the device
		status = IoCreateDevice(DriverObject, 0, &DeviceName, FILE_DEVICE_UNKNOWN, 0, TRUE, &device_object);
		if (!NT_SUCCESS(status))
		{
			KdPrint((DRIVER_PREFIX"Failed creating device\n"));
			break;
		}
		device_object->Flags |= DO_DIRECT_IO;
		// Create symlink
		status = IoCreateSymbolicLink(&SymlinkName, &DeviceName);
		if (!NT_SUCCESS(status))
		{
			KdPrint((DRIVER_PREFIX"Failed creating symlink, deleting device \n"));
			break;
		}
		symlink_created = true;

		// Process creation
		status = PsSetCreateProcessNotifyRoutineEx2(PsCreateProcessNotifySubsystems,OnProcessCallback, FALSE);
		if (!NT_SUCCESS(status))
		{
			KdPrint((DRIVER_PREFIX"Failed to set process creation callback \n"));
		}
		else {
			ps_creation_callback_created = true;
		}

		// Threads creation
		status = PsSetCreateThreadNotifyRoutine(OnThreadCallback);
		if (!NT_SUCCESS(status))
		{
			KdPrint((DRIVER_PREFIX"Failed to set thread creation callback \n"));
		}
		else {
			td_creation_callback_created = true;
		}
		OB_CALLBACK_REGISTRATION ObCbReg;
		OB_OPERATION_REGISTRATION ObOpReg[2];

		// Object
		ObOpReg[0].ObjectType = PsProcessType;
		ObOpReg[0].Operations = OB_OPERATION_HANDLE_CREATE | OB_OPERATION_HANDLE_DUPLICATE;
		ObOpReg[0].PreOperation = OnPreOpenOp;
		ObOpReg[0].PostOperation = nullptr;

		ObOpReg[1].ObjectType = PsThreadType;
		ObOpReg[1].Operations = OB_OPERATION_HANDLE_CREATE | OB_OPERATION_HANDLE_DUPLICATE;
		ObOpReg[1].PreOperation = OnPreOpenOp;
		ObOpReg[1].PostOperation = nullptr;

		ObCbReg.Altitude = RTL_CONSTANT_STRING(L"300");
		ObCbReg.OperationRegistration = ObOpReg;
		ObCbReg.Version = OB_FLT_REGISTRATION_VERSION;
		ObCbReg.OperationRegistrationCount = 2;

		status = ObRegisterCallbacks(&ObCbReg, &pCBRegistrationHandle);
		if (!NT_SUCCESS(status))
		{
			KdPrint((DRIVER_PREFIX"Failed to set object operation callback \n"));
		}
		else {
			ob_op_callback_created = true;
		}

		// Registry


	} while (false);

	if (!NT_SUCCESS(status))
	{
		KdPrint((DRIVER_PREFIX"Error in DriverEntry function (0x%X)\n",status));
		
		if (device_object) {
			IoDeleteDevice(device_object);
		}
		
		if(symlink_created) {

			UNICODE_STRING symlink_name = RTL_CONSTANT_STRING(L"\\??\\ProcMonDevice");
			IoDeleteSymbolicLink(&symlink_name);
		}
		
		if (ps_creation_callback_created)
		{

			PsSetCreateProcessNotifyRoutineEx2(PsCreateProcessNotifySubsystems, OnProcessCallback, TRUE);
		}
		
		if (td_creation_callback_created)
		{
			PsRemoveCreateThreadNotifyRoutine(OnThreadCallback);
		}

		if (ob_op_callback_created)
		{
			if (pCBRegistrationHandle != NULL)
			{
				ObUnRegisterCallbacks(pCBRegistrationHandle);
			}
		}

		return status;
	}


	g_ProcMonState.Lock.Init();
	InitializeListHead(&g_ProcMonState.Head);
	DriverObject->DriverUnload = UnloadProcMon;
	DriverObject->MajorFunction[IRP_MJ_CREATE] = DeviceCreateClose;
	DriverObject->MajorFunction[IRP_MJ_CLOSE] = DeviceCreateClose;
	DriverObject->MajorFunction[IRP_MJ_READ] = ProcMonRead;
	


	return status;
}

void UnloadProcMon(PDRIVER_OBJECT driver_object){
	// Remove callback
	PsSetCreateProcessNotifyRoutineEx2(PsCreateProcessNotifySubsystems,OnProcessCallback, TRUE);
	PsRemoveCreateThreadNotifyRoutine(OnThreadCallback);
	if (pCBRegistrationHandle != NULL)
	{
		ObUnRegisterCallbacks(pCBRegistrationHandle);
	}
	UNICODE_STRING SymlinkName = RTL_CONSTANT_STRING(L"\\??\\ProcMonDevice");
	IoDeleteSymbolicLink(&SymlinkName);
	IoDeleteDevice(driver_object->DeviceObject);
	Locker<FastMutex> lk(g_ProcMonState.Lock);
	while (!IsListEmpty(&g_ProcMonState.Head)) {
		auto link = RemoveHeadList(&g_ProcMonState.Head);
		ExFreePool(CONTAINING_RECORD(link, FullEventData, Link));
	}
}

VOID OnThreadCallback(HANDLE ProcessId, HANDLE ThreadId, BOOLEAN Create)
{
	if (gUnloading)
		return;
	auto size = sizeof(LIST_ENTRY) + sizeof(EventHeader);
	NTSTATUS status = STATUS_SUCCESS;
	if (Create)
	{
		size += sizeof(ThreadCreateInfo);
		KdPrint((DRIVER_PREFIX" Thread created TID: %u (PID: %u)\n", HandleToULong(ThreadId), HandleToULong(ProcessId)));
		auto thread_create_data = (FullEventData*)ExAllocatePool2(
			POOL_FLAG_PAGED | POOL_FLAG_UNINITIALIZED,
			size,
			DRIVER_TAG);
		if (thread_create_data == nullptr)
		{
			KdPrint((DRIVER_PREFIX"Out of memory\n"));
			return;
		}
		auto& header = thread_create_data->Data.Header;
		KeQuerySystemTimePrecise((PLARGE_INTEGER)&header.TimesTamp);
		header.Size = sizeof(EventData) + sizeof(EventHeader) + sizeof(ThreadCreateInfo);
		header.type = EventType::ThreadCreate;

		auto& data = thread_create_data->Data.ThreadCreate;
		data.ThreadId = HandleToULong(ThreadId);
		data.ProcessId = HandleToULong(ProcessId);
		
		AddData(thread_create_data);
	}
	else {
		// Thread exit
		size += sizeof(ThreadExitInfo);
		KdPrint((DRIVER_PREFIX" Thread exit TID: %u (PID: %u)\n", HandleToULong(ThreadId), HandleToULong(ProcessId)));
		auto thread_exit_data = (FullEventData*)ExAllocatePool2(POOL_FLAG_PAGED, size, DRIVER_TAG);
		if (thread_exit_data == nullptr)
		{
			KdPrint((DRIVER_PREFIX"Out of memory\n"));
			return;
		}
		auto& header = thread_exit_data->Data.Header;
		KeQuerySystemTimePrecise((PLARGE_INTEGER)&header.TimesTamp);
		header.Size = sizeof(EventData) + sizeof(EventHeader) + sizeof(ThreadExitInfo);
		header.type = EventType::ThreadExit;

		auto& data = thread_exit_data->Data.ThreadExit;
		data.ThreadId = HandleToULong(ThreadId);
		data.ProcessId = HandleToULong(ProcessId);
		PETHREAD p_thread;
		status = PsLookupThreadByThreadId(ThreadId, &p_thread);
		if (!NT_SUCCESS(status))
		{
			KdPrint((DRIVER_PREFIX" Could not find Thread  TID: %u (PID: %u)\n", HandleToULong(ThreadId), HandleToULong(ProcessId)));
			data.ExitCode = 0;
		}
		else {
			data.ExitCode = PsGetThreadExitStatus(p_thread);
			ObDereferenceObject(p_thread);
		}
		AddData(thread_exit_data);
	}

	return VOID();
}

VOID OnProcessCallback(PEPROCESS Process, HANDLE ProcessId, PPS_CREATE_NOTIFY_INFO CreateInfo)
{
	if (gUnloading)
		return;
	if (CreateInfo)
	{
		KdPrint((DRIVER_PREFIX" Process created id: %u\n",HandleToULong(ProcessId)));
		auto cmd_length = 0;
		if (CreateInfo->CommandLine)
		{
			cmd_length = CreateInfo->CommandLine->Length;
		}
		auto size = sizeof(LIST_ENTRY) + sizeof(EventHeader) + sizeof(ProcessCreateInfo) + cmd_length;
		FullEventData* proc_creation_event = (FullEventData*)ExAllocatePool2(
			POOL_FLAG_PAGED,
			size,
			DRIVER_TAG);
		// Set header values
		auto& header = proc_creation_event->Data.Header;
		KeQuerySystemTimePrecise((PLARGE_INTEGER)&header.TimesTamp);
		header.Size = sizeof(EventHeader) + sizeof(ProcessCreateInfo) + cmd_length;
		header.type = EventType::ProcessCreate;
		// Set event values
		auto& evnt = proc_creation_event->Data;
		evnt.ProcessCreate.ProcessId = HandleToULong(ProcessId);
		evnt.ProcessCreate.ParentProcessId = HandleToULong(CreateInfo->ParentProcessId);
		evnt.ProcessCreate.CreatingProcessId = HandleToULong(CreateInfo->CreatingThreadId.UniqueProcess);
		evnt.ProcessCreate.CommandLineLength = cmd_length/sizeof(WCHAR); /*length in char and not bytes (wchar = 2 bytes)*/
		if (CreateInfo->CommandLine)
		{
			memcpy(&evnt.ProcessCreate.CommandLine, CreateInfo->CommandLine->Buffer, cmd_length);
		}
		AddData(proc_creation_event);
	}
	else {
		// Exit case
		KdPrint((DRIVER_PREFIX" Process exit id: %u\n", HandleToULong(ProcessId)));
		auto size = sizeof(LIST_ENTRY) + sizeof(EventHeader) + sizeof(ProcessExitInfo);
		FullEventData* proc_exit_event = (FullEventData*)ExAllocatePool2(
			POOL_FLAG_PAGED | POOL_FLAG_UNINITIALIZED,
			size,
			DRIVER_TAG);
		if (proc_exit_event == nullptr)
		{
			KdPrint((DRIVER_PREFIX"Failed creating process exit event for pid: %u\n", HandleToULong(ProcessId)));
			return;
		}

		// Set header values
		auto& header = proc_exit_event->Data.Header;
		KeQuerySystemTimePrecise((PLARGE_INTEGER)&header.TimesTamp);
		header.Size = sizeof(ProcessExitInfo) + sizeof(EventHeader);
		header.type = EventType::ProcessExit;
		// Set event values
		auto& evnt = proc_exit_event->Data.ProcessExit;
		evnt.ProcessId = HandleToULong(ProcessId);
		evnt.ExitCode = PsGetProcessExitStatus(Process);

		AddData(proc_exit_event);
	}

	return VOID();
}

OB_PREOP_CALLBACK_STATUS OnPreOpenOp(PVOID RegistrationContext, POB_PRE_OPERATION_INFORMATION PreOperationInformation)
{
	UNREFERENCED_PARAMETER(RegistrationContext);
	if (gUnloading)
		return OB_PREOP_SUCCESS;

	auto size = sizeof(LIST_ENTRY) + sizeof(EventHeader) + sizeof(ObjectNotifyInfo);
	FullEventData* ob_notify_event = (FullEventData*)ExAllocatePool2(
		POOL_FLAG_PAGED,
		size,
		DRIVER_TAG);

	auto& header = ob_notify_event->Data.Header;
	auto& data = ob_notify_event->Data.ObjectNotify;

	header.Size = sizeof(ObjectNotifyInfo) + sizeof(EventHeader);
	KeQuerySystemTimePrecise((PLARGE_INTEGER)&header.TimesTamp);
	data.OP = PreOperationInformation->Operation == OB_OPERATION_HANDLE_CREATE ? OpType::OpHandleCreate : OpType::OpHandleDuplicate;

	if (PreOperationInformation->ObjectType == *PsProcessType)
	{
		KdPrint((DRIVER_PREFIX" Process pre op\n"));
		header.type = EventType::ProcessObject;
		data.Id = HandleToUlong(PsGetProcessId((PEPROCESS)PreOperationInformation->Object));
	}else if(PreOperationInformation->ObjectType == *PsThreadType){
		KdPrint((DRIVER_PREFIX" Thread pre op\n"));
		header.type = EventType::ThreadObject;
		data.Id = HandleToUlong(PsGetThreadId((PETHREAD)PreOperationInformation->Object));
	}

	AddData(ob_notify_event);

	return OB_PREOP_SUCCESS;
}

NTSTATUS CompleteIoRequest(PIRP Irp, NTSTATUS status, ULONG_PTR written)
{
	Irp->IoStatus.Status = status;
	Irp->IoStatus.Information = written;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return status;
}

NTSTATUS DeviceCreateClose(PDEVICE_OBJECT , PIRP Irp)
{
	return CompleteIoRequest(Irp);
}

void AddData(FullEventData* Data)
{
	Locker<FastMutex> lk(g_ProcMonState.Lock);
	if (g_ProcMonState.Count < g_MaxProcessCount)
	{
		KdPrint((DRIVER_PREFIX" Inserting data to list. (%u/%u)\n",g_ProcMonState.Count,g_MaxProcessCount));
		InsertTailList(&g_ProcMonState.Head, &Data->Link);
		g_ProcMonState.Count++;
	}
	else {
		KdPrint((DRIVER_PREFIX" Inserting data to list failed, reached max count (%u/%u)\n", g_ProcMonState.Count, g_MaxProcessCount));
	}
}


NTSTATUS ProcMonRead(PDEVICE_OBJECT, PIRP Irp)
{
	NTSTATUS status = STATUS_SUCCESS;
	auto IrpS = IoGetCurrentIrpStackLocation(Irp);
	// Direct IO so we need the MDl
	auto buffer =(PUCHAR)MmGetSystemAddressForMdlSafe(Irp->MdlAddress,NormalPagePriority);
	if (buffer == nullptr)
	{
		KdPrint((DRIVER_PREFIX" Buffer is nullptr\n"));
		return CompleteIoRequest(Irp, STATUS_INSUFFICIENT_RESOURCES);
	}
	auto read_params = IrpS->Parameters.Read;
	auto len_left = read_params.Length;
	ULONG info = 0;
	if (len_left < sizeof(FullEventData))
	{
		KdPrint((DRIVER_PREFIX" Insufficient length for read operation from user. (%u/%u)\n", len_left, sizeof(EventData)));
		status = STATUS_BUFFER_TOO_SMALL;
		return CompleteIoRequest(Irp,status);
	}
	Locker<FastMutex> lk(g_ProcMonState.Lock);
	while (!IsListEmpty(&g_ProcMonState.Head))
	{	
		// Get next link
		auto proc_link = g_ProcMonState.Head.Flink;
		auto data = CONTAINING_RECORD(proc_link, FullEventData, Link);
		auto size = data->Data.Header.Size;
		if (size > len_left)
		{
			break;
		}
		// We managed to pop one event.
		memcpy(buffer, &data->Data, size);
		buffer += size;
		len_left -= size;
		info += size;
		proc_link = RemoveHeadList(&g_ProcMonState.Head);
		ExFreePool(CONTAINING_RECORD(proc_link, FullEventData, Link));
		g_ProcMonState.Count--;
	}

	return CompleteIoRequest(Irp, STATUS_SUCCESS, info);

}
