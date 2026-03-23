#include "dll_loading_notification.h"

LdrRegisterDllNotification ldrRegisterDllNotification = nullptr;
LdrUnregisterDllNotification ldrUnregisterDllNotification = nullptr;

bool LinkDLLNotificationFunctions() {
    if (ldrRegisterDllNotification != nullptr) { return true; }

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto register_ = GetProcAddress(ntdll, "LdrRegisterDllNotification");
    auto unregister = GetProcAddress(ntdll, "LdrUnregisterDllNotification");

    if ((register_ == nullptr) | (unregister == nullptr)) {
        return false;
    }

    ldrRegisterDllNotification = reinterpret_cast<decltype(ldrRegisterDllNotification)>(register_);
    ldrUnregisterDllNotification = reinterpret_cast<decltype(ldrUnregisterDllNotification)>(unregister);

    return true;
}
