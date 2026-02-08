#include "../patch_system.h"
#include "../patch_helpers.h"
#include "../logger.h"
#include <bit>

#pragma comment(lib, "ntdll.lib")

extern "C" __declspec(dllimport) NTSTATUS __stdcall NtDelayExecution(BOOLEAN Alertable, LARGE_INTEGER* Interval);
extern "C" __declspec(dllimport) NTSTATUS __stdcall NtQueryTimerResolution(ULONG* MaximumTime, ULONG* MinimumTime, ULONG* CurrentTime);

static float tickRateLimitSettingStorage;
static float tickRateLimit;

static float frameRateLimitSettingStorage;
static float frameRateLimit;
static float frameRateLimitInactiveSettingStorage;
static float frameRateLimitInactive;

static double qpcToHectonanosecondsMultiplier;
static double hectonanosecondsToQPCMultiplier;
static uint64_t performanceFrequency;
static double performanceFrequencyFloat;

static uint64_t timerFrequency;

static uint64_t idealSimulationCycleTime;
static uint64_t previousSimulationCycleTime = 0;

static uint64_t idealPresentationFrameTime;
static uint64_t idealPresentationFrameInactiveTime;
static uint64_t previousPresentationFrameTime = 0;

struct ScriptHostBase {
    uint64_t HookedIdleSimulationCycle();
};

static decltype(&ScriptHostBase::HookedIdleSimulationCycle) originalIdleSimulationCycle;

uint64_t WaitUntilPrecisely(uint64_t time, uint64_t now) {
    uint64_t sleepUntil = (time / timerFrequency) * timerFrequency;

    if (sleepUntil > now) {
        uint64_t durationInQPC = sleepUntil - now;
        uint64_t duration = static_cast<uint64_t>(durationInQPC * qpcToHectonanosecondsMultiplier);
        NtDelayExecution(false, reinterpret_cast<LARGE_INTEGER*>(&duration));
    }

    do { QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&now)); } while (now < time);

    return now;
}

uint64_t ScriptHostBase::HookedIdleSimulationCycle() {
    if ((idealSimulationCycleTime == 0) | !*reinterpret_cast<const uint8_t*>((reinterpret_cast<uintptr_t>(this) + 0xa60))) { return previousSimulationCycleTime; }

    uint64_t idealTimeForThisCycle = previousSimulationCycleTime + idealSimulationCycleTime;

    uint64_t now;
    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&now));

    // If we're on time, we'll wait for the ideal cycle-time to elapse.
    if (now < idealTimeForThisCycle) { now = WaitUntilPrecisely(idealTimeForThisCycle, now); }

    // We'll round down the time so that if we're running late the next cycle will occur earlier.
    previousSimulationCycleTime = (now / idealSimulationCycleTime) * idealSimulationCycleTime;

    return previousSimulationCycleTime;
}

void __stdcall DelayAfterFramePresentation(uintptr_t graphicsDeviceStructure) {
    bool gameWindowIsNotForeground = *reinterpret_cast<const uint8_t*>(graphicsDeviceStructure + 0x8d);

    uint64_t idealTime = gameWindowIsNotForeground ? idealPresentationFrameInactiveTime : idealPresentationFrameTime;

    if (idealTime == 0) { return; }

    uint64_t idealTimeForThisFrame = previousPresentationFrameTime + idealTime;

    uint64_t now;
    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&now));

    // If we're on time, we'll wait for the ideal frame-time to elapse.
    while (now < idealTimeForThisFrame) { now = WaitUntilPrecisely(idealTimeForThisFrame, now); }

    // We'll round down the time so that if we're running late the next frame will occur earlier.
    previousPresentationFrameTime = (now / idealTime) * idealTime;
}

class SmoothPatchPrecise : public OptimizationPatch {
  private:
    // This is a call of Sims3::Scripting::ScriptHostBase::IdleSimulationCycle from Sims3::MonoEngine::MonoScriptHost::Simulate.
    static inline const AddressInfo idleSimulationCycleCallAddressInfo = {.name = "SmoothPatchPrecise::idleSimulationCycleCall",
        .addresses =
            {
                {GameVersion::Retail, 0x00d8246e},
                {GameVersion::Steam, 0x00d81fde},
                {GameVersion::EA, 0x00d8239e},
            },
        .pattern = "8B 86 C0 0A 00 00 3B C3 74 0F 8B 4C 24 5C 8B 11 6A 01 53 50 8B 42 24 FF D0 8B CE",
        .patternOffset = 27};

    static inline const AddressInfo limitFrameRateWhenInactiveAddressInfo = {
        .name = "SmoothPatchPrecise::limitFrameRateWhenInactive",
        .addresses =
            {
                {GameVersion::Retail, 0x00eca64a},
                {GameVersion::Steam, 0x00ec9fba},
                {GameVersion::EA, 0x00ec9f9a},
            },
        .pattern = "80 BE 8D 00 00 00 00 5E 74 15 8D 4C 24 24 51 C7 44 24 28 1E 00 00 00",
    };

    static constexpr uint32_t oneSecondAsHectonanoseconds = 10'000'000;

    std::vector<PatchHelper::PatchLocation> patchedLocations;

  public:
    SmoothPatchPrecise() : OptimizationPatch("SmoothPatchPrecise", nullptr) {
        RegisterFloatSetting(&tickRateLimitSettingStorage, "tickRateLimit", SettingUIType::InputBox,
            480.0f, // Most people will be using a 60 Hz display, so we default to a multiple of 60,
                    // 480 TPS should be fine for weaker processors.
            0.0f,
            10000.0f, // 10,000 Hz? Whatever, sure.
            "Tick-rate limit (how many times a second the game will simulate a slice of time)",
            {
                {"Unlimited##TPS", 0.0f},
                {"30 TPS", 30.0f},
                {"60 TPS", 60.0f},
                {"75 TPS", 75.0f},
                {"120 TPS", 120.0f},
                {"144 TPS", 144.0f},
                {"160 TPS", 160.0f},
                {"165 TPS", 165.0f},
                {"200 TPS", 200.0f},
                {"240 TPS", 240.0f},
                {"360 TPS", 360.0f},
                {"480 TPS", 480.0f},
                {"960 TPS", 960.0f},
            });

        RegisterFloatSetting(&frameRateLimitSettingStorage, "frameRateLimit", SettingUIType::InputBox,
            60.0f, // Most people will be using a 60 Hz display.
            0.0f,
            10000.0f, // 10,000 Hz? Whatever, sure.
            "Frame-rate limit (how many frames a second the game will present to the display)",
            {
                {"Unlimited##FPS", 0.0f},
                {"30 FPS##A", 30.0f},
                {"60 FPS##A", 60.0f},
                {"75 FPS##A", 75.0f},
                {"120 FPS##A", 120.0f},
                {"144 FPS##A", 144.0f},
                {"160 FPS##A", 160.0f},
                {"165 FPS##A", 165.0f},
                {"200 FPS##A", 200.0f},
                {"240 FPS##A", 240.0f},
                {"360 FPS##A", 360.0f},
                {"480 FPS##A", 480.0f},
                {"960 FPS##A", 960.0f},
            });

        RegisterFloatSetting(&frameRateLimitInactiveSettingStorage, "frameRateLimitInactive", SettingUIType::InputBox,
            30.0f, // This is what the game defaults to (roughly).
            -1.0f,
            10000.0f, // 10,000 Hz? Whatever, sure.
            "Inactive frame-rate limit (for when the game's window isn't active)",
            {
                {"Match FPS", -1.0f},
                {"30 FPS##I", 30.0f},
                {"60 FPS##I", 60.0f},
                {"75 FPS##I", 75.0f},
                {"120 FPS##I", 120.0f},
                {"144 FPS##I", 144.0f},
                {"160 FPS##I", 160.0f},
                {"165 FPS##I", 165.0f},
                {"200 FPS##I", 200.0f},
                {"240 FPS##I", 240.0f},
                {"360 FPS##I", 360.0f},
                {"480 FPS##I", 480.0f},
                {"960 FPS##I", 960.0f},
            });
    }

    bool Install() override {
        if (isEnabled) return true;
        lastError.clear();
        LOG_INFO("[SmoothPatchPrecise] Installing...");

        tickRateLimit = tickRateLimitSettingStorage;
        frameRateLimit = frameRateLimitSettingStorage;
        frameRateLimitInactive = frameRateLimitInactiveSettingStorage < 0.0f ? frameRateLimit : frameRateLimitInactiveSettingStorage;

        ULONG timerResolution;
        NtQueryTimerResolution(nullptr, nullptr, &timerResolution);

        QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER*>(&performanceFrequency));

        double frequency = static_cast<double>(performanceFrequency);
        performanceFrequencyFloat = frequency;
        idealSimulationCycleTime = tickRateLimit == 0 ? 0 : static_cast<uint64_t>(frequency / tickRateLimit);
        idealPresentationFrameTime = frameRateLimit == 0 ? 0 : static_cast<uint64_t>(frequency / frameRateLimit);
        idealPresentationFrameInactiveTime = frameRateLimitInactive == 0 ? 0 : static_cast<uint64_t>(frequency / frameRateLimitInactive);
        qpcToHectonanosecondsMultiplier = static_cast<double>(oneSecondAsHectonanoseconds) / frequency;
        hectonanosecondsToQPCMultiplier = frequency / oneSecondAsHectonanoseconds;
        timerFrequency = static_cast<uint64_t>(static_cast<double>(timerResolution) * hectonanosecondsToQPCMultiplier);
        timerFrequency = timerFrequency == 0 ? 1 : timerFrequency;

        LOG_INFO(std::format("[SmoothPatchPrecise] tickRateLimit: {}; frameRateLimit: {}; frameRateLimitInactive: {}; performanceFrequency: {}; idealSimulationCycleTime: {}; idealPresentationFrameTime: {}; "
                             "idealPresentationFrameInactiveTime: {}; timerFrequency: {}; qpcToHectonanosecondsMultiplier: {}; hectonanosecondsToQPCMultiplier: {}",
            tickRateLimit, frameRateLimit, frameRateLimitInactive, performanceFrequency, idealSimulationCycleTime, idealPresentationFrameTime, idealPresentationFrameInactiveTime, timerFrequency,
            qpcToHectonanosecondsMultiplier, hectonanosecondsToQPCMultiplier));

        auto idleSimulationCycleCallAddress = idleSimulationCycleCallAddressInfo.Resolve();
        if (!idleSimulationCycleCallAddress) { return Fail("Could not resolve idleSimulationCycleCall address"); }
        uintptr_t idleSimulationCycleCall = *idleSimulationCycleCallAddress;

        auto limitFrameRateWhenInactiveAddress = limitFrameRateWhenInactiveAddressInfo.Resolve();
        if (!limitFrameRateWhenInactiveAddress) { return Fail("Could not resolve limitFrameRateWhenInactive address"); }
        uintptr_t limitFrameRate = *limitFrameRateWhenInactiveAddress;

        bool successful = true;
        auto tx = PatchHelper::BeginTransaction();

        // clang-format off
        uint8_t frameRateLimiter[9] = {
            /* A DS prefix to pad this instruction such that the following call
               ends at the same place as the original CMP instruction,
               making this patch safe to uninstall at any time. */
            0x3E, 0x56,                   // ds: push esi
        //2:
            0xE8, 0x77, 0x77, 0x77, 0x77, // call DelayAfterFramePresentation
            0x5E,                         // pop esi
            0xEB,                         // jmp <rel8>
        };
        // clang-format on

        uintptr_t delayCallDisplacement = PatchHelper::CalculateRelativeOffset(limitFrameRate + 2, reinterpret_cast<uintptr_t>(&DelayAfterFramePresentation));
        std::memcpy(frameRateLimiter + 2 + 1, &delayCallDisplacement, 4);

        uintptr_t idleSimulationCycle = std::bit_cast<uintptr_t>(&ScriptHostBase::HookedIdleSimulationCycle);
        int32_t idleCallDisplacement = PatchHelper::CalculateRelativeOffset(idleSimulationCycleCall, idleSimulationCycle);

        successful &= PatchHelper::WriteDWORD(idleSimulationCycleCall + 1, idleCallDisplacement, &tx.locations);
        successful &= PatchHelper::WriteProtectedMemory(reinterpret_cast<void*>(limitFrameRate), frameRateLimiter, 9, &tx.locations);

        if (!successful || !PatchHelper::CommitTransaction(tx)) {
            PatchHelper::RollbackTransaction(tx);
            return Fail("Failed to install");
        }

        patchedLocations = tx.locations;

        isEnabled = true;
        LOG_INFO("[SmoothPatchPrecise] Successfully installed");
        return true;
    }

    bool Uninstall() override {
        if (!isEnabled) return true;

        lastError.clear();
        LOG_INFO("[SmoothPatchPrecise] Uninstalling...");

        if (!PatchHelper::RestoreAll(patchedLocations)) {
            isEnabled = true;
            return Fail("Failed to restore original code");
        }

        isEnabled = false;
        LOG_INFO("[SmoothPatchPrecise] Successfully uninstalled");
        return true;
    }
};

REGISTER_PATCH(SmoothPatchPrecise, {.displayName = "Smooth Patch (Precise Flavour)",
                                       .description = "Adjusts the game's simulation tick-rate limiter directly. For smoother gameplay.\n"
                                                      "Much like LazyDuchess's original Smooth Patch.\n\n"
                                                      "A frame-rate limiter is also included, for convenience.\n"
                                                      "Setting the tick-rate limit to a multiple of the frame-rate limit may, theoretically, result in better frame-pacing.",
                                       .category = "Performance",
                                       .experimental = false,
                                       .supportedVersions = VERSION_ALL,
                                       .technicalDetails = {
                                           "Credit goes to LazyDuchess for the original Smooth Patch.",
                                           "This patch was authored by \"Just Harry\".",
                                           "Unlike the original Smooth Patch, this patch does not alter sleep-durations unrelated to the game's simulator.",
                                           "Additionally, this patch can sleep for sub-millisecond durations, so there is a difference, for example, between 750 TPS and 1,000 TPS.",
                                       }})
