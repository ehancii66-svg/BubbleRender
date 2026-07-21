#include "contact_geometry.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kPi = 3.14159265358979323846f;

float Smooth01Local(float x)
{
    x = glm::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

} // namespace

DiscMeshData BuildContactFilmDisc(int segments, int rings)
{
    DiscMeshData disc;
    disc.positions.reserve(1 + (size_t)segments * (size_t)rings);
    disc.normals.reserve(1 + (size_t)segments * (size_t)rings);
    disc.indices.reserve((size_t)segments * (size_t)(1 + (rings - 1) * 6));

    disc.positions.push_back(glm::vec3(0.0f));
    disc.normals.push_back(glm::vec3(0.0f, 0.0f, 1.0f));

    for (int ring = 1; ring <= rings; ++ring) {
        float radial = (float)ring / (float)rings;
        for (int i = 0; i < segments; ++i) {
            float angle = 2.0f * kPi * (float)i / (float)segments;
            disc.positions.push_back(glm::vec3(std::cos(angle) * radial,
                                               std::sin(angle) * radial,
                                               0.0f));
            disc.normals.push_back(glm::vec3(0.0f, 0.0f, 1.0f));
        }
    }

    for (int i = 0; i < segments; ++i) {
        unsigned int curr = 1u + (unsigned int)i;
        unsigned int next = 1u + (unsigned int)((i + 1) % segments);
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

std::vector<Vertex> BuildCurvedContactFilmVertices(float normalizedCurvature,
                                                   int segments,
                                                   int rings)
{
    std::vector<Vertex> vertices;
    vertices.reserve(1 + (size_t)segments * (size_t)rings);
    normalizedCurvature = glm::clamp(normalizedCurvature, -0.85f, 0.85f);

    auto makeVertex = [&](float x, float y, float radial) {
        Vertex vertex{};
        float z = 0.0f;
        glm::vec3 normal(0.0f, 0.0f, 1.0f);
        float curvatureMagnitude = std::abs(normalizedCurvature);
        if (curvatureMagnitude > 1e-4f) {
            float sign = normalizedCurvature >= 0.0f ? 1.0f : -1.0f;
            float sphereRadius = 1.0f / curvatureMagnitude;
            float boundaryHeight = std::sqrt(std::max(sphereRadius * sphereRadius - 1.0f,
                                                       1e-6f));
            float localHeight = std::sqrt(std::max(sphereRadius * sphereRadius -
                                                   radial * radial, 1e-6f));
            z = sign * (localHeight - boundaryHeight);
            normal = glm::normalize(glm::vec3(sign * x / localHeight,
                                              sign * y / localHeight,
                                              1.0f));
        }
        vertex.Position = glm::vec3(x, y, z);
        vertex.Normal = normal;
        vertex.TexCoords = glm::vec2(x * 0.5f + 0.5f, y * 0.5f + 0.5f);
        vertex.Tangent = glm::vec3(0.0f);
        vertex.Bitangent = glm::vec3(0.0f);
        return vertex;
    };

    vertices.push_back(makeVertex(0.0f, 0.0f, 0.0f));
    for (int ring = 1; ring <= rings; ++ring) {
        float radial = (float)ring / (float)rings;
        for (int i = 0; i < segments; ++i) {
            float angle = 2.0f * kPi * (float)i / (float)segments;
            float x = std::cos(angle) * radial;
            float y = std::sin(angle) * radial;
            vertices.push_back(makeVertex(x, y, radial));
        }
    }
    return vertices;
}

std::vector<unsigned int> BuildContactBubblePatchIndices(int segments, int rings)
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

std::vector<Vertex> BuildBubbleShellVertices(float radius,
                                             float contactRadius,
                                             bool hasContact,
                                             int segments,
                                             int rings)
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
        float theta = kPi + (thetaCap - kPi) * Smooth01Local(v);
        float x = std::cos(theta);
        float yz = std::sin(theta);
        for (int i = 0; i <= segments; ++i) {
            float u = (float)i / (float)segments;
            float phi = 2.0f * kPi * u;
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

std::vector<Vertex> BuildContactBubblePatchVertices(float radius,
                                                    float capPlaneOffset,
                                                    int segments,
                                                    int rings)
{
    float contactRadius = std::sqrt(std::max(radius * radius - capPlaneOffset * capPlaneOffset, 0.0f));
    return BuildBubbleShellVertices(radius, contactRadius, true, segments, rings);
}
