#include "../patch_system.h"
#include "../patch_helpers.h"
#include "../logger.h"
#include "../critical_section_hook.h"
#include "imgui.h"

class CriticalSectionUpgradePatch : public OptimizationPatch {
  public:
    CriticalSectionUpgradePatch() : OptimizationPatch("CriticalSectionUpgrade", nullptr) {}

    bool Install() override {
        // Like the mimalloc patch, this `OptimizationPatch` exists solely to facilitate enablement.
        // This function just updates the state so it can be saved :D
        isEnabled = true;
        return true;
    }

    bool Uninstall() override {
        // Same as Install, we just update state.
        isEnabled = false;
        return true;
    }

    void RenderCustomUI() override {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "REQUIRES RESTART TO APPLY CHANGES");
        ImGui::TextWrapped("This patch replaces usage of Critical Section objects with SRWLock instances and requires a restart to take effect");

        if (g_upgradingCriticalSections) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Current Status: ACTIVE");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Current Status: INACTIVE");
        }

        if (isEnabled != g_upgradingCriticalSections) { ImGui::TextDisabled("(Will be %s on next restart)", isEnabled ? "Active" : "Inactive"); }
    }
};

REGISTER_PATCH(CriticalSectionUpgradePatch, {.displayName = "Critical Section Upgrade",
                                                .description = "Redirects usage of Win32 Critical Section objects to SRWLock instances.",
                                                .category = "Performance",
                                                .experimental = true,
                                                .supportedVersions = VERSION_ALL,
                                                .technicalDetails = {"Game must be restarted to use."}})
