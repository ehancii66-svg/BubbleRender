//
// BubbleRender — Windows desktop OpenGL entry point
// Ported from HarmonyOS napi_init.cpp
//

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
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
#include "simulation/vortex_sheet.h"
#include "shader/shader_refraction.h"
#include "shader/shader_background.h"
#include "shader/shader_skybox.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <iostream>
#include <unordered_map>
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
static int   g_SimSubdivs  = 3;       // icosphere subdivision level
static float g_SimPerturb  = 0.16f;   // matches HarmonyOS main-bubble stretch
static float g_SimTimeStep = 0.001f;  // Δt per substep
static int   g_SimSubsteps = 1;       // matches HarmonyOS DBSTT substep count
static bool  g_SimPaused   = false;
static constexpr int kSimFrameInterval = 6;
static int g_SimFrameCounter = 0;

// Helper: convert sim mesh data to Vertex vector for GPU upload
static std::vector<Vertex> buildVerticesFromSim(const VortexSheetSimulation& sim) {
    const auto& pos = sim.getPositions();
    const auto& nrm = sim.getNormals();
    std::vector<Vertex> verts(pos.size());
    for (size_t i = 0; i < pos.size(); ++i) {
        verts[i].Position   = pos[i];
        verts[i].Normal     = nrm[i];
        verts[i].TexCoords  = glm::vec2(0.0f);
        verts[i].Tangent    = glm::vec3(0.0f);
        verts[i].Bitangent  = glm::vec3(0.0f);
    }
    return verts;
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

struct DiscMeshData {
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<unsigned int> indices;
};

struct DisplayBubble {
    struct SurfaceControl {
        glm::vec3 localDir;
        float offset;
        float velocity;
    };

    int id;
    glm::vec3 basePosition;
    glm::vec3 position;
    glm::vec3 velocity;
    float radius;
    float initialRadius;
    float targetVolume;
    float phase;
    float windAmplitude;
    float floatAmplitude;
    float speed;
    float alpha;
    float contactTime;
    float mergeProgress;
    float filmThickness;
    float contactStrength;
    glm::vec3 contactAxis;
    bool volumeTransferred;
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
    } state;
    std::vector<SurfaceControl> surfaceControls;
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

    int a = -1;
    int b = -1;
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
    float filmThickness = 1.0f;
    float ruptureRisk = 0.0f;
    float restDistance = 0.0f;
    float targetVolume = 0.0f;
    State state = State::Free;
    bool active = false;
    bool bonded = false;
    bool candidate = false;
    bool persistentRenderPair = false;
    float candidateExitTime = 0.0f;
};

static std::vector<DisplayBubble> g_DisplayBubbles;
static std::vector<BubbleContactPair> g_ContactPairs;
static bool g_InteractionDemoActive = false;
static bool g_ShowMainBubble = true;
static Model *g_ContactFilmModel = nullptr;
static Model *g_ContactBubbleAModel = nullptr;
static Model *g_ContactBubbleBModel = nullptr;
static Model *g_FusedBubbleModel = nullptr;
static Model *g_NeckBridgeModel = nullptr;
static std::vector<Model *> g_DisplayBubbleModels;
static constexpr int kContactBubbleSegments = 72;
static constexpr int kContactBubbleRings = 28;
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
static constexpr float kShellCutDelay = 0.42f;

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
static float g_ThicknessVar = 160.0f; // 厚度扰动幅度 (nm)
static double g_Time = 0.0;           // 时间

static float &CurrentThickness()
{
    return (g_IridescenceMode == 2) ? g_AiryThickness : g_KimLutThickness;
}

static float Smooth01(float x)
{
    x = glm::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

static float BubbleVolume(float radius)
{
    static constexpr float kPi = 3.14159265358979323846f;
    return (4.0f / 3.0f) * kPi * radius * radius * radius;
}

static float RadiusFromVolume(float volume)
{
    static constexpr float kPi = 3.14159265358979323846f;
    return std::cbrt(std::max(volume, 0.0f) * 3.0f / (4.0f * kPi));
}

static float BubbleMass(const DisplayBubble& bubble)
{
    return std::max(BubbleVolume(std::max(bubble.radius, 0.001f)), 0.001f);
}

static float BubbleInvMass(const DisplayBubble& bubble)
{
    return 1.0f / BubbleMass(bubble);
}

static int FindContactPair(int a, int b)
{
    if (a > b) {
        std::swap(a, b);
    }

    for (size_t i = 0; i < g_ContactPairs.size(); ++i) {
        const auto& pair = g_ContactPairs[i];
        if (pair.a == a && pair.b == b) {
            return (int)i;
        }
    }
    return -1;
}

static int EnsureContactPair(int a, int b)
{
    if (a > b) {
        std::swap(a, b);
    }

    int existing = FindContactPair(a, b);
    if (existing >= 0) {
        return existing;
    }

    BubbleContactPair pair;
    pair.a = a;
    pair.b = b;
    pair.filmThickness = 1.0f;
    pair.state = BubbleContactPair::State::Free;
    pair.active = false;
    g_ContactPairs.push_back(pair);
    return (int)g_ContactPairs.size() - 1;
}

struct BubbleGridCell {
    int x;
    int y;
    int z;

    bool operator==(const BubbleGridCell& other) const
    {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct BubbleGridCellHash {
    size_t operator()(const BubbleGridCell& cell) const
    {
        size_t hx = std::hash<int>{}(cell.x);
        size_t hy = std::hash<int>{}(cell.y);
        size_t hz = std::hash<int>{}(cell.z);
        return hx ^ (hy << 1) ^ (hz << 7);
    }
};

static BubbleGridCell BubbleCellForPosition(const glm::vec3& position, float cellSize)
{
    return {
        (int)std::floor(position.x / cellSize),
        (int)std::floor(position.y / cellSize),
        (int)std::floor(position.z / cellSize)
    };
}

static std::vector<std::pair<int, int>> BuildBubbleBroadPhasePairs()
{
    std::vector<std::pair<int, int>> pairs;
    if (g_DisplayBubbles.size() < 2) {
        return pairs;
    }

    float maximumRadius = 0.001f;
    for (const auto& bubble : g_DisplayBubbles) {
        if (bubble.state != DisplayBubble::State::Dead) {
            maximumRadius = std::max(maximumRadius, bubble.radius);
        }
    }
    float cellSize = maximumRadius * 2.40f;

    std::unordered_map<BubbleGridCell, std::vector<int>, BubbleGridCellHash> grid;
    grid.reserve(g_DisplayBubbles.size() * 2);
    for (size_t i = 0; i < g_DisplayBubbles.size(); ++i) {
        const auto& bubble = g_DisplayBubbles[i];
        if (bubble.state == DisplayBubble::State::Dead) {
            continue;
        }
        grid[BubbleCellForPosition(bubble.position, cellSize)].push_back((int)i);
    }

    pairs.reserve(g_DisplayBubbles.size() * 4);
    for (size_t i = 0; i < g_DisplayBubbles.size(); ++i) {
        const auto& bubble = g_DisplayBubbles[i];
        if (bubble.state == DisplayBubble::State::Dead) {
            continue;
        }
        BubbleGridCell centerCell = BubbleCellForPosition(bubble.position, cellSize);
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    BubbleGridCell neighbor{
                        centerCell.x + dx,
                        centerCell.y + dy,
                        centerCell.z + dz
                    };
                    auto found = grid.find(neighbor);
                    if (found == grid.end()) {
                        continue;
                    }
                    for (int j : found->second) {
                        if (j > (int)i) {
                            pairs.emplace_back((int)i, j);
                        }
                    }
                }
            }
        }
    }
    return pairs;
}

static int BubbleActiveContactCount(int bubbleIndex)
{
    int count = 0;
    for (const BubbleContactPair& pair : g_ContactPairs) {
        if ((pair.candidate || pair.bonded) &&
            (pair.a == bubbleIndex || pair.b == bubbleIndex)) {
            ++count;
        }
    }
    return std::max(count, 1);
}

static std::vector<DisplayBubble::SurfaceControl> MakeSurfaceControls(float phase)
{
    std::vector<DisplayBubble::SurfaceControl> controls;
    controls.reserve(14);

    const glm::vec3 dirs[] = {
        { 1.0f,  0.0f,  0.0f}, {-1.0f,  0.0f,  0.0f},
        { 0.0f,  1.0f,  0.0f}, { 0.0f, -1.0f,  0.0f},
        { 0.0f,  0.0f,  1.0f}, { 0.0f,  0.0f, -1.0f},
        { 1.0f,  1.0f,  0.0f}, {-1.0f,  1.0f,  0.0f},
        { 1.0f, -1.0f,  0.0f}, {-1.0f, -1.0f,  0.0f},
        { 1.0f,  0.0f,  1.0f}, {-1.0f,  0.0f,  1.0f},
        { 0.0f,  1.0f, -1.0f}, { 0.0f, -1.0f, -1.0f}
    };

    for (int i = 0; i < 14; ++i) {
        DisplayBubble::SurfaceControl control{};
        control.localDir = glm::normalize(dirs[i]);
        control.offset = std::sin(phase + (float)i * 1.37f) * 0.006f;
        control.velocity = 0.0f;
        controls.push_back(control);
    }

    return controls;
}

static void UpdateSurfaceControls(DisplayBubble& bubble, const glm::vec3& contactAxis, float contactStrength, float dt)
{
    for (auto& control : bubble.surfaceControls) {
        float directionalMask = std::max(glm::dot(control.localDir, contactAxis), 0.0f);
        float target = directionalMask * contactStrength * -0.055f;
        float acceleration = (target - control.offset) * 18.0f - control.velocity * 5.2f;
        control.velocity += acceleration * dt;
        control.offset += control.velocity * dt;
        control.offset = glm::clamp(control.offset, -0.075f, 0.035f);
    }
}

static DiscMeshData BuildContactFilmDisc(int segments = 80, int rings = 10)
{
    DiscMeshData disc;
    const float curvature = 0.06f;
    const size_t vertexCount = 1 + (size_t)segments * (size_t)rings;
    disc.positions.reserve(vertexCount);
    disc.normals.reserve(vertexCount);
    disc.indices.reserve((size_t)segments * (1 + (rings - 1) * 2) * 3);

    disc.positions.push_back(glm::vec3(0.0f, 0.0f, curvature));
    disc.normals.push_back(glm::normalize(glm::vec3(0.0f, 0.0f, 1.0f)));

    for (int ring = 1; ring <= rings; ++ring) {
        float r = (float)ring / (float)rings;
        float z = curvature * (1.0f - r * r);
        for (int i = 0; i < segments; ++i) {
            float t = 2.0f * 3.14159265358979323846f * (float)i / (float)segments;
            float x = std::cos(t) * r;
            float y = std::sin(t) * r;
            disc.positions.push_back(glm::vec3(x, y, z));
            disc.normals.push_back(glm::normalize(glm::vec3(2.0f * curvature * x, 2.0f * curvature * y, 1.0f)));
        }
    }

    for (int i = 0; i < segments; ++i) {
        unsigned int curr = (unsigned int)i + 1;
        unsigned int next = (unsigned int)((i + 1) % segments) + 1;
        disc.indices.push_back(0);
        disc.indices.push_back(curr);
        disc.indices.push_back(next);
    }

    for (int ring = 2; ring <= rings; ++ring) {
        unsigned int innerStart = 1u + (unsigned int)(ring - 2) * (unsigned int)segments;
        unsigned int outerStart = 1u + (unsigned int)(ring - 1) * (unsigned int)segments;
        for (int i = 0; i < segments; ++i) {
            unsigned int inner0 = innerStart + (unsigned int)i;
            unsigned int inner1 = innerStart + (unsigned int)((i + 1) % segments);
            unsigned int outer0 = outerStart + (unsigned int)i;
            unsigned int outer1 = outerStart + (unsigned int)((i + 1) % segments);

            disc.indices.push_back(inner0);
            disc.indices.push_back(outer0);
            disc.indices.push_back(inner1);

            disc.indices.push_back(inner1);
            disc.indices.push_back(outer0);
            disc.indices.push_back(outer1);
        }
    }

    return disc;
}

static std::vector<unsigned int> BuildContactBubblePatchIndices(int segments = kContactBubbleSegments,
                                                                int rings = kContactBubbleRings)
{
    std::vector<unsigned int> indices;
    indices.reserve((size_t)rings * (size_t)segments * 6);
    for (int ring = 0; ring < rings; ++ring) {
        int row0 = ring * (segments + 1);
        int row1 = (ring + 1) * (segments + 1);
        for (int i = 0; i < segments; ++i) {
            unsigned int a = (unsigned int)(row0 + i);
            unsigned int b = (unsigned int)(row1 + i);
            unsigned int c = (unsigned int)(row0 + i + 1);
            unsigned int d = (unsigned int)(row1 + i + 1);
            indices.push_back(a);
            indices.push_back(b);
            indices.push_back(c);
            indices.push_back(c);
            indices.push_back(b);
            indices.push_back(d);
        }
    }
    return indices;
}

static std::vector<Vertex> BuildBubbleShellVertices(float radius,
                                                    float contactRadius,
                                                    bool hasContact,
                                                    int segments = kContactBubbleSegments,
                                                    int rings = kContactBubbleRings)
{
    std::vector<Vertex> vertices;
    vertices.reserve((size_t)(rings + 1) * (size_t)(segments + 1));

    contactRadius = hasContact ? glm::clamp(contactRadius, 0.0f, radius * 0.92f) : 0.0f;
    float capPlaneOffset = std::sqrt(std::max(radius * radius - contactRadius * contactRadius, 0.0f));
    float capCos = hasContact
        ? glm::clamp(capPlaneOffset / std::max(radius, 0.001f), -0.999f, 0.999f)
        : 1.0f;
    float thetaCap = hasContact ? std::acos(capCos) : 0.0f;

    for (int ring = 0; ring <= rings; ++ring) {
        float v = (float)ring / (float)rings;
        float theta = 3.14159265358979323846f + (thetaCap - 3.14159265358979323846f) * Smooth01(v);
        float x = std::cos(theta);
        float yz = std::sin(theta);
        for (int i = 0; i <= segments; ++i) {
            float u = (float)i / (float)segments;
            float phi = 2.0f * 3.14159265358979323846f * u;
            glm::vec3 normal = glm::normalize(glm::vec3(x, yz * std::cos(phi), yz * std::sin(phi)));

            Vertex vertex{};
            vertex.Position = normal * radius;
            vertex.Normal = normal;
            vertex.TexCoords = glm::vec2(u, v);
            vertex.Tangent = glm::vec3(0.0f);
            vertex.Bitangent = glm::vec3(0.0f);
            vertices.push_back(vertex);
        }
    }

    return vertices;
}

static std::vector<Vertex> BuildContactBubblePatchVertices(float radius,
                                                           float capPlaneOffset,
                                                           int segments = kContactBubbleSegments,
                                                           int rings = kContactBubbleRings)
{
    float contactRadius = std::sqrt(std::max(radius * radius - capPlaneOffset * capPlaneOffset, 0.0f));
    return BuildBubbleShellVertices(radius, contactRadius, true, segments, rings);
}

static float PlateauContactRadius(const BubbleContactPair& pair, const DisplayBubble& a, const DisplayBubble& b)
{
    float stableRadius = std::min(a.radius, b.radius) * 0.44f;
    float formingRadius = stableRadius * 0.08f;
    float relaxedRadius = glm::mix(formingRadius, stableRadius, Smooth01(pair.geometryBlend));
    float breathing = 1.0f + std::sin((float)g_Time * 1.15f + (float)(pair.a + pair.b) * 0.71f) * 0.025f * pair.geometryBlend;
    return relaxedRadius * breathing;
}

static std::vector<Vertex> BuildPlateauLobeVertices(float contactRadius,
                                                    bool leftLobe,
                                                    int segments = kContactBubbleSegments,
                                                    int rings = kContactBubbleRings)
{
    std::vector<Vertex> vertices;
    vertices.reserve((size_t)(rings + 1) * (size_t)(segments + 1));

    static constexpr float kPi = 3.14159265358979323846f;
    float sphereRadius = 2.0f * contactRadius / std::sqrt(3.0f);
    float centerX = (leftLobe ? -1.0f : 1.0f) * contactRadius / std::sqrt(3.0f);
    float theta0 = leftLobe ? kPi : 0.0f;
    float theta1 = leftLobe ? kPi / 3.0f : 2.0f * kPi / 3.0f;

    for (int ring = 0; ring <= rings; ++ring) {
        float t = (float)ring / (float)rings;
        float theta = glm::mix(theta0, theta1, t);
        float x = centerX + sphereRadius * std::cos(theta);
        float radial = sphereRadius * std::sin(theta);

        for (int i = 0; i <= segments; ++i) {
            float u = (float)i / (float)segments;
            float phi = 2.0f * kPi * u;
            glm::vec3 normal = glm::normalize(glm::vec3(
                std::cos(theta),
                std::sin(theta) * std::cos(phi),
                std::sin(theta) * std::sin(phi)));

            Vertex vertex{};
            vertex.Position = glm::vec3(x, radial * std::cos(phi), radial * std::sin(phi));
            vertex.Normal = normal;
            vertex.TexCoords = glm::vec2(u, t);
            vertex.Tangent = glm::vec3(0.0f);
            vertex.Bitangent = glm::vec3(0.0f);
            vertices.push_back(vertex);
        }
    }

    return vertices;
}

static float SmoothMax(float a, float b, float k)
{
    if (k <= 1e-5f) {
        return std::max(a, b);
    }
    float h = glm::clamp(0.5f + 0.5f * (b - a) / k, 0.0f, 1.0f);
    return glm::mix(b, a, h) + k * h * (1.0f - h);
}

static constexpr int kDoubleBubbleFilmRings = 8;

struct DoubleBubbleGeometry {
    float centerDistance = 2.0f;
    float radiusA = 1.0f;
    float radiusB = 1.0f;
    float flattenA = 0.0f;
    float flattenB = 0.0f;
    float contactRadius = 0.0f;
    float junctionBlend = 0.0f;
};

static void AppendRingPatchIndices(std::vector<unsigned int>& indices,
                                   int start,
                                   int segments,
                                   int rings,
                                   bool reverseWinding)
{
    for (int ring = 0; ring < rings; ++ring) {
        int row0 = start + ring * (segments + 1);
        int row1 = start + (ring + 1) * (segments + 1);
        for (int i = 0; i < segments; ++i) {
            unsigned int a = (unsigned int)(row0 + i);
            unsigned int b = (unsigned int)(row1 + i);
            unsigned int c = (unsigned int)(row0 + i + 1);
            unsigned int d = (unsigned int)(row1 + i + 1);
            if (reverseWinding) {
                indices.insert(indices.end(), {a, c, b, c, d, b});
            } else {
                indices.insert(indices.end(), {a, b, c, c, b, d});
            }
        }
    }
}

static std::vector<unsigned int> BuildFusedBubbleIndices(int segments = kContactBubbleSegments,
                                                        int rings = kContactBubbleRings)
{
    std::vector<unsigned int> indices;
    int capVertexCount = (rings + 1) * (segments + 1);
    int filmVertexCount = (kDoubleBubbleFilmRings + 1) * (segments + 1);
    indices.reserve((size_t)(rings * segments * 12 + kDoubleBubbleFilmRings * segments * 12));

    AppendRingPatchIndices(indices, 0, segments, rings, false);
    AppendRingPatchIndices(indices, capVertexCount, segments, rings, true);
    AppendRingPatchIndices(indices, capVertexCount * 2, segments, kDoubleBubbleFilmRings, false);
    AppendRingPatchIndices(indices, capVertexCount * 2 + filmVertexCount, segments, kDoubleBubbleFilmRings, true);
    return indices;
}

static void AppendAssemblyLobeVertices(std::vector<Vertex>& vertices,
                                       float centerX,
                                       float radius,
                                       float flatten,
                                       float contactRadius,
                                       float filmPlaneX,
                                       float junctionBlend,
                                       bool leftLobe,
                                       int segments,
                                       int rings)
{
    static constexpr float kPi = 3.14159265358979323846f;
    flatten = glm::clamp(flatten, 0.0f, 0.18f);
    float axialRadius = radius * (1.0f - flatten);
    float radialRadius = radius / std::sqrt(std::max(1.0f - flatten, 0.001f));
    contactRadius = glm::clamp(contactRadius, 0.0f, radialRadius * 0.94f);

    float thetaStart = leftLobe ? kPi : 0.0f;
    float thetaEnd = leftLobe ? 0.0f : kPi;
    float boundaryShift = 0.0f;
    if (contactRadius > 1e-6f) {
        float contactAngle = std::asin(glm::clamp(contactRadius / radialRadius, 0.0f, 0.94f));
        thetaEnd = leftLobe ? contactAngle : (kPi - contactAngle);
        float rawBoundaryX = centerX + axialRadius * std::cos(thetaEnd);
        boundaryShift = (filmPlaneX - rawBoundaryX) * glm::clamp(junctionBlend, 0.0f, 1.0f);
    }

    for (int ring = 0; ring <= rings; ++ring) {
        float t = (float)ring / (float)rings;
        float smoothT = Smooth01(t);
        float theta = glm::mix(thetaStart, thetaEnd, smoothT);
        float cpTheta = std::cos(theta);
        float spTheta = std::sin(theta);
        float x = centerX + axialRadius * cpTheta + boundaryShift * smoothT;
        float radial = radialRadius * spTheta;

        float dSmoothDt = 6.0f * t * (1.0f - t);
        float dThetaDt = (thetaEnd - thetaStart) * dSmoothDt;
        float dxDt = -axialRadius * spTheta * dThetaDt + boundaryShift * dSmoothDt;
        float drDt = radialRadius * cpTheta * dThetaDt;
        float orientation = leftLobe ? -1.0f : 1.0f;

        for (int i = 0; i <= segments; ++i) {
            float u = (float)i / (float)segments;
            float phi = 2.0f * kPi * u;
            float cp = std::cos(phi);
            float sp = std::sin(phi);
            glm::vec3 normal;
            if (std::abs(dxDt) + std::abs(drDt) > 1e-6f) {
                normal = glm::normalize(glm::vec3(
                    orientation * std::abs(drDt),
                    orientation * -dxDt * cp,
                    orientation * -dxDt * sp));
            } else {
                normal = glm::normalize(glm::vec3(
                    cpTheta / std::max(axialRadius, 0.001f),
                    spTheta * cp / std::max(radialRadius, 0.001f),
                    spTheta * sp / std::max(radialRadius, 0.001f)));
            }

            Vertex vertex{};
            vertex.Position = glm::vec3(x, radial * cp, radial * sp);
            vertex.Normal = normal;
            vertex.TexCoords = glm::vec2(u, t);
            vertex.Tangent = glm::vec3(0.0f);
            vertex.Bitangent = glm::vec3(0.0f);
            vertices.push_back(vertex);
        }
    }
}

static void AppendPlateauLobeVertices(std::vector<Vertex>& vertices,
                                      float contactRadius,
                                      bool leftLobe,
                                      int segments,
                                      int rings)
{
    static constexpr float kPi = 3.14159265358979323846f;
    float capSphereRadius = 2.0f * contactRadius / std::sqrt(3.0f);
    float centerX = (leftLobe ? -1.0f : 1.0f) * contactRadius / std::sqrt(3.0f);
    float theta0 = leftLobe ? kPi : 0.0f;
    float theta1 = leftLobe ? kPi / 3.0f : 2.0f * kPi / 3.0f;

    for (int ring = 0; ring <= rings; ++ring) {
        float t = (float)ring / (float)rings;
        float theta = glm::mix(theta0, theta1, Smooth01(t));
        float cpTheta = std::cos(theta);
        float spTheta = std::sin(theta);
        float x = centerX + capSphereRadius * cpTheta;
        float radial = capSphereRadius * spTheta;

        for (int i = 0; i <= segments; ++i) {
            float u = (float)i / (float)segments;
            float phi = 2.0f * kPi * u;
            float cp = std::cos(phi);
            float sp = std::sin(phi);
            glm::vec3 normal = glm::normalize(glm::vec3(cpTheta, spTheta * cp, spTheta * sp));

            Vertex vertex{};
            vertex.Position = glm::vec3(x, radial * cp, radial * sp);
            vertex.Normal = normal;
            vertex.TexCoords = glm::vec2(u, t * 0.82f);
            vertex.Tangent = glm::vec3(0.0f);
            vertex.Bitangent = glm::vec3(0.0f);
            vertices.push_back(vertex);
        }
    }
}

static void AppendSharedFilmVertices(std::vector<Vertex>& vertices,
                                     float filmPlaneX,
                                     float contactRadius,
                                     float normalSign,
                                     int segments)
{
    static constexpr float kPi = 3.14159265358979323846f;
    for (int ring = 0; ring <= kDoubleBubbleFilmRings; ++ring) {
        float t = (float)ring / (float)kDoubleBubbleFilmRings;
        float radial = contactRadius * Smooth01(t);
        for (int i = 0; i <= segments; ++i) {
            float u = (float)i / (float)segments;
            float phi = 2.0f * kPi * u;

            Vertex vertex{};
            vertex.Position = glm::vec3(filmPlaneX, radial * std::cos(phi), radial * std::sin(phi));
            vertex.Normal = glm::vec3(normalSign, 0.0f, 0.0f);
            vertex.TexCoords = glm::vec2(-1.0f, t);
            vertex.Tangent = glm::vec3(0.0f);
            vertex.Bitangent = glm::vec3(0.0f);
            vertices.push_back(vertex);
        }
    }
}

static std::vector<Vertex> BuildFusedBubbleVertices(const DoubleBubbleGeometry& geometry,
                                                    int segments = kContactBubbleSegments,
                                                    int rings = kContactBubbleRings)
{
    std::vector<Vertex> vertices;
    int capVertexCount = (rings + 1) * (segments + 1);
    int filmVertexCount = (kDoubleBubbleFilmRings + 1) * (segments + 1);
    vertices.reserve((size_t)(capVertexCount * 2 + filmVertexCount * 2));

    float halfDistance = geometry.centerDistance * 0.5f;
    float centerA = -halfDistance;
    float centerB = halfDistance;
    float flattenA = glm::clamp(geometry.flattenA, 0.0f, 0.18f);
    float flattenB = glm::clamp(geometry.flattenB, 0.0f, 0.18f);
    float radialA = geometry.radiusA / std::sqrt(std::max(1.0f - flattenA, 0.001f));
    float radialB = geometry.radiusB / std::sqrt(std::max(1.0f - flattenB, 0.001f));
    float contactRadius = glm::clamp(geometry.contactRadius, 0.0f,
                                     std::min(radialA, radialB) * 0.94f);

    float filmPlaneX = 0.0f;
    if (contactRadius > 1e-6f) {
        float axialA = geometry.radiusA * (1.0f - flattenA);
        float axialB = geometry.radiusB * (1.0f - flattenB);
        float angleA = std::asin(glm::clamp(contactRadius / radialA, 0.0f, 0.94f));
        float angleB = std::asin(glm::clamp(contactRadius / radialB, 0.0f, 0.94f));
        float boundaryA = centerA + axialA * std::cos(angleA);
        float boundaryB = centerB - axialB * std::cos(angleB);
        filmPlaneX = 0.5f * (boundaryA + boundaryB);
    }

    AppendAssemblyLobeVertices(vertices, centerA, geometry.radiusA, flattenA,
                               contactRadius, filmPlaneX, geometry.junctionBlend, true, segments, rings);
    AppendAssemblyLobeVertices(vertices, centerB, geometry.radiusB, flattenB,
                               contactRadius, filmPlaneX, geometry.junctionBlend, false, segments, rings);
    AppendSharedFilmVertices(vertices, filmPlaneX, contactRadius, 1.0f, segments);
    AppendSharedFilmVertices(vertices, filmPlaneX, contactRadius, -1.0f, segments);
    return vertices;
}

static std::vector<Vertex> BuildFusedBubbleVertices(float radius,
                                                    float blend,
                                                    int segments = kContactBubbleSegments,
                                                    int rings = kContactBubbleRings)
{
    float progress = Smooth01(blend);
    DoubleBubbleGeometry geometry;
    geometry.radiusA = radius;
    geometry.radiusB = radius;
    geometry.contactRadius = radius * 0.44f * progress;
    geometry.centerDistance = 2.0f * std::sqrt(std::max(
        radius * radius - geometry.contactRadius * geometry.contactRadius, 0.0f));
    geometry.flattenA = 0.025f * progress;
    geometry.flattenB = geometry.flattenA;
    geometry.junctionBlend = progress;
    return BuildFusedBubbleVertices(geometry, segments, rings);
}

static void BuildFusedBubbleMesh(float radius,
                                 std::vector<glm::vec3>& positions,
                                 std::vector<glm::vec3>& normals,
                                 std::vector<unsigned int>& indices)
{
    std::vector<Vertex> vertices = BuildFusedBubbleVertices(radius, 0.0f);
    positions.resize(vertices.size());
    normals.resize(vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i) {
        positions[i] = vertices[i].Position;
        normals[i] = vertices[i].Normal;
    }
    indices = BuildFusedBubbleIndices();
}

static std::vector<unsigned int> BuildNeckBridgeIndices(int segments = 48, int rings = 14)
{
    return BuildContactBubblePatchIndices(segments, rings);
}

static std::vector<Vertex> BuildNeckBridgeVertices(float halfLength,
                                                   float endRadius,
                                                   float waistRadius,
                                                   float blend,
                                                   int segments = 48,
                                                   int rings = 14)
{
    std::vector<Vertex> vertices;
    vertices.reserve((size_t)(rings + 1) * (size_t)(segments + 1));

    static constexpr float kPi = 3.14159265358979323846f;
    halfLength = std::max(halfLength, 0.02f);
    endRadius = std::max(endRadius, 0.01f);
    waistRadius = std::max(waistRadius, 0.006f);

    for (int ring = 0; ring <= rings; ++ring) {
        float u = (float)ring / (float)rings;
        float x = glm::mix(-halfLength, halfLength, u);
        float centered = std::abs(u * 2.0f - 1.0f);
        float smoothEnd = Smooth01(centered);
        float radius = glm::mix(waistRadius, endRadius, smoothEnd);
        radius *= 1.0f + std::sin((float)g_Time * 1.25f + u * 6.28318f) * 0.025f * blend;

        float drdu = (endRadius - waistRadius) * 6.0f * centered * (1.0f - centered);
        float drdx = drdu / std::max(2.0f * halfLength, 1e-4f);
        if (u < 0.5f) {
            drdx = -drdx;
        }

        for (int i = 0; i <= segments; ++i) {
            float v = (float)i / (float)segments;
            float phi = 2.0f * kPi * v;
            float cp = std::cos(phi);
            float sp = std::sin(phi);

            Vertex vertex{};
            vertex.Position = glm::vec3(x, radius * cp, radius * sp);
            vertex.Normal = glm::normalize(glm::vec3(-drdx, cp, sp));
            vertex.TexCoords = glm::vec2(v, u);
            vertex.Tangent = glm::vec3(0.0f);
            vertex.Bitangent = glm::vec3(0.0f);
            vertices.push_back(vertex);
        }
    }

    return vertices;
}

static void BuildNeckBridgeMesh(std::vector<glm::vec3>& positions,
                                std::vector<glm::vec3>& normals,
                                std::vector<unsigned int>& indices)
{
    std::vector<Vertex> vertices = BuildNeckBridgeVertices(0.1f, 0.08f, 0.035f, 0.0f);
    positions.resize(vertices.size());
    normals.resize(vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i) {
        positions[i] = vertices[i].Position;
        normals[i] = vertices[i].Normal;
    }
    indices = BuildNeckBridgeIndices();
}

static void BuildContactBubblePatchMesh(float radius,
                                        float capPlaneOffset,
                                        std::vector<glm::vec3>& positions,
                                        std::vector<glm::vec3>& normals,
                                        std::vector<unsigned int>& indices)
{
    std::vector<Vertex> vertices = BuildContactBubblePatchVertices(radius, capPlaneOffset);
    positions.resize(vertices.size());
    normals.resize(vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i) {
        positions[i] = vertices[i].Position;
        normals[i] = vertices[i].Normal;
    }
    indices = BuildContactBubblePatchIndices();
}

static void RebuildDisplayBubbleModels()
{
    for (Model* model : g_DisplayBubbleModels) {
        delete model;
    }
    g_DisplayBubbleModels.clear();

    std::vector<Vertex> vertices = BuildBubbleShellVertices(1.0f, 0.0f, false);
    std::vector<glm::vec3> positions(vertices.size());
    std::vector<glm::vec3> normals(vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i) {
        positions[i] = vertices[i].Position;
        normals[i] = vertices[i].Normal;
    }
    std::vector<unsigned int> indices = BuildContactBubblePatchIndices();

    g_DisplayBubbleModels.reserve(g_DisplayBubbles.size());
    for (size_t i = 0; i < g_DisplayBubbles.size(); ++i) {
        g_DisplayBubbleModels.push_back(
            Model::CreateFromVertices(positions, normals, indices));
    }
}

static glm::mat4 AxisBasisModel(const glm::vec3& center, const glm::vec3& axisToPartner)
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

static void ResetDisplayBubbles()
{
    struct BubbleSeed {
        glm::vec3 pos;
        float radius;
        float phase;
        float wind;
        float lift;
        float speed;
        glm::vec3 velocity;
    };

    const std::vector<BubbleSeed> seeds = {
        {{-1.12f, -0.18f, 1.08f}, 0.48f, 0.2f, 0.010f, 0.009f, 0.55f, { 0.145f,  0.020f,  0.000f}},
        {{ 1.10f, -0.16f, 1.12f}, 0.44f, 1.7f, 0.010f, 0.009f, 0.58f, {-0.140f,  0.018f,  0.000f}},
        {{ 0.02f,  1.22f, 1.08f}, 0.40f, 2.8f, 0.012f, 0.010f, 0.66f, {-0.004f, -0.130f,  0.004f}}
    };

    g_DisplayBubbles.clear();
    g_DisplayBubbles.reserve(seeds.size());
    for (size_t i = 0; i < seeds.size(); ++i) {
        const auto& seed = seeds[i];
        DisplayBubble bubble{};
        bubble.id = (int)i;
        bubble.basePosition = seed.pos;
        bubble.position = seed.pos;
        bubble.velocity = seed.velocity;
        bubble.radius = seed.radius;
        bubble.initialRadius = seed.radius;
        bubble.targetVolume = BubbleVolume(seed.radius);
        bubble.phase = seed.phase;
        bubble.windAmplitude = seed.wind;
        bubble.floatAmplitude = seed.lift;
        bubble.speed = seed.speed;
        bubble.alpha = 1.0f;
        bubble.contactTime = 0.0f;
        bubble.mergeProgress = 0.0f;
        bubble.filmThickness = 1.0f;
        bubble.contactStrength = 0.0f;
        bubble.contactAxis = glm::vec3(1.0f, 0.0f, 0.0f);
        bubble.volumeTransferred = false;
        bubble.state = DisplayBubble::State::Free;
        bubble.surfaceControls = MakeSurfaceControls(seed.phase);
        g_DisplayBubbles.push_back(bubble);
    }

    const glm::vec3 clusteredHomes[] = {
        {-0.38f, -0.12f, 1.09f},
        { 0.36f, -0.10f, 1.11f},
        { 0.00f,  0.40f, 1.09f}
    };
    for (size_t i = 0; i < g_DisplayBubbles.size(); ++i) {
        g_DisplayBubbles[i].basePosition = clusteredHomes[i];
    }

    g_MainBubbleVisualRadius = g_BubbleRadius;
    g_MainBubbleTargetRadius = g_BubbleRadius;
    g_ContactPairs.clear();
    g_InteractionDemoActive = false;
    g_ShowMainBubble = false;
    g_BridgeStrength = 0.0f;
    g_AutoTouchPoint = glm::vec2(-10.0f, -10.0f);
    g_AutoTouchStrength = 0.0f;
    g_AutoTouchVelocity = 0.0f;
    if (g_DecorativeBubbleModel) {
        RebuildDisplayBubbleModels();
    }
}

static void StartBubbleInteractionDemo()
{
    if (g_DisplayBubbles.size() < 2) {
        return;
    }

    g_ContactPairs.clear();
    DisplayBubble& a = g_DisplayBubbles[0];
    DisplayBubble& b = g_DisplayBubbles[1];
    glm::vec3 center(0.0f, 0.0f, 1.10f);
    glm::vec3 axis = glm::normalize(glm::vec3(1.0f, 0.04f, 0.0f));
    float radiusA = 0.56f;
    float radiusB = 0.56f;
    float finalFilmRadius = std::min(radiusA, radiusB) * 0.30f;
    float restDistance = std::sqrt(radiusA * radiusA - finalFilmRadius * finalFilmRadius)
        + std::sqrt(radiusB * radiusB - finalFilmRadius * finalFilmRadius);
    float centerDistance = radiusA + radiusB + std::min(radiusA, radiusB) * 0.065f;

    a.radius = radiusA;
    a.initialRadius = radiusA;
    a.targetVolume = BubbleVolume(radiusA);
    a.alpha = 1.0f;
    a.contactTime = 0.0f;
    a.mergeProgress = 0.0f;
    a.filmThickness = 1.0f;
    a.contactStrength = 0.0f;
    a.contactAxis = axis;
    a.volumeTransferred = false;
    a.state = DisplayBubble::State::Free;
    a.surfaceControls = MakeSurfaceControls(a.phase);
    a.position = center - axis * (centerDistance * 0.5f);
    a.basePosition = center - axis * (restDistance * 0.5f);
    a.velocity = axis * 0.085f;

    b.radius = radiusB;
    b.initialRadius = radiusB;
    b.targetVolume = BubbleVolume(radiusB);
    b.alpha = 1.0f;
    b.contactTime = 0.0f;
    b.mergeProgress = 0.0f;
    b.filmThickness = 1.0f;
    b.contactStrength = 0.0f;
    b.contactAxis = -axis;
    b.volumeTransferred = false;
    b.state = DisplayBubble::State::Free;
    b.surfaceControls = MakeSurfaceControls(b.phase);
    b.position = center + axis * (centerDistance * 0.5f);
    b.basePosition = center + axis * (restDistance * 0.5f);
    b.velocity = -axis * 0.085f;

    for (size_t i = 2; i < g_DisplayBubbles.size(); ++i) {
        g_DisplayBubbles[i].alpha = 0.0f;
        g_DisplayBubbles[i].state = DisplayBubble::State::Dead;
    }

    BubbleContactPair pair;
    pair.a = 0;
    pair.b = 1;
    pair.filmThickness = 1.0f;
    pair.geometryBlend = 0.0f;
    pair.restDistance = restDistance;
    pair.neckRadius = std::min(radiusA, radiusB) * 0.06f;
    pair.ruptureRisk = 0.0f;
    pair.targetVolume = a.targetVolume + b.targetVolume;
    pair.state = BubbleContactPair::State::Free;
    pair.active = true;
    pair.persistentRenderPair = true;
    g_ContactPairs.push_back(pair);

    g_InteractionDemoActive = true;
    g_ShowMainBubble = false;
    std::cout << "[BubbleInteraction] replay: two bubbles form a shared contact film" << std::endl;
}

static glm::vec3 BubbleVisualCenter(const DisplayBubble& bubble, float time)
{
    glm::vec3 windDir = glm::normalize(glm::vec3(0.75f, 0.22f, 0.0f));
    float motionScale = g_InteractionDemoActive ? 0.18f : 1.0f;
    float wind = sinf(time * bubble.speed + bubble.phase) * bubble.windAmplitude * motionScale;
    float lift = sinf(time * (bubble.speed * 0.73f) + bubble.phase * 1.7f) * bubble.floatAmplitude * motionScale;
    float depth = cosf(time * (bubble.speed * 0.51f) + bubble.phase * 0.9f) * bubble.windAmplitude * 0.22f * motionScale;
    return bubble.position + windDir * wind + glm::vec3(0.0f, lift, depth);
}

static glm::mat4 MakeContactFilmModel(const DisplayBubble& a, const DisplayBubble& b,
                                      const BubbleContactPair& pair, float time)
{
    glm::vec3 pa = BubbleVisualCenter(a, time);
    glm::vec3 pb = BubbleVisualCenter(b, time);
    glm::vec3 axis = pb - pa;
    float length = glm::length(axis);
    if (length < 1e-4f) {
        return glm::mat4(1.0f);
    }

    axis /= length;
    if (pair.contactFrameInitialized) {
        axis = pair.filteredNormal;
    }
    glm::vec3 up = std::abs(axis.y) < 0.92f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 side = glm::normalize(glm::cross(up, axis));
    glm::vec3 binormal = glm::cross(axis, side);

    float pulse = 1.0f + sinf(time * 0.70f + (float)(pair.a * 1.7f + pair.b)) * 0.008f;
    float filmRadius = std::max(0.002f, pair.contactRadius * pulse);
    float filmSag = filmRadius * 0.012f;
    glm::vec3 filmCenter = pair.contactFrameInitialized
        ? pair.filteredPlaneCenter
        : (pa + pb) * 0.5f;

    glm::mat4 basis(1.0f);
    basis[0] = glm::vec4(side, 0.0f);
    basis[1] = glm::vec4(binormal, 0.0f);
    basis[2] = glm::vec4(axis, 0.0f);
    basis[3] = glm::vec4(filmCenter, 1.0f);
    return basis * glm::scale(glm::mat4(1.0f), glm::vec3(filmRadius, filmRadius, filmSag));
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
    for (const auto& control : bubble.surfaceControls) {
        float worldMask = std::max(glm::dot(control.localDir, bubble.contactAxis), 0.0f);
        scale += bubble.radius * control.offset * worldMask * 0.20f;
    }
    glm::mat4 model = glm::translate(glm::mat4(1.0f), pos);
    if (bubble.contactStrength > 0.01f) {
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
    } else {
        model = glm::scale(model, glm::vec3(scale));
    }
    return model;
}

static glm::mat4 PersistentBubbleModelMatrix(const DisplayBubble& bubble, float time)
{
    (void)time;
    glm::mat4 model = glm::translate(glm::mat4(1.0f), bubble.position);
    return glm::scale(model, glm::vec3(std::max(bubble.radius, 0.001f)));
}

static std::vector<Vertex> BuildPersistentBubbleVertices(int bubbleIndex, float time)
{
    std::vector<Vertex> vertices = BuildBubbleShellVertices(1.0f, 0.0f, false);
    if (bubbleIndex < 0 || bubbleIndex >= (int)g_DisplayBubbles.size()) {
        return vertices;
    }

    const DisplayBubble& bubble = g_DisplayBubbles[(size_t)bubbleIndex];
    glm::vec3 motionAxis = bubble.velocity;
    if (glm::length(motionAxis) < 1e-4f) {
        motionAxis = glm::vec3(0.78f, 0.26f, 0.12f);
    }
    motionAxis = glm::normalize(motionAxis);
    float speedStretch = glm::clamp(glm::length(bubble.velocity) * 0.055f, 0.0f, 0.045f);
    float freeOscillation = std::sin(time * (0.72f + bubble.speed * 0.18f) + bubble.phase) * 0.012f;
    float axialScale = 1.0f + speedStretch + freeOscillation;
    float radialScale = 1.0f / std::sqrt(std::max(axialScale, 0.25f));
    float contactShare = 1.0f /
        std::sqrt((float)BubbleActiveContactCount(bubbleIndex));

    for (Vertex& vertex : vertices) {
        glm::vec3 direction = glm::normalize(vertex.Position);
        float alongMotion = glm::dot(direction, motionAxis);
        glm::vec3 axial = motionAxis * alongMotion * axialScale;
        glm::vec3 radial = (direction - motionAxis * alongMotion) * radialScale;
        glm::vec3 position = axial + radial;
        glm::vec3 contactOffset(0.0f);
        float vertexContactSum = 0.0f;

        for (const BubbleContactPair& pair : g_ContactPairs) {
            if ((!pair.candidate && !pair.bonded) ||
                (pair.a != bubbleIndex && pair.b != bubbleIndex)) {
                continue;
            }
            int otherIndex = pair.a == bubbleIndex ? pair.b : pair.a;
            if (otherIndex < 0 || otherIndex >= (int)g_DisplayBubbles.size()) {
                continue;
            }
            glm::vec3 contactDirection =
                g_DisplayBubbles[(size_t)otherIndex].position - bubble.position;
            float contactDistance = glm::length(contactDirection);
            if (contactDistance < 1e-5f) {
                continue;
            }
            contactDirection /= contactDistance;

            float pairStrength = pair.bonded
                ? glm::clamp(0.20f + 0.80f * std::max(pair.interactionCompression,
                                                       Smooth01(pair.contactTime / kFusionTime)),
                             0.0f, 1.0f)
                : Smooth01((bubble.radius + g_DisplayBubbles[(size_t)otherIndex].radius +
                            std::min(bubble.radius, g_DisplayBubbles[(size_t)otherIndex].radius) * 0.08f -
                            contactDistance) /
                           std::max(std::min(bubble.radius,
                                             g_DisplayBubbles[(size_t)otherIndex].radius) * 0.08f,
                                    0.001f));
            if (pairStrength <= 0.001f) {
                continue;
            }
            float deformationStrength = pairStrength * contactShare;

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
            if (glm::length(radialDirection) > 1e-5f) {
                radialDirection = glm::normalize(radialDirection);
            }

            contactOffset -= contactDirection * (0.008f + 0.075f * deformationStrength) * contactMask;
            contactOffset += radialDirection * (0.005f + 0.018f * deformationStrength) * rimMask;
            vertexContactSum += deformationStrength * contactMask;
        }

        float maxOffset = 0.18f;
        float offsetLength = glm::length(contactOffset);
        if (offsetLength > maxOffset) {
            contactOffset *= maxOffset / offsetLength;
        }
        float volumeCompensation = 1.0f + std::min(vertexContactSum, 2.0f) * 0.012f;
        vertex.Position = position * volumeCompensation + contactOffset;
        vertex.Normal = glm::normalize(vertex.Position);
    }

    std::vector<unsigned int> indices = BuildContactBubblePatchIndices();
    std::vector<glm::vec3> normalSums(vertices.size(), glm::vec3(0.0f));
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        unsigned int ia = indices[i];
        unsigned int ib = indices[i + 1];
        unsigned int ic = indices[i + 2];
        glm::vec3 faceNormal = glm::cross(vertices[ib].Position - vertices[ia].Position,
                                         vertices[ic].Position - vertices[ia].Position);
        if (glm::length(faceNormal) <= 1e-8f) {
            continue;
        }
        normalSums[ia] += faceNormal;
        normalSums[ib] += faceNormal;
        normalSums[ic] += faceNormal;
    }
    for (size_t i = 0; i < vertices.size(); ++i) {
        if (glm::length(normalSums[i]) > 1e-6f) {
            vertices[i].Normal = glm::normalize(normalSums[i]);
        }
    }
    return vertices;
}

static void UpdateBubblePair(BubbleContactPair& pair, float dt)
{
    if (!pair.active || pair.a < 0 || pair.b < 0 ||
        pair.a >= (int)g_DisplayBubbles.size() || pair.b >= (int)g_DisplayBubbles.size()) {
        pair.contactTime = std::max(0.0f, pair.contactTime - dt * 1.8f);
        pair.bridgeStrength = std::max(0.0f, pair.bridgeStrength - dt * 2.4f);
        pair.geometryBlend = std::max(0.0f, pair.geometryBlend - dt * 2.0f);
        if (!pair.candidate) {
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

    DisplayBubble& a = g_DisplayBubbles[(size_t)pair.a];
    DisplayBubble& b = g_DisplayBubbles[(size_t)pair.b];
    glm::vec3 delta = b.position - a.position;
    float dist = glm::length(delta);
    if (dist < 1e-4f) {
        return;
    }

    glm::vec3 n = delta / dist;
    float contactDistance = a.radius + b.radius;
    float minRadius = std::min(a.radius, b.radius);
    float proximityDistance = contactDistance + minRadius * 0.012f;
    float proximity = Smooth01((proximityDistance - dist) / std::max(proximityDistance - contactDistance, 0.001f));
    bool inContact = pair.bonded || dist <= proximityDistance || pair.contactTime > 0.0f;

    if (!inContact) {
        pair.state = BubbleContactPair::State::Free;
        pair.contactTime = std::max(0.0f, pair.contactTime - dt * 1.8f);
        pair.bridgeStrength = std::max(0.0f, pair.bridgeStrength - dt * 2.4f);
        pair.geometryBlend = std::max(0.0f, pair.geometryBlend - dt * 2.0f);
        if (!pair.candidate) {
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
    float sharedFilmProgress = Smooth01(std::max(pair.contactTime - kSharedFilmTime, 0.0f) /
                                        std::max(kNeckFormationTime - kSharedFilmTime, 0.001f));
    float fusionProgress = Smooth01(std::max(pair.contactTime - kNeckFormationTime, 0.0f) /
                                    std::max(kFusionTime - kNeckFormationTime, 0.001f));
    float bondBlend = Smooth01(pair.contactTime / 0.30f);
    bool reachedFusionCritical = pair.contactTime >= kFusionTime ||
                                 pair.filmThickness <= kFusionFilmThickness;

    // Film creation is history-dependent, while compression remains a
    // reversible response to current overlap, impact, and continuing forces.
    float baseFilmCompression = Smooth01(pair.contactTime / kNeckFormationTime);
    float baseOverlapRatio = 0.020f * bondBlend + 0.105f * baseFilmCompression +
                             0.235f * fusionProgress;
    float actualOverlapRatio = (contactDistance - dist) / std::max(minRadius, 0.001f);
    float crowdingScale = 1.0f / std::sqrt((float)std::max(
        BubbleActiveContactCount(pair.a), BubbleActiveContactCount(pair.b)));
    float commandedOverlapRatio = baseOverlapRatio +
                                  0.240f * crowdingScale * pair.interactionCompression;
    float normalizedOverlap = Smooth01((actualOverlapRatio - commandedOverlapRatio) / 0.10f);
    float impactDrive = Smooth01(std::max(-relNormalSpeed, 0.0f) / 0.32f);
    float demoExternalDrive = 0.0f;
    if (g_InteractionDemoActive && pair.contactTime > kFusionTime) {
        float postMergeTime = pair.contactTime - kFusionTime;
        float forceEnvelope = Smooth01(postMergeTime / 1.6f);
        float forcePulse = 0.80f + 0.20f * std::sin(postMergeTime * 1.15f);
        demoExternalDrive = forceEnvelope * forcePulse;
    }
    float bondedCompressionDrive = pair.bonded
        ? 0.82f * crowdingScale *
          Smooth01(std::max(pair.contactTime - kSharedFilmTime, 0.0f) / 2.0f)
        : 0.0f;
    float compressionTarget = glm::clamp(std::max({normalizedOverlap,
                                                   impactDrive,
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
        0.32f * pair.contactActivation);
    float crowdingRetention = std::pow(crowdingScale, 0.12f);
    float targetContactRatio = (0.52f + 0.22f * pair.interactionCompression) *
                               crowdingRetention;
    float targetContactRadius = minRadius * targetContactRatio * filmGrowth;
    float radiusAcceleration = (targetContactRadius - pair.contactRadius) * 28.0f -
                               pair.contactRadiusVelocity * 9.5f;
    pair.contactRadiusVelocity += radiusAcceleration * dt;
    pair.contactRadius = glm::clamp(pair.contactRadius + pair.contactRadiusVelocity * dt,
                                    0.0f, minRadius * 0.78f);

    float planeOffsetA = std::sqrt(std::max(a.radius * a.radius -
                                           pair.contactRadius * pair.contactRadius, 0.0f));
    glm::vec3 targetPlaneCenter = a.position + n * planeOffsetA;
    if (!pair.contactFrameInitialized) {
        pair.filteredNormal = n;
        pair.filteredPlaneCenter = targetPlaneCenter;
        pair.contactFrameInitialized = true;
    } else {
        float frameBlend = 1.0f - std::exp(-dt * 10.0f);
        glm::vec3 blendedNormal = glm::mix(pair.filteredNormal, n, frameBlend);
        if (glm::length(blendedNormal) > 1e-5f) {
            pair.filteredNormal = glm::normalize(blendedNormal);
        }
        pair.filteredPlaneCenter = glm::mix(pair.filteredPlaneCenter,
                                            targetPlaneCenter, frameBlend);
    }

    pair.bridgeStrength = Smooth01(pair.contactTime / 0.55f) * contactAmount;
    pair.geometryBlend = glm::clamp(glm::mix(sharedFilmProgress, 1.0f, fusionProgress) * contactAmount,
                                    0.0f, 1.0f);
    if (pair.contactTime < kSharedFilmTime || contactAmount < 0.10f) {
        pair.state = BubbleContactPair::State::Touch;
    } else if (pair.contactTime < kNeckFormationTime) {
        pair.state = BubbleContactPair::State::SharedFilm;
    } else if (!reachedFusionCritical) {
        pair.state = BubbleContactPair::State::NeckForming;
    } else {
        pair.state = BubbleContactPair::State::Merged;
        pair.bridgeStrength = 1.0f;
        pair.geometryBlend = 1.0f;
        pair.filmThickness = std::min(pair.filmThickness, kFusionFilmThickness);
    }
    pair.neckRadius = PlateauContactRadius(pair, a, b);

    if (pair.state == BubbleContactPair::State::Touch) {
        a.state = DisplayBubble::State::Touch;
    } else if (pair.state == BubbleContactPair::State::SharedFilm) {
        a.state = DisplayBubble::State::SharedFilm;
    } else if (pair.state == BubbleContactPair::State::NeckForming) {
        a.state = DisplayBubble::State::NeckForming;
    } else {
        a.state = DisplayBubble::State::Merged;
    }
    b.state = a.state;
    a.mergeProgress = fusionProgress;
    b.mergeProgress = fusionProgress;
    a.contactTime = pair.contactTime;
    b.contactTime = pair.contactTime;
    float visibleContact = glm::clamp(pair.bridgeStrength * (0.55f + 0.65f * pair.geometryBlend), 0.0f, 1.0f);
    a.contactStrength = visibleContact;
    b.contactStrength = visibleContact;
    a.filmThickness = pair.filmThickness;
    b.filmThickness = pair.filmThickness;
    a.contactAxis = n;
    b.contactAxis = -n;
    UpdateSurfaceControls(a, n, pair.bridgeStrength, dt);
    UpdateSurfaceControls(b, -n, pair.bridgeStrength, dt);

    if (pair.bridgeStrength > g_BridgeStrength) {
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

    if (g_SimPaused) {
        return;
    }

    std::vector<glm::vec3> previousPositions;
    previousPositions.reserve(g_DisplayBubbles.size());
    for (const auto& bubble : g_DisplayBubbles) {
        previousPositions.push_back(bubble.position);
    }

    for (auto& pair : g_ContactPairs) {
        if (!pair.bonded) {
            pair.preContactProgress *= std::exp(-dt * 1.2f);
            pair.contactActivation *= std::exp(-dt * 8.0f);
        }
        if (pair.candidate && !pair.bonded &&
            pair.a >= 0 && pair.b >= 0 &&
            pair.a < (int)g_DisplayBubbles.size() && pair.b < (int)g_DisplayBubbles.size()) {
            const DisplayBubble& a = g_DisplayBubbles[(size_t)pair.a];
            const DisplayBubble& b = g_DisplayBubbles[(size_t)pair.b];
            float minRadius = std::min(a.radius, b.radius);
            float releaseDistance = a.radius + b.radius + minRadius * 0.16f;
            if (glm::length(b.position - a.position) > releaseDistance) {
                pair.candidateExitTime += dt;
                if (pair.candidateExitTime > 0.28f) {
                    pair.candidate = false;
                    pair.candidateExitTime = 0.0f;
                }
            } else {
                pair.candidateExitTime = 0.0f;
            }
        }
        pair.active = pair.persistentRenderPair || pair.bonded || pair.candidate;
    }
    if (g_InteractionDemoActive) {
        for (auto& pair : g_ContactPairs) {
            if (pair.a >= 0 && pair.b >= 0 &&
                pair.a < (int)g_DisplayBubbles.size() && pair.b < (int)g_DisplayBubbles.size()) {
                pair.active = true;
            }
        }
    }

    for (auto& bubble : g_DisplayBubbles) {
        if (bubble.state == DisplayBubble::State::Dead) {
            continue;
        }

        bubble.state = DisplayBubble::State::Free;
        float driftScale = g_InteractionDemoActive ? 0.15f : 0.85f;
        glm::vec3 homePull = (bubble.basePosition - bubble.position) *
                             (g_InteractionDemoActive ? 0.020f : 0.340f);
        glm::vec3 slowDrift = glm::vec3(
            sinf((float)g_Time * bubble.speed + bubble.phase),
            cosf((float)g_Time * bubble.speed * 0.7f + bubble.phase * 1.3f),
            sinf((float)g_Time * bubble.speed * 0.5f + bubble.phase * 0.8f)) * (0.018f * driftScale);
        bubble.velocity += (homePull + slowDrift) * dt;
        bubble.velocity *= expf(dt * -0.48f);
        bubble.position += bubble.velocity * dt;
        bubble.contactStrength *= expf(dt * -3.0f);
        if (bubble.targetVolume > 0.0f) {
            float targetRadius = RadiusFromVolume(bubble.targetVolume);
            bubble.radius += (targetRadius - bubble.radius) * std::min(1.0f, dt * 1.8f);
        }
        if (bubble.state == DisplayBubble::State::Free) {
            bubble.filmThickness += (1.0f - bubble.filmThickness) * std::min(1.0f, dt * 0.4f);
            UpdateSurfaceControls(bubble, bubble.contactAxis, 0.0f, dt);
        }
    }

    // The interaction replay is a controlled visual experiment. Move the two
    // bubbles toward first contact at a deterministic rate so frame rate,
    // damping, and the weak ambient drift cannot stall the demonstration.
    if (g_InteractionDemoActive) {
        for (auto& pair : g_ContactPairs) {
            if (!pair.active || pair.contactTime > 0.0f ||
                pair.a < 0 || pair.b < 0 ||
                pair.a >= (int)g_DisplayBubbles.size() || pair.b >= (int)g_DisplayBubbles.size()) {
                continue;
            }
            DisplayBubble& a = g_DisplayBubbles[(size_t)pair.a];
            DisplayBubble& b = g_DisplayBubbles[(size_t)pair.b];
            glm::vec3 delta = b.position - a.position;
            float dist = glm::length(delta);
            if (dist <= 1e-5f) {
                continue;
            }
            glm::vec3 n = delta / dist;
            float targetDistance = a.radius + b.radius + std::min(a.radius, b.radius) * 0.006f;
            float remaining = dist - targetDistance;
            if (remaining > 0.0f) {
                float step = std::min(remaining, dt * 0.115f);
                float invMassA = BubbleInvMass(a);
                float invMassB = BubbleInvMass(b);
                float invMassSum = invMassA + invMassB;
                a.position += n * step * (invMassA / invMassSum);
                b.position -= n * step * (invMassB / invMassSum);
            }
        }
    }

    std::vector<std::pair<int, int>> broadPhasePairs = BuildBubbleBroadPhasePairs();
    for (const auto& candidatePair : broadPhasePairs) {
            int i = candidatePair.first;
            int j = candidatePair.second;
            auto& a = g_DisplayBubbles[(size_t)i];
            auto& b = g_DisplayBubbles[(size_t)j];

            glm::vec3 delta = b.position - a.position;
            float dist = glm::length(delta);
            float contactDistance = a.radius + b.radius;
            float minRadius = std::min(a.radius, b.radius);
            float nearRange = minRadius * 0.080f;
            float contactEpsilon = minRadius * 0.012f;
            if (dist > 1e-4f && dist < contactDistance + nearRange) {
                glm::vec3 n = delta / dist;
                float gap = dist - contactDistance;
                float contactActivation = Smooth01((nearRange - gap) /
                                                    std::max(nearRange, 0.001f));
                float capillaryPull = 0.100f * contactActivation;
                a.velocity += n * capillaryPull * dt;
                b.velocity -= n * capillaryPull * dt;

                int pairIndex = EnsureContactPair(i, j);
                BubbleContactPair& pair = g_ContactPairs[(size_t)pairIndex];
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
                if (!pair.contactFrameInitialized) {
                    pair.filteredNormal = n;
                    pair.filteredPlaneCenter = targetPlaneCenter;
                    pair.contactFrameInitialized = true;
                } else {
                    float frameBlend = 1.0f - std::exp(-dt * 12.0f);
                    glm::vec3 blendedNormal = glm::mix(pair.filteredNormal, n, frameBlend);
                    if (glm::length(blendedNormal) > 1e-5f) {
                        pair.filteredNormal = glm::normalize(blendedNormal);
                    }
                    pair.filteredPlaneCenter = glm::mix(pair.filteredPlaneCenter,
                                                        targetPlaneCenter, frameBlend);
                }
                pair.restDistance = contactDistance;
                pair.targetVolume = a.targetVolume + b.targetVolume;
            }
            if (dist <= 1e-4f || dist > contactDistance + contactEpsilon) {
                continue;
            }

            int pairIndex = EnsureContactPair(i, j);
            BubbleContactPair& pair = g_ContactPairs[(size_t)pairIndex];
            bool firstBondFrame = !pair.bonded;
            pair.active = true;
            pair.bonded = true;
            pair.candidate = true;
            pair.candidateExitTime = 0.0f;
            if (firstBondFrame) {
                float inheritedContact = glm::clamp(pair.preContactProgress, 0.0f, 1.0f);
                pair.contactTime = std::max(pair.contactTime, 0.20f * inheritedContact);
                pair.contactRadius = std::max(pair.contactRadius,
                    minRadius * 0.14f * inheritedContact);
                pair.interactionCompression = std::max(pair.interactionCompression,
                    0.10f * inheritedContact);
            }
            pair.restDistance = contactDistance;
            pair.targetVolume = a.targetVolume + b.targetVolume;
    }

    const int solverIterations = 6;
    const float mainRadius = g_MainBubbleVisualRadius;
    for (int iter = 0; iter < solverIterations; ++iter) {
        for (auto& pair : g_ContactPairs) {
            if (!pair.active || pair.a < 0 || pair.b < 0 ||
                pair.a >= (int)g_DisplayBubbles.size() || pair.b >= (int)g_DisplayBubbles.size()) {
                continue;
            }

            DisplayBubble& a = g_DisplayBubbles[(size_t)pair.a];
            DisplayBubble& b = g_DisplayBubbles[(size_t)pair.b];
            if (a.state == DisplayBubble::State::Dead || b.state == DisplayBubble::State::Dead) {
                continue;
            }

            glm::vec3 delta = b.position - a.position;
            float dist = glm::length(delta);
            if (dist < 1e-5f) {
                delta = glm::vec3(1.0f, 0.0f, 0.0f);
                dist = 1.0f;
            }

            glm::vec3 n = delta / dist;
            float minRadius = std::min(a.radius, b.radius);
            float contactEpsilon = minRadius * 0.012f;
            if (!pair.bonded && dist > a.radius + b.radius + contactEpsilon) {
                continue;
            }
            float bondBlend = Smooth01(pair.contactTime / 0.30f);
            float contactRadius = glm::clamp(pair.contactRadius,
                                             0.0f, minRadius * 0.82f);
            float restDistance =
                std::sqrt(std::max(a.radius * a.radius - contactRadius * contactRadius, 0.0f)) +
                std::sqrt(std::max(b.radius * b.radius - contactRadius * contactRadius, 0.0f));
            float constraint = dist - restDistance;
            if (constraint >= 0.0f) {
                float contactMargin = minRadius * (pair.bonded ? 0.24f : 0.035f);
                float adhesionRange = contactMargin * 1.15f;
                float adhesion = Smooth01((restDistance + adhesionRange - dist) / std::max(adhesionRange, 0.001f));
                adhesion *= 0.030f + 0.090f * Smooth01(pair.contactTime / 0.85f);
                adhesion *= bondBlend;
                if (adhesion <= 0.0f) {
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
            if (pair.bonded && pair.contactTime < 0.24f) {
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

        for (auto& bubble : g_DisplayBubbles) {
            if (bubble.state == DisplayBubble::State::Dead || !g_ShowMainBubble) {
                continue;
            }

            glm::vec3 delta = bubble.position;
            float dist = glm::length(delta);
            if (dist < 1e-5f) {
                delta = glm::vec3(1.0f, 0.0f, 0.0f);
                dist = 1.0f;
            }

            glm::vec3 n = delta / dist;
            float contactProgress = Smooth01(bubble.contactTime / 1.0f);
            float restDistance = mainRadius + bubble.radius - bubble.radius * (0.025f + 0.075f * contactProgress);
            if (dist < restDistance) {
                bubble.position += n * (restDistance - dist);
            }
        }
    }

    if (dt > 1e-5f) {
        for (size_t i = 0; i < g_DisplayBubbles.size(); ++i) {
            auto& bubble = g_DisplayBubbles[i];
            if (bubble.state == DisplayBubble::State::Dead) {
                continue;
            }

            bubble.velocity = (bubble.position - previousPositions[i]) / dt;
            float speed = glm::length(bubble.velocity);
            if (speed > 1.20f) {
                bubble.velocity *= 1.20f / speed;
            }
        }
    }

    for (auto& pair : g_ContactPairs) {
        if (!pair.active || pair.a < 0 || pair.b < 0 ||
            pair.a >= (int)g_DisplayBubbles.size() || pair.b >= (int)g_DisplayBubbles.size()) {
            UpdateBubblePair(pair, dt);
            continue;
        }

        DisplayBubble& a = g_DisplayBubbles[(size_t)pair.a];
        DisplayBubble& b = g_DisplayBubbles[(size_t)pair.b];
        glm::vec3 delta = b.position - a.position;
        float dist = glm::length(delta);
        float contactDistance = a.radius + b.radius;
        float contactEpsilon = std::min(a.radius, b.radius) * 0.012f;
        bool resolveContactImpulse = pair.candidate || pair.bonded ||
                                     dist <= contactDistance + contactEpsilon;
        if (dist > 1e-5f && resolveContactImpulse) {
            glm::vec3 n = delta / dist;
            float relNormalSpeed = glm::dot(b.velocity - a.velocity, n);
            if (pair.bonded || relNormalSpeed < 0.0f) {
                float invMassA = BubbleInvMass(a);
                float invMassB = BubbleInvMass(b);
                float dampingRate = glm::mix(1.5f, 22.0f,
                                             glm::clamp(pair.contactActivation,
                                                        0.0f, 1.0f));
                float normalDamping = 1.0f - std::exp(-dampingRate * dt);
                float impulseMagnitude = (-relNormalSpeed * normalDamping) /
                                         (invMassA + invMassB);
                glm::vec3 impulse = n * impulseMagnitude;
                a.velocity -= impulse * invMassA;
                b.velocity += impulse * invMassB;
            }
        }
        UpdateBubblePair(pair, dt);
    }

    for (auto& bubble : g_DisplayBubbles) {
        if (bubble.state == DisplayBubble::State::Dead || !g_ShowMainBubble) {
            continue;
        }

        glm::vec3 delta = bubble.position;
        float dist = glm::length(delta);
        if (dist < 1e-5f) {
            continue;
        }

        glm::vec3 n = delta / dist;
        float contactDistance = mainRadius + bubble.radius;
        float proximityDistance = contactDistance + bubble.radius * 0.28f;
        if (dist < proximityDistance) {
            float strength = Smooth01((proximityDistance - dist) / std::max(proximityDistance - contactDistance, 0.001f));
            bubble.contactTime += dt * (0.35f + 0.65f * strength);
            bubble.contactStrength = std::max(bubble.contactStrength, strength * 0.55f);
            bubble.filmThickness = std::max(0.32f, bubble.filmThickness - dt * 0.035f * strength);
            bubble.contactAxis = -n;
            bubble.state = strength > 0.65f ? DisplayBubble::State::SharedFilm : DisplayBubble::State::Touch;
            UpdateSurfaceControls(bubble, -n, strength, dt);

            float relSpeed = glm::dot(bubble.velocity, n);
            if (relSpeed < 0.0f) {
                bubble.velocity -= n * relSpeed * 0.72f;
            }

            if (strength > g_BridgeStrength) {
                g_BridgeStrength = strength;
                g_BridgeDirection = n;
                g_AutoTouchVelocity = 0.25f + strength * 0.55f;
            }
        } else {
            bubble.contactTime = std::max(0.0f, bubble.contactTime - dt * 1.2f);
        }
    }

    g_ContactPairs.erase(
        std::remove_if(g_ContactPairs.begin(), g_ContactPairs.end(), [](const BubbleContactPair& pair) {
            return !pair.active && pair.contactTime <= 0.01f && pair.geometryBlend <= 0.01f;
        }),
        g_ContactPairs.end());

    if (!g_ContactPairs.empty() && g_Time - s_LastContactLogTime >= 1.0) {
        const auto& pair = g_ContactPairs.front();
        if (pair.a >= 0 && pair.b >= 0 &&
            pair.a < (int)g_DisplayBubbles.size() && pair.b < (int)g_DisplayBubbles.size()) {
            const auto& a = g_DisplayBubbles[(size_t)pair.a];
            const auto& b = g_DisplayBubbles[(size_t)pair.b];
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
    if (!g_SimPaused && ((g_SimFrameCounter++ % kSimFrameInterval) == 0)) {
        g_Sim.update(1.0f / 60.0f);  // matches HarmonyOS throttled DBSTT update cadence
    }
    UpdateDisplayBubbleInteractions((float)g_DeltaTime);

    // Upload updated vertices from simulation to GPU
    if (g_RefractModel && g_RefractModel->getMeshCount() > 0) {
        auto verts = buildVerticesFromSim(g_Sim);
        Mesh* m = g_RefractModel->getMesh(0);
        if (m) m->updateVertices(verts);
    }

    // Model matrix: identity — all deformation is in the simulated vertex positions
    glm::mat4 view = g_Camera.getViewMatrix();
    glm::mat4 proj = g_Camera.getProjectionMatrix();

    if (g_BridgeStrength > 0.01f) {
        glm::vec4 clip = proj * view * glm::vec4(g_BridgeDirection * g_MainBubbleVisualRadius, 1.0f);
        if (clip.w > 0.001f) {
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
        g_RefractionShader->SetFloat("uOutputAlpha", outputAlpha);
        glm::vec2 touchPoint = glm::vec2(-10.0f, -10.0f);
        float touchStrength = 0.0f;
        float touchVelocity = 0.0f;
        if (interactive) {
            touchPoint = g_TouchStrength >= g_AutoTouchStrength ? g_TouchPoint : g_AutoTouchPoint;
            touchStrength = std::max(g_TouchStrength, g_AutoTouchStrength);
            touchVelocity = std::max(g_TouchVelocity, g_AutoTouchVelocity);
        }
        g_RefractionShader->SetVec2("uTouchPoint", touchPoint);
        g_RefractionShader->SetFloat("uTouchStrength", touchStrength);
        g_RefractionShader->SetFloat("uTouchVelocity", touchVelocity);
    };

    auto SetBubbleContactUniforms = [&](int bubbleIndex)
    {
        if (bubbleIndex < 0 || bubbleIndex >= (int)g_DisplayBubbles.size()) {
            return;
        }
        const DisplayBubble& bubble = g_DisplayBubbles[(size_t)bubbleIndex];
        int visualContactCount = 0;
        for (const BubbleContactPair& pair : g_ContactPairs) {
            if ((!pair.candidate && !pair.bonded) || pair.contactRadius <= 0.001f ||
                (pair.a != bubbleIndex && pair.b != bubbleIndex) ||
                visualContactCount >= 4) {
                continue;
            }
            int otherIndex = pair.a == bubbleIndex ? pair.b : pair.a;
            if (otherIndex < 0 || otherIndex >= (int)g_DisplayBubbles.size()) {
                continue;
            }
            if (!pair.contactFrameInitialized) {
                continue;
            }
            glm::vec3 planeNormal = pair.a == bubbleIndex
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

    auto DrawDisplayBubbles = [&](bool isBackFace, bool renderToFBO, GLuint backgroundTexture, bool straightComposite)
    {
        BindRefractionInputs(backgroundTexture);

        std::vector<float> shellAlphaScales(g_DisplayBubbles.size(), 1.0f);
        GLboolean structuralBlendWasEnabled = glIsEnabled(GL_BLEND);
        if (straightComposite) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
        }

        if (false) for (const auto& pair : g_ContactPairs) {
            if (!pair.active ||
                pair.a < 0 || pair.b < 0 ||
                pair.a >= (int)g_DisplayBubbles.size() || pair.b >= (int)g_DisplayBubbles.size() ||
                !g_FusedBubbleModel) {
                continue;
            }

            const DisplayBubble& a = g_DisplayBubbles[(size_t)pair.a];
            const DisplayBubble& b = g_DisplayBubbles[(size_t)pair.b];
            if (a.state == DisplayBubble::State::Dead || b.state == DisplayBubble::State::Dead) {
                continue;
            }

            glm::mat4 modelA = BubbleModelMatrix(a, (float)g_Time);
            glm::mat4 modelB = BubbleModelMatrix(b, (float)g_Time);
            glm::vec3 pa = glm::vec3(modelA[3]);
            glm::vec3 pb = glm::vec3(modelB[3]);
            glm::vec3 axis = pb - pa;
            float centerDistance = glm::length(axis);
            if (centerDistance <= 1e-4f) {
                continue;
            }
            axis /= centerDistance;

            auto modelVolumeRadius = [](const glm::mat4& model) {
                float sx = glm::length(glm::vec3(model[0]));
                float sy = glm::length(glm::vec3(model[1]));
                float sz = glm::length(glm::vec3(model[2]));
                return std::cbrt(std::max(sx * sy * sz, 1e-9f));
            };
            float radiusA = modelVolumeRadius(modelA);
            float radiusB = modelVolumeRadius(modelB);
            float minRadius = std::min(radiusA, radiusB);
            float contactDistance = radiusA + radiusB;
            float nearRange = minRadius * 0.080f;
            float approachProgress = Smooth01((contactDistance + nearRange - centerDistance) /
                                               std::max(nearRange, 0.001f));
            float contactProgress = Smooth01(pair.contactTime / kFusionTime);
            float filmGrowth = std::pow(contactProgress, 0.68f);
            float capillaryPulse = 1.0f +
                std::sin((float)g_Time * 0.48f + pair.contactTime * 0.55f) *
                0.008f * contactProgress;

            DoubleBubbleGeometry geometry;
            geometry.centerDistance = centerDistance;
            geometry.radiusA = radiusA;
            geometry.radiusB = radiusB;
            geometry.flattenA = 0.018f * approachProgress + 0.040f * filmGrowth +
                                0.080f * pair.interactionCompression;
            geometry.flattenB = geometry.flattenA;
            geometry.contactRadius = minRadius *
                (0.44f * filmGrowth + 0.20f * pair.interactionCompression) *
                capillaryPulse;
            geometry.junctionBlend = filmGrowth;
            if (Mesh* mesh = g_FusedBubbleModel->getMesh(0)) {
                mesh->updateVertices(BuildFusedBubbleVertices(geometry));
            }

            float assemblyAlpha = straightComposite
                ? (g_InteractionDemoActive ? 0.62f : 0.72f) * std::min(a.alpha, b.alpha)
                : std::min(a.alpha, b.alpha);
            SetRefractUniforms(AxisBasisModel((pa + pb) * 0.5f, axis),
                               std::max(radiusA, radiusB),
                               isBackFace, renderToFBO,
                               false, 0.0f, 0.0f, 2, assemblyAlpha, 1.0f);
            g_FusedBubbleModel->Draw(*g_RefractionShader);

            shellAlphaScales[(size_t)pair.a] = 0.0f;
            shellAlphaScales[(size_t)pair.b] = 0.0f;
        }

        if (straightComposite) {
            glDepthMask(GL_TRUE);
            if (!structuralBlendWasEnabled) {
                glDisable(GL_BLEND);
            }
        }

        if (false && g_InteractionDemoActive && !g_ContactPairs.empty() && g_FusedBubbleModel) {
            const BubbleContactPair& pair = g_ContactPairs[0];
            float topologyProgress = Smooth01(std::max(pair.contactTime - kSharedFilmTime, 0.0f) /
                                               std::max(kFusionTime - kSharedFilmTime, 0.001f));
            if (pair.active && pair.state == BubbleContactPair::State::Merged && topologyProgress > 0.01f &&
                pair.a >= 0 && pair.b >= 0 &&
                pair.a < (int)g_DisplayBubbles.size() && pair.b < (int)g_DisplayBubbles.size()) {
                const DisplayBubble& a = g_DisplayBubbles[(size_t)pair.a];
                const DisplayBubble& b = g_DisplayBubbles[(size_t)pair.b];
                glm::vec3 pa = BubbleVisualCenter(a, (float)g_Time);
                glm::vec3 pb = BubbleVisualCenter(b, (float)g_Time);
                glm::vec3 n = pb - pa;
                float dist = glm::length(n);
                if (dist > 1e-4f) {
                    n /= dist;
                    glm::vec3 doubleBubbleCenter = (pa + pb) * 0.5f;
                    float fusedRadius = std::max(a.radius, b.radius);
                    float blendWithBreathing = glm::clamp(topologyProgress +
                        std::sin((float)g_Time * 0.55f + pair.contactTime) * 0.008f * topologyProgress,
                        0.0f, 1.0f);

                    if (Mesh* mesh = g_FusedBubbleModel->getMesh(0)) {
                        mesh->updateVertices(BuildFusedBubbleVertices(fusedRadius, blendWithBreathing));
                    }

                    GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
                    if (straightComposite) {
                        glEnable(GL_BLEND);
                        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                        glDepthMask(GL_FALSE);
                    }

                    float alpha = (straightComposite ? 0.78f : 1.0f) * topologyProgress;
                    glm::mat4 fusedModel = AxisBasisModel(doubleBubbleCenter, n);
                    SetRefractUniforms(fusedModel, fusedRadius * 1.85f, isBackFace, renderToFBO, false, 0.012f, 0.0f, 2, alpha, pair.filmThickness);
                    g_FusedBubbleModel->Draw(*g_RefractionShader);

                    if (straightComposite) {
                        glDepthMask(GL_TRUE);
                        if (!blendWasEnabled) {
                        glDisable(GL_BLEND);
                        }
                    }
                    if (topologyProgress > 0.985f || pair.state == BubbleContactPair::State::Merged) {
                        return;
                    }
                }
            }
        }

        float independentBubbleAlphaScale = 1.0f;

        std::vector<std::pair<float, size_t>> sortedBubbles;
        sortedBubbles.reserve(g_DisplayBubbles.size());
        for (size_t bubbleIndex = 0; bubbleIndex < g_DisplayBubbles.size(); ++bubbleIndex)
        {
            const auto& bubble = g_DisplayBubbles[bubbleIndex];
            if (bubble.state == DisplayBubble::State::Dead || bubble.alpha <= 0.01f || bubble.radius <= 0.001f) {
                continue;
            }
            if (shellAlphaScales[bubbleIndex] <= 0.001f) {
                continue;
            }
            glm::vec3 center = glm::vec3(PersistentBubbleModelMatrix(bubble, (float)g_Time) *
                                         glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
            glm::vec3 toCamera = center - g_Camera.getPosition();
            sortedBubbles.emplace_back(glm::dot(toCamera, toCamera), bubbleIndex);
        }
        std::sort(sortedBubbles.begin(), sortedBubbles.end(),
                  [](const auto &a, const auto &b) { return a.first > b.first; });

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
            float shellAlphaScale = shellAlphaScales[item.second];
            glm::mat4 bubbleModel = PersistentBubbleModelMatrix(bubble, (float)g_Time);
            float alpha = straightComposite
                ? (g_InteractionDemoActive ? 0.62f : 0.72f) * bubble.alpha * independentBubbleAlphaScale * shellAlphaScale
                : bubble.alpha * independentBubbleAlphaScale * shellAlphaScale;
            SetRefractUniforms(bubbleModel, bubble.radius, isBackFace, renderToFBO,
                               false, 0.0f, 0.0f, 2, alpha, 1.0f);
            SetBubbleContactUniforms((int)item.second);
            Model* shellModel = item.second < g_DisplayBubbleModels.size()
                ? g_DisplayBubbleModels[item.second]
                : nullptr;
            if (shellModel) {
                if (Mesh* mesh = shellModel->getMesh(0)) {
                    mesh->updateVertices(BuildPersistentBubbleVertices((int)item.second,
                                                                        (float)g_Time));
                }
                shellModel->Draw(*g_RefractionShader);
            } else if (g_DecorativeBubbleModel) {
                g_DecorativeBubbleModel->Draw(*g_RefractionShader);
            }
        }

        if (false && !g_InteractionDemoActive && !g_ContactPairs.empty() && g_FusedBubbleModel) {
            const BubbleContactPair& pair = g_ContactPairs[0];
            float topologyProgress = Smooth01(pair.geometryBlend);
            if (pair.active && pair.state == BubbleContactPair::State::Merged && topologyProgress > 0.01f &&
                pair.a >= 0 && pair.b >= 0 &&
                pair.a < (int)g_DisplayBubbles.size() && pair.b < (int)g_DisplayBubbles.size()) {
                const DisplayBubble& a = g_DisplayBubbles[(size_t)pair.a];
                const DisplayBubble& b = g_DisplayBubbles[(size_t)pair.b];
                glm::vec3 pa = BubbleVisualCenter(a, (float)g_Time);
                glm::vec3 pb = BubbleVisualCenter(b, (float)g_Time);
                glm::vec3 axis = pb - pa;
                float dist = glm::length(axis);
                if (dist > 1e-4f) {
                    axis /= dist;
                    float fusedRadius = std::max(a.radius, b.radius);
                    float topologyBlend = glm::clamp(topologyProgress +
                        std::sin((float)g_Time * 0.64f + pair.contactTime) * 0.010f * topologyProgress,
                        0.0f, 1.0f);

                    if (Mesh* mesh = g_FusedBubbleModel->getMesh(0)) {
                        mesh->updateVertices(BuildFusedBubbleVertices(fusedRadius, topologyBlend));
                    }

                    glm::mat4 fusedModel = AxisBasisModel((pa + pb) * 0.5f, axis);
                    float alpha = (straightComposite ? 0.82f : 1.0f) * topologyProgress;
                    SetRefractUniforms(fusedModel, fusedRadius * 1.85f, isBackFace, renderToFBO, false,
                                       0.012f, 0.0f, 2, alpha, pair.filmThickness * 0.92f);
                    g_FusedBubbleModel->Draw(*g_RefractionShader);
                }
            }
        }

        if (straightComposite)
        {
            glDepthMask(GL_TRUE);
            if (!blendWasEnabled)
                glDisable(GL_BLEND);
        }
    };

    auto DrawContactBridges = [&](bool isBackFace, bool renderToFBO, GLuint backgroundTexture, bool straightComposite)
    {
        BindRefractionInputs(backgroundTexture);
        if (!g_ContactFilmModel) {
            return;
        }

        GLboolean filmBlendWasEnabled = glIsEnabled(GL_BLEND);
        GLboolean filmCullWasEnabled = glIsEnabled(GL_CULL_FACE);
        glDisable(GL_CULL_FACE);
        if (straightComposite) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
        }

        for (const BubbleContactPair& pair : g_ContactPairs) {
            if ((!pair.candidate && !pair.bonded) || pair.contactRadius <= 0.002f ||
                pair.a < 0 || pair.b < 0 ||
                pair.a >= (int)g_DisplayBubbles.size() ||
                pair.b >= (int)g_DisplayBubbles.size()) {
                continue;
            }
            const DisplayBubble& a = g_DisplayBubbles[(size_t)pair.a];
            const DisplayBubble& b = g_DisplayBubbles[(size_t)pair.b];
            float visibility = pair.contactActivation * pair.contactActivation;
            float alpha = (straightComposite ? 0.16f : 0.24f) * visibility;
            glm::mat4 filmModel = MakeContactFilmModel(a, b, pair, (float)g_Time);
            SetRefractUniforms(filmModel, std::max(pair.contactRadius, 0.01f),
                               isBackFace, renderToFBO, false,
                               0.0f, 0.0f, 2, alpha, 1.0f);
            g_RefractionShader->SetInt("uForceSharedFilm", 1);
            g_RefractionShader->SetFloat("uRefractionStrength", g_RefractionStrength * 0.22f);
            g_RefractionShader->SetFloat("uEnvironmentReflectionStrength",
                                         g_EnvironmentReflectionStrength * 0.45f);
            g_ContactFilmModel->Draw(*g_RefractionShader);
        }
        g_RefractionShader->SetInt("uForceSharedFilm", 0);

        if (straightComposite) {
            glDepthMask(GL_TRUE);
            if (!filmBlendWasEnabled) {
                glDisable(GL_BLEND);
            }
        }
        if (filmCullWasEnabled) {
            glEnable(GL_CULL_FACE);
        }
        return;

        BindRefractionInputs(backgroundTexture);

        GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
        GLboolean cullWasEnabled = glIsEnabled(GL_CULL_FACE);
        GLboolean depthWasEnabled = glIsEnabled(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        if (straightComposite)
        {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
        }

        for (const auto& pair : g_ContactPairs)
        {
            if (!pair.active || pair.geometryBlend <= 0.001f ||
                (pair.state != BubbleContactPair::State::SharedFilm &&
                 pair.state != BubbleContactPair::State::NeckForming &&
                 pair.state != BubbleContactPair::State::Merged) ||
                pair.a < 0 || pair.b < 0 ||
                pair.a >= (int)g_DisplayBubbles.size() || pair.b >= (int)g_DisplayBubbles.size()) {
                continue;
            }

            if (g_InteractionDemoActive && pair.state != BubbleContactPair::State::SharedFilm && !g_NeckBridgeModel) {
                continue;
            }
            if ((!g_InteractionDemoActive || pair.state == BubbleContactPair::State::SharedFilm) && !g_ContactFilmModel) {
                continue;
            }

            const auto& a = g_DisplayBubbles[(size_t)pair.a];
            const auto& b = g_DisplayBubbles[(size_t)pair.b];
            float topologyProgress = Smooth01(pair.geometryBlend);
            float earlyFilmFade = 1.0f;
            if (earlyFilmFade <= 0.01f) {
                continue;
            }
            float ruptureFade = (1.0f - pair.ruptureRisk * 0.55f) * earlyFilmFade;
            if (g_InteractionDemoActive && pair.state != BubbleContactPair::State::SharedFilm) {
                glm::vec3 pa = BubbleVisualCenter(a, (float)g_Time);
                glm::vec3 pb = BubbleVisualCenter(b, (float)g_Time);
                glm::vec3 axis = pb - pa;
                float centerDistance = glm::length(axis);
                if (centerDistance <= 1e-4f) {
                    continue;
                }
                axis /= centerDistance;

                float minRadius = std::min(a.radius, b.radius);
                float endRadius = minRadius * glm::mix(0.055f, 0.30f, pair.geometryBlend);
                float waistRadius = endRadius * glm::mix(0.35f, 0.58f, pair.bridgeStrength);
                float overlap = std::max((a.radius + b.radius) - centerDistance, 0.0f);
                float halfLength = std::max(minRadius * 0.055f,
                                            overlap * 0.55f + minRadius * 0.030f);
                halfLength = std::min(halfLength, minRadius * 0.34f);

                if (Mesh* mesh = g_NeckBridgeModel->getMesh(0)) {
                    mesh->updateVertices(BuildNeckBridgeVertices(halfLength, endRadius, waistRadius, pair.geometryBlend));
                }

                glm::mat4 neckModel = AxisBasisModel((pa + pb) * 0.5f, axis);
                float alpha = (straightComposite ? 0.28f : 0.48f) * pair.geometryBlend * ruptureFade;
                SetRefractUniforms(neckModel, std::max(endRadius, 0.03f), isBackFace, renderToFBO, false, 0.004f, 0.0f, 2, alpha, 1.0f);
                g_NeckBridgeModel->Draw(*g_RefractionShader);
            } else {
                glm::mat4 bridgeModel = MakeContactFilmModel(a, b, pair, (float)g_Time);
                float localRadius = std::max(PlateauContactRadius(pair, a, b), 0.03f);
                float filmGrow = Smooth01(pair.contactTime / kSharedFilmTime);
                float alpha = (straightComposite ? 0.30f : 0.42f) * filmGrow * ruptureFade;
                SetRefractUniforms(bridgeModel, localRadius, isBackFace, renderToFBO, false, 0.006f, 0.0f, 2, alpha, 1.0f);
                g_ContactFilmModel->Draw(*g_RefractionShader);
            }
        }

        if (straightComposite)
        {
            glDepthMask(GL_TRUE);
            if (!blendWasEnabled)
                glDisable(GL_BLEND);
        }
        if (cullWasEnabled)
            glEnable(GL_CULL_FACE);
        if (depthWasEnabled)
            glEnable(GL_DEPTH_TEST);
    };

    auto DrawMainBubble = [&](bool isBackFace, bool renderToFBO, GLuint backgroundTexture)
    {
        if (!g_ShowMainBubble) {
            return;
        }
        BindRefractionInputs(backgroundTexture);
        float mainScale = g_MainBubbleVisualRadius / std::max(g_BubbleRadius, 0.001f);
        glm::mat4 mainModel = glm::scale(glm::mat4(1.0f), glm::vec3(mainScale));
        SetRefractUniforms(mainModel, g_MainBubbleVisualRadius, isBackFace, renderToFBO, true, 0.085f, 0.0f, g_IridescenceMode, 1.0f, 1.0f);
        g_RefractModel->Draw(*g_RefractionShader);
    };

    auto SetBackgroundUniforms = [&]()
    {
        if (g_InteractionDemoActive) {
            return;
        }

        g_BackgroundShader->Use();
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

    DrawDisplayBubbles(false, true, g_BackgroundTexture, true);
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

    DrawDisplayBubbles(false, false, g_MainSceneTexture, true);
    DrawContactBridges(false, false, g_MainSceneTexture, true);
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
    case GLFW_KEY_J:
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
    case GLFW_KEY_L:
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
    case GLFW_KEY_I:
        StartBubbleInteractionDemo();
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
    delete g_ContactFilmModel;
    delete g_ContactBubbleAModel;
    delete g_ContactBubbleBModel;
    delete g_FusedBubbleModel;
    delete g_NeckBridgeModel;
    delete g_SkyboxModel;
    g_RefractModel = g_DecorativeBubbleModel = g_SkyboxModel = nullptr;
    g_ContactFilmModel = nullptr;
    g_ContactBubbleAModel = nullptr;
    g_ContactBubbleBModel = nullptr;
    g_FusedBubbleModel = nullptr;
    g_NeckBridgeModel = nullptr;

    for (Model* model : g_DisplayBubbleModels) {
        delete model;
    }
    g_DisplayBubbleModels.clear();

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
int main()
{
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

    std::cout << "OpenGL " << glGetString(GL_VERSION) << std::endl << std::flush;
    std::cout << "Renderer: " << glGetString(GL_RENDERER) << std::endl << std::flush;

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
    g_Sim.timeStep       = g_SimTimeStep;
    g_Sim.substepsPerFrame = g_SimSubsteps;
    g_SimFrameCounter = 0;

    // Build rendering Model from simulation mesh
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

    g_DecorativeBubbleModel = Model::CreateSphere(1.0f, 32, 16, true);
    {
        DiscMeshData disc = BuildContactFilmDisc(72);
        g_ContactFilmModel = Model::CreateFromVertices(disc.positions, disc.normals, disc.indices);
    }
    {
        std::vector<glm::vec3> positions;
        std::vector<glm::vec3> normals;
        std::vector<unsigned int> indices;
        BuildContactBubblePatchMesh(1.0f, 1.0f, positions, normals, indices);
        g_ContactBubbleAModel = Model::CreateFromVertices(positions, normals, indices);
        g_ContactBubbleBModel = Model::CreateFromVertices(positions, normals, indices);
        BuildFusedBubbleMesh(0.56f, positions, normals, indices);
        g_FusedBubbleModel = Model::CreateFromVertices(positions, normals, indices);
        BuildNeckBridgeMesh(positions, normals, indices);
        g_NeckBridgeModel = Model::CreateFromVertices(positions, normals, indices);
    }
    ResetDisplayBubbles();

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
    std::cout << std::endl;
    std::cout << "Appearance" << std::endl;
    std::cout << "  R / T          Fresnel edge power       - / +" << std::endl;
    std::cout << "  Y / H          Refraction strength      - / +" << std::endl;
    std::cout << "  U / J          Edge distortion          - / +" << std::endl;
    std::cout << "  O / P          Environment reflection   - / +" << std::endl;
    std::cout << std::endl;
    std::cout << "Thin Film" << std::endl;
    std::cout << "  L              Cycle mode: Kim2012 / LUT / Belcour Airy" << std::endl;
    std::cout << "  N / M          Film thickness (nm)      - / +" << std::endl;
    std::cout << "  1 / 2          Thickness variation      - / +" << std::endl;
    std::cout << std::endl;
    std::cout << "Simulation" << std::endl;
    std::cout << "  Z              Pause / resume" << std::endl;
    std::cout << "  X              Reset synced bubble scene" << std::endl;
    std::cout << "  I              Replay bubble contact / bridge demo" << std::endl;
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
              << std::endl;
    std::cout << "Scene: three persistent bubbles with wind drift and multi-contact deformation" << std::endl;
    std::cout << "==================================================" << std::endl;

    while (!glfwWindowShouldClose(window))
    {
        double currentTime = glfwGetTime();
        if (g_LastFrameTime == 0.0)
            g_LastFrameTime = currentTime;
        g_DeltaTime = currentTime - g_LastFrameTime;
        g_Time += g_DeltaTime;
        g_LastFrameTime = currentTime;
        UpdateFPS(window, currentTime);

        RenderFrame();
        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    // -------- Shutdown --------
    Cleanup();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
