#include "bubble_surface_system.h"

#include <algorithm>

#include "model.h"

size_t BubbleSurfaceSystem::PairKeyHash::operator()(const PairKey& key) const
{
    size_t firstHash = std::hash<uint64_t>{}(key.first);
    size_t secondHash = std::hash<uint64_t>{}(key.second);
    return firstHash ^ (secondHash + 0x9e3779b9u + (firstHash << 6u) + (firstHash >> 2u));
}

BubbleSurfaceSystem::PairKey BubbleSurfaceSystem::MakePairKey(uint64_t bubbleA,
                                                              uint64_t bubbleB)
{
    return bubbleA <= bubbleB
        ? PairKey{bubbleA, bubbleB}
        : PairKey{bubbleB, bubbleA};
}

Model* BubbleSurfaceSystem::EnsureModel(
    std::unordered_map<PairKey, std::unique_ptr<Model>, PairKeyHash>& models,
    const PairKey& key,
    const std::vector<Vertex>& vertices,
    const std::vector<unsigned int>& indices)
{
    auto it = models.find(key);
    if (it != models.end()) {
        return it->second.get();
    }

    std::unique_ptr<Model> model(Model::CreateFromVertices(vertices, indices));
    Model* result = model.get();
    models.emplace(key, std::move(model));
    return result;
}

Model* BubbleSurfaceSystem::EnsureBubble(uint64_t bubbleId,
                                         const std::vector<Vertex>& vertices,
                                         const std::vector<unsigned int>& indices)
{
    auto it = bubbleModels_.find(bubbleId);
    if (it != bubbleModels_.end()) {
        return it->second.get();
    }

    std::unique_ptr<Model> model(Model::CreateFromVertices(vertices, indices));
    Model* result = model.get();
    bubbleModels_.emplace(bubbleId, std::move(model));
    bubbleRestVertices_[bubbleId] = vertices;
    return result;
}

Model* BubbleSurfaceSystem::EnsureContact(uint64_t bubbleA,
                                          uint64_t bubbleB,
                                          const std::vector<Vertex>& vertices,
                                          const std::vector<unsigned int>& indices)
{
    return EnsureModel(contactModels_, MakePairKey(bubbleA, bubbleB), vertices, indices);
}

Model* BubbleSurfaceSystem::EnsureFusion(uint64_t bubbleA,
                                         uint64_t bubbleB,
                                         const std::vector<Vertex>& vertices,
                                         const std::vector<unsigned int>& indices)
{
    return EnsureModel(fusionModels_, MakePairKey(bubbleA, bubbleB), vertices, indices);
}

Model* BubbleSurfaceSystem::FindBubble(uint64_t bubbleId)
{
    auto it = bubbleModels_.find(bubbleId);
    return it == bubbleModels_.end() ? nullptr : it->second.get();
}

Model* BubbleSurfaceSystem::FindContact(uint64_t bubbleA, uint64_t bubbleB)
{
    auto it = contactModels_.find(MakePairKey(bubbleA, bubbleB));
    return it == contactModels_.end() ? nullptr : it->second.get();
}

Model* BubbleSurfaceSystem::FindFusion(uint64_t bubbleA, uint64_t bubbleB)
{
    auto it = fusionModels_.find(MakePairKey(bubbleA, bubbleB));
    return it == fusionModels_.end() ? nullptr : it->second.get();
}

const std::vector<Vertex>* BubbleSurfaceSystem::FindBubbleRestVertices(
    uint64_t bubbleId) const
{
    auto it = bubbleRestVertices_.find(bubbleId);
    return it == bubbleRestVertices_.end() ? nullptr : &it->second;
}

bool BubbleSurfaceSystem::PromoteFusion(uint64_t bubbleA,
                                        uint64_t bubbleB,
                                        uint64_t survivorId,
                                        uint64_t absorbedId,
                                        float survivorRadius)
{
    PairKey key = MakePairKey(bubbleA, bubbleB);
    auto fusionIt = fusionModels_.find(key);
    if (fusionIt == fusionModels_.end()) {
        return false;
    }

    Model* promotedModel = fusionIt->second.get();
    if (Mesh* mesh = promotedModel->getMesh(0)) {
        std::vector<Vertex> normalizedVertices = mesh->vertices;
        float inverseRadius = 1.0f / std::max(survivorRadius, 0.001f);
        for (Vertex& vertex : normalizedVertices) {
            vertex.Position *= inverseRadius;
            glm::vec3 radial = glm::normalize(vertex.Position);
            vertex.Position = radial;
            vertex.Normal = radial;
        }
        mesh->updateVertices(normalizedVertices);
        bubbleRestVertices_[survivorId] = normalizedVertices;
    }

    bubbleModels_[survivorId] = std::move(fusionIt->second);
    fusionModels_.erase(fusionIt);
    bubbleModels_.erase(absorbedId);
    bubbleRestVertices_.erase(absorbedId);
    contactModels_.erase(key);
    return true;
}

void BubbleSurfaceSystem::RemoveBubble(uint64_t bubbleId)
{
    bubbleModels_.erase(bubbleId);
    bubbleRestVertices_.erase(bubbleId);

    auto removePairsContainingBubble = [bubbleId](auto& models) {
        for (auto it = models.begin(); it != models.end();) {
            if (it->first.first == bubbleId || it->first.second == bubbleId) {
                it = models.erase(it);
            } else {
                ++it;
            }
        }
    };
    removePairsContainingBubble(contactModels_);
    removePairsContainingBubble(fusionModels_);
}

void BubbleSurfaceSystem::RemoveContact(uint64_t bubbleA, uint64_t bubbleB)
{
    PairKey key = MakePairKey(bubbleA, bubbleB);
    contactModels_.erase(key);
    fusionModels_.erase(key);
}

void BubbleSurfaceSystem::Clear()
{
    fusionModels_.clear();
    contactModels_.clear();
    bubbleModels_.clear();
    bubbleRestVertices_.clear();
}
