#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

#include "../../main/cpp/simulation/fusion_profile.h"

namespace {

float ShapeMode(const std::vector<FusionProfileNode>& nodes)
{
    float axial = 0.5f * (nodes.back().z - nodes.front().z);
    float radial = 1e-6f;
    for (const FusionProfileNode& node : nodes)
        radial = std::max(radial, node.r);
    return std::log(std::max(axial, 1e-6f) / radial);
}

bool RunCase(float sizeRatio, float dt)
{
    std::vector<FusionProfileNode> nodes(97);
    float finalRadius = std::cbrt(1.0f + sizeRatio * sizeRatio * sizeRatio);
    float axialRadius = finalRadius * 1.28f;
    float radialRadius = std::sqrt(
        finalRadius * finalRadius * finalRadius / axialRadius);
    constexpr float pi = 3.14159265358979323846f;
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        float theta = pi *
            (1.0f - static_cast<float>(i) /
                        static_cast<float>(nodes.size() - 1));
        nodes[i].z = axialRadius * std::cos(theta);
        nodes[i].r = radialRadius * std::sin(theta);
        nodes[i].initialZ = nodes[i].z;
        nodes[i].initialR = nodes[i].r;
    }

    float targetVolume = FusionProfileVolume(nodes);
    float maximumRelativeVolumeError = 0.0f;
    float previousMode = ShapeMode(nodes);
    int zeroCrossings = 0;
    FusionProfileConfig config;
    FusionProfileDiagnostics diagnostics;
    for (float time = 0.0f; time < 8.0f; time += dt)
    {
        diagnostics = AdvanceFusionProfile(
            nodes, targetVolume, dt, config);
        maximumRelativeVolumeError = std::max(
            maximumRelativeVolumeError,
            std::abs(diagnostics.volume - targetVolume) / targetVolume);
        float currentMode = ShapeMode(nodes);
        if (previousMode * currentMode < 0.0f)
            ++zeroCrossings;
        previousMode = currentMode;
    }

    std::cout << "ratio=" << sizeRatio << " dt=" << dt
              << " maxVolumeError=" << maximumRelativeVolumeError
              << " zeroCrossings=" << zeroCrossings
              << " finalMode=" << ShapeMode(nodes)
              << " rmsSpeed=" << diagnostics.rmsSpeed
              << " edgeRatio=" << diagnostics.edgeLengthRatio
              << " poleVr=" << nodes.front().vr << ","
              << nodes.back().vr << "\n";
    return maximumRelativeVolumeError < 0.005f &&
           nodes.front().r == 0.0f && nodes.back().r == 0.0f &&
           nodes.front().vr == 0.0f && nodes.back().vr == 0.0f;
}

} // namespace

int main()
{
    bool passed = true;
    passed &= RunCase(1.0f, 1.0f / 60.0f);
    passed &= RunCase(2.0f, 1.0f / 60.0f);
    passed &= RunCase(4.0f, 1.0f / 60.0f);
    passed &= RunCase(2.0f, 1.0f / 30.0f);
    return passed ? 0 : 1;
}
