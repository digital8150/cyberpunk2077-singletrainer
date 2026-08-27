#pragma once

#include <cstddef>
#include <cstdint>

namespace Game::AnimationData
{
    constexpr std::size_t kMaxSkeletonSegments = 64;
    constexpr std::size_t kMaxPosePoints = 8;

    struct SkeletonSegment
    {
        float start[3]{};
        float end[3]{};
    };

    struct VisualData
    {
        bool hasBounds = false;
        float boundsMinimum[3]{};
        float boundsMaximum[3]{};
        bool hasHeadPosition = false;
        float headPosition[3]{};
        // Live slot positions of the current pose. These are far tighter than the animation-system bounds and
        // follow crouching/prone poses, so the ESP box is built from them when they are available.
        std::size_t posePointCount = 0;
        float posePoints[kMaxPosePoints][3]{};
        std::size_t skeletonSegmentCount = 0;
        SkeletonSegment skeletonSegments[kMaxSkeletonSegments]{};
    };

    // Reads the active animation-system entry for an entity. All accesses are guarded because streaming can
    // invalidate an entry between the registry callback and the render frame.
    bool ReadVisualData(std::uint64_t entityId, const float entityPosition[3], VisualData& output);
}
