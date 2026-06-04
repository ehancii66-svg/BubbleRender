//
// BubbleRender — DBSTT (Da et al. 2015) Vortex Sheet Simulation
//
// Implements Algorithm 1 from "Double Bubbles Sans Toil and Trouble" for a
// single closed soap bubble.
//
// Key equations:
//   Eq (10): ΔΓ^v = -(σΔt/ρA)·(H₁^v - H₂^v)  — surface tension → circulation
//   Eq (11): H_i^v = ½ Σ_e |e|·θ_e              — scalar mean curvature
//   Eq (1):  γ = n × ∇_f Γ                       — vortex sheet strength
//   Eq (12): u(x) = (1/4π)∫γ×r/(|r|²+α²)^{3/2} — regularized Biot-Savart
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
    std::vector<float>     circulation;     // per-vertex circulation Γ
    std::vector<glm::vec3> vertexNormals;
    std::vector<float>     voronoiAreas;    // per-vertex Voronoi area A
    std::vector<float>     meanCurvature;   // per-vertex scalar H
    std::vector<glm::vec3> velocity;
};

// ============================================================
// DBSTT vortex sheet simulation for a single closed bubble
// ============================================================
class VortexSheetSimulation {
public:
    // Surface tension coefficient σ/ρ.
    // Physical soap film: ~0.021 m³/s².  In our dimensionless units,
    // reasonable values are 1–50.  Default is very conservative.
    float surfaceTensionStrength = 15.0f;   // faster, livelier oscillation

    // Regularization α for Biot-Savart (half avg edge length, auto-computed)
    float regularizationAlpha = 0.05f;

    // Circulation diffusion for stability (§6 of paper)
    float circulationDiffusion = 0.0005f;  // light damping only

    // When true, flip the sign of dΓ (Eq 10) for debugging.
    // false = paper convention:  dΓ ∝ –(H₁–H₂)
    // true  = reversed:          dΓ ∝ +(H₁–H₂)
    bool   flipSurfaceTensionSign = true;   // reversed sign works for single closed bubble

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
    void computeMeanCurvature();                        // Eq (11)
    void integrateSurfaceTension(float dt);             // Eq (10)
    void computeVortexSheetStrength(
        std::vector<glm::vec3>& gamma) const;           // Eq (1)
    void computeBiotSavartVelocities(
        const std::vector<glm::vec3>& gamma);           // Eq (12)
    void advectPositions(float dt);
    void diffuseCirculation();

    // Icosphere
    void buildIcosphere(float radius, int subdivs);
    void subdivideIcosphere();
};

#endif
