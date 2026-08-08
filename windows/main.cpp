//
// BubbleRender — Windows desktop OpenGL entry point
// Ported from HarmonyOS napi_init.cpp
//

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "stb_image.h"

#include "render/model.h"
#include "render/camera.h"
#include "render/shader.h"
#include "render/contact_geometry.h"
#include "render/bubble_surface_system.h"
#include "simulation/display_bubble.h"
#include "simulation/display_bubble_simulation.h"
#include "simulation/vortex_sheet.h"
#include "shader/shader_refraction.h"
#include "shader/shader_background.h"
#include "shader/shader_skybox.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>
#include <string>

// ============================================================
// Global state (mirrors napi_init.cpp globals)
// ============================================================
static int g_WindowWidth = 1280;
static int g_WindowHeight = 720;
static constexpr const char *g_AppTitle = "Soap Bubble Rendering - Thin Film + DBSTT";

static constexpr float kCameraMinDistance = 1.65f;
static constexpr float kCameraMaxDistance = 20.0f;
static glm::vec3 g_CameraOrbitTarget = glm::vec3(0.0f, 0.0f, 1.10f);
static float g_CameraDistance = 2.75f;
static float g_CurrentAngleX = 0.0f; // pitch
static float g_CurrentAngleY = 0.0f; // yaw

static float g_FresnelPower = 8.0f;
static float g_Shininess = 15.0f;
static float g_Diffuseness = 0.2f;
static float g_RefractionStrength = 0.35f;
static float g_EdgeDistortionBoost = 2.2f;
static float g_MaxOffsetRatio = 0.52f;
static float g_EnvironmentReflectionStrength = 0.65f;
static glm::vec3 g_Light = glm::vec3(-1.0f, 1.0f, 1.0f);
static int g_IridescenceMode = 2; // 0 = Kim2012, 1 = spectral LUT, 2 = Belcour Airy

// DBSTT vortex sheet simulation
static VortexSheetSimulation g_Sim;
static float g_BubbleRadius = 1.5f;
static float g_MainBubbleVisualRadius = 1.5f;
static float g_MainBubbleTargetRadius = 1.5f;
static int g_SimSubdivs = 3;         // icosphere subdivision level
static float g_SimPerturb = 0.16f;   // matches HarmonyOS main-bubble stretch
static float g_SimTimeStep = 0.001f; // Δt per substep
static int g_SimSubsteps = 1;        // matches HarmonyOS DBSTT substep count
static bool g_SimPaused = false;
static constexpr int kSimFrameInterval = 6;
static int g_SimFrameCounter = 0;

// Helper: convert sim mesh data to Vertex vector for GPU upload
static std::vector<Vertex> buildVerticesFromSim(const VortexSheetSimulation &sim)
{
    const auto &pos = sim.getPositions();
    const auto &nrm = sim.getNormals();
    std::vector<Vertex> verts(pos.size());
    for (size_t i = 0; i < pos.size(); ++i)
    {
        verts[i].Position = pos[i];
        verts[i].Normal = nrm[i];
        verts[i].TexCoords = glm::vec2(0.0f);
        verts[i].Tangent = glm::vec3(0.0f);
        verts[i].Bitangent = glm::vec3(0.0f);
    }
    return verts;
}

// ============================================================
// Per-display-bubble DBSTT vortex sheet sims
// ============================================================
// A DBSTT vortex sheet sim is created per bubble at unit radius.  It is
// only activated during burst: the boosted surface tension drives the
// membrane retraction as real physics.  Normal (non-burst) rendering and
// contact/fusion continue to use the existing control-point surface.
#include <unordered_map>
#include <memory>
static std::unordered_map<uint64_t, std::unique_ptr<VortexSheetSimulation>> g_BubbleSims;

static constexpr int kDisplayBubbleSimSubdivs = 2;

static VortexSheetSimulation *GetBubbleSim(uint64_t id)
{
    auto it = g_BubbleSims.find(id);
    return it != g_BubbleSims.end() ? it->second.get() : nullptr;
}

static VortexSheetSimulation *InitBubbleSim(uint64_t id, float radius)
{
    auto sim = std::make_unique<VortexSheetSimulation>();
    // Always create at unit radius — the model matrix handles visual scaling.
    // This keeps the sim independent of bubble.radius changes (e.g. demo).
    sim->initIcosphere(1.0f, kDisplayBubbleSimSubdivs, 0.04f);
    sim->timeStep = 0.001f;
    sim->substepsPerFrame = 3;
    sim->surfaceTensionStrength = 15.0f;
    sim->circulationDiffusion = 0.0008f;
    sim->flipSurfaceTensionSign = true;
    sim->diagnosticsEnabled = false;
    auto *ptr = sim.get();
    g_BubbleSims[id] = std::move(sim);
    return ptr;
}

// Build initial Vertex + index arrays from a sim mesh for EnsureBubble.
static std::pair<std::vector<Vertex>, std::vector<unsigned int>>
BuildSimRenderMesh(const VortexSheetSimulation &sim)
{
    const auto &pos = sim.getPositions();
    const auto &nrm = sim.getNormals();
    const auto &tris = sim.getTriangles();
    std::vector<Vertex> verts(pos.size());
    for (size_t i = 0; i < pos.size(); ++i)
    {
        verts[i].Position = pos[i];
        verts[i].Normal = nrm[i];
        verts[i].TexCoords = glm::vec2(0.0f);
        verts[i].Tangent = glm::vec3(0.0f);
        verts[i].Bitangent = glm::vec3(0.0f);
        verts[i].FilmDirection = glm::length(pos[i]) > 1e-6f
            ? glm::normalize(pos[i]) : glm::vec3(0.0f, 1.0f, 0.0f);
    }
    std::vector<unsigned int> indices(tris.size() * 3);
    for (size_t i = 0; i < tris.size(); ++i)
    {
        indices[3 * i]     = (unsigned int)tris[i].x;
        indices[3 * i + 1] = (unsigned int)tris[i].y;
        indices[3 * i + 2] = (unsigned int)tris[i].z;
    }
    return {verts, indices};
}

// Upload sim vertex positions to a render mesh (called each frame).
static void UpdateMeshFromSim(Mesh *mesh, const VortexSheetSimulation &sim)
{
    if (!mesh) return;
    const auto &pos = sim.getPositions();
    const auto &nrm = sim.getNormals();
    auto &verts = mesh->vertices;
    if (verts.size() != pos.size()) return;
    for (size_t i = 0; i < pos.size(); ++i)
    {
        verts[i].Position = pos[i];
        verts[i].Normal = nrm[i];
        // Keep FilmDirection pointing outward from the original direction
        // so the thin-film interference pattern stays stable.
        if (glm::dot(verts[i].FilmDirection, verts[i].FilmDirection) < 1e-6f)
            verts[i].FilmDirection = glm::length(pos[i]) > 1e-6f
                ? glm::normalize(pos[i]) : glm::vec3(0.0f, 1.0f, 0.0f);
    }
    mesh->updateVertices(verts);
}

static Camera g_Camera;

static Shader *g_RefractionShader = nullptr;
static Shader *g_BackgroundShader = nullptr;
static Shader *g_SkyboxShader = nullptr;

static Model *g_RefractModel = nullptr;
static Model *g_DecorativeBubbleModel = nullptr;
static Model *g_SkyboxModel = nullptr;

static std::vector<Model *> g_BackgroundModels;
static std::vector<glm::vec3> g_SpherePositions;

static std::vector<DisplayBubble> g_DisplayBubbles;
static std::vector<BubbleContactPair> g_ContactPairs;

static void UpdateBubbleSims(float dt)
{
    if (g_SimPaused) return;
    // DBSTT sims are only updated during burst.  Normal surface dynamics
    // are handled by BuildPersistentBubbleVertices.
    for (auto &bubble : g_DisplayBubbles)
    {
        if (bubble.state == DisplayBubble::State::Dead) continue;
        if (bubble.state != DisplayBubble::State::Burst) continue;
        auto *sim = GetBubbleSim(bubble.id);
        if (!sim) continue;

        sim->update(dt);
    }
}

struct RenderOnlyBubble
{
    glm::vec3 basePosition = glm::vec3(0.0f);
    float radius = 0.10f;
    float phase = 0.0f;
    float speed = 0.5f;
    float driftAmplitude = 0.03f;
    float alpha = 0.72f;
};
static std::vector<RenderOnlyBubble> g_RenderOnlyBubbles;
static uint64_t g_NextBubbleId = 1;
static uint64_t g_InteractionOutcomeSeed = 0;
static uint64_t g_ContactOutcomeSequence = 0;
static bool g_InteractionDemoActive = false;
static bool g_ShowMainBubble = true;
static BubbleSurfaceSystem g_BubbleSurfaces;
static glm::vec3 g_BridgeDirection = glm::vec3(0.0f, 0.0f, 1.0f);
static float g_BridgeStrength = 0.0f;
static glm::vec2 g_AutoTouchPoint = glm::vec2(-10.0f, -10.0f);
static float g_AutoTouchStrength = 0.0f;
static float g_AutoTouchVelocity = 0.0f;

static constexpr float kSharedFilmTime = 0.22f;
static constexpr float kNeckFormationTime = 1.15f;
static constexpr float kFusionTime = 3.45f;
static constexpr float kContactVisualTransitionTime = 0.35f;
static constexpr float kFusionFilmThickness = 0.18f;
static constexpr float kFilmRuptureDuration = 0.24f;
static const FusionProfileConfig kFusionProfileConfig = [] {
    FusionProfileConfig config;
    config.surfaceTension = 0.180f;
    config.velocityDamping = 1.00f;
    config.velocityLaplacianDamping = 2.5f;
    config.meshRegularization = 0.10f;
    config.tangentialRedistributionRate = 3.0f;
    config.resamplingEdgeRatio = 2.35f;
    config.resamplingCurvatureWeight = 1.20f;
    config.globalModeFrequency = 2.10f;
    config.globalModeDampingRatio = 0.40f;
    config.volumeConstraintStrength = 0.92f;
    config.volumeProjectionIterations = 4;
    config.initialNeckExpansionSpeed = 0.42f;
    config.maximumTimeStep = 1.0f / 480.0f;
    config.maximumSubsteps = 16;
    config.maximumNodeSpeed = 1.65f;
    return config;
}();
static constexpr float kFusionCompletionHold = 0.30f;
static constexpr float kFusionStableRmsSpeed = 0.010f;
static constexpr float kFusionStableCurvatureVariation = 0.50f;
static constexpr float kShellCutDelay = 0.42f;
static constexpr size_t kDefaultDisplayBubbleCount = 3;
static constexpr size_t kHardMaxDisplayBubbleCount = 16;
static constexpr float kDefaultBubbleSpawnPlaneZ = 1.10f;
static constexpr float kMinBubbleSpawnPlaneZ = 0.25f;
static constexpr float kMaxBubbleSpawnPlaneZ = 2.60f;
static constexpr float kBubbleSpawnPlaneStep = 0.12f;
static constexpr float kMinSpawnRadius = 0.22f;
static constexpr float kMaxSpawnRadius = 0.72f;
static float g_BubbleSpawnPlaneZ = kDefaultBubbleSpawnPlaneZ;
static float g_SpawnRadius = 0.40f;
static size_t g_MaxLiveBubbleCount = 8;
static bool g_WindEnabled = false;
static glm::vec3 g_GlobalWindDirection = glm::normalize(glm::vec3(1.0f, 0.20f, 0.0f));
static float g_GlobalWindStrength = 0.16f;
static constexpr float kMinGlobalWindStrength = 0.0f;
static constexpr float kMaxGlobalWindStrength = 0.45f;
static constexpr float kGlobalWindStrengthStep = 0.025f;
static bool g_LogContactDebug = false;
static bool g_DebugFusionProfile = false;
static bool g_DebugFusionCurvature = false;
static bool g_DebugFusionForces = false;
static int g_FusionRenderDiagnosticMode = 0;
static uint64_t g_FusionHandoffSerial = 0;
static double g_LastFusionHandoffTime = -1.0;
static std::string g_LastFusionHandoffSnapshot;
static bool g_DeleteMode = false;

static GLuint g_BackgroundTexture = 0;
static GLuint g_SceneBehindTexture = 0;
static GLuint g_MainSceneTexture = 0;
static GLuint g_BackFaceTexture = 0;
static GLuint g_BackgroundFBO = 0;
static GLuint g_SceneBehindFBO = 0;
static GLuint g_MainSceneFBO = 0;
static GLuint g_BackFaceFBO = 0;
static GLuint g_DepthRB = 0;
static GLuint g_CubemapTexture = 0;
static GLuint g_ThinFilmLUTTexture = 0;

static constexpr float kFBOOverscan = 1.0f;
static int g_FBOWidth = 0;
static int g_FBOHeight = 0;

static float g_CurrentFPS = 0.0f;
static int g_FPSFrameCount = 0;
static double g_FPSLastUpdate = 0.0;

// Thin-film thickness parameters
static float g_KimLutThickness = 350.0f; // Kim2012 / LUT thickness (nm)
static float g_AiryThickness = 740.0f;   // Belcour Airy thickness (nm)
static float g_ThicknessVar = 160.0f;    // 厚度扰动幅度 (nm)
static double g_Time = 0.0;              // 时间

static float &CurrentThickness()
{
    return (g_IridescenceMode == 2) ? g_AiryThickness : g_KimLutThickness;
}

static void UpdateCamera();
static size_t LiveDisplayBubbleCount();

static float Smooth01(float x)
{
    x = glm::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

static bool IsFiniteDirection(const glm::vec3 &direction)
{
    return std::isfinite(direction.x) &&
           std::isfinite(direction.y) &&
           std::isfinite(direction.z);
}

static void SaveValidationFrame(const std::filesystem::path &path,
                                const std::vector<unsigned char> &rgb,
                                int width,
                                int height)
{
    if (width <= 0 || height <= 0 ||
        rgb.size() != (size_t)width * (size_t)height * 3u)
    {
        return;
    }
    std::ofstream output(path, std::ios::binary);
    output << "P6\n" << width << " " << height << "\n255\n";
    const size_t rowBytes = (size_t)width * 3u;
    for (int y = height - 1; y >= 0; --y)
    {
        output.write(reinterpret_cast<const char *>(rgb.data() +
                                                     (size_t)y * rowBytes),
                     (std::streamsize)rowBytes);
    }
}

static std::pair<float, float> MeasureAxisymmetricSurface(
    const std::vector<Vertex> &vertices,
    int segments,
    int rings)
{
    if (segments <= 0 || rings <= 0 ||
        vertices.size() < (size_t)(segments + 1) * (size_t)(rings + 1))
    {
        return {0.0f, 0.0f};
    }
    float integral = 0.0f;
    float firstMoment = 0.0f;
    for (int ring = 1; ring <= rings; ++ring)
    {
        const glm::vec3 &p0 = vertices[(size_t)(ring - 1) *
                                       (size_t)(segments + 1)].Position;
        const glm::vec3 &p1 = vertices[(size_t)ring *
                                       (size_t)(segments + 1)].Position;
        float r0Sq = p0.y * p0.y + p0.z * p0.z;
        float r1Sq = p1.y * p1.y + p1.z * p1.z;
        float dx = std::abs(p1.x - p0.x);
        float slab = 0.5f * (r0Sq + r1Sq) * dx;
        integral += slab;
        firstMoment += slab * (0.5f * (p0.x + p1.x));
    }
    float volume = 3.14159265358979323846f * integral;
    float centroidX = integral > 1e-8f ? firstMoment / integral : 0.0f;
    return {volume, centroidX};
}

static glm::vec3 SafeNormalizeDirection(const glm::vec3 &direction,
                                        const glm::vec3 &fallback)
{
    float lengthSquared = glm::dot(direction, direction);
    if (!IsFiniteDirection(direction) || lengthSquared <= 1e-10f)
    {
        return fallback;
    }
    return direction / std::sqrt(lengthSquared);
}

static void BuildStableOrthonormalFrame(const glm::vec3 &axis,
                                        glm::vec3 &side,
                                        glm::vec3 &binormal)
{
    if (axis.z < -0.999999f)
    {
        side = glm::vec3(0.0f, -1.0f, 0.0f);
        binormal = glm::vec3(-1.0f, 0.0f, 0.0f);
        return;
    }

    float a = 1.0f / (1.0f + axis.z);
    float b = -axis.x * axis.y * a;
    side = glm::vec3(1.0f - axis.x * axis.x * a, b, -axis.x);
    binormal = glm::vec3(b, 1.0f - axis.y * axis.y * a, -axis.y);
}

static void UpdateTransportedFusionFrame(BubbleContactPair &pair,
                                         const glm::vec3 &newAxis)
{
    glm::vec3 axis = SafeNormalizeDirection(newAxis, pair.fusionAxis);
    if (!pair.fusionFrameInitialized)
    {
        BuildStableOrthonormalFrame(axis, pair.fusionSide, pair.fusionBinormal);
        pair.fusionAxis = axis;
        pair.fusionFrameInitialized = true;
        return;
    }

    glm::vec3 side = pair.fusionSide - axis * glm::dot(pair.fusionSide, axis);
    if (!IsFiniteDirection(side) || glm::dot(side, side) <= 1e-10f)
    {
        side = pair.fusionBinormal - axis * glm::dot(pair.fusionBinormal, axis);
    }
    if (!IsFiniteDirection(side) || glm::dot(side, side) <= 1e-10f)
    {
        BuildStableOrthonormalFrame(axis, side, pair.fusionBinormal);
    }
    else
    {
        side = SafeNormalizeDirection(side, pair.fusionSide);
        pair.fusionBinormal = SafeNormalizeDirection(
            glm::cross(axis, side),
            pair.fusionBinormal);
        side = SafeNormalizeDirection(
            glm::cross(pair.fusionBinormal, axis),
            side);
    }

    pair.fusionAxis = axis;
    pair.fusionSide = side;
}

static float NextContactOutcomeRandom01()
{
    uint64_t x = (++g_ContactOutcomeSequence) * 0x9E3779B97F4A7C15ull ^
                 g_InteractionOutcomeSeed * 0xD6E8FEB86659FD93ull;
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return (float)((x >> 40) & 0xFFFFFFull) / (float)0xFFFFFFull;
}

static const char *CoalescenceOutcomeName(BubbleContactPair::CoalescenceOutcome outcome)
{
    switch (outcome)
    {
    case BubbleContactPair::CoalescenceOutcome::SeparateAfterContact:
        return "separate";
    case BubbleContactPair::CoalescenceOutcome::StayDoubleBubble:
        return "stay-double";
    case BubbleContactPair::CoalescenceOutcome::WillCoalesce:
        return "will-coalesce";
    case BubbleContactPair::CoalescenceOutcome::Undecided:
    default:
        return "undecided";
    }
}

static float FusionSurfaceVisibility(const BubbleContactPair &pair)
{
    // A topology change cannot be alpha-cross-faded for a transparent,
    // refractive shell: overlapping the old and new complete surfaces changes
    // the optical path everywhere.  The replacement surface is identical away
    // from the tiny zippered neck, so switch connectivity atomically instead.
    return pair.fusionActive ? 1.0f : 0.0f;
}

static float BubbleFusionSurfaceVisibility(uint64_t bubbleId)
{
    float visibility = 0.0f;
    for (const BubbleContactPair &pair : g_ContactPairs)
    {
        if (pair.a == bubbleId || pair.b == bubbleId)
        {
            visibility = std::max(visibility, FusionSurfaceVisibility(pair));
        }
    }
    return visibility;
}

static bool PairCanBeginExclusiveFusion(const BubbleContactPair &pair)
{
    for (const BubbleContactPair &other : g_ContactPairs)
    {
        if (&other == &pair ||
            (!other.active && !other.candidate && !other.bonded))
        {
            continue;
        }
        if (other.a == pair.a || other.a == pair.b ||
            other.b == pair.a || other.b == pair.b)
        {
            return false;
        }
    }
    return true;
}

static void SetGlobalWindDirection(const glm::vec3 &direction)
{
    glm::vec3 planar(direction.x, direction.y, 0.0f);
    if (glm::length(planar) < 1e-5f)
    {
        return;
    }
    g_GlobalWindDirection = glm::normalize(planar);
    std::cout << "[BubbleWind] direction=("
              << g_GlobalWindDirection.x << ", "
              << g_GlobalWindDirection.y << ", "
              << g_GlobalWindDirection.z << ")"
              << " enabled=" << (g_WindEnabled ? "on" : "off")
              << " strength=" << g_GlobalWindStrength << std::endl;
}

static void SetGlobalWindStrength(float strength)
{
    g_GlobalWindStrength = glm::clamp(strength, kMinGlobalWindStrength, kMaxGlobalWindStrength);
    std::cout << "[BubbleWind] strength=" << g_GlobalWindStrength
              << " enabled=" << (g_WindEnabled ? "on" : "off")
              << " direction=(" << g_GlobalWindDirection.x << ", "
              << g_GlobalWindDirection.y << ", " << g_GlobalWindDirection.z << ")"
              << std::endl;
}

static void SetMaxLiveBubbleCount(size_t count)
{
    g_MaxLiveBubbleCount = glm::clamp(count,
                                      kDefaultDisplayBubbleCount,
                                      kHardMaxDisplayBubbleCount);
    std::cout << "[BubbleSpawn] maxLive=" << g_MaxLiveBubbleCount
              << " live=" << LiveDisplayBubbleCount() << std::endl;
}

static void SetBubbleSpawnPlaneZ(float z)
{
    g_BubbleSpawnPlaneZ = glm::clamp(z, kMinBubbleSpawnPlaneZ, kMaxBubbleSpawnPlaneZ);
    std::cout << "[BubbleSpawn] planeZ=" << g_BubbleSpawnPlaneZ << std::endl;
}

static uint64_t AllocateBubbleId()
{
    return g_NextBubbleId++;
}

static void UpdatePersistentSurfaceDynamics(float dt)
{
    if (dt <= 0.0f)
    {
        return;
    }

    std::vector<std::vector<glm::vec3>> targets(g_DisplayBubbles.size());
    std::vector<std::vector<float>> contactWeights(g_DisplayBubbles.size());
    for (size_t bubbleIndex = 0; bubbleIndex < g_DisplayBubbles.size(); ++bubbleIndex)
    {
        DisplayBubble &bubble = g_DisplayBubbles[bubbleIndex];
        targets[bubbleIndex].resize(bubble.surfaceControls.size(), glm::vec3(0.0f));
        contactWeights[bubbleIndex].resize(bubble.surfaceControls.size(), 0.0f);
        if (bubble.volumeTransferred)
        {
            continue;
        }
        for (size_t controlIndex = 0; controlIndex < bubble.surfaceControls.size(); ++controlIndex)
        {
            const auto &control = bubble.surfaceControls[controlIndex];
            float freeWave = std::sin((float)g_Time * (0.72f + bubble.speed * 0.12f) +
                                      bubble.phase + (float)controlIndex * 1.17f);
            glm::vec3 target = control.localDir * (freeWave * 0.0035f);

            glm::vec3 windShapeAxis = bubble.velocity;
            if (g_WindEnabled)
            {
                windShapeAxis += g_GlobalWindDirection * (g_GlobalWindStrength * 1.85f);
            }
            float windShapeSpeed = glm::length(windShapeAxis);
            if (windShapeSpeed > 1e-5f)
            {
                windShapeAxis /= windShapeSpeed;
                float radiusResponse = glm::clamp(
                    std::sqrt(0.40f / std::max(bubble.radius, 0.08f)),
                    0.62f, 1.35f);
                float windDrive = g_WindEnabled ? g_GlobalWindStrength : 0.0f;
                float shapeAmplitude = glm::clamp(
                    (windDrive * 0.075f + glm::length(bubble.velocity) * 0.030f) * radiusResponse,
                    0.0f, 0.052f);
                float alignment = glm::dot(control.localDir, windShapeAxis);
                float axialRadial = (alignment * alignment - 0.333f) * shapeAmplitude;
                float windwardMask = Smooth01((-alignment - 0.10f) / 0.90f);
                float leewardMask = Smooth01((alignment - 0.05f) / 0.95f);
                float sideRipple = std::sin((float)g_Time * (1.30f + bubble.speed * 0.18f) +
                                            bubble.phase * 0.73f + (float)controlIndex * 0.91f);
                glm::vec3 sideDir = control.localDir - windShapeAxis * alignment;
                if (glm::length(sideDir) > 1e-5f)
                {
                    sideDir = glm::normalize(sideDir);
                }
                else
                {
                    sideDir = glm::vec3(0.0f);
                }

                target += control.localDir * axialRadial;
                target -= windShapeAxis * (0.026f * shapeAmplitude / 0.052f) * windwardMask;
                target += windShapeAxis * (0.016f * shapeAmplitude / 0.052f) * leewardMask;
                target += sideDir * (sideRipple * shapeAmplitude * 0.18f) *
                          (1.0f - std::abs(alignment));
            }

            targets[bubbleIndex][controlIndex] = target;
        }
    }

    for (const BubbleContactPair &pair : g_ContactPairs)
    {
        int aIndex = -1;
        int bIndex = -1;
        if ((!pair.candidate && !pair.bonded) ||
            !ResolveContactPairIndices(g_DisplayBubbles, pair, aIndex, bIndex))
        {
            continue;
        }

        const DisplayBubble &a = g_DisplayBubbles[(size_t)aIndex];
        const DisplayBubble &b = g_DisplayBubbles[(size_t)bIndex];
        glm::vec3 axis = b.position - a.position;
        float distance = glm::length(axis);
        if (distance <= 1e-5f)
        {
            continue;
        }
        axis /= distance;
        float pairStrength = glm::clamp(std::max(pair.contactActivation,
                                                 pair.bonded ? 0.25f + 0.75f * pair.interactionCompression : 0.0f),
                                        0.0f, 1.0f);
        float radiusRatioA = glm::clamp(pair.contactRadius / std::max(a.radius, 0.001f), 0.0f, 0.90f);
        float radiusRatioB = glm::clamp(pair.contactRadius / std::max(b.radius, 0.001f), 0.0f, 0.90f);

        auto accumulateContact = [&](int bubbleIndex, int otherBubbleIndex,
                                     const glm::vec3 &contactAxis,
                                     float radiusRatio)
        {
            const DisplayBubble &bubble = g_DisplayBubbles[(size_t)bubbleIndex];
            const DisplayBubble &other = g_DisplayBubbles[(size_t)otherBubbleIndex];
            float sizeDeformScale = glm::clamp(
                std::sqrt(std::max(other.radius, 0.001f) / std::max(bubble.radius, 0.001f)),
                0.62f, 1.65f);
            float coneCos = glm::mix(0.90f,
                                     std::sqrt(std::max(1.0f - radiusRatio * radiusRatio, 0.0f)),
                                     pairStrength);
            coneCos = glm::clamp(coneCos, 0.28f, 0.94f);
            for (size_t controlIndex = 0; controlIndex < bubble.surfaceControls.size(); ++controlIndex)
            {
                const glm::vec3 direction = bubble.surfaceControls[controlIndex].localDir;
                float alignment = glm::dot(direction, contactAxis);
                float capMask = Smooth01((alignment - coneCos) /
                                         std::max(1.0f - coneCos, 0.001f));
                float rimMask = std::exp(-std::pow((alignment - (coneCos - 0.08f)) / 0.10f, 2.0f));
                float weight = pairStrength * std::max(capMask, 0.32f * rimMask);
                if (weight <= 0.0001f)
                {
                    continue;
                }

                glm::vec3 radial = direction - contactAxis * alignment;
                if (glm::length(radial) > 1e-5f)
                {
                    radial = glm::normalize(radial);
                }
                glm::vec3 contactTarget =
                    -contactAxis * (0.018f + 0.070f * pairStrength) * capMask * sizeDeformScale +
                    radial * (0.004f + 0.014f * pairStrength) * rimMask *
                        glm::mix(1.0f, sizeDeformScale, 0.45f);
                float oldWeight = contactWeights[(size_t)bubbleIndex][controlIndex];
                float newWeight = oldWeight + weight;
                targets[(size_t)bubbleIndex][controlIndex] =
                    (targets[(size_t)bubbleIndex][controlIndex] * std::max(1.0f - oldWeight, 0.0f) +
                     contactTarget * weight) /
                    std::max(std::max(1.0f - oldWeight, 0.0f) + weight, 0.001f);
                contactWeights[(size_t)bubbleIndex][controlIndex] = glm::clamp(newWeight, 0.0f, 1.0f);
            }
        };

        accumulateContact(aIndex, bIndex, axis, radiusRatioA);
        accumulateContact(bIndex, aIndex, -axis, radiusRatioB);
    }

    // Match the material velocity of the two surface caps. This is a low-cost
    // analogue of multi-MELP's matching-velocity/contact coupling condition.
    for (const BubbleContactPair &pair : g_ContactPairs)
    {
        int aIndex = -1;
        int bIndex = -1;
        if ((!pair.candidate && !pair.bonded) ||
            !ResolveContactPairIndices(g_DisplayBubbles, pair, aIndex, bIndex))
        {
            continue;
        }
        DisplayBubble &a = g_DisplayBubbles[(size_t)aIndex];
        DisplayBubble &b = g_DisplayBubbles[(size_t)bIndex];
        float coupling = glm::clamp(pair.contactActivation, 0.0f, 1.0f);
        if (coupling <= 0.001f)
        {
            continue;
        }

        glm::vec3 pairAxis = b.position - a.position;
        float pairDistance = glm::length(pairAxis);
        if (pairDistance <= 1e-5f)
        {
            continue;
        }
        pairAxis /= pairDistance;
        auto pairControlWeight = [&](const DisplayBubble &bubble,
                                     const DisplayBubble::SurfaceControl &control,
                                     const glm::vec3 &contactAxis)
        {
            float radiusRatio = glm::clamp(pair.contactRadius /
                                               std::max(bubble.radius, 0.001f),
                                           0.0f, 0.90f);
            float coneCos = glm::mix(0.90f,
                                     std::sqrt(std::max(1.0f - radiusRatio * radiusRatio, 0.0f)),
                                     coupling);
            coneCos = glm::clamp(coneCos, 0.28f, 0.94f);
            float alignment = glm::dot(control.localDir, contactAxis);
            return Smooth01((alignment - coneCos) /
                            std::max(1.0f - coneCos, 0.001f));
        };

        glm::vec3 averageA(0.0f), averageB(0.0f);
        float weightA = 0.0f, weightB = 0.0f;
        for (size_t i = 0; i < a.surfaceControls.size(); ++i)
        {
            float w = pairControlWeight(a, a.surfaceControls[i], pairAxis);
            averageA += (a.velocity + a.surfaceControls[i].velocity * a.radius) * w;
            weightA += w;
        }
        for (size_t i = 0; i < b.surfaceControls.size(); ++i)
        {
            float w = pairControlWeight(b, b.surfaceControls[i], -pairAxis);
            averageB += (b.velocity + b.surfaceControls[i].velocity * b.radius) * w;
            weightB += w;
        }
        averageA = weightA > 1e-5f ? averageA / weightA : a.velocity;
        averageB = weightB > 1e-5f ? averageB / weightB : b.velocity;
        float invMassA = BubbleInvMass(a);
        float invMassB = BubbleInvMass(b);
        glm::vec3 sharedVelocity = (averageA * invMassB + averageB * invMassA) /
                                   std::max(invMassA + invMassB, 0.001f);
        float velocityBlend = 1.0f - std::exp(-dt * glm::mix(6.0f, 32.0f, coupling));

        for (size_t i = 0; i < a.surfaceControls.size(); ++i)
        {
            float w = pairControlWeight(a, a.surfaceControls[i], pairAxis) * coupling;
            glm::vec3 desired = (sharedVelocity - a.velocity) / std::max(a.radius, 0.001f);
            a.surfaceControls[i].velocity = glm::mix(a.surfaceControls[i].velocity,
                                                     desired, velocityBlend * w);
        }
        for (size_t i = 0; i < b.surfaceControls.size(); ++i)
        {
            float w = pairControlWeight(b, b.surfaceControls[i], -pairAxis) * coupling;
            glm::vec3 desired = (sharedVelocity - b.velocity) / std::max(b.radius, 0.001f);
            b.surfaceControls[i].velocity = glm::mix(b.surfaceControls[i].velocity,
                                                     desired, velocityBlend * w);
        }
    }

    for (size_t bubbleIndex = 0; bubbleIndex < g_DisplayBubbles.size(); ++bubbleIndex)
    {
        DisplayBubble &bubble = g_DisplayBubbles[bubbleIndex];
        if (bubble.state == DisplayBubble::State::Dead || bubble.volumeTransferred)
        {
            continue;
        }
        for (size_t controlIndex = 0; controlIndex < bubble.surfaceControls.size(); ++controlIndex)
        {
            auto &control = bubble.surfaceControls[controlIndex];
            float contactWeight = contactWeights[bubbleIndex][controlIndex];
            float stiffness = glm::mix(14.0f, 48.0f, contactWeight);
            float damping = glm::mix(5.0f, 7.2f, contactWeight);
            glm::vec3 acceleration =
                (targets[bubbleIndex][controlIndex] - control.displacement) * stiffness -
                control.velocity * damping;
            control.velocity += acceleration * dt;
            control.displacement += control.velocity * dt;
            float displacementLength = glm::length(control.displacement);
            if (displacementLength > 0.115f)
            {
                control.displacement *= 0.115f / displacementLength;
                control.velocity *= 0.65f;
            }
        }
    }
}

static float ContactFilmWorldCurvature(const DisplayBubble &a,
                                       const DisplayBubble &b,
                                       const BubbleContactPair &pair)
{
    float radiusA = std::max(a.radius, 0.001f);
    float radiusB = std::max(b.radius, 0.001f);
    // Young-Laplace pressure gives k_partition = 1/rA - 1/rB for a
    // normal directed from A to B. The sign bends the film toward the
    // larger, lower-pressure bubble. Equal radii naturally give a plane.
    float mismatch = std::abs(radiusA - radiusB) / std::max(std::max(radiusA, radiusB), 0.001f);
    float curvature = (1.0f / radiusA - 1.0f / radiusB) * (1.0f + mismatch * 0.85f);
    float filmRadius = std::max(pair.contactRadius, 0.002f);
    float maximumCurvature = 0.85f / filmRadius;
    return glm::clamp(curvature, -maximumCurvature, maximumCurvature);
}

static void DecideCoalescenceOutcome(BubbleContactPair &pair,
                                     const DisplayBubble &a,
                                     const DisplayBubble &b,
                                     float relNormalSpeed)
{
    if (pair.outcome != BubbleContactPair::CoalescenceOutcome::Undecided ||
        pair.contactTime < 0.85f ||
        pair.contactRadius <= 0.001f)
    {
        return;
    }

    float minRadius = std::max(std::min(a.radius, b.radius), 0.001f);
    float maxRadius = std::max(std::max(a.radius, b.radius), minRadius);
    float sizeRatio = glm::clamp(minRadius / maxRadius, 0.0f, 1.0f);
    float sizeMismatch = 1.0f - sizeRatio;
    float contactAge = Smooth01((pair.contactTime - 0.65f) / 1.35f);
    float thinFilm = Smooth01((0.42f - pair.filmThickness) / 0.26f);
    float impact = Smooth01(std::max(-relNormalSpeed, 0.0f) / 0.52f);
    float contactSize = Smooth01((pair.contactRadius / minRadius - 0.18f) / 0.46f);
    float windInstability = g_WindEnabled
                                ? Smooth01(g_GlobalWindStrength /
                                           std::max(kMaxGlobalWindStrength, 0.001f))
                                : 0.0f;

    float separationProbability = glm::clamp(
        0.005f +
            (1.0f - contactSize) * 0.010f +
            impact * 0.005f +
            windInstability * 0.0025f,
        0.005f, 0.025f);
    float sizeMismatchGate = Smooth01((sizeMismatch - 0.06f) / 0.30f);
    float fusionProbability = glm::clamp(
        0.005f + sizeMismatchGate *
            (0.76f +
             thinFilm * 0.07f +
             impact * 0.04f +
             contactAge * 0.03f),
        0.005f, 0.82f);
    float probabilitySum = separationProbability + fusionProbability;
    if (probabilitySum > 0.92f)
    {
        float scale = 0.92f / probabilitySum;
        separationProbability *= scale;
        fusionProbability *= scale;
    }
    pair.coalescenceScore = fusionProbability;
    if (!pair.outcomeRandomSampled)
    {
        pair.coalescenceRandom = NextContactOutcomeRandom01();
        pair.outcomeRandomSampled = true;
    }
    if (pair.coalescenceRandom < separationProbability)
    {
        pair.outcome = BubbleContactPair::CoalescenceOutcome::SeparateAfterContact;
        pair.persistentRenderPair = true;
    }
    else if (pair.coalescenceRandom < separationProbability + fusionProbability)
    {
        pair.outcome = BubbleContactPair::CoalescenceOutcome::WillCoalesce;
    }
    else
    {
        pair.outcome = BubbleContactPair::CoalescenceOutcome::StayDoubleBubble;
    }

    if (g_LogContactDebug)
    {
        std::cout << "[BubbleCoalescence] pair=(" << pair.a << ", " << pair.b << ")"
                  << " outcome=" << CoalescenceOutcomeName(pair.outcome)
                  << " score=" << pair.coalescenceScore
                  << " random=" << pair.coalescenceRandom
                  << " sceneSeed=" << g_InteractionOutcomeSeed
                  << " separate=" << separationProbability
                  << " sizeRatio=" << sizeRatio
                  << " mismatch=" << sizeMismatch
                  << " film=" << pair.filmThickness
                  << std::endl;
    }
}

static float PlateauContactRadius(const BubbleContactPair &pair, const DisplayBubble &a, const DisplayBubble &b)
{
    float stableRadius = std::min(a.radius, b.radius) * 0.44f;
    float formingRadius = stableRadius * 0.08f;
    float relaxedRadius = glm::mix(formingRadius, stableRadius, Smooth01(pair.geometryBlend));
    float breathing = 1.0f + std::sin((float)g_Time * 1.15f + (float)(pair.a + pair.b) * 0.71f) * 0.025f * pair.geometryBlend;
    return relaxedRadius * breathing;
}

static void RebuildDisplayBubbleModels()
{
    g_BubbleSurfaces.Clear();
    for (const DisplayBubble &bubble : g_DisplayBubbles)
    {
        std::vector<Vertex> vertices = BuildBubbleShellVertices(1.0f, 0.0f, false);
        for (Vertex &vertex : vertices)
            vertex.FilmDirection = glm::normalize(vertex.Position);
        g_BubbleSurfaces.EnsureBubble(bubble.id, vertices,
                                      BuildContactBubblePatchIndices());
    }
}

static Model *FindDisplayBubbleModel(uint64_t bubbleId)
{
    return g_BubbleSurfaces.FindBubble(bubbleId);
}

static uint64_t AddBubble(const glm::vec3 &position,
                          float radius,
                          const glm::vec3 &velocity,
                          const glm::vec3 &basePosition,
                          float phase,
                          float windAmplitude,
                          float floatAmplitude,
                          float speed,
                          bool rebuildModels = true)
{
    DisplayBubble bubble{};
    bubble.id = AllocateBubbleId();
    bubble.basePosition = basePosition;
    bubble.position = position;
    bubble.velocity = velocity;
    bubble.radius = radius;
    bubble.initialRadius = radius;
    bubble.targetVolume = BubbleVolume(radius);
    bubble.phase = phase;
    bubble.windAmplitude = windAmplitude;
    bubble.floatAmplitude = floatAmplitude;
    bubble.speed = speed;
    bubble.alpha = 1.0f;
    bubble.contactTime = 0.0f;
    bubble.mergeProgress = 0.0f;
    bubble.filmThickness = 1.0f;
    bubble.contactStrength = 0.0f;
    bubble.surfaceDynamicsBlend = 1.0f;
    bubble.contactAxis = glm::vec3(1.0f, 0.0f, 0.0f);
    bubble.volumeTransferred = false;
    bubble.state = DisplayBubble::State::Free;
    bubble.surfaceControls = MakeSurfaceControls(phase);
    bubble.visualState.initialized = true;
    bubble.visualState.interferencePhase = phase;
    bubble.visualState.noiseSeed =
        static_cast<uint32_t>((bubble.id * 2654435761ull) & 0xffffffffu);
    bubble.visualState.animationTimeOrigin = static_cast<float>(g_Time);
    bubble.visualState.textureOriginWorld = position;
    bubble.visualState.textureBasis = glm::mat3(1.0f);
    bubble.visualState.referenceRadius = std::max(radius, 0.001f);
    bubble.visualState.opacity = bubble.alpha;

    uint64_t id = bubble.id;
    g_DisplayBubbles.push_back(bubble);
    // Create a DBSTT sim for burst physics (activated only on burst).
    InitBubbleSim(id, radius);
    if (rebuildModels && g_DecorativeBubbleModel)
    {
        std::vector<Vertex> vertices = BuildBubbleShellVertices(1.0f, 0.0f, false);
        for (Vertex &vertex : vertices)
        {
            vertex.FilmDirection = glm::normalize(vertex.Position);
        }
        g_BubbleSurfaces.EnsureBubble(id, vertices,
                                      BuildContactBubblePatchIndices());
    }
    return id;
}

static uint64_t AddBubble(const glm::vec3 &position,
                          float radius,
                          const glm::vec3 &velocity)
{
    float phase = (float)g_NextBubbleId * 1.37f;
    return AddBubble(position, radius, velocity, position, phase,
                     0.010f, 0.009f, 0.56f, true);
}

static bool RemoveBubble(uint64_t id)
{
    int bubbleIndex = FindBubbleIndexById(g_DisplayBubbles, id);
    if (bubbleIndex < 0)
    {
        return false;
    }

    g_DisplayBubbles.erase(g_DisplayBubbles.begin() + bubbleIndex);
    g_ContactPairs.erase(
        std::remove_if(g_ContactPairs.begin(), g_ContactPairs.end(),
                       [id](const BubbleContactPair &pair)
                       {
                           return pair.a == id || pair.b == id;
                       }),
        g_ContactPairs.end());

    g_BubbleSurfaces.RemoveBubble(id);
    g_BubbleSims.erase(id);
    return true;
}

static void FinalizeCompletedFusions()
{
    struct CompletedFusion
    {
        uint64_t a = 0;
        uint64_t b = 0;
        glm::vec3 center = glm::vec3(0.0f);
        glm::vec3 velocity = glm::vec3(0.0f);
        float conservedVolume = 0.0f;
        float theoreticalVolume = 0.0f;
        int zeroCrossings = 0;
        float maximumRelativeVolumeError = 0.0f;
        float maximumCentroidError = 0.0f;
        float finalShapeMode = 0.0f;
        float finalRmsSpeed = 0.0f;
        float finalCurvatureVariation = 0.0f;
        float initialArea = 0.0f;
        float finalArea = 0.0f;
        float fusionElapsed = 0.0f;
        BubbleVisualState visualState;
    };
    std::vector<CompletedFusion> completedPairs;
    for (const BubbleContactPair &pair : g_ContactPairs)
    {
        if (pair.fusionComplete)
        {
            CompletedFusion completed;
            completed.a = pair.a;
            completed.b = pair.b;
            completed.center = pair.fusionProfileInitialized
                ? pair.fusionCenter +
                      pair.fusionCenterVelocity * pair.fusionElapsed
                : glm::vec3(0.0f);
            completed.velocity = pair.fusionCenterVelocity;
            completed.conservedVolume = pair.conservedFusionVolume;
            completed.theoreticalVolume =
                pair.theoreticalUncompressedVolume;
            completed.zeroCrossings = pair.fusionShapeZeroCrossings;
            completed.maximumRelativeVolumeError =
                pair.maximumRelativeVolumeError;
            completed.maximumCentroidError = pair.maximumCentroidError;
            completed.finalShapeMode = pair.fusionShapeMode;
            completed.finalRmsSpeed = pair.fusionRmsSpeed;
            completed.finalCurvatureVariation =
                pair.fusionCurvatureVariation;
            completed.initialArea = pair.initialFusionArea;
            completed.finalArea = pair.currentFusionArea;
            completed.fusionElapsed = pair.fusionElapsed;
            completed.visualState = pair.visualState;
            completedPairs.push_back(completed);
        }
    }

    for (const CompletedFusion &completed : completedPairs)
    {
        int survivorIndex = FindBubbleIndexById(g_DisplayBubbles, completed.a);
        int absorbedIndex = FindBubbleIndexById(g_DisplayBubbles, completed.b);
        if (survivorIndex < 0 || absorbedIndex < 0)
        {
            continue;
        }

        DisplayBubble &survivor = g_DisplayBubbles[(size_t)survivorIndex];
        const DisplayBubble &absorbed = g_DisplayBubbles[(size_t)absorbedIndex];
        float volumeA = survivor.targetVolume > 0.0f
                            ? survivor.targetVolume
                            : BubbleVolume(survivor.radius);
        float volumeB = absorbed.targetVolume > 0.0f
                            ? absorbed.targetVolume
                            : BubbleVolume(absorbed.radius);
        float mergedVolume =
            std::max(completed.conservedVolume, 0.001f);
        glm::vec3 mergedCenter = completed.center;
        glm::vec3 mergedVelocity = completed.velocity;
        if (!std::isfinite(mergedCenter.x) || !std::isfinite(mergedCenter.y) ||
            !std::isfinite(mergedCenter.z))
        {
            mergedCenter =
                (survivor.position * volumeA + absorbed.position * volumeB) /
                mergedVolume;
        }
        float mergedRadius = RadiusFromVolume(mergedVolume);

        Model *beforeModel = g_BubbleSurfaces.FindFusion(
            completed.a, completed.b);
        Mesh *beforeMesh = beforeModel ? beforeModel->getMesh(0) : nullptr;
        unsigned int beforeVao = beforeMesh ? beforeMesh->VAO : 0u;
        const void *beforeModelAddress = beforeModel;
        float beforeOpticalBlend =
            completed.visualState.opticalNormalBlendAtOrigin *
            std::exp(-completed.visualState.opticalNormalRelaxationRate *
                     std::max(static_cast<float>(g_Time) -
                                  completed.visualState.animationTimeOrigin,
                              0.0f));

        if (!g_BubbleSurfaces.PromoteFusion(completed.a, completed.b,
                                            survivor.id, absorbed.id))
        {
            continue;
        }

        survivor.position = mergedCenter;
        survivor.basePosition = mergedCenter;
        survivor.velocity = mergedVelocity;
        survivor.radius = mergedRadius;
        survivor.initialRadius = mergedRadius;
        survivor.targetVolume = mergedVolume;
        survivor.alpha = std::max(survivor.alpha, absorbed.alpha);
        survivor.contactTime = 0.0f;
        survivor.mergeProgress = 1.0f;
        survivor.filmThickness = std::min(survivor.filmThickness,
                                          absorbed.filmThickness);
        survivor.contactStrength = 0.0f;
        survivor.surfaceDynamicsBlend = 0.0f;
        survivor.volumeTransferred = true;
        survivor.state = DisplayBubble::State::Merged;
        survivor.visualState = completed.visualState;

        Model *afterModel = g_BubbleSurfaces.FindBubble(survivor.id);
        Mesh *afterMesh = afterModel ? afterModel->getMesh(0) : nullptr;
        unsigned int afterVao = afterMesh ? afterMesh->VAO : 0u;
        float afterOpticalBlend =
            survivor.visualState.opticalNormalBlendAtOrigin *
            std::exp(-survivor.visualState.opticalNormalRelaxationRate *
                     std::max(static_cast<float>(g_Time) -
                                  survivor.visualState.animationTimeOrigin,
                              0.0f));

        auto describeMesh = [](const Mesh *mesh) {
            std::ostringstream stream;
            if (!mesh || mesh->vertices.empty()) {
                stream << "mesh_vertices=0\n";
                return stream.str();
            }
            glm::vec2 uvMin(std::numeric_limits<float>::max());
            glm::vec2 uvMax(-std::numeric_limits<float>::max());
            float normalMin = std::numeric_limits<float>::max();
            float normalMax = 0.0f;
            float normalMean = 0.0f;
            for (const Vertex &vertex : mesh->vertices) {
                uvMin = glm::min(uvMin, vertex.TexCoords);
                uvMax = glm::max(uvMax, vertex.TexCoords);
                float length = glm::length(vertex.Normal);
                normalMin = std::min(normalMin, length);
                normalMax = std::max(normalMax, length);
                normalMean += length;
            }
            normalMean /= static_cast<float>(mesh->vertices.size());
            int inwardFaces = 0;
            int validFaces = 0;
            for (std::size_t i = 0; i + 2 < mesh->indices.size(); i += 3) {
                const glm::vec3 &a = mesh->vertices[mesh->indices[i]].Position;
                const glm::vec3 &b = mesh->vertices[mesh->indices[i + 1]].Position;
                const glm::vec3 &c = mesh->vertices[mesh->indices[i + 2]].Position;
                glm::vec3 face = glm::cross(b - a, c - a);
                if (glm::length(face) <= 1e-10f)
                    continue;
                ++validFaces;
                if (glm::dot(face, (a + b + c) / 3.0f) < 0.0f)
                    ++inwardFaces;
            }
            stream << "mesh_vertices=" << mesh->vertices.size() << "\n"
                   << "mesh_indices=" << mesh->indices.size() << "\n"
                   << "uv_min=" << uvMin.x << "," << uvMin.y << "\n"
                   << "uv_max=" << uvMax.x << "," << uvMax.y << "\n"
                   << "normal_length_min=" << normalMin << "\n"
                   << "normal_length_max=" << normalMax << "\n"
                   << "normal_length_mean=" << normalMean << "\n"
                   << "valid_faces=" << validFaces << "\n"
                   << "inward_faces=" << inwardFaces << "\n";
            return stream.str();
        };
        std::ostringstream handoffSnapshot;
        auto writeVisualState = [&](const char *prefix,
                                    const BubbleVisualState &state,
                                    float opticalBlend) {
            handoffSnapshot
                << prefix << "film_base_scale="
                << state.filmBaseThicknessScale << "\n"
                << prefix << "film_amplitude_scale="
                << state.filmThicknessAmplitudeScale << "\n"
                << prefix << "interference_phase="
                << state.interferencePhase << "\n"
                << prefix << "noise_seed=" << state.noiseSeed << "\n"
                << prefix << "animation_time_origin="
                << state.animationTimeOrigin << "\n"
                << prefix << "texture_origin="
                << state.textureOriginWorld.x << ","
                << state.textureOriginWorld.y << ","
                << state.textureOriginWorld.z << "\n"
                << prefix << "texture_basis_x="
                << state.textureBasis[0].x << ","
                << state.textureBasis[0].y << ","
                << state.textureBasis[0].z << "\n"
                << prefix << "texture_basis_y="
                << state.textureBasis[1].x << ","
                << state.textureBasis[1].y << ","
                << state.textureBasis[1].z << "\n"
                << prefix << "texture_basis_z="
                << state.textureBasis[2].x << ","
                << state.textureBasis[2].y << ","
                << state.textureBasis[2].z << "\n"
                << prefix << "reference_radius="
                << state.referenceRadius << "\n"
                << prefix << "ior=" << state.ior << "\n"
                << prefix << "fresnel_strength="
                << state.fresnelStrength << "\n"
                << prefix << "opacity=" << state.opacity << "\n"
                << prefix << "optical_normal_blend="
                << opticalBlend << "\n";
        };
        handoffSnapshot
            << "completion_global_time=" << g_Time << "\n"
            << "completion_fusion_elapsed=" << completed.fusionElapsed << "\n"
            << "shader_program="
            << (g_RefractionShader ? g_RefractionShader->m_id : 0u) << "\n"
            << "material_id=0 (shared uniform-driven material)\n"
            << "base_color=none (spectral thin-film shader)\n"
            << "fresnel_power=" << g_FresnelPower << "\n"
            << "shader_thickness_nm=" << CurrentThickness() << "\n"
            << "shader_thickness_variation_nm=" << g_ThicknessVar << "\n"
            << "environment_cubemap=" << g_CubemapTexture << "\n"
            << "before_type=StatefulFusion\n"
            << "before_model_ptr=" << beforeModelAddress << "\n"
            << "before_mesh_vao=" << beforeVao << "\n"
            << "before_center=" << completed.center.x << ","
            << completed.center.y << "," << completed.center.z << "\n"
            << "before_radius=" << mergedRadius << "\n"
            << "before_model_matrix=translation(center)\n"
            << "before_alpha=" << std::max(survivor.alpha, absorbed.alpha)
            << "\n"
            << "before_render_order=after ordinary DisplayBubble objects\n"
            << "after_type=PromotedDisplayBubble\n"
            << "after_model_ptr=" << static_cast<const void *>(afterModel) << "\n"
            << "after_mesh_vao=" << afterVao << "\n"
            << "after_center=" << survivor.position.x << ","
            << survivor.position.y << "," << survivor.position.z << "\n"
            << "after_radius=" << survivor.radius << "\n"
            << "after_model_matrix=translation(center)\n"
            << "after_alpha=" << survivor.alpha << "\n"
            << "after_render_order=promoted-last after ordinary DisplayBubble objects\n"
            << "same_model=" << (beforeModelAddress == afterModel) << "\n"
            << "same_mesh_vao=" << (beforeVao == afterVao) << "\n";
        writeVisualState("before_", completed.visualState,
                         beforeOpticalBlend);
        writeVisualState("after_", survivor.visualState,
                         afterOpticalBlend);
        handoffSnapshot << describeMesh(afterMesh);
        g_LastFusionHandoffSnapshot = handoffSnapshot.str();
        ++g_FusionHandoffSerial;
        g_LastFusionHandoffTime = g_Time;

        std::cout << "[FusionHandoff] serial=" << g_FusionHandoffSerial
                  << " globalTime=" << g_Time
                  << " fusionElapsed=" << completed.fusionElapsed
                  << " before={type:StatefulFusion,model:" << beforeModelAddress
                  << ",vao:" << beforeVao
                  << ",material:0,shader:"
                  << (g_RefractionShader ? g_RefractionShader->m_id : 0u)
                  << ",center:(" << completed.center.x << ","
                  << completed.center.y << "," << completed.center.z << ")"
                  << ",radius:" << mergedRadius
                  << ",opticalBlend:" << beforeOpticalBlend << "}"
                  << " after={type:PromotedDisplayBubble,model:"
                  << static_cast<const void *>(afterModel)
                  << ",vao:" << afterVao
                  << ",material:0,shader:"
                  << (g_RefractionShader ? g_RefractionShader->m_id : 0u)
                  << ",center:(" << survivor.position.x << ","
                  << survivor.position.y << "," << survivor.position.z << ")"
                  << ",radius:" << survivor.radius
                  << ",opticalBlend:" << afterOpticalBlend << "}"
                  << " sameModel=" << (beforeModelAddress == afterModel)
                  << " sameVAO=" << (beforeVao == afterVao)
                  << " visualSeed=" << survivor.visualState.noiseSeed
                  << " visualTimeOrigin="
                  << survivor.visualState.animationTimeOrigin
                  << std::endl;

        std::cout << "[BubbleFusion] pair=(" << completed.a << ", " << completed.b
                  << ") survivor=" << survivor.id
                  << " radius=" << mergedRadius
                  << " volumeBefore=" << mergedVolume
                  << " volumeAfter=" << BubbleVolume(mergedRadius)
                  << " maxVolumeError="
                  << completed.maximumRelativeVolumeError * 100.0f << "%"
                  << " maxCentroidError=" << completed.maximumCentroidError
                  << " zeroCrossings=" << completed.zeroCrossings
                  << " theoreticalVolume=" << completed.theoreticalVolume
                  << " contactCompression="
                  << (1.0f - mergedVolume /
                                  std::max(completed.theoreticalVolume,
                                           1e-7f)) *
                         100.0f
                  << "%"
                  << " areaDrop="
                  << completed.initialArea - completed.finalArea
                  << " finalShapeMode=" << completed.finalShapeMode
                  << " finalRmsSpeed=" << completed.finalRmsSpeed
                  << " finalCurvatureVariation="
                  << completed.finalCurvatureVariation
                  << std::endl;
        RemoveBubble(completed.b);
    }
}

static size_t LiveDisplayBubbleCount()
{
    size_t count = 0;
    for (const auto &bubble : g_DisplayBubbles)
    {
        if (bubble.state != DisplayBubble::State::Dead)
        {
            ++count;
        }
    }
    return count;
}

static void TriggerBubbleBurst(uint64_t bubbleId)
{
    int index = FindBubbleIndexById(g_DisplayBubbles, bubbleId);
    if (index < 0)
    {
        return;
    }
    DisplayBubble &bubble = g_DisplayBubbles[(size_t)index];
    if (bubble.state == DisplayBubble::State::Burst ||
        bubble.state == DisplayBubble::State::Dead)
    {
        return;
    }

    bubble.state = DisplayBubble::State::Burst;
    bubble.burstElapsed = 0.0f;
    bubble.burstDuration = 0.35f;
    bubble.burstScale = 1.0f;
    bubble.contactStrength = 0.0f;
    bubble.surfaceDynamicsBlend = 0.0f;

    // Re-initialise the sim at the bubble's actual radius so the initial
    // shape matches what the user sees, then boost surface tension so the
    // membrane retracts inward under real DBSTT physics.
    auto *sim = GetBubbleSim(bubbleId);
    if (sim)
    {
        // Re-init at actual radius, then punch a hole toward the camera so
        // the membrane has a free edge to retract from under surface tension.
        sim->initIcosphere(bubble.radius, kDisplayBubbleSimSubdivs, 0.04f);
        glm::vec3 holeDir = glm::normalize(g_Camera.getPosition() - bubble.position);
        sim->punchHole(holeDir, 0.45f);  // ~26 degree opening
        sim->surfaceTensionStrength = 120.0f;
        sim->substepsPerFrame = 12;
        sim->timeStep = 0.002f;
        sim->circulationDiffusion = 0.0001f;
        sim->diagnosticsEnabled = false;

        // Replace the render mesh with the (holed) sim mesh so physics
        // drives the retraction shape during the burst.
        Model *shellModel = FindDisplayBubbleModel(bubbleId);
        if (shellModel)
        {
            Mesh *mesh = shellModel->getMesh(0);
            if (mesh)
            {
                auto [sv, si] = BuildSimRenderMesh(*sim);
                mesh->updateGeometry(sv, si);
            }
        }
    }
    else
    {
        // Fallback: no sim available — use manual shrink.
        bubble.burstScale = 1.0f;
    }

    // Deactivate contact pairs involving this bubble so the contact
    // logic does not fight a dying, expanding bubble.
    for (auto &pair : g_ContactPairs)
    {
        if (pair.a == bubbleId || pair.b == bubbleId)
        {
            pair.bonded = false;
            pair.candidate = false;
            pair.active = false;
            pair.contactActivation = 0.0f;
            pair.bridgeStrength = 0.0f;
            // Critical: clear fusion state so the fusion surface stops
            // rendering and the bursting shell becomes visible.
            pair.fusionActive = false;
            pair.fusionComplete = false;
            pair.persistentRenderPair = false;
            pair.contactRadius = 0.0f;
            pair.contactRadiusVelocity = 0.0f;
            pair.geometryBlend = 0.0f;
            pair.interactionCompression = 0.0f;
        }
    }

    // Apply a ripple impulse to nearby bubbles, mimicking the shockwave
    // that the DBSTT paper gets for free from the vortex-sheet solver.
    glm::vec3 burstCenter = bubble.position;
    float burstRadius = bubble.radius;
    float impulseRange = burstRadius * 4.0f;
    for (auto &other : g_DisplayBubbles)
    {
        if (other.id == bubbleId ||
            other.state == DisplayBubble::State::Burst ||
            other.state == DisplayBubble::State::Dead)
        {
            continue;
        }
        glm::vec3 delta = other.position - burstCenter;
        float dist = glm::length(delta);
        if (dist < 1e-4f || dist > impulseRange)
        {
            continue;
        }
        float falloff = 1.0f - dist / impulseRange;
        // Outward kick from shockwave + expansion for contact partners
        // (their shared film just vanished, so internal pressure releases).
        glm::vec3 impulse = glm::normalize(delta) * (0.8f * falloff);
        other.velocity += impulse;
        // Surface jolt for visible ripple.
        other.contactStrength = std::max(other.contactStrength, 0.85f * falloff);

        // If this neighbour was in contact with the bursting bubble, it
        // should expand — the shared membrane that constrained it is gone.
        for (const auto &pair : g_ContactPairs)
        {
            if ((pair.a == bubbleId && pair.b == other.id) ||
                (pair.b == bubbleId && pair.a == other.id))
            {
                float expandBoost = 1.15f * falloff;
                other.radius += expandBoost * 0.04f;
                other.contactStrength = std::max(other.contactStrength, 1.0f);
                break;
            }
        }
    }

    g_InteractionDemoActive = false;
    g_ShowMainBubble = false;
    std::cout << "[BubbleBurst] id=" << bubbleId << std::endl;
}

struct BubbleSpawnRequest
{
    glm::vec3 position = glm::vec3(0.0f, 0.0f, kDefaultBubbleSpawnPlaneZ);
    glm::vec3 basePosition = glm::vec3(0.0f, 0.0f, kDefaultBubbleSpawnPlaneZ);
    glm::vec3 initialVelocity = glm::vec3(0.0f);
    float radius = 0.40f;
    float shapePerturbation = 1.0f;
    float filmThickness = 1.0f;
    float phase = 0.0f;
    float windAmplitude = 0.010f;
    float floatAmplitude = 0.009f;
    float speed = 0.56f;
};

static uint64_t SpawnBubble(const BubbleSpawnRequest &request)
{
    if (LiveDisplayBubbleCount() >= g_MaxLiveBubbleCount)
    {
        std::cout << "[BubbleSpawn] maximum live bubbles reached: "
                  << g_MaxLiveBubbleCount
                  << " (hard cap " << kHardMaxDisplayBubbleCount << ")"
                  << std::endl;
        return 0;
    }

    float radius = glm::clamp(request.radius, kMinSpawnRadius, kMaxSpawnRadius);
    g_InteractionDemoActive = false;
    g_ShowMainBubble = false;

    uint64_t id = AddBubble(request.position,
                            radius,
                            request.initialVelocity,
                            request.basePosition,
                            request.phase,
                            request.windAmplitude,
                            request.floatAmplitude,
                            request.speed,
                            true);
    int bubbleIndex = FindBubbleIndexById(g_DisplayBubbles, id);
    if (bubbleIndex >= 0)
    {
        DisplayBubble &bubble = g_DisplayBubbles[(size_t)bubbleIndex];
        bubble.filmThickness = glm::clamp(request.filmThickness, 0.2f, 1.6f);
        float perturbation = glm::clamp(request.shapePerturbation, 0.0f, 2.0f);
        for (auto &control : bubble.surfaceControls)
        {
            control.displacement *= perturbation;
        }
    }

    std::cout << "[BubbleSpawn] add id=" << id
              << " live=" << LiveDisplayBubbleCount()
              << " radius=" << radius
              << " position=(" << request.position.x << ", "
              << request.position.y << ", " << request.position.z << ")"
              << std::endl;
    return id;
}

static bool BubblePlanePointFromScreen(double screenX, double screenY, glm::vec3 &worldPoint)
{
    UpdateCamera();
    glm::vec3 origin = g_Camera.getPosition();
    glm::vec3 direction = g_Camera.getRayDirectionFromScreen((float)screenX, (float)screenY);
    if (std::abs(direction.z) < 1e-5f)
    {
        return false;
    }

    float t = (g_BubbleSpawnPlaneZ - origin.z) / direction.z;
    if (t <= 0.0f)
    {
        return false;
    }
    worldPoint = origin + direction * t;
    return true;
}

static uint64_t SpawnBubbleAtScreenPoint(double screenX, double screenY)
{
    glm::vec3 position;
    if (!BubblePlanePointFromScreen(screenX, screenY, position))
    {
        std::cout << "[BubbleSpawn] screen ray did not hit activity plane" << std::endl;
        return 0;
    }

    float spawnIndex = (float)std::max<uint64_t>(g_NextBubbleId, 1);
    glm::vec3 windDir = glm::normalize(glm::vec3(0.75f, 0.22f, 0.0f));
    glm::vec3 tangent(-windDir.y, windDir.x, 0.0f);
    tangent = glm::normalize(tangent);

    BubbleSpawnRequest request;
    request.position = position;
    request.basePosition = position;
    request.initialVelocity =
        windDir * (0.070f + 0.020f * std::sin(spawnIndex * 0.51f)) +
        tangent * (0.020f * std::cos(spawnIndex * 0.83f));
    request.radius = g_SpawnRadius;
    request.phase = spawnIndex * 1.37f;
    request.windAmplitude = 0.010f;
    request.floatAmplitude = 0.009f;
    request.speed = 0.54f + 0.12f * Smooth01(0.5f + 0.5f * std::sin(spawnIndex));
    request.shapePerturbation = 0.85f;
    return SpawnBubble(request);
}

static bool RaySphereIntersection(const glm::vec3 &rayOrigin,
                                  const glm::vec3 &rayDirection,
                                  const glm::vec3 &sphereCenter,
                                  float sphereRadius,
                                  float &hitDistance)
{
    glm::vec3 offset = rayOrigin - sphereCenter;
    float projected = glm::dot(offset, rayDirection);
    float constant = glm::dot(offset, offset) - sphereRadius * sphereRadius;
    float discriminant = projected * projected - constant;
    if (discriminant < 0.0f)
    {
        return false;
    }

    float root = std::sqrt(discriminant);
    float nearHit = -projected - root;
    float farHit = -projected + root;
    hitDistance = nearHit >= 0.0f ? nearHit : farHit;
    return hitDistance >= 0.0f;
}

static bool RemoveBubbleAtScreenPoint(double screenX, double screenY)
{
    UpdateCamera();
    glm::vec3 rayOrigin = g_Camera.getPosition();
    glm::vec3 rayDirection = SafeNormalizeDirection(
        g_Camera.getRayDirectionFromScreen((float)screenX, (float)screenY),
        glm::vec3(0.0f, 0.0f, -1.0f));

    uint64_t selectedId = 0;
    float nearestHit = std::numeric_limits<float>::max();
    for (const DisplayBubble &bubble : g_DisplayBubbles)
    {
        if (bubble.state == DisplayBubble::State::Dead || bubble.alpha <= 0.01f)
        {
            continue;
        }

        float hitDistance = 0.0f;
        float pickRadius = std::max(bubble.radius, 0.001f);
        if (RaySphereIntersection(rayOrigin, rayDirection,
                                  bubble.position, pickRadius, hitDistance) &&
            hitDistance < nearestHit)
        {
            nearestHit = hitDistance;
            selectedId = bubble.id;
        }
    }

    if (selectedId == 0)
    {
        std::cout << "[BubbleSpawn] no bubble under cursor" << std::endl;
        return false;
    }

    g_InteractionDemoActive = false;
    g_ShowMainBubble = false;
    bool removed = RemoveBubble(selectedId);
    if (removed)
    {
        std::cout << "[BubbleSpawn] remove id=" << selectedId
                  << " live=" << LiveDisplayBubbleCount() << std::endl;
    }
    return removed;
}

static bool BurstBubbleAtScreenPoint(double screenX, double screenY)
{
    UpdateCamera();
    glm::vec3 rayOrigin = g_Camera.getPosition();
    glm::vec3 rayDirection = SafeNormalizeDirection(
        g_Camera.getRayDirectionFromScreen((float)screenX, (float)screenY),
        glm::vec3(0.0f, 0.0f, -1.0f));

    uint64_t selectedId = 0;
    float nearestHit = std::numeric_limits<float>::max();
    for (const DisplayBubble &bubble : g_DisplayBubbles)
    {
        if (bubble.state == DisplayBubble::State::Dead ||
            bubble.state == DisplayBubble::State::Burst ||
            bubble.alpha <= 0.01f)
        {
            continue;
        }

        float hitDistance = 0.0f;
        float pickRadius = std::max(bubble.radius, 0.001f);
        if (RaySphereIntersection(rayOrigin, rayDirection,
                                  bubble.position, pickRadius, hitDistance) &&
            hitDistance < nearestHit)
        {
            nearestHit = hitDistance;
            selectedId = bubble.id;
        }
    }

    if (selectedId == 0)
    {
        std::cout << "[BubbleBurst] no bubble under cursor" << std::endl;
        return false;
    }

    TriggerBubbleBurst(selectedId);
    return true;
}

static glm::mat4 AxisBasisModel(const glm::vec3 &center, const glm::vec3 &axisToPartner)
{
    glm::vec3 axis = glm::normalize(axisToPartner);
    glm::vec3 up = std::abs(axis.y) < 0.92f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 side = glm::normalize(glm::cross(up, axis));
    glm::vec3 binormal = glm::cross(axis, side);

    glm::mat4 basis(1.0f);
    basis[0] = glm::vec4(axis, 0.0f);
    basis[1] = glm::vec4(side, 0.0f);
    basis[2] = glm::vec4(binormal, 0.0f);
    basis[3] = glm::vec4(center, 1.0f);
    return basis;
}

static void ResetRenderOnlyBubbles()
{
    g_RenderOnlyBubbles = {
        {{-1.72f, -0.72f, 0.92f}, 0.12f, 0.3f, 0.42f, 0.035f, 0.70f},
        {{-1.35f, 0.92f, 1.34f}, 0.09f, 1.2f, 0.56f, 0.030f, 0.66f},
        {{-0.84f, -0.96f, 1.28f}, 0.07f, 2.1f, 0.62f, 0.026f, 0.62f},
        {{-0.28f, 1.12f, 0.82f}, 0.11f, 2.8f, 0.48f, 0.034f, 0.68f},
        {{0.52f, -1.02f, 1.18f}, 0.08f, 3.7f, 0.66f, 0.028f, 0.64f},
        {{0.98f, 0.96f, 0.88f}, 0.13f, 4.5f, 0.44f, 0.038f, 0.72f},
        {{1.48f, -0.70f, 1.38f}, 0.10f, 5.4f, 0.58f, 0.032f, 0.67f},
        {{1.84f, 0.18f, 0.96f}, 0.07f, 6.1f, 0.70f, 0.024f, 0.60f},
        {{0.14f, 1.34f, 1.42f}, 0.06f, 6.8f, 0.74f, 0.022f, 0.58f}};
}

static void ResetDisplayBubbles()
{
    struct BubbleSeed
    {
        glm::vec3 pos;
        float radius;
        float phase;
        float wind;
        float lift;
        float speed;
        glm::vec3 velocity;
    };

    const std::vector<BubbleSeed> seeds = {
        {{-0.72f, -0.18f, 1.08f}, 0.46f, 0.2f, 0.010f, 0.009f, 0.55f, {0.090f, 0.018f, 0.000f}},
        {{0.68f, -0.16f, 1.12f}, 0.43f, 1.7f, 0.010f, 0.009f, 0.58f, {-0.085f, 0.016f, 0.000f}},
        {{0.00f, 0.78f, 1.08f}, 0.40f, 2.8f, 0.012f, 0.010f, 0.62f, {0.000f, -0.082f, 0.003f}},
        {{1.22f, 0.54f, 1.04f}, 0.34f, 3.6f, 0.010f, 0.009f, 0.52f, {0.045f, 0.000f, 0.000f}},
        {{1.90f, 0.56f, 1.06f}, 0.22f, 4.4f, 0.011f, 0.010f, 0.68f, {-0.075f, 0.000f, 0.000f}},
        {{-1.48f, 0.42f, 1.20f}, 0.28f, 5.2f, 0.010f, 0.009f, 0.50f, {0.018f, -0.012f, 0.000f}}};

    g_DisplayBubbles.clear();
    g_BubbleSims.clear();
    ++g_InteractionOutcomeSeed;
    g_NextBubbleId = 1;
    g_CameraOrbitTarget = glm::vec3(0.20f, -0.10f, 1.10f);
    g_CameraDistance = 3.75f;
    g_CurrentAngleX = 0.0f;
    g_CurrentAngleY = 0.0f;
    g_DisplayBubbles.reserve(seeds.size());
    const glm::vec3 clusteredHomes[] = {
        {-0.30f, -0.10f, 1.09f},
        {0.28f, -0.08f, 1.11f},
        {0.00f, 0.34f, 1.09f},
        {1.36f, 0.54f, 1.05f},
        {1.78f, 0.55f, 1.05f},
        {-1.38f, 0.36f, 1.18f}};
    for (size_t i = 0; i < seeds.size(); ++i)
    {
        const auto &seed = seeds[i];
        AddBubble(seed.pos, seed.radius, seed.velocity, clusteredHomes[i],
                  seed.phase, seed.wind, seed.lift, seed.speed, false);
    }

    g_MainBubbleVisualRadius = g_BubbleRadius;
    g_MainBubbleTargetRadius = g_BubbleRadius;
    g_ContactPairs.clear();
    ResetRenderOnlyBubbles();

    DisplayBubble &outerA = g_DisplayBubbles[3];
    DisplayBubble &outerB = g_DisplayBubbles[4];
    float outerFilmRadius = std::min(outerA.radius, outerB.radius) * 0.24f;
    BubbleContactPair outerFusion;
    outerFusion.a = outerA.id;
    outerFusion.b = outerB.id;
    outerFusion.filmThickness = 1.0f;
    outerFusion.restDistance =
        std::sqrt(outerA.radius * outerA.radius - outerFilmRadius * outerFilmRadius) +
        std::sqrt(outerB.radius * outerB.radius - outerFilmRadius * outerFilmRadius);
    outerFusion.neckRadius = std::min(outerA.radius, outerB.radius) * 0.05f;
    outerFusion.targetVolume = outerA.targetVolume + outerB.targetVolume;
    outerFusion.outcome = BubbleContactPair::CoalescenceOutcome::WillCoalesce;
    outerFusion.coalescenceScore = 1.0f;
    outerFusion.active = true;
    outerFusion.persistentRenderPair = true;
    g_ContactPairs.push_back(outerFusion);

    g_InteractionDemoActive = false;
    g_ShowMainBubble = false;
    g_BridgeStrength = 0.0f;
    g_AutoTouchPoint = glm::vec2(-10.0f, -10.0f);
    g_AutoTouchStrength = 0.0f;
    g_AutoTouchVelocity = 0.0f;
    if (g_DecorativeBubbleModel)
    {
        RebuildDisplayBubbleModels();
    }
}

static void StartBubbleInteractionDemo()
{
    enum class DemoVariant
    {
        StableDouble,
        UnequalFusion,
        Separate
    };
    static int s_NextVariant = 0;
    DemoVariant variant = (DemoVariant)s_NextVariant;
    s_NextVariant = (s_NextVariant + 1) % 3;

    ResetDisplayBubbles();
    g_RenderOnlyBubbles.clear();
    g_CameraOrbitTarget = glm::vec3(0.0f, 0.0f, 1.10f);
    g_CameraDistance = 2.75f;

    g_ContactPairs.clear();
    DisplayBubble &a = g_DisplayBubbles[0];
    DisplayBubble &b = g_DisplayBubbles[1];
    glm::vec3 center(0.0f, 0.0f, 1.10f);
    glm::vec3 axis = glm::normalize(glm::vec3(1.0f, 0.04f, 0.0f));
    float radiusA = variant == DemoVariant::UnequalFusion ? 0.64f : 0.56f;
    float radiusB = variant == DemoVariant::UnequalFusion
                        ? 0.31f
                        : (variant == DemoVariant::Separate ? 0.50f : 0.56f);
    float finalFilmRadius = std::min(radiusA, radiusB) * 0.30f;
    float restDistance = std::sqrt(radiusA * radiusA - finalFilmRadius * finalFilmRadius) + std::sqrt(radiusB * radiusB - finalFilmRadius * finalFilmRadius);
    float centerDistance = radiusA + radiusB + std::min(radiusA, radiusB) * 0.065f;
    float volumeA = BubbleVolume(radiusA);
    float volumeB = BubbleVolume(radiusB);
    float totalVolume = volumeA + volumeB;
    float invMassA = 1.0f / std::max(volumeA, 0.001f);
    float invMassB = 1.0f / std::max(volumeB, 0.001f);
    float invMassSum = invMassA + invMassB;
    float approachSpeed = variant == DemoVariant::Separate ? 0.13f : 0.10f;

    a.radius = radiusA;
    a.initialRadius = radiusA;
    a.targetVolume = BubbleVolume(radiusA);
    a.alpha = 1.0f;
    a.contactTime = 0.0f;
    a.mergeProgress = 0.0f;
    a.filmThickness = 1.0f;
    a.contactStrength = 0.0f;
    a.surfaceDynamicsBlend = 1.0f;
    a.contactAxis = axis;
    a.volumeTransferred = false;
    a.state = DisplayBubble::State::Free;
    a.surfaceControls = MakeSurfaceControls(a.phase);
    a.position = center - axis * (centerDistance * volumeB / totalVolume);
    a.basePosition = center - axis * (restDistance * volumeB / totalVolume);
    a.velocity = axis * (approachSpeed * invMassA / invMassSum);

    b.radius = radiusB;
    b.initialRadius = radiusB;
    b.targetVolume = BubbleVolume(radiusB);
    b.alpha = 1.0f;
    b.contactTime = 0.0f;
    b.mergeProgress = 0.0f;
    b.filmThickness = 1.0f;
    b.contactStrength = 0.0f;
    b.surfaceDynamicsBlend = 1.0f;
    b.contactAxis = -axis;
    b.volumeTransferred = false;
    b.state = DisplayBubble::State::Free;
    b.surfaceControls = MakeSurfaceControls(b.phase);
    b.position = center + axis * (centerDistance * volumeA / totalVolume);
    b.basePosition = center + axis * (restDistance * volumeA / totalVolume);
    b.velocity = -axis * (approachSpeed * invMassB / invMassSum);

    for (size_t i = 2; i < g_DisplayBubbles.size(); ++i)
    {
        g_DisplayBubbles[i].alpha = 0.0f;
        g_DisplayBubbles[i].state = DisplayBubble::State::Dead;
    }

    BubbleContactPair pair;
    pair.a = a.id;
    pair.b = b.id;
    pair.filmThickness = 1.0f;
    pair.geometryBlend = 0.0f;
    pair.restDistance = restDistance;
    pair.neckRadius = std::min(radiusA, radiusB) * 0.06f;
    pair.ruptureRisk = 0.0f;
    pair.targetVolume = a.targetVolume + b.targetVolume;
    pair.state = BubbleContactPair::State::Free;
    if (variant == DemoVariant::StableDouble)
    {
        pair.outcome = BubbleContactPair::CoalescenceOutcome::StayDoubleBubble;
        pair.coalescenceScore = 0.0f;
    }
    else if (variant == DemoVariant::UnequalFusion)
    {
        pair.outcome = BubbleContactPair::CoalescenceOutcome::WillCoalesce;
        pair.coalescenceScore = 1.0f;
    }
    else
    {
        pair.outcome = BubbleContactPair::CoalescenceOutcome::SeparateAfterContact;
        pair.coalescenceScore = 0.0f;
    }
    pair.active = true;
    pair.persistentRenderPair = true;
    g_ContactPairs.push_back(pair);

    g_InteractionDemoActive = true;
    RebuildDisplayBubbleModels();
    g_ShowMainBubble = false;
    const char *variantName = variant == DemoVariant::StableDouble
                                  ? "equal-size stable figure-eight"
                                  : (variant == DemoVariant::UnequalFusion
                                         ? "small bubble absorbed by large bubble"
                                         : "temporary contact then separation");
    std::cout << "[BubbleInteraction] replay: " << variantName
              << " (press G for next)" << std::endl;
}

static void ConfigureFusionValidationCase(const std::string &caseName)
{
    if (g_DisplayBubbles.size() < 2 || g_ContactPairs.empty())
        return;
    DisplayBubble &a = g_DisplayBubbles[0];
    DisplayBubble &b = g_DisplayBubbles[1];
    BubbleContactPair &pair = g_ContactPairs[0];
    float radiusA = 0.64f;
    float radiusB = 0.32f;
    glm::vec3 axis = glm::normalize(glm::vec3(1.0f, 0.04f, 0.0f));
    if (caseName == "equal")
    {
        radiusA = radiusB = 0.50f;
    }
    else if (caseName == "tiny")
    {
        radiusB = 0.13f;
    }
    else if (caseName == "diagonal")
    {
        radiusA = 0.56f;
        radiusB = 0.38f;
        axis = glm::normalize(glm::vec3(1.0f, 0.62f, 0.28f));
    }

    glm::vec3 center(0.0f, 0.0f, 1.10f);
    float volumeA = BubbleVolume(radiusA);
    float volumeB = BubbleVolume(radiusB);
    float totalVolume = volumeA + volumeB;
    float filmRadius = std::min(radiusA, radiusB) * 0.30f;
    float restDistance =
        std::sqrt(radiusA * radiusA - filmRadius * filmRadius) +
        std::sqrt(radiusB * radiusB - filmRadius * filmRadius);
    float centerDistance =
        radiusA + radiusB + std::min(radiusA, radiusB) * 0.065f;

    a.radius = a.initialRadius = radiusA;
    b.radius = b.initialRadius = radiusB;
    a.targetVolume = volumeA;
    b.targetVolume = volumeB;
    a.position = center - axis * (centerDistance * volumeB / totalVolume);
    b.position = center + axis * (centerDistance * volumeA / totalVolume);
    a.basePosition = center - axis * (restDistance * volumeB / totalVolume);
    b.basePosition = center + axis * (restDistance * volumeA / totalVolume);
    a.velocity = axis * 0.05f;
    b.velocity = -axis * 0.05f;
    a.contactAxis = axis;
    b.contactAxis = -axis;
    pair.restDistance = restDistance;
    pair.targetVolume = totalVolume;
    pair.outcome =
        BubbleContactPair::CoalescenceOutcome::WillCoalesce;
    pair.coalescenceScore = 1.0f;
    pair.active = true;
    pair.persistentRenderPair = true;
    std::cout << "[FusionValidation] case=" << caseName
              << " radii=(" << radiusA << ", " << radiusB
              << ") axis=(" << axis.x << ", " << axis.y
              << ", " << axis.z << ")" << std::endl;
}

static glm::vec3 BubbleVisualCenter(const DisplayBubble &bubble, float time)
{
    glm::vec3 windDir = glm::normalize(glm::vec3(0.75f, 0.22f, 0.0f));
    float motionScale = g_InteractionDemoActive ? 0.18f : 1.0f;
    float wind = sinf(time * bubble.speed + bubble.phase) * bubble.windAmplitude * motionScale;
    float lift = sinf(time * (bubble.speed * 0.73f) + bubble.phase * 1.7f) * bubble.floatAmplitude * motionScale;
    float depth = cosf(time * (bubble.speed * 0.51f) + bubble.phase * 0.9f) * bubble.windAmplitude * 0.22f * motionScale;
    return bubble.position + windDir * wind + glm::vec3(0.0f, lift, depth);
}

static glm::mat4 MakeContactFilmModel(const DisplayBubble &a, const DisplayBubble &b,
                                      const BubbleContactPair &pair, float time)
{
    glm::vec3 pa = BubbleVisualCenter(a, time);
    glm::vec3 pb = BubbleVisualCenter(b, time);
    glm::vec3 axis = pb - pa;
    float length = glm::length(axis);
    if (length < 1e-4f)
    {
        return glm::mat4(1.0f);
    }

    axis /= length;
    if (pair.contactFrameInitialized)
    {
        axis = pair.filteredNormal;
    }
    glm::vec3 up = std::abs(axis.y) < 0.92f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 side = glm::normalize(glm::cross(up, axis));
    glm::vec3 binormal = glm::cross(axis, side);

    float pulse = 1.0f + sinf(time * 0.70f + (float)(pair.a * 1.7f + pair.b)) * 0.008f;
    float filmRadius = std::max(0.002f, pair.contactRadius * pulse);
    glm::vec3 filmCenter = pair.contactFrameInitialized
                               ? pair.filteredPlaneCenter
                               : (pa + pb) * 0.5f;

    glm::mat4 basis(1.0f);
    basis[0] = glm::vec4(side, 0.0f);
    basis[1] = glm::vec4(binormal, 0.0f);
    basis[2] = glm::vec4(axis, 0.0f);
    basis[3] = glm::vec4(filmCenter, 1.0f);
    return basis * glm::scale(glm::mat4(1.0f), glm::vec3(filmRadius));
}

static glm::mat4 BubbleModelMatrix(const DisplayBubble &bubble, float time)
{
    glm::vec3 windDir = glm::normalize(glm::vec3(0.75f, 0.22f, 0.0f));
    float motionScale = g_InteractionDemoActive ? 0.18f : 1.0f;
    float shapeWobble = g_InteractionDemoActive ? 0.006f : 0.018f;
    float wind = sinf(time * bubble.speed + bubble.phase) * bubble.windAmplitude * motionScale;
    float lift = sinf(time * (bubble.speed * 0.73f) + bubble.phase * 1.7f) * bubble.floatAmplitude * motionScale;
    float depth = cosf(time * (bubble.speed * 0.51f) + bubble.phase * 0.9f) * bubble.windAmplitude * 0.22f * motionScale;
    float wobble = 1.0f + sinf(time * (bubble.speed * 0.20f) + bubble.phase * 2.1f) * shapeWobble;
    float bridgePulse = (bubble.state == DisplayBubble::State::Touch ||
                         bubble.state == DisplayBubble::State::SharedFilm ||
                         bubble.state == DisplayBubble::State::NeckForming)
                            ? sinf(time * 1.10f + bubble.phase) * (g_InteractionDemoActive ? 0.006f : 0.018f)
                            : 0.0f;

    glm::vec3 pos = bubble.position + windDir * wind + glm::vec3(0.0f, lift, depth);
    float scale = std::max(0.001f, bubble.radius) * (wobble + bridgePulse);
    for (const auto &control : bubble.surfaceControls)
    {
        float worldMask = std::max(glm::dot(control.localDir, bubble.contactAxis), 0.0f);
        float radialOffset = glm::dot(control.displacement, control.localDir);
        scale += bubble.radius * radialOffset * worldMask * 0.08f;
    }
    glm::mat4 model = glm::translate(glm::mat4(1.0f), pos);
    if (bubble.contactStrength > 0.01f)
    {
        glm::vec3 axis = glm::normalize(bubble.contactAxis);
        glm::vec3 up = std::abs(axis.y) < 0.92f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
        glm::vec3 side = glm::normalize(glm::cross(up, axis));
        glm::vec3 binormal = glm::cross(axis, side);
        float globalContact = glm::clamp(bubble.contactStrength, 0.0f, 1.0f);
        float squash = 1.0f - 0.025f * globalContact;
        float bulge = 1.0f + 0.010f * globalContact;
        glm::mat4 basis(1.0f);
        basis[0] = glm::vec4(axis, 0.0f);
        basis[1] = glm::vec4(side, 0.0f);
        basis[2] = glm::vec4(binormal, 0.0f);
        model *= basis * glm::scale(glm::mat4(1.0f), glm::vec3(scale * squash, scale * bulge, scale * bulge));
    }
    else
    {
        model = glm::scale(model, glm::vec3(scale));
    }
    return model;
}

static glm::mat4 PersistentBubbleModelMatrix(const DisplayBubble &bubble,
                                            float time,
                                            bool worldScaleSurface)
{
    (void)time;
    glm::mat4 model = glm::translate(glm::mat4(1.0f), bubble.position);
    if (bubble.state == DisplayBubble::State::Burst)
    {
        // If we have a sim, its mesh is at world radius — pure translation.
        // Otherwise fall back to manual burstScale scaling.
        if (GetBubbleSim(bubble.id))
            return model;
        model = glm::scale(model, glm::vec3(std::max(bubble.burstScale, 0.001f)));
        return model;
    }
    return worldScaleSurface
               ? model
               : glm::scale(model, glm::vec3(std::max(bubble.radius, 0.001f)));
}

static void PreserveFilmCoordinates(std::vector<Vertex> &vertices,
                                    const std::vector<Vertex> &previousVertices)
{
    if (vertices.size() != previousVertices.size())
    {
        return;
    }
    for (size_t i = 0; i < vertices.size(); ++i)
    {
        if (glm::dot(previousVertices[i].FilmDirection,
                     previousVertices[i].FilmDirection) > 1e-8f)
        {
            vertices[i].FilmDirection = previousVertices[i].FilmDirection;
        }
    }
}

static std::vector<Vertex> BuildPersistentBubbleVertices(
    int bubbleIndex,
    float time,
    const std::vector<Vertex> *restVertices,
    bool worldScaleSurface,
    const std::vector<Vertex> *previousVertices = nullptr)
{
    std::vector<Vertex> vertices = restVertices
                                       ? *restVertices
                                       : BuildBubbleShellVertices(1.0f, 0.0f, false);
    if (bubbleIndex < 0 || bubbleIndex >= (int)g_DisplayBubbles.size())
    {
        return vertices;
    }

    const DisplayBubble &bubble = g_DisplayBubbles[(size_t)bubbleIndex];
    float deformationUnitScale = worldScaleSurface ? bubble.radius : 1.0f;
    float surfaceDynamicsBlend = Smooth01(bubble.surfaceDynamicsBlend);
    glm::vec3 motionAxis = bubble.velocity;
    if (glm::length(motionAxis) < 1e-4f)
    {
        motionAxis = glm::vec3(0.78f, 0.26f, 0.12f);
    }
    motionAxis = glm::normalize(motionAxis);
    float speedStretch = glm::clamp(glm::length(bubble.velocity) * 0.055f, 0.0f, 0.045f) *
                         surfaceDynamicsBlend;
    float freeOscillation = std::sin(time * (0.72f + bubble.speed * 0.18f) + bubble.phase) *
                            0.012f * surfaceDynamicsBlend;
    float axialScale = 1.0f + speedStretch + freeOscillation;
    float radialScale = 1.0f / std::sqrt(std::max(axialScale, 0.25f));
    float contactShare = 1.0f /
                         std::sqrt((float)BubbleActiveContactCount(g_DisplayBubbles, g_ContactPairs, bubbleIndex));
    float meanControlRadialOffset = 0.0f;
    for (const auto &control : bubble.surfaceControls)
    {
        meanControlRadialOffset += glm::dot(control.displacement, control.localDir);
    }
    if (!bubble.surfaceControls.empty())
    {
        meanControlRadialOffset /= (float)bubble.surfaceControls.size();
    }
    float persistentVolumeScale = glm::mix(
        1.0f,
        glm::clamp(1.0f - meanControlRadialOffset, 0.94f, 1.06f),
        surfaceDynamicsBlend);

    for (Vertex &vertex : vertices)
    {
        glm::vec3 restPosition = vertex.Position;
        glm::vec3 restNormal = SafeNormalizeDirection(vertex.Normal, restPosition);
        float restRadius = std::max(glm::length(restPosition), 0.001f);
        glm::vec3 direction = restPosition / restRadius;
        vertex.FilmDirection = direction;
        float alongMotion = glm::dot(direction, motionAxis);
        glm::vec3 axial = motionAxis * alongMotion * axialScale;
        glm::vec3 radial = (direction - motionAxis * alongMotion) * radialScale;
        glm::vec3 position = (axial + radial) * restRadius;
        glm::vec3 persistentSurfaceOffset(0.0f);
        float persistentWeight = 0.0f;
        for (const auto &control : bubble.surfaceControls)
        {
            float alignment = std::max(glm::dot(direction, control.localDir), 0.0f);
            float weight = std::pow(alignment, 8.0f);
            persistentSurfaceOffset += control.displacement * weight;
            persistentWeight += weight;
        }
        if (persistentWeight > 1e-5f)
        {
            position += persistentSurfaceOffset / persistentWeight *
                        surfaceDynamicsBlend * deformationUnitScale;
        }
        glm::vec3 contactOffset(0.0f);
        float vertexContactSum = 0.0f;

        for (const BubbleContactPair &pair : g_ContactPairs)
        {
            int otherIndex = -1;
            if ((!pair.candidate && !pair.bonded) ||
                !PairContainsBubbleIndex(g_DisplayBubbles, pair, bubbleIndex, &otherIndex))
            {
                continue;
            }
            const DisplayBubble &otherBubble = g_DisplayBubbles[(size_t)otherIndex];
            glm::vec3 contactDirection =
                otherBubble.position - bubble.position;
            float contactDistance = glm::length(contactDirection);
            if (contactDistance < 1e-5f)
            {
                continue;
            }
            contactDirection /= contactDistance;

            float bondedStrength = glm::clamp(
                0.20f + 0.80f * std::max(pair.interactionCompression,
                                         Smooth01(pair.contactTime / kFusionTime)),
                0.0f, 1.0f);
            float pairStrength = pair.bonded
                                     ? std::max(pair.contactActivation, bondedStrength)
                                     : pair.contactActivation;
            if (pairStrength <= 0.001f)
            {
                continue;
            }
            float sizeDeformScale = glm::clamp(
                std::sqrt(std::max(otherBubble.radius, 0.001f) / std::max(bubble.radius, 0.001f)),
                0.62f, 1.65f);
            float deformationStrength = pairStrength * contactShare * sizeDeformScale;

            float contactRatio = glm::clamp(pair.contactRadius /
                                                std::max(bubble.radius, 0.001f),
                                            0.0f, 0.92f);
            float geometricConeCos = std::sqrt(std::max(1.0f - contactRatio * contactRatio,
                                                        0.0f));
            float coneCos = pair.bonded
                                ? geometricConeCos
                                : glm::mix(0.94f, 0.70f, deformationStrength);
            float alignment = glm::dot(direction, contactDirection);
            float contactMask = Smooth01((alignment - coneCos) /
                                         std::max(1.0f - coneCos, 0.001f));
            float rimCenter = coneCos - 0.045f;
            float rimMask = std::exp(-std::pow((alignment - rimCenter) / 0.045f, 2.0f));
            glm::vec3 radialDirection = direction - contactDirection * alignment;
            if (glm::length(radialDirection) > 1e-5f)
            {
                radialDirection = glm::normalize(radialDirection);
            }

            // Keep only a small analytic correction for an exact seal against
            // the contact plane. Most deformation now comes from persistent
            // surface-control dynamics above.
            contactOffset -= contactDirection *
                             (0.006f + 0.040f * deformationStrength) *
                             contactMask * deformationUnitScale;
            contactOffset += radialDirection *
                             (0.003f + 0.010f * deformationStrength) *
                             rimMask * deformationUnitScale;
            vertexContactSum += deformationStrength * contactMask;
        }

        float maxOffset = 0.18f * deformationUnitScale;
        float offsetLength = glm::length(contactOffset);
        if (offsetLength > maxOffset)
        {
            contactOffset *= maxOffset / offsetLength;
        }
        float volumeCompensation = 1.0f + std::min(vertexContactSum, 2.0f) * 0.012f;
        vertex.Position = position * (volumeCompensation * persistentVolumeScale) + contactOffset;
        glm::vec3 deformedNormal = SafeNormalizeDirection(vertex.Position, restNormal);
        vertex.Normal = SafeNormalizeDirection(
            glm::mix(restNormal, deformedNormal, surfaceDynamicsBlend),
            restNormal);
    }

    std::vector<unsigned int> indices = BuildContactBubblePatchIndices();
    std::vector<glm::vec3> normalSums(vertices.size(), glm::vec3(0.0f));
    for (size_t i = 0; i + 2 < indices.size(); i += 3)
    {
        unsigned int ia = indices[i];
        unsigned int ib = indices[i + 1];
        unsigned int ic = indices[i + 2];
        glm::vec3 faceNormal = glm::cross(vertices[ib].Position - vertices[ia].Position,
                                          vertices[ic].Position - vertices[ia].Position);
        if (glm::length(faceNormal) <= 1e-8f)
        {
            continue;
        }
        normalSums[ia] += faceNormal;
        normalSums[ib] += faceNormal;
        normalSums[ic] += faceNormal;
    }
    float rebuiltNormalBlend = glm::clamp(
        std::max(surfaceDynamicsBlend, bubble.contactStrength),
        0.0f, 1.0f);
    for (size_t i = 0; i < vertices.size(); ++i)
    {
        if (glm::length(normalSums[i]) > 1e-6f)
        {
            vertices[i].Normal = SafeNormalizeDirection(
                glm::mix(vertices[i].Normal,
                         glm::normalize(normalSums[i]),
                         rebuiltNormalBlend),
                vertices[i].Normal);
        }
    }
    if (previousVertices)
    {
        PreserveFilmCoordinates(vertices, *previousVertices);
    }
    return vertices;
}

static float MeasureFusionShapeMode(
    const std::vector<FusionProfileNode> &profile)
{
    if (profile.size() < 3)
        return 0.0f;
    float axialHalfExtent =
        0.5f * std::max(profile.back().z - profile.front().z, 1e-5f);
    float radialExtent = 1e-5f;
    for (const FusionProfileNode &node : profile)
        radialExtent = std::max(radialExtent, node.r);
    return std::log(axialHalfExtent / radialExtent);
}

static float SphereProfileRadius(float z, float center, float radius)
{
    float offset = z - center;
    return std::sqrt(std::max(radius * radius - offset * offset, 0.0f));
}

static void InitializeFusionProfile(BubbleContactPair &pair,
                                    const DisplayBubble &a,
                                    const DisplayBubble &b,
                                    const glm::vec3 &axis)
{
    const float volumeA = a.targetVolume > 0.0f
                              ? a.targetVolume
                              : BubbleVolume(a.radius);
    const float volumeB = b.targetVolume > 0.0f
                              ? b.targetVolume
                              : BubbleVolume(b.radius);
    const float theoreticalVolume = std::max(volumeA + volumeB, 1e-6f);
    const glm::vec3 weightedCenter =
        (a.position * volumeA + b.position * volumeB) / theoreticalVolume;
    pair.fusionCenterVelocity =
        (a.velocity * volumeA + b.velocity * volumeB) / theoreticalVolume;

    // A merged film receives one visual identity at rupture. This state is
    // subsequently shared by the stateful mesh and the promoted bubble.
    pair.visualState = BubbleVisualState{};
    pair.visualState.initialized = true;
    pair.visualState.filmBaseThicknessScale =
        (a.visualState.filmBaseThicknessScale * volumeA +
         b.visualState.filmBaseThicknessScale * volumeB) /
        theoreticalVolume;
    pair.visualState.filmThicknessAmplitudeScale =
        (a.visualState.filmThicknessAmplitudeScale * volumeA +
         b.visualState.filmThicknessAmplitudeScale * volumeB) /
        theoreticalVolume;
    pair.visualState.interferencePhase =
        (a.visualState.interferencePhase * volumeA +
         b.visualState.interferencePhase * volumeB) /
        theoreticalVolume;
    pair.visualState.noiseSeed = static_cast<uint32_t>(
        ((pair.a * 2246822519ull) ^ (pair.b * 3266489917ull)) &
        0xffffffffu);
    pair.visualState.animationTimeOrigin = static_cast<float>(g_Time);
    pair.visualState.textureOriginWorld = weightedCenter;
    pair.visualState.textureBasis[0] = axis;
    pair.visualState.textureBasis[1] = pair.fusionSide;
    pair.visualState.textureBasis[2] = pair.fusionBinormal;
    pair.visualState.referenceRadius = RadiusFromVolume(theoreticalVolume);
    pair.visualState.ior =
        (a.visualState.ior * volumeA + b.visualState.ior * volumeB) /
        theoreticalVolume;
    pair.visualState.fresnelStrength =
        (a.visualState.fresnelStrength * volumeA +
         b.visualState.fresnelStrength * volumeB) /
        theoreticalVolume;
    pair.visualState.opacity = std::max(a.alpha, b.alpha);
    // Use the geometric normal exclusively for refraction.  The transported
    // filmDirection's axial component flips sign at the neck (each parent
    // bubble's outward normal points in the opposite direction), and using it
    // as the refraction normal creates a visible seam there.  The geometric
    // normal is continuous across the neck, so it eliminates the line.
    // Iridescence noise is unaffected: it uses vFilmCoordinate (world-space),
    // not filmDirection.  The NdotV used by thin-film interference changes
    // only slightly (geometric vs film normal are similar away from the neck).
    pair.visualState.opticalNormalBlendAtOrigin = 0.0f;
    pair.visualState.opticalNormalRelaxationRate = 0.0f;

    const float centerA = glm::dot(a.position - weightedCenter, axis);
    const float centerB = glm::dot(b.position - weightedCenter, axis);
    const float minRadius = std::max(std::min(a.radius, b.radius), 1e-4f);
    const float centerDistance = std::max(centerB - centerA, 1e-5f);
    const float intersectionZ =
        (a.radius * a.radius - b.radius * b.radius -
         centerA * centerA + centerB * centerB) /
        (2.0f * centerDistance);
    const float bridgeHalfWidth = 0.36f * minRadius;
    const float leftJoin = glm::clamp(
        intersectionZ - bridgeHalfWidth,
        centerA - 0.96f * a.radius,
        centerA + 0.96f * a.radius);
    const float rightJoin = glm::clamp(
        intersectionZ + bridgeHalfWidth,
        centerB - 0.96f * b.radius,
        centerB + 0.96f * b.radius);
    const float neckZ = glm::clamp(intersectionZ,
                                   leftJoin + 1e-4f,
                                   rightJoin - 1e-4f);
    const float leftRadius = SphereProfileRadius(
        leftJoin, centerA, a.radius);
    const float rightRadius = SphereProfileRadius(
        rightJoin, centerB, b.radius);
    const float initialNeckRadius = std::min(
        0.14f * minRadius,
        0.72f * std::min(leftRadius, rightRadius));
    auto sphereSlope = [](float z, float center, float radius) {
        float r = SphereProfileRadius(z, center, radius);
        return -(z - center) / std::max(r, 1e-5f);
    };
    const float rawLeftSlope = sphereSlope(leftJoin, centerA, a.radius);
    const float rawRightSlope = sphereSlope(rightJoin, centerB, b.radius);
    const float leftSecant = (initialNeckRadius - leftRadius) /
                             std::max(neckZ - leftJoin, 1e-5f);
    const float rightSecant = (rightRadius - initialNeckRadius) /
                              std::max(rightJoin - neckZ, 1e-5f);
    // Monotone Hermite bounds prevent the analytic bridge from developing a
    // hidden overshoot between sampled rings.
    const float leftSlope = glm::clamp(rawLeftSlope,
                                       3.0f * leftSecant, 0.0f);
    const float rightSlope = glm::clamp(rawRightSlope,
                                        0.0f, 3.0f * rightSecant);
    glm::vec2 leftTangent = glm::normalize(glm::vec2(1.0f, leftSlope));
    glm::vec2 rightTangent = glm::normalize(glm::vec2(1.0f, rightSlope));
    glm::vec2 leftPoint(leftJoin, leftRadius);
    glm::vec2 neckPoint(neckZ, initialNeckRadius);
    glm::vec2 rightPoint(rightJoin, rightRadius);
    float leftHandle = 0.34f * glm::length(neckPoint - leftPoint);
    float rightHandle = 0.34f * glm::length(rightPoint - neckPoint);
    auto cubicBezier = [](const glm::vec2 &p0, const glm::vec2 &p1,
                          const glm::vec2 &p2, const glm::vec2 &p3,
                          float t) {
        float u = 1.0f - t;
        return u * u * u * p0 + 3.0f * u * u * t * p1 +
               3.0f * u * t * t * p2 + t * t * t * p3;
    };

    // Construct the initial meridian parametrically. Cubic bridge handles
    // match the two sphere tangents and a horizontal neck tangent, so steep
    // caps do not turn into a high-order r(z) overshoot.
    std::vector<glm::vec2> denseProfile;
    denseProfile.reserve(2049);
    auto appendPoint = [&](const glm::vec2 &point) {
        if (denseProfile.empty() ||
            glm::length(point - denseProfile.back()) > 1e-7f)
            denseProfile.push_back(point);
    };
    constexpr int denseSectionSamples = 512;
    float leftTheta = std::acos(glm::clamp(
        (leftJoin - centerA) / a.radius, -1.0f, 1.0f));
    for (int i = 0; i <= denseSectionSamples; ++i)
    {
        float t = static_cast<float>(i) / denseSectionSamples;
        float theta = glm::mix(3.14159265358979323846f, leftTheta, t);
        appendPoint(glm::vec2(centerA + a.radius * std::cos(theta),
                              a.radius * std::sin(theta)));
    }
    for (int i = 1; i <= denseSectionSamples; ++i)
    {
        float t = static_cast<float>(i) / denseSectionSamples;
        appendPoint(cubicBezier(
            leftPoint, leftPoint + leftHandle * leftTangent,
            neckPoint - glm::vec2(leftHandle, 0.0f), neckPoint, t));
    }
    std::size_t denseNeckIndex = denseProfile.size() - 1;
    for (int i = 1; i <= denseSectionSamples; ++i)
    {
        float t = static_cast<float>(i) / denseSectionSamples;
        appendPoint(cubicBezier(
            neckPoint, neckPoint + glm::vec2(rightHandle, 0.0f),
            rightPoint - rightHandle * rightTangent, rightPoint, t));
    }
    float rightTheta = std::acos(glm::clamp(
        (rightJoin - centerB) / b.radius, -1.0f, 1.0f));
    for (int i = 1; i <= denseSectionSamples; ++i)
    {
        float t = static_cast<float>(i) / denseSectionSamples;
        float theta = glm::mix(rightTheta, 0.0f, t);
        appendPoint(glm::vec2(centerB + b.radius * std::cos(theta),
                              b.radius * std::sin(theta)));
    }

    std::vector<float> denseMetric(denseProfile.size(), 0.0f);
    for (std::size_t i = 1; i < denseProfile.size(); ++i)
    {
        float ds = glm::length(denseProfile[i] - denseProfile[i - 1]);
        float midZ = 0.5f * (denseProfile[i].x + denseProfile[i - 1].x);
        float neckWeight = 1.0f + 2.0f * std::exp(
            -0.5f * (midZ - neckZ) * (midZ - neckZ) /
            std::max(0.22f * minRadius * 0.22f * minRadius, 1e-8f));
        denseMetric[i] = denseMetric[i - 1] + ds * neckWeight;
    }
    float neckMetric = denseMetric[denseNeckIndex];
    int neckSampleIndex = glm::clamp(
        static_cast<int>(std::round(
            static_cast<float>(kFusionSurfaceRings) * neckMetric /
            std::max(denseMetric.back(), 1e-7f))),
        2, kFusionSurfaceRings - 2);

    pair.fusionProfile.assign(
        static_cast<std::size_t>(kFusionSurfaceRings + 1),
        FusionProfileNode{});
    pair.fusionNeckNode = 0;
    float closestNeckDistance = std::numeric_limits<float>::max();
    std::size_t denseSegment = 0;
    for (int i = 0; i <= kFusionSurfaceRings; ++i)
    {
        float desiredMetric = i <= neckSampleIndex
            ? neckMetric * static_cast<float>(i) /
                  static_cast<float>(neckSampleIndex)
            : neckMetric + (denseMetric.back() - neckMetric) *
                  static_cast<float>(i - neckSampleIndex) /
                  static_cast<float>(kFusionSurfaceRings - neckSampleIndex);
        while (denseSegment + 1 < denseMetric.size() - 1 &&
               denseMetric[denseSegment + 1] < desiredMetric)
            ++denseSegment;
        float metricSpan = std::max(
            denseMetric[denseSegment + 1] - denseMetric[denseSegment], 1e-8f);
        float sampleT = glm::clamp(
            (desiredMetric - denseMetric[denseSegment]) / metricSpan,
            0.0f, 1.0f);
        glm::vec2 sample = glm::mix(denseProfile[denseSegment],
                                    denseProfile[denseSegment + 1], sampleT);
        float z = sample.x;
        float dzA = z - centerA;
        float dzB = z - centerB;
        float rhoA = SphereProfileRadius(z, centerA, a.radius);
        float rhoB = SphereProfileRadius(z, centerB, b.radius);
        float distanceFromContact = z - neckZ;

        FusionProfileNode &node =
            pair.fusionProfile[static_cast<std::size_t>(i)];
        node.z = z;
        node.r = sample.y;
        if (i == 0 || i == kFusionSurfaceRings)
            node.r = 0.0f;

        float sourceBlend = Smooth01(
            0.5f + 0.5f * distanceFromContact /
                       std::max(0.22f * minRadius, 1e-4f));
        glm::vec2 filmA(dzA / std::max(a.radius, 1e-4f),
                        rhoA / std::max(a.radius, 1e-4f));
        glm::vec2 filmB(dzB / std::max(b.radius, 1e-4f),
                        rhoB / std::max(b.radius, 1e-4f));
        glm::vec2 film = glm::mix(filmA, filmB, sourceBlend);
        float filmLength = glm::length(film);
        if (filmLength > 1e-6f)
            film /= filmLength;
        else
            film = glm::vec2(0.0f, 1.0f);
        node.filmAxial = film.x;
        node.filmRadial = film.y;
        node.opticalRadiusScale =
            glm::mix(a.radius, b.radius, sourceBlend);

        float neckImpulse =
            kFusionProfileConfig.initialNeckExpansionSpeed *
            std::exp(-0.5f * distanceFromContact * distanceFromContact /
                     std::max(0.16f * minRadius * 0.16f * minRadius, 1e-8f));
        node.vr = neckImpulse;
        node.vz = 0.0f;
        node.initialZ = node.z;
        node.initialR = node.r;

        float neckDistance = std::abs(distanceFromContact);
        if (neckDistance < closestNeckDistance)
        {
            closestNeckDistance = neckDistance;
            pair.fusionNeckNode = static_cast<std::size_t>(i);
        }
    }

    FairInitialFusionProfile(pair.fusionProfile, neckZ,
                             0.46f * minRadius, 3);
    for (FusionProfileNode &node : pair.fusionProfile)
    {
        node.initialZ = node.z;
        node.initialR = node.r;
    }

    // Keep the persistent local profile at its own gas-volume centroid.
    // Shifting the world centre by the opposite amount preserves every
    // vertex's world-space position at the topology event.
    float localCentroid = FusionProfileCentroidZ(pair.fusionProfile);
    for (FusionProfileNode &node : pair.fusionProfile)
    {
        node.z -= localCentroid;
        node.initialZ -= localCentroid;
    }
    pair.fusionCenter = weightedCenter + axis * localCentroid;
    pair.theoreticalUncompressedVolume = theoreticalVolume;
    pair.conservedFusionVolume =
        std::max(FusionProfileVolume(pair.fusionProfile), 1e-6f);
    pair.targetVolume = pair.conservedFusionVolume;
    pair.initialFusionArea = FusionProfileArea(pair.fusionProfile);
    pair.currentFusionArea = pair.initialFusionArea;
    pair.fusionRmsSpeed = 0.0f;
    pair.fusionCurvatureVariation = 0.0f;
    pair.fusionStableTime = 0.0f;
    pair.maximumRelativeVolumeError = 0.0f;
    pair.maximumCentroidError = 0.0f;
    pair.fusionShapeMode = MeasureFusionShapeMode(pair.fusionProfile);
    pair.previousFusionShapeMode = pair.fusionShapeMode;
    pair.fusionShapeZeroCrossings = 0;
    pair.fusionProfileInitialized = true;

    pair.fusionDiagnosticNodeIndices.clear();
    pair.fusionDiagnosticTrails.clear();
    const std::size_t last = pair.fusionProfile.size() - 1;
    pair.fusionDiagnosticNodeIndices = {
        last / 4, pair.fusionNeckNode, (3 * last) / 4};
    pair.fusionDiagnosticTrails.resize(
        pair.fusionDiagnosticNodeIndices.size());

    std::cout << "[FusionProfile] topology init: theoretical_volume="
              << pair.theoreticalUncompressedVolume
              << ", conserved_closed_surface="
              << pair.conservedFusionVolume
              << ", pre_contact_compression="
              << (1.0f - pair.conservedFusionVolume /
                              pair.theoreticalUncompressedVolume) *
                     100.0f
              << "%, nodes=" << pair.fusionProfile.size() << std::endl;
}

static void UpdateBubblePair(BubbleContactPair &pair, float dt)
{
    int aIndex = -1;
    int bIndex = -1;
    if (!pair.active || !ResolveContactPairIndices(g_DisplayBubbles, pair, aIndex, bIndex))
    {
        pair.contactTime = std::max(0.0f, pair.contactTime - dt * 1.8f);
        pair.bridgeStrength = std::max(0.0f, pair.bridgeStrength - dt * 2.4f);
        pair.geometryBlend = std::max(0.0f, pair.geometryBlend - dt * 2.0f);
        if (!pair.candidate)
        {
            pair.contactRadius = std::max(0.0f, pair.contactRadius - dt * 0.5f);
        }
        pair.contactRadiusVelocity *= std::exp(-dt * 8.0f);
        pair.interactionVelocity *= std::exp(-dt * 6.0f);
        pair.interactionCompression = std::max(0.0f,
                                               pair.interactionCompression + pair.interactionVelocity * dt - dt * 0.9f);
        pair.filmThickness += (1.0f - pair.filmThickness) * std::min(1.0f, dt * 1.6f);
        pair.ruptureRisk = 0.0f;
        pair.state = BubbleContactPair::State::Free;
        return;
    }

    DisplayBubble &a = g_DisplayBubbles[(size_t)aIndex];
    DisplayBubble &b = g_DisplayBubbles[(size_t)bIndex];
    glm::vec3 delta = b.position - a.position;
    float dist = glm::length(delta);
    if (dist < 1e-4f)
    {
        return;
    }

    glm::vec3 n = delta / dist;
    float contactDistance = a.radius + b.radius;
    float minRadius = std::min(a.radius, b.radius);
    float proximityDistance = contactDistance + minRadius * 0.012f;
    float proximity = Smooth01((proximityDistance - dist) / std::max(proximityDistance - contactDistance, 0.001f));
    bool inContact = pair.bonded || dist <= proximityDistance || pair.contactTime > 0.0f;

    if (!inContact)
    {
        pair.state = BubbleContactPair::State::Free;
        pair.contactTime = std::max(0.0f, pair.contactTime - dt * 1.8f);
        pair.bridgeStrength = std::max(0.0f, pair.bridgeStrength - dt * 2.4f);
        pair.geometryBlend = std::max(0.0f, pair.geometryBlend - dt * 2.0f);
        if (!pair.candidate)
        {
            pair.contactRadius = std::max(0.0f, pair.contactRadius - dt * 0.5f);
        }
        pair.contactRadiusVelocity *= std::exp(-dt * 8.0f);
        pair.interactionVelocity *= std::exp(-dt * 6.0f);
        pair.interactionCompression = std::max(0.0f,
                                               pair.interactionCompression + pair.interactionVelocity * dt - dt * 0.9f);
        pair.filmThickness += (1.0f - pair.filmThickness) * std::min(1.0f, dt * 1.6f);
        pair.ruptureRisk = 0.0f;
        return;
    }

    pair.visualTransitionTime = std::min(kContactVisualTransitionTime,
                                         pair.visualTransitionTime + dt);

    float relNormalSpeed = glm::dot(b.velocity - a.velocity, n);
    float contactAmount = std::max(proximity, Smooth01((contactDistance - dist + minRadius * 0.10f) / (minRadius * 0.26f)));
    float compression = contactAmount;
    pair.contactTime += dt * (0.45f + 0.90f * contactAmount);
    pair.filmThickness = std::max(0.12f, pair.filmThickness - dt * (0.018f + 0.050f * contactAmount));
    pair.ruptureRisk = Smooth01((0.35f - pair.filmThickness) / 0.09f) * Smooth01((std::abs(relNormalSpeed) - 0.55f) / 0.55f);
    DecideCoalescenceOutcome(pair, a, b, relNormalSpeed);
    if (pair.outcome == BubbleContactPair::CoalescenceOutcome::SeparateAfterContact &&
        pair.contactTime >= 0.72f)
    {
        if (!pair.separationImpulseApplied)
        {
            float invMassA = BubbleInvMass(a);
            float invMassB = BubbleInvMass(b);
            float invMassSum = std::max(invMassA + invMassB, 0.001f);
            float separationSpeed = glm::mix(
                0.10f, 0.18f,
                Smooth01(std::abs(relNormalSpeed) / 0.45f));
            a.velocity -= n * (separationSpeed * invMassA / invMassSum);
            b.velocity += n * (separationSpeed * invMassB / invMassSum);
            pair.separationImpulseApplied = true;
        }

        pair.separationElapsed += dt;
        pair.bonded = false;
        pair.candidate = pair.separationElapsed < 0.55f;
        pair.persistentRenderPair = pair.separationElapsed < 0.95f;
        pair.contactActivation *= std::exp(-dt * 3.8f);
        pair.bridgeStrength *= std::exp(-dt * 3.2f);
        pair.geometryBlend *= std::exp(-dt * 2.8f);
        pair.contactRadius = std::max(0.0f, pair.contactRadius - dt * minRadius * 0.42f);
        pair.filmThickness += (1.0f - pair.filmThickness) * std::min(1.0f, dt * 1.8f);
        pair.state = BubbleContactPair::State::Separated;
        a.state = DisplayBubble::State::Separated;
        b.state = DisplayBubble::State::Separated;
        if (!pair.persistentRenderPair)
        {
            pair.active = false;
        }
        return;
    }
    bool keepStableDouble =
        pair.outcome == BubbleContactPair::CoalescenceOutcome::StayDoubleBubble;
    if (keepStableDouble)
    {
        pair.filmThickness = std::max(pair.filmThickness, 0.31f);
    }
    float sharedFilmProgress = Smooth01(std::max(pair.contactTime - kSharedFilmTime, 0.0f) /
                                        std::max(kNeckFormationTime - kSharedFilmTime, 0.001f));
    float bondBlend = Smooth01(pair.contactTime / 0.30f);
    bool canCoalesce =
        pair.outcome == BubbleContactPair::CoalescenceOutcome::WillCoalesce ||
        (g_InteractionDemoActive && pair.outcome == BubbleContactPair::CoalescenceOutcome::Undecided);
    if (canCoalesce && pair.contactTime >= kNeckFormationTime &&
        !pair.fusionActive && PairCanBeginExclusiveFusion(pair))
    {
        pair.fusionActive = true;
        pair.fusionElapsed = 0.0f;
        pair.ruptureProgress = 0.0f;
        pair.neckProgress = 0.0f;
        pair.relaxationProgress = 0.0f;
        pair.fusionCompletionHold = 0.0f;
        UpdateTransportedFusionFrame(pair, n);
        InitializeFusionProfile(pair, a, b, pair.fusionAxis);
    }
    if (pair.fusionActive && !pair.fusionComplete &&
        pair.fusionProfileInitialized)
    {
        pair.fusionElapsed += dt;
        pair.ruptureProgress =
            Smooth01(pair.fusionElapsed / kFilmRuptureDuration);

        FusionProfileDiagnostics diagnostics = AdvanceFusionProfile(
            pair.fusionProfile,
            pair.conservedFusionVolume,
            dt,
            kFusionProfileConfig);
        pair.fusionDiagnostics = diagnostics;
        pair.currentFusionArea = diagnostics.area;
        pair.fusionRmsSpeed = diagnostics.rmsSpeed;
        pair.fusionCurvatureVariation = diagnostics.curvatureVariation;
        float relativeVolumeError =
            std::abs(diagnostics.volume - pair.conservedFusionVolume) /
            std::max(pair.conservedFusionVolume, 1e-7f);
        pair.maximumRelativeVolumeError =
            std::max(pair.maximumRelativeVolumeError, relativeVolumeError);
        pair.maximumCentroidError =
            std::max(pair.maximumCentroidError,
                     std::abs(diagnostics.centroidZ));

        pair.previousFusionShapeMode = pair.fusionShapeMode;
        pair.fusionShapeMode =
            MeasureFusionShapeMode(pair.fusionProfile);
        if (pair.previousFusionShapeMode *
                pair.fusionShapeMode < 0.0f)
        {
            ++pair.fusionShapeZeroCrossings;
        }

        float finalRadius =
            RadiusFromVolume(pair.conservedFusionVolume);
        float opticalRelaxation =
            1.0f - std::exp(-0.70f * dt);
        for (FusionProfileNode &node : pair.fusionProfile)
        {
            node.opticalRadiusScale +=
                (finalRadius - node.opticalRadiusScale) *
                opticalRelaxation;
        }
        const FusionProfileNode &neckNode =
            pair.fusionProfile[std::min(pair.fusionNeckNode,
                                        pair.fusionProfile.size() - 1)];
        pair.neckRadius = neckNode.r;
        pair.neckProgress = glm::clamp(
            (pair.neckRadius - 0.14f * minRadius) /
                std::max(0.62f * finalRadius -
                             0.14f * minRadius,
                         1e-5f),
            0.0f, 1.0f);

        // This is an energy diagnostic only. It is not consumed by vertex
        // generation or by the dynamics.
        float sphereArea =
            4.0f * 3.14159265358979323846f *
            finalRadius * finalRadius;
        pair.relaxationProgress = glm::clamp(
            1.0f -
                (pair.currentFusionArea - sphereArea) /
                    std::max(pair.initialFusionArea - sphereArea, 1e-6f),
            0.0f, 1.0f);
        pair.filmThickness = std::min(
            pair.filmThickness,
            glm::mix(kFusionFilmThickness, 0.08f,
                     pair.ruptureProgress));

        glm::vec3 worldCenter =
            pair.fusionCenter +
            pair.fusionCenterVelocity * pair.fusionElapsed;
        for (std::size_t trailIndex = 0;
             trailIndex < pair.fusionDiagnosticNodeIndices.size();
             ++trailIndex)
        {
            std::size_t nodeIndex = std::min(
                pair.fusionDiagnosticNodeIndices[trailIndex],
                pair.fusionProfile.size() - 1);
            const FusionProfileNode &node =
                pair.fusionProfile[nodeIndex];
            glm::vec3 worldPosition =
                worldCenter + pair.fusionAxis * node.z +
                pair.fusionSide * node.r;
            std::vector<glm::vec3> &trail =
                pair.fusionDiagnosticTrails[trailIndex];
            trail.push_back(worldPosition);
            if (trail.size() > 180)
                trail.erase(trail.begin());
        }

        bool dynamicallySettled =
            pair.fusionElapsed > 1.0f &&
            pair.fusionShapeZeroCrossings >= 1 &&
            pair.fusionRmsSpeed <= kFusionStableRmsSpeed &&
            pair.fusionCurvatureVariation <=
                kFusionStableCurvatureVariation &&
            relativeVolumeError <= 0.001f;
        pair.fusionCompletionHold = dynamicallySettled
                                        ? pair.fusionCompletionHold + dt
                                        : 0.0f;
        pair.fusionComplete =
            pair.fusionCompletionHold >= kFusionCompletionHold;
    }

    // Film creation is history-dependent, while compression remains a
    // reversible response to current overlap, impact, and continuing forces.
    float baseFilmCompression = Smooth01(pair.contactTime / kNeckFormationTime);
    float effectiveFusionProgress = pair.fusionActive ? pair.neckProgress : 0.0f;
    float baseOverlapRatio = 0.020f * bondBlend + 0.105f * baseFilmCompression +
                             0.235f * effectiveFusionProgress;
    float actualOverlapRatio = (contactDistance - dist) / std::max(minRadius, 0.001f);
    float crowdingScale = 1.0f / std::sqrt((float)std::max(
                                     BubbleActiveContactCount(g_DisplayBubbles, g_ContactPairs, aIndex),
                                     BubbleActiveContactCount(g_DisplayBubbles, g_ContactPairs, bIndex)));
    float commandedOverlapRatio = baseOverlapRatio +
                                  0.240f * crowdingScale * pair.interactionCompression;
    float normalizedOverlap = Smooth01((actualOverlapRatio - commandedOverlapRatio) / 0.10f);
    float impactDrive = Smooth01(std::max(-relNormalSpeed, 0.0f) / 0.32f);
    float demoExternalDrive = 0.0f;
    if (g_InteractionDemoActive && pair.fusionActive)
    {
        float postMergeTime = pair.fusionElapsed;
        float forceEnvelope = Smooth01(postMergeTime / 1.6f);
        float forcePulse = 0.80f + 0.20f * std::sin(postMergeTime * 1.15f);
        demoExternalDrive = forceEnvelope * forcePulse;
    }
    float bondedCompressionDrive = pair.bonded
                                       ? 0.82f * crowdingScale *
                                             Smooth01(std::max(pair.contactTime - kSharedFilmTime, 0.0f) / 2.0f)
                                       : 0.0f;
    float compressionTarget = glm::clamp(std::max({0.72f * normalizedOverlap,
                                                   0.50f * impactDrive,
                                                   bondedCompressionDrive,
                                                   demoExternalDrive}),
                                         0.0f, 1.0f);
    float compressionAcceleration =
        (compressionTarget - pair.interactionCompression) * 13.0f -
        pair.interactionVelocity * 5.8f;
    pair.interactionVelocity += compressionAcceleration * dt;
    pair.interactionCompression = glm::clamp(
        pair.interactionCompression + pair.interactionVelocity * dt, 0.0f, 1.0f);
    float filmGrowth = std::max(
        std::pow(Smooth01(pair.contactTime / 1.05f), 0.55f),
        0.45f * pair.contactActivation);
    float crowdingRetention = std::pow(crowdingScale, 0.12f);
    float stableDoubleScale = keepStableDouble ? 0.82f : 1.0f;
    float targetContactRatio = (0.52f + 0.22f * pair.interactionCompression) *
                               stableDoubleScale *
                               crowdingRetention;
    float targetContactRadius = minRadius * targetContactRatio * filmGrowth;
    float radiusAcceleration = (targetContactRadius - pair.contactRadius) * 42.0f -
                               pair.contactRadiusVelocity * 14.0f;
    pair.contactRadiusVelocity += radiusAcceleration * dt;
    pair.contactRadius = glm::clamp(pair.contactRadius + pair.contactRadiusVelocity * dt,
                                    0.0f, minRadius * 0.78f);

    float planeOffsetA = std::sqrt(std::max(a.radius * a.radius -
                                                pair.contactRadius * pair.contactRadius,
                                            0.0f));
    glm::vec3 targetPlaneCenter = a.position + n * planeOffsetA;
    if (!pair.contactFrameInitialized)
    {
        pair.filteredNormal = n;
        pair.filteredPlaneCenter = targetPlaneCenter;
        pair.contactFrameInitialized = true;
    }
    else
    {
        float frameBlend = 1.0f - std::exp(-dt * 10.0f);
        glm::vec3 blendedNormal = glm::mix(pair.filteredNormal, n, frameBlend);
        if (glm::length(blendedNormal) > 1e-5f)
        {
            pair.filteredNormal = glm::normalize(blendedNormal);
        }
        pair.filteredPlaneCenter = glm::mix(pair.filteredPlaneCenter,
                                            targetPlaneCenter, frameBlend);
    }
    UpdateTransportedFusionFrame(pair, pair.filteredNormal);

    pair.bridgeStrength = Smooth01(pair.contactTime / 0.55f) * contactAmount;
    float geometryTarget = glm::mix(sharedFilmProgress, 1.0f, effectiveFusionProgress) * contactAmount;
    if (keepStableDouble)
    {
        geometryTarget = std::min(geometryTarget, 0.78f);
    }
    pair.geometryBlend = glm::clamp(geometryTarget, 0.0f, 1.0f);
    if (pair.contactTime < kSharedFilmTime || contactAmount < 0.10f)
    {
        pair.state = BubbleContactPair::State::Touch;
    }
    else if (keepStableDouble || !pair.fusionActive)
    {
        pair.state = BubbleContactPair::State::SharedFilm;
    }
    else if (pair.relaxationProgress <= 0.001f)
    {
        pair.state = BubbleContactPair::State::NeckForming;
    }
    else
    {
        pair.state = BubbleContactPair::State::Merged;
        pair.bridgeStrength = 1.0f;
        pair.geometryBlend = 1.0f;
        pair.filmThickness = std::min(pair.filmThickness, kFusionFilmThickness);
    }
    if (!pair.fusionActive)
        pair.neckRadius = PlateauContactRadius(pair, a, b);

    if (pair.state == BubbleContactPair::State::Touch)
    {
        a.state = DisplayBubble::State::Touch;
    }
    else if (pair.state == BubbleContactPair::State::SharedFilm)
    {
        a.state = DisplayBubble::State::SharedFilm;
    }
    else if (pair.state == BubbleContactPair::State::NeckForming)
    {
        a.state = DisplayBubble::State::NeckForming;
    }
    else
    {
        a.state = DisplayBubble::State::Merged;
    }
    b.state = a.state;
    float visualMergeProgress = std::max(pair.neckProgress * 0.55f,
                                         pair.relaxationProgress);
    a.mergeProgress = visualMergeProgress;
    b.mergeProgress = visualMergeProgress;
    a.contactTime = pair.contactTime;
    b.contactTime = pair.contactTime;
    float visibleContact = glm::clamp(pair.bridgeStrength * (0.55f + 0.65f * pair.geometryBlend), 0.0f, 1.0f);
    a.contactStrength = visibleContact;
    b.contactStrength = visibleContact;
    a.filmThickness = pair.filmThickness;
    b.filmThickness = pair.filmThickness;
    a.contactAxis = n;
    b.contactAxis = -n;
    if (pair.bridgeStrength > g_BridgeStrength)
    {
        g_BridgeStrength = pair.bridgeStrength;
        g_BridgeDirection = glm::normalize((a.position + b.position) * 0.5f);
        g_AutoTouchVelocity = 0.35f + pair.bridgeStrength * 0.45f;
    }
}

static void UpdateDisplayBubbleInteractions(float dt)
{
    dt = glm::clamp(dt, 0.0f, 1.0f / 30.0f);
    static double s_LastContactLogTime = -1.0;
    g_BridgeStrength = 0.0f;
    g_AutoTouchStrength *= expf(dt * -4.0f);
    g_AutoTouchVelocity *= expf(dt * -5.0f);

    if (g_SimPaused)
    {
        return;
    }

    std::vector<glm::vec3> previousPositions;
    previousPositions.reserve(g_DisplayBubbles.size());
    for (const auto &bubble : g_DisplayBubbles)
    {
        previousPositions.push_back(bubble.position);
    }

    for (auto &pair : g_ContactPairs)
    {
        if (!pair.bonded)
        {
            pair.preContactProgress *= std::exp(-dt * 1.2f);
            pair.contactActivation *= std::exp(-dt * 8.0f);
        }
        int aIndex = -1;
        int bIndex = -1;
        if (pair.candidate && !pair.bonded &&
            ResolveContactPairIndices(g_DisplayBubbles, pair, aIndex, bIndex))
        {
            const DisplayBubble &a = g_DisplayBubbles[(size_t)aIndex];
            const DisplayBubble &b = g_DisplayBubbles[(size_t)bIndex];
            float minRadius = std::min(a.radius, b.radius);
            float releaseDistance = a.radius + b.radius + minRadius * 0.16f;
            if (glm::length(b.position - a.position) > releaseDistance)
            {
                pair.candidateExitTime += dt;
                if (pair.candidateExitTime > 0.28f)
                {
                    pair.candidate = false;
                    pair.candidateExitTime = 0.0f;
                }
            }
            else
            {
                pair.candidateExitTime = 0.0f;
            }
        }
        pair.active = pair.persistentRenderPair || pair.bonded || pair.candidate;
    }
    if (g_InteractionDemoActive)
    {
        for (auto &pair : g_ContactPairs)
        {
            int aIndex = -1;
            int bIndex = -1;
            if (ResolveContactPairIndices(g_DisplayBubbles, pair, aIndex, bIndex))
            {
                pair.active = true;
            }
        }
    }

    for (auto &bubble : g_DisplayBubbles)
    {
        if (bubble.state == DisplayBubble::State::Dead)
        {
            continue;
        }

    if (bubble.state == DisplayBubble::State::Burst)
    {
        // If we have a sim, physics drives the retraction — only manage
        // alpha fade.  Otherwise fall back to manual burstScale shrink.
        bubble.burstElapsed += dt;
        float t = glm::clamp(bubble.burstElapsed / bubble.burstDuration,
                             0.0f, 1.0f);
        auto *sim = GetBubbleSim(bubble.id);
        if (!sim)
        {
            float shrink = 1.0f - t * t * (3.0f - 2.0f * t);
            bubble.burstScale = std::max(shrink, 0.0f);
        }
        bubble.alpha = std::max(1.0f - t * 2.5f, 0.0f);
        bubble.velocity *= expf(dt * -2.5f);
        bubble.position += bubble.velocity * dt;
        if (t >= 1.0f)
            {
                bubble.state = DisplayBubble::State::Dead;
                bubble.alpha = 0.0f;
            }
            continue;
        }

        bool mergedBubble = bubble.volumeTransferred;
        bubble.state = mergedBubble
                           ? DisplayBubble::State::Merged
                           : DisplayBubble::State::Free;
        float swayScale = mergedBubble
                              ? 0.90f
                              : (g_InteractionDemoActive ? 0.45f : 1.0f);
        float swayAmplitude = (0.028f + bubble.floatAmplitude * 1.4f) * swayScale;
        glm::vec3 swayOffset = glm::vec3(
                                   sinf((float)g_Time * bubble.speed + bubble.phase) * 0.85f,
                                   cosf((float)g_Time * bubble.speed * 0.73f + bubble.phase * 1.31f),
                                   sinf((float)g_Time * bubble.speed * 0.57f + bubble.phase * 0.79f) * 0.48f) *
                               swayAmplitude;
        float swayPullRate = mergedBubble
                                 ? 0.22f
                                 : (g_InteractionDemoActive ? 0.16f : 0.34f);
        glm::vec3 swayPull =
            (bubble.basePosition + swayOffset - bubble.position) * swayPullRate;
        float windRadiusResponse = glm::clamp(std::sqrt(0.40f / std::max(bubble.radius, 0.08f)),
                                              0.58f, 1.35f);
        glm::vec3 globalWind = g_WindEnabled
                                   ? g_GlobalWindDirection * (g_GlobalWindStrength * windRadiusResponse)
                                   : glm::vec3(0.0f);
        bubble.velocity += (swayPull + globalWind) * dt;
        bubble.velocity *= expf(dt * (mergedBubble ? -0.72f : -0.48f));
        bubble.position += bubble.velocity * dt;
        if (mergedBubble)
        {
            bubble.contactStrength = 0.0f;
            bubble.surfaceDynamicsBlend = 0.0f;
        }
        else
        {
            bubble.contactStrength *= expf(dt * -3.0f);
        }
        if (bubble.targetVolume > 0.0f)
        {
            float targetRadius = RadiusFromVolume(bubble.targetVolume);
            bubble.radius += (targetRadius - bubble.radius) * std::min(1.0f, dt * 1.8f);
        }
        if (bubble.state == DisplayBubble::State::Free)
        {
            bubble.filmThickness += (1.0f - bubble.filmThickness) * std::min(1.0f, dt * 0.4f);
        }
    }

    // Remove bubbles that finished their burst animation.
    std::vector<uint64_t> deadBubbleIds;
    for (const auto &bubble : g_DisplayBubbles)
    {
        if (bubble.state == DisplayBubble::State::Dead)
        {
            deadBubbleIds.push_back(bubble.id);
        }
    }
    for (uint64_t id : deadBubbleIds)
    {
        RemoveBubble(id);
        std::cout << "[BubbleBurst] removed id=" << id
                  << " live=" << LiveDisplayBubbleCount() << std::endl;
    }

    // The interaction replay is a controlled visual experiment. Move the two
    // bubbles toward first contact at a deterministic rate so frame rate,
    // damping, and the weak ambient drift cannot stall the demonstration.
    if (g_InteractionDemoActive)
    {
        for (auto &pair : g_ContactPairs)
        {
            int aIndex = -1;
            int bIndex = -1;
            if (!pair.active || pair.contactTime > 0.0f ||
                !ResolveContactPairIndices(g_DisplayBubbles, pair, aIndex, bIndex))
            {
                continue;
            }
            DisplayBubble &a = g_DisplayBubbles[(size_t)aIndex];
            DisplayBubble &b = g_DisplayBubbles[(size_t)bIndex];
            glm::vec3 delta = b.position - a.position;
            float dist = glm::length(delta);
            if (dist <= 1e-5f)
            {
                continue;
            }
            glm::vec3 n = delta / dist;
            float targetDistance = a.radius + b.radius + std::min(a.radius, b.radius) * 0.006f;
            float remaining = dist - targetDistance;
            if (remaining > 0.0f)
            {
                float step = std::min(remaining, dt * 0.115f);
                float invMassA = BubbleInvMass(a);
                float invMassB = BubbleInvMass(b);
                float invMassSum = invMassA + invMassB;
                a.position += n * step * (invMassA / invMassSum);
                b.position -= n * step * (invMassB / invMassSum);
            }
        }
    }

    std::vector<std::pair<int, int>> broadPhasePairs = BuildBubbleBroadPhasePairs(g_DisplayBubbles);
    for (const auto &candidatePair : broadPhasePairs)
    {
        int i = candidatePair.first;
        int j = candidatePair.second;
        auto &a = g_DisplayBubbles[(size_t)i];
        auto &b = g_DisplayBubbles[(size_t)j];

        // Never create or reactivate contacts involving bursting/dead bubbles.
        if (a.state == DisplayBubble::State::Burst ||
            a.state == DisplayBubble::State::Dead ||
            b.state == DisplayBubble::State::Burst ||
            b.state == DisplayBubble::State::Dead)
        {
            continue;
        }

        int existingPairIndex = FindContactPair(g_ContactPairs, a.id, b.id);
        if (existingPairIndex >= 0)
        {
            const BubbleContactPair &existingPair =
                g_ContactPairs[(size_t)existingPairIndex];
            if (existingPair.outcome ==
                    BubbleContactPair::CoalescenceOutcome::SeparateAfterContact &&
                existingPair.separationElapsed < 1.25f)
            {
                continue;
            }
        }

        glm::vec3 delta = b.position - a.position;
        float dist = glm::length(delta);
        float contactDistance = a.radius + b.radius;
        float minRadius = std::min(a.radius, b.radius);
        float nearRange = minRadius * 0.080f;
        float contactEpsilon = minRadius * 0.012f;
        if (dist > 1e-4f && dist < contactDistance + nearRange)
        {
            glm::vec3 n = delta / dist;
            float gap = dist - contactDistance;
            float contactActivation = Smooth01((nearRange - gap) /
                                               std::max(nearRange, 0.001f));
            float capillaryPull = 0.100f * contactActivation;
            a.velocity += n * capillaryPull * dt;
            b.velocity -= n * capillaryPull * dt;

            int pairIndex = EnsureContactPairByIndex(g_DisplayBubbles, g_ContactPairs, i, j);
            if (pairIndex < 0)
            {
                continue;
            }
            BubbleContactPair &pair = g_ContactPairs[(size_t)pairIndex];
            if (pair.outcome == BubbleContactPair::CoalescenceOutcome::Undecided &&
                !pair.outcomeRandomSampled)
            {
                // Sample once when this pair first enters the contact zone.
                // The stored value survives brief bounce/re-contact cycles and
                // is discarded only when the contact pair itself expires.
                pair.coalescenceRandom = NextContactOutcomeRandom01();
                pair.outcomeRandomSampled = true;
            }
            pair.active = true;
            pair.candidate = true;
            pair.candidateExitTime = 0.0f;
            pair.contactActivation = contactActivation;
            pair.preContactProgress = contactActivation;
            float preContactRadius = minRadius * 0.10f *
                                     contactActivation * contactActivation;
            pair.contactRadius = std::max(pair.contactRadius, preContactRadius);
            float planeOffsetA = std::sqrt(std::max(a.radius * a.radius -
                                                        pair.contactRadius * pair.contactRadius,
                                                    0.0f));
            glm::vec3 targetPlaneCenter = a.position + n * planeOffsetA;
            if (!pair.contactFrameInitialized)
            {
                pair.filteredNormal = n;
                pair.filteredPlaneCenter = targetPlaneCenter;
                pair.contactFrameInitialized = true;
            }
            else
            {
                float frameBlend = 1.0f - std::exp(-dt * 12.0f);
                glm::vec3 blendedNormal = glm::mix(pair.filteredNormal, n, frameBlend);
                if (glm::length(blendedNormal) > 1e-5f)
                {
                    pair.filteredNormal = glm::normalize(blendedNormal);
                }
                pair.filteredPlaneCenter = glm::mix(pair.filteredPlaneCenter,
                                                    targetPlaneCenter, frameBlend);
            }
            pair.restDistance = contactDistance;
            pair.targetVolume = a.targetVolume + b.targetVolume;
        }
        if (dist <= 1e-4f || dist > contactDistance + contactEpsilon)
        {
            continue;
        }

        int pairIndex = EnsureContactPairByIndex(g_DisplayBubbles, g_ContactPairs, i, j);
        if (pairIndex < 0)
        {
            continue;
        }
        BubbleContactPair &pair = g_ContactPairs[(size_t)pairIndex];
        bool firstBondFrame = !pair.bonded;
        pair.active = true;
        pair.bonded = true;
        pair.candidate = true;
        pair.candidateExitTime = 0.0f;
        if (firstBondFrame)
        {
            float inheritedContact = glm::clamp(pair.preContactProgress, 0.0f, 1.0f);
            glm::vec3 bondNormal = delta / dist;
            float incomingSpeed = std::max(-glm::dot(b.velocity - a.velocity,
                                                     bondNormal),
                                           0.0f);
            pair.contactTime = std::max(pair.contactTime, 0.20f * inheritedContact);
            pair.contactRadius = std::max(pair.contactRadius,
                                          minRadius * 0.14f * inheritedContact);
            pair.contactRadiusVelocity = std::max(pair.contactRadiusVelocity,
                                                  incomingSpeed * 0.22f);
            pair.interactionCompression = std::max(pair.interactionCompression,
                                                   0.10f * inheritedContact);
            pair.interactionVelocity = std::max(pair.interactionVelocity,
                                                incomingSpeed / std::max(minRadius, 0.001f) * 0.06f);
        }
        pair.restDistance = contactDistance;
        pair.targetVolume = a.targetVolume + b.targetVolume;
    }

    const int solverIterations = 6;
    const float mainRadius = g_MainBubbleVisualRadius;
    for (int iter = 0; iter < solverIterations; ++iter)
    {
        for (auto &pair : g_ContactPairs)
        {
            int aIndex = -1;
            int bIndex = -1;
            if (!pair.active || !ResolveContactPairIndices(g_DisplayBubbles, pair, aIndex, bIndex))
            {
                continue;
            }

            DisplayBubble &a = g_DisplayBubbles[(size_t)aIndex];
            DisplayBubble &b = g_DisplayBubbles[(size_t)bIndex];
            if (a.state == DisplayBubble::State::Dead || b.state == DisplayBubble::State::Dead)
            {
                continue;
            }

            glm::vec3 delta = b.position - a.position;
            float dist = glm::length(delta);
            if (dist < 1e-5f)
            {
                delta = glm::vec3(1.0f, 0.0f, 0.0f);
                dist = 1.0f;
            }

            glm::vec3 n = delta / dist;
            float minRadius = std::min(a.radius, b.radius);
            float contactEpsilon = minRadius * 0.012f;
            if (!pair.bonded && dist > a.radius + b.radius + contactEpsilon)
            {
                continue;
            }
            float bondBlend = Smooth01(pair.contactTime / 0.30f);
            float contactRadius = glm::clamp(pair.contactRadius,
                                             0.0f, minRadius * 0.82f);
            float restDistance =
                std::sqrt(std::max(a.radius * a.radius - contactRadius * contactRadius, 0.0f)) +
                std::sqrt(std::max(b.radius * b.radius - contactRadius * contactRadius, 0.0f));
            float constraint = dist - restDistance;
            if (constraint >= 0.0f)
            {
                float contactMargin = minRadius * (pair.bonded ? 0.24f : 0.035f);
                float adhesionRange = contactMargin * 1.15f;
                float adhesion = Smooth01((restDistance + adhesionRange - dist) / std::max(adhesionRange, 0.001f));
                adhesion *= 0.030f + 0.090f * Smooth01(pair.contactTime / 0.85f);
                adhesion *= bondBlend;
                if (adhesion <= 0.0f)
                {
                    continue;
                }

                float invMassA = BubbleInvMass(a);
                float invMassB = BubbleInvMass(b);
                float correctionMagnitude = std::min(constraint * adhesion, minRadius * 0.0065f);
                glm::vec3 correction = n * correctionMagnitude;
                a.position += correction * invMassA / (invMassA + invMassB);
                b.position -= correction * invMassB / (invMassA + invMassB);
                continue;
            }

            float invMassA = BubbleInvMass(a);
            float invMassB = BubbleInvMass(b);
            if (pair.bonded && pair.contactTime < 0.24f)
            {
                // A soap film absorbs the initial overlap as local shape
                // deformation. Do not kick the centers apart before the
                // contact circle has had time to grow.
                continue;
            }
            float compliance = 0.0007f / std::max(dt * dt, 1e-5f);
            float correctionMagnitude = (-constraint) / (invMassA + invMassB + compliance);
            glm::vec3 correction = n * correctionMagnitude;
            a.position -= correction * invMassA;
            b.position += correction * invMassB;
        }

        for (auto &bubble : g_DisplayBubbles)
        {
            if (bubble.state == DisplayBubble::State::Dead || !g_ShowMainBubble)
            {
                continue;
            }

            glm::vec3 delta = bubble.position;
            float dist = glm::length(delta);
            if (dist < 1e-5f)
            {
                delta = glm::vec3(1.0f, 0.0f, 0.0f);
                dist = 1.0f;
            }

            glm::vec3 n = delta / dist;
            float contactProgress = Smooth01(bubble.contactTime / 1.0f);
            float restDistance = mainRadius + bubble.radius - bubble.radius * (0.025f + 0.075f * contactProgress);
            if (dist < restDistance)
            {
                bubble.position += n * (restDistance - dist);
            }
        }
    }

    if (dt > 1e-5f)
    {
        for (size_t i = 0; i < g_DisplayBubbles.size(); ++i)
        {
            auto &bubble = g_DisplayBubbles[i];
            if (bubble.state == DisplayBubble::State::Dead)
            {
                continue;
            }

            bubble.velocity = (bubble.position - previousPositions[i]) / dt;
            float speed = glm::length(bubble.velocity);
            if (speed > 1.20f)
            {
                bubble.velocity *= 1.20f / speed;
            }
        }
    }

    for (auto &pair : g_ContactPairs)
    {
        int aIndex = -1;
        int bIndex = -1;
        if (!pair.active || !ResolveContactPairIndices(g_DisplayBubbles, pair, aIndex, bIndex))
        {
            UpdateBubblePair(pair, dt);
            continue;
        }

        DisplayBubble &a = g_DisplayBubbles[(size_t)aIndex];
        DisplayBubble &b = g_DisplayBubbles[(size_t)bIndex];
        glm::vec3 delta = b.position - a.position;
        float dist = glm::length(delta);
        float contactDistance = a.radius + b.radius;
        float contactEpsilon = std::min(a.radius, b.radius) * 0.012f;
        bool resolveContactImpulse = pair.candidate || pair.bonded ||
                                     dist <= contactDistance + contactEpsilon;
        if (dist > 1e-5f && resolveContactImpulse)
        {
            glm::vec3 n = delta / dist;
            float relNormalSpeed = glm::dot(b.velocity - a.velocity, n);
            if (pair.bonded || relNormalSpeed < 0.0f)
            {
                float invMassA = BubbleInvMass(a);
                float invMassB = BubbleInvMass(b);
                float activation = glm::clamp(pair.contactActivation, 0.0f, 1.0f);
                float dampingRate = pair.bonded
                                        ? glm::mix(1.2f, 6.2f, activation)
                                        : glm::mix(0.5f, 3.5f, activation);
                float normalDamping = 1.0f - std::exp(-dampingRate * dt);
                float absorbedClosingSpeed = std::max(-relNormalSpeed, 0.0f) * normalDamping;
                if (absorbedClosingSpeed > 0.0f)
                {
                    float minRadius = std::max(std::min(a.radius, b.radius), 0.001f);
                    pair.contactRadiusVelocity += absorbedClosingSpeed * 0.25f;
                    pair.interactionVelocity += absorbedClosingSpeed / minRadius * 0.05f;
                }
                float impulseMagnitude = (-relNormalSpeed * normalDamping) /
                                         (invMassA + invMassB);
                glm::vec3 impulse = n * impulseMagnitude;
                a.velocity -= impulse * invMassA;
                b.velocity += impulse * invMassB;
            }
        }
        UpdateBubblePair(pair, dt);
    }

    for (auto &bubble : g_DisplayBubbles)
    {
        if (bubble.state == DisplayBubble::State::Dead || !g_ShowMainBubble)
        {
            continue;
        }

        glm::vec3 delta = bubble.position;
        float dist = glm::length(delta);
        if (dist < 1e-5f)
        {
            continue;
        }

        glm::vec3 n = delta / dist;
        float contactDistance = mainRadius + bubble.radius;
        float proximityDistance = contactDistance + bubble.radius * 0.28f;
        if (dist < proximityDistance)
        {
            float strength = Smooth01((proximityDistance - dist) / std::max(proximityDistance - contactDistance, 0.001f));
            bubble.contactTime += dt * (0.35f + 0.65f * strength);
            bubble.contactStrength = std::max(bubble.contactStrength, strength * 0.55f);
            bubble.filmThickness = std::max(0.32f, bubble.filmThickness - dt * 0.035f * strength);
            bubble.contactAxis = -n;
            bubble.state = strength > 0.65f ? DisplayBubble::State::SharedFilm : DisplayBubble::State::Touch;
            float relSpeed = glm::dot(bubble.velocity, n);
            if (relSpeed < 0.0f)
            {
                bubble.velocity -= n * relSpeed * 0.72f;
            }

            if (strength > g_BridgeStrength)
            {
                g_BridgeStrength = strength;
                g_BridgeDirection = n;
                g_AutoTouchVelocity = 0.25f + strength * 0.55f;
            }
        }
        else
        {
            bubble.contactTime = std::max(0.0f, bubble.contactTime - dt * 1.2f);
        }
    }

    UpdatePersistentSurfaceDynamics(dt);
    FinalizeCompletedFusions();

    for (const BubbleContactPair &pair : g_ContactPairs)
    {
        if (!pair.active && pair.contactTime <= 0.01f && pair.geometryBlend <= 0.01f)
        {
            g_BubbleSurfaces.RemoveContact(pair.a, pair.b);
        }
    }
    g_ContactPairs.erase(
        std::remove_if(g_ContactPairs.begin(), g_ContactPairs.end(), [](const BubbleContactPair &pair)
                       { return !pair.active && pair.contactTime <= 0.01f && pair.geometryBlend <= 0.01f; }),
        g_ContactPairs.end());

    if (g_LogContactDebug && !g_ContactPairs.empty() && g_Time - s_LastContactLogTime >= 1.0)
    {
        const auto &pair = g_ContactPairs.front();
        int aIndex = -1;
        int bIndex = -1;
        if (ResolveContactPairIndices(g_DisplayBubbles, pair, aIndex, bIndex))
        {
            const auto &a = g_DisplayBubbles[(size_t)aIndex];
            const auto &b = g_DisplayBubbles[(size_t)bIndex];
            float dist = glm::length(b.position - a.position);
            std::cout << "[BubbleContact] dist=" << dist
                      << " sumR=" << (a.radius + b.radius)
                      << " active=" << pair.active
                      << " bonded=" << pair.bonded
                      << " candidate=" << pair.candidate
                      << " state=" << (int)pair.state
                      << " t=" << pair.contactTime
                      << " bridge=" << pair.bridgeStrength
                      << " geom=" << pair.geometryBlend
                      << " compression=" << pair.interactionCompression
                      << " activation=" << pair.contactActivation
                      << " outcome=" << CoalescenceOutcomeName(pair.outcome)
                      << " coalesceScore=" << pair.coalescenceScore
                      << " contactR=" << pair.contactRadius
                      << " film=" << pair.filmThickness
                      << std::endl;
            s_LastContactLogTime = g_Time;
        }
    }
}

static void UpdateFPS(GLFWwindow *window, double currentTime)
{
    ++g_FPSFrameCount;
    if (g_FPSLastUpdate == 0.0)
        g_FPSLastUpdate = currentTime;

    double elapsed = currentTime - g_FPSLastUpdate;
    if (elapsed >= 0.25)
    {
        g_CurrentFPS = (float)((double)g_FPSFrameCount / elapsed);
        g_FPSFrameCount = 0;
        g_FPSLastUpdate = currentTime;

        char title[160];
        std::snprintf(title, sizeof(title), "%s | FPS %.1f", g_AppTitle, g_CurrentFPS);
        glfwSetWindowTitle(window, title);
    }
}

// ============================================================
// Mouse state
// ============================================================
static bool g_MousePressed = false;
static bool g_TouchPressed = false;
static float g_LastMouseX = 0.0f;
static float g_LastMouseY = 0.0f;
static float g_LastTouchX = 0.0f;
static float g_LastTouchY = 0.0f;
static glm::vec2 g_TouchPoint = glm::vec2(-10.0f, -10.0f);
static float g_TouchStrength = 0.0f;
static float g_TouchVelocity = 0.0f;

static double g_LastFrameTime = 0.0;
static double g_DeltaTime = 0.0;

// ============================================================
// Cubemap loading (filesystem version)
// ============================================================
static GLuint LoadCubemapTexture(const std::vector<std::string> &faceFiles)
{
    GLuint textureID;
    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_CUBE_MAP, textureID);

    int width, height, nrChannels;
    for (unsigned int i = 0; i < faceFiles.size(); i++)
    {
        stbi_set_flip_vertically_on_load(false);
        unsigned char *data = stbi_load(faceFiles[i].c_str(), &width, &height, &nrChannels, 0);
        if (data)
        {
            GLenum format = (nrChannels == 4) ? GL_RGBA : GL_RGB;
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, format,
                         width, height, 0, format, GL_UNSIGNED_BYTE, data);
            stbi_image_free(data);
        }
        else
        {
            std::cerr << "Failed to load cubemap face: " << faceFiles[i] << std::endl;
            stbi_image_free(data);
            glDeleteTextures(1, &textureID);
            return 0;
        }
    }

    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

    return textureID;
}

static GLuint LoadTexture2D(const std::string &filePath)
{
    int width, height, nrChannels;
    stbi_set_flip_vertically_on_load(false);
    unsigned char *data = stbi_load(filePath.c_str(), &width, &height, &nrChannels, 0);
    if (!data)
    {
        std::cerr << "Failed to load texture: " << filePath << std::endl;
        return 0;
    }

    GLenum format = (nrChannels == 4) ? GL_RGBA : GL_RGB;
    GLuint textureID;
    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_2D, textureID);
    glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    stbi_image_free(data);
    return textureID;
}

// ============================================================
// FBO initialization
// ============================================================
static void InitFBO(int w, int h)
{
    // Overscan: FBO is larger than screen to provide margin for refraction offsets
    g_FBOWidth = static_cast<int>(w * kFBOOverscan);
    g_FBOHeight = static_cast<int>(h * kFBOOverscan);
    int fboW = g_FBOWidth;
    int fboH = g_FBOHeight;

    float borderColor[] = {0.0f, 0.0f, 0.0f, 0.0f};

    glGenRenderbuffers(1, &g_DepthRB);
    glBindRenderbuffer(GL_RENDERBUFFER, g_DepthRB);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, fboW, fboH);

    // -------- backgroundFBO --------
    glGenTextures(1, &g_BackgroundTexture);
    glBindTexture(GL_TEXTURE_2D, g_BackgroundTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fboW, fboH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);

    glGenFramebuffers(1, &g_BackgroundFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, g_BackgroundFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_BackgroundTexture, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_DepthRB);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        std::cerr << "Background FBO incomplete!" << std::endl;
    }

    // -------- sceneBehindFBO --------
    // Contains the visible scene behind the main bubble, including display bubbles.
    // The main bubble samples this texture through its two-pass screen-space refraction.
    glGenTextures(1, &g_SceneBehindTexture);
    glBindTexture(GL_TEXTURE_2D, g_SceneBehindTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fboW, fboH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);

    glGenFramebuffers(1, &g_SceneBehindFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, g_SceneBehindFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_SceneBehindTexture, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_DepthRB);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        std::cerr << "Scene-behind FBO incomplete!" << std::endl;
    }

    // -------- mainSceneFBO --------
    // Contains background + main bubble only. Display bubbles sample it in the
    // final screen pass so a foreground small bubble can reveal the large one.
    glGenTextures(1, &g_MainSceneTexture);
    glBindTexture(GL_TEXTURE_2D, g_MainSceneTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fboW, fboH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);

    glGenFramebuffers(1, &g_MainSceneFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, g_MainSceneFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_MainSceneTexture, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_DepthRB);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        std::cerr << "Main-scene FBO incomplete!" << std::endl;
    }

    // -------- backFaceFBO --------
    glGenTextures(1, &g_BackFaceTexture);
    glBindTexture(GL_TEXTURE_2D, g_BackFaceTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fboW, fboH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);

    glGenFramebuffers(1, &g_BackFaceFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, g_BackFaceFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_BackFaceTexture, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_DepthRB);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        std::cerr << "BackFace FBO incomplete!" << std::endl;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// ============================================================
// Update camera from angles
// ============================================================
static void UpdateCamera()
{
    float radius = g_CameraDistance;
    float pitch = glm::radians(g_CurrentAngleX);
    float yaw = glm::radians(g_CurrentAngleY);
    const float maxPitch = glm::radians(89.0f);
    pitch = glm::clamp(pitch, -maxPitch, maxPitch);

    glm::vec3 cameraPos;
    cameraPos.x = radius * cos(pitch) * sin(yaw);
    cameraPos.y = radius * sin(pitch);
    cameraPos.z = radius * cos(pitch) * cos(yaw);

    g_Camera.setPosition(g_CameraOrbitTarget + cameraPos);
    g_Camera.setTarget(g_CameraOrbitTarget);
}

// ============================================================
// Render one frame
// ============================================================
static void RenderFrame()
{
    int w = g_WindowWidth;
    int h = g_WindowHeight;
    if (w <= 0 || h <= 0)
        return;

    if (!g_TouchPressed)
    {
        g_TouchStrength *= expf((float)g_DeltaTime * -3.2f);
        g_TouchVelocity *= expf((float)g_DeltaTime * -5.0f);
        if (g_TouchStrength < 0.01f)
        {
            g_TouchStrength = 0.0f;
            g_TouchPoint = glm::vec2(-10.0f, -10.0f);
        }
    }

    UpdateCamera();

    // ---- DBSTT simulation step ----
    if (!g_SimPaused && ((g_SimFrameCounter++ % kSimFrameInterval) == 0))
    {
        g_Sim.update(1.0f / 60.0f); // matches HarmonyOS throttled DBSTT update cadence
    }
    UpdateDisplayBubbleInteractions((float)g_DeltaTime);

    // Run per-display-bubble DBSTT vortex sheet sims
    UpdateBubbleSims((float)g_DeltaTime);

    // Upload updated vertices from simulation to GPU
    if (g_RefractModel && g_RefractModel->getMeshCount() > 0)
    {
        auto verts = buildVerticesFromSim(g_Sim);
        Mesh *m = g_RefractModel->getMesh(0);
        if (m)
            m->updateVertices(verts);
    }

    // Model matrix: identity — all deformation is in the simulated vertex positions
    glm::mat4 view = g_Camera.getViewMatrix();
    glm::mat4 proj = g_Camera.getProjectionMatrix();

    if (g_BridgeStrength > 0.01f)
    {
        glm::vec4 clip = proj * view * glm::vec4(g_BridgeDirection * g_MainBubbleVisualRadius, 1.0f);
        if (clip.w > 0.001f)
        {
            glm::vec3 ndc = glm::vec3(clip) / clip.w;
            g_AutoTouchPoint = glm::vec2(ndc.x * 0.5f + 0.5f, ndc.y * 0.5f + 0.5f);
            g_AutoTouchStrength = std::max(g_AutoTouchStrength, g_BridgeStrength);
            g_AutoTouchVelocity = std::max(g_AutoTouchVelocity, g_BridgeStrength);
        }
    }

    auto SetRefractUniforms = [&](const glm::mat4 &model,
                                  float localRadius,
                                  bool isBackFace,
                                  bool renderToFBO,
                                  bool interactive,
                                  float geometryWobbleStrength,
                                  float contactDeformStrength,
                                  int iridescenceMode,
                                  float outputAlpha,
                                  float thicknessScale)
    {
        // Compute sphere's screen-space pixel radius for refraction offset scaling.
        // Use the simulation's bounding radius (slightly larger than nominal due to deformation).
        glm::vec3 center = glm::vec3(model * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        float dist = glm::length(g_Camera.getPosition() - center);
        float fovVert = glm::radians(g_Camera.getFov());
        float pixelsPerRadian = (h / 2.0f) / tanf(fovVert / 2.0f);
        float angularRadius = atanf((localRadius * 1.15f) / std::max(dist, 0.001f));
        float spherePixelRadius = angularRadius * pixelsPerRadian;

        g_RefractionShader->Use();
        g_RefractionShader->SetMat4("uModel", model);
        g_RefractionShader->SetMat4("uView", view);
        g_RefractionShader->SetMat4("uProj", proj);
        g_RefractionShader->SetVec3("uCameraPos", g_Camera.getPosition());
        g_RefractionShader->SetFloat("uGeometryWobbleStrength", geometryWobbleStrength);
        g_RefractionShader->SetFloat("uContactDeformStrength", contactDeformStrength);
        g_RefractionShader->SetInt("uVisualContactCount", 0);
        g_RefractionShader->SetInt("uForceSharedFilm", 0);
        g_RefractionShader->SetFloat("uFresnelPower", g_FresnelPower);
        g_RefractionShader->SetFloat("uShininess", g_Shininess);
        g_RefractionShader->SetFloat("uDiffuseness", g_Diffuseness);
        g_RefractionShader->SetFloat("uRefractionStrength", g_RefractionStrength);
        g_RefractionShader->SetFloat("uEdgeDistortionBoost", g_EdgeDistortionBoost);
        g_RefractionShader->SetFloat("uMaxOffsetRatio", g_MaxOffsetRatio);
        g_RefractionShader->SetFloat("uEnvironmentReflectionStrength", g_EnvironmentReflectionStrength);
        g_RefractionShader->SetFloat("uSpherePixelRadius", spherePixelRadius);
        g_RefractionShader->SetInt("uIsBackFace", isBackFace ? 1 : 0);
        g_RefractionShader->SetInt("uRenderToFBO", renderToFBO ? 1 : 0);
        g_RefractionShader->SetInt("uIridescenceMode", iridescenceMode);
        g_RefractionShader->SetInt("uBackgroundTexture", 0);
        g_RefractionShader->SetInt("uEnvironmentMap", 1);
        g_RefractionShader->SetInt("uThinFilmLUT", 2);
        g_RefractionShader->SetVec3("uLight", g_Light);
        g_RefractionShader->SetVec2("uWinResolution", glm::vec2((float)w, (float)h));
        g_RefractionShader->SetVec2("uFBOSize", glm::vec2((float)g_FBOWidth, (float)g_FBOHeight));
        g_RefractionShader->SetFloat("uThickness", CurrentThickness() * thicknessScale);
        g_RefractionShader->SetFloat("uThicknessVar", g_ThicknessVar);
        g_RefractionShader->SetFloat("uTime", (float)g_Time);
        g_RefractionShader->SetVec3("uTextureOriginWorld", center);
        g_RefractionShader->SetMat3("uTextureBasis", glm::mat3(1.0f));
        g_RefractionShader->SetFloat("uTextureReferenceRadius",
                                     std::max(localRadius, 0.001f));
        g_RefractionShader->SetFloat("uVisualTime", (float)g_Time);
        g_RefractionShader->SetVec3("uFilmNoiseOffset", glm::vec3(0.0f));
        g_RefractionShader->SetFloat("uFilmBaseThicknessScale", 1.0f);
        g_RefractionShader->SetFloat("uFilmThicknessAmplitudeScale", 1.0f);
        g_RefractionShader->SetFloat("uFilmIor", 1.33f);
        g_RefractionShader->SetFloat("uOutputAlpha", outputAlpha);
        g_RefractionShader->SetFloat("uOpticalNormalBlend", 0.0f);
        g_RefractionShader->SetInt("uFusionDiagnosticMode",
                                   g_FusionRenderDiagnosticMode);
        glm::vec2 touchPoint = glm::vec2(-10.0f, -10.0f);
        float touchStrength = 0.0f;
        float touchVelocity = 0.0f;
        if (interactive)
        {
            touchPoint = g_TouchStrength >= g_AutoTouchStrength ? g_TouchPoint : g_AutoTouchPoint;
            touchStrength = std::max(g_TouchStrength, g_AutoTouchStrength);
            touchVelocity = std::max(g_TouchVelocity, g_AutoTouchVelocity);
        }
        g_RefractionShader->SetVec2("uTouchPoint", touchPoint);
        g_RefractionShader->SetFloat("uTouchStrength", touchStrength);
        g_RefractionShader->SetFloat("uTouchVelocity", touchVelocity);
    };

    auto ApplyPersistentVisualState = [&](const BubbleVisualState &state)
    {
        if (!state.initialized)
            return;
        auto seedComponent = [](uint32_t seed, uint32_t shift) {
            uint32_t value = (seed >> shift) & 0x3ffu;
            return static_cast<float>(value) / 1023.0f * 17.0f;
        };
        glm::vec3 noiseOffset(
            seedComponent(state.noiseSeed, 0u),
            seedComponent(state.noiseSeed ^ 0x9e3779b9u, 10u),
            seedComponent(state.noiseSeed ^ 0x85ebca6bu, 20u));
        float visualTime = static_cast<float>(g_Time) -
                           state.animationTimeOrigin +
                           state.interferencePhase;
        float opticalBlend = state.opticalNormalBlendAtOrigin * std::exp(
            -state.opticalNormalRelaxationRate *
            std::max(static_cast<float>(g_Time) -
                         state.animationTimeOrigin,
                     0.0f));
        g_RefractionShader->SetVec3("uTextureOriginWorld",
                                    state.textureOriginWorld);
        g_RefractionShader->SetMat3("uTextureBasis", state.textureBasis);
        g_RefractionShader->SetFloat("uTextureReferenceRadius",
                                     std::max(state.referenceRadius, 0.001f));
        g_RefractionShader->SetFloat("uVisualTime", visualTime);
        g_RefractionShader->SetVec3("uFilmNoiseOffset", noiseOffset);
        g_RefractionShader->SetFloat("uFilmBaseThicknessScale",
                                     state.filmBaseThicknessScale);
        g_RefractionShader->SetFloat("uFilmThicknessAmplitudeScale",
                                     state.filmThicknessAmplitudeScale);
        g_RefractionShader->SetFloat("uFilmIor", state.ior);
        g_RefractionShader->SetFloat("uFresnelPower",
                                     g_FresnelPower * state.fresnelStrength);
        g_RefractionShader->SetFloat("uOpticalNormalBlend", opticalBlend);
    };

    auto SetBubbleContactUniforms = [&](int bubbleIndex)
    {
        if (bubbleIndex < 0 || bubbleIndex >= (int)g_DisplayBubbles.size())
        {
            return;
        }
       const DisplayBubble &bubble = g_DisplayBubbles[(size_t)bubbleIndex];
        // Bursting bubbles must not get contact-plane clipping — the sim
        // mesh has a completely different vertex layout.
        if (bubble.state == DisplayBubble::State::Burst)
        {
            g_RefractionShader->SetInt("uVisualContactCount", 0);
            return;
        }
        int visualContactCount = 0;
        for (const BubbleContactPair &pair : g_ContactPairs)
        {
            int otherIndex = -1;
            if ((!pair.candidate && !pair.bonded) || pair.contactRadius <= 0.001f ||
                !PairContainsBubbleIndex(g_DisplayBubbles, pair, bubbleIndex, &otherIndex) ||
                visualContactCount >= 4)
            {
                continue;
            }
            // Skip pairs where the other bubble is bursting or dead.
            if (g_DisplayBubbles[(size_t)otherIndex].state == DisplayBubble::State::Burst ||
                g_DisplayBubbles[(size_t)otherIndex].state == DisplayBubble::State::Dead)
            {
                continue;
            }
            if (!pair.contactFrameInitialized)
            {
                continue;
            }
            int aIndex = -1;
            int bIndex = -1;
            ResolveContactPairIndices(g_DisplayBubbles, pair, aIndex, bIndex);
            glm::vec3 planeNormal = aIndex == bubbleIndex
                                        ? pair.filteredNormal
                                        : -pair.filteredNormal;
            float strength = glm::clamp(pair.contactActivation, 0.0f, 1.0f);
            std::string index = std::to_string(visualContactCount);
            g_RefractionShader->SetVec3("uVisualContactPlanePoints[" + index + "]",
                                        pair.filteredPlaneCenter);
            g_RefractionShader->SetVec3("uVisualContactPlaneNormals[" + index + "]",
                                        planeNormal);
            g_RefractionShader->SetFloat("uVisualContactBlendWidths[" + index + "]",
                                         std::max(bubble.radius * 0.018f, 0.002f));
            g_RefractionShader->SetFloat("uVisualContactStrengths[" + index + "]", strength);
            ++visualContactCount;
        }
        g_RefractionShader->SetInt("uVisualContactCount", visualContactCount);
    };

    auto BindRefractionInputs = [&](GLuint backgroundTexture)
    {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, backgroundTexture);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_CUBE_MAP, g_CubemapTexture);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, g_ThinFilmLUTTexture);
    };

    auto DrawRenderOnlyBubbles = [&](bool renderToFBO,
                                     GLuint backgroundTexture,
                                     bool straightComposite)
    {
        if (g_RenderOnlyBubbles.empty() || !g_DecorativeBubbleModel)
        {
            return;
        }

        BindRefractionInputs(backgroundTexture);
        std::vector<std::pair<float, size_t>> sortedBubbles;
        sortedBubbles.reserve(g_RenderOnlyBubbles.size());
        for (size_t i = 0; i < g_RenderOnlyBubbles.size(); ++i)
        {
            const RenderOnlyBubble &bubble = g_RenderOnlyBubbles[i];
            glm::vec3 drift(
                std::sin((float)g_Time * bubble.speed + bubble.phase) * 0.72f,
                std::cos((float)g_Time * bubble.speed * 0.77f + bubble.phase * 1.31f),
                std::sin((float)g_Time * bubble.speed * 0.61f + bubble.phase * 0.83f) * 0.42f);
            glm::vec3 center = bubble.basePosition + drift * bubble.driftAmplitude;
            glm::vec3 toCamera = center - g_Camera.getPosition();
            sortedBubbles.emplace_back(glm::dot(toCamera, toCamera), i);
        }
        std::sort(sortedBubbles.begin(), sortedBubbles.end(),
                  [](const auto &a, const auto &b)
                  { return a.first > b.first; });

        GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
        GLboolean depthWriteWasEnabled = GL_TRUE;
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthWriteWasEnabled);
        if (straightComposite)
        {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
        }

        for (const auto &item : sortedBubbles)
        {
            const RenderOnlyBubble &bubble = g_RenderOnlyBubbles[item.second];
            glm::vec3 drift(
                std::sin((float)g_Time * bubble.speed + bubble.phase) * 0.72f,
                std::cos((float)g_Time * bubble.speed * 0.77f + bubble.phase * 1.31f),
                std::sin((float)g_Time * bubble.speed * 0.61f + bubble.phase * 0.83f) * 0.42f);
            glm::vec3 center = bubble.basePosition + drift * bubble.driftAmplitude;
            glm::mat4 model = glm::translate(glm::mat4(1.0f), center);
            model = glm::scale(model, glm::vec3(bubble.radius));
            float alpha = (straightComposite ? 0.72f : 1.0f) * bubble.alpha;
            SetRefractUniforms(model, bubble.radius, false, renderToFBO,
                               false, 0.0f, 0.0f, 2, alpha, 1.0f);
            g_RefractionShader->SetInt("uVisualContactCount", 0);
            g_DecorativeBubbleModel->Draw(*g_RefractionShader);
        }

        if (straightComposite)
        {
            glDepthMask(depthWriteWasEnabled);
            if (!blendWasEnabled)
            {
                glDisable(GL_BLEND);
            }
        }
    };

    auto DrawDisplayBubbles = [&](bool isBackFace, bool renderToFBO, GLuint backgroundTexture, bool straightComposite)
    {
        BindRefractionInputs(backgroundTexture);

        float independentBubbleAlphaScale = 1.0f;

        std::vector<std::pair<float, size_t>> sortedBubbles;
        sortedBubbles.reserve(g_DisplayBubbles.size());
        for (size_t bubbleIndex = 0; bubbleIndex < g_DisplayBubbles.size(); ++bubbleIndex)
        {
            const auto &bubble = g_DisplayBubbles[bubbleIndex];
            if (bubble.state == DisplayBubble::State::Dead || bubble.alpha <= 0.01f || bubble.radius <= 0.001f)
            {
                continue;
            }
            bool worldScaleSurface = g_BubbleSurfaces.BubbleUsesWorldScale(bubble.id);
            glm::vec3 center = glm::vec3(PersistentBubbleModelMatrix(
                                             bubble, (float)g_Time, worldScaleSurface) *
                                         glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
            glm::vec3 toCamera = center - g_Camera.getPosition();
            sortedBubbles.emplace_back(glm::dot(toCamera, toCamera), bubbleIndex);
        }
        std::sort(sortedBubbles.begin(), sortedBubbles.end(),
                  [](const auto &a, const auto &b)
                  {
                      bool aPromoted =
                          g_DisplayBubbles[a.second].volumeTransferred;
                      bool bPromoted =
                          g_DisplayBubbles[b.second].volumeTransferred;
                      if (aPromoted != bPromoted)
                          return !aPromoted;
                      return a.first > b.first;
                  });

        GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
        if (straightComposite)
        {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
        }

        for (const auto &item : sortedBubbles)
        {
            const auto &bubble = g_DisplayBubbles[item.second];
            bool worldScaleSurface = g_BubbleSurfaces.BubbleUsesWorldScale(bubble.id);
            glm::mat4 bubbleModel = PersistentBubbleModelMatrix(
                bubble, (float)g_Time, worldScaleSurface);
            float fusionSurfaceVisibility = BubbleFusionSurfaceVisibility(bubble.id);
            float alpha = straightComposite
                              ? (g_InteractionDemoActive ? 0.62f : 0.72f) * bubble.alpha * independentBubbleAlphaScale
                              : bubble.alpha * independentBubbleAlphaScale;
            // Bursting bubbles are always drawn (physics drives their shape).
            if (bubble.state != DisplayBubble::State::Burst)
                alpha *= 1.0f - fusionSurfaceVisibility;
            if (alpha <= 0.005f)
            {
                continue;
            }
            SetRefractUniforms(bubbleModel, bubble.radius, isBackFace, renderToFBO,
                               false, 0.0f, 0.0f, 2, alpha, 1.0f);
            ApplyPersistentVisualState(bubble.visualState);
            SetBubbleContactUniforms((int)item.second);
            Model *shellModel = FindDisplayBubbleModel(bubble.id);
            if (shellModel)
            {
                if (renderToFBO && bubble.state == DisplayBubble::State::Burst)
                {
                    Mesh *mesh = shellModel->getMesh(0);
                    if (mesh)
                    {
                        auto *sim = GetBubbleSim(bubble.id);
                        if (sim)
                            UpdateMeshFromSim(mesh, *sim);
                    }
                }
                else if (renderToFBO && !bubble.volumeTransferred)
                {
                    Mesh *mesh = shellModel->getMesh(0);
                    if (mesh)
                    {
                        const std::vector<Vertex> *restVertices =
                            g_BubbleSurfaces.FindBubbleRestVertices(bubble.id);
                        mesh->updateVertices(BuildPersistentBubbleVertices(
                            (int)item.second, (float)g_Time,
                            restVertices, worldScaleSurface, &mesh->vertices));
                    }
                }
                shellModel->Draw(*g_RefractionShader);
            }
            else if (g_DecorativeBubbleModel)
            {
                g_DecorativeBubbleModel->Draw(*g_RefractionShader);
            }
        }

        if (straightComposite)
        {
            glDepthMask(GL_TRUE);
            if (!blendWasEnabled)
                glDisable(GL_BLEND);
        }
    };

    auto DrawFusionSurfaces = [&](bool isBackFace, bool renderToFBO,
                                  GLuint backgroundTexture, bool straightComposite)
    {
        BindRefractionInputs(backgroundTexture);

        GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
        GLboolean depthWriteWasEnabled = GL_TRUE;
        GLint blendSrcRgb = GL_ONE;
        GLint blendDstRgb = GL_ZERO;
        GLint blendSrcAlpha = GL_ONE;
        GLint blendDstAlpha = GL_ZERO;
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthWriteWasEnabled);
        glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);
        if (straightComposite)
        {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
        }

        std::vector<std::pair<float, BubbleContactPair *>> sortedPairs;
        sortedPairs.reserve(g_ContactPairs.size());
        for (BubbleContactPair &pair : g_ContactPairs)
        {
            float visibility = FusionSurfaceVisibility(pair);
            // Skip pairs where either bubble is bursting or dead.
            int aIndex = -1;
            int bIndex = -1;
            if (visibility <= 0.001f ||
                !ResolveContactPairIndices(g_DisplayBubbles, pair, aIndex, bIndex))
            {
                continue;
            }
            const DisplayBubble &a = g_DisplayBubbles[(size_t)aIndex];
            if (a.state == DisplayBubble::State::Burst ||
                a.state == DisplayBubble::State::Dead)
            {
                continue;
            }
            const DisplayBubble &b = g_DisplayBubbles[(size_t)bIndex];
            if (b.state == DisplayBubble::State::Burst ||
                b.state == DisplayBubble::State::Dead)
            {
                continue;
            }
            float volumeA = a.targetVolume > 0.0f ? a.targetVolume : BubbleVolume(a.radius);
            float volumeB = b.targetVolume > 0.0f ? b.targetVolume : BubbleVolume(b.radius);
            glm::vec3 targetCenter = pair.fusionProfileInitialized
                ? pair.fusionCenter +
                      pair.fusionCenterVelocity * pair.fusionElapsed
                : (a.position * volumeA + b.position * volumeB) /
                      std::max(volumeA + volumeB, 0.001f);
            glm::vec3 cameraForward = glm::normalize(g_Camera.getTarget() -
                                                     g_Camera.getPosition());
            float cameraDepth = glm::dot(targetCenter - g_Camera.getPosition(),
                                         cameraForward);
            sortedPairs.emplace_back(cameraDepth, &pair);
        }
        std::sort(sortedPairs.begin(), sortedPairs.end(),
                  [](const auto &a, const auto &b)
                  { return a.first > b.first; });

        for (const auto &sortedPair : sortedPairs)
        {
            BubbleContactPair &pair = *sortedPair.second;
            float visibility = FusionSurfaceVisibility(pair);
            int aIndex = -1;
            int bIndex = -1;
            if (!ResolveContactPairIndices(g_DisplayBubbles, pair, aIndex, bIndex))
            {
                continue;
            }

            const DisplayBubble &a = g_DisplayBubbles[(size_t)aIndex];
            const DisplayBubble &b = g_DisplayBubbles[(size_t)bIndex];
            float volumeA = a.targetVolume > 0.0f ? a.targetVolume : BubbleVolume(a.radius);
            float volumeB = b.targetVolume > 0.0f ? b.targetVolume : BubbleVolume(b.radius);
            float targetVolume = pair.fusionProfileInitialized
                                     ? pair.conservedFusionVolume
                                     : std::max(volumeA + volumeB, 0.001f);
            float targetRadius = RadiusFromVolume(targetVolume);
            glm::vec3 targetCenter = pair.fusionProfileInitialized
                ? pair.fusionCenter +
                      pair.fusionCenterVelocity * pair.fusionElapsed
                : (a.position * volumeA + b.position * volumeB) /
                      targetVolume;

            glm::vec3 axis = pair.fusionFrameInitialized
                                 ? pair.fusionAxis
                                 : b.position - a.position;
            if (!IsFiniteDirection(axis) || glm::length(axis) <= 1e-5f)
            {
                axis = pair.contactFrameInitialized
                           ? pair.filteredNormal
                           : glm::vec3(1.0f, 0.0f, 0.0f);
            }
            if (!IsFiniteDirection(axis) || glm::length(axis) <= 1e-5f)
            {
                axis = glm::vec3(1.0f, 0.0f, 0.0f);
            }
            axis = glm::normalize(axis);
            glm::vec3 side;
            glm::vec3 binormal;
            if (pair.fusionFrameInitialized)
            {
                side = SafeNormalizeDirection(
                    pair.fusionSide - axis * glm::dot(pair.fusionSide, axis),
                    pair.fusionSide);
                binormal = SafeNormalizeDirection(
                    glm::cross(axis, side),
                    pair.fusionBinormal);
            }
            else
            {
                BuildStableOrthonormalFrame(axis, side, binormal);
            }
            float centerA = glm::dot(a.position - targetCenter, axis);
            float centerB = glm::dot(b.position - targetCenter, axis);

            glm::mat3 fusionBasis(1.0f);
            fusionBasis[0] = axis;
            fusionBasis[1] = side;
            fusionBasis[2] = binormal;

            FusionSurfaceParameters parameters;
            parameters.centerA = centerA;
            parameters.radiusA = a.radius;
            parameters.centerB = centerB;
            parameters.radiusB = b.radius;
            parameters.opticalReferenceRadius = targetRadius;
            std::vector<Vertex> fusionVertices =
                BuildFusionSurfaceVertices(parameters,
                                           pair.fusionProfile);
            if (fusionVertices.empty())
                continue;
            for (Vertex &vertex : fusionVertices)
            {
                vertex.Position = fusionBasis * vertex.Position;
                vertex.Normal = SafeNormalizeDirection(fusionBasis * vertex.Normal,
                                                       vertex.Normal);
                vertex.FilmDirection = SafeNormalizeDirection(
                    fusionBasis * vertex.FilmDirection, vertex.Normal);
            }

            Model *fusionSurface = g_BubbleSurfaces.FindFusion(pair.a, pair.b);
            if (!fusionSurface)
            {
                fusionSurface = g_BubbleSurfaces.EnsureFusion(
                    pair.a, pair.b, fusionVertices, BuildFusionSurfaceIndices());
            }
            else if (renderToFBO)
            {
                Mesh *mesh = fusionSurface->getMesh(0);
                if (mesh)
                {
                    mesh->updateVertices(fusionVertices);
                }
            }
            if (!fusionSurface)
            {
                continue;
            }

            glm::mat4 fusionModel = glm::translate(glm::mat4(1.0f), targetCenter);

            float pairAlpha = std::max(a.alpha, b.alpha);
            float alpha = (straightComposite
                               ? (g_InteractionDemoActive ? 0.62f : 0.72f)
                               : 1.0f) *
                          pairAlpha * visibility;
            // Each persistent material ring carries its parent-film direction
            // and optical scale. No target sphere is sampled in this pass.
            SetRefractUniforms(fusionModel, targetRadius,
                               isBackFace, renderToFBO, false,
                               0.0f, 0.0f, 2, alpha, 1.0f);
            ApplyPersistentVisualState(pair.visualState);
            g_RefractionShader->SetInt("uVisualContactCount", 0);
            fusionSurface->Draw(*g_RefractionShader);
        }

        if (straightComposite)
        {
            glDepthMask(depthWriteWasEnabled);
            glBlendFuncSeparate(blendSrcRgb, blendDstRgb,
                                blendSrcAlpha, blendDstAlpha);
            if (!blendWasEnabled)
            {
                glDisable(GL_BLEND);
            }
            else
            {
                glEnable(GL_BLEND);
            }
        }
    };

    auto DrawContactBridges = [&](bool isBackFace, bool renderToFBO, GLuint backgroundTexture, bool straightComposite)
    {
        BindRefractionInputs(backgroundTexture);

        GLboolean filmBlendWasEnabled = glIsEnabled(GL_BLEND);
        GLboolean filmCullWasEnabled = glIsEnabled(GL_CULL_FACE);
        glDisable(GL_CULL_FACE);
        if (straightComposite)
        {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
        }

        for (const BubbleContactPair &pair : g_ContactPairs)
        {
            int aIndex = -1;
            int bIndex = -1;
            // The shared internal film ruptures at the topology event.  Do not
            // alpha-fade this obsolete disc over the new neck: that produces
            // a detached translucent remnant during coalescence.
            if (pair.fusionActive ||
                (!pair.candidate && !pair.bonded) || pair.contactRadius <= 0.002f ||
                !ResolveContactPairIndices(g_DisplayBubbles, pair, aIndex, bIndex))
            {
                continue;
            }
            const DisplayBubble &a = g_DisplayBubbles[(size_t)aIndex];
            if (a.state == DisplayBubble::State::Burst ||
                a.state == DisplayBubble::State::Dead)
                continue;
            const DisplayBubble &b = g_DisplayBubbles[(size_t)bIndex];
            if (b.state == DisplayBubble::State::Burst ||
                b.state == DisplayBubble::State::Dead)
                continue;
            float visibility = pair.contactActivation * pair.contactActivation;
            float alpha = (straightComposite ? 0.16f : 0.24f) * visibility;
            if (alpha <= 0.002f)
            {
                continue;
            }
            float pulse = 1.0f + std::sin((float)g_Time * 0.70f +
                                          (float)(pair.a * 0.37f + pair.b * 0.19f)) *
                                     0.008f;
            float filmRadius = std::max(0.002f, pair.contactRadius * pulse);
            float normalizedCurvature = ContactFilmWorldCurvature(a, b, pair) * filmRadius;
            std::vector<Vertex> contactVertices =
                BuildCurvedContactFilmVertices(normalizedCurvature, 0.0f);
            for (Vertex &vertex : contactVertices)
            {
                glm::vec3 materialDirection(vertex.Position.x,
                                            vertex.Position.y,
                                            0.35f + vertex.Position.z);
                vertex.FilmDirection = SafeNormalizeDirection(materialDirection,
                                                              glm::vec3(0.0f, 0.0f, 1.0f));
            }
            Model *contactSurface = g_BubbleSurfaces.FindContact(pair.a, pair.b);
            if (!contactSurface)
            {
                contactSurface = g_BubbleSurfaces.EnsureContact(
                    pair.a, pair.b, contactVertices, BuildContactFilmDisc(72).indices);
            }
            else if (renderToFBO)
            {
                Mesh *mesh = contactSurface->getMesh(0);
                if (mesh)
                {
                    PreserveFilmCoordinates(contactVertices, mesh->vertices);
                    mesh->updateVertices(contactVertices);
                }
            }
            if (!contactSurface)
            {
                continue;
            }
            glm::mat4 filmModel = MakeContactFilmModel(a, b, pair, (float)g_Time);
            SetRefractUniforms(filmModel, std::max(pair.contactRadius, 0.01f),
                               isBackFace, renderToFBO, false,
                               0.0f, 0.0f, 2, alpha, 1.0f);
            g_RefractionShader->SetInt("uForceSharedFilm", 1);
            contactSurface->Draw(*g_RefractionShader);
        }
        g_RefractionShader->SetInt("uForceSharedFilm", 0);

        if (straightComposite)
        {
            glDepthMask(GL_TRUE);
            if (!filmBlendWasEnabled)
            {
                glDisable(GL_BLEND);
            }
        }
        if (filmCullWasEnabled)
        {
            glEnable(GL_CULL_FACE);
        }
    };

    auto DrawMainBubble = [&](bool isBackFace, bool renderToFBO, GLuint backgroundTexture)
    {
        if (!g_ShowMainBubble)
        {
            return;
        }
        BindRefractionInputs(backgroundTexture);
        float mainScale = g_MainBubbleVisualRadius / std::max(g_BubbleRadius, 0.001f);
        glm::mat4 mainModel = glm::scale(glm::mat4(1.0f), glm::vec3(mainScale));
        SetRefractUniforms(mainModel, g_MainBubbleVisualRadius, isBackFace, renderToFBO, true, 0.085f, 0.0f, g_IridescenceMode, 1.0f, 1.0f);
        g_RefractModel->Draw(*g_RefractionShader);
    };

    auto DrawSpawnPreview = [&](GLuint backgroundTexture)
    {
        GLFWwindow *window = glfwGetCurrentContext();
        if (!window || !g_DecorativeBubbleModel)
        {
            return;
        }
        bool shiftDown =
            glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
            glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
        if (!shiftDown)
        {
            return;
        }

        double cursorX = 0.0;
        double cursorY = 0.0;
        glfwGetCursorPos(window, &cursorX, &cursorY);
        glm::vec3 previewCenter;
        if (!BubblePlanePointFromScreen(cursorX, cursorY, previewCenter))
        {
            return;
        }

        BindRefractionInputs(backgroundTexture);
        glm::mat4 previewModel = glm::translate(glm::mat4(1.0f), previewCenter);
        previewModel = glm::scale(previewModel, glm::vec3(g_SpawnRadius));

        GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
        GLboolean depthWriteWasEnabled = GL_TRUE;
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthWriteWasEnabled);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);

        float pulse = 0.82f + 0.18f * std::sin((float)g_Time * 3.2f);
        SetRefractUniforms(previewModel, g_SpawnRadius, false, false,
                           false, 0.0f, 0.0f, 2,
                           0.18f + 0.08f * pulse, 0.85f);
        g_RefractionShader->SetFloat("uRefractionStrength", g_RefractionStrength * 0.35f);
        g_RefractionShader->SetFloat("uEnvironmentReflectionStrength",
                                     g_EnvironmentReflectionStrength * 0.55f);
        g_DecorativeBubbleModel->Draw(*g_RefractionShader);

        glDepthMask(depthWriteWasEnabled);
        if (!blendWasEnabled)
        {
            glDisable(GL_BLEND);
        }
    };

    auto SetBackgroundUniforms = [&]()
    {

        g_BackgroundShader->Use();
        g_BackgroundShader->SetVec4("uColor",
                                    glm::vec4(1.0f));
        for (size_t i = 0; i < g_BackgroundModels.size(); ++i)
        {
            glm::mat4 bgModel = glm::translate(glm::mat4(1.0f), g_SpherePositions[i]);
            glm::mat4 mvp = g_Camera.getMVP(bgModel);
            g_BackgroundShader->SetMat4("uMVP", mvp);
            g_BackgroundModels[i]->Draw(*g_BackgroundShader);
        }
    };

    auto DrawSkybox = [&]()
    {
        g_SkyboxShader->Use();
        glm::mat4 viewNoTranslation = glm::mat4(glm::mat3(g_Camera.getViewMatrix()));
        glm::mat4 projection = g_Camera.getProjectionMatrix();
        g_SkyboxShader->SetMat4("projection", projection);
        g_SkyboxShader->SetMat4("view", viewNoTranslation);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_CUBE_MAP, g_CubemapTexture);
        g_SkyboxShader->SetInt("skybox", 0);

        g_SkyboxModel->Draw(*g_SkyboxShader);
    };

    int fboW = g_FBOWidth;
    int fboH = g_FBOHeight;

    // ========== Pass 1: Render background to backgroundFBO (overscanned) ==========
    glBindFramebuffer(GL_FRAMEBUFFER, g_BackgroundFBO);
    glViewport(0, 0, fboW, fboH);
    glEnable(GL_DEPTH_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glCullFace(GL_BACK);

    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    DrawSkybox();
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);

    SetBackgroundUniforms();

    // ========== Pass 2: Render scene behind the main bubble ==========
    // This texture includes display bubbles. The main bubble will refract it,
    // so small bubbles remain visible when covered by the large transparent bubble.
    glBindFramebuffer(GL_FRAMEBUFFER, g_SceneBehindFBO);
    glViewport(0, 0, fboW, fboH);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glCullFace(GL_BACK);

    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    DrawSkybox();
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);

    SetBackgroundUniforms();

    DrawRenderOnlyBubbles(true, g_BackgroundTexture, true);
    DrawDisplayBubbles(false, true, g_BackgroundTexture, true);
    DrawFusionSurfaces(false, true, g_BackgroundTexture, true);
    DrawContactBridges(false, true, g_BackgroundTexture, true);

    // ========== Pass 3: Render main bubble back faces to backFaceFBO ==========
    glBindFramebuffer(GL_FRAMEBUFFER, g_BackFaceFBO);
    glViewport(0, 0, fboW, fboH);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glCullFace(GL_FRONT);

    DrawMainBubble(true, true, g_SceneBehindTexture);

    // ========== Pass 4: Render main-only scene for display-bubble refraction ==========
    glBindFramebuffer(GL_FRAMEBUFFER, g_MainSceneFBO);
    glViewport(0, 0, fboW, fboH);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glCullFace(GL_BACK);

    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    DrawSkybox();
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);

    SetBackgroundUniforms();

    DrawMainBubble(false, true, g_BackFaceTexture);

    // ========== Pass 5: Render to screen ==========
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);

    glCullFace(GL_BACK);

    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    DrawSkybox();
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);

    SetBackgroundUniforms();

    DrawMainBubble(false, false, g_BackFaceTexture);

    DrawRenderOnlyBubbles(false, g_MainSceneTexture, true);
    DrawDisplayBubbles(false, false, g_MainSceneTexture, true);
    DrawFusionSurfaces(false, false, g_MainSceneTexture, true);
    DrawContactBridges(false, false, g_MainSceneTexture, true);
    DrawSpawnPreview(g_MainSceneTexture);

    if ((g_DebugFusionProfile || g_DebugFusionCurvature ||
         g_DebugFusionForces) && g_DecorativeBubbleModel)
    {
        GLboolean depthWasEnabled = glIsEnabled(GL_DEPTH_TEST);
        GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        g_BackgroundShader->Use();

        auto drawDiagnosticSphere =
            [&](const glm::vec3 &position, float radius,
                const glm::vec4 &color)
        {
            glm::mat4 model =
                glm::translate(glm::mat4(1.0f), position);
            model = glm::scale(model, glm::vec3(radius));
            g_BackgroundShader->SetMat4(
                "uMVP", g_Camera.getMVP(model));
            g_BackgroundShader->SetVec4("uColor", color);
            g_DecorativeBubbleModel->Draw(*g_BackgroundShader);
        };
        auto drawDiagnosticSegment =
            [&](const glm::vec3 &start, const glm::vec3 &end,
                float thickness, const glm::vec4 &color)
        {
            glm::vec3 vector = end - start;
            float length = glm::length(vector);
            if (length <= 1e-6f)
                return;
            glm::vec3 direction = vector / length;
            float alignment = glm::clamp(direction.x, -1.0f, 1.0f);
            glm::quat rotation;
            if (alignment < -0.9999f)
                rotation = glm::angleAxis(
                    3.14159265358979323846f,
                    glm::vec3(0.0f, 1.0f, 0.0f));
            else
            {
                glm::vec3 crossAxis = glm::cross(
                    glm::vec3(1.0f, 0.0f, 0.0f), direction);
                rotation = glm::normalize(glm::quat(
                    1.0f + alignment,
                    crossAxis.x, crossAxis.y, crossAxis.z));
            }
            glm::mat4 model = glm::translate(
                glm::mat4(1.0f), 0.5f * (start + end)) *
                glm::mat4_cast(rotation);
            model = glm::scale(model,
                glm::vec3(0.5f * length, thickness, thickness));
            g_BackgroundShader->SetMat4("uMVP", g_Camera.getMVP(model));
            g_BackgroundShader->SetVec4("uColor", color);
            g_DecorativeBubbleModel->Draw(*g_BackgroundShader);
        };

        for (const BubbleContactPair &pair : g_ContactPairs)
        {
            if (!pair.fusionActive ||
                !pair.fusionProfileInitialized)
                continue;
            glm::vec3 center =
                pair.fusionCenter +
                pair.fusionCenterVelocity * pair.fusionElapsed;
            auto nodePosition = [&](std::size_t index) {
                const FusionProfileNode &node = pair.fusionProfile[index];
                return center + pair.fusionAxis * node.z +
                       pair.fusionSide * node.r;
            };
            if (g_DebugFusionProfile)
            {
                for (std::size_t i = 0;
                     i + 1 < pair.fusionProfile.size(); ++i)
                    drawDiagnosticSegment(
                        nodePosition(i), nodePosition(i + 1), 0.0025f,
                        glm::vec4(0.05f, 0.95f, 1.0f, 0.92f));
                for (std::size_t i = 0; i < pair.fusionProfile.size(); ++i)
                    drawDiagnosticSphere(
                        nodePosition(i), 0.0065f,
                        glm::vec4(0.05f, 1.0f, 0.35f, 0.98f));
            }
            if (g_DebugFusionCurvature)
            {
                float scale = std::max(
                    pair.fusionDiagnostics.maxAbsCurvature, 1e-5f);
                for (std::size_t i = 0; i < pair.fusionProfile.size(); ++i)
                {
                    float heat = glm::clamp(
                        std::abs(pair.fusionProfile[i].curvature) / scale,
                        0.0f, 1.0f);
                    glm::vec4 color(
                        heat, 0.25f + 0.75f * (1.0f - heat),
                        1.0f - heat, 0.98f);
                    drawDiagnosticSphere(nodePosition(i), 0.009f, color);
                }
            }
            if (g_DebugFusionForces)
            {
                float forceScale = std::max(
                    pair.fusionDiagnostics.maxNormalForce, 1e-7f);
                for (std::size_t i = 0; i < pair.fusionProfile.size(); i += 2)
                {
                    const FusionProfileNode &node = pair.fusionProfile[i];
                    glm::vec3 force = pair.fusionAxis * node.normalForceZ +
                                      pair.fusionSide * node.normalForceR;
                    glm::vec3 arrow = force * (0.10f / forceScale);
                    drawDiagnosticSphere(
                        nodePosition(i), 0.0055f,
                        glm::vec4(1.0f, 0.75f, 0.05f, 0.95f));
                    drawDiagnosticSegment(
                        nodePosition(i), nodePosition(i) + arrow,
                        0.0035f,
                        glm::vec4(1.0f, 0.12f, 0.03f, 0.95f));
                }
            }
            if (g_DebugFusionProfile)
            {
            for (std::size_t trailIndex = 0;
                 trailIndex < pair.fusionDiagnosticTrails.size();
                 ++trailIndex)
            {
                const std::vector<glm::vec3> &trail =
                    pair.fusionDiagnosticTrails[trailIndex];
                for (std::size_t sample = 0;
                     sample < trail.size(); sample += 6)
                {
                    float age = trail.empty()
                                    ? 0.0f
                                    : static_cast<float>(sample + 1) /
                                          static_cast<float>(trail.size());
                    drawDiagnosticSphere(
                        trail[sample], 0.006f,
                        glm::vec4(0.15f, 1.0f, 0.30f,
                                  0.20f + 0.65f * age));
                }

                std::size_t nodeIndex = std::min(
                    pair.fusionDiagnosticNodeIndices[trailIndex],
                    pair.fusionProfile.size() - 1);
                const FusionProfileNode &node =
                    pair.fusionProfile[nodeIndex];
                glm::vec3 position =
                    center + pair.fusionAxis * node.z +
                    pair.fusionSide * node.r;
                drawDiagnosticSphere(
                    position, 0.014f,
                    glm::vec4(0.05f, 1.0f, 0.20f, 1.0f));

                glm::vec3 velocity =
                    pair.fusionAxis * node.vz +
                    pair.fusionSide * node.vr;
                float speed = glm::length(velocity);
                if (speed > 1e-5f)
                {
                    glm::vec3 arrow = velocity *
                                      (0.12f / std::max(speed, 0.12f));
                    float arrowLength = glm::length(arrow);
                    glm::vec3 arrowDirection =
                        arrow / arrowLength;
                    float alignment = glm::clamp(
                        arrowDirection.x, -1.0f, 1.0f);
                    glm::quat rotation;
                    if (alignment < -0.9999f)
                        rotation = glm::angleAxis(
                            3.14159265358979323846f,
                            glm::vec3(0.0f, 1.0f, 0.0f));
                    else
                    {
                        glm::vec3 crossAxis =
                            glm::cross(glm::vec3(1.0f, 0.0f, 0.0f),
                                       arrowDirection);
                        rotation = glm::normalize(glm::quat(
                            1.0f + alignment,
                            crossAxis.x, crossAxis.y, crossAxis.z));
                    }
                    glm::mat4 model =
                        glm::translate(glm::mat4(1.0f),
                                       position + 0.5f * arrow) *
                        glm::mat4_cast(rotation);
                    model = glm::scale(
                        model,
                        glm::vec3(0.5f * arrowLength,
                                  0.0045f, 0.0045f));
                    g_BackgroundShader->SetMat4(
                        "uMVP", g_Camera.getMVP(model));
                    g_BackgroundShader->SetVec4(
                        "uColor",
                        glm::vec4(1.0f, 0.18f, 0.05f, 0.95f));
                    g_DecorativeBubbleModel->Draw(
                        *g_BackgroundShader);
                }
            }
            }
        }
        if (depthWasEnabled)
            glEnable(GL_DEPTH_TEST);
        if (!blendWasEnabled)
            glDisable(GL_BLEND);
    }
}

// ============================================================
// GLFW callbacks
// ============================================================
static void FramebufferSizeCallback(GLFWwindow *window, int width, int height)
{
    g_WindowWidth = width;
    g_WindowHeight = height;
    g_Camera.setViewportSize((float)width, (float)height);

    // Recreate FBOs for new size
    if (g_BackgroundFBO)
        glDeleteFramebuffers(1, &g_BackgroundFBO);
    if (g_BackgroundTexture)
        glDeleteTextures(1, &g_BackgroundTexture);
    if (g_SceneBehindFBO)
        glDeleteFramebuffers(1, &g_SceneBehindFBO);
    if (g_SceneBehindTexture)
        glDeleteTextures(1, &g_SceneBehindTexture);
    if (g_MainSceneFBO)
        glDeleteFramebuffers(1, &g_MainSceneFBO);
    if (g_MainSceneTexture)
        glDeleteTextures(1, &g_MainSceneTexture);
    if (g_BackFaceFBO)
        glDeleteFramebuffers(1, &g_BackFaceFBO);
    if (g_BackFaceTexture)
        glDeleteTextures(1, &g_BackFaceTexture);
    if (g_DepthRB)
        glDeleteRenderbuffers(1, &g_DepthRB);

    InitFBO(width, height);
}

static void MouseButtonCallback(GLFWwindow *window, int button, int action, int mods)
{
    if (button == GLFW_MOUSE_BUTTON_LEFT)
    {
        if (action == GLFW_PRESS && g_DeleteMode && !(mods & GLFW_MOD_SHIFT))
        {
            double x, y;
            glfwGetCursorPos(window, &x, &y);
            BurstBubbleAtScreenPoint(x, y);
            g_MousePressed = false;
            return;
        }

        if (action == GLFW_PRESS && (mods & GLFW_MOD_SHIFT))
        {
            double x, y;
            glfwGetCursorPos(window, &x, &y);
            SpawnBubbleAtScreenPoint(x, y);
            g_MousePressed = false;
            return;
        }

        g_MousePressed = (action == GLFW_PRESS);
        if (g_MousePressed)
        {
            double x, y;
            glfwGetCursorPos(window, &x, &y);
            g_LastMouseX = (float)x;
            g_LastMouseY = (float)y;
        }
    }
    else if (button == GLFW_MOUSE_BUTTON_RIGHT)
    {
        if (action == GLFW_PRESS && (mods & GLFW_MOD_SHIFT))
        {
            double x, y;
            glfwGetCursorPos(window, &x, &y);
            if (g_DeleteMode)
            {
                BurstBubbleAtScreenPoint(x, y);
            }
            else
            {
                RemoveBubbleAtScreenPoint(x, y);
            }
            g_TouchPressed = false;
            return;
        }

        g_TouchPressed = (action == GLFW_PRESS);
        if (g_TouchPressed)
        {
            double x, y;
            glfwGetCursorPos(window, &x, &y);
            g_LastTouchX = (float)x;
            g_LastTouchY = (float)y;
            g_TouchPoint = glm::vec2((float)x / (float)std::max(g_WindowWidth, 1),
                                     1.0f - (float)y / (float)std::max(g_WindowHeight, 1));
            g_TouchStrength = 1.0f;
            g_TouchVelocity = 0.0f;
        }
    }
}

static void CursorPosCallback(GLFWwindow *window, double xpos, double ypos)
{
    if (g_MousePressed)
    {
        float dx = (float)xpos - g_LastMouseX;
        float dy = (float)ypos - g_LastMouseY;

        g_CurrentAngleY += dx * 0.5f;
        g_CurrentAngleX += dy * 0.5f;

        g_LastMouseX = (float)xpos;
        g_LastMouseY = (float)ypos;
    }

    if (g_TouchPressed)
    {
        float dx = (float)xpos - g_LastTouchX;
        float dy = (float)ypos - g_LastTouchY;
        float norm = (float)std::max(std::max(g_WindowWidth, g_WindowHeight), 1);
        g_TouchVelocity = glm::clamp(glm::length(glm::vec2(dx, dy)) / norm * 18.0f, 0.0f, 1.0f);
        g_TouchStrength = glm::clamp(0.65f + g_TouchVelocity * 0.9f, 0.0f, 1.35f);
        g_TouchPoint = glm::vec2((float)xpos / (float)std::max(g_WindowWidth, 1),
                                 1.0f - (float)ypos / (float)std::max(g_WindowHeight, 1));
        g_LastTouchX = (float)xpos;
        g_LastTouchY = (float)ypos;
    }
}

static void ScrollCallback(GLFWwindow *window, double xoffset, double yoffset)
{
    (void)xoffset;
    bool shiftDown =
        glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
        glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
    if (shiftDown)
    {
        SetBubbleSpawnPlaneZ(g_BubbleSpawnPlaneZ + (float)yoffset * kBubbleSpawnPlaneStep);
        return;
    }

    g_CameraDistance -= (float)yoffset * 0.5f;
    g_CameraDistance = glm::clamp(g_CameraDistance, kCameraMinDistance, kCameraMaxDistance);
}

static void KeyCallback(GLFWwindow *window, int key, int scancode, int action, int mods)
{
    if (action != GLFW_PRESS && action != GLFW_REPEAT)
        return;

    const float step = 0.5f;

    switch (key)
    {
    case GLFW_KEY_R:
        g_FresnelPower = std::max(0.5f, g_FresnelPower - step);
        std::cout << "FresnelPower: " << g_FresnelPower << std::endl;
        break;
    case GLFW_KEY_T:
        g_FresnelPower = std::min(20.0f, g_FresnelPower + step);
        std::cout << "FresnelPower: " << g_FresnelPower << std::endl;
        break;
    case GLFW_KEY_Y:
        g_RefractionStrength = std::max(0.0f, g_RefractionStrength - 0.1f);
        std::cout << "RefractionStrength: " << g_RefractionStrength << std::endl;
        break;
    case GLFW_KEY_H:
        g_RefractionStrength = std::min(4.0f, g_RefractionStrength + 0.1f);
        std::cout << "RefractionStrength: " << g_RefractionStrength << std::endl;
        break;
    case GLFW_KEY_U:
        g_EdgeDistortionBoost = std::max(1.0f, g_EdgeDistortionBoost - 0.1f);
        std::cout << "EdgeDistortionBoost: " << g_EdgeDistortionBoost << std::endl;
        break;
    case GLFW_KEY_8:
        g_EdgeDistortionBoost = std::min(3.0f, g_EdgeDistortionBoost + 0.1f);
        std::cout << "EdgeDistortionBoost: " << g_EdgeDistortionBoost << std::endl;
        break;
    case GLFW_KEY_O:
        g_EnvironmentReflectionStrength = std::max(0.0f, g_EnvironmentReflectionStrength - 0.05f);
        std::cout << "EnvironmentReflectionStrength: " << g_EnvironmentReflectionStrength << std::endl;
        break;
    case GLFW_KEY_P:
        g_EnvironmentReflectionStrength = std::min(1.5f, g_EnvironmentReflectionStrength + 0.05f);
        std::cout << "EnvironmentReflectionStrength: " << g_EnvironmentReflectionStrength << std::endl;
        break;
    case GLFW_KEY_SEMICOLON:
        g_IridescenceMode = (g_IridescenceMode + 1) % 3;
        std::cout << "IridescenceMode: "
                  << (g_IridescenceMode == 0 ? "Kim2012" : (g_IridescenceMode == 1 ? "Spectral LUT" : "Belcour Airy"))
                  << ", Thickness: " << CurrentThickness() << " nm" << std::endl;
        break;
    case GLFW_KEY_N:
        CurrentThickness() = std::max(100.0f, CurrentThickness() - 20.0f);
        std::cout << "Thickness: " << CurrentThickness() << " nm" << std::endl;
        break;
    case GLFW_KEY_M:
        CurrentThickness() = std::min(2000.0f, CurrentThickness() + 20.0f);
        std::cout << "Thickness: " << CurrentThickness() << " nm" << std::endl;
        break;
    case GLFW_KEY_1:
        g_ThicknessVar = std::max(0.0f, g_ThicknessVar - 20.0f);
        std::cout << "ThicknessVar: " << g_ThicknessVar << " nm" << std::endl;
        break;
    case GLFW_KEY_2:
        g_ThicknessVar = std::min(500.0f, g_ThicknessVar + 20.0f);
        std::cout << "ThicknessVar: " << g_ThicknessVar << " nm" << std::endl;
        break;
    // ---- DBSTT Simulation controls ----
    case GLFW_KEY_Z:
        g_SimPaused = !g_SimPaused;
        std::cout << "Simulation " << (g_SimPaused ? "PAUSED" : "RUNNING") << std::endl;
        break;
    case GLFW_KEY_X:
        std::cout << "Resetting simulation..." << std::endl;
        g_Sim.initIcosphere(g_BubbleRadius, g_SimSubdivs, g_SimPerturb);
        g_Sim.timeStep = g_SimTimeStep;
        g_Sim.substepsPerFrame = g_SimSubsteps;
        g_SimFrameCounter = 0;
        g_Sim.surfaceTensionStrength = 15.0f;
        g_Sim.circulationDiffusion = 0.0005f;
        g_Sim.flipSurfaceTensionSign = true;
        ResetDisplayBubbles();
        break;
    case GLFW_KEY_3:
        g_Sim.surfaceTensionStrength = std::max(1.0f, g_Sim.surfaceTensionStrength - 1.0f);
        std::cout << "SurfaceTensionStrength: " << g_Sim.surfaceTensionStrength << std::endl;
        break;
    case GLFW_KEY_4:
        g_Sim.surfaceTensionStrength = std::min(50.0f, g_Sim.surfaceTensionStrength + 1.0f);
        std::cout << "SurfaceTensionStrength: " << g_Sim.surfaceTensionStrength << std::endl;
        break;
    case GLFW_KEY_LEFT_BRACKET:
        g_SpawnRadius = std::max(kMinSpawnRadius, g_SpawnRadius - 0.03f);
        std::cout << "[BubbleSpawn] radius=" << g_SpawnRadius << std::endl;
        break;
    case GLFW_KEY_RIGHT_BRACKET:
        g_SpawnRadius = std::min(kMaxSpawnRadius, g_SpawnRadius + 0.03f);
        std::cout << "[BubbleSpawn] radius=" << g_SpawnRadius << std::endl;
        break;
    case GLFW_KEY_COMMA:
        SetMaxLiveBubbleCount(g_MaxLiveBubbleCount > kDefaultDisplayBubbleCount
                                  ? g_MaxLiveBubbleCount - 1
                                  : kDefaultDisplayBubbleCount);
        break;
    case GLFW_KEY_PERIOD:
        SetMaxLiveBubbleCount(g_MaxLiveBubbleCount + 1);
        break;
    case GLFW_KEY_F:
        g_WindEnabled = !g_WindEnabled;
        std::cout << "[BubbleWind] " << (g_WindEnabled ? "enabled" : "disabled")
                  << " direction=(" << g_GlobalWindDirection.x << ", "
                  << g_GlobalWindDirection.y << ", " << g_GlobalWindDirection.z << ")"
                  << " strength=" << g_GlobalWindStrength << std::endl;
        break;
    case GLFW_KEY_Q:
        SetGlobalWindStrength(g_GlobalWindStrength - kGlobalWindStrengthStep);
        break;
    case GLFW_KEY_E:
        SetGlobalWindStrength(g_GlobalWindStrength + kGlobalWindStrengthStep);
        break;
    case GLFW_KEY_C:
        g_LogContactDebug = !g_LogContactDebug;
        std::cout << "[BubbleContact] debug log "
                  << (g_LogContactDebug ? "enabled" : "disabled") << std::endl;
        break;
    case GLFW_KEY_I:
        SetGlobalWindDirection(glm::vec3(0.0f, 1.0f, 0.0f));
        break;
    case GLFW_KEY_J:
        SetGlobalWindDirection(glm::vec3(-1.0f, 0.0f, 0.0f));
        break;
    case GLFW_KEY_K:
        SetGlobalWindDirection(glm::vec3(0.0f, -1.0f, 0.0f));
        break;
    case GLFW_KEY_L:
        SetGlobalWindDirection(glm::vec3(1.0f, 0.0f, 0.0f));
        break;
    case GLFW_KEY_G:
        StartBubbleInteractionDemo();
        break;
    case GLFW_KEY_D:
        g_DeleteMode = !g_DeleteMode;
        std::cout << "[BubbleBurst] delete mode "
                  << (g_DeleteMode ? "ON (left-click to pop bubbles)" : "OFF")
                  << std::endl;
        break;
    case GLFW_KEY_ESCAPE:
        glfwSetWindowShouldClose(window, GLFW_TRUE);
        break;
    default:
        break;
    }
}

// ============================================================
// Cleanup
// ============================================================
static void Cleanup()
{
    delete g_RefractModel;
    delete g_DecorativeBubbleModel;
    delete g_SkyboxModel;
    g_RefractModel = g_DecorativeBubbleModel = g_SkyboxModel = nullptr;
    g_BubbleSurfaces.Clear();

    for (auto *sphere : g_BackgroundModels)
    {
        delete sphere;
    }
    g_BackgroundModels.clear();

    if (g_CubemapTexture)
        glDeleteTextures(1, &g_CubemapTexture);
    if (g_ThinFilmLUTTexture)
        glDeleteTextures(1, &g_ThinFilmLUTTexture);
    if (g_BackgroundFBO)
        glDeleteFramebuffers(1, &g_BackgroundFBO);
    if (g_BackgroundTexture)
        glDeleteTextures(1, &g_BackgroundTexture);
    if (g_SceneBehindFBO)
        glDeleteFramebuffers(1, &g_SceneBehindFBO);
    if (g_SceneBehindTexture)
        glDeleteTextures(1, &g_SceneBehindTexture);
    if (g_MainSceneFBO)
        glDeleteFramebuffers(1, &g_MainSceneFBO);
    if (g_MainSceneTexture)
        glDeleteTextures(1, &g_MainSceneTexture);
    if (g_BackFaceFBO)
        glDeleteFramebuffers(1, &g_BackFaceFBO);
    if (g_BackFaceTexture)
        glDeleteTextures(1, &g_BackFaceTexture);
    if (g_DepthRB)
        glDeleteRenderbuffers(1, &g_DepthRB);
    delete g_RefractionShader;
    delete g_BackgroundShader;
    delete g_SkyboxShader;
    g_RefractionShader = g_BackgroundShader = g_SkyboxShader = nullptr;
}

// ============================================================
// Main
// ============================================================
int main(int argc, char **argv)
{
    bool validateFusion = false;
    std::string validationCase = "two-to-one";
    float validationFixedDt = 0.0f;
    std::filesystem::path validationOutput = "validation_frames";
    for (int i = 1; i < argc; ++i)
    {
        std::string argument(argv[i]);
        if (argument == "--validate-fusion")
        {
            validateFusion = true;
        }
        else if (std::string(argv[i]) ==
                 "--debug-fusion-profile")
        {
            g_DebugFusionProfile = true;
        }
        else if (argument == "--debug-fusion-curvature")
        {
            g_DebugFusionCurvature = true;
        }
        else if (argument == "--debug-fusion-forces")
        {
            g_DebugFusionForces = true;
        }
        else if (argument.rfind("--fusion-render-debug=", 0) == 0)
        {
            std::string mode = argument.substr(22);
            if (mode == "unlit") g_FusionRenderDiagnosticMode = 1;
            else if (mode == "normal") g_FusionRenderDiagnosticMode = 2;
            else if (mode == "thickness") g_FusionRenderDiagnosticMode = 3;
            else if (mode == "phase") g_FusionRenderDiagnosticMode = 4;
            else if (mode == "coordinate") g_FusionRenderDiagnosticMode = 5;
        }
        else if (argument.rfind("--fusion-case=", 0) == 0)
        {
            validationCase = argument.substr(14);
        }
        else if (argument.rfind("--fixed-dt=", 0) == 0)
        {
            validationFixedDt =
                std::max(std::stof(argument.substr(11)), 0.0f);
        }
        else if (argument.rfind("--validation-output=", 0) == 0)
        {
            validationOutput = argument.substr(20);
        }
    }
#ifdef _WIN32
    SetConsoleTitleA(g_AppTitle);
#endif

    // -------- GLFW init --------
    if (!glfwInit())
    {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return -1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow *window = glfwCreateWindow(g_WindowWidth, g_WindowHeight,
                                          g_AppTitle, nullptr, nullptr);
    if (!window)
    {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(0); // Show uncapped FPS for performance testing/demo tuning.

    // -------- GLAD init --------
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
    {
        std::cerr << "Failed to initialize GLAD" << std::endl;
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    std::cout << "OpenGL " << glGetString(GL_VERSION) << std::endl
              << std::flush;
    std::cout << "Renderer: " << glGetString(GL_RENDERER) << std::endl
              << std::flush;

    // -------- Callbacks --------
    glfwSetFramebufferSizeCallback(window, FramebufferSizeCallback);
    glfwSetMouseButtonCallback(window, MouseButtonCallback);
    glfwSetCursorPosCallback(window, CursorPosCallback);
    glfwSetScrollCallback(window, ScrollCallback);
    glfwSetKeyCallback(window, KeyCallback);

    // -------- Camera --------
    g_Camera.setViewportSize((float)g_WindowWidth, (float)g_WindowHeight);
    g_Camera.setPosition(g_CameraOrbitTarget + glm::vec3(0.0f, 0.0f, g_CameraDistance));
    g_Camera.setTarget(g_CameraOrbitTarget);

    // -------- Shaders --------
    g_RefractionShader = new Shader(refractionVertexShader, refractionFragmentShader);
    g_BackgroundShader = new Shader(backgroundVertexShader, backgroundFragmentShader);
    g_SkyboxShader = new Shader(skyboxVertexShader, skyboxFragmentShader);

    if (g_RefractionShader->m_id == 0 || g_BackgroundShader->m_id == 0 || g_SkyboxShader->m_id == 0)
    {
        std::cerr << "Shader creation failed!" << std::endl;
        Cleanup();
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    // -------- Cubemap --------
    std::vector<std::string> cubemapFaces = {
        "assets/skybox/right.jpg", "assets/skybox/left.jpg",
        "assets/skybox/top.jpg", "assets/skybox/bottom.jpg",
        "assets/skybox/front.jpg", "assets/skybox/back.jpg"};
    g_CubemapTexture = LoadCubemapTexture(cubemapFaces);
    if (g_CubemapTexture == 0)
    {
        std::cerr << "Failed to load cubemap! Check assets/skybox/ directory." << std::endl;
        Cleanup();
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    g_ThinFilmLUTTexture = LoadTexture2D("assets/lut/thinfilm_belcour_bubble.png");
    if (g_ThinFilmLUTTexture == 0)
    {
        std::cerr << "Failed to load thin-film LUT!" << std::endl;
        Cleanup();
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    // -------- Models --------
    g_SkyboxModel = Model::CreateSkyboxCube();

    // Initialize DBSTT vortex sheet simulation (replaces static sphere).
    g_Sim.initIcosphere(g_BubbleRadius, g_SimSubdivs, g_SimPerturb);
    g_Sim.timeStep = g_SimTimeStep;
    g_Sim.substepsPerFrame = g_SimSubsteps;
    g_SimFrameCounter = 0;

    // Build rendering Model from simulation mesh
    {
        const auto &pos = g_Sim.getPositions();
        const auto &nrm = g_Sim.getNormals();
        std::vector<unsigned int> idx;
        for (const auto &t : g_Sim.getTriangles())
        {
            idx.push_back((unsigned int)t.x);
            idx.push_back((unsigned int)t.y);
            idx.push_back((unsigned int)t.z);
        }
        g_RefractModel = Model::CreateFromVertices(pos, nrm, idx);
    }

    g_DecorativeBubbleModel = Model::CreateSphere(1.0f, 32, 16, true);
    ResetDisplayBubbles();
    if (validateFusion)
    {
        // The interaction demo cycles stable / unequal fusion / separation.
        // Advance to the unequal coalescence case for deterministic capture.
        StartBubbleInteractionDemo();
        StartBubbleInteractionDemo();
        ConfigureFusionValidationCase(validationCase);
    }

    // Background spheres (5×5 grid)
    float sphere_z = -5.0f;
    float sphere_radius = 0.15f;
    for (int i = 0; i < 5; ++i)
    {
        for (int j = 0; j < 5; ++j)
        {
            glm::vec3 pos = {-2.0f + float(i) * 1.0f, -2.0f + float(j) * 1.0f, sphere_z};
            g_SpherePositions.push_back(pos);
        }
    }
    for (auto &pos : g_SpherePositions)
    {
        Model *sphere = Model::CreateSphere(sphere_radius, 16, 8, false);
        if (sphere)
        {
            g_BackgroundModels.push_back(sphere);
        }
    }

    // -------- FBOs --------
    InitFBO(g_WindowWidth, g_WindowHeight);

    // -------- Global GL state --------
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    // -------- Main loop --------
    std::cout << std::endl;
    std::cout << "==================================================" << std::endl;
    std::cout << "  " << g_AppTitle << std::endl;
    std::cout << "==================================================" << std::endl;
    std::cout << "View" << std::endl;
    std::cout << "  Left drag      Rotate camera" << std::endl;
    std::cout << "  Right drag     Touch ripple on bubble" << std::endl;
    std::cout << "  Mouse wheel    Zoom in / out" << std::endl;
    std::cout << "  Shift+Wheel    Adjust bubble spawn depth" << std::endl;
    std::cout << "  Hold Shift     Preview spawn bubble" << std::endl;
    std::cout << std::endl;
    std::cout << "Appearance" << std::endl;
    std::cout << "  R / T          Fresnel edge power       - / +" << std::endl;
    std::cout << "  Y / H          Refraction strength      - / +" << std::endl;
    std::cout << "  U / 8          Edge distortion          - / +" << std::endl;
    std::cout << "  O / P          Environment reflection   - / +" << std::endl;
    std::cout << std::endl;
    std::cout << "Thin Film" << std::endl;
    std::cout << "  ;              Cycle mode: Kim2012 / LUT / Belcour Airy" << std::endl;
    std::cout << "  N / M          Film thickness (nm)      - / +" << std::endl;
    std::cout << "  1 / 2          Thickness variation      - / +" << std::endl;
    std::cout << std::endl;
    std::cout << "Simulation" << std::endl;
    std::cout << "  Z              Pause / resume" << std::endl;
    std::cout << "  X              Reset opening showcase scene" << std::endl;
    std::cout << "  G              Cycle interaction outcome replay" << std::endl;
    std::cout << "  Shift+Left     Spawn bubble at cursor plane" << std::endl;
    std::cout << "  Shift+Right    Remove bubble under cursor" << std::endl;
    std::cout << "  D              Toggle delete/pop mode" << std::endl;
    std::cout << "  (delete mode)  Left-click to pop bubble" << std::endl;
    std::cout << "  [ / ]          Spawn radius               - / +" << std::endl;
    std::cout << "  , / .          Max live bubbles           - / +" << std::endl;
    std::cout << "  F              Toggle global wind" << std::endl;
    std::cout << "  Q / E          Wind strength             - / +" << std::endl;
    std::cout << "  I / J / K / L  Wind direction: up / left / down / right" << std::endl;
    std::cout << "  C              Toggle contact debug log" << std::endl;
    std::cout << "  3 / 4          Surface tension          - / +" << std::endl;
    std::cout << std::endl;
    std::cout << "  ESC            Quit" << std::endl;
    std::cout << "--------------------------------------------------" << std::endl;
    std::cout << "Current: mode="
              << (g_IridescenceMode == 0 ? "Kim2012" : (g_IridescenceMode == 1 ? "LUT" : "Belcour Airy"))
              << ", thickness=" << CurrentThickness() << " nm"
              << ", refraction=" << g_RefractionStrength
              << ", edge=" << g_EdgeDistortionBoost
              << ", reflection=" << g_EnvironmentReflectionStrength
              << ", spawnRadius=" << g_SpawnRadius
              << ", spawnPlaneZ=" << g_BubbleSpawnPlaneZ
              << ", maxLive=" << g_MaxLiveBubbleCount
              << ", wind=" << (g_WindEnabled ? "on" : "off")
              << " strength=" << g_GlobalWindStrength
              << " dir=(" << g_GlobalWindDirection.x << ", "
              << g_GlobalWindDirection.y << ", " << g_GlobalWindDirection.z << ")"
              << ", contactLog=" << (g_LogContactDebug ? "on" : "off")
              << std::endl;
    std::cout << "Scene: three-bubble center with ambient showcase bubbles" << std::endl;
    std::cout << "==================================================" << std::endl;

    std::vector<unsigned char> validationPreviousFrame;
    bool validationWasFusing = false;
    int validationSavedFrame = 0;
    double validationRuptureTime = -1.0;
    double validationNextCaptureTime = -1.0;
    std::ofstream validationProfileCsv;
    std::deque<std::vector<unsigned char>> validationHandoffPreFrames;
    uint64_t validationObservedHandoffSerial = g_FusionHandoffSerial;
    int validationHandoffPostRemaining = -1;
    int validationHandoffFrameIndex = 0;
    if (validateFusion)
    {
        std::filesystem::create_directories(validationOutput);
        validationProfileCsv.open(
            validationOutput / "fusion_profile_trajectory.csv");
        validationProfileCsv
            << "time,node,z,r,vz,vr,mass,curvature,normal_force_z,"
               "normal_force_r,volume_correction,volume,area,centroid_z,"
               "rms_speed,curvature_variation,shape_mode,min_edge,max_edge,"
               "edge_ratio,min_angle_deg,min_angle_node,max_curvature,"
               "max_curvature_node,max_normal_force,max_normal_force_node,"
               "max_volume_correction,max_volume_correction_node,"
               "max_edge_node\n";
    }

    while (!glfwWindowShouldClose(window))
    {
        double currentTime = glfwGetTime();
        if (g_LastFrameTime == 0.0)
            g_LastFrameTime = currentTime;
        g_DeltaTime = validationFixedDt > 0.0f
                          ? validationFixedDt
                          : currentTime - g_LastFrameTime;
        g_Time += g_DeltaTime;
        g_LastFrameTime = currentTime;
        UpdateFPS(window, currentTime);

        RenderFrame();
        if (validateFusion)
        {
            if (validationProfileCsv)
            {
                for (const BubbleContactPair &pair : g_ContactPairs)
                {
                    if (!pair.fusionActive ||
                        !pair.fusionProfileInitialized)
                        continue;
                    float volume =
                        FusionProfileVolume(pair.fusionProfile);
                    for (std::size_t nodeIndex = 0;
                         nodeIndex < pair.fusionProfile.size();
                         ++nodeIndex)
                    {
                        const FusionProfileNode &node =
                            pair.fusionProfile[nodeIndex];
                        validationProfileCsv
                            << pair.fusionElapsed << ","
                            << nodeIndex << "," << node.z << ","
                            << node.r << "," << node.vz << ","
                            << node.vr << "," << node.mass << ","
                            << node.curvature << ","
                            << node.normalForceZ << ","
                            << node.normalForceR << ","
                            << node.volumeCorrection << ","
                            << volume << ","
                            << pair.currentFusionArea << ","
                            << FusionProfileCentroidZ(
                                   pair.fusionProfile)
                            << "," << pair.fusionRmsSpeed << ","
                            << pair.fusionCurvatureVariation << ","
                            << pair.fusionShapeMode << ","
                            << pair.fusionDiagnostics.minEdgeLength << ","
                            << pair.fusionDiagnostics.maxEdgeLength << ","
                            << pair.fusionDiagnostics.edgeLengthRatio << ","
                            << pair.fusionDiagnostics.minInternalAngleDegrees << ","
                            << pair.fusionDiagnostics.minAngleNode << ","
                            << pair.fusionDiagnostics.maxAbsCurvature << ","
                            << pair.fusionDiagnostics.maxCurvatureNode << ","
                            << pair.fusionDiagnostics.maxNormalForce << ","
                            << pair.fusionDiagnostics.maxNormalForceNode << ","
                            << pair.fusionDiagnostics.maxVolumeCorrection << ","
                            << pair.fusionDiagnostics.maxVolumeCorrectionNode << ","
                            << pair.fusionDiagnostics.maxEdgeNode << "\n";
                    }
                }
            }
            int captureWidth = 0;
            int captureHeight = 0;
            glfwGetFramebufferSize(window, &captureWidth, &captureHeight);
            std::vector<unsigned char> currentFrame(
                (size_t)captureWidth * (size_t)captureHeight * 3u);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadBuffer(GL_BACK);
            glReadPixels(0, 0, captureWidth, captureHeight,
                         GL_RGB, GL_UNSIGNED_BYTE, currentFrame.data());

            if (g_FusionHandoffSerial != validationObservedHandoffSerial)
            {
                validationObservedHandoffSerial = g_FusionHandoffSerial;
                int preIndex = 0;
                for (const std::vector<unsigned char> &frame :
                     validationHandoffPreFrames)
                {
                    char fileName[64];
                    std::snprintf(fileName, sizeof(fileName),
                                  "handoff_%03d.ppm", preIndex++);
                    SaveValidationFrame(validationOutput / fileName,
                                        frame, captureWidth, captureHeight);
                }
                validationHandoffFrameIndex = preIndex;
                char fileName[64];
                std::snprintf(fileName, sizeof(fileName),
                              "handoff_%03d.ppm",
                              validationHandoffFrameIndex++);
                SaveValidationFrame(validationOutput / fileName,
                                    currentFrame, captureWidth, captureHeight);
                validationHandoffPostRemaining = 10;
                std::ofstream handoffMetadata(
                    validationOutput / "handoff_metadata.txt");
                handoffMetadata
                    << "completion_frame=" << preIndex << "\n"
                    << "global_time=" << g_LastFusionHandoffTime << "\n"
                    << "diagnostic_mode=" << g_FusionRenderDiagnosticMode
                    << "\n"
                    << g_LastFusionHandoffSnapshot;
            }
            else if (validationHandoffPostRemaining > 0)
            {
                char fileName[64];
                std::snprintf(fileName, sizeof(fileName),
                              "handoff_%03d.ppm",
                              validationHandoffFrameIndex++);
                SaveValidationFrame(validationOutput / fileName,
                                    currentFrame, captureWidth, captureHeight);
                --validationHandoffPostRemaining;
            }
            else if (validationHandoffPostRemaining < 0)
            {
                validationHandoffPreFrames.push_back(currentFrame);
                while (validationHandoffPreFrames.size() > 10)
                    validationHandoffPreFrames.pop_front();
            }

            bool isFusing = false;
            for (const BubbleContactPair &pair : g_ContactPairs)
            {
                isFusing = isFusing || pair.fusionActive;
            }
            if (isFusing && !validationWasFusing)
            {
                if (!validationPreviousFrame.empty())
                {
                    SaveValidationFrame(
                        validationOutput / "000_pre_rupture.ppm",
                        validationPreviousFrame, captureWidth, captureHeight);
                }
                SaveValidationFrame(
                    validationOutput / "001_rupture.ppm",
                    currentFrame, captureWidth, captureHeight);
                validationSavedFrame = 2;
                validationRuptureTime = g_Time;
                validationNextCaptureTime = g_Time + 0.10;
            }
            else if (validationRuptureTime >= 0.0)
            {
                if (g_Time >= validationNextCaptureTime)
                {
                    char fileName[48];
                    std::snprintf(fileName, sizeof(fileName),
                                  "%03d_relax.ppm",
                                  validationSavedFrame++);
                    SaveValidationFrame(validationOutput / fileName,
                                        currentFrame,
                                        captureWidth, captureHeight);
                    validationNextCaptureTime += 0.10;
                }
                if (g_Time - validationRuptureTime >= 6.0)
                {
                    for (const BubbleContactPair &pair : g_ContactPairs)
                    {
                        if (!pair.fusionProfileInitialized)
                            continue;
                        std::cout
                            << "[FusionValidation] conserved_volume="
                            << pair.conservedFusionVolume
                            << " final_volume="
                            << FusionProfileVolume(pair.fusionProfile)
                            << " max_volume_error="
                            << pair.maximumRelativeVolumeError * 100.0f
                            << "% max_centroid_error="
                            << pair.maximumCentroidError
                            << " zero_crossings="
                            << pair.fusionShapeZeroCrossings
                            << " final_shape_mode="
                            << pair.fusionShapeMode
                            << " rms_speed=" << pair.fusionRmsSpeed
                            << " curvature_variation="
                            << pair.fusionCurvatureVariation
                            << std::endl;
                    }
                    glfwSetWindowShouldClose(window, GLFW_TRUE);
                }
            }
            validationPreviousFrame = std::move(currentFrame);
            validationWasFusing = isFusing;
        }
        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    // -------- Shutdown --------
    Cleanup();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
