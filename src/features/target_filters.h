#pragma once
#include "features.h"
#include "../game/entity_tracker.h"

namespace Features::TargetFilters
{
    inline bool EspCategory(Game::EntityTracker::NpcCategory category,
                            Game::EntityTracker::Hostility hostility, const EspSettings& settings)
    {
        using namespace Game::EntityTracker;
        if (category == NpcCategory::Police) return settings.showPolice;
        if (hostility == Hostility::Hostile) return settings.showEnemies;
        switch (category)
        {
        case NpcCategory::Civilian: return settings.showCivilians;
        case NpcCategory::Enemy: return settings.showEnemies;
        default: return settings.showUnclassified;
        }
    }

    inline bool AimCategory(const Game::EntityTracker::PuppetSnapshot& puppet, const AimbotSettings& settings)
    {
        using namespace Game::EntityTracker;
        if (puppet.isDead) return false;
        if (puppet.category == NpcCategory::Police) return settings.targetPolice;
        return (puppet.hostility == Hostility::Hostile || puppet.category == NpcCategory::Enemy) &&
               settings.targetEnemies;
    }

    inline bool EspPose(const Game::EntityTracker::PuppetSnapshot& puppet, const EspSettings& settings)
    {
        return settings.enabled && (settings.boundingBoxes || settings.skeleton || settings.healthBars ||
               settings.showName || settings.showDistance) && (!settings.hideDead || !puppet.isDead) &&
               EspCategory(puppet.category, puppet.hostility, settings);
    }

    inline bool AimPose(const Game::EntityTracker::PuppetSnapshot& puppet, const AimbotSettings& settings)
    {
        return settings.enabled && AimCategory(puppet, settings) &&
               (!settings.requireHealthPool || (puppet.healthValid && puppet.healthCurrent > 0.0f)) &&
               (!settings.limitHealthPool || !puppet.healthValid || puppet.healthMax <= settings.maxHealthPool);
    }
}
