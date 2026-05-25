//
// Created on 2026/3/25.
//
// Node APIs are not fully supported. To solve the compilation error of the interface cannot be found,
// please include "napi/native_api.h".

#include "camera.h"

Camera::Camera(): m_position(0.0f, 0.0f, 10.0f),
    m_target(0.0f, 0.0f, 0.0f),
    m_up(0.0f, 1.0f, 0.0f),
    m_fovDeg(60.0f),
    m_aspect(0.45f), //2.21
    m_near(0.1f),
    m_far(100.0f),
    m_width(1280.0f),
    m_height(2832.0f)
{
    updateViewMatrix();
    updateProjectionMatrix();
}

Camera::~Camera(){}

void Camera::setPosition(const glm::vec3& pos) {
    m_position = pos;
    updateViewMatrix();
}

void Camera::setTarget(const glm::vec3& target) {
    m_target = target;
    updateViewMatrix();
}

void Camera::setUp(const glm::vec3& up) {
    m_up = up;
    updateViewMatrix();
}

void Camera::setFov(float fovDegree) {
    m_fovDeg = fovDegree;
    updateProjectionMatrix();
}

void Camera::setNearFar(float nearPlane, float farPlane) {
    m_near = nearPlane;
    m_far = farPlane;
    updateProjectionMatrix();
}

void Camera::setViewportSize(float width, float height) {
    m_width = width;
    m_height = height;
    m_aspect = width / height;
    updateProjectionMatrix();
}

void Camera::updateViewMatrix() {
    m_view = glm::lookAt(m_position, m_target, m_up);
}

void Camera::updateProjectionMatrix() {
    m_proj = glm::perspective(glm::radians(m_fovDeg), m_aspect, m_near, m_far);
}

glm::vec3 Camera::getRayDirectionFromScreen(float screenX, float screenY) const {
    float ndcX = (2.0f * screenX) / m_width - 1.0f;
    float ndcY = 1.0 - (2.0f * screenY) / m_height; //?
    
    glm::vec4 rayClip(ndcX, ndcY, -1.0f, 1.0f);
    glm::vec4 rayEye = glm::inverse(m_proj) * rayClip;
    rayEye = glm::vec4(rayEye.x, rayEye.y, -1.0f, 0.0f);
    glm::vec3 rayDirWorld = glm::normalize(glm::vec3(glm::inverse(m_view) * rayEye));
    
    return rayDirWorld;
}