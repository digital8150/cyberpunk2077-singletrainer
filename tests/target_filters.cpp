#include "features/target_filters.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

void Check(bool condition, const char* name)
{
    if (!condition) { std::fprintf(stderr, "%s\n", name); std::exit(1); }
}

int main()
{
    using namespace Game::EntityTracker;
    using namespace Features;
    using namespace Features::TargetFilters;
    EspSettings esp;
    esp.enabled = true;
    esp.showCivilians = esp.showUnclassified = false;
    AimbotSettings aim;
    aim.enabled = true;
    PuppetSnapshot npc;
    npc.healthValid = true;
    npc.healthCurrent = npc.healthMax = 100;
    for (auto category : {NpcCategory::Civilian, NpcCategory::Other})
    {
        npc.category = category;
        npc.hostility = Hostility::Unknown;
        Check(!EspPose(npc, esp) && !AimPose(npc, aim), "disabled civilian/other must request no pose");
        npc.hostility = Hostility::Hostile;
        Check(EspPose(npc, esp) && AimPose(npc, aim), "hostile archetypes must follow enemy toggle");
    }
    npc.category = NpcCategory::Police;
    esp.showPolice = false;
    Check(!EspPose(npc, esp) && !AimPose(npc, aim), "hostile police must keep separate toggle");
    aim.targetPolice = true;
    Check(AimPose(npc, aim) && !EspPose(npc, esp), "aim-only target must remain eligible");
    npc.category = NpcCategory::Enemy;
    npc.isDead = true;
    Check(!EspPose(npc, esp) && !AimPose(npc, aim), "dead targets must respect consumer rules");
    esp.hideDead = false;
    Check(EspPose(npc, esp), "explicit corpse display needs pose");
    npc.isDead = false;
    npc.healthValid = false;
    Check(!AimPose(npc, aim), "missing required health must reject aim pose");
    aim.requireHealthPool = false;
    Check(AimPose(npc, aim), "optional health must not reject aim pose");
    npc.healthValid = true;
    npc.healthMax = 5000;
    Check(!AimPose(npc, aim), "health cap must apply before pose request");
    esp.boundingBoxes = esp.skeleton = esp.healthBars = esp.showName = esp.showDistance = false;
    esp.nativeHighlight = true;
    Check(!EspPose(npc, esp), "native-only ESP needs no skeleton cache");
    aim.enabled = esp.enabled = false;
    Check(!EspPose(npc, esp) && !AimPose(npc, aim), "disabled consumers must request nothing");
}
