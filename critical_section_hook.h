#pragma once
#include <windows.h>
#include <stdint.h>
#include <toml++/toml.hpp>

void InitializeCriticalSectionUpgrading(const toml::table* config);

extern bool g_upgradingCriticalSections;

// very evil struct
union UpgradedCriticalSection {
    struct Data {
        uintptr_t debugInfo;
        SRWLOCK srwLock;
        uint32_t recursiveAcquisitionCounter;
        HANDLE owningThread;
    };

    Data data;
    CRITICAL_SECTION criticalSection;

    __forceinline bool IsUpgraded() const {
        // When a critical-section is initialised, debugInfo will either
        // be a valid pointer or -1, but not zero, whereas our upgraded
        // critical-sections will have it zeroed out.
        // So we can use this field to determine whether this is an actual critical-section or not.
        return data.debugInfo == 0;
    }

    static void __stdcall Initialize(UpgradedCriticalSection* self);
    static void __stdcall InitializeWithSpinCount(UpgradedCriticalSection* self, DWORD spinCount);
    static void __stdcall Delete(UpgradedCriticalSection* self);
    static void __stdcall Enter(UpgradedCriticalSection* self);
    static BOOL __stdcall TryEnter(UpgradedCriticalSection* self);
    static void __stdcall Leave(UpgradedCriticalSection* self);
    static void __stdcall CheckedDelete(UpgradedCriticalSection* self);
    static void __stdcall CheckedEnter(UpgradedCriticalSection* self);
    static BOOL __stdcall CheckedTryEnter(UpgradedCriticalSection* self);
    static void __stdcall CheckedLeave(UpgradedCriticalSection* self);
};
