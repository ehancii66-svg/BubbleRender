//
// Created on 2026/3/21.
//
// Node APIs are not fully supported. To solve the compilation error of the interface cannot be found,
// please include "napi/native_api.h".

#include <rawfile/raw_file_manager.h>
#include "napi/native_api.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <hilog/log.h>
#include <native_window/external_window.h>
#include <glm/gtc/type_ptr.hpp>
#include "render/model.h"
#include "render/camera.h"
#include "render/stb_image.h"
#include "shader/shader_refraction.h"
#include "shader/shader_background.h"
#include "shader/shader_skybox.h"
#include "shader/shader_quad.h"
#include "simulation/vortex_sheet.h"
#include "simulation/display_bubble.h"
#include "simulation/display_bubble_simulation.h"
#include "render/contact_geometry.h"
#include "render/bubble_surface_system.h"

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <string>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <limits>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "RefractionDemo"

constexpr float kPi = 3.1415926535f;
constexpr float kTwoPi = 6.283185307f;

EGLDisplay display = EGL_NO_DISPLAY;
EGLContext context = EGL_NO_CONTEXT;
EGLSurface surface = EGL_NO_SURFACE;
NativeWindow* nativeWindow = nullptr;
NativeResourceManager* g_ResourceManager = nullptr;

//float width = 1280.0f, height = 2832.0f;
float width = 100.0f, height = 100.0f;
float g_InputViewportWidth = 0.0f, g_InputViewportHeight = 0.0f;
float currentAngleX = 0.0f, currentAngleY = 0.0f;
float g_CameraDistance = 4.0f;
glm::vec3 g_CameraOrbitTarget{0.0f, 0.18f, 1.10f};

Shader* refractionShader = nullptr;
Shader* backgroundShader = nullptr;
Shader* skyboxShader = nullptr;
Shader* quadShader = nullptr;

GLuint backgroundFBO = 0, backgroundTexture = 0;
GLuint sceneBehindFBO = 0, sceneBehindTexture = 0;
GLuint mainSceneFBO = 0, mainSceneTexture = 0;
GLuint finalSceneFBO = 0, finalSceneTexture = 0;
GLuint backFaceFBO = 0, backFaceTexture = 0;
GLuint depthRB = 0;
GLuint cubemapTexture = 0;
GLuint quadVAO = 0, quadVBO = 0;

Camera g_camera;

Model* g_RefractModel;
Model* g_DecorativeBubbleModel;
Model* g_SkyboxModel;
std::vector<Model*> g_BackgroundModels;
std::vector<glm::vec3> g_SpherePositions;

struct RenderOnlyBubble {
    glm::vec3 basePosition;
    float radius;
    float phase;
    float windAmplitude;
    float floatAmplitude;
    float speed;
};

static std::vector<RenderOnlyBubble> g_RenderOnlyBubbles;
static std::vector<DisplayBubble> g_DisplayBubbles;
static std::vector<BubbleContactPair> g_ContactPairs;
static BubbleSurfaceSystem g_BubbleSurfaces;
static uint64_t g_NextBubbleId = 1;
static uint64_t g_InteractionOutcomeSeed = 0;
static uint64_t g_ContactOutcomeSequence = 0;
static int g_InteractionDemoIndex = -1;
static bool g_InteractionDemoActive = false;
static bool g_ShowMainBubble = false;
static bool g_LogContactDebug = false;
static constexpr float g_MainBubbleVisualRadius = 1.5f;
static glm::vec3 g_BridgeDirection = glm::vec3(0.0f, 0.0f, 1.0f);
static float g_BridgeStrength = 0.0f;
static float g_AutoTouchStrength = 0.0f;
static float g_AutoTouchVelocity = 0.0f;
static bool g_WindEnabled = false;
static float g_GlobalWindStrength = 0.0f;
static glm::vec3 g_GlobalWindDirection = glm::normalize(glm::vec3(1.0f, 0.20f, 0.0f));
static constexpr float kMaxGlobalWindStrength = 0.45f;
static constexpr float kBurstAmbientImpulseStrength = 0.028f;
static constexpr float kBurstAmbientReachScale = 2.2f;
static constexpr float kBurstNeighborImmediateDisplacement = 0.045f;
static constexpr int kMobileBurstSimSubsteps = 12;
static constexpr float kMobileBurstSimTimeStep = 0.002f;
static bool g_AddPreviewVisible = false;
static glm::vec3 g_AddPreviewPosition{0.0f};
static float g_AddPreviewRadius = 0.48f;

static constexpr float kSharedFilmTime = 0.22f;
static constexpr float kNeckFormationTime = 1.15f;
static constexpr float kFusionTime = 3.45f;
static constexpr float kContactVisualTransitionTime = 0.35f;
static constexpr float kFusionFilmThickness = 0.18f;
static constexpr float kFilmRuptureDuration = 0.24f;
static const FusionProfileConfig kFusionProfileConfig = [] {
    FusionProfileConfig config;
    // Surface-tension-driven, stateful meridian dynamics. None of these
    // parameters is used as a target-sphere interpolation weight.
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

// ---- Rendering parameters ----
float g_FresnelPower = 8.0f;
float g_Shininess = 15.0f;
float g_Diffuseness = 0.2f;
float g_RefractionStrength = 0.35f;
float g_EdgeDistortionBoost = 2.2f;
float g_MaxOffsetRatio = 0.52f;
float g_EnvironmentReflectionStrength = 0.65f;
glm::vec3 light{-1.0f, 1.0f, 1.0f};
int   g_IridescenceMode = 2;         // 0=Kim2012, 1=SpectralLUT, 2=BelcourAiry
float g_KimLutThickness = 350.0f;
float g_AiryThickness = 740.0f;
float g_ThicknessVar = 160.0f;
double g_Time = 0.0;
double g_LastFrameTime = 0.0;
double g_CurrentFps = 0.0;
double g_FpsLogWindowStart = 0.0;
int g_FpsLogFrameCount = 0;

// ---- FBO overscan ----
static constexpr float kRenderScale = 0.5f;
static constexpr float kFBOOverscan = 1.0f;
int g_FBOWidth = 0, g_FBOHeight = 0;

// ---- Thin-film LUT texture ----
GLuint g_ThinFilmLUTTexture = 0;

// ---- DBSTT simulation ----
static VortexSheetSimulation g_Sim;
static float g_BubbleRadius = 1.5f;
static int   g_SimSubdivs  = 3;
static float g_SimPerturb  = 0.16f;
static bool  g_SimPaused   = false;
static bool  g_VisualPaused = false;
static constexpr int kSimFrameInterval = 6;

static float& CurrentThickness() {
    return (g_IridescenceMode == 2) ? g_AiryThickness : g_KimLutThickness;
}

static glm::mat4 RenderOnlyBubbleModelMatrix(const RenderOnlyBubble& bubble, float time) {
    glm::vec3 windDir = glm::normalize(glm::vec3(0.75f, 0.22f, 0.0f));
    float wind = sinf(time * bubble.speed + bubble.phase) * bubble.windAmplitude;
    float lift = sinf(time * (bubble.speed * 0.73f) + bubble.phase * 1.7f) * bubble.floatAmplitude;
    float depth = cosf(time * (bubble.speed * 0.51f) + bubble.phase * 0.9f) * bubble.windAmplitude * 0.22f;
    float wobble = 1.0f + sinf(time * (bubble.speed * 1.13f) + bubble.phase * 2.1f) * 0.035f;

    glm::vec3 pos = bubble.basePosition + windDir * wind + glm::vec3(0.0f, lift, depth);
    glm::mat4 model = glm::translate(glm::mat4(1.0f), pos);
    model = glm::scale(model, glm::vec3(bubble.radius * wobble));
    return model;
}

// ---- Touch state ----
float g_LastTouchX = 0.0f, g_LastTouchY = 0.0f;
glm::vec2 g_TouchPoint{-10.0f, -10.0f};
float g_TouchStrength = 0.0f;
float g_TouchVelocity = 0.0f;
bool  g_TouchPressed = false;

struct BurstSimFrame {
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    float simulationTime = 0.0f;
};

struct BurstSimTimeline {
    std::deque<BurstSimFrame> frames;
    uint64_t generation = 0;
};

struct BurstSimState {
    std::shared_ptr<VortexSheetSimulation> sim;
    uint64_t generation = 0;
    float simulationTime = 0.0f;
};

struct BurstSimRequest {
    uint64_t id = 0;
    uint64_t generation = 0;
    float targetTime = 0.0f;
};

static std::mutex g_BurstSimMutex;
static std::condition_variable g_BurstSimCondition;
static std::thread g_BurstSimThread;
static bool g_BurstSimStop = false;
static bool g_BurstSimWorkerBusy = false;
static uint64_t g_NextBurstSimGeneration = 1;
static std::unordered_map<uint64_t, BurstSimState> g_BubbleSims;
static std::unordered_map<uint64_t, BurstSimRequest> g_BurstSimRequests;
static std::unordered_map<uint64_t, BurstSimTimeline> g_BurstSimTimelines;
static constexpr size_t kBurstTimelineFrameLimit = 24;
static constexpr float kBurstSimulationLookAhead = 0.12f;
static constexpr uint64_t kAmbientSimRequestInterval = 2;
static constexpr float kAmbientSimAdvance = 0.015f;
static uint64_t g_AmbientSimRequestFrame = 0;
static size_t g_AmbientSimCursor = 0;
// Windows advances twelve 0.002 s DBSTT substeps per rendered frame. At the
// target 60 FPS this is about 1.5 seconds of simulation per visual second.
static constexpr float kBurstSimulationTimeScale = 1.5f;

static std::shared_ptr<VortexSheetSimulation> CreateBubbleSim() {
    auto sim = std::make_shared<VortexSheetSimulation>();
    sim->initIcosphere(1.0f, 1, 0.08f);
    sim->timeStep = 0.001f;
    sim->substepsPerFrame = 3;
    sim->surfaceTensionStrength = 15.0f;
    sim->circulationDiffusion = 0.0008f;
    sim->flipSurfaceTensionSign = true;
    sim->diagnosticsEnabled = false;
    return sim;
}

static float Smooth01(float x) {
    x = glm::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

static VortexSheetSimulation* InitBubbleSim(uint64_t id) {
    auto sim = CreateBubbleSim();
    VortexSheetSimulation* ptr = sim.get();
    std::lock_guard<std::mutex> lock(g_BurstSimMutex);
    BurstSimState state;
    state.sim = std::move(sim);
    state.generation = g_NextBurstSimGeneration++;
    g_BubbleSims[id] = std::move(state);
    return ptr;
}

static VortexSheetSimulation* GetBubbleSim(uint64_t id) {
    auto found = g_BubbleSims.find(id);
    return found != g_BubbleSims.end() ? found->second.sim.get() : nullptr;
}

static size_t LiveDisplayBubbleCount() {
    return static_cast<size_t>(std::count_if(
        g_DisplayBubbles.begin(), g_DisplayBubbles.end(),
        [](const DisplayBubble& bubble) { return bubble.state != DisplayBubble::State::Dead; }));
}

static std::vector<Vertex> BuildVerticesFromSim(const VortexSheetSimulation& sim) {
    const auto& positions = sim.getPositions();
    const auto& normals = sim.getNormals();
    std::vector<Vertex> vertices(positions.size());
    for (size_t i = 0; i < positions.size(); ++i) {
        vertices[i].Position = positions[i];
        vertices[i].Normal = normals[i];
        vertices[i].TexCoords = glm::vec2(0.0f);
        vertices[i].Tangent = glm::vec3(0.0f);
        vertices[i].Bitangent = glm::vec3(0.0f);
        vertices[i].FilmDirection = glm::length(positions[i]) > 1e-6f
            ? glm::normalize(positions[i]) : glm::vec3(0.0f, 1.0f, 0.0f);
    }
    return vertices;
}

static std::vector<unsigned int> BuildIndicesFromSim(const VortexSheetSimulation& sim) {
    std::vector<unsigned int> indices;
    indices.reserve(sim.getTriangles().size() * 3);
    for (const glm::ivec3& triangle : sim.getTriangles()) {
        indices.push_back(static_cast<unsigned int>(triangle.x));
        indices.push_back(static_cast<unsigned int>(triangle.y));
        indices.push_back(static_cast<unsigned int>(triangle.z));
    }
    return indices;
}

static void UpdateMeshFromSnapshot(Mesh* mesh,
                                   const BurstSimFrame& previous,
                                   const BurstSimFrame& current,
                                   float blend) {
    if (!mesh) return;
    auto& vertices = mesh->vertices;
    if (vertices.size() != previous.positions.size() ||
        vertices.size() != current.positions.size()) return;
    blend = glm::clamp(blend, 0.0f, 1.0f);
    for (size_t i = 0; i < vertices.size(); ++i) {
        vertices[i].Position = glm::mix(previous.positions[i], current.positions[i], blend);
        glm::vec3 blendedNormal = glm::mix(previous.normals[i], current.normals[i], blend);
        vertices[i].Normal = glm::length(blendedNormal) > 1e-6f
            ? glm::normalize(blendedNormal) : current.normals[i];
        if (glm::dot(vertices[i].FilmDirection, vertices[i].FilmDirection) < 1e-6f) {
            vertices[i].FilmDirection = glm::length(current.positions[i]) > 1e-6f
                ? glm::normalize(current.positions[i]) : glm::vec3(0.0f, 1.0f, 0.0f);
        }
    }
    mesh->updateVertices(vertices);
}

static void BurstSimWorkerMain() {
    std::unique_lock<std::mutex> lock(g_BurstSimMutex);
    while (true) {
        g_BurstSimCondition.wait(lock, [] {
            return g_BurstSimStop || !g_BurstSimRequests.empty();
        });
        if (g_BurstSimStop) break;

        std::vector<BurstSimRequest> requests;
        requests.reserve(g_BurstSimRequests.size());
        for (const auto& item : g_BurstSimRequests) requests.push_back(item.second);
        g_BurstSimRequests.clear();
        g_BurstSimWorkerBusy = true;
        lock.unlock();

        for (const BurstSimRequest& request : requests) {
            std::shared_ptr<VortexSheetSimulation> sim;
            float simulationTime = 0.0f;
            {
                std::lock_guard<std::mutex> stateLock(g_BurstSimMutex);
                auto found = g_BubbleSims.find(request.id);
                if (found == g_BubbleSims.end() ||
                    found->second.generation != request.generation) continue;
                sim = found->second.sim;
                simulationTime = found->second.simulationTime;
            }
            if (!sim) continue;

            float stepTime = std::max(
                sim->timeStep * static_cast<float>(sim->substepsPerFrame), 0.001f);
            while (simulationTime + 1e-6f < request.targetTime) {
                sim->update(0.0f);
                simulationTime += stepTime;

                BurstSimFrame frame;
                frame.positions = sim->getPositions();
                frame.normals = sim->getNormals();
                frame.simulationTime = simulationTime;

                std::lock_guard<std::mutex> stateLock(g_BurstSimMutex);
                auto found = g_BubbleSims.find(request.id);
                if (found == g_BubbleSims.end() ||
                    found->second.generation != request.generation ||
                    found->second.sim.get() != sim.get()) break;
                found->second.simulationTime = simulationTime;
                BurstSimTimeline& timeline = g_BurstSimTimelines[request.id];
                if (timeline.generation != request.generation) {
                    timeline.frames.clear();
                    timeline.generation = request.generation;
                }
                timeline.frames.push_back(std::move(frame));
                while (timeline.frames.size() > kBurstTimelineFrameLimit) {
                    timeline.frames.pop_front();
                }
            }
        }

        lock.lock();
        g_BurstSimWorkerBusy = false;
    }
}

static void StartBurstSimWorker() {
    if (g_BurstSimThread.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(g_BurstSimMutex);
        g_BurstSimStop = false;
        g_BurstSimWorkerBusy = false;
    }
    g_BurstSimThread = std::thread(BurstSimWorkerMain);
}

static void StopBurstSimWorker() {
    if (!g_BurstSimThread.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(g_BurstSimMutex);
        g_BurstSimStop = true;
        g_BurstSimCondition.notify_all();
    }
    g_BurstSimThread.join();
    std::lock_guard<std::mutex> lock(g_BurstSimMutex);
    g_BurstSimStop = false;
    g_BurstSimWorkerBusy = false;
    g_BurstSimRequests.clear();
}

static void RequestBubbleSimUpdates() {
    if (g_SimPaused || g_VisualPaused) return;
    std::unique_lock<std::mutex> lock(g_BurstSimMutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    if (g_BurstSimStop) return;
    bool burstActive = false;
    for (const DisplayBubble& bubble : g_DisplayBubbles) {
        if (bubble.state != DisplayBubble::State::Burst) continue;
        burstActive = true;
        auto found = g_BubbleSims.find(bubble.id);
        if (found == g_BubbleSims.end()) continue;
        BurstSimRequest& request = g_BurstSimRequests[bubble.id];
        request.id = bubble.id;
        request.generation = found->second.generation;
        request.targetTime = std::max(
            request.targetTime,
            std::min(bubble.burstDuration * kBurstSimulationTimeScale,
                     bubble.burstElapsed * kBurstSimulationTimeScale +
                         kBurstSimulationLookAhead));
    }

    if (!burstActive && !g_DisplayBubbles.empty() &&
        (g_AmbientSimRequestFrame++ % kAmbientSimRequestInterval) == 0) {
        const size_t bubbleCount = g_DisplayBubbles.size();
        for (size_t attempt = 0; attempt < bubbleCount; ++attempt) {
            size_t index = (g_AmbientSimCursor + attempt) % bubbleCount;
            const DisplayBubble& bubble = g_DisplayBubbles[index];
            if (bubble.state == DisplayBubble::State::Dead ||
                bubble.state == DisplayBubble::State::Burst) {
                continue;
            }
            auto found = g_BubbleSims.find(bubble.id);
            if (found == g_BubbleSims.end()) continue;
            BurstSimRequest& request = g_BurstSimRequests[bubble.id];
            request.id = bubble.id;
            request.generation = found->second.generation;
            request.targetTime = std::max(
                request.targetTime,
                found->second.simulationTime + kAmbientSimAdvance);
            g_AmbientSimCursor = (index + 1) % bubbleCount;
            break;
        }
    }
    if (g_BurstSimRequests.empty()) return;
    g_BurstSimCondition.notify_one();
}

static float LatestBurstVisualTime(uint64_t id) {
    std::lock_guard<std::mutex> lock(g_BurstSimMutex);
    auto found = g_BurstSimTimelines.find(id);
    if (found == g_BurstSimTimelines.end() || found->second.frames.empty()) return 0.0f;
    return found->second.frames.back().simulationTime / kBurstSimulationTimeScale;
}

static void ApplyBurstSnapshot(uint64_t id, float sampleTime, Mesh* mesh) {
    if (!mesh) return;
    BurstSimFrame previous;
    BurstSimFrame current;
    {
        std::unique_lock<std::mutex> lock(g_BurstSimMutex, std::try_to_lock);
        if (!lock.owns_lock()) return;
        auto found = g_BurstSimTimelines.find(id);
        if (found == g_BurstSimTimelines.end() || found->second.frames.empty()) return;
        const std::deque<BurstSimFrame>& frames = found->second.frames;
        current = frames.back();
        previous = current;
        for (size_t i = 1; i < frames.size(); ++i) {
            if (frames[i].simulationTime >= sampleTime) {
                previous = frames[i - 1];
                current = frames[i];
                break;
            }
        }
    }
    float interval = current.simulationTime - previous.simulationTime;
    float blend = interval > 1e-6f
        ? (sampleTime - previous.simulationTime) / interval : 1.0f;
    UpdateMeshFromSnapshot(mesh, previous, current, blend);
}

static bool CopyLatestBubbleSimFrame(uint64_t id,
                                     std::vector<glm::vec3>& positions,
                                     float& simulationTime) {
    std::unique_lock<std::mutex> lock(g_BurstSimMutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    auto state = g_BubbleSims.find(id);
    auto timeline = g_BurstSimTimelines.find(id);
    if (state == g_BubbleSims.end() || timeline == g_BurstSimTimelines.end() ||
        timeline->second.generation != state->second.generation ||
        timeline->second.frames.empty()) {
        return false;
    }
    const BurstSimFrame& frame = timeline->second.frames.back();
    positions = frame.positions;
    simulationTime = frame.simulationTime;
    return positions.size() >= 4;
}

static void ApplyAmbientDbsttDeformation(const DisplayBubble& bubble,
                                         bool worldScaleSurface,
                                         const std::vector<unsigned int>& indices,
                                         std::vector<Vertex>& vertices) {
    if (bubble.state == DisplayBubble::State::Burst || vertices.empty()) return;

    std::vector<glm::vec3> samples;
    float simulationTime = 0.0f;
    if (!CopyLatestBubbleSimFrame(bubble.id, samples, simulationTime)) return;

    float meanRadius = 0.0f;
    for (const glm::vec3& sample : samples) meanRadius += glm::length(sample);
    meanRadius /= static_cast<float>(samples.size());
    float activation = Smooth01(simulationTime / 0.06f);
    float deformationScale = worldScaleSurface ? bubble.radius : 1.0f;
    float rotation = bubble.phase * 0.37f;
    float c = std::cos(rotation);
    float s = std::sin(rotation);

    float modeNumerators[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float modeDenominators[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    for (const glm::vec3& sample : samples) {
        float sampleRadius = glm::length(sample);
        if (sampleRadius <= 1e-6f) continue;
        glm::vec3 direction = sample / sampleRadius;
        float basis[5] = {
            direction.x * direction.x - direction.y * direction.y,
            direction.y * direction.y - direction.z * direction.z,
            2.0f * direction.x * direction.y,
            2.0f * direction.x * direction.z,
            2.0f * direction.y * direction.z
        };
        float offset = sampleRadius - meanRadius;
        for (size_t mode = 0; mode < 5; ++mode) {
            modeNumerators[mode] += offset * basis[mode];
            modeDenominators[mode] += basis[mode] * basis[mode];
        }
    }
    float modeCoefficients[5];
    for (size_t mode = 0; mode < 5; ++mode) {
        modeCoefficients[mode] = modeDenominators[mode] > 1e-6f
            ? modeNumerators[mode] / modeDenominators[mode]
            : 0.0f;
    }

    for (Vertex& vertex : vertices) {
        glm::vec3 positionDirection = glm::length(vertex.Position) > 1e-6f
            ? glm::normalize(vertex.Position)
            : glm::vec3(0.0f, 1.0f, 0.0f);
        glm::vec3 filmDirection = glm::dot(vertex.FilmDirection, vertex.FilmDirection) > 1e-8f
            ? glm::normalize(vertex.FilmDirection)
            : positionDirection;
        glm::vec3 sampleDirection(
            c * filmDirection.x - s * filmDirection.z,
            filmDirection.y,
            s * filmDirection.x + c * filmDirection.z);
        float basis[5] = {
            sampleDirection.x * sampleDirection.x -
                sampleDirection.y * sampleDirection.y,
            sampleDirection.y * sampleDirection.y -
                sampleDirection.z * sampleDirection.z,
            2.0f * sampleDirection.x * sampleDirection.y,
            2.0f * sampleDirection.x * sampleDirection.z,
            2.0f * sampleDirection.y * sampleDirection.z
        };
        float radialOffset = 0.0f;
        for (size_t mode = 0; mode < 5; ++mode) {
            radialOffset += modeCoefficients[mode] * basis[mode];
        }
        radialOffset = glm::clamp(radialOffset, -0.045f, 0.045f);
        vertex.Position += positionDirection *
                           (radialOffset * deformationScale * activation * 0.82f);
    }

    std::vector<glm::vec3> normalSums(vertices.size(), glm::vec3(0.0f));
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        unsigned int ia = indices[i];
        unsigned int ib = indices[i + 1];
        unsigned int ic = indices[i + 2];
        if (ia >= vertices.size() || ib >= vertices.size() || ic >= vertices.size()) continue;
        glm::vec3 faceNormal = glm::cross(vertices[ib].Position - vertices[ia].Position,
                                          vertices[ic].Position - vertices[ia].Position);
        if (glm::length(faceNormal) <= 1e-8f) continue;
        normalSums[ia] += faceNormal;
        normalSums[ib] += faceNormal;
        normalSums[ic] += faceNormal;
    }
    for (size_t i = 0; i < vertices.size(); ++i) {
        if (glm::length(normalSums[i]) > 1e-6f) {
            vertices[i].Normal = glm::normalize(normalSums[i]);
        }
    }
}

static void RetireBubbleSim(uint64_t id) {
    std::lock_guard<std::mutex> lock(g_BurstSimMutex);
    g_BubbleSims.erase(id);
    g_BurstSimRequests.erase(id);
    g_BurstSimTimelines.erase(id);
}

static DisplayBubble& AddInteractiveBubble(const glm::vec3& position, float radius,
                                           const glm::vec3& velocity = glm::vec3(0.0f)) {
    DisplayBubble bubble;
    bubble.id = g_NextBubbleId++;
    bubble.basePosition = position;
    bubble.position = position;
    bubble.velocity = velocity;
    bubble.radius = radius;
    bubble.initialRadius = radius;
    bubble.targetVolume = BubbleVolume(radius);
    bubble.phase = static_cast<float>(bubble.id) * 1.37f;
    bubble.windAmplitude = 0.10f;
    bubble.floatAmplitude = 0.06f;
    bubble.speed = 0.55f + 0.08f * static_cast<float>(bubble.id % 5);
    bubble.surfaceControls = MakeSurfaceControls(bubble.phase);
    bubble.visualState.initialized = true;
    bubble.visualState.interferencePhase = bubble.phase;
    bubble.visualState.noiseSeed = static_cast<uint32_t>(
        (bubble.id * 2654435761ull) & 0xffffffffu);
    bubble.visualState.animationTimeOrigin = static_cast<float>(g_Time);
    bubble.visualState.textureOriginWorld = position;
    bubble.visualState.textureBasis = glm::mat3(1.0f);
    bubble.visualState.referenceRadius = std::max(radius, 0.001f);
    bubble.visualState.opacity = bubble.alpha;
    g_DisplayBubbles.push_back(bubble);
    InitBubbleSim(bubble.id);
    if (g_DecorativeBubbleModel) {
        std::vector<Vertex> vertices = BuildBubbleShellVertices(1.0f, 0.0f, false);
        for (Vertex& vertex : vertices) {
            vertex.FilmDirection = glm::normalize(vertex.Position);
        }
        g_BubbleSurfaces.EnsureBubble(
            bubble.id, vertices, BuildContactBubblePatchIndices());
    }
    return g_DisplayBubbles.back();
}

static BubbleContactPair& PresetPair(uint64_t a, uint64_t b,
                                    BubbleContactPair::CoalescenceOutcome outcome) {
    int index = EnsureContactPairById(g_ContactPairs, a, b);
    BubbleContactPair& pair = g_ContactPairs[static_cast<size_t>(index)];
    pair.outcome = outcome;
    pair.candidate = true;
    pair.active = true;
    pair.persistentRenderPair = true;
    return pair;
}

static void ClearInteractiveScene() {
    g_BubbleSurfaces.Clear();
    {
        std::lock_guard<std::mutex> lock(g_BurstSimMutex);
        g_BubbleSims.clear();
        g_BurstSimTimelines.clear();
        g_BurstSimRequests.clear();
    }
    g_ContactPairs.clear();
    g_DisplayBubbles.clear();
    g_NextBubbleId = 1;
}

static void ResetOpeningScene() {
    ClearInteractiveScene();
    ++g_InteractionOutcomeSeed;
    g_InteractionDemoIndex = -1;
    g_InteractionDemoActive = false;
    g_CameraOrbitTarget = glm::vec3(0.0f, 0.18f, 1.10f);
    g_CameraDistance = 4.0f;
    currentAngleX = 0.0f;
    currentAngleY = 0.0f;

    struct BubbleSeed {
        glm::vec3 position;
        glm::vec3 home;
        float radius;
        float phase;
        float wind;
        float lift;
        float speed;
        glm::vec3 velocity;
    };
    const BubbleSeed seeds[] = {
        {{-0.38f, -0.20f, 1.08f}, {-0.22f, -0.12f, 1.09f}, 0.34f, 0.2f, 0.008f, 0.008f, 0.55f, { 0.050f, 0.014f, 0.000f}},
        {{ 0.38f, -0.18f, 1.12f}, { 0.22f, -0.10f, 1.11f}, 0.33f, 1.7f, 0.008f, 0.008f, 0.58f, {-0.048f, 0.013f, 0.000f}},
        {{ 0.00f,  0.62f, 1.08f}, { 0.00f,  0.31f, 1.09f}, 0.32f, 2.8f, 0.009f, 0.009f, 0.62f, { 0.000f,-0.050f, 0.002f}},
        {{ 0.70f,  0.70f, 1.18f}, { 0.72f,  0.76f, 1.17f}, 0.34f, 4.3f, 0.008f, 0.008f, 0.52f, { 0.000f, 0.035f, 0.000f}},
        {{ 0.70f,  1.30f, 1.20f}, { 0.72f,  1.04f, 1.18f}, 0.20f, 5.2f, 0.008f, 0.008f, 0.58f, { 0.000f,-0.060f, 0.000f}}
    };
    g_DisplayBubbles.reserve(sizeof(seeds) / sizeof(seeds[0]));
    for (const BubbleSeed& seed : seeds) {
        DisplayBubble& bubble = AddInteractiveBubble(seed.position, seed.radius, seed.velocity);
        bubble.basePosition = seed.home;
        bubble.phase = seed.phase;
        bubble.windAmplitude = seed.wind;
        bubble.floatAmplitude = seed.lift;
        bubble.speed = seed.speed;
        bubble.surfaceControls = MakeSurfaceControls(seed.phase);
    }

    // Keep the opening composition deterministic: the separate upper-right
    // pair demonstrates the small-into-large fusion path without disturbing
    // the central three-bubble shared-film interaction.
    if (g_DisplayBubbles.size() >= 5) {
        PresetPair(g_DisplayBubbles[3].id, g_DisplayBubbles[4].id,
                   BubbleContactPair::CoalescenceOutcome::WillCoalesce);
    }

    g_RenderOnlyBubbles = {
        {{-0.68f, -0.92f, 0.92f}, 0.09f, 0.3f, 0.30f, 0.030f, 0.70f},
        {{-0.62f,  0.48f, 1.34f}, 0.07f, 1.2f, 0.34f, 0.026f, 0.66f},
        {{-0.40f, -1.18f, 1.28f}, 0.06f, 2.1f, 0.36f, 0.022f, 0.62f},
        {{-0.18f,  1.52f, 0.82f}, 0.08f, 2.8f, 0.30f, 0.028f, 0.68f},
        {{ 0.24f, -1.08f, 1.18f}, 0.06f, 3.7f, 0.38f, 0.023f, 0.64f},
        {{ 0.58f,  1.44f, 0.88f}, 0.09f, 4.5f, 0.28f, 0.030f, 0.72f},
        {{ 0.66f, -0.68f, 1.38f}, 0.07f, 5.4f, 0.34f, 0.026f, 0.67f},
        {{ 0.72f,  0.22f, 0.96f}, 0.05f, 6.1f, 0.40f, 0.020f, 0.60f},
        {{ 0.12f,  1.82f, 1.42f}, 0.05f, 6.8f, 0.42f, 0.018f, 0.58f}
    };
}

static void CycleInteractionDemo() {
    ClearInteractiveScene();
    g_RenderOnlyBubbles.clear();
    g_InteractionDemoIndex = (g_InteractionDemoIndex + 1) % 3;
    g_InteractionDemoActive = true;
    g_CameraOrbitTarget = glm::vec3(0.0f, 0.12f, 1.10f);
    g_CameraDistance = 5.10f;
    currentAngleX = 0.0f;
    currentAngleY = 0.0f;

    glm::vec3 center(0.0f, 0.0f, 1.10f);
    glm::vec3 axis = glm::normalize(glm::vec3(1.0f, 0.04f, 0.0f));
    float radiusA = g_InteractionDemoIndex == 1 ? 0.64f : 0.56f;
    float radiusB = g_InteractionDemoIndex == 1 ? 0.31f
        : (g_InteractionDemoIndex == 2 ? 0.50f : 0.56f);
    float finalFilmRadius = std::min(radiusA, radiusB) * 0.30f;
    float restDistance = std::sqrt(radiusA * radiusA - finalFilmRadius * finalFilmRadius) +
                         std::sqrt(radiusB * radiusB - finalFilmRadius * finalFilmRadius);
    float centerDistance = radiusA + radiusB + std::min(radiusA, radiusB) * 0.065f;
    float volumeA = BubbleVolume(radiusA);
    float volumeB = BubbleVolume(radiusB);
    float totalVolume = volumeA + volumeB;
    float invMassA = 1.0f / std::max(volumeA, 0.001f);
    float invMassB = 1.0f / std::max(volumeB, 0.001f);
    float invMassSum = invMassA + invMassB;
    float approachSpeed = g_InteractionDemoIndex == 2 ? 0.13f : 0.10f;

    DisplayBubble& a = AddInteractiveBubble(
        center - axis * (centerDistance * volumeB / totalVolume), radiusA,
        axis * (approachSpeed * invMassA / invMassSum));
    a.basePosition = center - axis * (restDistance * volumeB / totalVolume);
    uint64_t aId = a.id;
    DisplayBubble& b = AddInteractiveBubble(
        center + axis * (centerDistance * volumeA / totalVolume), radiusB,
        -axis * (approachSpeed * invMassB / invMassSum));
    b.basePosition = center + axis * (restDistance * volumeA / totalVolume);

    BubbleContactPair& pair = PresetPair(
        aId, b.id,
        g_InteractionDemoIndex == 0
            ? BubbleContactPair::CoalescenceOutcome::StayDoubleBubble
            : (g_InteractionDemoIndex == 1
                ? BubbleContactPair::CoalescenceOutcome::WillCoalesce
                : BubbleContactPair::CoalescenceOutcome::SeparateAfterContact));
    pair.restDistance = restDistance;
    pair.neckRadius = std::min(radiusA, radiusB) * 0.06f;
    pair.targetVolume = a.targetVolume + b.targetVolume;
}

#include "simulation/windows_interaction_port.inc"

static bool RaySphereHit(const glm::vec3& origin, const glm::vec3& direction,
                         const DisplayBubble& bubble, float& distance) {
    glm::vec3 oc = origin - bubble.position;
    float b = glm::dot(oc, direction);
    float pickRadius = bubble.radius * 1.32f + 0.06f;
    float c = glm::dot(oc, oc) - pickRadius * pickRadius;
    float discriminant = b * b - c;
    if (discriminant < 0.0f) return false;
    float root = std::sqrt(discriminant);
    float t = -b - root;
    if (t < 0.0f) t = -b + root;
    if (t < 0.0f) return false;
    distance = t;
    return true;
}

static uint64_t PickBubble(float x, float y) {
    glm::vec3 origin = g_camera.getPosition();
    glm::vec3 direction = g_camera.getRayDirectionFromScreen(x, y);
    uint64_t picked = 0;
    float nearest = 1e30f;
    for (const DisplayBubble& bubble : g_DisplayBubbles) {
        if (bubble.state == DisplayBubble::State::Dead) continue;
        float distance = 0.0f;
        if (RaySphereHit(origin, direction, bubble, distance) && distance < nearest) {
            nearest = distance;
            picked = bubble.id;
        }
    }
    return picked;
}

static bool TriggerBubbleBurst(uint64_t id) {
    int index = FindBubbleIndexById(g_DisplayBubbles, id);
    if (index < 0) return false;
    DisplayBubble& bubble = g_DisplayBubbles[static_cast<size_t>(index)];
    if (bubble.state == DisplayBubble::State::Burst || bubble.state == DisplayBubble::State::Dead) return false;
    bubble.state = DisplayBubble::State::Burst;
    bubble.burstElapsed = 0.0f;
    bubble.burstDuration = 0.35f;
    bubble.burstScale = 1.0f;
    bubble.alpha = 1.0f;
    bubble.contactStrength = 0.0f;
    bubble.surfaceDynamicsBlend = 0.0f;

    std::vector<Vertex> burstVertices;
    std::vector<unsigned int> burstIndices;
    {
        std::lock_guard<std::mutex> lock(g_BurstSimMutex);
        auto found = g_BubbleSims.find(id);
        if (found == g_BubbleSims.end()) {
            BurstSimState state;
            state.generation = g_NextBurstSimGeneration++;
            found = g_BubbleSims.emplace(id, std::move(state)).first;
        }
        auto burstSim = std::make_shared<VortexSheetSimulation>();
        found->second.sim = burstSim;
        found->second.generation = g_NextBurstSimGeneration++;
        found->second.simulationTime = 0.0f;
        VortexSheetSimulation* sim = burstSim.get();
        sim->initIcosphere(bubble.radius, 2, 0.04f);
        glm::vec3 holeDirection = glm::normalize(g_camera.getPosition() - bubble.position);
        sim->punchHole(holeDirection, 0.45f);
        sim->surfaceTensionStrength = 120.0f;
        sim->substepsPerFrame = kMobileBurstSimSubsteps;
        sim->timeStep = kMobileBurstSimTimeStep;
        sim->circulationDiffusion = 0.0001f;
        sim->diagnosticsEnabled = false;
        burstVertices = BuildVerticesFromSim(*sim);
        burstIndices = BuildIndicesFromSim(*sim);
        BurstSimTimeline& timeline = g_BurstSimTimelines[id];
        timeline.frames.clear();
        timeline.generation = found->second.generation;
        BurstSimFrame initialFrame;
        initialFrame.positions = sim->getPositions();
        initialFrame.normals = sim->getNormals();
        timeline.frames.push_back(std::move(initialFrame));
        g_BurstSimRequests.erase(id);
    }
    Model* shellModel = g_BubbleSurfaces.FindBubble(id);
    if (shellModel) {
        Mesh* mesh = shellModel->getMesh(0);
        if (mesh) {
            mesh->updateGeometry(burstVertices, burstIndices);
        }
    }

    std::unordered_set<uint64_t> contactPartnerIds;
    for (const BubbleContactPair& pair : g_ContactPairs) {
        if (pair.a != id && pair.b != id) continue;
        if (!pair.active && !pair.bonded && !pair.persistentRenderPair &&
            pair.contactActivation <= 0.0f && pair.contactRadius <= 0.0f) {
            continue;
        }
        contactPartnerIds.insert(pair.a == id ? pair.b : pair.a);
    }

    for (BubbleContactPair& pair : g_ContactPairs) {
        if (pair.a == id || pair.b == id) {
            pair.bonded = false;
            pair.candidate = false;
            pair.active = false;
            pair.contactActivation = 0.0f;
            pair.bridgeStrength = 0.0f;
            pair.fusionActive = false;
            pair.fusionComplete = false;
            pair.persistentRenderPair = false;
            pair.contactRadius = 0.0f;
            pair.contactRadiusVelocity = 0.0f;
            pair.geometryBlend = 0.0f;
            pair.interactionCompression = 0.0f;
            g_BubbleSurfaces.RemoveContact(pair.a, pair.b);
        }
    }

    glm::vec3 burstCenter = bubble.position;
    for (DisplayBubble& other : g_DisplayBubbles) {
        if (other.id == id || other.state == DisplayBubble::State::Burst ||
            other.state == DisplayBubble::State::Dead) continue;

        glm::vec3 delta = other.position - burstCenter;
        float distance = glm::length(delta);
        if (distance <= 1e-4f) continue;

        glm::vec3 impulseDirection = delta / distance;
        float mobility = glm::clamp(
            std::pow(bubble.radius / std::max(other.radius, 0.08f), 1.25f),
            0.35f, 1.35f);
        float surfaceGap = std::max(
            distance - bubble.radius - other.radius, 0.0f);
        float ambientReach = std::max(
            bubble.radius * kBurstAmbientReachScale, 0.20f);
        if (surfaceGap < ambientReach) {
            float ambientFalloff = Smooth01(
                1.0f - surfaceGap / ambientReach);
            ambientFalloff *= ambientFalloff;
            other.velocity += impulseDirection *
                (kBurstAmbientImpulseStrength * ambientFalloff * mobility);
        }

        if (contactPartnerIds.find(other.id) == contactPartnerIds.end()) continue;

        float contactDistance = bubble.radius + other.radius;
        float contactTolerance = std::max(
            std::min(bubble.radius, other.radius) * 0.05f, 0.001f);
        float gap = std::max(distance - contactDistance, 0.0f);
        if (gap > contactTolerance) continue;

        // Keep approximately the previous response at actual contact while
        // fading out stale pairs near the edge of the geometric tolerance.
        float contactProximity = 1.0f - gap / contactTolerance;
        float responseStrength = 0.70f *
            std::sqrt(glm::clamp(contactProximity, 0.0f, 1.0f));

        // Make the shock visible on the first presented frame without
        // restoring the old excessive sustained velocity. Only bubbles that
        // were genuinely touching when the burst began receive this response.
        glm::vec3 immediateDisplacement = impulseDirection *
            (kBurstNeighborImmediateDisplacement * responseStrength *
             mobility * 1.20f);
        other.position += immediateDisplacement;
        other.basePosition += immediateDisplacement * 0.75f;
        other.contactAxis = -impulseDirection;
        other.contactStrength = std::max(other.contactStrength, responseStrength);
        other.surfaceDynamicsBlend = std::max(
            other.surfaceDynamicsBlend, 0.85f * responseStrength);
    }
    g_ShowMainBubble = false;
    return true;
}

static glm::mat4 AxisXModel(const glm::vec3& center, const glm::vec3& axis) {
    glm::vec3 x = glm::normalize(axis);
    glm::vec3 reference = std::abs(x.y) < 0.90f ? glm::vec3(0.0f, 1.0f, 0.0f)
                                                : glm::vec3(0.0f, 0.0f, 1.0f);
    glm::vec3 z = glm::normalize(glm::cross(x, reference));
    glm::vec3 y = glm::normalize(glm::cross(z, x));
    glm::mat4 model(1.0f);
    model[0] = glm::vec4(x, 0.0f);
    model[1] = glm::vec4(y, 0.0f);
    model[2] = glm::vec4(z, 0.0f);
    model[3] = glm::vec4(center, 1.0f);
    return model;
}

static glm::mat4 AxisZModel(const glm::vec3& center, const glm::vec3& normal, float scale) {
    glm::vec3 z = glm::normalize(normal);
    glm::vec3 reference = std::abs(z.y) < 0.90f ? glm::vec3(0.0f, 1.0f, 0.0f)
                                                : glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 x = glm::normalize(glm::cross(reference, z));
    glm::vec3 y = glm::normalize(glm::cross(z, x));
    glm::mat4 model(1.0f);
    model[0] = glm::vec4(x * scale, 0.0f);
    model[1] = glm::vec4(y * scale, 0.0f);
    model[2] = glm::vec4(z * scale, 0.0f);
    model[3] = glm::vec4(center, 1.0f);
    return model;
}


GLuint LoadCubemapTexture(NativeResourceManager* resMgr, const std::vector<std::string>& faceFiles){
    GLuint textureID;
    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_CUBE_MAP, textureID);
    
    int width, height, nrChannels;
    for (unsigned int i = 0; i < faceFiles.size(); i++){
        RawFile* rawFile = OH_ResourceManager_OpenRawFile(resMgr, faceFiles[i].c_str());
        if (!rawFile) {
            OH_LOG_ERROR(LOG_APP, "Failed to open rawfile: %{public}s", faceFiles[i].c_str());
            glDeleteTextures(1, &textureID);
            return 0;
        }
        long fileSize = OH_ResourceManager_GetRawFileSize(rawFile);
        std::vector<unsigned char> buffer(fileSize);
        int readSize = OH_ResourceManager_ReadRawFile(rawFile, buffer.data(), fileSize);
        OH_ResourceManager_CloseRawFile(rawFile);
        
        stbi_set_flip_vertically_on_load(false);
        stbi_uc* data = stbi_load_from_memory(buffer.data(), readSize, &width, &height, &nrChannels, 0);
        if (data) {
            GLenum format = (nrChannels == 4) ? GL_RGBA : GL_RGB;
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, format, width, height, 0, format, GL_UNSIGNED_BYTE, data);
            stbi_image_free(data);
        } else {
            OH_LOG_ERROR(LOG_APP, "Failed to decode image: %{public}s", faceFiles[i].c_str());
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

GLuint CompileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512] = {0};
        glGetShaderInfoLog(shader, 512, nullptr, infoLog);
        OH_LOG_ERROR(LOG_APP, "Shader compile error: %s", infoLog);
    }
    return shader;
}

GLuint CreateProgram(const char* vsSource, const char* fsSource) {
    GLuint vs = CompileShader(GL_VERTEX_SHADER, vsSource);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fsSource);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

void InitFullscreenQuad() {
    if (quadVAO != 0) return;

    float vertices[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
        -1.0f,  1.0f,
         1.0f, -1.0f,
         1.0f,  1.0f
    };

    glGenVertexArrays(1, &quadVAO);
    glGenBuffers(1, &quadVBO);
    glBindVertexArray(quadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}

void InitFBO(int w, int h) {
    g_FBOWidth  = std::max(1, static_cast<int>(w * kRenderScale * kFBOOverscan));
    g_FBOHeight = std::max(1, static_cast<int>(h * kRenderScale * kFBOOverscan));
    int fboW = g_FBOWidth, fboH = g_FBOHeight;

    glGenRenderbuffers(1, &depthRB);
    glBindRenderbuffer(GL_RENDERBUFFER, depthRB);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, fboW, fboH);

    auto CreateColorTarget = [&](GLuint& fbo, GLuint& texture, const char* name) {
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fboW, fboH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRB);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            OH_LOG_ERROR(LOG_APP, "%{public}s FBO incomplete!", name);
    };

    CreateColorTarget(backgroundFBO, backgroundTexture, "Background");
    CreateColorTarget(sceneBehindFBO, sceneBehindTexture, "Scene-behind");
    CreateColorTarget(mainSceneFBO, mainSceneTexture, "Main-scene");
    CreateColorTarget(finalSceneFBO, finalSceneTexture, "Final-scene");
    CreateColorTarget(backFaceFBO, backFaceTexture, "Back-face");

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static napi_value MakeBool(napi_env env, bool value) {
    napi_value result;
    napi_get_boolean(env, value, &result);
    return result;
}

static void ReleaseGraphicsResources() {
    StopBurstSimWorker();
    delete g_RefractModel;
    delete g_DecorativeBubbleModel;
    delete g_SkyboxModel;
    g_RefractModel = nullptr;
    g_DecorativeBubbleModel = nullptr;
    g_SkyboxModel = nullptr;
    for (auto* sphere : g_BackgroundModels){
        delete sphere;
    }
    g_BackgroundModels.clear();
    g_SpherePositions.clear();
    g_RenderOnlyBubbles.clear();
    ClearInteractiveScene();

    if (cubemapTexture) glDeleteTextures(1, &cubemapTexture);
    if (g_ThinFilmLUTTexture) glDeleteTextures(1, &g_ThinFilmLUTTexture);

    if (backgroundFBO) glDeleteFramebuffers(1, &backgroundFBO);
    if (backgroundTexture) glDeleteTextures(1, &backgroundTexture);
    if (sceneBehindFBO) glDeleteFramebuffers(1, &sceneBehindFBO);
    if (sceneBehindTexture) glDeleteTextures(1, &sceneBehindTexture);
    if (mainSceneFBO) glDeleteFramebuffers(1, &mainSceneFBO);
    if (mainSceneTexture) glDeleteTextures(1, &mainSceneTexture);
    if (finalSceneFBO) glDeleteFramebuffers(1, &finalSceneFBO);
    if (finalSceneTexture) glDeleteTextures(1, &finalSceneTexture);
    if (backFaceFBO) glDeleteFramebuffers(1, &backFaceFBO);
    if (backFaceTexture) glDeleteTextures(1, &backFaceTexture);
    if (depthRB) glDeleteRenderbuffers(1, &depthRB);
    if (quadVBO) glDeleteBuffers(1, &quadVBO);
    if (quadVAO) glDeleteVertexArrays(1, &quadVAO);

    cubemapTexture = 0;
    g_ThinFilmLUTTexture = 0;
    backgroundFBO = backgroundTexture = 0;
    sceneBehindFBO = sceneBehindTexture = 0;
    mainSceneFBO = mainSceneTexture = 0;
    finalSceneFBO = finalSceneTexture = 0;
    backFaceFBO = backFaceTexture = 0;
    depthRB = 0;
    quadVBO = quadVAO = 0;
    g_FBOWidth = 0;
    g_FBOHeight = 0;

    delete refractionShader;
    delete backgroundShader;
    delete skyboxShader;
    delete quadShader;
    refractionShader = nullptr;
    backgroundShader = nullptr;
    skyboxShader = nullptr;
    quadShader = nullptr;

    if (display != EGL_NO_DISPLAY) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        eglTerminate(display);
    }
    display = EGL_NO_DISPLAY;
    surface = EGL_NO_SURFACE;
    context = EGL_NO_CONTEXT;

    if (nativeWindow) {
        OH_NativeWindow_DestroyNativeWindow(nativeWindow);
        nativeWindow = nullptr;
    }

    if (g_ResourceManager) {
        OH_ResourceManager_ReleaseNativeResourceManager(g_ResourceManager);
        g_ResourceManager = nullptr;
    }

    width = 100.0f;
    height = 100.0f;
    g_InputViewportWidth = 0.0f;
    g_InputViewportHeight = 0.0f;
    g_LastFrameTime = 0.0;
    g_CurrentFps = 0.0;
    g_FpsLogWindowStart = 0.0;
    g_FpsLogFrameCount = 0;
}

static napi_value InitGraphics(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (display != EGL_NO_DISPLAY && surface != EGL_NO_SURFACE && context != EGL_NO_CONTEXT) {
        OH_LOG_INFO(LOG_APP, "InitGraphics skipped because graphics is already initialized");
        return MakeBool(env, true);
    }
    ReleaseGraphicsResources();

    g_ResourceManager = OH_ResourceManager_InitNativeResourceManager(env, args[0]);
    if (g_ResourceManager == nullptr) return MakeBool(env, false);

    size_t strSize = 0;
    char strBuf[256] = {0};
    if (napi_get_value_string_utf8(env, args[1], strBuf, sizeof(strBuf), &strSize) != napi_ok) {
        ReleaseGraphicsResources();
        return MakeBool(env, false);
    }
    std::string surfaceIdStr(strBuf);
    if (surfaceIdStr.empty()) {
        ReleaseGraphicsResources();
        return MakeBool(env, false);
    }
    uint64_t surfaceId = std::stoull(surfaceIdStr);
    OH_NativeWindow_CreateNativeWindowFromSurfaceId(surfaceId, &nativeWindow);
    if (nativeWindow == nullptr) {
        OH_LOG_ERROR(LOG_APP, "CreateNativeWindowFromSurfaceId failed, surfaceId=%{public}s", surfaceIdStr.c_str());
        ReleaseGraphicsResources();
        return MakeBool(env, false);
    }
    int32_t oldFormat = 0;
    int32_t getFormatRet = OH_NativeWindow_NativeWindowHandleOpt(nativeWindow, GET_FORMAT, &oldFormat);
    int32_t setFormatRet = OH_NativeWindow_NativeWindowHandleOpt(nativeWindow, SET_FORMAT,
        NATIVEBUFFER_PIXEL_FMT_RGBA_8888);
    int32_t newFormat = 0;
    OH_NativeWindow_NativeWindowHandleOpt(nativeWindow, GET_FORMAT, &newFormat);
    OH_LOG_INFO(LOG_APP,
        "native window format getRet=%{public}d old=%{public}d setRet=%{public}d new=%{public}d",
        getFormatRet, oldFormat, setFormatRet, newFormat);
    
    char filesDirBuf[512] = {0};
    if (napi_get_value_string_utf8(env, args[2], filesDirBuf, sizeof(filesDirBuf), &strSize) != napi_ok) {
        ReleaseGraphicsResources();
        return MakeBool(env, false);
    }
    std::string filesDir(filesDirBuf);
    OH_LOG_INFO(LOG_APP, "filesDir = %{public}s", filesDir.c_str());

    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
        OH_LOG_ERROR(LOG_APP, "eglGetDisplay failed, error=0x%{public}x", eglGetError());
        ReleaseGraphicsResources();
        return MakeBool(env, false);
    }

    if (eglInitialize(display, nullptr, nullptr) == EGL_FALSE) {
        OH_LOG_ERROR(LOG_APP, "eglInitialize failed, error=0x%{public}x", eglGetError());
        ReleaseGraphicsResources();
        return MakeBool(env, false);
    }

    EGLConfig config;
    EGLint numConfig = 0;
    EGLint attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_NONE
    };
    if (eglChooseConfig(display, attribs, &config, 1, &numConfig) == EGL_FALSE || numConfig <= 0) {
        OH_LOG_ERROR(LOG_APP, "eglChooseConfig failed, error=0x%{public}x, numConfig=%{public}d", eglGetError(), numConfig);
        ReleaseGraphicsResources();
        return MakeBool(env, false);
    }

    EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
    if (context == EGL_NO_CONTEXT) {
        OH_LOG_ERROR(LOG_APP, "eglCreateContext failed, error=0x%{public}x", eglGetError());
        ReleaseGraphicsResources();
        return MakeBool(env, false);
    }
    surface = eglCreateWindowSurface(display, config, (EGLNativeWindowType)nativeWindow, nullptr);
    if (surface == EGL_NO_SURFACE) {
        OH_LOG_ERROR(LOG_APP, "eglCreateWindowSurface failed, error=0x%{public}x", eglGetError());
        ReleaseGraphicsResources();
        return MakeBool(env, false);
    }
    if (eglMakeCurrent(display, surface, surface, context) == EGL_FALSE) {
        OH_LOG_ERROR(LOG_APP, "eglMakeCurrent failed, error=0x%{public}x", eglGetError());
        ReleaseGraphicsResources();
        return MakeBool(env, false);
    }

    EGLint surfW = 0, surfH = 0;
    eglQuerySurface(display, surface, EGL_WIDTH, &surfW);
    eglQuerySurface(display, surface, EGL_HEIGHT, &surfH);
    if (surfW <= 1 || surfH <= 1) {
        OH_LOG_ERROR(LOG_APP, "surface size invalid = %{public}dx%{public}d", surfW, surfH);
        ReleaseGraphicsResources();
        return MakeBool(env, false);
    }
    width = static_cast<float>(surfW);
    height = static_cast<float>(surfH);
    OH_LOG_INFO(LOG_APP, "surface size = %{public}dx%{public}d", surfW, surfH);

    const GLubyte* glVendor = glGetString(GL_VENDOR);
    const GLubyte* glRenderer = glGetString(GL_RENDERER);
    const GLubyte* glVersion = glGetString(GL_VERSION);
    OH_LOG_INFO(LOG_APP, "GL_VENDOR = %{public}s", glVendor ? reinterpret_cast<const char*>(glVendor) : "unknown");
    OH_LOG_INFO(LOG_APP, "GL_RENDERER = %{public}s", glRenderer ? reinterpret_cast<const char*>(glRenderer) : "unknown");
    OH_LOG_INFO(LOG_APP, "GL_VERSION = %{public}s", glVersion ? reinterpret_cast<const char*>(glVersion) : "unknown");
    
    g_camera.setViewportSize(width, height);
    g_camera.setPosition(g_CameraOrbitTarget + glm::vec3(0.0f, 0.0f, g_CameraDistance));
    g_camera.setTarget(g_CameraOrbitTarget);

    // Shader: concatenate common + main for the fragment shader
    refractionShader = new Shader(refractionVertexShader, refractionFragmentShader);
    backgroundShader = new Shader(backgroundVertexShader, backgroundFragmentShader);
    skyboxShader = new Shader(skyboxVertexShader, skyboxFragmentShader);
    quadShader = new Shader(quadVertexShader, quadFragmentShader);
    if (refractionShader->m_id == 0 || backgroundShader->m_id == 0 ||
        skyboxShader->m_id == 0 || quadShader->m_id == 0){
        OH_LOG_ERROR(LOG_APP, "create shader failed");
        ReleaseGraphicsResources();
        return MakeBool(env, false);
    }

    // ---- Cubemap ----
    std::vector<std::string> cubemapFaces = {
        "skybox/right.jpg", "skybox/left.jpg",
        "skybox/top.jpg", "skybox/bottom.jpg",
        "skybox/front.jpg", "skybox/back.jpg"
    };
    cubemapTexture = LoadCubemapTexture(g_ResourceManager, cubemapFaces);
    if (cubemapTexture == 0) {
        OH_LOG_ERROR(LOG_APP, "Failed to load cubemap texture");
        ReleaseGraphicsResources();
        return MakeBool(env, false);
    }

    // ---- Thin-film LUT texture ----
    {
        RawFile* lutFile = OH_ResourceManager_OpenRawFile(g_ResourceManager, "thinfilm_belcour_bubble.png");
        if (lutFile) {
            long sz = OH_ResourceManager_GetRawFileSize(lutFile);
            std::vector<unsigned char> buf(sz);
            OH_ResourceManager_ReadRawFile(lutFile, buf.data(), sz);
            OH_ResourceManager_CloseRawFile(lutFile);
            int w, h, ch;
            stbi_uc* data = stbi_load_from_memory(buf.data(), sz, &w, &h, &ch, 0);
            if (data) {
                glGenTextures(1, &g_ThinFilmLUTTexture);
                glBindTexture(GL_TEXTURE_2D, g_ThinFilmLUTTexture);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                stbi_image_free(data);
                OH_LOG_INFO(LOG_APP, "Thin-film LUT loaded: %{public}dx%{public}d", w, h);
            }
        } else {
            OH_LOG_WARN(LOG_APP, "Thin-film LUT not found, LUT mode will show black");
        }
    }

    // ---- Skybox model ----
    g_SkyboxModel = Model::CreateSkyboxCube();
    if (!g_SkyboxModel) {
        OH_LOG_ERROR(LOG_APP, "create skybox failed");
        ReleaseGraphicsResources();
        return MakeBool(env, false);
    }

    // ---- Decorative bubble model (smooth sphere) ----
    g_DecorativeBubbleModel = Model::CreateSphere(1.0f, 32, 16, true);
    if (!g_DecorativeBubbleModel) {
        OH_LOG_ERROR(LOG_APP, "create decorative bubble model failed");
        ReleaseGraphicsResources();
        return MakeBool(env, false);
    }

    ResetOpeningScene();

    // ---- DBSTT simulation init ----
    g_Sim.initIcosphere(g_BubbleRadius, g_SimSubdivs, g_SimPerturb);
    g_Sim.timeStep = 0.001f;
    g_Sim.substepsPerFrame = 1;
    {
        const auto& pos = g_Sim.getPositions();
        const auto& nrm = g_Sim.getNormals();
        std::vector<unsigned int> idx;
        for (const auto& t : g_Sim.getTriangles()) {
            idx.push_back((unsigned int)t.x);
            idx.push_back((unsigned int)t.y);
            idx.push_back((unsigned int)t.z);
        }
        g_RefractModel = Model::CreateFromVertices(pos, nrm, idx);
    }
    if (!g_RefractModel) {
        OH_LOG_ERROR(LOG_APP, "create refract model failed");
        ReleaseGraphicsResources();
        return MakeBool(env, false);
    }

    // ---- Background spheres ----
    float sphere_z = -5.0f;
    float sphere_radius = 0.15f;
    for (int i = 0; i < 5; ++i){
        for (int j = 0; j < 5; ++j){
            glm::vec3 pos = {-2.0f + float(i) * 1.0f, -2.0f + float(j) * 1.0f, sphere_z};
            g_SpherePositions.push_back(pos);
        }
    }
    for (auto& pos : g_SpherePositions) {
        Model* sphere = Model::CreateSphere(sphere_radius, 16, 8, false);
        if (sphere) g_BackgroundModels.push_back(sphere);
    }

    InitFBO(surfW, surfH);
    InitFullscreenQuad();
    StartBurstSimWorker();
    OH_LOG_INFO(LOG_APP, "FBO size = %{public}dx%{public}d, renderScale = %{public}.2f, display bubbles = %{public}zu, sim substeps = %{public}d, sim interval = %{public}d",
        g_FBOWidth, g_FBOHeight, kRenderScale, g_DisplayBubbles.size(),
        g_Sim.substepsPerFrame, kSimFrameInterval);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    return nullptr;
}

static napi_value RenderFrame(napi_env env, napi_callback_info info) {
    if (display == EGL_NO_DISPLAY || surface == EGL_NO_SURFACE) return nullptr;

    EGLint surfW = 0, surfH = 0;
    if (eglQuerySurface(display, surface, EGL_WIDTH, &surfW) == EGL_FALSE ||
        eglQuerySurface(display, surface, EGL_HEIGHT, &surfH) == EGL_FALSE ||
        surfW <= 0 || surfH <= 0) return nullptr;
    float w = static_cast<float>(surfW);
    float h = static_cast<float>(surfH);

    // ---- Time ----
    double now = 0.0;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    now = ts.tv_sec + ts.tv_nsec * 1e-9;
    if (g_LastFrameTime == 0.0) g_LastFrameTime = now;
    double dt = now - g_LastFrameTime;
    g_LastFrameTime = now;
    const double visualDt = g_VisualPaused ? 0.0 : dt;
    g_Time += visualDt;
    if (dt > 1e-6) {
        double instantFps = 1.0 / dt;
        g_CurrentFps = (g_CurrentFps <= 0.0)
            ? instantFps
            : g_CurrentFps * 0.85 + instantFps * 0.15;
    }
    if (g_FpsLogWindowStart == 0.0) {
        g_FpsLogWindowStart = now;
    }
    ++g_FpsLogFrameCount;
    double fpsLogElapsed = now - g_FpsLogWindowStart;
    if (fpsLogElapsed >= 15.0) {
        double avgFps = static_cast<double>(g_FpsLogFrameCount) / fpsLogElapsed;
        OH_LOG_INFO(LOG_APP,
            "perf avgFps15s=%{public}.2f smoothFps=%{public}.2f surface=%{public}dx%{public}d fbo=%{public}dx%{public}d renderScale=%{public}.2f displayBubbles=%{public}zu simSubsteps=%{public}d simInterval=%{public}d",
            avgFps, g_CurrentFps, surfW, surfH, g_FBOWidth, g_FBOHeight,
            kRenderScale, g_DisplayBubbles.size(), g_Sim.substepsPerFrame,
            kSimFrameInterval);
        g_FpsLogFrameCount = 0;
        g_FpsLogWindowStart = now;
    }

    // ---- Touch decay ----
    if (!g_TouchPressed && !g_VisualPaused) {
        g_TouchStrength *= expf(-3.2f * (float)visualDt);
        g_TouchVelocity  *= expf(-5.0f * (float)visualDt);
        if (g_TouchStrength < 0.01f) {
            g_TouchStrength = 0.0f;
            g_TouchPoint = glm::vec2(-10.0f, -10.0f);
        }
    }

    // ---- Camera ----
    g_camera.setViewportSize(w, h);
    float radius = g_CameraDistance;
    float pitch = glm::radians(currentAngleX);
    float yaw   = glm::radians(currentAngleY);
    pitch = glm::clamp(pitch, glm::radians(-89.0f), glm::radians(89.0f));
    glm::vec3 cameraPos = g_CameraOrbitTarget + glm::vec3(
                        radius * cos(pitch) * sin(yaw),
                        radius * sin(pitch),
                        radius * cos(pitch) * cos(yaw));
    g_camera.setPosition(cameraPos);
    g_camera.setTarget(g_CameraOrbitTarget);

    if (!g_VisualPaused) {
        windows_parity::UpdateDisplayBubbleInteractions(static_cast<float>(visualDt));
    }

    glm::mat4 view = g_camera.getViewMatrix();
    glm::mat4 proj = g_camera.getProjectionMatrix();

    // ---- SetRefractUniforms: per-bubble with individual model matrix ----
    auto SetRefractUniforms = [&](const glm::mat4& model, float localRadius, bool isBackFace,
                                  bool renderToFBO, bool interactive, float geometryWobbleStrength,
                                  int iridescenceMode, float outputAlpha) {
        float targetW = renderToFBO ? static_cast<float>(g_FBOWidth) : w;
        float targetH = renderToFBO ? static_cast<float>(g_FBOHeight) : h;
        glm::vec3 center = glm::vec3(model * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        float dist = glm::length(cameraPos - center);
        float fovVert = glm::radians(g_camera.getFov());
        float pixelsPerRadian = (static_cast<float>(g_FBOHeight) / 2.0f) / tanf(fovVert / 2.0f);
        float angularRadius = atanf((localRadius * 1.15f) / std::max(dist, 0.001f));
        float spherePixelRadius = angularRadius * pixelsPerRadian;

        refractionShader->Use();
        refractionShader->SetMat4("uModel", model);
        refractionShader->SetMat4("uView", view);
        refractionShader->SetMat4("uProj", proj);
        refractionShader->SetVec3("uCameraPos", cameraPos);
        refractionShader->SetFloat("uGeometryWobbleStrength", geometryWobbleStrength);
        refractionShader->SetFloat("uContactDeformStrength", 0.0f);
        refractionShader->SetInt("uVisualContactCount", 0);
        refractionShader->SetInt("uForceSharedFilm", 0);
        refractionShader->SetFloat("uFresnelPower", g_FresnelPower);
        refractionShader->SetFloat("uShininess", g_Shininess);
        refractionShader->SetFloat("uDiffuseness", g_Diffuseness);
        refractionShader->SetFloat("uRefractionStrength", g_RefractionStrength);
        refractionShader->SetFloat("uEdgeDistortionBoost", g_EdgeDistortionBoost);
        refractionShader->SetFloat("uMaxOffsetRatio", g_MaxOffsetRatio);
        refractionShader->SetFloat("uEnvironmentReflectionStrength", g_EnvironmentReflectionStrength);
        refractionShader->SetFloat("uSpherePixelRadius", spherePixelRadius);
        refractionShader->SetInt("uIsBackFace", isBackFace ? 1 : 0);
        refractionShader->SetInt("uRenderToFBO", renderToFBO ? 1 : 0);
        refractionShader->SetInt("uIridescenceMode", iridescenceMode);
        refractionShader->SetInt("uBackgroundTexture", 0);
        refractionShader->SetInt("uEnvironmentMap", 1);
        refractionShader->SetInt("uThinFilmLUT", 2);
        refractionShader->SetVec3("uLight", light);
        refractionShader->SetVec2("uWinResolution", {targetW, targetH});
        refractionShader->SetVec2("uFBOSize", {(float)g_FBOWidth, (float)g_FBOHeight});
        refractionShader->SetFloat("uThickness", CurrentThickness());
        refractionShader->SetFloat("uThicknessVar", g_ThicknessVar);
        refractionShader->SetFloat("uTime", (float)g_Time);
        refractionShader->SetVec3("uTextureOriginWorld", center);
        refractionShader->SetMat3("uTextureBasis", glm::mat3(1.0f));
        refractionShader->SetFloat("uTextureReferenceRadius",
                                   std::max(localRadius, 0.001f));
        refractionShader->SetFloat("uVisualTime", (float)g_Time);
        refractionShader->SetVec3("uFilmNoiseOffset", glm::vec3(0.0f));
        refractionShader->SetFloat("uFilmBaseThicknessScale", 1.0f);
        refractionShader->SetFloat("uFilmThicknessAmplitudeScale", 1.0f);
        refractionShader->SetFloat("uFilmIor", 1.33f);
        refractionShader->SetInt("uFusionDiagnosticMode", 0);
        refractionShader->SetFloat("uOutputAlpha", outputAlpha);
        refractionShader->SetFloat("uOpticalNormalBlend", 0.0f);
        refractionShader->SetVec2("uTouchPoint", interactive ? g_TouchPoint : glm::vec2(-10.0f, -10.0f));
        refractionShader->SetFloat("uTouchStrength", interactive ? g_TouchStrength : 0.0f);
        refractionShader->SetFloat("uTouchVelocity", interactive ? g_TouchVelocity : 0.0f);
    };

    auto ApplyPersistentVisualState = [&](const BubbleVisualState& state) {
        if (!state.initialized) return;
        auto seedComponent = [](uint32_t seed, uint32_t shift) {
            uint32_t value = (seed >> shift) & 0x3ffu;
            return static_cast<float>(value) / 1023.0f * 17.0f;
        };
        glm::vec3 noiseOffset(
            seedComponent(state.noiseSeed, 0u),
            seedComponent(state.noiseSeed ^ 0x9e3779b9u, 10u),
            seedComponent(state.noiseSeed ^ 0x85ebca6bu, 20u));
        float elapsed = std::max(
            static_cast<float>(g_Time) - state.animationTimeOrigin, 0.0f);
        refractionShader->SetVec3("uTextureOriginWorld",
                                  state.textureOriginWorld);
        refractionShader->SetMat3("uTextureBasis", state.textureBasis);
        refractionShader->SetFloat(
            "uTextureReferenceRadius",
            std::max(state.referenceRadius, 0.001f));
        refractionShader->SetFloat(
            "uVisualTime", elapsed + state.interferencePhase);
        refractionShader->SetVec3("uFilmNoiseOffset", noiseOffset);
        refractionShader->SetFloat("uFilmBaseThicknessScale",
                                   state.filmBaseThicknessScale);
        refractionShader->SetFloat("uFilmThicknessAmplitudeScale",
                                   state.filmThicknessAmplitudeScale);
        refractionShader->SetFloat("uFilmIor", state.ior);
        refractionShader->SetFloat("uFresnelPower",
                                   g_FresnelPower *
                                       state.fresnelStrength);
        refractionShader->SetFloat(
            "uOpticalNormalBlend",
            state.opticalNormalBlendAtOrigin *
                std::exp(-state.opticalNormalRelaxationRate * elapsed));
    };

    auto BindRefractionInputs = [&](GLuint backgroundTex) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, backgroundTex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_CUBE_MAP, cubemapTexture);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, g_ThinFilmLUTTexture);
    };

    auto DrawDisplayBubbles = [&](bool isBackFace, bool renderToFBO, GLuint backgroundTex,
                                  bool straightComposite) {
        BindRefractionInputs(backgroundTex);

        std::vector<std::pair<float, const RenderOnlyBubble*>> sortedBubbles;
        sortedBubbles.reserve(g_RenderOnlyBubbles.size());
        for (const auto& bubble : g_RenderOnlyBubbles) {
            glm::vec3 center = glm::vec3(RenderOnlyBubbleModelMatrix(bubble, (float)g_Time)
                * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
            glm::vec3 toCamera = center - cameraPos;
            sortedBubbles.emplace_back(glm::dot(toCamera, toCamera), &bubble);
        }
        std::sort(sortedBubbles.begin(), sortedBubbles.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });

        GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
        if (straightComposite) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
        }

        for (const auto& item : sortedBubbles) {
            const auto& bubble = *item.second;
            glm::mat4 bubbleModel = RenderOnlyBubbleModelMatrix(bubble, (float)g_Time);
            float alpha = straightComposite ? 0.72f : 1.0f;
            SetRefractUniforms(bubbleModel, bubble.radius, isBackFace, renderToFBO,
                               false, 0.055f, 2, alpha);
            g_DecorativeBubbleModel->Draw(*refractionShader);
        }

        if (straightComposite) {
            glDepthMask(GL_TRUE);
            if (!blendWasEnabled) glDisable(GL_BLEND);
        }
    };

    auto SetBubbleContactPlanes = [&](const DisplayBubble& bubble) {
        int count = 0;
        for (const BubbleContactPair& pair : g_ContactPairs) {
            if (count >= 4 || !pair.active || pair.contactActivation <= 0.001f ||
                pair.state == BubbleContactPair::State::Separated) continue;
            bool isA = pair.a == bubble.id;
            bool isB = pair.b == bubble.id;
            if (!isA && !isB) continue;
            glm::vec3 normal = isA ? pair.filteredNormal : -pair.filteredNormal;
            float fusionRetention = 1.0f - Smooth01(pair.relaxationProgress);
            std::string suffix = "[" + std::to_string(count) + "]";
            GLint pointLocation = glGetUniformLocation(
                refractionShader->m_id, ("uVisualContactPlanePoints" + suffix).c_str());
            GLint normalLocation = glGetUniformLocation(
                refractionShader->m_id, ("uVisualContactPlaneNormals" + suffix).c_str());
            glUniform3fv(pointLocation, 1, glm::value_ptr(pair.filteredPlaneCenter));
            glUniform3fv(normalLocation, 1, glm::value_ptr(normal));
            glUniform1f(glGetUniformLocation(
                refractionShader->m_id, ("uVisualContactBlendWidths" + suffix).c_str()),
                std::max(0.008f, bubble.radius * 0.025f));
            glUniform1f(glGetUniformLocation(
                refractionShader->m_id, ("uVisualContactStrengths" + suffix).c_str()),
                pair.contactActivation * fusionRetention);
            ++count;
        }
        refractionShader->SetInt("uVisualContactCount", count);
    };

    bool interactiveMeshesUpdated = false;
    bool fusionMeshesUpdated = false;
    bool contactMeshesUpdated = false;
    auto DrawInteractiveBubbles = [&](bool isBackFace, bool renderToFBO, GLuint backgroundTex) {
        BindRefractionInputs(backgroundTex);
        GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
        if (!isBackFace) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
        }

        std::vector<std::pair<float, DisplayBubble*>> sortedBubbles;
        for (DisplayBubble& bubble : g_DisplayBubbles) {
            if (bubble.state == DisplayBubble::State::Dead) continue;
            bool worldScaleSurface = g_BubbleSurfaces.BubbleUsesWorldScale(bubble.id);
            glm::vec3 center = glm::vec3(
                windows_parity::PersistentBubbleModelMatrix(
                    bubble, static_cast<float>(g_Time), worldScaleSurface) *
                glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
            float distanceSq = glm::dot(center - cameraPos, center - cameraPos);
            sortedBubbles.emplace_back(distanceSq, &bubble);
        }
        std::sort(sortedBubbles.begin(), sortedBubbles.end(),
                  [](const auto& lhs, const auto& rhs) {
                      bool lhsPromoted = lhs.second->volumeTransferred;
                      bool rhsPromoted = rhs.second->volumeTransferred;
                      if (lhsPromoted != rhsPromoted)
                          return !lhsPromoted;
                      return lhs.first > rhs.first;
                  });

        for (const auto& entry : sortedBubbles) {
            DisplayBubble& bubble = *entry.second;
            bool worldScaleSurface = g_BubbleSurfaces.BubbleUsesWorldScale(bubble.id);
            float fusionVisibility = windows_parity::BubbleFusionSurfaceVisibility(bubble.id);
            float shellAlpha = 0.72f * bubble.alpha;
            if (bubble.state != DisplayBubble::State::Burst) {
                shellAlpha *= 1.0f - fusionVisibility;
            }
            if (shellAlpha <= 0.005f) continue;
            glm::mat4 model = windows_parity::PersistentBubbleModelMatrix(
                bubble, static_cast<float>(g_Time), worldScaleSurface);
            SetRefractUniforms(model, bubble.radius, isBackFace, renderToFBO,
                               false, 0.0f,
                               g_IridescenceMode, shellAlpha);
            ApplyPersistentVisualState(bubble.visualState);
            SetBubbleContactPlanes(bubble);

            Model* modelToDraw = g_BubbleSurfaces.FindBubble(bubble.id);
            if (bubble.state == DisplayBubble::State::Burst) {
                if (modelToDraw && !interactiveMeshesUpdated) {
                    Mesh* mesh = modelToDraw->getMesh(0);
                    if (mesh) {
                        ApplyBurstSnapshot(
                            bubble.id,
                            bubble.burstElapsed * kBurstSimulationTimeScale,
                            mesh);
                    }
                }
                refractionShader->SetInt("uVisualContactCount", 0);
            } else if (modelToDraw && !bubble.volumeTransferred && !interactiveMeshesUpdated) {
                Mesh* mesh = modelToDraw->getMesh(0);
                if (mesh) {
                    const std::vector<Vertex>* restVertices =
                        g_BubbleSurfaces.FindBubbleRestVertices(bubble.id);
                    std::vector<Vertex> vertices = windows_parity::BuildPersistentBubbleVertices(
                        FindBubbleIndexById(g_DisplayBubbles, bubble.id),
                        static_cast<float>(g_Time), restVertices,
                        worldScaleSurface, &mesh->vertices);
                    ApplyAmbientDbsttDeformation(
                        bubble, worldScaleSurface, mesh->indices, vertices);
                    mesh->updateVertices(vertices);
                }
            }
            if (!modelToDraw) modelToDraw = g_DecorativeBubbleModel;
            if (modelToDraw) modelToDraw->Draw(*refractionShader);
        }

        if (g_AddPreviewVisible && !isBackFace && g_DecorativeBubbleModel) {
            glm::mat4 previewModel = glm::translate(glm::mat4(1.0f), g_AddPreviewPosition);
            previewModel = glm::scale(previewModel, glm::vec3(g_AddPreviewRadius));
            SetRefractUniforms(previewModel, g_AddPreviewRadius, false, renderToFBO,
                               false, 0.0f, g_IridescenceMode, 0.34f);
            refractionShader->SetInt("uVisualContactCount", 0);
            g_DecorativeBubbleModel->Draw(*refractionShader);
        }
        interactiveMeshesUpdated = true;

        for (BubbleContactPair& pair : g_ContactPairs) {
            int ai = -1, bi = -1;
            if (!pair.active || !ResolveContactPairIndices(g_DisplayBubbles, pair, ai, bi)) continue;
            DisplayBubble& a = g_DisplayBubbles[static_cast<size_t>(ai)];
            DisplayBubble& b = g_DisplayBubbles[static_cast<size_t>(bi)];
            if (a.state == DisplayBubble::State::Dead || b.state == DisplayBubble::State::Dead) continue;

            float fusionVisibility = windows_parity::FusionSurfaceVisibility(pair);
            if (fusionVisibility > 0.001f && pair.fusionProfileInitialized) {
                float volumeA = a.targetVolume > 0.0f ? a.targetVolume : BubbleVolume(a.radius);
                float volumeB = b.targetVolume > 0.0f ? b.targetVolume : BubbleVolume(b.radius);
                float targetVolume = pair.conservedFusionVolume;
                float targetRadius = RadiusFromVolume(targetVolume);
                glm::vec3 targetCenter =
                    pair.fusionCenter +
                    pair.fusionCenterVelocity * pair.fusionElapsed;
                glm::vec3 axis = pair.fusionFrameInitialized
                    ? pair.fusionAxis : b.position - a.position;
                if (!windows_parity::IsFiniteDirection(axis) || glm::length(axis) <= 1e-5f) {
                    axis = pair.contactFrameInitialized
                        ? pair.filteredNormal : glm::vec3(1.0f, 0.0f, 0.0f);
                }
                axis = windows_parity::SafeNormalizeDirection(axis, glm::vec3(1.0f, 0.0f, 0.0f));
                glm::vec3 side;
                glm::vec3 binormal;
                if (pair.fusionFrameInitialized) {
                    side = windows_parity::SafeNormalizeDirection(
                        pair.fusionSide - axis * glm::dot(pair.fusionSide, axis), pair.fusionSide);
                    binormal = windows_parity::SafeNormalizeDirection(
                        glm::cross(axis, side), pair.fusionBinormal);
                } else {
                    windows_parity::BuildStableOrthonormalFrame(axis, side, binormal);
                }
                glm::mat3 fusionBasis(1.0f);
                fusionBasis[0] = axis;
                fusionBasis[1] = side;
                fusionBasis[2] = binormal;
                FusionSurfaceParameters parameters;
                parameters.centerA = glm::dot(a.position - targetCenter, axis);
                parameters.centerB = glm::dot(b.position - targetCenter, axis);
                parameters.radiusA = a.radius;
                parameters.radiusB = b.radius;
                parameters.opticalReferenceRadius = targetRadius;
                std::vector<Vertex> fusionVertices =
                    BuildFusionSurfaceVertices(
                        parameters, pair.fusionProfile);
                for (Vertex& vertex : fusionVertices) {
                    vertex.Position = fusionBasis * vertex.Position;
                    vertex.Normal = windows_parity::SafeNormalizeDirection(
                        fusionBasis * vertex.Normal, vertex.Normal);
                    vertex.FilmDirection = windows_parity::SafeNormalizeDirection(
                        fusionBasis * vertex.FilmDirection, vertex.Normal);
                }
                Model* fusionModel = g_BubbleSurfaces.FindFusion(pair.a, pair.b);
                if (!fusionModel) {
                    fusionModel = g_BubbleSurfaces.EnsureFusion(
                        pair.a, pair.b, fusionVertices, BuildFusionSurfaceIndices());
                } else if (!fusionMeshesUpdated) {
                    Mesh* mesh = fusionModel->getMesh(0);
                    if (mesh) {
                        mesh->updateVertices(fusionVertices);
                    }
                }
                glm::mat4 fusionMatrix = glm::translate(glm::mat4(1.0f), targetCenter);
                SetRefractUniforms(fusionMatrix, targetRadius, isBackFace, renderToFBO,
                                   false, 0.0f, g_IridescenceMode,
                                   0.72f * std::max(a.alpha, b.alpha) *
                                   fusionVisibility);
                ApplyPersistentVisualState(pair.visualState);
                refractionShader->SetInt("uVisualContactCount", 0);
                refractionShader->SetInt("uForceSharedFilm", 0);
                if (fusionModel) fusionModel->Draw(*refractionShader);
            }

            // The shared contact film belongs to the pre-fusion topology. Once
            // the unified fusion surface is active, drawing both surfaces
            // produces a refractive ring at the former neck.
            float filmVisibility = pair.contactActivation * pair.contactActivation;
            float filmAlpha = 0.16f * filmVisibility;
            if (!pair.fusionActive && filmAlpha > 0.002f && pair.contactRadius > 0.002f) {
                float pulse = 1.0f + std::sin(static_cast<float>(g_Time) * 0.70f +
                    static_cast<float>(pair.a * 0.37f + pair.b * 0.19f)) * 0.008f;
                float filmRadius = std::max(0.002f, pair.contactRadius * pulse);
                float normalizedCurvature =
                    windows_parity::ContactFilmWorldCurvature(a, b, pair) * filmRadius;
                std::vector<Vertex> contactVertices =
                    BuildCurvedContactFilmVertices(normalizedCurvature, 0.0f);
                for (Vertex& vertex : contactVertices) {
                    glm::vec3 materialDirection(
                        vertex.Position.x, vertex.Position.y, 0.35f + vertex.Position.z);
                    vertex.FilmDirection = windows_parity::SafeNormalizeDirection(
                        materialDirection, glm::vec3(0.0f, 0.0f, 1.0f));
                }
                Model* filmModel = g_BubbleSurfaces.FindContact(pair.a, pair.b);
                if (!filmModel) {
                    filmModel = g_BubbleSurfaces.EnsureContact(
                        pair.a, pair.b, contactVertices, BuildContactFilmDisc(72).indices);
                } else if (!contactMeshesUpdated) {
                    Mesh* mesh = filmModel->getMesh(0);
                    if (mesh) {
                        windows_parity::PreserveFilmCoordinates(contactVertices, mesh->vertices);
                        mesh->updateVertices(contactVertices);
                    }
                }
                glm::mat4 filmMatrix = windows_parity::MakeContactFilmModel(
                    a, b, pair, static_cast<float>(g_Time));
                SetRefractUniforms(filmMatrix, pair.contactRadius, isBackFace, renderToFBO,
                                   false, 0.0f, g_IridescenceMode, filmAlpha);
                refractionShader->SetInt("uVisualContactCount", 0);
                refractionShader->SetInt("uForceSharedFilm", 1);
                if (filmModel) filmModel->Draw(*refractionShader);
            }
        }
        refractionShader->SetInt("uForceSharedFilm", 0);
        fusionMeshesUpdated = true;
        contactMeshesUpdated = true;

        if (!isBackFace) {
            glDepthMask(GL_TRUE);
            if (!blendWasEnabled) glDisable(GL_BLEND);
        }
    };

    auto SetBackgroundUniforms = [&]() {
        backgroundShader->Use();
        for (size_t i = 0; i < g_BackgroundModels.size(); ++i) {
            glm::mat4 bgModel = glm::translate(glm::mat4(1.0f), g_SpherePositions[i]);
            glm::mat4 mvp = g_camera.getMVP(bgModel);
            backgroundShader->SetMat4("uMVP", mvp);
            g_BackgroundModels[i]->Draw(*backgroundShader);
        }
    };

    auto DrawSkybox = [&]() {
        skyboxShader->Use();
        glm::mat4 viewNT = glm::mat4(glm::mat3(view));
        skyboxShader->SetMat4("projection", proj);
        skyboxShader->SetMat4("view", viewNT);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_CUBE_MAP, cubemapTexture);
        skyboxShader->SetInt("skybox", 0);
        g_SkyboxModel->Draw(*skyboxShader);
    };

    int fboW = g_FBOWidth, fboH = g_FBOHeight;

    // ===== Pass 1: Background → backgroundFBO (overscan) =====
    glBindFramebuffer(GL_FRAMEBUFFER, backgroundFBO);
    glViewport(0, 0, fboW, fboH);
    glEnable(GL_DEPTH_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glCullFace(GL_BACK);
    glDepthFunc(GL_LEQUAL); glDepthMask(GL_FALSE);
    DrawSkybox();
    glDepthMask(GL_TRUE); glDepthFunc(GL_LESS);
    SetBackgroundUniforms();

    // ===== Pass 2: Render scene behind main bubble =====
    glBindFramebuffer(GL_FRAMEBUFFER, sceneBehindFBO);
    glViewport(0, 0, fboW, fboH);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glCullFace(GL_BACK);
    glDepthFunc(GL_LEQUAL); glDepthMask(GL_FALSE);
    DrawSkybox();
    glDepthMask(GL_TRUE); glDepthFunc(GL_LESS);
    SetBackgroundUniforms();
    DrawDisplayBubbles(false, true, backgroundTexture, true);
    DrawInteractiveBubbles(false, true, backgroundTexture);

    // ===== Pass 3: Render main bubble back faces =====
    // The main bubble samples the scene behind it, including decorative bubbles.
    glBindFramebuffer(GL_FRAMEBUFFER, backFaceFBO);
    glViewport(0, 0, fboW, fboH);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glCullFace(GL_FRONT);

    // ===== Pass 4: Render main-only scene =====
    // Foreground decorative bubbles sample the main-only scene.
    glBindFramebuffer(GL_FRAMEBUFFER, mainSceneFBO);
    glViewport(0, 0, fboW, fboH);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glCullFace(GL_BACK);
    glDepthFunc(GL_LEQUAL); glDepthMask(GL_FALSE);
    DrawSkybox();
    glDepthMask(GL_TRUE); glDepthFunc(GL_LESS);
    SetBackgroundUniforms();

    // ===== Pass 5: Composite complete scene into the low-res final target =====
    glBindFramebuffer(GL_FRAMEBUFFER, finalSceneFBO);
    glViewport(0, 0, fboW, fboH);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glCullFace(GL_BACK);
    glDepthFunc(GL_LEQUAL); glDepthMask(GL_FALSE);
    DrawSkybox();
    glDepthMask(GL_TRUE); glDepthFunc(GL_LESS);
    SetBackgroundUniforms();
    DrawInteractiveBubbles(false, true, mainSceneTexture);
    DrawDisplayBubbles(false, true, mainSceneTexture, true);

    // ===== Pass 6: Upscale final scene to the screen =====
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, surfW, surfH);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glClear(GL_COLOR_BUFFER_BIT);
    quadShader->Use();
    quadShader->SetInt("uTexture", 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, finalSceneTexture);
    glBindVertexArray(quadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);

    eglSwapBuffers(display, surface);
    if (!g_VisualPaused) {
        // Present contact and shock-wave motion first; prepare the next burst
        // membrane state afterward so DBSTT cannot delay visible feedback.
        RequestBubbleSimUpdates();
    }
    return MakeBool(env, true);
}

static napi_value SetInputViewport(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    double logicalW = 0.0;
    double logicalH = 0.0;
    if (argc >= 1) {
        napi_get_value_double(env, args[0], &logicalW);
    }
    if (argc >= 2) {
        napi_get_value_double(env, args[1], &logicalH);
    }

    if (logicalW > 1e-3 && logicalH > 1e-3) {
        g_InputViewportWidth = static_cast<float>(logicalW);
        g_InputViewportHeight = static_cast<float>(logicalH);
        OH_LOG_INFO(LOG_APP,
            "setInputViewport logical=(%{public}.2f,%{public}.2f) render=(%{public}.2f,%{public}.2f)",
            g_InputViewportWidth, g_InputViewportHeight, width, height);
    } else {
        g_InputViewportWidth = 0.0f;
        g_InputViewportHeight = 0.0f;
        OH_LOG_WARN(LOG_APP, "setInputViewport reset because input size is invalid");
    }
    return nullptr;
}

static napi_value GetFps(napi_env env, napi_callback_info info)
{
    napi_value result;
    napi_create_double(env, g_CurrentFps, &result);
    return result;
}

static napi_value OnTouch(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    double x = 0.0, y = 0.0, type = 0.0;
    napi_get_value_double(env, args[0], &x);
    napi_get_value_double(env, args[1], &y);
    napi_get_value_double(env, args[2], &type);
    int touchType = static_cast<int>(type);
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);

    if (g_InputViewportWidth > 1e-3f && g_InputViewportHeight > 1e-3f && width > 1e-3f && height > 1e-3f) {
        float scaleX = width / g_InputViewportWidth;
        float scaleY = height / g_InputViewportHeight;
        fx *= scaleX;
        fy *= scaleY;
    }

    if (touchType == 0) { // Down
        g_TouchPressed = true;
        g_LastTouchX = fx;
        g_LastTouchY = fy;
        g_TouchPoint = {fx / fmaxf(width, 1.0f), 1.0f - fy / fmaxf(height, 1.0f)};
        g_TouchStrength = 1.0f;
        g_TouchVelocity = 0.0f;
    }else if (touchType == 2) { // Move
        float dx = fx - g_LastTouchX;
        float dy = fy - g_LastTouchY;
        float norm = fmaxf(fmaxf(width, height), 1.0f);
        g_TouchVelocity = glm::clamp(glm::length(glm::vec2(dx, dy)) / norm * 18.0f, 0.0f, 1.0f);
        g_TouchStrength = glm::clamp(0.65f + g_TouchVelocity * 0.9f, 0.0f, 1.35f);
        g_TouchPoint = {fx / fmaxf(width, 1.0f), 1.0f - fy / fmaxf(height, 1.0f)};
        g_LastTouchX = fx;
        g_LastTouchY = fy;
    } else if (touchType == 1) { // Up
        g_TouchPressed = false;
    }

    return nullptr;
}

// ---- Parameter control NAPI ----
static napi_value SetIridescenceMode(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t mode; napi_get_value_int32(env, args[0], &mode);
    g_IridescenceMode = mode % 3;
    OH_LOG_INFO(LOG_APP, "IridescenceMode = %{public}d", g_IridescenceMode);
    return nullptr;
}
static napi_value SetSurfaceTension(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    double val; napi_get_value_double(env, args[0], &val);
    g_Sim.surfaceTensionStrength = (float)val;
    return nullptr;
}
static napi_value SetRefractionStrength(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    double val; napi_get_value_double(env, args[0], &val);
    g_RefractionStrength = (float)val;
    return nullptr;
}
static napi_value SetFresnelPower(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    double val; napi_get_value_double(env, args[0], &val);
    g_FresnelPower = glm::clamp((float)val, 0.5f, 20.0f);
    return nullptr;
}
static napi_value SetEdgeDistortionBoost(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    double val; napi_get_value_double(env, args[0], &val);
    g_EdgeDistortionBoost = glm::clamp((float)val, 1.0f, 3.0f);
    return nullptr;
}
static napi_value SetMaxOffsetRatio(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    double val; napi_get_value_double(env, args[0], &val);
    g_MaxOffsetRatio = glm::clamp((float)val, 0.1f, 1.0f);
    return nullptr;
}
static napi_value SetEnvironmentReflectionStrength(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    double val; napi_get_value_double(env, args[0], &val);
    g_EnvironmentReflectionStrength = glm::clamp((float)val, 0.0f, 1.5f);
    return nullptr;
}
static napi_value SetCameraDistance(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    double val; napi_get_value_double(env, args[0], &val);
    g_CameraDistance = glm::clamp((float)val, 2.0f, 20.0f);
    return nullptr;
}
static napi_value ToggleSimulation(napi_env env, napi_callback_info info) {
    g_SimPaused = !g_SimPaused;
    g_VisualPaused = g_SimPaused;
    OH_LOG_INFO(LOG_APP, "Visual simulation %{public}s", g_VisualPaused ? "PAUSED" : "RUNNING");
    return nullptr;
}
static napi_value SetThickness(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    double val; napi_get_value_double(env, args[0], &val);
    CurrentThickness() = glm::clamp((float)val, 100.0f, 2000.0f);
    return nullptr;
}
static napi_value SetThicknessVar(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    double val; napi_get_value_double(env, args[0], &val);
    g_ThicknessVar = glm::clamp((float)val, 0.0f, 500.0f);
    return nullptr;
}
static napi_value RotateCamera(napi_env env, napi_callback_info info) {
    size_t argc = 2; napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    double dx = 0.0, dy = 0.0;
    if (argc >= 1) napi_get_value_double(env, args[0], &dx);
    if (argc >= 2) napi_get_value_double(env, args[1], &dy);
    currentAngleY += (float)dx * 0.3f;
    currentAngleX += (float)dy * 0.3f;
    currentAngleX = glm::clamp(currentAngleX, -89.0f, 89.0f);
    return nullptr;
}

static napi_value ResetCameraViewApi(napi_env env, napi_callback_info info) {
    currentAngleX = 0.0f;
    currentAngleY = 0.0f;
    g_CameraDistance = g_InteractionDemoActive ? 5.10f : 4.0f;
    napi_value result;
    napi_create_double(env, g_CameraDistance, &result);
    return result;
}

static void MapLogicalPointToSurface(float& x, float& y) {
    if (g_InputViewportWidth > 1e-3f && g_InputViewportHeight > 1e-3f &&
        width > 1e-3f && height > 1e-3f) {
        x *= width / g_InputViewportWidth;
        y *= height / g_InputViewportHeight;
    }
}

static glm::vec3 SpawnPositionFromSurfacePoint(float x, float y, float depth) {
    glm::vec3 origin = g_camera.getPosition();
    glm::vec3 direction = g_camera.getRayDirectionFromScreen(x, y);
    float denominator = direction.z;
    float t = std::abs(denominator) > 1e-5f
        ? (depth - origin.z) / denominator
        : g_CameraDistance;
    if (t < 0.25f) t = g_CameraDistance;
    return origin + direction * t;
}

static napi_value ResetOpeningSceneApi(napi_env env, napi_callback_info info) {
    ResetOpeningScene();
    return nullptr;
}

static napi_value CycleInteractionDemoApi(napi_env env, napi_callback_info info) {
    CycleInteractionDemo();
    napi_value result;
    napi_create_int32(env, g_InteractionDemoIndex, &result);
    return result;
}

static napi_value SetWindStrengthApi(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    double value = 0.0;
    if (argc >= 1) napi_get_value_double(env, args[0], &value);
    g_GlobalWindStrength = glm::clamp(static_cast<float>(value), 0.0f, 0.45f);
    g_WindEnabled = g_GlobalWindStrength > 0.001f;
    return nullptr;
}

static napi_value SetWindDirectionApi(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    double x = 1.0;
    double y = 0.2;
    if (argc >= 1) napi_get_value_double(env, args[0], &x);
    if (argc >= 2) napi_get_value_double(env, args[1], &y);
    glm::vec3 direction(static_cast<float>(x), static_cast<float>(y), 0.0f);
    if (glm::length(direction) > 1e-5f) {
        g_GlobalWindDirection = glm::normalize(direction);
    }
    return nullptr;
}

static napi_value AddBubbleAtScreenApi(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    double x = 0.0, y = 0.0, radius = 0.48, depth = 0.0;
    if (argc >= 1) napi_get_value_double(env, args[0], &x);
    if (argc >= 2) napi_get_value_double(env, args[1], &y);
    if (argc >= 3) napi_get_value_double(env, args[2], &radius);
    if (argc >= 4) napi_get_value_double(env, args[3], &depth);
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);
    MapLogicalPointToSurface(fx, fy);
    glm::vec3 position = SpawnPositionFromSurfacePoint(
        fx, fy, static_cast<float>(depth));
    if (g_DisplayBubbles.size() >= 24) {
        return MakeBool(env, false);
    }
    AddInteractiveBubble(position, glm::clamp(static_cast<float>(radius), 0.16f, 1.10f));
    return MakeBool(env, true);
}

static napi_value SetAddPreviewApi(napi_env env, napi_callback_info info) {
    size_t argc = 5;
    napi_value args[5];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    double x = 0.0, y = 0.0, radius = 0.48, depth = 0.0;
    bool visible = false;
    if (argc >= 1) napi_get_value_double(env, args[0], &x);
    if (argc >= 2) napi_get_value_double(env, args[1], &y);
    if (argc >= 3) napi_get_value_double(env, args[2], &radius);
    if (argc >= 4) napi_get_value_double(env, args[3], &depth);
    if (argc >= 5) napi_get_value_bool(env, args[4], &visible);
    g_AddPreviewVisible = visible;
    if (visible) {
        float fx = static_cast<float>(x);
        float fy = static_cast<float>(y);
        MapLogicalPointToSurface(fx, fy);
        g_AddPreviewPosition = SpawnPositionFromSurfacePoint(
            fx, fy, static_cast<float>(depth));
        g_AddPreviewRadius = glm::clamp(static_cast<float>(radius), 0.16f, 1.10f);
    }
    return nullptr;
}

static napi_value BurstBubbleAtScreenApi(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    double x = 0.0, y = 0.0;
    if (argc >= 1) napi_get_value_double(env, args[0], &x);
    if (argc >= 2) napi_get_value_double(env, args[1], &y);
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);
    MapLogicalPointToSurface(fx, fy);
    return MakeBool(env, TriggerBubbleBurst(PickBubble(fx, fy)));
}

static napi_value HasBubbleAtScreenApi(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    double x = 0.0, y = 0.0;
    if (argc >= 1) napi_get_value_double(env, args[0], &x);
    if (argc >= 2) napi_get_value_double(env, args[1], &y);
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);
    MapLogicalPointToSurface(fx, fy);
    return MakeBool(env, PickBubble(fx, fy) != 0);
}

static napi_value ResetSimulation(napi_env env, napi_callback_info info) {
    g_Sim.initIcosphere(g_BubbleRadius, g_SimSubdivs, g_SimPerturb);
    g_Sim.timeStep = 0.001f;
    g_Sim.substepsPerFrame = 1;
    g_SimPaused = false;
    g_VisualPaused = false;
    g_Sim.surfaceTensionStrength = 15.0f;
    g_Sim.circulationDiffusion = 0.0005f;
    g_Sim.flipSurfaceTensionSign = true;
    g_AiryThickness = 740.0f;
    g_KimLutThickness = 740.0f;
    g_ThicknessVar = 160.0f;
    g_RefractionStrength = 0.35f;
    g_EdgeDistortionBoost = 2.2f;
    g_EnvironmentReflectionStrength = 0.65f;
    g_CameraDistance = 4.0f;
    g_CameraOrbitTarget = glm::vec3(0.0f, 0.18f, 1.10f);
    ResetOpeningScene();
    return nullptr;
}

static napi_value ReleaseResource(napi_env env, napi_callback_info info) {
    ReleaseGraphicsResources();
    return nullptr;
}

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        { "initGraphics", nullptr, InitGraphics, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "renderFrame", nullptr, RenderFrame, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "getFps", nullptr, GetFps, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "onTouch", nullptr, OnTouch, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setInputViewport", nullptr, SetInputViewport, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "releaseResource", nullptr, ReleaseResource, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setIridescenceMode", nullptr, SetIridescenceMode, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setSurfaceTension", nullptr, SetSurfaceTension, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setRefractionStrength", nullptr, SetRefractionStrength, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "resetSimulation", nullptr, ResetSimulation, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "rotateCamera", nullptr, RotateCamera, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "resetCameraView", nullptr, ResetCameraViewApi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setFresnelPower", nullptr, SetFresnelPower, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setEdgeDistortionBoost", nullptr, SetEdgeDistortionBoost, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setMaxOffsetRatio", nullptr, SetMaxOffsetRatio, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setEnvironmentReflectionStrength", nullptr, SetEnvironmentReflectionStrength, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setCameraDistance", nullptr, SetCameraDistance, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "toggleSimulation", nullptr, ToggleSimulation, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setThickness", nullptr, SetThickness, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setThicknessVar", nullptr, SetThicknessVar, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "resetOpeningScene", nullptr, ResetOpeningSceneApi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "cycleInteractionDemo", nullptr, CycleInteractionDemoApi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setWindStrength", nullptr, SetWindStrengthApi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setWindDirection", nullptr, SetWindDirectionApi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "addBubbleAtScreen", nullptr, AddBubbleAtScreenApi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setAddPreview", nullptr, SetAddPreviewApi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "burstBubbleAtScreen", nullptr, BurstBubbleAtScreenApi, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "hasBubbleAtScreen", nullptr, HasBubbleAtScreenApi, nullptr, nullptr, nullptr, napi_default, nullptr },
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}
EXTERN_C_END

static napi_module demoModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "entry",
    .nm_priv = ((void*)0),
    .reserved = { 0 },
};

extern "C" __attribute__((constructor)) void RegisterEntryModule(void) {
    napi_module_register(&demoModule);
}
