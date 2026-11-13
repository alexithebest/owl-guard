// ============================================================================
//  OPEN SORCES FREE JOIN https://discord.gg/MMtdav2EJy FOR MORE!
// ============================================================================
// antigrab.c - Owl Guard ring-0 shield
//
// ObRegisterCallbacks no tipo Process: qualquer handle aberto contra Discord
// (ou contra um PID explicitamente protegido) perde
// PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD.
// e o bloqueio real. sem isso, o userland so consegue detectar e matar.

#include <ntifs.h>
#include "../src/shared.h"

// exportado pelo ntoskrnl, mas ausente do header do WDK 28000
NTSYSAPI PCHAR NTAPI PsGetProcessImageFileName(_In_ PEPROCESS Process);

#define VANTA_POOL_TAG        'GnaV'
#define MAX_PROTECTED_PIDS    64
#define VANTA_ALTITUDE        L"321000"

static ULONG          g_pids[MAX_PROTECTED_PIDS];
static volatile LONG  g_pid_count = 0;
static KSPIN_LOCK     g_lock;
static PVOID          g_ob_handle = NULL;
static BOOLEAN        g_callbacks = FALSE;
static PDEVICE_OBJECT g_device = NULL;
static BOOLEAN        g_symlink = FALSE;

// ---------------------------------------------------------------- helpers

static BOOLEAN ascii_ieq(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return FALSE;
    }
    return *a == *b;
}

static BOOLEAN is_discord(PEPROCESS process) {
    PCHAR name = PsGetProcessImageFileName(process);
    if (!name) return FALSE;
    return ascii_ieq(name, "Discord.exe") ||
           ascii_ieq(name, "DiscordCanary.exe") ||
           ascii_ieq(name, "DiscordPTB.exe");
}

static VOID add_pid(ULONG pid) {
    KIRQL irql;
    LONG i;
    if (pid == 0 || pid == 4) return;
    KeAcquireSpinLock(&g_lock, &irql);
    for (i = 0; i < g_pid_count; ++i) {
        if (g_pids[i] == pid) {
            KeReleaseSpinLock(&g_lock, irql);
            return;
        }
    }
    if (g_pid_count < MAX_PROTECTED_PIDS)
        g_pids[g_pid_count++] = pid;
    KeReleaseSpinLock(&g_lock, irql);
}

static VOID remove_pid(ULONG pid) {
    KIRQL irql;
    LONG i;
    KeAcquireSpinLock(&g_lock, &irql);
    for (i = 0; i < g_pid_count; ++i) {
        if (g_pids[i] == pid) {
            g_pids[i] = g_pids[g_pid_count - 1];
            g_pid_count--;
            break;
        }
    }
    KeReleaseSpinLock(&g_lock, irql);
}

static BOOLEAN is_protected(ULONG pid) {
    KIRQL irql;
    BOOLEAN found = FALSE;
    LONG i;
    KeAcquireSpinLock(&g_lock, &irql);
    for (i = 0; i < g_pid_count; ++i) {
        if (g_pids[i] == pid) { found = TRUE; break; }
    }
    KeReleaseSpinLock(&g_lock, irql);
    return found;
}

// ---------------------------------------------------------------- Ob callback

static OB_PREOP_CALLBACK_STATUS vanta_pre_op(PVOID context,
                                             POB_PRE_OPERATION_INFORMATION info) {
    PEPROCESS target;
    ULONG pid;
    ACCESS_MASK deny = VANTA_DENY_MASK;

    UNREFERENCED_PARAMETER(context);

    if (!info || !info->Object || !info->Parameters) return OB_PREOP_SUCCESS;
    if (info->ObjectType != *PsProcessType) return OB_PREOP_SUCCESS;

    // handles abertos pelo proprio kernel (AV/EDR) passam - drop esta linha
    // se quiser bloquear tambem os scanners ring-0.
    if (info->KernelHandle) return OB_PREOP_SUCCESS;

    target = (PEPROCESS)info->Object;
    pid = (ULONG)(ULONG_PTR)PsGetProcessId(target);

    if (!is_protected(pid) && !is_discord(target)) return OB_PREOP_SUCCESS;

    // nunca bloquear o proprio alvo contra si mesmo
    if (pid == (ULONG)(ULONG_PTR)PsGetCurrentProcessId()) return OB_PREOP_SUCCESS;

    if (info->Operation == OB_OPERATION_HANDLE_CREATE) {
        info->Parameters->CreateHandleInformation.DesiredAccess &= ~deny;
        info->Parameters->CreateHandleInformation.OriginalDesiredAccess &= ~deny;
    } else if (info->Operation == OB_OPERATION_HANDLE_DUPLICATE) {
        info->Parameters->DuplicateHandleInformation.DesiredAccess &= ~deny;
    }

    return OB_PREOP_SUCCESS;
}

static NTSTATUS register_callbacks(void) {
    OB_OPERATION_REGISTRATION operation = {0};
    OB_CALLBACK_REGISTRATION registration = {0};
    UNICODE_STRING altitude;

    operation.ObjectType = PsProcessType;
    operation.Operations = OB_OPERATION_HANDLE_CREATE | OB_OPERATION_HANDLE_DUPLICATE;
    operation.PreOperation = vanta_pre_op;

    RtlInitUnicodeString(&altitude, VANTA_ALTITUDE);

    registration.Version = OB_FLT_REGISTRATION_VERSION;
    registration.OperationRegistrationCount = 1;
    registration.Altitude = altitude;
    registration.OperationRegistration = &operation;

    return ObRegisterCallbacks(&registration, &g_ob_handle);
}

// ---------------------------------------------------------------- process notify

static VOID vanta_process_notify(HANDLE parent_id, HANDLE process_id, BOOLEAN create) {
    UNREFERENCED_PARAMETER(parent_id);

    if (create) {
        PEPROCESS process = NULL;
        if (NT_SUCCESS(PsLookupProcessByProcessId(process_id, &process))) {
            if (is_discord(process))
                add_pid((ULONG)(ULONG_PTR)process_id);
            ObDereferenceObject(process);
        }
    } else {
        remove_pid((ULONG)(ULONG_PTR)process_id);
    }
}

// ---------------------------------------------------------------- IOCTL

static NTSTATUS dispatch_pass(PDEVICE_OBJECT device, PIRP irp) {
    UNREFERENCED_PARAMETER(device);
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS dispatch_control(PDEVICE_OBJECT device, PIRP irp) {
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    PVOID buffer = irp->AssociatedIrp.SystemBuffer;
    ULONG in_len = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG out_len = stack->Parameters.DeviceIoControl.OutputBufferLength;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG info = 0;

    UNREFERENCED_PARAMETER(device);

    switch (stack->Parameters.DeviceIoControl.IoControlCode) {
        case IOCTL_VANTA_PROTECT_PID:
        case IOCTL_VANTA_UNPROTECT_PID:
            if (buffer && in_len >= sizeof(ULONG)) {
                ULONG pid = *(ULONG*)buffer;
                if (stack->Parameters.DeviceIoControl.IoControlCode == IOCTL_VANTA_PROTECT_PID)
                    add_pid(pid);
                else
                    remove_pid(pid);
                status = STATUS_SUCCESS;
            } else {
                status = STATUS_INVALID_PARAMETER;
            }
            break;

        case IOCTL_VANTA_QUERY:
            if (buffer && out_len >= sizeof(ULONG)) {
                KIRQL irql;
                KeAcquireSpinLock(&g_lock, &irql);
                *(ULONG*)buffer = (ULONG)g_pid_count;
                KeReleaseSpinLock(&g_lock, irql);
                info = sizeof(ULONG);
                status = STATUS_SUCCESS;
            } else {
                status = STATUS_BUFFER_TOO_SMALL;
            }
            break;

        default:
            break;
    }

    irp->IoStatus.Status = status;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

// ---------------------------------------------------------------- lifecycle

static VOID vanta_unload(PDRIVER_OBJECT driver) {
    UNREFERENCED_PARAMETER(driver);

    if (g_callbacks && g_ob_handle) {
        ObUnRegisterCallbacks(g_ob_handle);
        g_ob_handle = NULL;
        g_callbacks = FALSE;
    }
    PsSetCreateProcessNotifyRoutine(vanta_process_notify, TRUE);
    if (g_symlink) {
        UNICODE_STRING link;
        RtlInitUnicodeString(&link, VANTA_SYMLINK_NAME);
        IoDeleteSymbolicLink(&link);
        g_symlink = FALSE;
    }
    if (g_device) {
        IoDeleteDevice(g_device);
        g_device = NULL;
    }
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry_path) {
    UNICODE_STRING device_name;
    UNICODE_STRING link_name;
    PDEVICE_OBJECT device = NULL;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(registry_path);

    KeInitializeSpinLock(&g_lock);
    RtlZeroMemory(g_pids, sizeof(g_pids));
    g_pid_count = 0;

    status = register_callbacks();
    if (!NT_SUCCESS(status)) return status;
    g_callbacks = TRUE;

    status = PsSetCreateProcessNotifyRoutine(vanta_process_notify, FALSE);
    if (!NT_SUCCESS(status)) {
        ObUnRegisterCallbacks(g_ob_handle);
        g_ob_handle = NULL;
        g_callbacks = FALSE;
        return status;
    }

    RtlInitUnicodeString(&device_name, VANTA_DEVICE_NAME);
    status = IoCreateDevice(driver, 0, &device_name, FILE_DEVICE_UNKNOWN,
                            FILE_DEVICE_SECURE_OPEN, FALSE, &device);
    if (!NT_SUCCESS(status)) {
        PsSetCreateProcessNotifyRoutine(vanta_process_notify, TRUE);
        ObUnRegisterCallbacks(g_ob_handle);
        g_callbacks = FALSE;
        return status;
    }
    g_device = device;

    RtlInitUnicodeString(&link_name, VANTA_SYMLINK_NAME);
    status = IoCreateSymbolicLink(&link_name, &device_name);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(device);
        g_device = NULL;
        PsSetCreateProcessNotifyRoutine(vanta_process_notify, TRUE);
        ObUnRegisterCallbacks(g_ob_handle);
        g_callbacks = FALSE;
        return status;
    }
    g_symlink = TRUE;

    driver->MajorFunction[IRP_MJ_CREATE] = dispatch_pass;
    driver->MajorFunction[IRP_MJ_CLOSE] = dispatch_pass;
    driver->MajorFunction[IRP_MJ_CLEANUP] = dispatch_pass;
    driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = dispatch_control;
    driver->DriverUnload = vanta_unload;

    return STATUS_SUCCESS;
}
