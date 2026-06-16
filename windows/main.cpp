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
#include <vector>
#include <string>

// ============================================================
// Global state (mirrors napi_init.cpp globals)
// ============================================================
static int g_WindowWidth = 1280;
static int g_WindowHeight = 720;
static constexpr const char *g_AppTitle = "Soap Bubble Rendering - Thin Film + DBSTT";

static float g_CameraDistance = 3.5f;
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
static int   g_SimSubdivs  = 3;       // icosphere subdivision level
static float g_SimPerturb  = 0.10f;   // initial stretch 10%
static float g_SimTimeStep = 0.001f;  // Δt per substep
static int   g_SimSubsteps = 3;       // substeps per frame
static bool  g_SimPaused   = false;

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

struct DisplayBubble {
    glm::vec3 basePosition;
    float radius;
    float phase;
    float windAmplitude;
    float floatAmplitude;
    float speed;
};

static std::vector<DisplayBubble> g_DisplayBubbles;

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

static glm::mat4 BubbleModelMatrix(const DisplayBubble &bubble, float time)
{
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

    g_Camera.setPosition(cameraPos);
    g_Camera.setTarget(glm::vec3(0.0f, 0.0f, 0.0f));
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
    if (!g_SimPaused) {
        g_Sim.update(1.0f / 60.0f);  // target ~60fps simulation rate
    }

    // Upload updated vertices from simulation to GPU
    if (g_RefractModel && g_RefractModel->getMeshCount() > 0) {
        auto verts = buildVerticesFromSim(g_Sim);
        Mesh* m = g_RefractModel->getMesh(0);
        if (m) m->updateVertices(verts);
    }

    // Model matrix: identity — all deformation is in the simulated vertex positions
    glm::mat4 view = g_Camera.getViewMatrix();
    glm::mat4 proj = g_Camera.getProjectionMatrix();

    auto SetRefractUniforms = [&](const glm::mat4 &model,
                                  float localRadius,
                                  bool isBackFace,
                                  bool renderToFBO,
                                  bool interactive,
                                  int iridescenceMode)
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
        g_RefractionShader->SetFloat("uThickness", CurrentThickness());
        g_RefractionShader->SetFloat("uThicknessVar", g_ThicknessVar);
        g_RefractionShader->SetFloat("uTime", (float)g_Time);
        g_RefractionShader->SetVec2("uTouchPoint", interactive ? g_TouchPoint : glm::vec2(-10.0f, -10.0f));
        g_RefractionShader->SetFloat("uTouchStrength", interactive ? g_TouchStrength : 0.0f);
        g_RefractionShader->SetFloat("uTouchVelocity", interactive ? g_TouchVelocity : 0.0f);
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

    auto DrawDisplayBubbles = [&](bool isBackFace, bool renderToFBO, GLuint backgroundTexture)
    {
        BindRefractionInputs(backgroundTexture);
        for (const auto &bubble : g_DisplayBubbles)
        {
            glm::mat4 bubbleModel = BubbleModelMatrix(bubble, (float)g_Time);
            SetRefractUniforms(bubbleModel, bubble.radius, isBackFace, renderToFBO, false, 2);
            g_DecorativeBubbleModel->Draw(*g_RefractionShader);
        }
    };

    auto DrawMainBubble = [&](bool isBackFace, bool renderToFBO, GLuint backgroundTexture)
    {
        BindRefractionInputs(backgroundTexture);
        glm::mat4 mainModel = glm::mat4(1.0f);
        SetRefractUniforms(mainModel, g_BubbleRadius, isBackFace, renderToFBO, true, g_IridescenceMode);
        g_RefractModel->Draw(*g_RefractionShader);
    };

    auto SetBackgroundUniforms = [&]()
    {
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

    DrawDisplayBubbles(false, true, g_BackgroundTexture);

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

    DrawDisplayBubbles(false, false, g_MainSceneTexture);
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
    g_CameraDistance = glm::clamp(g_CameraDistance, 3.5f, 20.0f);
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
        g_Sim.surfaceTensionStrength = 15.0f;
        g_Sim.circulationDiffusion = 0.0005f;
        g_Sim.flipSurfaceTensionSign = true;
        break;
    case GLFW_KEY_3:
        g_Sim.surfaceTensionStrength = std::max(1.0f, g_Sim.surfaceTensionStrength - 1.0f);
        std::cout << "SurfaceTensionStrength: " << g_Sim.surfaceTensionStrength << std::endl;
        break;
    case GLFW_KEY_4:
        g_Sim.surfaceTensionStrength = std::min(50.0f, g_Sim.surfaceTensionStrength + 1.0f);
        std::cout << "SurfaceTensionStrength: " << g_Sim.surfaceTensionStrength << std::endl;
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
    g_Camera.setPosition(glm::vec3(0.0f, 0.0f, g_CameraDistance));
    g_Camera.setTarget(glm::vec3(0.0f, 0.0f, 0.0f));

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
    g_DisplayBubbles = {
        {{-4.05f,  1.05f, -2.10f}, 0.42f, 0.2f, 0.20f, 0.11f, 0.55f},
        {{-3.50f, -0.95f, -2.60f}, 0.24f, 1.1f, 0.13f, 0.07f, 0.82f},
        {{-3.10f,  2.55f, -3.15f}, 0.20f, 2.0f, 0.10f, 0.06f, 0.94f},
        {{-2.55f,  0.10f, -1.35f}, 0.34f, 2.8f, 0.16f, 0.09f, 0.72f},
        {{-2.10f, -2.10f, -2.30f}, 0.30f, 3.7f, 0.15f, 0.08f, 0.68f},
        {{-1.45f,  3.05f, -2.90f}, 0.18f, 4.5f, 0.09f, 0.06f, 1.02f},
        {{-1.25f, -1.85f,  0.95f}, 0.24f, 5.4f, 0.11f, 0.07f, 0.78f},
        {{-0.65f,  1.90f, -3.55f}, 0.22f, 6.2f, 0.11f, 0.07f, 0.90f},
        {{-0.55f, -2.75f,  1.20f}, 0.20f, 7.1f, 0.10f, 0.06f, 0.86f},
        {{ 0.45f,  2.85f, -2.55f}, 0.24f, 8.0f, 0.12f, 0.07f, 0.86f},
        {{ 0.85f, -2.05f, -3.15f}, 0.20f, 8.9f, 0.10f, 0.06f, 0.98f},
        {{ 1.25f, -1.45f,  0.85f}, 0.22f, 9.7f, 0.11f, 0.07f, 0.80f},
        {{ 1.35f,  1.55f, -1.75f}, 0.38f, 10.6f, 0.18f, 0.11f, 0.62f},
        {{ 1.85f,  2.95f, -3.45f}, 0.18f, 11.4f, 0.09f, 0.06f, 1.00f},
        {{ 2.25f, -1.95f, -2.05f}, 0.32f, 12.2f, 0.16f, 0.08f, 0.80f},
        {{ 2.75f,  0.35f, -1.55f}, 0.46f, 13.1f, 0.22f, 0.12f, 0.58f},
        {{ 3.35f, -0.85f, -2.85f}, 0.24f, 13.9f, 0.12f, 0.07f, 0.92f},
        {{ 3.95f,  1.15f, -2.35f}, 0.34f, 14.8f, 0.17f, 0.09f, 0.70f}
    };

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
    std::cout << "  X              Reset bubble" << std::endl;
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
    std::cout << "Scene: 1 interactive bubble + " << g_DisplayBubbles.size()
              << " drifting display bubbles" << std::endl;
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
