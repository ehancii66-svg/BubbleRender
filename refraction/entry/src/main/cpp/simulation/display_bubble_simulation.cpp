
#include "display_bubble_simulation.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include <glm/glm.hpp>

namespace {

constexpr float kPi = 3.14159265358979323846f;

struct BubbleGridCell {
    int x;
    int y;
    int z;

    bool operator==(const BubbleGridCell& other) const
    {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct BubbleGridCellHash {
    size_t operator()(const BubbleGridCell& cell) const
    {
        size_t hx = std::hash<int>{}(cell.x);
        size_t hy = std::hash<int>{}(cell.y);
        size_t hz = std::hash<int>{}(cell.z);
        return hx ^ (hy << 1) ^ (hz << 7);
    }
};

BubbleGridCell BubbleCellForPosition(const glm::vec3& position, float cellSize)
{
    return {
        (int)std::floor(position.x / cellSize),
        (int)std::floor(position.y / cellSize),
        (int)std::floor(position.z / cellSize)
    };
}

} // namespace

float BubbleVolume(float radius)
{
    return (4.0f / 3.0f) * kPi * radius * radius * radius;
}

float RadiusFromVolume(float volume)
{
    return std::cbrt(std::max(volume, 0.0f) * 3.0f / (4.0f * kPi));
}

float BubbleMass(const DisplayBubble& bubble)
{
    return std::max(BubbleVolume(std::max(bubble.radius, 0.001f)), 0.001f);
}

float BubbleInvMass(const DisplayBubble& bubble)
{
    return 1.0f / BubbleMass(bubble);
}

int FindBubbleIndexById(const std::vector<DisplayBubble>& bubbles, uint64_t id)
{
    if (id == 0) {
        return -1;
    }
    for (size_t i = 0; i < bubbles.size(); ++i) {
        if (bubbles[i].id == id) {
            return (int)i;
        }
    }
    return -1;
}

bool ResolveContactPairIndices(const std::vector<DisplayBubble>& bubbles,
                               const BubbleContactPair& pair,
                               int& aIndex,
                               int& bIndex)
{
    aIndex = FindBubbleIndexById(bubbles, pair.a);
    bIndex = FindBubbleIndexById(bubbles, pair.b);
    return aIndex >= 0 && bIndex >= 0;
}

bool PairContainsBubbleIndex(const std::vector<DisplayBubble>& bubbles,
                             const BubbleContactPair& pair,
                             int bubbleIndex,
                             int* otherIndex)
{
    if (bubbleIndex < 0 || bubbleIndex >= (int)bubbles.size()) {
        return false;
    }
    int aIndex = -1;
    int bIndex = -1;
    if (!ResolveContactPairIndices(bubbles, pair, aIndex, bIndex)) {
        return false;
    }
    if (aIndex == bubbleIndex) {
        if (otherIndex) {
            *otherIndex = bIndex;
        }
        return true;
    }
    if (bIndex == bubbleIndex) {
        if (otherIndex) {
            *otherIndex = aIndex;
        }
        return true;
    }
    return false;
}

int FindContactPair(const std::vector<BubbleContactPair>& pairs, uint64_t a, uint64_t b)
{
    if (a > b) {
        std::swap(a, b);
    }

    for (size_t i = 0; i < pairs.size(); ++i) {
        const auto& pair = pairs[i];
        if (pair.a == a && pair.b == b) {
            return (int)i;
        }
    }
    return -1;
}

int EnsureContactPairById(std::vector<BubbleContactPair>& pairs, uint64_t a, uint64_t b)
{
    if (a == 0 || b == 0 || a == b) {
        return -1;
    }
    if (a > b) {
        std::swap(a, b);
    }

    int existing = FindContactPair(pairs, a, b);
    if (existing >= 0) {
        return existing;
    }

    BubbleContactPair pair;
    pair.a = a;
    pair.b = b;
    pair.filmThickness = 1.0f;
    pair.state = BubbleContactPair::State::Free;
    pair.active = false;
    pairs.push_back(pair);
    return (int)pairs.size() - 1;
}

int EnsureContactPairByIndex(const std::vector<DisplayBubble>& bubbles,
                             std::vector<BubbleContactPair>& pairs,
                             int aIndex,
                             int bIndex)
{
    if (aIndex < 0 || bIndex < 0 ||
        aIndex >= (int)bubbles.size() ||
        bIndex >= (int)bubbles.size()) {
        return -1;
    }
    return EnsureContactPairById(pairs, bubbles[(size_t)aIndex].id, bubbles[(size_t)bIndex].id);
}

std::vector<std::pair<int, int>> BuildBubbleBroadPhasePairs(const std::vector<DisplayBubble>& bubbles)
{
    std::vector<std::pair<int, int>> pairs;
    if (bubbles.size() < 2) {
        return pairs;
    }

    float maximumRadius = 0.001f;
    for (const auto& bubble : bubbles) {
        if (bubble.state != DisplayBubble::State::Dead) {
            maximumRadius = std::max(maximumRadius, bubble.radius);
        }
    }
    float cellSize = maximumRadius * 2.40f;

    std::unordered_map<BubbleGridCell, std::vector<int>, BubbleGridCellHash> grid;
    grid.reserve(bubbles.size() * 2);
    for (size_t i = 0; i < bubbles.size(); ++i) {
        const auto& bubble = bubbles[i];
        if (bubble.state == DisplayBubble::State::Dead) {
            continue;
        }
        grid[BubbleCellForPosition(bubble.position, cellSize)].push_back((int)i);
    }

    pairs.reserve(bubbles.size() * 4);
    for (size_t i = 0; i < bubbles.size(); ++i) {
        const auto& bubble = bubbles[i];
        if (bubble.state == DisplayBubble::State::Dead) {
            continue;
        }
        BubbleGridCell centerCell = BubbleCellForPosition(bubble.position, cellSize);
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    BubbleGridCell neighbor{
                        centerCell.x + dx,
                        centerCell.y + dy,
                        centerCell.z + dz
                    };
                    auto found = grid.find(neighbor);
                    if (found == grid.end()) {
                        continue;
                    }
                    for (int j : found->second) {
                        if (j > (int)i) {
                            pairs.emplace_back((int)i, j);
                        }
                    }
                }
            }
        }
    }
    return pairs;
}

int BubbleActiveContactCount(const std::vector<DisplayBubble>& bubbles,
                             const std::vector<BubbleContactPair>& pairs,
                             int bubbleIndex)
{
    int count = 0;
    for (const BubbleContactPair& pair : pairs) {
        if ((pair.candidate || pair.bonded) &&
            PairContainsBubbleIndex(bubbles, pair, bubbleIndex)) {
            ++count;
        }
    }
    return std::max(count, 1);
}

std::vector<DisplayBubble::SurfaceControl> MakeSurfaceControls(float phase)
{
    std::vector<DisplayBubble::SurfaceControl> controls;
    controls.reserve(26);

    int index = 0;
    for (int z = -1; z <= 1; ++z) {
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                if (x == 0 && y == 0 && z == 0) {
                    continue;
                }
                DisplayBubble::SurfaceControl control{};
                control.localDir = glm::normalize(glm::vec3((float)x, (float)y, (float)z));
                float initialOffset = std::sin(phase + (float)index * 1.37f) * 0.0035f;
                control.displacement = control.localDir * initialOffset;
                control.velocity = glm::vec3(0.0f);
                controls.push_back(control);
                ++index;
            }
        }
    }

    return controls;
}

