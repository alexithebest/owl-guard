// shared.h - contrato entre o app de usuario e o driver ring-0
#pragma once

#ifdef _KERNEL_MODE
#include <ntddk.h>

// WDK 28000 nao expoe os access masks de processo no namespace kernel
#ifndef PROCESS_CREATE_THREAD
#define PROCESS_CREATE_THREAD 0x0002
#endif
#ifndef PROCESS_VM_OPERATION
#define PROCESS_VM_OPERATION 0x0008
#endif
#ifndef PROCESS_VM_READ
#define PROCESS_VM_READ 0x0010
#endif
#ifndef PROCESS_VM_WRITE
#define PROCESS_VM_WRITE 0x0020
#endif

#else
#include <windows.h>
#include <winioctl.h>
#endif

#define VANTA_DEVICE_NAME   L"\\Device\\VantaGuard"
#define VANTA_SYMLINK_NAME  L"\\DosDevices\\VantaGuard"
#define VANTA_USER_DEVICE   L"\\\\.\\VantaGuard"
#define VANTA_SERVICE_NAME  L"OwlGuard"

#define IOCTL_VANTA_PROTECT_PID \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VANTA_UNPROTECT_PID \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VANTA_QUERY \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

// acesso removido de qualquer handle aberto contra um processo protegido
#define VANTA_DENY_MASK (PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD)
