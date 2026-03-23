#pragma once
#include <windows.h>
#include <winternl.h>
#include <stdint.h>

// Refer to https://learn.microsoft.com/windows/win32/devnotes/ldrdllnotification

static constexpr uint32_t LDR_DLL_NOTIFICATION_REASON_LOADED = 1;
static constexpr uint32_t LDR_DLL_NOTIFICATION_REASON_UNLOADED = 2;

struct LDR_DLL_LOADED_NOTIFICATION_DATA {
    uint32_t Flags;
    const UNICODE_STRING* FullDllName;
    const UNICODE_STRING* BaseDllName;
    void* DllBase;
    uint32_t SizeOfImage;
};

struct LDR_DLL_UNLOADED_NOTIFICATION_DATA {
    uint32_t Flags;
    const UNICODE_STRING* FullDllName;
    const UNICODE_STRING* BaseDllName;
    void* DllBase;
    uint32_t SizeOfImage;
};

union LDR_DLL_NOTIFICATION_DATA {
    LDR_DLL_LOADED_NOTIFICATION_DATA Loaded;
    LDR_DLL_UNLOADED_NOTIFICATION_DATA Unloaded;
};

extern "C" {
typedef void(__stdcall* PLDR_DLL_NOTIFICATION_FUNCTION)(uint32_t NotificationReason, const LDR_DLL_NOTIFICATION_DATA* NotificationData, void* Context);

typedef NTSTATUS(__stdcall* LdrRegisterDllNotification)(uint32_t Flags, PLDR_DLL_NOTIFICATION_FUNCTION NotificationFunction, void* Context, void** Cookie);
typedef NTSTATUS(__stdcall* LdrUnregisterDllNotification)(void* Cookie);
}

extern LdrRegisterDllNotification ldrRegisterDllNotification;
extern LdrUnregisterDllNotification ldrUnregisterDllNotification;

bool LinkDLLNotificationFunctions();
