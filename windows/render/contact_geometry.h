#ifndef WINDOWS_RENDER_CONTACT_GEOMETRY_H
#define WINDOWS_RENDER_CONTACT_GEOMETRY_H

#include <vector>

#include <glm/glm.hpp>

#include "mesh.h"

struct DiscMeshData {
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<unsigned int> indices;
};

inline constexpr int kContactBubbleSegments = 72;
inline constexpr int kContactBubbleRings = 28;

DiscMeshData BuildContactFilmDisc(int segments = 80, int rings = 10);

std::vector<Vertex> BuildCurvedContactFilmVertices(float normalizedCurvature,
                                                   int segments = 72,
                                                   int rings = 10);

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
