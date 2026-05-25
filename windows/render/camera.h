//
// Desktop OpenGL version — cleaned up from HarmonyOS original
//

#ifndef WINDOWS_CAMERA_H
#define WINDOWS_CAMERA_H

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

class Camera {
public:
    Camera();
    ~Camera();

    void setPosition(const glm::vec3& pos);
    void setTarget(const glm::vec3& target);
    void setUp(const glm::vec3& up);
    void setFov(float fovDegree);
    void setNearFar(float nearPlane, float farPlane);
    void setViewportSize(float width, float height);

    const glm::vec3& getPosition() const { return m_position; }
    const glm::vec3& getTarget() const { return m_target; }
    const glm::vec3& getUp() const { return m_up; }
    float getFov() const { return m_fovDeg; }
    float getAspect() const { return m_aspect; }
    float getNear() const { return m_near; }
    float getFar() const { return m_far; }

    const glm::mat4& getViewMatrix() const { return m_view; }
    const glm::mat4& getProjectionMatrix() const { return m_proj; }
    glm::mat4 getViewProjectionMatrix() const { return m_proj * m_view; }
    glm::mat4 getMVP(const glm::mat4& model) const { return m_proj * m_view * model; }

    glm::vec3 getRayDirectionFromScreen(float screenX, float screenY) const;

private:
    void updateViewMatrix();
    void updateProjectionMatrix();

    glm::vec3 m_position;
    glm::vec3 m_target;
    glm::vec3 m_up;

    float m_fovDeg;
    float m_aspect;
    float m_near;
    float m_far;

    float m_width;
    float m_height;

    glm::mat4 m_view;
    glm::mat4 m_proj;
};

#endif // WINDOWS_CAMERA_H
