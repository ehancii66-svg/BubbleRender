
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

glm::vec3 SafeNormalize(const glm::vec3& value, const glm::vec3& fallback)
{
    float lengthSquared = glm::dot(value, value);
    if (lengthSquared <= 1e-10f) {
        return fallback;
    }
    return value / std::sqrt(lengthSquared);
}

} // namespace

DiscMeshData BuildContactFilmDisc(int segments, int rings)
{
    DiscMeshData disc;
    disc.positions.reserve((size_t)(segments + 1) * (size_t)(rings + 1));
    disc.normals.reserve((size_t)(segments + 1) * (size_t)(rings + 1));
    disc.indices.reserve((size_t)segments * (size_t)rings * 6);

    for (int ring = 0; ring <= rings; ++ring) {
        float radial = (float)ring / (float)rings;
        for (int i = 0; i <= segments; ++i) {
            float angle = 2.0f * kPi * (float)i / (float)segments;
            disc.positions.push_back(glm::vec3(std::cos(angle) * radial,
                                               std::sin(angle) * radial,
                                               0.0f));
            disc.normals.push_back(glm::vec3(0.0f, 0.0f, 1.0f));
        }
    }

    for (int ring = 0; ring < rings; ++ring) {
        unsigned int innerStart = (unsigned int)ring * (unsigned int)(segments + 1);
        unsigned int outerStart = (unsigned int)(ring + 1) * (unsigned int)(segments + 1);
        for (int i = 0; i < segments; ++i) {
            unsigned int inner0 = innerStart + (unsigned int)i;
            unsigned int inner1 = innerStart + (unsigned int)i + 1u;
            unsigned int outer0 = outerStart + (unsigned int)i;
            unsigned int outer1 = outerStart + (unsigned int)i + 1u;

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
                                                   float innerRadius,
                                                   int segments,
                                                   int rings)
{
    std::vector<Vertex> vertices;
    vertices.reserve((size_t)(segments + 1) * (size_t)(rings + 1));
    normalizedCurvature = glm::clamp(normalizedCurvature, -0.85f, 0.85f);
    innerRadius = glm::clamp(innerRadius, 0.0f, 0.98f);

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

    for (int ring = 0; ring <= rings; ++ring) {
        float ringProgress = (float)ring / (float)rings;
        float radial = glm::mix(innerRadius, 1.0f, ringProgress);
        for (int i = 0; i <= segments; ++i) {
            float angle = 2.0f * kPi * (float)i / (float)segments;
            float x = std::cos(angle) * radial;
            float y = std::sin(angle) * radial;
            vertices.push_back(makeVertex(x, y, radial));
        }
    }
    return vertices;
}

std::vector<unsigned int> BuildFusionSurfaceIndices(int segments, int rings)
{
    return BuildContactBubblePatchIndices(segments, rings);
}

std::vector<Vertex> BuildFusionSurfaceVertices(const FusionSurfaceParameters& parameters,
                                               int segments,
                                               int rings)
{
    float centerA = parameters.centerA;
    float radiusA = parameters.radiusA;
    float centerB = parameters.centerB;
    float radiusB = parameters.radiusB;
    float targetRadius = parameters.targetRadius;
    float neckProgress = parameters.neckProgress;
    float relaxationProgress = parameters.relaxationProgress;
    float oscillation = parameters.oscillation;
    radiusA = std::max(radiusA, 0.001f);
    radiusB = std::max(radiusB, 0.001f);
    targetRadius = std::max(targetRadius, 0.001f);
    neckProgress = Smooth01Local(neckProgress);
    relaxationProgress = Smooth01Local(relaxationProgress);

    if (centerA > centerB) {
        std::swap(centerA, centerB);
        std::swap(radiusA, radiusB);
    }

    float sourceLeft = std::min(centerA - radiusA, centerB - radiusB);
    float sourceRight = std::max(centerA + radiusA, centerB + radiusB);
    float minRadius = std::min(radiusA, radiusB);
    float contactCenter = 0.5f * ((centerA + radiusA) + (centerB - radiusB));
    float bridgeHalfWidth = minRadius * glm::mix(0.12f, 0.92f, neckProgress);
    float neckRadius = minRadius * glm::mix(0.025f, 0.78f,
                                            std::pow(neckProgress, 0.65f));

    std::vector<float> axial((size_t)rings + 1u);
    std::vector<float> radial((size_t)rings + 1u);
    std::vector<float> sourceAxial((size_t)rings + 1u);
    for (int ring = 0; ring <= rings; ++ring) {
        float v = (float)ring / (float)rings;
        float sourceX = glm::mix(sourceLeft, sourceRight, v);
        float targetTheta = kPi * (1.0f - Smooth01Local(v));
        float targetX = std::cos(targetTheta) * targetRadius;
        float x = glm::mix(sourceX, targetX, relaxationProgress);
        float rhoASq = radiusA * radiusA - (x - centerA) * (x - centerA);
        float rhoBSq = radiusB * radiusB - (x - centerB) * (x - centerB);
        float sourceRho = std::max(std::sqrt(std::max(rhoASq, 0.0f)),
                                   std::sqrt(std::max(rhoBSq, 0.0f)));

        float bridgeDistance = std::abs(x - contactCenter) /
                               std::max(bridgeHalfWidth, 0.001f);
        float bridgeShape = bridgeDistance < 1.0f
            ? 1.0f - Smooth01Local(bridgeDistance)
            : 0.0f;
        sourceRho = std::max(sourceRho, neckRadius * bridgeShape);

        float targetRhoSq = targetRadius * targetRadius - targetX * targetX;
        float targetRho = std::sqrt(std::max(targetRhoSq, 0.0f));
        float rho = glm::mix(sourceRho, targetRho, relaxationProgress);

        float normalizedX = x / std::max(targetRadius, 0.001f);
        float quadrupole = 0.5f * (3.0f * normalizedX * normalizedX - 1.0f);
        rho *= std::max(0.0f, 1.0f - oscillation * quadrupole);
        x *= 1.0f + oscillation;

        axial[(size_t)ring] = x;
        radial[(size_t)ring] = ring == 0 || ring == rings ? 0.0f : rho;
        sourceAxial[(size_t)ring] = sourceX;
    }

    float volumeIntegral = 0.0f;
    for (int ring = 1; ring <= rings; ++ring) {
        float dx = std::max(axial[(size_t)ring] - axial[(size_t)ring - 1u], 0.0f);
        float rho0 = radial[(size_t)ring - 1u];
        float rho1 = radial[(size_t)ring];
        volumeIntegral += 0.5f * (rho0 * rho0 + rho1 * rho1) * dx;
    }
    if (volumeIntegral > 1e-6f) {
        float targetIntegral = (4.0f / 3.0f) * targetRadius * targetRadius * targetRadius;
        float radialVolumeScale = glm::mix(
            std::sqrt(targetIntegral / volumeIntegral),
            1.0f,
            relaxationProgress);
        for (int ring = 1; ring < rings; ++ring) {
            radial[(size_t)ring] *= radialVolumeScale;
        }
    }

    std::vector<Vertex> vertices;
    vertices.reserve((size_t)(rings + 1) * (size_t)(segments + 1));
    for (int ring = 0; ring <= rings; ++ring) {
        float dx = 1.0f;
        float dr = 0.0f;
        if (ring == 0) {
            dx = axial[1] - axial[0];
            dr = radial[1] - radial[0];
        } else if (ring == rings) {
            dx = axial[(size_t)rings] - axial[(size_t)rings - 1u];
            dr = radial[(size_t)rings] - radial[(size_t)rings - 1u];
        } else {
            dx = axial[(size_t)ring + 1u] - axial[(size_t)ring - 1u];
            dr = radial[(size_t)ring + 1u] - radial[(size_t)ring - 1u];
        }
        float slope = dr / std::max(std::abs(dx), 1e-5f);

        for (int segment = 0; segment <= segments; ++segment) {
            float u = (float)segment / (float)segments;
            float angle = 2.0f * kPi * u;
            float cosine = std::cos(angle);
            float sine = std::sin(angle);

            Vertex vertex{};
            vertex.Position = glm::vec3(axial[(size_t)ring],
                                        radial[(size_t)ring] * cosine,
                                        radial[(size_t)ring] * sine);
            glm::vec3 geometricNormal;
            if (ring == 0) {
                geometricNormal = glm::vec3(-1.0f, 0.0f, 0.0f);
            } else if (ring == rings) {
                geometricNormal = glm::vec3(1.0f, 0.0f, 0.0f);
            } else {
                geometricNormal = glm::normalize(glm::vec3(-slope, cosine, sine));
            }
            glm::vec3 targetNormal = SafeNormalize(vertex.Position, geometricNormal);
            vertex.Normal = SafeNormalize(
                glm::mix(geometricNormal, targetNormal, relaxationProgress),
                targetNormal);
            vertex.TexCoords = glm::vec2(u, (float)ring / (float)rings);
            float materialX = sourceAxial[(size_t)ring];
            float sourceRhoA = std::sqrt(std::max(
                radiusA * radiusA - (materialX - centerA) * (materialX - centerA),
                0.0f));
            float sourceRhoB = std::sqrt(std::max(
                radiusB * radiusB - (materialX - centerB) * (materialX - centerB),
                0.0f));
            glm::vec3 radialFallback(0.0f, cosine, sine);
            glm::vec3 sourceDirectionA = SafeNormalize(
                glm::vec3(materialX - centerA, sourceRhoA * cosine, sourceRhoA * sine),
                radialFallback);
            glm::vec3 sourceDirectionB = SafeNormalize(
                glm::vec3(materialX - centerB, sourceRhoB * cosine, sourceRhoB * sine),
                radialFallback);
            float materialBlendHalfWidth = std::max(minRadius * 0.32f, 0.001f);
            float sourceBlend = Smooth01Local(
                (materialX - contactCenter + materialBlendHalfWidth) /
                (2.0f * materialBlendHalfWidth));
            glm::vec3 sourceFilmDirection = sourceBlend <= 0.5f
                ? SafeNormalize(
                    glm::mix(sourceDirectionA, radialFallback,
                             Smooth01Local(sourceBlend * 2.0f)),
                    radialFallback)
                : SafeNormalize(
                    glm::mix(radialFallback, sourceDirectionB,
                             Smooth01Local((sourceBlend - 0.5f) * 2.0f)),
                    radialFallback);
            vertex.FilmDirection = sourceFilmDirection;
            vertex.Tangent = glm::vec3(0.0f);
            vertex.Bitangent = glm::vec3(0.0f);
            vertices.push_back(vertex);
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

