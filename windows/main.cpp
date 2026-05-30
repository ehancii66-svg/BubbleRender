//
// BubbleRender — Windows desktop OpenGL entry point
// Ported from HarmonyOS napi_init.cpp
//

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "stb_image.h"

#include "render/model.h"
#include "render/camera.h"
#include "render/shader.h"
#include "shader/shader_refraction.h"
#include "shader/shader_background.h"
#include "shader/shader_skybox.h"

#include <iostream>
#include <vector>
#include <string>

// ============================================================
// Global state (mirrors napi_init.cpp globals)
// ============================================================
static int g_WindowWidth = 1280;
static int g_WindowHeight = 720;

static float g_CameraDistance = 7.0f;
static float g_CurrentAngleX = 0.0f; // pitch
static float g_CurrentAngleY = 0.0f; // yaw

static float g_FresnelPower = 8.0f;
static float g_Shininess = 15.0f;
static float g_Diffuseness = 0.2f;
static float g_RefractionStrength = 1.4f;
static float g_EdgeDistortionBoost = 1.7f;
static float g_MaxOffsetRatio = 0.7f;
static float g_EnvironmentReflectionStrength = 0.65f;
static glm::vec3 g_Light = glm::vec3(-1.0f, 1.0f, 1.0f);

static Camera g_Camera;

static Shader *g_RefractionShader = nullptr;
static Shader *g_BackgroundShader = nullptr;
static Shader *g_SkyboxShader = nullptr;

static Model *g_RefractModel = nullptr;
static Model *g_SkyboxModel = nullptr;

static std::vector<Model *> g_BackgroundModels;
static std::vector<glm::vec3> g_SpherePositions;

static GLuint g_BackgroundTexture = 0;
static GLuint g_BackFaceTexture = 0;
static GLuint g_BackgroundFBO = 0;
static GLuint g_BackFaceFBO = 0;
static GLuint g_DepthRB = 0;
static GLuint g_CubemapTexture = 0;

static constexpr float kFBOOverscan = 1.3f;
static int g_FBOWidth = 0;
static int g_FBOHeight = 0;

// Kim2012 iridescence parameters
static float g_Thickness = 350.0f;    // 皂膜厚度 (nm)
static float g_ThicknessVar = 250.0f; // 厚度扰动幅度 (nm)
static double g_Time = 0.0;           // 时间

// ============================================================
// Mouse state
// ============================================================
static bool g_MousePressed = false;
static float g_LastMouseX = 0.0f;
static float g_LastMouseY = 0.0f;

static double g_LastFrameTime = 0.0;

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

    UpdateCamera();

    glm::mat4 model = glm::mat4(1.0f);
    glm::mat4 view = g_Camera.getViewMatrix();
    glm::mat4 proj = g_Camera.getProjectionMatrix();

    auto SetRefractUniforms = [&](bool isBackFace)
    {
        // Compute sphere's screen-space pixel radius for refraction offset scaling
        float dist = glm::length(g_Camera.getPosition());
        float fovVert = glm::radians(g_Camera.getFov());
        float pixelsPerRadian = (h / 2.0f) / tanf(fovVert / 2.0f);
        float angularRadius = atanf(1.5f / dist);
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
        g_RefractionShader->SetInt("uBackgroundTexture", 0);
        g_RefractionShader->SetInt("uEnvironmentMap", 1);
        g_RefractionShader->SetVec3("uLight", g_Light);
        g_RefractionShader->SetVec2("uWinResolution", glm::vec2((float)w, (float)h));
        g_RefractionShader->SetVec2("uFBOSize", glm::vec2((float)g_FBOWidth, (float)g_FBOHeight));
        g_RefractionShader->SetFloat("uThickness", g_Thickness);
        g_RefractionShader->SetFloat("uThicknessVar", g_ThicknessVar);
        g_RefractionShader->SetFloat("uTime", (float)g_Time);
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

    // ========== Pass 2: Render refraction sphere back faces to backFaceFBO ==========
    glBindFramebuffer(GL_FRAMEBUFFER, g_BackFaceFBO);
    glViewport(0, 0, fboW, fboH);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glCullFace(GL_FRONT);

    SetRefractUniforms(true);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_BackgroundTexture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_CUBE_MAP, g_CubemapTexture);
    g_RefractModel->Draw(*g_RefractionShader);

    // ========== Pass 3: Render to screen ==========
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

    SetRefractUniforms(false);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_BackFaceTexture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_CUBE_MAP, g_CubemapTexture);
    g_RefractModel->Draw(*g_RefractionShader);
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
}

static void CursorPosCallback(GLFWwindow *window, double xpos, double ypos)
{
    if (!g_MousePressed)
        return;

    float dx = (float)xpos - g_LastMouseX;
    float dy = (float)ypos - g_LastMouseY;

    g_CurrentAngleY += dx * 0.5f;
    g_CurrentAngleX += dy * 0.5f;

    g_LastMouseX = (float)xpos;
    g_LastMouseY = (float)ypos;
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
    case GLFW_KEY_F:
        g_Diffuseness = std::max(0.0f, g_Diffuseness - 0.05f);
        std::cout << "Diffuseness: " << g_Diffuseness << std::endl;
        break;
    case GLFW_KEY_G:
        g_Diffuseness = std::min(1.0f, g_Diffuseness + 0.05f);
        std::cout << "Diffuseness: " << g_Diffuseness << std::endl;
        break;
    case GLFW_KEY_V:
        g_Shininess = std::max(1.0f, g_Shininess - step);
        std::cout << "Shininess: " << g_Shininess << std::endl;
        break;
    case GLFW_KEY_B:
        g_Shininess = std::min(64.0f, g_Shininess + step);
        std::cout << "Shininess: " << g_Shininess << std::endl;
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
    case GLFW_KEY_I:
        g_MaxOffsetRatio = std::max(0.1f, g_MaxOffsetRatio - 0.05f);
        std::cout << "MaxOffsetRatio: " << g_MaxOffsetRatio << std::endl;
        break;
    case GLFW_KEY_K:
        g_MaxOffsetRatio = std::min(1.2f, g_MaxOffsetRatio + 0.05f);
        std::cout << "MaxOffsetRatio: " << g_MaxOffsetRatio << std::endl;
        break;
    case GLFW_KEY_O:
        g_EnvironmentReflectionStrength = std::max(0.0f, g_EnvironmentReflectionStrength - 0.05f);
        std::cout << "EnvironmentReflectionStrength: " << g_EnvironmentReflectionStrength << std::endl;
        break;
    case GLFW_KEY_P:
        g_EnvironmentReflectionStrength = std::min(1.5f, g_EnvironmentReflectionStrength + 0.05f);
        std::cout << "EnvironmentReflectionStrength: " << g_EnvironmentReflectionStrength << std::endl;
        break;
    case GLFW_KEY_N:
        g_Thickness = std::max(100.0f, g_Thickness - 20.0f);
        std::cout << "Thickness: " << g_Thickness << " nm" << std::endl;
        break;
    case GLFW_KEY_M:
        g_Thickness = std::min(2000.0f, g_Thickness + 20.0f);
        std::cout << "Thickness: " << g_Thickness << " nm" << std::endl;
        break;
    case GLFW_KEY_1:
        g_ThicknessVar = std::max(0.0f, g_ThicknessVar - 20.0f);
        std::cout << "ThicknessVar: " << g_ThicknessVar << " nm" << std::endl;
        break;
    case GLFW_KEY_2:
        g_ThicknessVar = std::min(500.0f, g_ThicknessVar + 20.0f);
        std::cout << "ThicknessVar: " << g_ThicknessVar << " nm" << std::endl;
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
    delete g_SkyboxModel;
    g_RefractModel = g_SkyboxModel = nullptr;

    for (auto *sphere : g_BackgroundModels)
    {
        delete sphere;
    }
    g_BackgroundModels.clear();

    if (g_CubemapTexture)
        glDeleteTextures(1, &g_CubemapTexture);
    if (g_BackgroundFBO)
        glDeleteFramebuffers(1, &g_BackgroundFBO);
    if (g_BackgroundTexture)
        glDeleteTextures(1, &g_BackgroundTexture);
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
                                          "BubbleRender - Iridescence (Kim2012)", nullptr, nullptr);
    if (!window)
    {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // VSync

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
    std::cout << ">>> PAST GLAD INIT" << std::endl << std::flush;

    // -------- Callbacks --------
    std::cout << "Setting callbacks..." << std::endl;
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
    std::cout << "Creating shaders..." << std::endl;
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

    // -------- Debug: verify thin film formula -----
    std::cout << "=== Kim2012 iridescence formula check ===" << std::endl;
    auto cppThinFilm = [](float cosTheta, float thickness, float wavelength)
    {
        float n = 1.33f;
        float sinTheta2 = max(0.0f, 1.0f - cosTheta * cosTheta);
        float sinTheta = sqrt(sinTheta2);
        // Fix: cos(theta_t) = sqrt(n^2 - sin^2(theta_i)) / n
        float cosT = sqrt(max(0.0f, n * n - sinTheta2)) / n;
        float Rs = (cosTheta - cosT) / (cosTheta + cosT);
        float Rp = (n * n * cosTheta - cosT) / (n * n * cosTheta + cosT);
        // Phase uses correct cosT via sinT route
        float sinT = sinTheta / n;
        float cosTphase = sqrt(max(0.0f, 1.0f - sinT * sinT));
        float phi = (4.0f * 3.14159265f * n * thickness * cosTphase) / wavelength;
        float cosPhi = cos(phi);
        float Rs2 = Rs * Rs, Rp2 = Rp * Rp;
        float denom_s = 1.0f + Rs2 * Rs2 - 2.0f * Rs2 * cosPhi;
        float denom_p = 1.0f + Rp2 * Rp2 - 2.0f * Rp2 * cosPhi;
        float rs = (denom_s > 1e-6f) ? Rs2 * (1.0f - cosPhi) / denom_s : 0.0f;
        float rp = (denom_p > 1e-6f) ? Rp2 * (1.0f - cosPhi) / denom_p : 0.0f;
        return rs + rp;
    };
    std::cout << "thickness=" << g_Thickness << "nm, thicknessVar=" << g_ThicknessVar << "nm" << std::endl;
    std::cout << "NdotV=0.9: R=" << cppThinFilm(0.9f, g_Thickness, 615.0f)
              << " G=" << cppThinFilm(0.9f, g_Thickness, 535.0f)
              << " B=" << cppThinFilm(0.9f, g_Thickness, 465.0f) << std::endl;
    std::cout << "NdotV=0.5: R=" << cppThinFilm(0.5f, g_Thickness, 615.0f)
              << " G=" << cppThinFilm(0.5f, g_Thickness, 535.0f)
              << " B=" << cppThinFilm(0.5f, g_Thickness, 465.0f) << std::endl;
    std::cout << "NdotV=0.1: R=" << cppThinFilm(0.1f, g_Thickness, 615.0f)
              << " G=" << cppThinFilm(0.1f, g_Thickness, 535.0f)
              << " B=" << cppThinFilm(0.1f, g_Thickness, 465.0f) << std::endl;
    std::cout << "===========================================" << std::endl;

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

    // -------- Models --------
    g_SkyboxModel = Model::CreateSkyboxCube();

    g_RefractModel = Model::CreateSphere(1.5f, 64, 32, true);

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
        Model *sphere = Model::CreateSphere(sphere_radius, 32, 16, false);
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
    std::cout << "=== Controls ===" << std::endl;
    std::cout << "Mouse drag  : Rotate view" << std::endl;
    std::cout << "Mouse wheel : Zoom in/out" << std::endl;
    std::cout << "R/T : Fresnel power -/+" << std::endl;
    std::cout << "F/G : Diffuseness -/+" << std::endl;
    std::cout << "V/B : Shininess -/+" << std::endl;
    std::cout << "Y/H : Refraction strength -/+" << std::endl;
    std::cout << "U/J : Edge distortion boost -/+" << std::endl;
    std::cout << "I/K : Max offset ratio -/+" << std::endl;
    std::cout << "O/P : Environment reflection -/+" << std::endl;
    std::cout << "N/M : Film thickness -/+ (nm)" << std::endl;
    std::cout << "1/2 : Thickness variation -/+" << std::endl;
    std::cout << "ESC : Quit" << std::endl;
    std::cout << "================" << std::endl;

    while (!glfwWindowShouldClose(window))
    {
        double currentTime = glfwGetTime();
        if (g_LastFrameTime == 0.0)
            g_LastFrameTime = currentTime;
        g_Time += currentTime - g_LastFrameTime;
        g_LastFrameTime = currentTime;

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
