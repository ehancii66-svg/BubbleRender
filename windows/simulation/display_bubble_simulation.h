#ifndef WINDOWS_SIMULATION_DISPLAY_BUBBLE_SIMULATION_H
#define WINDOWS_SIMULATION_DISPLAY_BUBBLE_SIMULATION_H

#include <cstdint>
#include <utility>
#include <vector>

#include "display_bubble.h"

float BubbleVolume(float radius);
float RadiusFromVolume(float volume);
float BubbleMass(const DisplayBubble& bubble);
float BubbleInvMass(const DisplayBubble& bubble);

int FindBubbleIndexById(const std::vector<DisplayBubble>& bubbles, uint64_t id);
bool ResolveContactPairIndices(const std::vector<DisplayBubble>& bubbles,
                               const BubbleContactPair& pair,
                               int& aIndex,
                               int& bIndex);
bool PairContainsBubbleIndex(const std::vector<DisplayBubble>& bubbles,
                             const BubbleContactPair& pair,
                             int bubbleIndex,
                             int* otherIndex = nullptr);

int FindContactPair(const std::vector<BubbleContactPair>& pairs, uint64_t a, uint64_t b);
int EnsureContactPairById(std::vector<BubbleContactPair>& pairs, uint64_t a, uint64_t b);
int EnsureContactPairByIndex(const std::vector<DisplayBubble>& bubbles,
                             std::vector<BubbleContactPair>& pairs,
                             int aIndex,
                             int bIndex);

std::vector<std::pair<int, int>> BuildBubbleBroadPhasePairs(const std::vector<DisplayBubble>& bubbles);
int BubbleActiveContactCount(const std::vector<DisplayBubble>& bubbles,
                             const std::vector<BubbleContactPair>& pairs,
                             int bubbleIndex);

std::vector<DisplayBubble::SurfaceControl> MakeSurfaceControls(float phase);

#endif
