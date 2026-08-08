#include "fusion_profile.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kPi = 3.14159265358979323846f;

struct ProfileGradients {
    std::vector<float> areaZ;
    std::vector<float> areaR;
    std::vector<float> volumeZ;
    std::vector<float> volumeR;
    float area = 0.0f;
    float volume = 0.0f;
};

struct ProfileFrame {
    float tangentZ = 1.0f;
    float tangentR = 0.0f;
    float normalZ = 0.0f;
    float normalR = 1.0f;
};

ProfileFrame ComputeFrame(const std::vector<FusionProfileNode>& nodes,
                          size_t i)
{
    ProfileFrame frame;
    if (i == 0) {
        frame.tangentZ = nodes[1].z - nodes[0].z;
        frame.tangentR = nodes[1].r - nodes[0].r;
    } else if (i + 1 == nodes.size()) {
        frame.tangentZ = nodes[i].z - nodes[i - 1].z;
        frame.tangentR = nodes[i].r - nodes[i - 1].r;
    } else {
        float l0 = std::hypot(nodes[i].z - nodes[i - 1].z,
                              nodes[i].r - nodes[i - 1].r);
        float l1 = std::hypot(nodes[i + 1].z - nodes[i].z,
                              nodes[i + 1].r - nodes[i].r);
        float w0 = 1.0f / std::max(l0, 1e-6f);
        float w1 = 1.0f / std::max(l1, 1e-6f);
        frame.tangentZ = w0 * (nodes[i].z - nodes[i - 1].z) +
                         w1 * (nodes[i + 1].z - nodes[i].z);
        frame.tangentR = w0 * (nodes[i].r - nodes[i - 1].r) +
                         w1 * (nodes[i + 1].r - nodes[i].r);
    }
    float length = std::max(std::hypot(frame.tangentZ, frame.tangentR),
                            1e-7f);
    frame.tangentZ /= length;
    frame.tangentR /= length;
    frame.normalZ = -frame.tangentR;
    frame.normalR = frame.tangentZ;
    if (i == 0) {
        frame.tangentZ = 0.0f;
        frame.tangentR = 1.0f;
        frame.normalZ = -1.0f;
        frame.normalR = 0.0f;
    } else if (i + 1 == nodes.size()) {
        frame.tangentZ = 0.0f;
        frame.tangentR = -1.0f;
        frame.normalZ = 1.0f;
        frame.normalR = 0.0f;
    }
    return frame;
}

ProfileGradients ComputeGradients(const std::vector<FusionProfileNode>& nodes)
{
    ProfileGradients result;
    const size_t count = nodes.size();
    result.areaZ.assign(count, 0.0f);
    result.areaR.assign(count, 0.0f);
    result.volumeZ.assign(count, 0.0f);
    result.volumeR.assign(count, 0.0f);
    for (size_t i = 0; i + 1 < count; ++i) {
        const FusionProfileNode& a = nodes[i];
        const FusionProfileNode& b = nodes[i + 1];
        float dz = b.z - a.z;
        float dr = b.r - a.r;
        float length = std::sqrt(dz * dz + dr * dr);
        length = std::max(length, 1e-6f);
        float radiusSum = std::max(a.r + b.r, 0.0f);
        float area = kPi * radiusSum * length;
        result.area += area;

        float commonZ = kPi * radiusSum * dz / length;
        result.areaZ[i] -= commonZ;
        result.areaZ[i + 1] += commonZ;
        result.areaR[i] += kPi * length -
                           kPi * radiusSum * dr / length;
        result.areaR[i + 1] += kPi * length +
                               kPi * radiusSum * dr / length;

        // The profile is kept ordered in z, so each segment is an exact
        // conical frustum for volume and its analytic gradient.
        float shape = a.r * a.r + a.r * b.r + b.r * b.r;
        float volume = (kPi / 3.0f) * dz * shape;
        result.volume += volume;
        float zGradient = (kPi / 3.0f) * shape;
        result.volumeZ[i] -= zGradient;
        result.volumeZ[i + 1] += zGradient;
        result.volumeR[i] += (kPi / 3.0f) * dz *
                             (2.0f * a.r + b.r);
        result.volumeR[i + 1] += (kPi / 3.0f) * dz *
                                 (a.r + 2.0f * b.r);
    }
    return result;
}

void UpdateMassAndCurvature(std::vector<FusionProfileNode>& nodes)
{
    if (nodes.size() < 3) {
        return;
    }
    std::vector<float> areaWeight(nodes.size(), 0.0f);
    float totalArea = 0.0f;
    for (size_t i = 0; i + 1 < nodes.size(); ++i) {
        float dz = nodes[i + 1].z - nodes[i].z;
        float dr = nodes[i + 1].r - nodes[i].r;
        float length = std::max(std::sqrt(dz * dz + dr * dr), 1e-6f);
        float area = kPi * std::max(nodes[i].r + nodes[i + 1].r, 0.0f) *
                     length;
        areaWeight[i] += 0.5f * area;
        areaWeight[i + 1] += 0.5f * area;
        totalArea += area;
    }
    // A polar cap has small, but not zero, represented area.  The old 0.15%
    // floor made the cap behave like an ordinary ring and polluted pressure
    // weighting. Keep only a numerical floor here.
    float massFloor = std::max(totalArea * 1e-6f, 1e-9f);
    for (size_t i = 0; i < nodes.size(); ++i) {
        nodes[i].mass = std::max(areaWeight[i], massFloor);
    }

    for (size_t i = 1; i + 1 < nodes.size(); ++i) {
        float dz0 = nodes[i].z - nodes[i - 1].z;
        float dr0 = nodes[i].r - nodes[i - 1].r;
        float dz1 = nodes[i + 1].z - nodes[i].z;
        float dr1 = nodes[i + 1].r - nodes[i].r;
        float l0 = std::max(std::sqrt(dz0 * dz0 + dr0 * dr0), 1e-6f);
        float l1 = std::max(std::sqrt(dz1 * dz1 + dr1 * dr1), 1e-6f);
        float tz0 = dz0 / l0;
        float tr0 = dr0 / l0;
        float tz1 = dz1 / l1;
        float tr1 = dr1 / l1;
        float averageLength = 0.5f * (l0 + l1);
        float tangentZ = 0.5f * (tz0 + tz1);
        float tangentR = 0.5f * (tr0 + tr1);
        float tangentLength = std::max(
            std::sqrt(tangentZ * tangentZ + tangentR * tangentR),
            1e-6f);
        tangentZ /= tangentLength;
        tangentR /= tangentLength;
        float normalZ = -tangentR;
        float normalR = tangentZ;
        float meridianCurvature =
            -((tz1 - tz0) * normalZ +
              (tr1 - tr0) * normalR) /
            std::max(averageLength, 1e-6f);
        float azimuthCurvature = tangentZ /
                                 std::max(nodes[i].r, 1e-6f);
        // Near the axis both principal curvatures have the same smooth
        // limit. Blend to that limit instead of dividing by a tiny radius.
        float polarScale = std::max(std::min(l0, l1), 1e-6f);
        float polarBlend = std::clamp(nodes[i].r / polarScale - 0.35f,
                                      0.0f, 1.0f);
        azimuthCurvature = meridianCurvature * (1.0f - polarBlend) +
                           azimuthCurvature * polarBlend;
        nodes[i].curvature = meridianCurvature + azimuthCurvature;
    }
    // Analytic smooth-pole limit. For a local circular cap,
    // k_meridional + k_azimuthal = 4*axialRise/chordLength^2.
    auto polarCurvature = [](float axialRise0, float radial0,
                             float axialRise1, float radial1) {
        float chord0 = axialRise0 * axialRise0 + radial0 * radial0;
        float chord1 = axialRise1 * axialRise1 + radial1 * radial1;
        float k0 = 4.0f * std::max(axialRise0, 0.0f) /
                   std::max(chord0, 1e-8f);
        float k1 = 4.0f * std::max(axialRise1, 0.0f) /
                   std::max(chord1, 1e-8f);
        return 0.65f * k0 + 0.35f * k1;
    };
    float leftDz = nodes[1].z - nodes[0].z;
    float leftDz2 = nodes[2].z - nodes[0].z;
    nodes.front().curvature = polarCurvature(
        leftDz, nodes[1].r, leftDz2, nodes[2].r);
    size_t last = nodes.size() - 1;
    float rightDz = nodes[last].z - nodes[last - 1].z;
    float rightDz2 = nodes[last].z - nodes[last - 2].z;
    nodes.back().curvature = polarCurvature(
        rightDz, nodes[last - 1].r, rightDz2, nodes[last - 2].r);
}

void EnforceSmoothPoleBoundary(std::vector<FusionProfileNode>& nodes)
{
    if (nodes.size() < 5)
        return;
    float leftDenominator = nodes[2].r * nodes[2].r -
                            nodes[1].r * nodes[1].r;
    if (leftDenominator > 1e-8f) {
        float coefficient = std::clamp(
            (nodes[2].z - nodes[1].z) / leftDenominator,
            0.0f, 1e4f);
        nodes[0].z = nodes[1].z - coefficient *
                     nodes[1].r * nodes[1].r;
        nodes[0].vz = nodes[1].vz - 2.0f * coefficient *
                      nodes[1].r * nodes[1].vr;
    }
    size_t last = nodes.size() - 1;
    float rightDenominator = nodes[last - 2].r * nodes[last - 2].r -
                             nodes[last - 1].r * nodes[last - 1].r;
    if (rightDenominator > 1e-8f) {
        float coefficient = std::clamp(
            (nodes[last - 1].z - nodes[last - 2].z) /
                rightDenominator,
            0.0f, 1e4f);
        nodes[last].z = nodes[last - 1].z + coefficient *
                        nodes[last - 1].r * nodes[last - 1].r;
        nodes[last].vz = nodes[last - 1].vz + 2.0f * coefficient *
                         nodes[last - 1].r * nodes[last - 1].vr;
    }
    nodes.front().r = 0.0f;
    nodes.front().vr = 0.0f;
    nodes.back().r = 0.0f;
    nodes.back().vr = 0.0f;
}

void EnforceProfileOrdering(std::vector<FusionProfileNode>& nodes)
{
    if (nodes.size() < 2) {
        return;
    }
    float span = std::max(nodes.back().z - nodes.front().z, 0.01f);
    // Only prevent inversion. A smooth polar cap needs axial rise O(r^2),
    // so a uniform linear spacing floor would manufacture a cone tip.
    float minimumSpacing = span / (float)(nodes.size() - 1) * 0.002f;
    nodes.front().r = 0.0f;
    nodes.front().vr = 0.0f;
    for (size_t i = 1; i < nodes.size(); ++i) {
        float minimumZ = nodes[i - 1].z + minimumSpacing;
        if (nodes[i].z < minimumZ) {
            nodes[i].z = minimumZ;
            nodes[i].vz = std::max(nodes[i].vz, nodes[i - 1].vz);
        }
        if (i + 1 < nodes.size()) {
            nodes[i].r = std::max(nodes[i].r, 1e-5f);
        }
    }
    nodes.back().r = 0.0f;
    nodes.back().vr = 0.0f;
    EnforceSmoothPoleBoundary(nodes);
}

float EdgeLengthRatio(const std::vector<FusionProfileNode>& nodes)
{
    float minimum = 1e30f;
    float maximum = 0.0f;
    for (size_t i = 0; i + 1 < nodes.size(); ++i) {
        float length = std::hypot(nodes[i + 1].z - nodes[i].z,
                                  nodes[i + 1].r - nodes[i].r);
        minimum = std::min(minimum, length);
        maximum = std::max(maximum, length);
    }
    return maximum / std::max(minimum, 1e-7f);
}

void RedistributeTangentially(std::vector<FusionProfileNode>& nodes,
                              float blend,
                              float curvatureWeight)
{
    if (nodes.size() < 3 || blend <= 0.0f) {
        return;
    }
    std::vector<FusionProfileNode> source = nodes;
    std::vector<float> arc(source.size(), 0.0f);
    for (size_t i = 1; i < source.size(); ++i) {
        float dz = source[i].z - source[i - 1].z;
        float dr = source[i].r - source[i - 1].r;
        float length = std::sqrt(dz * dz + dr * dr);
        float localCurvature = 0.5f *
            (std::abs(source[i - 1].curvature) +
             std::abs(source[i].curvature));
        float metricWeight = 1.0f + std::min(
            curvatureWeight * localCurvature * length, 2.0f);
        arc[i] = arc[i - 1] + length * metricWeight;
    }
    float totalArc = arc.back();
    if (totalArc <= 1e-6f) {
        return;
    }
    size_t segment = 0;
    for (size_t i = 1; i + 1 < nodes.size(); ++i) {
        float desiredArc = totalArc * (float)i /
                           (float)(nodes.size() - 1);
        while (segment + 1 < arc.size() - 1 &&
               arc[segment + 1] < desiredArc) {
            ++segment;
        }
        float segmentArc = std::max(
            arc[segment + 1] - arc[segment], 1e-7f);
        float t = std::clamp(
            (desiredArc - arc[segment]) / segmentArc, 0.0f, 1.0f);
        const FusionProfileNode& a = source[segment];
        const FusionProfileNode& b = source[segment + 1];
        auto sample = [t](float x, float y) {
            return x + (y - x) * t;
        };
        auto approach = [blend](float current, float target) {
            return current + (target - current) * blend;
        };
        nodes[i].z = approach(nodes[i].z, sample(a.z, b.z));
        nodes[i].r = approach(nodes[i].r, sample(a.r, b.r));
        nodes[i].vz = approach(nodes[i].vz, sample(a.vz, b.vz));
        nodes[i].vr = approach(nodes[i].vr, sample(a.vr, b.vr));
        nodes[i].filmAxial = approach(
            nodes[i].filmAxial,
            sample(a.filmAxial, b.filmAxial));
        nodes[i].filmRadial = approach(
            nodes[i].filmRadial,
            sample(a.filmRadial, b.filmRadial));
        nodes[i].opticalRadiusScale = approach(
            nodes[i].opticalRadiusScale,
            sample(a.opticalRadiusScale, b.opticalRadiusScale));
    }
}

} // namespace

float FusionProfileVolume(const std::vector<FusionProfileNode>& nodes)
{
    return std::max(ComputeGradients(nodes).volume, 0.0f);
}

float FusionProfileArea(const std::vector<FusionProfileNode>& nodes)
{
    return ComputeGradients(nodes).area;
}

float FusionProfileCentroidZ(const std::vector<FusionProfileNode>& nodes)
{
    float volume = 0.0f;
    float firstMoment = 0.0f;
    for (size_t i = 0; i + 1 < nodes.size(); ++i) {
        const FusionProfileNode& a = nodes[i];
        const FusionProfileNode& b = nodes[i + 1];
        float dz = b.z - a.z;
        float shape = a.r * a.r + a.r * b.r + b.r * b.r;
        float slab = (kPi / 3.0f) * dz * shape;
        // Frustum centroid is approximated at its volume-weighted midpoint;
        // the error is second order at the current profile resolution.
        float denominator = std::max(shape, 1e-8f);
        float localFraction = (a.r * a.r + 2.0f * a.r * b.r +
                               3.0f * b.r * b.r) /
                              (4.0f * denominator);
        float centroid = a.z + dz * localFraction;
        volume += slab;
        firstMoment += slab * centroid;
    }
    return std::abs(volume) > 1e-8f ? firstMoment / volume : 0.0f;
}

void FairInitialFusionProfile(std::vector<FusionProfileNode>& nodes,
                              float neckZ,
                              float fairingHalfWidth,
                              int iterations)
{
    if (nodes.size() < 5 || iterations <= 0)
        return;
    const float targetVolume = FusionProfileVolume(nodes);
    for (int iteration = 0; iteration < iterations; ++iteration) {
        UpdateMassAndCurvature(nodes);
        std::vector<float> filtered(nodes.size(), 0.0f);
        filtered.front() = nodes.front().curvature;
        filtered.back() = nodes.back().curvature;
        float meanEdge = 0.0f;
        for (size_t i = 0; i + 1 < nodes.size(); ++i)
            meanEdge += std::hypot(nodes[i + 1].z - nodes[i].z,
                                   nodes[i + 1].r - nodes[i].r);
        meanEdge /= static_cast<float>(nodes.size() - 1);
        for (size_t i = 1; i + 1 < nodes.size(); ++i) {
            float l0 = std::max(std::hypot(nodes[i].z - nodes[i - 1].z,
                                          nodes[i].r - nodes[i - 1].r), 1e-6f);
            float l1 = std::max(std::hypot(nodes[i + 1].z - nodes[i].z,
                                          nodes[i + 1].r - nodes[i].r), 1e-6f);
            filtered[i] = (nodes[i - 1].curvature / l0 +
                           2.0f * nodes[i].curvature * (1.0f / l0 + 1.0f / l1) +
                           nodes[i + 1].curvature / l1) /
                          (3.0f * (1.0f / l0 + 1.0f / l1));
        }
        for (size_t i = 1; i + 1 < nodes.size(); ++i) {
            float distance = std::abs(nodes[i].z - neckZ) /
                             std::max(fairingHalfWidth, 1e-5f);
            float window = 1.0f - std::clamp(distance, 0.0f, 1.0f);
            window = window * window * (3.0f - 2.0f * window);
            ProfileFrame frame = ComputeFrame(nodes, i);
            float displacement = 0.12f * meanEdge * meanEdge *
                                 (filtered[i] - nodes[i].curvature) * window;
            displacement = std::clamp(displacement,
                                      -0.08f * meanEdge,
                                      0.08f * meanEdge);
            nodes[i].z += displacement * frame.normalZ;
            nodes[i].r += displacement * frame.normalR;
        }
        EnforceProfileOrdering(nodes);

        // Global normal projection restores exactly the volume present before
        // fairing; polar caps participate only through their true area mass.
        UpdateMassAndCurvature(nodes);
        for (int projectionIteration = 0;
             projectionIteration < 2; ++projectionIteration) {
            ProfileGradients gradients = ComputeGradients(nodes);
            float denominator = 0.0f;
            std::vector<ProfileFrame> frames(nodes.size());
            std::vector<float> normalGradient(nodes.size(), 0.0f);
            for (size_t i = 0; i < nodes.size(); ++i) {
                frames[i] = ComputeFrame(nodes, i);
                normalGradient[i] =
                    gradients.volumeZ[i] * frames[i].normalZ +
                    gradients.volumeR[i] * frames[i].normalR;
                denominator += normalGradient[i] * normalGradient[i] /
                               std::max(nodes[i].mass, 1e-9f);
            }
            float multiplier = (targetVolume - gradients.volume) /
                               std::max(denominator, 1e-9f);
            for (size_t i = 0; i < nodes.size(); ++i) {
                float displacement = multiplier * normalGradient[i] /
                                     std::max(nodes[i].mass, 1e-9f);
                nodes[i].z += displacement * frames[i].normalZ;
                if (i > 0 && i + 1 < nodes.size())
                    nodes[i].r += displacement * frames[i].normalR;
            }
            EnforceProfileOrdering(nodes);
        }
    }
}

FusionProfileDiagnostics AdvanceFusionProfile(
    std::vector<FusionProfileNode>& nodes,
    float targetVolume,
    float dt,
    const FusionProfileConfig& config)
{
    FusionProfileDiagnostics diagnostics;
    if (nodes.size() < 3 || dt <= 0.0f || targetVolume <= 0.0f) {
        diagnostics.volume = FusionProfileVolume(nodes);
        diagnostics.area = FusionProfileArea(nodes);
        diagnostics.centroidZ = FusionProfileCentroidZ(nodes);
        return diagnostics;
    }

    float boundedDt = std::min(dt, config.maximumTimeStep *
                                   (float)config.maximumSubsteps);
    int substeps = std::clamp(
        (int)std::ceil(boundedDt / config.maximumTimeStep),
        1, config.maximumSubsteps);
    float subDt = boundedDt / (float)substeps;

    for (FusionProfileNode& node : nodes) {
        node.normalForceZ = 0.0f;
        node.normalForceR = 0.0f;
        node.volumeCorrection = 0.0f;
    }

    for (int substep = 0; substep < substeps; ++substep) {
        UpdateMassAndCurvature(nodes);
        // Curvature is a geometric field, so suppress grid-scale alternating
        // noise before converting it to capillary force. This stencil reads
        // only the current surface and introduces no target geometry.
        std::vector<float> smoothedCurvature(nodes.size());
        for (int pass = 0; pass < 2; ++pass) {
            smoothedCurvature.front() = nodes.front().curvature;
            smoothedCurvature.back() = nodes.back().curvature;
            for (size_t i = 1; i + 1 < nodes.size(); ++i) {
                float l0 = std::max(std::hypot(
                    nodes[i].z - nodes[i - 1].z,
                    nodes[i].r - nodes[i - 1].r), 1e-6f);
                float l1 = std::max(std::hypot(
                    nodes[i + 1].z - nodes[i].z,
                    nodes[i + 1].r - nodes[i].r), 1e-6f);
                float w0 = 1.0f / l0;
                float w1 = 1.0f / l1;
                float wc = 2.0f * (w0 + w1);
                smoothedCurvature[i] =
                    (w0 * nodes[i - 1].curvature +
                     wc * nodes[i].curvature +
                     w1 * nodes[i + 1].curvature) /
                    (w0 + wc + w1);
            }
            for (size_t i = 0; i < nodes.size(); ++i) {
                nodes[i].curvature = smoothedCurvature[i];
            }
        }
        ProfileGradients gradients = ComputeGradients(nodes);
        std::vector<ProfileFrame> frames(nodes.size());
        for (size_t i = 0; i < nodes.size(); ++i) {
            frames[i] = ComputeFrame(nodes, i);
        }
        float denominator = 0.0f;
        float curvatureMean = 0.0f;
        float curvatureWeight = 0.0f;
        for (size_t i = 0; i < nodes.size(); ++i) {
            float inverseMass = 1.0f / std::max(nodes[i].mass, 1e-7f);
            float normalGradient =
                gradients.volumeZ[i] * frames[i].normalZ +
                gradients.volumeR[i] * frames[i].normalR;
            denominator += normalGradient * normalGradient * inverseMass;
            curvatureMean += nodes[i].curvature * nodes[i].mass;
            curvatureWeight += nodes[i].mass;
        }
        denominator = std::max(denominator, 1e-9f);
        curvatureMean /= std::max(curvatureWeight, 1e-8f);

        size_t radialMaximumIndex = 0;
        for (size_t i = 1; i < nodes.size(); ++i) {
            if (nodes[i].r > nodes[radialMaximumIndex].r) {
                radialMaximumIndex = i;
            }
        }
        float axialHalfExtent =
            0.5f * std::max(nodes.back().z - nodes.front().z, 1e-5f);
        float radialExtent =
            std::max(nodes[radialMaximumIndex].r, 1e-5f);
        float shapeMode = std::log(axialHalfExtent / radialExtent);
        float shapeModeVelocity =
            0.5f * (nodes.back().vz - nodes.front().vz) /
                axialHalfExtent -
            nodes[radialMaximumIndex].vr / radialExtent;
        float modeDrive =
            (-config.globalModeFrequency * config.globalModeFrequency *
                 shapeMode -
             2.0f * config.globalModeDampingRatio *
                 config.globalModeFrequency * shapeModeVelocity) /
            1.5f;

        std::vector<float> meshForceZ(nodes.size(), 0.0f);
        std::vector<float> meshForceR(nodes.size(), 0.0f);
        std::vector<float> viscousZ(nodes.size(), 0.0f);
        std::vector<float> viscousR(nodes.size(), 0.0f);
        for (size_t i = 1; i + 1 < nodes.size(); ++i) {
            viscousZ[i] = config.velocityLaplacianDamping *
                          (nodes[i - 1].vz - 2.0f * nodes[i].vz +
                           nodes[i + 1].vz);
            viscousR[i] = config.velocityLaplacianDamping *
                          (nodes[i - 1].vr - 2.0f * nodes[i].vr +
                           nodes[i + 1].vr);
        }
        float meanEdgeLength = 0.0f;
        for (size_t i = 0; i + 1 < nodes.size(); ++i) {
            float dz = nodes[i + 1].z - nodes[i].z;
            float dr = nodes[i + 1].r - nodes[i].r;
            meanEdgeLength += std::sqrt(dz * dz + dr * dr);
        }
        meanEdgeLength /= (float)(nodes.size() - 1);
        for (size_t i = 0; i + 1 < nodes.size(); ++i) {
            float dz = nodes[i + 1].z - nodes[i].z;
            float dr = nodes[i + 1].r - nodes[i].r;
            float length = std::max(std::sqrt(dz * dz + dr * dr),
                                    1e-6f);
            float spring = config.meshRegularization *
                           (length - meanEdgeLength);
            float forceZ = spring * dz / length;
            float forceR = spring * dr / length;
            meshForceZ[i] += forceZ;
            meshForceR[i] += forceR;
            meshForceZ[i + 1] -= forceZ;
            meshForceR[i + 1] -= forceR;
        }

        for (size_t i = 0; i < nodes.size(); ++i) {
            float inverseMass = 1.0f / std::max(nodes[i].mass, 1e-7f);
            float tangentZ = frames[i].tangentZ;
            float tangentR = frames[i].tangentR;
            float normalZ = frames[i].normalZ;
            float normalR = frames[i].normalR;
            float curvatureAcceleration =
                config.surfaceTension *
                (curvatureMean - nodes[i].curvature);
            float tangentialMeshForce =
                meshForceZ[i] * tangentZ +
                meshForceR[i] * tangentR;
            float modeZ = modeDrive * nodes[i].z;
            float modeR = -0.5f * modeDrive * nodes[i].r;
            float modeNormal = modeZ * normalZ + modeR * normalR;
            float totalNormalAcceleration = curvatureAcceleration + modeNormal;
            float forceZ = nodes[i].mass * totalNormalAcceleration * normalZ -
                           config.velocityDamping * nodes[i].mass * nodes[i].vz +
                           tangentialMeshForce * tangentZ +
                           nodes[i].mass * viscousZ[i];
            float forceR = nodes[i].mass * totalNormalAcceleration * normalR -
                           config.velocityDamping * nodes[i].mass * nodes[i].vr +
                           tangentialMeshForce * tangentR +
                           nodes[i].mass * viscousR[i];
            if (i == 0 || i + 1 == nodes.size()) {
                forceR = 0.0f;
            }
            nodes[i].normalForceZ =
                nodes[i].mass * totalNormalAcceleration * normalZ;
            nodes[i].normalForceR =
                nodes[i].mass * totalNormalAcceleration * normalR;
            nodes[i].vz += forceZ * inverseMass * subDt;
            nodes[i].vr += forceR * inverseMass * subDt;
            float speed = std::sqrt(nodes[i].vz * nodes[i].vz +
                                    nodes[i].vr * nodes[i].vr);
            if (speed > config.maximumNodeSpeed) {
                float scale = config.maximumNodeSpeed / speed;
                nodes[i].vz *= scale;
                nodes[i].vr *= scale;
            }
        }
        // Lagrange multiplier pressure impulse: remove precisely the global
        // volume-changing component of velocity without referencing a shape.
        float volumeRate = 0.0f;
        for (size_t i = 0; i < nodes.size(); ++i) {
            volumeRate += gradients.volumeZ[i] * nodes[i].vz +
                          gradients.volumeR[i] * nodes[i].vr;
        }
        float velocityMultiplier = -volumeRate / denominator;
        for (size_t i = 0; i < nodes.size(); ++i) {
            float inverseMass = 1.0f / std::max(nodes[i].mass, 1e-7f);
            float normalGradient =
                gradients.volumeZ[i] * frames[i].normalZ +
                gradients.volumeR[i] * frames[i].normalR;
            float impulse = velocityMultiplier * normalGradient * inverseMass;
            nodes[i].vz += impulse * frames[i].normalZ;
            nodes[i].vr += impulse * frames[i].normalR;
            float speed = std::sqrt(nodes[i].vz * nodes[i].vz +
                                    nodes[i].vr * nodes[i].vr);
            if (speed > config.maximumNodeSpeed) {
                float scale = config.maximumNodeSpeed / speed;
                nodes[i].vz *= scale;
                nodes[i].vr *= scale;
            }
        }
        nodes.front().vr = 0.0f;
        nodes.back().vr = 0.0f;
        for (FusionProfileNode& node : nodes) {
            node.z += node.vz * subDt;
            node.r += node.vr * subDt;
        }
        EnforceProfileOrdering(nodes);
        if (EdgeLengthRatio(nodes) > config.resamplingEdgeRatio) {
            RedistributeTangentially(
                nodes,
                std::min(config.tangentialRedistributionRate * subDt * 8.0f,
                         0.35f),
                config.resamplingCurvatureWeight);
            EnforceProfileOrdering(nodes);
        }

        // Position-level pressure projection removes accumulated integration
        // drift. It follows the volume gradient; it never references a sphere.
        for (int iteration = 0;
             iteration < config.volumeProjectionIterations;
             ++iteration) {
            ProfileGradients projection = ComputeGradients(nodes);
            UpdateMassAndCurvature(nodes);
            std::vector<ProfileFrame> projectionFrames(nodes.size());
            for (size_t i = 0; i < nodes.size(); ++i) {
                projectionFrames[i] = ComputeFrame(nodes, i);
            }
            float projectionDenominator = 0.0f;
            for (size_t i = 0; i < nodes.size(); ++i) {
                float inverseMass = 1.0f / std::max(nodes[i].mass, 1e-7f);
                float normalGradient =
                    projection.volumeZ[i] * projectionFrames[i].normalZ +
                    projection.volumeR[i] * projectionFrames[i].normalR;
                projectionDenominator += normalGradient * normalGradient *
                                         inverseMass;
            }
            if (projectionDenominator <= 1e-9f) {
                break;
            }
            float multiplier = -(projection.volume - targetVolume) /
                               projectionDenominator *
                               config.volumeConstraintStrength;
            for (size_t i = 0; i < nodes.size(); ++i) {
                float inverseMass = 1.0f / std::max(nodes[i].mass, 1e-7f);
                float normalGradient =
                    projection.volumeZ[i] * projectionFrames[i].normalZ +
                    projection.volumeR[i] * projectionFrames[i].normalR;
                float displacement = multiplier * normalGradient * inverseMass;
                nodes[i].z += displacement * projectionFrames[i].normalZ;
                if (i > 0 && i + 1 < nodes.size())
                    nodes[i].r += displacement * projectionFrames[i].normalR;
                nodes[i].volumeCorrection += std::abs(displacement);
            }
            EnforceProfileOrdering(nodes);
        }

        // Local coordinates stay centred on the actual gas-volume centroid.
        float centroid = FusionProfileCentroidZ(nodes);
        for (FusionProfileNode& node : nodes) {
            node.z -= centroid;
        }
    }

    UpdateMassAndCurvature(nodes);
    diagnostics.volume = FusionProfileVolume(nodes);
    diagnostics.area = FusionProfileArea(nodes);
    diagnostics.centroidZ = FusionProfileCentroidZ(nodes);
    float speedSquared = 0.0f;
    float curvatureMean = 0.0f;
    float curvatureWeight = 0.0f;
    for (size_t i = 0; i < nodes.size(); ++i) {
        speedSquared += nodes[i].vz * nodes[i].vz +
                        nodes[i].vr * nodes[i].vr;
        if (i > 0 && i + 1 < nodes.size()) {
            curvatureMean += nodes[i].curvature * nodes[i].mass;
            curvatureWeight += nodes[i].mass;
        }
    }
    diagnostics.rmsSpeed = std::sqrt(speedSquared /
                                     (float)nodes.size());
    curvatureMean /= std::max(curvatureWeight, 1e-8f);
    float variance = 0.0f;
    for (size_t i = 1; i + 1 < nodes.size(); ++i) {
        float difference = nodes[i].curvature - curvatureMean;
        variance += difference * difference * nodes[i].mass;
    }
    diagnostics.curvatureVariation =
        std::sqrt(variance / std::max(curvatureWeight, 1e-8f)) /
        std::max(std::abs(curvatureMean), 1e-5f);

    diagnostics.minEdgeLength = 1e30f;
    for (size_t i = 0; i + 1 < nodes.size(); ++i) {
        float edge = std::hypot(nodes[i + 1].z - nodes[i].z,
                                nodes[i + 1].r - nodes[i].r);
        diagnostics.minEdgeLength = std::min(diagnostics.minEdgeLength, edge);
        if (edge > diagnostics.maxEdgeLength) {
            diagnostics.maxEdgeLength = edge;
            diagnostics.maxEdgeNode = static_cast<int>(i);
        }
    }
    diagnostics.edgeLengthRatio = diagnostics.maxEdgeLength /
        std::max(diagnostics.minEdgeLength, 1e-7f);
    for (size_t i = 0; i < nodes.size(); ++i) {
        float curvature = std::abs(nodes[i].curvature);
        if (curvature > diagnostics.maxAbsCurvature) {
            diagnostics.maxAbsCurvature = curvature;
            diagnostics.maxCurvatureNode = static_cast<int>(i);
        }
        float force = std::hypot(nodes[i].normalForceZ,
                                 nodes[i].normalForceR);
        if (force > diagnostics.maxNormalForce) {
            diagnostics.maxNormalForce = force;
            diagnostics.maxNormalForceNode = static_cast<int>(i);
        }
        if (nodes[i].volumeCorrection > diagnostics.maxVolumeCorrection) {
            diagnostics.maxVolumeCorrection = nodes[i].volumeCorrection;
            diagnostics.maxVolumeCorrectionNode = static_cast<int>(i);
        }
        if (i > 0 && i + 1 < nodes.size()) {
            float az = nodes[i - 1].z - nodes[i].z;
            float ar = nodes[i - 1].r - nodes[i].r;
            float bz = nodes[i + 1].z - nodes[i].z;
            float br = nodes[i + 1].r - nodes[i].r;
            float cosine = (az * bz + ar * br) /
                std::max(std::hypot(az, ar) * std::hypot(bz, br), 1e-8f);
            float angle = std::acos(std::clamp(cosine, -1.0f, 1.0f)) *
                          180.0f / kPi;
            if (angle < diagnostics.minInternalAngleDegrees) {
                diagnostics.minInternalAngleDegrees = angle;
                diagnostics.minAngleNode = static_cast<int>(i);
            }
        }
    }
    return diagnostics;
}
