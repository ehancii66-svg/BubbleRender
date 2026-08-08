#ifndef WINDOWS_RENDER_CONTACT_GEOMETRY_H
#define WINDOWS_RENDER_CONTACT_GEOMETRY_H

#include <vector>

#include <glm/glm.hpp>

#include "mesh.h"
#include "../simulation/fusion_profile.h"

struct DiscMeshData {
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<unsigned int> indices;
};

inline constexpr int kContactBubbleSegments = 72;
inline constexpr int kContactBubbleRings = 28;
inline constexpr int kFusionSurfaceSegments = 96;
inline constexpr int kFusionSurfaceRings = 96;

struct FusionSurfaceParameters {
    float centerA = -0.5f;
    float radiusA = 0.5f;
    float centerB = 0.5f;
    float radiusB = 0.5f;
    float opticalReferenceRadius = 0.63f;
};

DiscMeshData BuildContactFilmDisc(int segments = 80, int rings = 10);

std::vector<Vertex> BuildCurvedContactFilmVertices(float normalizedCurvature,
                                                   float innerRadius = 0.0f,
                                                   int segments = 72,
                                                   int rings = 10);

std::vector<unsigned int> BuildFusionSurfaceIndices(int segments = kFusionSurfaceSegments,
                                                    int rings = kFusionSurfaceRings);

std::vector<Vertex> BuildFusionSurfaceVertices(const FusionSurfaceParameters& parameters,
                                               const std::vector<FusionProfileNode>& profile,
                                               int segments = kFusionSurfaceSegments,
                                               int rings = kFusionSurfaceRings);

std::vector<unsigned int> BuildContactBubblePatchIndices(int segments = kContactBubbleSegments,
                                                         int rings = kContactBubbleRings);

std::vector<Vertex> BuildBubbleShellVertices(float radius,
                                             float contactRadius,
                                             bool hasContact,
                                             int segments = kContactBubbleSegments,
                                             int rings = kContactBubbleRings);

std::vector<Vertex> BuildContactBubblePatchVertices(float radius,
                                                    float capPlaneOffset,
                                                    int segments = kContactBubbleSegments,
                                                    int rings = kContactBubbleRings);

#endif
