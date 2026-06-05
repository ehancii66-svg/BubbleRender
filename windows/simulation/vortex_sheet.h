//
// BubbleRender - DBSTT (Da et al. 2015) Vortex Sheet Simulation
//
// Implements Algorithm 1 from "Double Bubbles Sans Toil and Trouble" for a
// single closed soap bubble.
//
// Key equations:
//   Eq (10): surface tension changes circulation.
//   Eq (11): scalar mean curvature.
//   Eq (1):  vortex sheet strength from circulation gradient.
//   Eq (12): regularized Biot-Savart velocity.
//

#ifndef WINDOWS_SIMULATION_VORTEX_SHEET_H
#define WINDOWS_SIMULATION_VORTEX_SHEET_H

#include <glm/glm.hpp>
#include <vector>
#include <cstdint>

// ============================================================
// Simulation mesh data structure
// ============================================================
struct SimMesh {
    std::vector<glm::vec3> positions;
    std::vector<glm::ivec3> triangles;
    std::vector<float>     circulation;     // per-vertex circulation
    std::vector<glm::vec3> vertexNormals;
    std::vector<float>     voronoiAreas;    // per-vertex Voronoi area
    std::vector<float>     meanCurvature;   // per-vertex scalar curvature
    std::vector<glm::vec3> velocity;
};

// ============================================================
// DBSTT vortex sheet simulation for a single closed bubble
// ============================================================
class VortexSheetSimulation {
public:
    // Surface tension coefficient in dimensionless units.
    float surfaceTensionStrength = 15.0f;

    // Regularization alpha for Biot-Savart, auto-computed from edge length.
    float regularizationAlpha = 0.05f;

    // Circulation diffusion for stability.
    float circulationDiffusion = 0.0005f;

    // Current sign toggle; true is stable for this single-bubble demo.
    bool   flipSurfaceTensionSign = true;
    bool   diagnosticsEnabled = false;

    // Simulation control
    int    substepsPerFrame = 5;
    float  timeStep = 0.0005f;

    VortexSheetSimulation();

    void initIcosphere(float radius, int subdivisionLevel, float perturbAmount);
    void initFromMesh(const std::vector<glm::vec3>& verts,
                      const std::vector<unsigned int>& indices,
                      float perturbAmount);

    void update(float frameDt);

    // Accessors
    const std::vector<glm::vec3>& getPositions() const { return m_mesh.positions; }
    const std::vector<glm::vec3>& getNormals()   const { return m_mesh.vertexNormals; }
    const std::vector<glm::ivec3>& getTriangles() const { return m_mesh.triangles; }
    const std::vector<float>&     getCirculation() const { return m_mesh.circulation; }
    int  getVertexCount()    const { return (int)m_mesh.positions.size(); }
    int  getTriangleCount()  const { return (int)m_mesh.triangles.size(); }
    float getAverageEdgeLength() const { return m_avgEdgeLength; }

private:
    SimMesh m_mesh;
    float   m_avgEdgeLength = 0.0f;

    // Algorithm steps
    void computeVertexNormalsAndAreas();
    void computeMeanCurvature();
    void integrateSurfaceTension(float dt);
    void computeVortexSheetStrength(std::vector<glm::vec3>& gamma) const;
    void computeBiotSavartVelocities(const std::vector<glm::vec3>& gamma);
    void advectPositions(float dt);
    void diffuseCirculation();

    // Icosphere
    void buildIcosphere(float radius, int subdivs);
    void subdivideIcosphere();
};

#endif
