#include "critical_section_hook.h"
#include "utils.h"
#include "config/config_paths.h"
#include "patch_system.h"
#include "patch_helpers.h"
#include "logger.h"
#include "dll_loading_notification.h"
#include <intrin.h>
#include <stdint.h>
#include <format>
#include <string>
#include <vector>

bool g_upgradingCriticalSections = false;
static inline void* g_criticalSectionUpgradeDLLNotificationCookie = nullptr;

static inline uintptr_t g_CheckedDelete;
static inline uintptr_t g_CheckedEnter;
static inline uintptr_t g_CheckedLeave;

static_assert(offsetof(UpgradedCriticalSection::Data, srwLock) == 4);
static_assert(offsetof(UpgradedCriticalSection::Data, recursiveAcquisitionCounter) == 8);
static_assert(offsetof(UpgradedCriticalSection::Data, owningThread) == 12);
static_assert(sizeof(UpgradedCriticalSection::Data) == 16);
#define srwLock 4
#define recursiveAcquisitionCounter 8
#define owningThread 12
// This is the offset of `ClientId.UniqueThread` in the TEB.
#define currentThread 0x24

#pragma warning(disable : 4102, justification: "Shut up and compile the code.")

__declspec(naked) void __stdcall UpgradedCriticalSection::Initialize(UpgradedCriticalSection* self) {
    // Just zero init the structure.
    __asm {
        mov ecx, [esp + 4]
        pxor xmm0, xmm0
        // This is atomic.
        movdqu [ecx], xmm0
        ret 4
    }
}

__declspec(naked) void __stdcall UpgradedCriticalSection::InitializeWithSpinCount(UpgradedCriticalSection* self, DWORD spinCount) {
    __asm {
        mov ecx, [esp + 4]
        pxor xmm0, xmm0
        movdqu [ecx], xmm0
        ret 8
    }
}

__declspec(naked) void __stdcall UpgradedCriticalSection::Delete(UpgradedCriticalSection* self) {
    // This could be a no-op, but whatever, we'll zero it for the craic.
    __asm {
        mov ecx, [esp + 4]
        pxor xmm0, xmm0
        movdqu [ecx], xmm0
        ret 4
    }
}

__declspec(naked) void __stdcall UpgradedCriticalSection::Enter(UpgradedCriticalSection* self) {
    __asm {
        mov ecx, [esp + 4]
        mov edx, fs:[currentThread]
        cmp [ecx + owningThread], edx
        je ownedByThisThread
    acquire:
        add ecx, srwLock
        push ecx
        call AcquireSRWLockExclusive
        mov edx, fs:[currentThread]
        mov ecx, [esp + 4]
        mov [ecx + owningThread], edx
        ret 4
    ownedByThisThread:
        inc dword ptr [ecx + recursiveAcquisitionCounter]
        ret 4
    }
}

static_assert(sizeof(BOOL) == 4);
static_assert(sizeof(BOOLEAN) == 1);

__declspec(naked) BOOL __stdcall UpgradedCriticalSection::TryEnter(UpgradedCriticalSection* self) {
    __asm {
        mov ecx, [esp + 4]
        mov edx, fs:[currentThread]
        xor eax, eax
        cmp dword ptr [ecx + owningThread], 0
        jne ownedByAnyThread
    ownedByNoThread:
        add ecx, srwLock
        push ecx
        call TryAcquireSRWLockExclusive
        // TryAcquireSRWLockExclusive returns a BOOLEAN, whereas TryEnterCriticalSection returns a BOOL.
        movzx eax, al;
        test eax, eax
        // We have to branch here, a cmov can't be used as we may not have acquired the lock.
        jz notAcquired
    acquired:
        mov edx, fs:[currentThread]
        mov ecx, [esp + 4]
        mov [ecx + owningThread], edx
    notAcquired:
        ret 4
    ownedByAnyThread:
        cmp [ecx + owningThread], edx
        jne ownedByOtherThread
    ownedByThisThread:
        inc dword ptr [ecx + recursiveAcquisitionCounter]
        inc eax
    ownedByOtherThread:
        ret 4
    }
}

__declspec(naked) void __stdcall UpgradedCriticalSection::Leave(UpgradedCriticalSection* self) {
    __asm {
        mov ecx, [esp + 4]
        cmp dword ptr [ecx + recursiveAcquisitionCounter], 0
        jne lockIsStillHeld
    release:
        lea edx, [ecx + srwLock]
        mov [esp + 4], edx
        and dword ptr [ecx + owningThread], 0
        jmp ReleaseSRWLockExclusive
    lockIsStillHeld:
        dec dword ptr [ecx + recursiveAcquisitionCounter]
        ret 4
    }
}

#undef srwLock
#undef owningThread
#undef recursiveAcquisitionCounter

void __stdcall UpgradedCriticalSection::CheckedDelete(UpgradedCriticalSection* self) {
    if (self->IsUpgraded()) { UpgradedCriticalSection::Delete(self); } else { DeleteCriticalSection(&self->criticalSection); }
}

void __stdcall UpgradedCriticalSection::CheckedEnter(UpgradedCriticalSection* self) {
    if (self->IsUpgraded()) { UpgradedCriticalSection::Enter(self); } else { EnterCriticalSection(&self->criticalSection); }
}

BOOL __stdcall UpgradedCriticalSection::CheckedTryEnter(UpgradedCriticalSection* self) {
    if (self->IsUpgraded()) { return UpgradedCriticalSection::TryEnter(self); } else { return TryEnterCriticalSection(&self->criticalSection); }
}

void __stdcall UpgradedCriticalSection::CheckedLeave(UpgradedCriticalSection* self) {
    if (self->IsUpgraded()) { UpgradedCriticalSection::Leave(self); } else { LeaveCriticalSection(&self->criticalSection); }
}

#pragma warning(default : 4102)

bool ShouldEnableCriticalSectionUpgrading(const toml::table* config) {
    if (config == nullptr) { return false; }
    return config->at_path("patches.CriticalSectionUpgrade.enabled").value_or(false);
}

static inline const AddressInfo monoLinkInitializeCriticalSectionAndSpinCountBranchAddressInfo = {.name = "CriticalSectionUpgrade::monoLinkInitializeCriticalSectionAndSpinCountBranch",
    .addresses =
        {
            {GameVersion::Retail, 0x00e4e7c6},
            {GameVersion::Steam, 0x00e4e4d6},
            {GameVersion::EA, 0x00e4e526},
        },
    .pattern = "85 C0 74 11 68 A0 0F 00 00 68 ?? ?? ?? ?? FF D0 E9 ?? ?? ?? ??",
    .patternOffset = -14};

void ApplyCriticalSectionUpgradePatches(const IMAGE_IMPORT_DESCRIPTOR* kernel32Import) {
    using namespace std::literals;

    auto monoLinkInitializeCriticalSectionAndSpinCountBranchAddress = monoLinkInitializeCriticalSectionAndSpinCountBranchAddressInfo.Resolve();
    if (!monoLinkInitializeCriticalSectionAndSpinCountBranchAddress) {
        LOG_ERROR("[CriticalSectionUpgrade] Could not resolve monoLinkInitializeCriticalSectionAndSpinCountBranch address");
        return;
    }
    uintptr_t monoLinkInitializeCriticalSectionAndSpinCountBranch = *monoLinkInitializeCriticalSectionAndSpinCountBranchAddress;

    if (g_gameVersion == GameVersion::EA) {
        g_CheckedDelete = reinterpret_cast<uintptr_t>(&UpgradedCriticalSection::CheckedDelete);
        g_CheckedEnter = reinterpret_cast<uintptr_t>(&UpgradedCriticalSection::CheckedEnter);
        g_CheckedLeave = reinterpret_cast<uintptr_t>(&UpgradedCriticalSection::CheckedLeave);
    }

    PEHelper::ImportAddressTableRemapper remapper{};
    remapper.remap = {
        {"InitializeCriticalSection"sv, reinterpret_cast<uintptr_t>(&UpgradedCriticalSection::Initialize)},
        {"InitializeCriticalSectionAndSpinCount"sv, reinterpret_cast<uintptr_t>(&UpgradedCriticalSection::InitializeWithSpinCount)},
        {"DeleteCriticalSection"sv, reinterpret_cast<uintptr_t>(&UpgradedCriticalSection::Delete)},
        {"EnterCriticalSection"sv, reinterpret_cast<uintptr_t>(&UpgradedCriticalSection::Enter)},
        {"TryEnterCriticalSection"sv, reinterpret_cast<uintptr_t>(&UpgradedCriticalSection::TryEnter)},
        {"LeaveCriticalSection"sv, reinterpret_cast<uintptr_t>(&UpgradedCriticalSection::Leave)},
    };

    bool successful = true;
    auto tx = PatchHelper::BeginTransaction();
    remapper.tracker = &tx.locations;

    if (g_gameVersion == GameVersion::EA) {
        // A1 ?? ?? ?? ?? 56 57 83 C0 10 50 FF 15 .. .. .. ..
        successful &= PatchHelper::WriteDWORD(0x004fa4f7 + 2, reinterpret_cast<uintptr_t>(&g_CheckedEnter), &tx.locations);
        // 83 C0 10 50 FF 15 .. .. .. .. 5F 8B C6 5E
        successful &= PatchHelper::WriteDWORD(0x004fa569 + 2, reinterpret_cast<uintptr_t>(&g_CheckedLeave), &tx.locations);
        // A1 ?? ?? ?? ?? 53 83 C0 10 50 FF 15 .. .. .. ..
        successful &= PatchHelper::WriteDWORD(0x004fa35b + 2, reinterpret_cast<uintptr_t>(&g_CheckedEnter), &tx.locations);
        // 8B 15 ?? ?? ?? ?? 83 C2 10 52 FF 15 .. .. .. ..
        successful &= PatchHelper::WriteDWORD(0x004fa3b3 + 2, reinterpret_cast<uintptr_t>(&g_CheckedLeave), &tx.locations);
        // A1 ?? ?? ?? ?? 83 C0 10 50 FF 15 .. .. .. .. 8B 0D ?? ?? ?? ?? 8B 51 FC
        successful &= PatchHelper::WriteDWORD(0x004fa31c + 2, reinterpret_cast<uintptr_t>(&g_CheckedDelete), &tx.locations);
    }

    // Mono links InitializeCriticalSectionAndSpinCount via GetProcAddress, so we'll skip that,
    // so that it uses InitializeCriticalSection via the game's IAT.
    successful &= PatchHelper::WriteByte(monoLinkInitializeCriticalSectionAndSpinCountBranch, 0xEB /* jmp */, &tx.locations);
    successful &= WalkImportAddressTable(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)), kernel32Import, &remapper);
    successful &= !remapper.anyFailed;

    if (!successful || !PatchHelper::CommitTransaction(tx)) {
        PatchHelper::RollbackTransaction(tx);
        LOG_ERROR("[CriticalSectionUpgrade] Failed to install");
        return;
    }

    g_upgradingCriticalSections = true;

    LOG_INFO("[CriticalSectionUpgrade] Successfully installed");
}

void __stdcall InitializeCriticalSectionUpgradingViaDLLNotification(uint32_t reason, const LDR_DLL_NOTIFICATION_DATA* data, void* context) {
    auto kernel32Import = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(context);

    if (!PEHelper::DLLImportIsBound(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)), kernel32Import)) {
        LOG_INFO("[CriticalSectionUpgrade] Kernel32.dll is still not yet bound.");
        return;
    }

    ldrUnregisterDllNotification(g_criticalSectionUpgradeDLLNotificationCookie);
    g_criticalSectionUpgradeDLLNotificationCookie = nullptr;

    ApplyCriticalSectionUpgradePatches(kernel32Import);
}

void InitializeCriticalSectionUpgrading(const toml::table* config) {
    assert(!g_upgradingCriticalSections);

    if (!ShouldEnableCriticalSectionUpgrading(config)) {
        return;
    }

    LOG_INFO("[CriticalSectionUpgrade] Installing...");

    uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const IMAGE_IMPORT_DESCRIPTOR* kernel32Import = PEHelper::FindDLLImportByName(exe, reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(GetGameImportTableAddress(exe)), "KERNEL32.DLL");

    if (kernel32Import == nullptr) {
        LOG_ERROR("[CriticalSectionUpgrade] Could not find import of Kernel32.dll");
        return;
    }

    if (!PEHelper::DLLImportIsBound(exe, kernel32Import)) {
        LOG_INFO("[CriticalSectionUpgrade] Kernel32.dll is not yet bound. Installing via DLL notification instead.");

        if (LinkDLLNotificationFunctions()) {
            NTSTATUS error = ldrRegisterDllNotification(0, &InitializeCriticalSectionUpgradingViaDLLNotification, const_cast<IMAGE_IMPORT_DESCRIPTOR*>(kernel32Import), &g_criticalSectionUpgradeDLLNotificationCookie);
            if (error) {
                LOG_ERROR(std::format("[CriticalSectionUpgrade] Could not register for DLL notifications. NTSTATUS: 0x{:08X}", error));
            }
        } else {
            LOG_ERROR("[CriticalSectionUpgrade] DLL notification functions could not be linked.");
        }
        return;
    }

    LOG_INFO("[CriticalSectionUpgrade] Kernel32.dll is bound. Installing immediately.");

    ApplyCriticalSectionUpgradePatches(kernel32Import);
}
