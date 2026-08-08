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
    segments = std::max(segments, 8);
    rings = std::max(rings, 2);
    std::vector<unsigned int> indices;
    indices.reserve(static_cast<size_t>(rings - 1) *
                    static_cast<size_t>(segments) * 6u);
    const unsigned int firstRing = 1u;
    for (int j = 0; j < segments; ++j)
    {
        indices.push_back(0u);
        indices.push_back(firstRing + static_cast<unsigned int>(j));
        indices.push_back(firstRing + static_cast<unsigned int>(j + 1));
    }
    for (int ring = 1; ring < rings - 1; ++ring)
    {
        unsigned int row0 = 1u + static_cast<unsigned int>(ring - 1) *
                                  static_cast<unsigned int>(segments + 1);
        unsigned int row1 = row0 + static_cast<unsigned int>(segments + 1);
        for (int j = 0; j < segments; ++j)
        {
            unsigned int a = row0 + static_cast<unsigned int>(j);
            unsigned int b = row1 + static_cast<unsigned int>(j);
            unsigned int c = a + 1u;
            unsigned int d = b + 1u;
            indices.insert(indices.end(), {a, b, c, c, b, d});
        }
    }
    unsigned int rightPole = 1u + static_cast<unsigned int>(rings - 1) *
                                   static_cast<unsigned int>(segments + 1);
    unsigned int lastRing = rightPole -
                            static_cast<unsigned int>(segments + 1);
    for (int j = 0; j < segments; ++j)
    {
        indices.push_back(lastRing + static_cast<unsigned int>(j));
        indices.push_back(rightPole);
        indices.push_back(lastRing + static_cast<unsigned int>(j + 1));
    }
    return indices;
}

std::vector<Vertex> BuildFusionSurfaceVertices(
    const FusionSurfaceParameters& parameters,
    const std::vector<FusionProfileNode>& profile,
    int segments,
    int rings)
{
    std::vector<Vertex> vertices;
    if (profile.size() < 3)
        return vertices;

    segments = std::max(segments, 8);
    rings = static_cast<int>(profile.size()) - 1;
    float opticalReferenceRadius =
        std::max(parameters.opticalReferenceRadius, 0.001f);
    // One shared vertex per pole avoids duplicate positions and degenerate
    // triangles. Only non-polar rings retain a duplicated UV seam.
    vertices.reserve(2 + (rings - 1) * (segments + 1));

    for (int i = 0; i <= rings; ++i)
    {
        const FusionProfileNode& node = profile[static_cast<std::size_t>(i)];
        glm::vec2 tangent;
        if (i == 0)
            tangent = glm::vec2(profile[1].z - node.z,
                                profile[1].r - node.r);
        else if (i == rings)
            tangent = glm::vec2(node.z - profile[profile.size() - 2].z,
                                node.r - profile[profile.size() - 2].r);
        else
        {
            glm::vec2 previous(
                node.z - profile[static_cast<std::size_t>(i - 1)].z,
                node.r - profile[static_cast<std::size_t>(i - 1)].r);
            glm::vec2 next(
                profile[static_cast<std::size_t>(i + 1)].z - node.z,
                profile[static_cast<std::size_t>(i + 1)].r - node.r);
            // Arc-length-consistent tangent: unequal ring spacing must not
            // bias the rendered normal toward the longer adjacent edge.
            tangent = previous / std::max(glm::length(previous), 1e-7f) +
                      next / std::max(glm::length(next), 1e-7f);
        }

        float tangentLength = glm::length(tangent);
        if (tangentLength > 1e-7f)
            tangent /= tangentLength;
        else
            tangent = glm::vec2(1.0f, 0.0f);

        int angularVertexCount = (i == 0 || i == rings)
                                     ? 1
                                     : segments + 1;
        for (int j = 0; j < angularVertexCount; ++j)
        {
            float u = static_cast<float>(j) / static_cast<float>(segments);
            float phi = 2.0f * kPi * u;
            float c = std::cos(phi);
            float s = std::sin(phi);

            Vertex vertex{};
            vertex.Position = glm::vec3(node.z, node.r * c, node.r * s);

            if (i == 0)
                vertex.Normal = glm::vec3(-1.0f, 0.0f, 0.0f);
            else if (i == rings)
                vertex.Normal = glm::vec3(1.0f, 0.0f, 0.0f);
            else
                vertex.Normal = SafeNormalize(
                    glm::vec3(-tangent.y, tangent.x * c, tangent.x * s),
                    glm::vec3(0.0f, c, s));

            vertex.TexCoords = glm::vec2(u,
                static_cast<float>(i) / static_cast<float>(rings));
            vertex.Tangent = glm::vec3(0.0f);

            glm::vec3 transportedFilmDirection(
                node.filmAxial,
                node.filmRadial * c,
                node.filmRadial * s);
            vertex.FilmDirection = SafeNormalize(
                transportedFilmDirection, vertex.Normal);
            vertex.OpticalRadiusScale =
                std::max(node.opticalRadiusScale / opticalReferenceRadius,
                         0.001f);
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
