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
#include "simulation/vortex_sheet.h"

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <string>
#include <utility>
#include <vector>
#include <fstream>
#include <filesystem>
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
float g_CameraDistance = 7.0f;

Shader* refractionShader = nullptr;
Shader* backgroundShader = nullptr;
Shader* skyboxShader = nullptr;

GLuint backgroundFBO = 0, backgroundTexture = 0;
GLuint sceneBehindFBO = 0, sceneBehindTexture = 0;
GLuint mainSceneFBO = 0, mainSceneTexture = 0;
GLuint backFaceFBO = 0, backFaceTexture = 0;
GLuint depthRB = 0;
GLuint cubemapTexture = 0;

Camera g_camera;

Model* g_RefractModel;
Model* g_DecorativeBubbleModel;
Model* g_SkyboxModel;
std::vector<Model*> g_BackgroundModels;
std::vector<glm::vec3> g_SpherePositions;

struct DisplayBubble {
    glm::vec3 basePosition;
    float radius;
    float phase;
    float windAmplitude;
    float floatAmplitude;
    float speed;
};

static std::vector<DisplayBubble> g_DisplayBubbles;

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

// ---- FBO overscan ----
static constexpr float kFBOOverscan = 1.3f;
int g_FBOWidth = 0, g_FBOHeight = 0;

// ---- Thin-film LUT texture ----
GLuint g_ThinFilmLUTTexture = 0;

// ---- DBSTT simulation ----
static VortexSheetSimulation g_Sim;
static float g_BubbleRadius = 1.5f;
static int   g_SimSubdivs  = 3;
static float g_SimPerturb  = 0.10f;
static bool  g_SimPaused   = false;

static std::vector<Vertex> buildVerticesFromSim(const VortexSheetSimulation& sim) {
    const auto& pos = sim.getPositions();
    const auto& nrm = sim.getNormals();
    std::vector<Vertex> verts(pos.size());
    for (size_t i = 0; i < pos.size(); ++i) {
        verts[i].Position  = pos[i];
        verts[i].Normal    = nrm[i];
        verts[i].TexCoords = glm::vec2(0.0f);
        verts[i].Tangent   = glm::vec3(0.0f);
        verts[i].Bitangent = glm::vec3(0.0f);
    }
    return verts;
}

static float& CurrentThickness() {
    return (g_IridescenceMode == 2) ? g_AiryThickness : g_KimLutThickness;
}

static glm::mat4 BubbleModelMatrix(const DisplayBubble& bubble, float time) {
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

void InitFBO(int w, int h) {
    g_FBOWidth  = static_cast<int>(w * kFBOOverscan);
    g_FBOHeight = static_cast<int>(h * kFBOOverscan);
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
    CreateColorTarget(backFaceFBO, backFaceTexture, "Back-face");

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static napi_value InitGraphics(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    g_ResourceManager = OH_ResourceManager_InitNativeResourceManager(env, args[0]);
    if (g_ResourceManager == nullptr) return nullptr;

    size_t strSize = 0;
    char strBuf[256] = {0};
    if (napi_get_value_string_utf8(env, args[1], strBuf, sizeof(strBuf), &strSize) != napi_ok) return nullptr;
    std::string surfaceIdStr(strBuf);
    if (surfaceIdStr.empty()) return nullptr;
    uint64_t surfaceId = std::stoull(surfaceIdStr);
    OH_NativeWindow_CreateNativeWindowFromSurfaceId(surfaceId, &nativeWindow);
    if (nativeWindow == nullptr) return nullptr;
    
    char filesDirBuf[512] = {0};
    if (napi_get_value_string_utf8(env, args[2], filesDirBuf, sizeof(filesDirBuf), &strSize) != napi_ok) return nullptr;
    std::string filesDir(filesDirBuf);
    OH_LOG_INFO(LOG_APP, "filesDir = %{public}s", filesDir.c_str());

    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) return nullptr;

    eglInitialize(display, nullptr, nullptr);

    EGLConfig config;
    EGLint numConfig = 0;
    EGLint attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_NONE
    };
    eglChooseConfig(display, attribs, &config, 1, &numConfig);

    EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
    surface = eglCreateWindowSurface(display, config, (EGLNativeWindowType)nativeWindow, nullptr);
    if (eglMakeCurrent(display, surface, surface, context) == EGL_FALSE) return nullptr;

    EGLint surfW = 0, surfH = 0;
    eglQuerySurface(display, surface, EGL_WIDTH, &surfW);
    eglQuerySurface(display, surface, EGL_HEIGHT, &surfH);
    if (surfW > 0 && surfH > 0) {
        width = static_cast<float>(surfW);
        height = static_cast<float>(surfH);
        OH_LOG_INFO(LOG_APP, "get surfW and surfH success");
    }
    
    g_camera.setViewportSize(width, height);
    g_camera.setPosition({0.0f, 0.0f, g_CameraDistance});

    // Shader: concatenate common + main for the fragment shader
    std::string fsFull = std::string(refractionFragmentCommon) + "\n" + std::string(refractionFragmentMain);
    refractionShader = new Shader(refractionVertexShader, fsFull.c_str());
    backgroundShader = new Shader(backgroundVertexShader, backgroundFragmentShader);
    skyboxShader = new Shader(skyboxVertexShader, skyboxFragmentShader);
    if (refractionShader->m_id == 0 || backgroundShader->m_id == 0 || skyboxShader->m_id == 0){
        OH_LOG_ERROR(LOG_APP, "create shader failed");
        return nullptr;
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
        return nullptr;
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
        return nullptr;
    }

    // ---- Decorative bubble model (smooth sphere) ----
    g_DecorativeBubbleModel = Model::CreateSphere(1.0f, 64, 32, true);
    if (!g_DecorativeBubbleModel) {
        OH_LOG_ERROR(LOG_APP, "create decorative bubble model failed");
        return nullptr;
    }

    // 18 display bubbles distributed around the scene
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

    // ---- DBSTT simulation init ----
    g_Sim.initIcosphere(g_BubbleRadius, g_SimSubdivs, g_SimPerturb);
    g_Sim.timeStep = 0.001f;
    g_Sim.substepsPerFrame = 5;
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
        return nullptr;
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
        Model* sphere = Model::CreateSphere(sphere_radius, 32, 16, false);
        if (sphere) g_BackgroundModels.push_back(sphere);
    }

    InitFBO(surfW, surfH);

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
    g_Time += dt;

    // ---- Touch decay ----
    if (!g_TouchPressed) {
        g_TouchStrength *= expf(-3.2f * (float)dt);
        g_TouchVelocity  *= expf(-5.0f * (float)dt);
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
    glm::vec3 cameraPos(radius * cos(pitch) * sin(yaw),
                        radius * sin(pitch),
                        radius * cos(pitch) * cos(yaw));
    g_camera.setPosition(cameraPos);
    g_camera.setTarget(glm::vec3(0.0f, 0.0f, 0.0f));

    // ---- DBSTT simulation ----
    if (!g_SimPaused) g_Sim.update(1.0f / 60.0f);
    if (g_RefractModel && g_RefractModel->getMeshCount() > 0) {
        auto verts = buildVerticesFromSim(g_Sim);
        Mesh* m = g_RefractModel->getMesh(0);
        if (m) m->updateVertices(verts);
    }

    glm::mat4 view = g_camera.getViewMatrix();
    glm::mat4 proj = g_camera.getProjectionMatrix();

    // ---- SetRefractUniforms: per-bubble with individual model matrix ----
    auto SetRefractUniforms = [&](const glm::mat4& model, float localRadius, bool isBackFace,
                                  bool renderToFBO, bool interactive, int iridescenceMode,
                                  float outputAlpha) {
        glm::vec3 center = glm::vec3(model * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        float dist = glm::length(cameraPos - center);
        float fovVert = glm::radians(g_camera.getFov());
        float pixelsPerRadian = (h / 2.0f) / tanf(fovVert / 2.0f);
        float angularRadius = atanf((localRadius * 1.15f) / std::max(dist, 0.001f));
        float spherePixelRadius = angularRadius * pixelsPerRadian;

        refractionShader->Use();
        refractionShader->SetMat4("uModel", model);
        refractionShader->SetMat4("uView", view);
        refractionShader->SetMat4("uProj", proj);
        refractionShader->SetVec3("uCameraPos", cameraPos);
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
        refractionShader->SetVec2("uWinResolution", {w, h});
        refractionShader->SetVec2("uFBOSize", {(float)g_FBOWidth, (float)g_FBOHeight});
        refractionShader->SetFloat("uThickness", CurrentThickness());
        refractionShader->SetFloat("uThicknessVar", g_ThicknessVar);
        refractionShader->SetFloat("uTime", (float)g_Time);
        refractionShader->SetFloat("uOutputAlpha", outputAlpha);
        refractionShader->SetVec2("uTouchPoint", interactive ? g_TouchPoint : glm::vec2(-10.0f, -10.0f));
        refractionShader->SetFloat("uTouchStrength", interactive ? g_TouchStrength : 0.0f);
        refractionShader->SetFloat("uTouchVelocity", interactive ? g_TouchVelocity : 0.0f);
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

        std::vector<std::pair<float, const DisplayBubble*>> sortedBubbles;
        sortedBubbles.reserve(g_DisplayBubbles.size());
        for (const auto& bubble : g_DisplayBubbles) {
            glm::vec3 center = glm::vec3(BubbleModelMatrix(bubble, (float)g_Time)
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
            glm::mat4 bubbleModel = BubbleModelMatrix(bubble, (float)g_Time);
            float alpha = straightComposite ? 0.72f : 1.0f;
            SetRefractUniforms(bubbleModel, bubble.radius, isBackFace, renderToFBO,
                               false, 2, alpha);
            g_DecorativeBubbleModel->Draw(*refractionShader);
        }

        if (straightComposite) {
            glDepthMask(GL_TRUE);
            if (!blendWasEnabled) glDisable(GL_BLEND);
        }
    };

    auto DrawMainBubble = [&](bool isBackFace, bool renderToFBO, GLuint backgroundTex) {
        BindRefractionInputs(backgroundTex);
        glm::mat4 mainModel = glm::mat4(1.0f);
        SetRefractUniforms(mainModel, g_BubbleRadius, isBackFace, renderToFBO,
                           true, g_IridescenceMode, 1.0f);
        g_RefractModel->Draw(*refractionShader);
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

    // ===== Pass 3: Render main bubble back faces =====
    // The main bubble samples the scene behind it, including decorative bubbles.
    glBindFramebuffer(GL_FRAMEBUFFER, backFaceFBO);
    glViewport(0, 0, fboW, fboH);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glCullFace(GL_FRONT);
    DrawMainBubble(true, true, sceneBehindTexture);

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
    DrawMainBubble(false, true, backFaceTexture);

    // ===== Pass 5: Composite front faces to screen =====
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, surfW, surfH);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glCullFace(GL_BACK);
    glDepthFunc(GL_LEQUAL); glDepthMask(GL_FALSE);
    DrawSkybox();
    glDepthMask(GL_TRUE); glDepthFunc(GL_LESS);
    SetBackgroundUniforms();
    DrawMainBubble(false, false, backFaceTexture);
    DrawDisplayBubbles(false, false, mainSceneTexture, true);

    eglSwapBuffers(display, surface);
    return nullptr;
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
    g_CameraDistance = glm::clamp((float)val, 3.5f, 20.0f);
    return nullptr;
}
static napi_value ToggleSimulation(napi_env env, napi_callback_info info) {
    g_SimPaused = !g_SimPaused;
    OH_LOG_INFO(LOG_APP, "Simulation %{public}s", g_SimPaused ? "PAUSED" : "RUNNING");
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
static napi_value ResetSimulation(napi_env env, napi_callback_info info) {
    g_Sim.initIcosphere(g_BubbleRadius, g_SimSubdivs, g_SimPerturb);
    g_Sim.timeStep = 0.001f;
    g_Sim.substepsPerFrame = 5;
    g_Sim.surfaceTensionStrength = 15.0f;
    g_Sim.circulationDiffusion = 0.0005f;
    g_Sim.flipSurfaceTensionSign = true;
    return nullptr;
}

static napi_value ReleaseResource(napi_env env, napi_callback_info info) {
    delete g_RefractModel, g_DecorativeBubbleModel, g_SkyboxModel;
    g_RefractModel = g_DecorativeBubbleModel = g_SkyboxModel = nullptr;
    for (auto* sphere : g_BackgroundModels){
        delete sphere;
    }
    g_BackgroundModels.clear();
    
    if (cubemapTexture) glDeleteTextures(1, &cubemapTexture);
    if (g_ThinFilmLUTTexture) glDeleteTextures(1, &g_ThinFilmLUTTexture);

    if (backgroundFBO) glDeleteFramebuffers(1, &backgroundFBO);
    if (backgroundTexture) glDeleteTextures(1, &backgroundTexture);
    if (sceneBehindFBO) glDeleteFramebuffers(1, &sceneBehindFBO);
    if (sceneBehindTexture) glDeleteTextures(1, &sceneBehindTexture);
    if (mainSceneFBO) glDeleteFramebuffers(1, &mainSceneFBO);
    if (mainSceneTexture) glDeleteTextures(1, &mainSceneTexture);
    if (backFaceFBO) glDeleteFramebuffers(1, &backFaceFBO);
    if (backFaceTexture) glDeleteTextures(1, &backFaceTexture);
    if (depthRB) glDeleteRenderbuffers(1, &depthRB);

    delete refractionShader;
    delete backgroundShader;
    delete skyboxShader;
    refractionShader = backgroundShader = skyboxShader = nullptr;

    if (display != EGL_NO_DISPLAY) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        eglTerminate(display);
        display = EGL_NO_DISPLAY;
    }

    if (nativeWindow) {
        OH_NativeWindow_DestroyNativeWindow(nativeWindow);
        nativeWindow = nullptr;
    }

    if (g_ResourceManager) {
        OH_ResourceManager_ReleaseNativeResourceManager(g_ResourceManager);
        g_ResourceManager = nullptr;
    }

    return nullptr;
}

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        { "initGraphics", nullptr, InitGraphics, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "renderFrame", nullptr, RenderFrame, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "onTouch", nullptr, OnTouch, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setInputViewport", nullptr, SetInputViewport, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "releaseResource", nullptr, ReleaseResource, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setIridescenceMode", nullptr, SetIridescenceMode, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setSurfaceTension", nullptr, SetSurfaceTension, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setRefractionStrength", nullptr, SetRefractionStrength, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "resetSimulation", nullptr, ResetSimulation, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "rotateCamera", nullptr, RotateCamera, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setFresnelPower", nullptr, SetFresnelPower, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setEdgeDistortionBoost", nullptr, SetEdgeDistortionBoost, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setMaxOffsetRatio", nullptr, SetMaxOffsetRatio, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setEnvironmentReflectionStrength", nullptr, SetEnvironmentReflectionStrength, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setCameraDistance", nullptr, SetCameraDistance, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "toggleSimulation", nullptr, ToggleSimulation, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setThickness", nullptr, SetThickness, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setThicknessVar", nullptr, SetThicknessVar, nullptr, nullptr, nullptr, napi_default, nullptr },
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
