//
// BubbleRender - DBSTT (Da et al. 2015) Vortex Sheet Simulation
// Implementation for a single closed soap bubble.
//

#include "vortex_sheet.h"
#include <glm/gtc/constants.hpp>
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <iostream>

// ============================================================
// Internal helpers
// ============================================================

static const float kPi      = 3.14159265358979323846f;
static const float kFourPi  = 4.0f * kPi;
static const float kEpsilon = 1e-10f;

namespace {

inline uint64_t edgeKey(int i, int j) {
    if (i > j) std::swap(i, j);
    return (uint64_t(i) << 32) | uint64_t(j);
}

inline float safeAcos(float x) {
    return std::acos(std::max(-1.0f, std::min(1.0f, x)));
}

} // anonymous namespace

// ============================================================
// Construction
// ============================================================

VortexSheetSimulation::VortexSheetSimulation() = default;

// ============================================================
// Initialization: icosphere
// ============================================================

void VortexSheetSimulation::initIcosphere(float radius, int subdivs, float perturbAmount)
{
    buildIcosphere(radius, subdivs);

    if (perturbAmount > 0.0f) {
        float angle = 0.7f;
        glm::vec3 stretchAxis(std::cos(angle), std::sin(angle) * 0.5f, std::sin(angle + 1.3f));
        stretchAxis = glm::normalize(stretchAxis);

        for (auto& p : m_mesh.positions) {
            float proj = glm::dot(p, stretchAxis);
            p += stretchAxis * proj * perturbAmount;
        }
    }

    int nv = (int)m_mesh.positions.size();
    m_mesh.circulation.assign(nv, 0.0f);
    m_mesh.vertexNormals.assign(nv, glm::vec3(0.0f));
    m_mesh.voronoiAreas.assign(nv, 0.0f);
    m_mesh.meanCurvature.assign(nv, 0.0f);
    m_mesh.velocity.assign(nv, glm::vec3(0.0f));

    computeVertexNormalsAndAreas();
    m_avgEdgeLength = 0.0f;
    {
        int edgeCount = 0;
        for (const auto& tri : m_mesh.triangles) {
            for (int k = 0; k < 3; ++k) {
                glm::vec3 d = m_mesh.positions[tri[k]] - m_mesh.positions[tri[(k+1)%3]];
                m_avgEdgeLength += glm::length(d);
                ++edgeCount;
            }
        }
        if (edgeCount > 0) m_avgEdgeLength /= float(edgeCount);
    }
    regularizationAlpha = m_avgEdgeLength * 1.0f;

    if (diagnosticsEnabled) {
        computeMeanCurvature();
        float Hmin = 1e9f, Hmax = -1e9f, Havg = 0.0f;
        int negC = 0;
        for (int i = 0; i < nv; ++i) {
            float h = m_mesh.meanCurvature[i];
            Hmin = std::min(Hmin, h); Hmax = std::max(Hmax, h); Havg += h;
            if (h < 0.0f) negC++;
        }
        Havg /= nv;
        std::cout << "[DBSTT] Initial H: [" << Hmin << "," << Hmax
                  << "] avg=" << Havg << " neg=" << negC << "/" << nv
                  << " (sphere target=" << (1.0f/radius) << ")"
                  << std::endl;
        std::fill(m_mesh.meanCurvature.begin(), m_mesh.meanCurvature.end(), 0.0f);
    }

    std::cout << "[DBSTT] Mesh: " << nv << " verts, "
              << m_mesh.triangles.size() << " tris, "
              << "edge=" << m_avgEdgeLength << ", alpha=" << regularizationAlpha
              << std::endl;
}

// ============================================================
// Initialization: from external mesh
// ============================================================

void VortexSheetSimulation::initFromMesh(
    const std::vector<glm::vec3>& verts,
    const std::vector<unsigned int>& indices,
    float perturbAmount)
{
    int nv = (int)verts.size();
    int nt = (int)indices.size() / 3;

    m_mesh.positions = verts;
    m_mesh.triangles.clear();
    m_mesh.triangles.reserve(nt);
    for (int i = 0; i < nt; ++i) {
        m_mesh.triangles.emplace_back(
            indices[3*i], indices[3*i+1], indices[3*i+2]);
    }

    if (perturbAmount > 0.0f) {
        float angle = 0.7f;
        glm::vec3 stretchAxis(std::cos(angle), std::sin(angle) * 0.5f, std::sin(angle + 1.3f));
        stretchAxis = glm::normalize(stretchAxis);
        for (auto& p : m_mesh.positions) {
            float proj = glm::dot(p, stretchAxis);
            p += stretchAxis * proj * perturbAmount;
        }
    }

    m_mesh.circulation.assign(nv, 0.0f);
    m_mesh.vertexNormals.assign(nv, glm::vec3(0.0f));
    m_mesh.voronoiAreas.assign(nv, 0.0f);
    m_mesh.meanCurvature.assign(nv, 0.0f);
    m_mesh.velocity.assign(nv, glm::vec3(0.0f));

    computeVertexNormalsAndAreas();
    m_avgEdgeLength = 0.0f;
    {
        int ec = 0;
        for (const auto& tri : m_mesh.triangles) {
            for (int k = 0; k < 3; ++k) {
                glm::vec3 d = m_mesh.positions[tri[k]] - m_mesh.positions[tri[(k+1)%3]];
                m_avgEdgeLength += glm::length(d);
                ++ec;
            }
        }
        if (ec > 0) m_avgEdgeLength /= float(ec);
    }
    regularizationAlpha = m_avgEdgeLength * 1.0f;
}

// ============================================================
// Per-frame update - Algorithm 1 from Da et al. 2015
// ============================================================

void VortexSheetSimulation::update(float frameDt)
{
    for (int s = 0; s < substepsPerFrame; ++s) {
        // Step 1: Geometry
        computeVertexNormalsAndAreas();

        // Step 2: Mean curvature H (§4.3, Eq 11)
        computeMeanCurvature();

        // Step 3: Integrate surface tension → Γ (§4.3, Eq 10)
        integrateSurfaceTension(timeStep);

        // Step 4: Circulation diffusion (§6, stability)
        diffuseCirculation();

        // Step 5: Vortex sheet strength γ = n × ∇_f Γ (§3.1, Eq 1)
        std::vector<glm::vec3> gamma;
        computeVortexSheetStrength(gamma);

        // Step 6: Biot-Savart velocity (§4.4, Eq 12)
        computeBiotSavartVelocities(gamma);

        // Step 7: Advect positions (§4.5)
        advectPositions(timeStep);
    }

    // Final normal update for rendering
    computeVertexNormalsAndAreas();

    if (diagnosticsEnabled) {
        static int frameCount = 0;
        if (++frameCount % 60 == 0) {
            int nv = (int)m_mesh.positions.size();
            float minR = 1e9f, maxR = 0.0f, avgR = 0.0f;
            float maxV = 0.0f, maxH = 0.0f, maxAbsGamma = 0.0f;
            int poleIdx = 0, eqIdx = 0;
            for (int i = 0; i < nv; ++i) {
                float r = glm::length(m_mesh.positions[i]);
                avgR += r;
                if (r > maxR) { maxR = r; poleIdx = i; }
                if (r < minR) { minR = r; eqIdx = i; }
                maxV = std::max(maxV, glm::length(m_mesh.velocity[i]));
                maxH = std::max(maxH, std::abs(m_mesh.meanCurvature[i]));
                maxAbsGamma = std::max(maxAbsGamma, std::abs(m_mesh.circulation[i]));
            }
            avgR /= nv;

            // Velocity direction at the farthest point ("pole")
            const auto& vPole = m_mesh.velocity[poleIdx];
            const auto& nPole = m_mesh.vertexNormals[poleIdx];
            float vdotn_pole = glm::dot(vPole, nPole);
            float Hpole = m_mesh.meanCurvature[poleIdx];
            float Gpole = m_mesh.circulation[poleIdx];

            // Velocity direction at the nearest point ("equator")
            const auto& vEq = m_mesh.velocity[eqIdx];
            const auto& nEq = m_mesh.vertexNormals[eqIdx];
            float vdotn_eq = glm::dot(vEq, nEq);
            float Heq = m_mesh.meanCurvature[eqIdx];

            const char* status = "?";
            if (vdotn_pole < -1e-6f && vdotn_eq > 1e-6f)
                status = "RESTORATIVE OK";    // pole: inward, eq: outward
            else if (vdotn_pole > 1e-6f && vdotn_eq < -1e-6f)
                status = "DESTRUCTIVE BAD";   // pole: outward, eq: inward
            else
                status = "MIXED";

            std::cout << "[DBSTT] r=[" << minR << "," << maxR << "] avgR=" << avgR
                      << " max|v|=" << maxV << " max|H|=" << maxH
                      << " max|circulation|=" << maxAbsGamma
                      << " vDotN(pole)=" << vdotn_pole << " vDotN(eq)=" << vdotn_eq
                      << " Hpole=" << Hpole << " Heq=" << Heq
                      << " Gpole=" << Gpole
                      << " " << status
                      << std::endl;
        }
    }
}

// ============================================================
// Per-vertex normals and Voronoi areas
// ============================================================

void VortexSheetSimulation::computeVertexNormalsAndAreas()
{
    int nv = (int)m_mesh.positions.size();
    std::fill(m_mesh.vertexNormals.begin(), m_mesh.vertexNormals.end(), glm::vec3(0.0f));
    std::fill(m_mesh.voronoiAreas.begin(), m_mesh.voronoiAreas.end(), 0.0f);

    for (const auto& tri : m_mesh.triangles) {
        glm::vec3 e1 = m_mesh.positions[tri.y] - m_mesh.positions[tri.x];
        glm::vec3 e2 = m_mesh.positions[tri.z] - m_mesh.positions[tri.x];
        glm::vec3 fn = glm::cross(e1, e2);
        float area2 = glm::length(fn);
        if (area2 < kEpsilon) continue;
        fn /= area2;

        glm::vec3 e01 = m_mesh.positions[tri.y] - m_mesh.positions[tri.x];
        glm::vec3 e02 = m_mesh.positions[tri.z] - m_mesh.positions[tri.x];
        glm::vec3 e10 = -e01;
        glm::vec3 e12 = m_mesh.positions[tri.z] - m_mesh.positions[tri.y];
        glm::vec3 e20 = -e02;
        glm::vec3 e21 = -e12;

        float l01 = glm::length(e01), l02 = glm::length(e02), l12 = glm::length(e12);
        float ang0 = (l01 > kEpsilon && l02 > kEpsilon) ?
            safeAcos(glm::dot(e01, e02) / (l01 * l02)) : 0.0f;
        float ang1 = (l01 > kEpsilon && l12 > kEpsilon) ?
            safeAcos(glm::dot(e10, e12) / (l01 * l12)) : 0.0f;
        float ang2 = (l02 > kEpsilon && l12 > kEpsilon) ?
            safeAcos(glm::dot(e20, e21) / (l02 * l12)) : 0.0f;

        m_mesh.vertexNormals[tri.x] += fn * ang0;
        m_mesh.vertexNormals[tri.y] += fn * ang1;
        m_mesh.vertexNormals[tri.z] += fn * ang2;

        float triArea = area2 * 0.5f;
        float thirdArea = triArea / 3.0f;
        m_mesh.voronoiAreas[tri.x] += thirdArea;
        m_mesh.voronoiAreas[tri.y] += thirdArea;
        m_mesh.voronoiAreas[tri.z] += thirdArea;
    }

    for (int i = 0; i < nv; ++i) {
        float len = glm::length(m_mesh.vertexNormals[i]);
        if (len > kEpsilon) {
            m_mesh.vertexNormals[i] /= len;
        } else {
            glm::vec3 pos = m_mesh.positions[i];
            float r = glm::length(pos);
            m_mesh.vertexNormals[i] = (r > kEpsilon)
                ? glm::normalize(pos) : glm::vec3(0.0f, 1.0f, 0.0f);
        }
    }
}

// ============================================================
// §4.3 Eq (11): Signed scalar mean curvature at each vertex
// ============================================================

void VortexSheetSimulation::computeMeanCurvature()
{
    int nv = (int)m_mesh.positions.size();
    std::fill(m_mesh.meanCurvature.begin(), m_mesh.meanCurvature.end(), 0.0f);

    // Build neighbor list
    std::vector<std::vector<int>> neighbors(nv);
    for (const auto& tri : m_mesh.triangles) {
        neighbors[tri.x].push_back(tri.y);
        neighbors[tri.x].push_back(tri.z);
        neighbors[tri.y].push_back(tri.x);
        neighbors[tri.y].push_back(tri.z);
        neighbors[tri.z].push_back(tri.x);
        neighbors[tri.z].push_back(tri.y);
    }
    for (auto& nl : neighbors) {
        std::sort(nl.begin(), nl.end());
        nl.erase(std::unique(nl.begin(), nl.end()), nl.end());
    }

    // Build edge-to-face map for cotan computation
    std::unordered_map<uint64_t, std::vector<int>> edgeFaces;
    for (int fi = 0; fi < (int)m_mesh.triangles.size(); ++fi) {
        const auto& t = m_mesh.triangles[fi];
        for (int k = 0; k < 3; ++k) {
            int a = t[k], b = t[(k+1)%3];
            edgeFaces[edgeKey(a, b)].push_back(fi);
        }
    }

    // Compute mean curvature normal K_i = H_i * n_i * A_i using cotan formula
    // K_i = -(1/4) * Σ_j (cot α_ij + cot β_ij) * (v_j - v_i)
    // where α, β are angles opposite edge (i,j) in the two incident triangles.
    std::vector<glm::vec3> curvNormal(nv, glm::vec3(0.0f));

    for (const auto& [key, faceList] : edgeFaces) {
        if (faceList.size() != 2) continue;

        int vi = int(key >> 32);
        int vj = int(key & 0xFFFFFFFF);
        int fi0 = faceList[0], fi1 = faceList[1];

        const auto& t0 = m_mesh.triangles[fi0];
        const auto& t1 = m_mesh.triangles[fi1];

        // Find vertex opposite edge (i,j) in each triangle
        auto opp = [&](const glm::ivec3& t, int a, int b) -> int {
            for (int k = 0; k < 3; ++k)
                if (t[k] != a && t[k] != b) return t[k];
            return -1;
        };
        int vk0 = opp(t0, vi, vj);
        int vk1 = opp(t1, vi, vj);
        if (vk0 < 0 || vk1 < 0) continue;

        // Cotangent of angle at vk0 (opposite edge vi-vj in triangle t0)
        glm::vec3 e_vi_vk0 = m_mesh.positions[vi] - m_mesh.positions[vk0];
        glm::vec3 e_vj_vk0 = m_mesh.positions[vj] - m_mesh.positions[vk0];
        float cot0 = glm::dot(e_vi_vk0, e_vj_vk0) /
                     glm::length(glm::cross(e_vi_vk0, e_vj_vk0));

        // Cotangent of angle at vk1 (opposite edge vi-vj in triangle t1)
        glm::vec3 e_vi_vk1 = m_mesh.positions[vi] - m_mesh.positions[vk1];
        glm::vec3 e_vj_vk1 = m_mesh.positions[vj] - m_mesh.positions[vk1];
        float cot1 = glm::dot(e_vi_vk1, e_vj_vk1) /
                     glm::length(glm::cross(e_vi_vk1, e_vj_vk1));

        float weight = cot0 + cot1;
        if (!std::isfinite(weight)) continue;

        glm::vec3 diff = m_mesh.positions[vj] - m_mesh.positions[vi];
        glm::vec3 contrib = weight * diff;

        curvNormal[vi] += contrib;
        curvNormal[vj] -= contrib;  // opposite sign for the other vertex
    }

    // Extract scalar mean curvature: H_i = -dot(K_i/A_i, n_i)
    // (minus sign because K points inward for convex → H > 0 with outward n)
    for (int i = 0; i < nv; ++i) {
        float area = m_mesh.voronoiAreas[i];
        if (area < kEpsilon) continue;
        glm::vec3 K = curvNormal[i] * (0.25f / area);  // K = H_i * n_i
        m_mesh.meanCurvature[i] = -glm::dot(K, m_mesh.vertexNormals[i]);
    }
}

// ============================================================
// §4.3 Eq (10): ΔΓ = -(σΔt/ρA)·(H₁-H₂) = -(σΔt/ρA)·2H
// ============================================================

void VortexSheetSimulation::integrateSurfaceTension(float dt)
{
    // Eq (10): ΔΓ^v = -(σΔt/ρA_v)(H₁^v - H₂^v)
    //
    // H₁^v, H₂^v are INTEGRATED mean curvatures (units: length).
    // meanCurvature[i] now stores POINTWISE H (units: 1/length) from cotan.
    // Integrated H = pointwise H × A_v, so:
    //   ΔΓ = sign × (σΔt/ρA_v) × 2 × (H × A_v)
    //       = sign × (σΔt/ρ) × 2 × H                    (A_v cancels!)
    float sigmaOverRho = surfaceTensionStrength;
    float sign = flipSurfaceTensionSign ? 1.0f : -1.0f;

    for (int i = 0; i < (int)m_mesh.positions.size(); ++i) {
        float Hv = m_mesh.meanCurvature[i];
        float dGamma = sign * sigmaOverRho * dt * 2.0f * Hv;
        m_mesh.circulation[i] += dGamma;
    }
}

// ============================================================
// §3.1 Eq (1): γ = n × ∇_f Γ
// ============================================================

void VortexSheetSimulation::computeVortexSheetStrength(
    std::vector<glm::vec3>& gamma) const
{
    int nt = (int)m_mesh.triangles.size();
    gamma.assign(nt, glm::vec3(0.0f));

    for (int ti = 0; ti < nt; ++ti) {
        const auto& tri = m_mesh.triangles[ti];
        const glm::vec3& p0 = m_mesh.positions[tri.x];
        const glm::vec3& p1 = m_mesh.positions[tri.y];
        const glm::vec3& p2 = m_mesh.positions[tri.z];

        float g0 = m_mesh.circulation[tri.x];
        float g1 = m_mesh.circulation[tri.y];
        float g2 = m_mesh.circulation[tri.z];

        glm::vec3 e1 = p1 - p0;
        glm::vec3 e2 = p2 - p0;
        glm::vec3 n  = glm::cross(e1, e2);
        float area2  = glm::length(n);
        if (area2 < kEpsilon) continue;
        n /= area2;
        float area = area2 * 0.5f;

        glm::vec3 centroid = (p0 + p1 + p2) / 3.0f;
        if (glm::dot(n, centroid) < 0.0f) n = -n;

        // Surface gradient of Γ on the triangle:
        //   ∇_f Γ = n × (Γ₀(v₂-v₁) + Γ₁(v₀-v₂) + Γ₂(v₁-v₀)) / (2A)
        glm::vec3 gradNum = g0 * (p2 - p1) + g1 * (p0 - p2) + g2 * (p1 - p0);
        glm::vec3 gradGamma = glm::cross(n, gradNum) / (2.0f * area);

        // γ = n × ∇_f Γ
        gamma[ti] = glm::cross(n, gradGamma);
    }
}

// ============================================================
// §4.4 Eq (12): Regularized Biot-Savart integral
// u(x) = (1/4π)·Σ_t γ_t × (x-c_t) / (|x-c_t|²+α²)^{3/2} · A_t
// ============================================================

void VortexSheetSimulation::computeBiotSavartVelocities(
    const std::vector<glm::vec3>& gamma)
{
    int nv = (int)m_mesh.positions.size();
    int nt = (int)m_mesh.triangles.size();
    float alpha2 = regularizationAlpha * regularizationAlpha;

    std::fill(m_mesh.velocity.begin(), m_mesh.velocity.end(), glm::vec3(0.0f));

    // Precompute triangle centroids and areas
    struct TriData {
        glm::vec3 centroid;
        float area;
        glm::vec3 gammaVal;
    };
    std::vector<TriData> triData(nt);

    for (int ti = 0; ti < nt; ++ti) {
        const auto& tri = m_mesh.triangles[ti];
        const glm::vec3& p0 = m_mesh.positions[tri.x];
        const glm::vec3& p1 = m_mesh.positions[tri.y];
        const glm::vec3& p2 = m_mesh.positions[tri.z];

        triData[ti].centroid = (p0 + p1 + p2) / 3.0f;

        glm::vec3 e1 = p1 - p0;
        glm::vec3 e2 = p2 - p0;
        triData[ti].area = 0.5f * glm::length(glm::cross(e1, e2));
        triData[ti].gammaVal = gamma[ti];
    }

    for (int vi = 0; vi < nv; ++vi) {
        glm::vec3 vel(0.0f);
        const glm::vec3& x = m_mesh.positions[vi];

        for (int ti = 0; ti < nt; ++ti) {
            if (triData[ti].area < kEpsilon) continue;

            glm::vec3 r = x - triData[ti].centroid;
            float r2 = glm::dot(r, r);
            float denom = std::pow(r2 + alpha2, 1.5f);
            if (denom < kEpsilon) continue;

            glm::vec3 contrib = glm::cross(triData[ti].gammaVal, r);
            vel += contrib * (triData[ti].area / denom);
        }

        m_mesh.velocity[vi] = vel / kFourPi;
    }
}

// ============================================================
// §4.5: Integrate vertex positions (forward Euler + safety clamp)
// ============================================================

void VortexSheetSimulation::advectPositions(float dt)
{
    float maxDisp = m_avgEdgeLength * 0.1f;

    for (int i = 0; i < (int)m_mesh.positions.size(); ++i) {
        glm::vec3 disp = m_mesh.velocity[i] * dt;
        float len2 = glm::dot(disp, disp);
        if (len2 > maxDisp * maxDisp) {
            disp *= maxDisp / std::sqrt(len2);
        }
        m_mesh.positions[i] += disp;
    }

    // Centering correction
    glm::vec3 center(0.0f);
    for (const auto& p : m_mesh.positions) center += p;
    center /= float(m_mesh.positions.size());
    for (auto& p : m_mesh.positions) p -= center;
}

// ============================================================
// Stability: Laplacian diffusion of circulation (§6)
// ============================================================

void VortexSheetSimulation::diffuseCirculation()
{
    if (circulationDiffusion <= 0.0f) return;

    int nv = (int)m_mesh.positions.size();

    std::vector<std::vector<int>> neighbors(nv);
    for (const auto& tri : m_mesh.triangles) {
        neighbors[tri.x].push_back(tri.y);
        neighbors[tri.x].push_back(tri.z);
        neighbors[tri.y].push_back(tri.x);
        neighbors[tri.y].push_back(tri.z);
        neighbors[tri.z].push_back(tri.x);
        neighbors[tri.z].push_back(tri.y);
    }
    for (auto& nl : neighbors) {
        std::sort(nl.begin(), nl.end());
        nl.erase(std::unique(nl.begin(), nl.end()), nl.end());
    }

    std::vector<float> newGamma = m_mesh.circulation;
    for (int i = 0; i < nv; ++i) {
        if (neighbors[i].empty()) continue;
        float avg = 0.0f;
        for (int nb : neighbors[i]) avg += m_mesh.circulation[nb];
        avg /= float(neighbors[i].size());
        float diff = avg - m_mesh.circulation[i];
        newGamma[i] += circulationDiffusion * diff;
    }
    m_mesh.circulation.swap(newGamma);
}

// ============================================================
// Icosphere construction
// ============================================================

void VortexSheetSimulation::buildIcosphere(float radius, int subdivs)
{
    const float PHI = (1.0f + std::sqrt(5.0f)) / 2.0f;

    std::vector<glm::vec3> verts = {
        {-1.0f,  PHI, 0.0f}, { 1.0f,  PHI, 0.0f}, {-1.0f, -PHI, 0.0f}, { 1.0f, -PHI, 0.0f},
        { 0.0f, -1.0f,  PHI}, { 0.0f,  1.0f,  PHI}, { 0.0f, -1.0f, -PHI}, { 0.0f,  1.0f, -PHI},
        { PHI, 0.0f, -1.0f}, { PHI, 0.0f,  1.0f}, {-PHI, 0.0f, -1.0f}, {-PHI, 0.0f,  1.0f},
    };

    for (auto& v : verts) v = glm::normalize(v);

    std::vector<glm::ivec3> faces = {
        {0, 11, 5}, {0, 5, 1},  {0, 1, 7},  {0, 7, 10}, {0, 10, 11},
        {1, 5, 9},  {5, 11, 4}, {11, 10, 2},{10, 7, 6}, {7, 1, 8},
        {3, 9, 4},  {3, 4, 2},  {3, 2, 6},  {3, 6, 8},  {3, 8, 9},
        {4, 9, 5},  {2, 4, 11}, {6, 2, 10}, {8, 6, 7},  {9, 8, 1},
    };

    m_mesh.positions = verts;
    m_mesh.triangles = faces;

    for (int s = 0; s < subdivs; ++s) {
        subdivideIcosphere();
    }

    for (auto& p : m_mesh.positions) {
        p = glm::normalize(p) * radius;
    }
}

void VortexSheetSimulation::subdivideIcosphere()
{
    std::unordered_map<uint64_t, int> midpointCache;

    auto getMidpoint = [&](int i0, int i1) -> int {
        uint64_t key = edgeKey(i0, i1);
        auto it = midpointCache.find(key);
        if (it != midpointCache.end()) return it->second;

        glm::vec3 mid = glm::normalize(m_mesh.positions[i0] + m_mesh.positions[i1]);
        int idx = (int)m_mesh.positions.size();
        m_mesh.positions.push_back(mid);
        midpointCache[key] = idx;
        return idx;
    };

    std::vector<glm::ivec3> newFaces;
    newFaces.reserve(m_mesh.triangles.size() * 4);

    for (const auto& tri : m_mesh.triangles) {
        int a = tri.x, b = tri.y, c = tri.z;
        int ab = getMidpoint(a, b);
        int bc = getMidpoint(b, c);
        int ca = getMidpoint(c, a);

        newFaces.push_back({a,  ab, ca});
        newFaces.push_back({b,  bc, ab});
        newFaces.push_back({c,  ca, bc});
        newFaces.push_back({ab, bc, ca});
    }

    m_mesh.triangles.swap(newFaces);
}
