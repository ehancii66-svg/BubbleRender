
#ifndef WINDOWS_RENDER_BUBBLE_SURFACE_SYSTEM_H
#define WINDOWS_RENDER_BUBBLE_SURFACE_SYSTEM_H

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "mesh.h"

class Model;

class BubbleSurfaceSystem {
public:
    BubbleSurfaceSystem() = default;
    ~BubbleSurfaceSystem() = default;

    BubbleSurfaceSystem(const BubbleSurfaceSystem&) = delete;
    BubbleSurfaceSystem& operator=(const BubbleSurfaceSystem&) = delete;

    Model* EnsureBubble(uint64_t bubbleId,
                        const std::vector<Vertex>& vertices,
                        const std::vector<unsigned int>& indices);
    Model* EnsureContact(uint64_t bubbleA,
                         uint64_t bubbleB,
                         const std::vector<Vertex>& vertices,
                         const std::vector<unsigned int>& indices);
    Model* EnsureFusion(uint64_t bubbleA,
                        uint64_t bubbleB,
                        const std::vector<Vertex>& vertices,
                        const std::vector<unsigned int>& indices);

    Model* FindBubble(uint64_t bubbleId);
    Model* FindContact(uint64_t bubbleA, uint64_t bubbleB);
    Model* FindFusion(uint64_t bubbleA, uint64_t bubbleB);
    const std::vector<Vertex>* FindBubbleRestVertices(uint64_t bubbleId) const;
    bool BubbleUsesWorldScale(uint64_t bubbleId) const;

    bool PromoteFusion(uint64_t bubbleA,
                       uint64_t bubbleB,
                       uint64_t survivorId,
                       uint64_t absorbedId);
    void RemoveBubble(uint64_t bubbleId);
    void RemoveContact(uint64_t bubbleA, uint64_t bubbleB);
    void Clear();

private:
    struct PairKey {
        uint64_t first = 0;
        uint64_t second = 0;

        bool operator==(const PairKey& other) const
        {
            return first == other.first && second == other.second;
        }
    };

    struct PairKeyHash {
        size_t operator()(const PairKey& key) const;
    };

    static PairKey MakePairKey(uint64_t bubbleA, uint64_t bubbleB);
    static Model* EnsureModel(
        std::unordered_map<PairKey, std::unique_ptr<Model>, PairKeyHash>& models,
        const PairKey& key,
        const std::vector<Vertex>& vertices,
        const std::vector<unsigned int>& indices);

    std::unordered_map<uint64_t, std::unique_ptr<Model>> bubbleModels_;
    std::unordered_map<uint64_t, std::vector<Vertex>> bubbleRestVertices_;
    std::unordered_map<uint64_t, bool> bubbleWorldScale_;
    std::unordered_map<PairKey, std::unique_ptr<Model>, PairKeyHash> contactModels_;
    std::unordered_map<PairKey, std::unique_ptr<Model>, PairKeyHash> fusionModels_;
};

#endif

