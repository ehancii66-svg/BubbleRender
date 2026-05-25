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

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

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
GLuint backFaceFBO = 0, backFaceTexture = 0;
GLuint depthRB = 0;
GLuint cubemapTexture = 0;

Camera g_camera;

Model* g_RefractModel;
Model* g_SkyboxModel;
std::vector<Model*> g_BackgroundModels;
std::vector<glm::vec3> g_SpherePositions;

float g_FresnelPower = 8.0f;
float g_Shininess = 15.0f;
float g_Diffuseness = 0.2f;
glm::vec3 light{-1.0f, 1.0f, 1.0f};

float g_LastTouchX = 0.0f, g_LastTouchY = 0.0f;


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
    glGenRenderbuffers(1, &depthRB);
    glBindRenderbuffer(GL_RENDERBUFFER, depthRB);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);

    // ---------- backgroundFBO ----------
    glGenTextures(1, &backgroundTexture);
    glBindTexture(GL_TEXTURE_2D, backgroundTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &backgroundFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, backgroundFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, backgroundTexture, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRB);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        OH_LOG_ERROR(LOG_APP, "Back FBO incomplete!");
    }

    // ---------- backFaceFBO ----------
    glGenTextures(1, &backFaceTexture);
    glBindTexture(GL_TEXTURE_2D, backFaceTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &backFaceFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, backFaceFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, backFaceTexture, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRB);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        OH_LOG_ERROR(LOG_APP, "Main FBO incomplete!");
    }

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

    refractionShader = new Shader(refractionVertexShader, refractionFragmentShader);
    backgroundShader = new Shader(backgroundVertexShader, backgroundFragmentShader);
    skyboxShader = new Shader (skyboxVertexShader, skyboxFragmentShader);
    if (refractionShader->m_id == 0 || backgroundShader->m_id == 0 || skyboxShader->m_id == 0){
        OH_LOG_ERROR(LOG_APP, "create shader failed");
        return nullptr;
    }
    
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
    
    g_SkyboxModel = Model::CreateSkyboxCube();
    if (!g_SkyboxModel) {
        OH_LOG_ERROR(LOG_APP, "create skybox failed");
        return nullptr;
    }
    
    g_RefractModel = Model::CreateSphere(1.5f, 64, 32, true);
    if (!g_RefractModel) {
        OH_LOG_ERROR(LOG_APP, "load refract model failed");
        return nullptr;
    }
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
        if (sphere) {
            g_BackgroundModels.push_back(sphere);
        }
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
    width = static_cast<float>(surfW);
    height = static_cast<float>(surfH);
    
    g_camera.setViewportSize(width, height);
    
    // rotate camera
    float radius = g_CameraDistance;
    float pitch = glm::radians(currentAngleX);
    float yaw = glm::radians(currentAngleY);
    const float maxPitch = glm::radians(89.0f);
    pitch = glm::clamp(pitch, -maxPitch, maxPitch);
    glm::vec3 cameraPos;
    cameraPos.x = radius * cos(pitch) * sin(yaw);
    cameraPos.y = radius * sin(pitch);
    cameraPos.z = radius * cos(pitch) * cos(yaw);
    g_camera.setPosition(cameraPos);
    g_camera.setTarget(glm::vec3(0.0f, 0.0f, 0.0f));
    glm::mat4 model = glm::mat4(1.0f);
    
//    // rotate model
//    glm::vec3 cameraPos = g_camera.getPosition();
//    glm::mat4 rotateModel = glm::rotate(glm::mat4(1.0f), glm::radians(currentAngleX), glm::vec3(1.0f, 0.0f, 0.0f));
//    rotateModel = glm::rotate(rotateModel, glm::radians(currentAngleY), glm::vec3(0.0f, 1.0f, 0.0f));
//    glm::mat4 model = rotateModel * glm::mat4(1.0f);
    
    glm::mat4 view = g_camera.getViewMatrix();
    glm::mat4 proj = g_camera.getProjectionMatrix();
//    glm::mat4 mvp = g_camera.getMVP(model);
    
    auto SetRefractUniforms = [&]() {
        refractionShader->Use();
        refractionShader->SetMat4("uModel", model);
        refractionShader->SetMat4("uView", view);
        refractionShader->SetMat4("uProj", proj);
        refractionShader->SetVec3("uCameraPos", cameraPos);

        refractionShader->SetFloat("uFresnelPower", g_FresnelPower);
        refractionShader->SetFloat("uShininess", g_Shininess);
        refractionShader->SetFloat("uDiffuseness", g_Diffuseness);
        refractionShader->SetVec3("uLight", light);
        refractionShader->SetVec2("uWinResolution", {width, height});
    };
    
    auto SetBackgroundUniforms = [&]() {
        backgroundShader->Use();
        for (size_t i = 0; i < g_BackgroundModels.size(); ++i){
            glm::mat4 model = glm::translate(glm::mat4(1.0f), g_SpherePositions[i]);
            glm::mat4 mvp = g_camera.getMVP(model);
            backgroundShader->SetMat4("uMVP", mvp);
            g_BackgroundModels[i]->Draw(*backgroundShader);
        }
    };
    
    auto DrawSkybox = [&]() {
        skyboxShader->Use();
        glm::mat4 viewNoTranslation = glm::mat4(glm::mat3(g_camera.getViewMatrix()));
        glm::mat4 projection = g_camera.getProjectionMatrix();
        skyboxShader->SetMat4("projection", projection);
        skyboxShader->SetMat4("view", viewNoTranslation);
    
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_CUBE_MAP, cubemapTexture);
        skyboxShader->SetInt("skybox", 0);
    
        g_SkyboxModel->Draw(*skyboxShader);
    };

    // ---------- 1. 渲染背景到 backgroundFBO ----------
    glBindFramebuffer(GL_FRAMEBUFFER, backgroundFBO);
    glViewport(0, 0, surfW, surfH);
    glEnable(GL_DEPTH_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);  // 黑色背景
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    
    glCullFace(GL_BACK);
    
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    DrawSkybox();
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
    
    SetBackgroundUniforms();

    // ---------- 2. 渲染折射物体背面到 backFaceFBO ----------
    glBindFramebuffer(GL_FRAMEBUFFER, backFaceFBO);
    glViewport(0, 0, surfW, surfH);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glCullFace(GL_FRONT);

    SetRefractUniforms();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, backgroundTexture);
    glUniform1i(glGetUniformLocation(refractionShader->m_id, "uBackgroundTexture"), 0);
    g_RefractModel->Draw(*refractionShader);

    // ---------- 3. 渲染折射物体正面到屏幕 ----------
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, surfW, surfH);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    
    glCullFace(GL_BACK);
    
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    DrawSkybox();
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
    
    SetBackgroundUniforms();

    SetRefractUniforms();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, backFaceTexture);
//    glBindTexture(GL_TEXTURE_2D, backgroundTexture);
    glUniform1i(glGetUniformLocation(refractionShader->m_id, "uBackgroundTexture"), 0);
    g_RefractModel->Draw(*refractionShader);

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
        g_LastTouchX = fx;
        g_LastTouchY = fy;
    }else if (touchType == 2) { // Move
        float dx = fx - g_LastTouchX;
        float dy = fy - g_LastTouchY;
        currentAngleY += dx * 0.5f;
        currentAngleX += dy * 0.5f;
        g_LastTouchX = fx;
        g_LastTouchY = fy;
    }

    return nullptr;
}

static napi_value ReleaseResource(napi_env env, napi_callback_info info) {
    delete g_RefractModel, g_SkyboxModel;
    g_RefractModel = g_SkyboxModel = nullptr;
    for (auto* sphere : g_BackgroundModels){
        delete sphere;
    }
    g_BackgroundModels.clear();
    
    if (cubemapTexture) glDeleteTextures(1, &cubemapTexture);

    if (backgroundFBO) glDeleteFramebuffers(1, &backgroundFBO);
    if (backgroundTexture) glDeleteTextures(1, &backgroundTexture);
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