#include <ntddk.h>
#include <wdm.h>
#include <intrin.h>

// ----------------------------------------------------------------
//  Manual declarations
// ----------------------------------------------------------------

NTSYSAPI NTSTATUS NTAPI ZwSetInformationProcess(
    _In_ HANDLE ProcessHandle,
    _In_ PROCESSINFOCLASS ProcessInformationClass,
    _In_ PVOID ProcessInformation,
    _In_ ULONG ProcessInformationLength
);

NTSYSAPI NTSTATUS NTAPI ZwQueryInformationProcess(
    _In_      HANDLE ProcessHandle,
    _In_      PROCESSINFOCLASS ProcessInformationClass,
    _Out_     PVOID ProcessInformation,
    _In_      ULONG ProcessInformationLength,
    _Out_opt_ PULONG ReturnLength
);

NTKERNELAPI NTSTATUS ObOpenObjectByPointer(
    _In_     PVOID Object,
    _In_     ULONG HandleAttributes,
    _In_opt_ PACCESS_STATE PassedAccessState,
    _In_     ACCESS_MASK DesiredAccess,
    _In_opt_ POBJECT_TYPE ObjectType,
    _In_     KPROCESSOR_MODE AccessMode,
    _Out_    PHANDLE Handle
);

NTKERNELAPI NTSTATUS PsLookupProcessByProcessId(
    _In_  HANDLE ProcessId,
    _Out_ PEPROCESS *Process
);

#ifndef PROCESS_SET_INFORMATION
#define PROCESS_SET_INFORMATION (0x0200)
#endif

#ifndef PROCESS_QUERY_INFORMATION
#define PROCESS_QUERY_INFORMATION (0x0400)
#endif

#define DRIVER_TAG        'HCPU'
#define DEVICE_NAME       L"\\Device\\HybridCpuDriver"
#define DOS_DEVICE_NAME   L"\\DosDevices\\HybridCpuDriver"

#define CPUID_HYBRID_LEAF     0x1A
#define CORE_TYPE_PERFORMANCE 0x40
#define CORE_TYPE_EFFICIENCY  0x20

#define IOCTL_GET_HYBRID_INFO \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_DATA)

#define SCAN_INTERVAL_MS              100
#define EPROCESS_ACTIVE_LINKS_OFFSET  0x448
#define EPROCESS_UNIQUEPID_OFFSET     0x440
#define EPROCESS_PRIORITYCLASS_OFFSET 0x5B7

// ----------------------------------------------------------------
//  Structs
// ----------------------------------------------------------------

typedef struct _HYBRID_CPU_INFO {
    ULONG     ProcessorCount;
    BOOLEAN   IsHybrid;
    KAFFINITY PCoreMask;
    KAFFINITY ECoreMask;
} HYBRID_CPU_INFO, *PHYBRID_CPU_INFO;

typedef struct _HYBRID_INFO_OUTPUT {
    ULONG   ProcessorCount;
    BOOLEAN IsHybrid;
    ULONG64 PCoreMask;
    ULONG64 ECoreMask;
} HYBRID_INFO_OUTPUT, *PHYBRID_INFO_OUTPUT;

// ----------------------------------------------------------------
//  Globals
// ----------------------------------------------------------------

HYBRID_CPU_INFO g_HybridInfo       = { 0 };
PDEVICE_OBJECT  g_DeviceObject     = NULL;
BOOLEAN         g_NotifyRegistered = FALSE;

KTIMER          g_ScanTimer;
KDPC            g_ScanDpc;
BOOLEAN         g_TimerRunning     = FALSE;

KEVENT          g_ScanEvent;
PETHREAD        g_WorkerThread     = NULL;
BOOLEAN         g_WorkerRunning    = FALSE;

// ----------------------------------------------------------------
//  Priority class helper
//  1=Idle  2=Normal  3=High  4=Realtime  5=BelowNormal  6=AboveNormal
// ----------------------------------------------------------------

static BOOLEAN
IsHighPriority(UCHAR priorityClass)
{
    return (priorityClass == 3 ||  // High
            priorityClass == 4 ||  // Realtime
            priorityClass == 6);   // Above Normal
}

// ----------------------------------------------------------------
//  CPUID helper
// ----------------------------------------------------------------

static VOID
QueryCpuid(ULONG Leaf, ULONG *Eax, ULONG *Ebx, ULONG *Ecx, ULONG *Edx)
{
    int regs[4] = { 0 };
    __cpuid(regs, (int)Leaf);
    *Eax = (ULONG)regs[0];
    *Ebx = (ULONG)regs[1];
    *Ecx = (ULONG)regs[2];
    *Edx = (ULONG)regs[3];
}

// ----------------------------------------------------------------
//  Per-processor DPC: classify each logical processor
// ----------------------------------------------------------------

static VOID
ClassifyProcessorDpc(PKDPC Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);

    PHYBRID_CPU_INFO info = (PHYBRID_CPU_INFO)Context;
    ULONG eax = 0, ebx = 0, ecx = 0, edx = 0;
    ULONG procIndex = KeGetCurrentProcessorNumber();
    KAFFINITY mask  = (KAFFINITY)1 << procIndex;

    QueryCpuid(CPUID_HYBRID_LEAF, &eax, &ebx, &ecx, &edx);

    ULONG coreType = (eax >> 24) & 0xFF;

    if (coreType == CORE_TYPE_PERFORMANCE) {
        InterlockedOr((LONG volatile *)&info->PCoreMask, (LONG)mask);
        KdPrint(("HybridCPU: Processor %u -> P-Core\n", procIndex));
    } else if (coreType == CORE_TYPE_EFFICIENCY) {
        InterlockedOr((LONG volatile *)&info->ECoreMask, (LONG)mask);
        KdPrint(("HybridCPU: Processor %u -> E-Core\n", procIndex));
    } else {
        InterlockedOr((LONG volatile *)&info->PCoreMask, (LONG)mask);
        KdPrint(("HybridCPU: Processor %u -> Unknown (treated as P-Core)\n", procIndex));
    }
}

// ----------------------------------------------------------------
//  Detect hybrid topology
// ----------------------------------------------------------------

static VOID
DetectHybridTopology(VOID)
{
    ULONG eax, ebx, ecx, edx;

    QueryCpuid(7, &eax, &ebx, &ecx, &edx);
    g_HybridInfo.IsHybrid       = (BOOLEAN)((edx >> 15) & 1);
    g_HybridInfo.ProcessorCount = KeQueryActiveProcessorCount(NULL);

    KdPrint(("HybridCPU: IsHybrid=%d, ProcessorCount=%u\n",
             g_HybridInfo.IsHybrid, g_HybridInfo.ProcessorCount));

    ULONG count = g_HybridInfo.ProcessorCount;

    PKDPC dpcs = (PKDPC)ExAllocatePoolWithTag(
                     NonPagedPool, count * sizeof(KDPC), DRIVER_TAG);
    if (!dpcs) {
        KdPrint(("HybridCPU: Failed to allocate DPC array.\n"));
        return;
    }

    for (ULONG i = 0; i < count; i++) {
        KeInitializeDpc(&dpcs[i], ClassifyProcessorDpc, &g_HybridInfo);
        KeSetTargetProcessorDpc(&dpcs[i], (CCHAR)i);
        KeInsertQueueDpc(&dpcs[i], NULL, NULL);
    }

    LARGE_INTEGER interval;
    interval.QuadPart = -500000LL;
    KeDelayExecutionThread(KernelMode, FALSE, &interval);

    ExFreePoolWithTag(dpcs, DRIVER_TAG);

    KdPrint(("HybridCPU: P-Core mask=0x%IX  E-Core mask=0x%IX\n",
             g_HybridInfo.PCoreMask, g_HybridInfo.ECoreMask));
}

// ----------------------------------------------------------------
//  Apply affinity to a process
// ----------------------------------------------------------------

static VOID
ApplyAffinityToProcess(PEPROCESS Process, KAFFINITY AffinityMask)
{
    HANDLE hProcess = NULL;
    NTSTATUS status;

    status = ObOpenObjectByPointer(
        Process,
        OBJ_KERNEL_HANDLE,
        NULL,
        PROCESS_SET_INFORMATION,
        *PsProcessType,
        KernelMode,
        &hProcess);

    if (!NT_SUCCESS(status)) return;

    status = ZwSetInformationProcess(
        hProcess,
        ProcessAffinityMask,
        &AffinityMask,
        sizeof(KAFFINITY));

    if (!NT_SUCCESS(status)) {
        KdPrint(("HybridCPU: ZwSetInformationProcess failed: 0x%X\n", status));
    }

    ZwClose(hProcess);
}

// ----------------------------------------------------------------
//  Scan all running processes and fix affinity based on priority
// ----------------------------------------------------------------

static VOID
ScanAndUpdateProcesses(VOID)
{
    PUCHAR systemProc = (PUCHAR)PsInitialSystemProcess;
    if (!systemProc) return;

    PLIST_ENTRY listHead = (PLIST_ENTRY)(systemProc + EPROCESS_ACTIVE_LINKS_OFFSET);
    PLIST_ENTRY entry    = listHead->Flink;

    while (entry != listHead) {
        __try {
            PUCHAR procBase     = (PUCHAR)entry - EPROCESS_ACTIVE_LINKS_OFFSET;
            HANDLE pid          = *(PHANDLE)(procBase + EPROCESS_UNIQUEPID_OFFSET);

            if ((ULONG_PTR)pid <= 4) goto next;

            UCHAR priorityClass      = *(procBase + EPROCESS_PRIORITYCLASS_OFFSET);
            KAFFINITY targetAffinity = IsHighPriority(priorityClass)
                ? g_HybridInfo.PCoreMask
                : g_HybridInfo.ECoreMask;

            PEPROCESS process = (PEPROCESS)procBase;
            HANDLE hProcess   = NULL;

            NTSTATUS status = ObOpenObjectByPointer(
                process,
                OBJ_KERNEL_HANDLE,
                NULL,
                PROCESS_QUERY_INFORMATION | PROCESS_SET_INFORMATION,
                *PsProcessType,
                KernelMode,
                &hProcess);

            if (NT_SUCCESS(status)) {
                KAFFINITY currentAffinity = 0;
                ULONG retLen = 0;

                NTSTATUS qStatus = ZwQueryInformationProcess(
                    hProcess,
                    ProcessAffinityMask,
                    &currentAffinity,
                    sizeof(KAFFINITY),
                    &retLen);

                if (NT_SUCCESS(qStatus) && currentAffinity != targetAffinity) {
                    KdPrint(("HybridCPU: Updating PID %Iu priorityClass=%u -> 0x%IX\n",
                             (ULONG_PTR)pid, priorityClass, targetAffinity));

                    ZwSetInformationProcess(
                        hProcess,
                        ProcessAffinityMask,
                        &targetAffinity,
                        sizeof(KAFFINITY));
                }

                ZwClose(hProcess);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { }

    next:
        entry = entry->Flink;
    }
}

// ----------------------------------------------------------------
//  Worker thread
// ----------------------------------------------------------------

static VOID
WorkerThreadProc(PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);

    KdPrint(("HybridCPU: Worker thread started.\n"));

    while (g_WorkerRunning) {
        KeWaitForSingleObject(&g_ScanEvent, Executive, KernelMode, FALSE, NULL);
        if (!g_WorkerRunning) break;
        KeClearEvent(&g_ScanEvent);
        ScanAndUpdateProcesses();
    }

    KdPrint(("HybridCPU: Worker thread exiting.\n"));
    PsTerminateSystemThread(STATUS_SUCCESS);
}

// ----------------------------------------------------------------
//  Timer DPC: fires every 100ms, signals worker thread
// ----------------------------------------------------------------

static VOID
ScanTimerDpc(PKDPC Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);

    KeSetEvent(&g_ScanEvent, IO_NO_INCREMENT, FALSE);
}

// ----------------------------------------------------------------
//  Process notify callback (catches new processes instantly)
// ----------------------------------------------------------------

static VOID
ProcessNotifyCallback(
    PEPROCESS              Process,
    HANDLE                 ProcessId,
    PPS_CREATE_NOTIFY_INFO CreateInfo)
{
    if (CreateInfo == NULL) return;
    if (!g_HybridInfo.IsHybrid) return;
    if (g_HybridInfo.PCoreMask == 0 || g_HybridInfo.ECoreMask == 0) return;

    UCHAR priorityClass = 2;

#pragma warning(push)
#pragma warning(disable: 4214)
    __try {
        PUCHAR procBase = (PUCHAR)Process;
        priorityClass = *(procBase + EPROCESS_PRIORITYCLASS_OFFSET);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        priorityClass = 2;
    }
#pragma warning(pop)

    KAFFINITY mask = IsHighPriority(priorityClass)
        ? g_HybridInfo.PCoreMask
        : g_HybridInfo.ECoreMask;

    KdPrint(("HybridCPU: PID %Iu priorityClass=%u -> %s (0x%IX)\n",
             (ULONG_PTR)ProcessId, priorityClass,
             IsHighPriority(priorityClass) ? "P-Cores" : "E-Cores", mask));

    ApplyAffinityToProcess(Process, mask);
}

// ----------------------------------------------------------------
//  Dispatch: IOCTL
// ----------------------------------------------------------------

static NTSTATUS
DispatchIoControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PIO_STACK_LOCATION stack  = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS           status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR          info   = 0;

    if (stack->Parameters.DeviceIoControl.IoControlCode == IOCTL_GET_HYBRID_INFO)
    {
        if (stack->Parameters.DeviceIoControl.OutputBufferLength
                >= sizeof(HYBRID_INFO_OUTPUT))
        {
            PHYBRID_INFO_OUTPUT out =
                (PHYBRID_INFO_OUTPUT)Irp->AssociatedIrp.SystemBuffer;

            out->ProcessorCount = g_HybridInfo.ProcessorCount;
            out->IsHybrid       = g_HybridInfo.IsHybrid;
            out->PCoreMask      = (ULONG64)g_HybridInfo.PCoreMask;
            out->ECoreMask      = (ULONG64)g_HybridInfo.ECoreMask;

            status = STATUS_SUCCESS;
            info   = sizeof(HYBRID_INFO_OUTPUT);
        }
        else
        {
            status = STATUS_BUFFER_TOO_SMALL;
        }
    }

    Irp->IoStatus.Status      = status;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

// ----------------------------------------------------------------
//  Dispatch: Create / Close
// ----------------------------------------------------------------

static NTSTATUS
DispatchCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status      = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

// ----------------------------------------------------------------
//  Unload
// ----------------------------------------------------------------

VOID DriverUnload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);

    g_WorkerRunning = FALSE;

    if (g_TimerRunning) {
        KeCancelTimer(&g_ScanTimer);
        g_TimerRunning = FALSE;
    }

    KeSetEvent(&g_ScanEvent, IO_NO_INCREMENT, FALSE);

    if (g_WorkerThread) {
        KeWaitForSingleObject(g_WorkerThread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(g_WorkerThread);
        g_WorkerThread = NULL;
    }

    if (g_NotifyRegistered) {
        PsSetCreateProcessNotifyRoutineEx(ProcessNotifyCallback, TRUE);
        g_NotifyRegistered = FALSE;
    }

    UNICODE_STRING dos;
    RtlInitUnicodeString(&dos, DOS_DEVICE_NAME);
    IoDeleteSymbolicLink(&dos);

    if (g_DeviceObject) IoDeleteDevice(g_DeviceObject);

    KdPrint(("HybridCPU: Unloaded.\n"));
}

// ----------------------------------------------------------------
//  DriverEntry
// ----------------------------------------------------------------

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    UNICODE_STRING devName, dosName;
    RtlInitUnicodeString(&devName, DEVICE_NAME);
    RtlInitUnicodeString(&dosName, DOS_DEVICE_NAME);

    NTSTATUS status = IoCreateDevice(
        DriverObject, 0, &devName,
        FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN,
        FALSE, &g_DeviceObject);

    if (!NT_SUCCESS(status)) {
        KdPrint(("HybridCPU: IoCreateDevice failed: 0x%X\n", status));
        return status;
    }

    status = IoCreateSymbolicLink(&dosName, &devName);
    if (!NT_SUCCESS(status)) {
        KdPrint(("HybridCPU: IoCreateSymbolicLink failed: 0x%X\n", status));
        IoDeleteDevice(g_DeviceObject);
        return status;
    }

    DriverObject->DriverUnload                         = DriverUnload;
    DriverObject->MajorFunction[IRP_MJ_CREATE]         = DispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]          = DispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchIoControl;

    g_DeviceObject->Flags |= DO_BUFFERED_IO;
    g_DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    DetectHybridTopology();

    KeInitializeEvent(&g_ScanEvent, SynchronizationEvent, FALSE);

    g_WorkerRunning = TRUE;
    HANDLE hThread  = NULL;
    status = PsCreateSystemThread(
        &hThread, THREAD_ALL_ACCESS,
        NULL, NULL, NULL,
        WorkerThreadProc, NULL);

    if (!NT_SUCCESS(status)) {
        KdPrint(("HybridCPU: PsCreateSystemThread failed: 0x%X\n", status));
        g_WorkerRunning = FALSE;
    } else {
        ObReferenceObjectByHandle(hThread, THREAD_ALL_ACCESS, NULL,
                                  KernelMode, (PVOID *)&g_WorkerThread, NULL);
        ZwClose(hThread);
        KdPrint(("HybridCPU: Worker thread started.\n"));
    }

    KeInitializeTimer(&g_ScanTimer);
    KeInitializeDpc(&g_ScanDpc, ScanTimerDpc, NULL);

    LARGE_INTEGER dueTime;
    dueTime.QuadPart = -1000000LL;
    KeSetTimerEx(&g_ScanTimer, dueTime, SCAN_INTERVAL_MS, &g_ScanDpc);
    g_TimerRunning = TRUE;

    status = PsSetCreateProcessNotifyRoutineEx(ProcessNotifyCallback, FALSE);
    if (!NT_SUCCESS(status)) {
        KdPrint(("HybridCPU: PsSetCreateProcessNotifyRoutineEx failed: 0x%X\n", status));
    } else {
        g_NotifyRegistered = TRUE;
        KdPrint(("HybridCPU: Process notify registered.\n"));
    }

    KdPrint(("HybridCPU: Driver loaded successfully.\n"));
    return STATUS_SUCCESS;
}
