
#ifndef WINDOWS_SIMULATION_DISPLAY_BUBBLE_H
#define WINDOWS_SIMULATION_DISPLAY_BUBBLE_H

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "fusion_profile.h"

// Rendering identity that survives geometry and lifecycle transitions.
// Stable promotion must not look like a newly-created bubble to the shader.
struct BubbleVisualState {
    bool initialized = false;
    float filmBaseThicknessScale = 1.0f;
    float filmThicknessAmplitudeScale = 1.0f;
    float interferencePhase = 0.0f;
    uint32_t noiseSeed = 0u;
    float animationTimeOrigin = 0.0f;
    glm::vec3 textureOriginWorld = glm::vec3(0.0f);
    glm::mat3 textureBasis = glm::mat3(1.0f);
    float referenceRadius = 1.0f;
    float ior = 1.33f;
    float fresnelStrength = 1.0f;
    float opacity = 1.0f;
    float opticalNormalBlendAtOrigin = 1.0f;
    float opticalNormalRelaxationRate = 0.0f;
};

struct DisplayBubble {
    struct SurfaceControl {
        glm::vec3 localDir;
        glm::vec3 displacement;
        glm::vec3 velocity;
    };

    uint64_t id = 0;
    glm::vec3 basePosition = glm::vec3(0.0f);
    glm::vec3 position = glm::vec3(0.0f);
    glm::vec3 velocity = glm::vec3(0.0f);
    float radius = 0.0f;
    float initialRadius = 0.0f;
    float targetVolume = 0.0f;
    float phase = 0.0f;
    float windAmplitude = 0.0f;
    float floatAmplitude = 0.0f;
    float speed = 0.0f;
    float alpha = 1.0f;
    float contactTime = 0.0f;
    float mergeProgress = 0.0f;
    float filmThickness = 1.0f;
    float contactStrength = 0.0f;
    float surfaceDynamicsBlend = 1.0f;
    glm::vec3 contactAxis = glm::vec3(1.0f, 0.0f, 0.0f);
    bool volumeTransferred = false;
    float burstElapsed = 0.0f;
    float burstDuration = 0.5f;
    float burstScale = 1.0f;

    enum class State {
        Free,
        Touch,
        SharedFilm,
        NeckForming,
        Merged,
        Pinching,
        Separated,
        Burst,
        Dead
    } state = State::Free;

    std::vector<SurfaceControl> surfaceControls;
    BubbleVisualState visualState;
};

struct BubbleContactPair {
    enum class State {
        Free,
        Touch,
        SharedFilm,
        NeckForming,
        Merged,
        Pinching,
        Separated,
        Burst
    };

    enum class CoalescenceOutcome {
        Undecided,
        SeparateAfterContact,
        StayDoubleBubble,
        WillCoalesce
    };

    uint64_t a = 0;
    uint64_t b = 0;
    float contactTime = 0.0f;
    float visualTransitionTime = 0.0f;
    float preContactProgress = 0.0f;
    float contactActivation = 0.0f;
    float interactionCompression = 0.0f;
    float interactionVelocity = 0.0f;
    float bridgeStrength = 0.0f;
    float geometryBlend = 0.0f;
    float neckRadius = 0.0f;
    float contactRadius = 0.0f;
    float contactRadiusVelocity = 0.0f;
    glm::vec3 filteredNormal = glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 filteredPlaneCenter = glm::vec3(0.0f);
    bool contactFrameInitialized = false;
    glm::vec3 fusionAxis = glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 fusionSide = glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec3 fusionBinormal = glm::vec3(0.0f, 0.0f, 1.0f);
    bool fusionFrameInitialized = false;
    float filmThickness = 1.0f;
    float ruptureRisk = 0.0f;
    float coalescenceScore = 0.0f;
    float coalescenceRandom = 0.0f;
    bool outcomeRandomSampled = false;
    float stableDepthRandom = 0.5f;
    bool stableDepthRandomSampled = false;
    float fusionElapsed = 0.0f;
    float ruptureProgress = 0.0f;
    float neckProgress = 0.0f;
    float relaxationProgress = 0.0f;
    // Persistent Lagrangian meridian. These nodes are created once at rupture
    // and subsequently advanced from their previous positions and velocities.
    std::vector<FusionProfileNode> fusionProfile;
    bool fusionProfileInitialized = false;
    float conservedFusionVolume = 0.0f;
    float theoreticalUncompressedVolume = 0.0f;
    float initialFusionArea = 0.0f;
    float currentFusionArea = 0.0f;
    float fusionRmsSpeed = 0.0f;
    float fusionCurvatureVariation = 0.0f;
    FusionProfileDiagnostics fusionDiagnostics;
    BubbleVisualState visualState;
    float fusionStableTime = 0.0f;
    std::size_t fusionNeckNode = 0;
    float fusionShapeMode = 0.0f;
    float previousFusionShapeMode = 0.0f;
    int fusionShapeZeroCrossings = 0;
    std::vector<std::size_t> fusionDiagnosticNodeIndices;
    std::vector<std::vector<glm::vec3>> fusionDiagnosticTrails;
    glm::vec3 fusionCenter = glm::vec3(0.0f);
    glm::vec3 fusionCenterVelocity = glm::vec3(0.0f);
    float maximumRelativeVolumeError = 0.0f;
    float maximumCentroidError = 0.0f;
    float fusionCompletionHold = 0.0f;
    float separationElapsed = 0.0f;
    float restDistance = 0.0f;
    float targetVolume = 0.0f;
    State state = State::Free;
    CoalescenceOutcome outcome = CoalescenceOutcome::Undecided;
    bool active = false;
    bool bonded = false;
    bool candidate = false;
    bool persistentRenderPair = false;
    bool fusionActive = false;
    bool fusionComplete = false;
    bool separationImpulseApplied = false;
    float candidateExitTime = 0.0f;
};

#endif
