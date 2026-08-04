// Mechanically mirrored from windows/main.cpp. Keep behavior changes in sync with Windows.
namespace windows_parity {
static float Smooth01(float x)
{
    x = glm::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

static bool IsFiniteDirection(const glm::vec3 &direction)
{
    return std::isfinite(direction.x) &&
           std::isfinite(direction.y) &&
           std::isfinite(direction.z);
}

static glm::vec3 SafeNormalizeDirection(const glm::vec3 &direction,
                                        const glm::vec3 &fallback)
{
    float lengthSquared = glm::dot(direction, direction);
    if (!IsFiniteDirection(direction) || lengthSquared <= 1e-10f)
    {
        return fallback;
    }
    return direction / std::sqrt(lengthSquared);
}

static void BuildStableOrthonormalFrame(const glm::vec3 &axis,
                                        glm::vec3 &side,
                                        glm::vec3 &binormal)
{
    if (axis.z < -0.999999f)
    {
        side = glm::vec3(0.0f, -1.0f, 0.0f);
        binormal = glm::vec3(-1.0f, 0.0f, 0.0f);
        return;
    }

    float a = 1.0f / (1.0f + axis.z);
    float b = -axis.x * axis.y * a;
    side = glm::vec3(1.0f - axis.x * axis.x * a, b, -axis.x);
    binormal = glm::vec3(b, 1.0f - axis.y * axis.y * a, -axis.y);
}

static void UpdateTransportedFusionFrame(BubbleContactPair &pair,
                                         const glm::vec3 &newAxis)
{
    glm::vec3 axis = SafeNormalizeDirection(newAxis, pair.fusionAxis);
    if (!pair.fusionFrameInitialized)
    {
        BuildStableOrthonormalFrame(axis, pair.fusionSide, pair.fusionBinormal);
        pair.fusionAxis = axis;
        pair.fusionFrameInitialized = true;
        return;
    }

    glm::vec3 side = pair.fusionSide - axis * glm::dot(pair.fusionSide, axis);
    if (!IsFiniteDirection(side) || glm::dot(side, side) <= 1e-10f)
    {
        side = pair.fusionBinormal - axis * glm::dot(pair.fusionBinormal, axis);
    }
    if (!IsFiniteDirection(side) || glm::dot(side, side) <= 1e-10f)
    {
        BuildStableOrthonormalFrame(axis, side, pair.fusionBinormal);
    }
    else
    {
        side = SafeNormalizeDirection(side, pair.fusionSide);
        pair.fusionBinormal = SafeNormalizeDirection(
            glm::cross(axis, side),
            pair.fusionBinormal);
        side = SafeNormalizeDirection(
            glm::cross(pair.fusionBinormal, axis),
            side);
    }

    pair.fusionAxis = axis;
    pair.fusionSide = side;
}

static float NextContactOutcomeRandom01()
{
    uint64_t x = (++g_ContactOutcomeSequence) * 0x9E3779B97F4A7C15ull ^
                 g_InteractionOutcomeSeed * 0xD6E8FEB86659FD93ull;
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return (float)((x >> 40) & 0xFFFFFFull) / (float)0xFFFFFFull;
}

static const char *CoalescenceOutcomeName(BubbleContactPair::CoalescenceOutcome outcome)
{
    switch (outcome)
    {
    case BubbleContactPair::CoalescenceOutcome::SeparateAfterContact:
        return "separate";
    case BubbleContactPair::CoalescenceOutcome::StayDoubleBubble:
        return "stay-double";
    case BubbleContactPair::CoalescenceOutcome::WillCoalesce:
        return "will-coalesce";
    case BubbleContactPair::CoalescenceOutcome::Undecided:
    default:
        return "undecided";
    }
}

static float FusionSurfaceVisibility(const BubbleContactPair &pair)
{
    if (!pair.fusionActive)
    {
        return 0.0f;
    }

    return Smooth01(pair.fusionElapsed /
                    std::max(kContactVisualTransitionTime, 0.001f));
}

static float BubbleFusionSurfaceVisibility(uint64_t bubbleId)
{
    float visibility = 0.0f;
    for (const BubbleContactPair &pair : g_ContactPairs)
    {
        if (pair.a == bubbleId || pair.b == bubbleId)
        {
            visibility = std::max(visibility, FusionSurfaceVisibility(pair));
        }
    }
    return visibility;
}

static bool PairCanBeginExclusiveFusion(const BubbleContactPair &pair)
{
    for (const BubbleContactPair &other : g_ContactPairs)
    {
        if (&other == &pair ||
            (!other.active && !other.candidate && !other.bonded))
        {
            continue;
        }
        if (other.a == pair.a || other.a == pair.b ||
            other.b == pair.a || other.b == pair.b)
        {
            return false;
        }
    }
    return true;
}
static void UpdatePersistentSurfaceDynamics(float dt)
{
    if (dt <= 0.0f)
    {
        return;
    }

    std::vector<std::vector<glm::vec3>> targets(g_DisplayBubbles.size());
    std::vector<std::vector<float>> contactWeights(g_DisplayBubbles.size());
    for (size_t bubbleIndex = 0; bubbleIndex < g_DisplayBubbles.size(); ++bubbleIndex)
    {
        DisplayBubble &bubble = g_DisplayBubbles[bubbleIndex];
        targets[bubbleIndex].resize(bubble.surfaceControls.size(), glm::vec3(0.0f));
        contactWeights[bubbleIndex].resize(bubble.surfaceControls.size(), 0.0f);
        if (bubble.volumeTransferred)
        {
            continue;
        }
        for (size_t controlIndex = 0; controlIndex < bubble.surfaceControls.size(); ++controlIndex)
        {
            const auto &control = bubble.surfaceControls[controlIndex];
            float freeWave = std::sin((float)g_Time * (0.72f + bubble.speed * 0.12f) +
                                      bubble.phase + (float)controlIndex * 1.17f);
            glm::vec3 target = control.localDir * (freeWave * 0.0035f);

            glm::vec3 windShapeAxis = bubble.velocity;
            if (g_WindEnabled)
            {
                windShapeAxis += g_GlobalWindDirection * (g_GlobalWindStrength * 1.85f);
            }
            float windShapeSpeed = glm::length(windShapeAxis);
            if (windShapeSpeed > 1e-5f)
            {
                windShapeAxis /= windShapeSpeed;
                float radiusResponse = glm::clamp(
                    std::sqrt(0.40f / std::max(bubble.radius, 0.08f)),
                    0.62f, 1.35f);
                float windDrive = g_WindEnabled ? g_GlobalWindStrength : 0.0f;
                float shapeAmplitude = glm::clamp(
                    (windDrive * 0.075f + glm::length(bubble.velocity) * 0.030f) * radiusResponse,
                    0.0f, 0.052f);
                float alignment = glm::dot(control.localDir, windShapeAxis);
                float axialRadial = (alignment * alignment - 0.333f) * shapeAmplitude;
                float windwardMask = Smooth01((-alignment - 0.10f) / 0.90f);
                float leewardMask = Smooth01((alignment - 0.05f) / 0.95f);
                float sideRipple = std::sin((float)g_Time * (1.30f + bubble.speed * 0.18f) +
                                            bubble.phase * 0.73f + (float)controlIndex * 0.91f);
                glm::vec3 sideDir = control.localDir - windShapeAxis * alignment;
                if (glm::length(sideDir) > 1e-5f)
                {
                    sideDir = glm::normalize(sideDir);
                }
                else
                {
                    sideDir = glm::vec3(0.0f);
                }

                target += control.localDir * axialRadial;
                target -= windShapeAxis * (0.026f * shapeAmplitude / 0.052f) * windwardMask;
                target += windShapeAxis * (0.016f * shapeAmplitude / 0.052f) * leewardMask;
                target += sideDir * (sideRipple * shapeAmplitude * 0.18f) *
                          (1.0f - std::abs(alignment));
            }

            targets[bubbleIndex][controlIndex] = target;
        }
    }

    for (const BubbleContactPair &pair : g_ContactPairs)
    {
        int aIndex = -1;
        int bIndex = -1;
        if ((!pair.candidate && !pair.bonded) ||
            !ResolveContactPairIndices(g_DisplayBubbles, pair, aIndex, bIndex))
        {
            continue;
        }

        const DisplayBubble &a = g_DisplayBubbles[(size_t)aIndex];
        const DisplayBubble &b = g_DisplayBubbles[(size_t)bIndex];
        glm::vec3 axis = b.position - a.position;
        float distance = glm::length(axis);
        if (distance <= 1e-5f)
        {
            continue;
        }
        axis /= distance;
        float pairStrength = glm::clamp(std::max(pair.contactActivation,
                                                 pair.bonded ? 0.25f + 0.75f * pair.interactionCompression : 0.0f),
                                        0.0f, 1.0f);
        float radiusRatioA = glm::clamp(pair.contactRadius / std::max(a.radius, 0.001f), 0.0f, 0.90f);
        float radiusRatioB = glm::clamp(pair.contactRadius / std::max(b.radius, 0.001f), 0.0f, 0.90f);

        auto accumulateContact = [&](int bubbleIndex, int otherBubbleIndex,
                                     const glm::vec3 &contactAxis,
                                     float radiusRatio)
        {
            const DisplayBubble &bubble = g_DisplayBubbles[(size_t)bubbleIndex];
            const DisplayBubble &other = g_DisplayBubbles[(size_t)otherBubbleIndex];
            float sizeDeformScale = glm::clamp(
                std::sqrt(std::max(other.radius, 0.001f) / std::max(bubble.radius, 0.001f)),
                0.62f, 1.65f);
            float coneCos = glm::mix(0.90f,
                                     std::sqrt(std::max(1.0f - radiusRatio * radiusRatio, 0.0f)),
                                     pairStrength);
            coneCos = glm::clamp(coneCos, 0.28f, 0.94f);
            for (size_t controlIndex = 0; controlIndex < bubble.surfaceControls.size(); ++controlIndex)
            {
                const glm::vec3 direction = bubble.surfaceControls[controlIndex].localDir;
                float alignment = glm::dot(direction, contactAxis);
                float capMask = Smooth01((alignment - coneCos) /
                                         std::max(1.0f - coneCos, 0.001f));
                float rimMask = std::exp(-std::pow((alignment - (coneCos - 0.08f)) / 0.10f, 2.0f));
                float weight = pairStrength * std::max(capMask, 0.32f * rimMask);
                if (weight <= 0.0001f)
                {
                    continue;
                }

                glm::vec3 radial = direction - contactAxis * alignment;
                if (glm::length(radial) > 1e-5f)
                {
                    radial = glm::normalize(radial);
                }
                glm::vec3 contactTarget =
                    -contactAxis * (0.018f + 0.070f * pairStrength) * capMask * sizeDeformScale +
                    radial * (0.004f + 0.014f * pairStrength) * rimMask *
                        glm::mix(1.0f, sizeDeformScale, 0.45f);
                float oldWeight = contactWeights[(size_t)bubbleIndex][controlIndex];
                float newWeight = oldWeight + weight;
                targets[(size_t)bubbleIndex][controlIndex] =
                    (targets[(size_t)bubbleIndex][controlIndex] * std::max(1.0f - oldWeight, 0.0f) +
                     contactTarget * weight) /
                    std::max(std::max(1.0f - oldWeight, 0.0f) + weight, 0.001f);
                contactWeights[(size_t)bubbleIndex][controlIndex] = glm::clamp(newWeight, 0.0f, 1.0f);
            }
        };

        accumulateContact(aIndex, bIndex, axis, radiusRatioA);
        accumulateContact(bIndex, aIndex, -axis, radiusRatioB);
    }

    // Match the material velocity of the two surface caps. This is a low-cost
    // analogue of multi-MELP's matching-velocity/contact coupling condition.
    for (const BubbleContactPair &pair : g_ContactPairs)
    {
        int aIndex = -1;
        int bIndex = -1;
        if ((!pair.candidate && !pair.bonded) ||
            !ResolveContactPairIndices(g_DisplayBubbles, pair, aIndex, bIndex))
        {
            continue;
        }
        DisplayBubble &a = g_DisplayBubbles[(size_t)aIndex];
        DisplayBubble &b = g_DisplayBubbles[(size_t)bIndex];
        float coupling = glm::clamp(pair.contactActivation, 0.0f, 1.0f);
        if (coupling <= 0.001f)
        {
            continue;
        }

        glm::vec3 pairAxis = b.position - a.position;
        float pairDistance = glm::length(pairAxis);
        if (pairDistance <= 1e-5f)
        {
            continue;
        }
        pairAxis /= pairDistance;
        auto pairControlWeight = [&](const DisplayBubble &bubble,
                                     const DisplayBubble::SurfaceControl &control,
                                     const glm::vec3 &contactAxis)
        {
            float radiusRatio = glm::clamp(pair.contactRadius /
                                               std::max(bubble.radius, 0.001f),
                                           0.0f, 0.90f);
            float coneCos = glm::mix(0.90f,
                                     std::sqrt(std::max(1.0f - radiusRatio * radiusRatio, 0.0f)),
                                     coupling);
            coneCos = glm::clamp(coneCos, 0.28f, 0.94f);
            float alignment = glm::dot(control.localDir, contactAxis);
            return Smooth01((alignment - coneCos) /
                            std::max(1.0f - coneCos, 0.001f));
        };

        glm::vec3 averageA(0.0f), averageB(0.0f);
        float weightA = 0.0f, weightB = 0.0f;
        for (size_t i = 0; i < a.surfaceControls.size(); ++i)
        {
            float w = pairControlWeight(a, a.surfaceControls[i], pairAxis);
            averageA += (a.velocity + a.surfaceControls[i].velocity * a.radius) * w;
            weightA += w;
        }
        for (size_t i = 0; i < b.surfaceControls.size(); ++i)
        {
            float w = pairControlWeight(b, b.surfaceControls[i], -pairAxis);
            averageB += (b.velocity + b.surfaceControls[i].velocity * b.radius) * w;
            weightB += w;
        }
        averageA = weightA > 1e-5f ? averageA / weightA : a.velocity;
        averageB = weightB > 1e-5f ? averageB / weightB : b.velocity;
        float invMassA = BubbleInvMass(a);
        float invMassB = BubbleInvMass(b);
        glm::vec3 sharedVelocity = (averageA * invMassB + averageB * invMassA) /
                                   std::max(invMassA + invMassB, 0.001f);
        float velocityBlend = 1.0f - std::exp(-dt * glm::mix(6.0f, 32.0f, coupling));

        for (size_t i = 0; i < a.surfaceControls.size(); ++i)
        {
            float w = pairControlWeight(a, a.surfaceControls[i], pairAxis) * coupling;
            glm::vec3 desired = (sharedVelocity - a.velocity) / std::max(a.radius, 0.001f);
            a.surfaceControls[i].velocity = glm::mix(a.surfaceControls[i].velocity,
                                                     desired, velocityBlend * w);
        }
        for (size_t i = 0; i < b.surfaceControls.size(); ++i)
        {
            float w = pairControlWeight(b, b.surfaceControls[i], -pairAxis) * coupling;
            glm::vec3 desired = (sharedVelocity - b.velocity) / std::max(b.radius, 0.001f);
            b.surfaceControls[i].velocity = glm::mix(b.surfaceControls[i].velocity,
                                                     desired, velocityBlend * w);
        }
    }

    for (size_t bubbleIndex = 0; bubbleIndex < g_DisplayBubbles.size(); ++bubbleIndex)
    {
        DisplayBubble &bubble = g_DisplayBubbles[bubbleIndex];
        if (bubble.state == DisplayBubble::State::Dead || bubble.volumeTransferred)
        {
            continue;
        }
        for (size_t controlIndex = 0; controlIndex < bubble.surfaceControls.size(); ++controlIndex)
        {
            auto &control = bubble.surfaceControls[controlIndex];
            float contactWeight = contactWeights[bubbleIndex][controlIndex];
            float stiffness = glm::mix(14.0f, 48.0f, contactWeight);
            float damping = glm::mix(5.0f, 7.2f, contactWeight);
            glm::vec3 acceleration =
                (targets[bubbleIndex][controlIndex] - control.displacement) * stiffness -
                control.velocity * damping;
            control.velocity += acceleration * dt;
            control.displacement += control.velocity * dt;
            float displacementLength = glm::length(control.displacement);
            if (displacementLength > 0.115f)
            {
                control.displacement *= 0.115f / displacementLength;
                control.velocity *= 0.65f;
            }
        }
    }
}

static float ContactFilmWorldCurvature(const DisplayBubble &a,
                                       const DisplayBubble &b,
                                       const BubbleContactPair &pair)
{
    float radiusA = std::max(a.radius, 0.001f);
    float radiusB = std::max(b.radius, 0.001f);
    // Young-Laplace pressure gives k_partition = 1/rA - 1/rB for a
    // normal directed from A to B. The sign bends the film toward the
    // larger, lower-pressure bubble. Equal radii naturally give a plane.
    float mismatch = std::abs(radiusA - radiusB) / std::max(std::max(radiusA, radiusB), 0.001f);
    float curvature = (1.0f / radiusA - 1.0f / radiusB) * (1.0f + mismatch * 0.85f);
    float filmRadius = std::max(pair.contactRadius, 0.002f);
    float maximumCurvature = 0.85f / filmRadius;
    return glm::clamp(curvature, -maximumCurvature, maximumCurvature);
}

static void DecideCoalescenceOutcome(BubbleContactPair &pair,
                                     const DisplayBubble &a,
                                     const DisplayBubble &b,
                                     float relNormalSpeed)
{
    if (pair.outcome != BubbleContactPair::CoalescenceOutcome::Undecided ||
        pair.contactTime < 0.85f ||
        pair.contactRadius <= 0.001f)
    {
        return;
    }

    float minRadius = std::max(std::min(a.radius, b.radius), 0.001f);
    float maxRadius = std::max(std::max(a.radius, b.radius), minRadius);
    float sizeRatio = glm::clamp(minRadius / maxRadius, 0.0f, 1.0f);
    float sizeMismatch = 1.0f - sizeRatio;
    float contactAge = Smooth01((pair.contactTime - 0.65f) / 1.35f);
    float thinFilm = Smooth01((0.42f - pair.filmThickness) / 0.26f);
    float impact = Smooth01(std::max(-relNormalSpeed, 0.0f) / 0.52f);
    float contactSize = Smooth01((pair.contactRadius / minRadius - 0.18f) / 0.46f);
    float windInstability = g_WindEnabled
                                ? Smooth01(g_GlobalWindStrength /
                                           std::max(kMaxGlobalWindStrength, 0.001f))
                                : 0.0f;

    float separationProbability = glm::clamp(
        0.005f +
            (1.0f - contactSize) * 0.010f +
            impact * 0.005f +
            windInstability * 0.0025f,
        0.005f, 0.025f);
    float sizeMismatchGate = Smooth01((sizeMismatch - 0.06f) / 0.30f);
    float fusionProbability = glm::clamp(
        0.005f + sizeMismatchGate *
            (0.76f +
             thinFilm * 0.07f +
             impact * 0.04f +
             contactAge * 0.03f),
        0.005f, 0.82f);
    float probabilitySum = separationProbability + fusionProbability;
    if (probabilitySum > 0.92f)
    {
        float scale = 0.92f / probabilitySum;
        separationProbability *= scale;
        fusionProbability *= scale;
    }
    pair.coalescenceScore = fusionProbability;
    if (!pair.outcomeRandomSampled)
    {
        pair.coalescenceRandom = NextContactOutcomeRandom01();
        pair.outcomeRandomSampled = true;
    }
    if (pair.coalescenceRandom < separationProbability)
    {
        pair.outcome = BubbleContactPair::CoalescenceOutcome::SeparateAfterContact;
        pair.persistentRenderPair = true;
    }
    else if (pair.coalescenceRandom < separationProbability + fusionProbability)
    {
        pair.outcome = BubbleContactPair::CoalescenceOutcome::WillCoalesce;
    }
    else
    {
        pair.outcome = BubbleContactPair::CoalescenceOutcome::StayDoubleBubble;
    }

    if (g_LogContactDebug)
    {
        std::cout << "[BubbleCoalescence] pair=(" << pair.a << ", " << pair.b << ")"
                  << " outcome=" << CoalescenceOutcomeName(pair.outcome)
                  << " score=" << pair.coalescenceScore
                  << " random=" << pair.coalescenceRandom
                  << " sceneSeed=" << g_InteractionOutcomeSeed
                  << " separate=" << separationProbability
                  << " sizeRatio=" << sizeRatio
                  << " mismatch=" << sizeMismatch
                  << " film=" << pair.filmThickness
                  << std::endl;
    }
}

static float PlateauContactRadius(const BubbleContactPair &pair, const DisplayBubble &a, const DisplayBubble &b)
{
    float stableRadius = std::min(a.radius, b.radius) * 0.44f;
    float formingRadius = stableRadius * 0.08f;
    float relaxedRadius = glm::mix(formingRadius, stableRadius, Smooth01(pair.geometryBlend));
    float breathing = 1.0f + std::sin((float)g_Time * 1.15f + (float)(pair.a + pair.b) * 0.71f) * 0.025f * pair.geometryBlend;
    return relaxedRadius * breathing;
}

static bool RemoveBubble(uint64_t id)
{
    int bubbleIndex = FindBubbleIndexById(g_DisplayBubbles, id);
    if (bubbleIndex < 0)
    {
        return false;
    }

    g_DisplayBubbles.erase(g_DisplayBubbles.begin() + bubbleIndex);
    g_ContactPairs.erase(
        std::remove_if(g_ContactPairs.begin(), g_ContactPairs.end(),
                       [id](const BubbleContactPair &pair)
                       {
                           return pair.a == id || pair.b == id;
                       }),
        g_ContactPairs.end());

    g_BubbleSurfaces.RemoveBubble(id);
    RetireBubbleSim(id);
    return true;
}

static void FinalizeCompletedFusions()
{
    std::vector<std::pair<uint64_t, uint64_t>> completedPairs;
    for (const BubbleContactPair &pair : g_ContactPairs)
    {
        if (pair.fusionComplete)
        {
            completedPairs.emplace_back(pair.a, pair.b);
        }
    }

    for (const auto &ids : completedPairs)
    {
        int survivorIndex = FindBubbleIndexById(g_DisplayBubbles, ids.first);
        int absorbedIndex = FindBubbleIndexById(g_DisplayBubbles, ids.second);
        if (survivorIndex < 0 || absorbedIndex < 0)
        {
            continue;
        }

        DisplayBubble &survivor = g_DisplayBubbles[(size_t)survivorIndex];
        const DisplayBubble &absorbed = g_DisplayBubbles[(size_t)absorbedIndex];
        float volumeA = survivor.targetVolume > 0.0f
                            ? survivor.targetVolume
                            : BubbleVolume(survivor.radius);
        float volumeB = absorbed.targetVolume > 0.0f
                            ? absorbed.targetVolume
                            : BubbleVolume(absorbed.radius);
        float mergedVolume = std::max(volumeA + volumeB, 0.001f);
        glm::vec3 mergedCenter = (survivor.position * volumeA + absorbed.position * volumeB) /
                                 mergedVolume;
        glm::vec3 mergedVelocity =
            (survivor.velocity * volumeA + absorbed.velocity * volumeB) /
            mergedVolume;
        // Contact constraints are converted back into center velocity at the
        // end of each frame. Near fusion completion that numerical correction
        // can be much larger than the bubbles' actual drift, so do not let it
        // launch the merged bubble across the scene.
        constexpr float kMaxPostFusionSpeed = 0.12f;
        float mergedSpeed = glm::length(mergedVelocity);
        if (mergedSpeed > kMaxPostFusionSpeed)
        {
            mergedVelocity *= kMaxPostFusionSpeed / mergedSpeed;
        }
        float mergedRadius = RadiusFromVolume(mergedVolume);

        if (!g_BubbleSurfaces.PromoteFusion(ids.first, ids.second,
                                            survivor.id, absorbed.id))
        {
            continue;
        }

        survivor.position = mergedCenter;
        survivor.basePosition = mergedCenter;
        survivor.velocity = mergedVelocity;
        survivor.radius = mergedRadius;
        survivor.initialRadius = mergedRadius;
        survivor.targetVolume = mergedVolume;
        survivor.alpha = std::max(survivor.alpha, absorbed.alpha);
        survivor.contactTime = 0.0f;
        survivor.mergeProgress = 1.0f;
        survivor.filmThickness = std::min(survivor.filmThickness,
                                          absorbed.filmThickness);
        survivor.contactStrength = 0.0f;
        survivor.surfaceDynamicsBlend = 0.0f;
        survivor.volumeTransferred = true;
        survivor.state = DisplayBubble::State::Merged;

        std::cout << "[BubbleFusion] pair=(" << ids.first << ", " << ids.second
                  << ") survivor=" << survivor.id
                  << " radius=" << mergedRadius << std::endl;
        RemoveBubble(ids.second);
    }
}
static glm::vec3 BubbleVisualCenter(const DisplayBubble &bubble, float time)
{
    glm::vec3 windDir = glm::normalize(glm::vec3(0.75f, 0.22f, 0.0f));
    float motionScale = g_InteractionDemoActive ? 0.18f : 1.0f;
    float wind = sinf(time * bubble.speed + bubble.phase) * bubble.windAmplitude * motionScale;
    float lift = sinf(time * (bubble.speed * 0.73f) + bubble.phase * 1.7f) * bubble.floatAmplitude * motionScale;
    float depth = cosf(time * (bubble.speed * 0.51f) + bubble.phase * 0.9f) * bubble.windAmplitude * 0.22f * motionScale;
    return bubble.position + windDir * wind + glm::vec3(0.0f, lift, depth);
}

static glm::mat4 MakeContactFilmModel(const DisplayBubble &a, const DisplayBubble &b,
                                      const BubbleContactPair &pair, float time)
{
    glm::vec3 pa = BubbleVisualCenter(a, time);
    glm::vec3 pb = BubbleVisualCenter(b, time);
    glm::vec3 axis = pb - pa;
    float length = glm::length(axis);
    if (length < 1e-4f)
    {
        return glm::mat4(1.0f);
    }

    axis /= length;
    if (pair.contactFrameInitialized)
    {
        axis = pair.filteredNormal;
    }
    glm::vec3 up = std::abs(axis.y) < 0.92f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 side = glm::normalize(glm::cross(up, axis));
    glm::vec3 binormal = glm::cross(axis, side);

    float pulse = 1.0f + sinf(time * 0.70f + (float)(pair.a * 1.7f + pair.b)) * 0.008f;
    float filmRadius = std::max(0.002f, pair.contactRadius * pulse);
    glm::vec3 filmCenter = pair.contactFrameInitialized
                               ? pair.filteredPlaneCenter
                               : (pa + pb) * 0.5f;

    glm::mat4 basis(1.0f);
    basis[0] = glm::vec4(side, 0.0f);
    basis[1] = glm::vec4(binormal, 0.0f);
    basis[2] = glm::vec4(axis, 0.0f);
    basis[3] = glm::vec4(filmCenter, 1.0f);
    return basis * glm::scale(glm::mat4(1.0f), glm::vec3(filmRadius));
}

static glm::mat4 BubbleModelMatrix(const DisplayBubble &bubble, float time)
{
    glm::vec3 windDir = glm::normalize(glm::vec3(0.75f, 0.22f, 0.0f));
    float motionScale = g_InteractionDemoActive ? 0.18f : 1.0f;
    float shapeWobble = g_InteractionDemoActive ? 0.006f : 0.018f;
    float wind = sinf(time * bubble.speed + bubble.phase) * bubble.windAmplitude * motionScale;
    float lift = sinf(time * (bubble.speed * 0.73f) + bubble.phase * 1.7f) * bubble.floatAmplitude * motionScale;
    float depth = cosf(time * (bubble.speed * 0.51f) + bubble.phase * 0.9f) * bubble.windAmplitude * 0.22f * motionScale;
    float wobble = 1.0f + sinf(time * (bubble.speed * 0.20f) + bubble.phase * 2.1f) * shapeWobble;
    float bridgePulse = (bubble.state == DisplayBubble::State::Touch ||
                         bubble.state == DisplayBubble::State::SharedFilm ||
                         bubble.state == DisplayBubble::State::NeckForming)
                            ? sinf(time * 1.10f + bubble.phase) * (g_InteractionDemoActive ? 0.006f : 0.018f)
                            : 0.0f;

    glm::vec3 pos = bubble.position + windDir * wind + glm::vec3(0.0f, lift, depth);
    float scale = std::max(0.001f, bubble.radius) * (wobble + bridgePulse);
    for (const auto &control : bubble.surfaceControls)
    {
        float worldMask = std::max(glm::dot(control.localDir, bubble.contactAxis), 0.0f);
        float radialOffset = glm::dot(control.displacement, control.localDir);
        scale += bubble.radius * radialOffset * worldMask * 0.08f;
    }
    glm::mat4 model = glm::translate(glm::mat4(1.0f), pos);
    if (bubble.contactStrength > 0.01f)
    {
        glm::vec3 axis = glm::normalize(bubble.contactAxis);
        glm::vec3 up = std::abs(axis.y) < 0.92f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
        glm::vec3 side = glm::normalize(glm::cross(up, axis));
        glm::vec3 binormal = glm::cross(axis, side);
        float globalContact = glm::clamp(bubble.contactStrength, 0.0f, 1.0f);
        float squash = 1.0f - 0.025f * globalContact;
        float bulge = 1.0f + 0.010f * globalContact;
        glm::mat4 basis(1.0f);
        basis[0] = glm::vec4(axis, 0.0f);
        basis[1] = glm::vec4(side, 0.0f);
        basis[2] = glm::vec4(binormal, 0.0f);
        model *= basis * glm::scale(glm::mat4(1.0f), glm::vec3(scale * squash, scale * bulge, scale * bulge));
    }
    else
    {
        model = glm::scale(model, glm::vec3(scale));
    }
    return model;
}

static glm::mat4 PersistentBubbleModelMatrix(const DisplayBubble &bubble,
                                            float time,
                                            bool worldScaleSurface)
{
    (void)time;
    glm::mat4 model = glm::translate(glm::mat4(1.0f), bubble.position);
    if (bubble.state == DisplayBubble::State::Burst)
    {
        // If we have a sim, its mesh is at world radius 鈥?pure translation.
        // Otherwise fall back to manual burstScale scaling.
        if (GetBubbleSim(bubble.id))
            return model;
        model = glm::scale(model, glm::vec3(std::max(bubble.burstScale, 0.001f)));
        return model;
    }
    return worldScaleSurface
               ? model
               : glm::scale(model, glm::vec3(std::max(bubble.radius, 0.001f)));
}

static void PreserveFilmCoordinates(std::vector<Vertex> &vertices,
                                    const std::vector<Vertex> &previousVertices)
{
    if (vertices.size() != previousVertices.size())
    {
        return;
    }
    for (size_t i = 0; i < vertices.size(); ++i)
    {
        if (glm::dot(previousVertices[i].FilmDirection,
                     previousVertices[i].FilmDirection) > 1e-8f)
        {
            vertices[i].FilmDirection = previousVertices[i].FilmDirection;
        }
    }
}

static std::vector<Vertex> BuildPersistentBubbleVertices(
    int bubbleIndex,
    float time,
    const std::vector<Vertex> *restVertices,
    bool worldScaleSurface,
    const std::vector<Vertex> *previousVertices = nullptr)
{
    std::vector<Vertex> vertices = restVertices
                                       ? *restVertices
                                       : BuildBubbleShellVertices(1.0f, 0.0f, false);
    if (bubbleIndex < 0 || bubbleIndex >= (int)g_DisplayBubbles.size())
    {
        return vertices;
    }

    const DisplayBubble &bubble = g_DisplayBubbles[(size_t)bubbleIndex];
    float deformationUnitScale = worldScaleSurface ? bubble.radius : 1.0f;
    float surfaceDynamicsBlend = Smooth01(bubble.surfaceDynamicsBlend);
    glm::vec3 motionAxis = bubble.velocity;
    if (glm::length(motionAxis) < 1e-4f)
    {
        motionAxis = glm::vec3(0.78f, 0.26f, 0.12f);
    }
    motionAxis = glm::normalize(motionAxis);
    float speedStretch = glm::clamp(glm::length(bubble.velocity) * 0.055f, 0.0f, 0.045f) *
                         surfaceDynamicsBlend;
    float freeOscillation = std::sin(time * (0.72f + bubble.speed * 0.18f) + bubble.phase) *
                            0.012f * surfaceDynamicsBlend;
    float axialScale = 1.0f + speedStretch + freeOscillation;
    float radialScale = 1.0f / std::sqrt(std::max(axialScale, 0.25f));
    float contactShare = 1.0f /
                         std::sqrt((float)BubbleActiveContactCount(g_DisplayBubbles, g_ContactPairs, bubbleIndex));
    float meanControlRadialOffset = 0.0f;
    for (const auto &control : bubble.surfaceControls)
    {
        meanControlRadialOffset += glm::dot(control.displacement, control.localDir);
    }
    if (!bubble.surfaceControls.empty())
    {
        meanControlRadialOffset /= (float)bubble.surfaceControls.size();
    }
    float persistentVolumeScale = glm::mix(
        1.0f,
        glm::clamp(1.0f - meanControlRadialOffset, 0.94f, 1.06f),
        surfaceDynamicsBlend);

    for (Vertex &vertex : vertices)
    {
        glm::vec3 restPosition = vertex.Position;
        glm::vec3 restNormal = SafeNormalizeDirection(vertex.Normal, restPosition);
        float restRadius = std::max(glm::length(restPosition), 0.001f);
        glm::vec3 direction = restPosition / restRadius;
        vertex.FilmDirection = direction;
        float alongMotion = glm::dot(direction, motionAxis);
        glm::vec3 axial = motionAxis * alongMotion * axialScale;
        glm::vec3 radial = (direction - motionAxis * alongMotion) * radialScale;
        glm::vec3 position = (axial + radial) * restRadius;
        glm::vec3 persistentSurfaceOffset(0.0f);
        float persistentWeight = 0.0f;
        for (const auto &control : bubble.surfaceControls)
        {
            float alignment = std::max(glm::dot(direction, control.localDir), 0.0f);
            float weight = std::pow(alignment, 8.0f);
            persistentSurfaceOffset += control.displacement * weight;
            persistentWeight += weight;
        }
        if (persistentWeight > 1e-5f)
        {
            position += persistentSurfaceOffset / persistentWeight *
                        surfaceDynamicsBlend * deformationUnitScale;
        }
        glm::vec3 contactOffset(0.0f);
        float vertexContactSum = 0.0f;

        for (const BubbleContactPair &pair : g_ContactPairs)
        {
            int otherIndex = -1;
            if ((!pair.candidate && !pair.bonded) ||
                !PairContainsBubbleIndex(g_DisplayBubbles, pair, bubbleIndex, &otherIndex))
            {
                continue;
            }
            const DisplayBubble &otherBubble = g_DisplayBubbles[(size_t)otherIndex];
            glm::vec3 contactDirection =
                otherBubble.position - bubble.position;
            float contactDistance = glm::length(contactDirection);
            if (contactDistance < 1e-5f)
            {
                continue;
            }
            contactDirection /= contactDistance;

            float bondedStrength = glm::clamp(
                0.20f + 0.80f * std::max(pair.interactionCompression,
                                         Smooth01(pair.contactTime / kFusionTime)),
                0.0f, 1.0f);
            float pairStrength = pair.bonded
                                     ? std::max(pair.contactActivation, bondedStrength)
                                     : pair.contactActivation;
            if (pairStrength <= 0.001f)
            {
                continue;
            }
            float sizeDeformScale = glm::clamp(
                std::sqrt(std::max(otherBubble.radius, 0.001f) / std::max(bubble.radius, 0.001f)),
                0.62f, 1.65f);
            float deformationStrength = pairStrength * contactShare * sizeDeformScale;

            float contactRatio = glm::clamp(pair.contactRadius /
                                                std::max(bubble.radius, 0.001f),
                                            0.0f, 0.92f);
            float geometricConeCos = std::sqrt(std::max(1.0f - contactRatio * contactRatio,
                                                        0.0f));
            float coneCos = pair.bonded
                                ? geometricConeCos
                                : glm::mix(0.94f, 0.70f, deformationStrength);
            float alignment = glm::dot(direction, contactDirection);
            float contactMask = Smooth01((alignment - coneCos) /
                                         std::max(1.0f - coneCos, 0.001f));
            float rimCenter = coneCos - 0.045f;
            float rimMask = std::exp(-std::pow((alignment - rimCenter) / 0.045f, 2.0f));
            glm::vec3 radialDirection = direction - contactDirection * alignment;
            if (glm::length(radialDirection) > 1e-5f)
            {
                radialDirection = glm::normalize(radialDirection);
            }

            // Keep only a small analytic correction for an exact seal against
            // the contact plane. Most deformation now comes from persistent
            // surface-control dynamics above.
            contactOffset -= contactDirection *
                             (0.006f + 0.040f * deformationStrength) *
                             contactMask * deformationUnitScale;
            contactOffset += radialDirection *
                             (0.003f + 0.010f * deformationStrength) *
                             rimMask * deformationUnitScale;
            vertexContactSum += deformationStrength * contactMask;
        }

        float maxOffset = 0.18f * deformationUnitScale;
        float offsetLength = glm::length(contactOffset);
        if (offsetLength > maxOffset)
        {
            contactOffset *= maxOffset / offsetLength;
        }
        float volumeCompensation = 1.0f + std::min(vertexContactSum, 2.0f) * 0.012f;
        vertex.Position = position * (volumeCompensation * persistentVolumeScale) + contactOffset;
        glm::vec3 deformedNormal = SafeNormalizeDirection(vertex.Position, restNormal);
        vertex.Normal = SafeNormalizeDirection(
            glm::mix(restNormal, deformedNormal, surfaceDynamicsBlend),
            restNormal);
    }

    std::vector<unsigned int> indices = BuildContactBubblePatchIndices();
    std::vector<glm::vec3> normalSums(vertices.size(), glm::vec3(0.0f));
    for (size_t i = 0; i + 2 < indices.size(); i += 3)
    {
        unsigned int ia = indices[i];
        unsigned int ib = indices[i + 1];
        unsigned int ic = indices[i + 2];
        glm::vec3 faceNormal = glm::cross(vertices[ib].Position - vertices[ia].Position,
                                          vertices[ic].Position - vertices[ia].Position);
        if (glm::length(faceNormal) <= 1e-8f)
        {
            continue;
        }
        normalSums[ia] += faceNormal;
        normalSums[ib] += faceNormal;
        normalSums[ic] += faceNormal;
    }
    float rebuiltNormalBlend = glm::clamp(
        std::max(surfaceDynamicsBlend, bubble.contactStrength),
        0.0f, 1.0f);
    for (size_t i = 0; i < vertices.size(); ++i)
    {
        if (glm::length(normalSums[i]) > 1e-6f)
        {
            vertices[i].Normal = SafeNormalizeDirection(
                glm::mix(vertices[i].Normal,
                         glm::normalize(normalSums[i]),
                         rebuiltNormalBlend),
                vertices[i].Normal);
        }
    }
    if (previousVertices)
    {
        PreserveFilmCoordinates(vertices, *previousVertices);
    }
    return vertices;
}

static void UpdateBubblePair(BubbleContactPair &pair, float dt)
{
    int aIndex = -1;
    int bIndex = -1;
    if (!pair.active || !ResolveContactPairIndices(g_DisplayBubbles, pair, aIndex, bIndex))
    {
        pair.contactTime = std::max(0.0f, pair.contactTime - dt * 1.8f);
        pair.bridgeStrength = std::max(0.0f, pair.bridgeStrength - dt * 2.4f);
        pair.geometryBlend = std::max(0.0f, pair.geometryBlend - dt * 2.0f);
        if (!pair.candidate)
        {
            pair.contactRadius = std::max(0.0f, pair.contactRadius - dt * 0.5f);
        }
        pair.contactRadiusVelocity *= std::exp(-dt * 8.0f);
        pair.interactionVelocity *= std::exp(-dt * 6.0f);
        pair.interactionCompression = std::max(0.0f,
                                               pair.interactionCompression + pair.interactionVelocity * dt - dt * 0.9f);
        pair.filmThickness += (1.0f - pair.filmThickness) * std::min(1.0f, dt * 1.6f);
        pair.ruptureRisk = 0.0f;
        pair.state = BubbleContactPair::State::Free;
        return;
    }

    DisplayBubble &a = g_DisplayBubbles[(size_t)aIndex];
    DisplayBubble &b = g_DisplayBubbles[(size_t)bIndex];
    glm::vec3 delta = b.position - a.position;
    float dist = glm::length(delta);
    if (dist < 1e-4f)
    {
        return;
    }

    glm::vec3 n = delta / dist;
    float contactDistance = a.radius + b.radius;
    float minRadius = std::min(a.radius, b.radius);
    float proximityDistance = contactDistance + minRadius * 0.012f;
    float proximity = Smooth01((proximityDistance - dist) / std::max(proximityDistance - contactDistance, 0.001f));
    bool inContact = pair.bonded || dist <= proximityDistance || pair.contactTime > 0.0f;

    if (!inContact)
    {
        pair.state = BubbleContactPair::State::Free;
        pair.contactTime = std::max(0.0f, pair.contactTime - dt * 1.8f);
        pair.bridgeStrength = std::max(0.0f, pair.bridgeStrength - dt * 2.4f);
        pair.geometryBlend = std::max(0.0f, pair.geometryBlend - dt * 2.0f);
        if (!pair.candidate)
        {
            pair.contactRadius = std::max(0.0f, pair.contactRadius - dt * 0.5f);
        }
        pair.contactRadiusVelocity *= std::exp(-dt * 8.0f);
        pair.interactionVelocity *= std::exp(-dt * 6.0f);
        pair.interactionCompression = std::max(0.0f,
                                               pair.interactionCompression + pair.interactionVelocity * dt - dt * 0.9f);
        pair.filmThickness += (1.0f - pair.filmThickness) * std::min(1.0f, dt * 1.6f);
        pair.ruptureRisk = 0.0f;
        return;
    }

    pair.visualTransitionTime = std::min(kContactVisualTransitionTime,
                                         pair.visualTransitionTime + dt);

    float relNormalSpeed = glm::dot(b.velocity - a.velocity, n);
    float contactAmount = std::max(proximity, Smooth01((contactDistance - dist + minRadius * 0.10f) / (minRadius * 0.26f)));
    float compression = contactAmount;
    pair.contactTime += dt * (0.45f + 0.90f * contactAmount);
    pair.filmThickness = std::max(0.12f, pair.filmThickness - dt * (0.018f + 0.050f * contactAmount));
    pair.ruptureRisk = Smooth01((0.35f - pair.filmThickness) / 0.09f) * Smooth01((std::abs(relNormalSpeed) - 0.55f) / 0.55f);
    DecideCoalescenceOutcome(pair, a, b, relNormalSpeed);
    if (pair.outcome == BubbleContactPair::CoalescenceOutcome::SeparateAfterContact &&
        pair.contactTime >= 0.72f)
    {
        if (!pair.separationImpulseApplied)
        {
            float invMassA = BubbleInvMass(a);
            float invMassB = BubbleInvMass(b);
            float invMassSum = std::max(invMassA + invMassB, 0.001f);
            float separationSpeed = glm::mix(
                0.10f, 0.18f,
                Smooth01(std::abs(relNormalSpeed) / 0.45f));
            a.velocity -= n * (separationSpeed * invMassA / invMassSum);
            b.velocity += n * (separationSpeed * invMassB / invMassSum);
            pair.separationImpulseApplied = true;
        }

        pair.separationElapsed += dt;
        pair.bonded = false;
        pair.candidate = pair.separationElapsed < 0.55f;
        pair.persistentRenderPair = pair.separationElapsed < 0.95f;
        pair.contactActivation *= std::exp(-dt * 3.8f);
        pair.bridgeStrength *= std::exp(-dt * 3.2f);
        pair.geometryBlend *= std::exp(-dt * 2.8f);
        pair.contactRadius = std::max(0.0f, pair.contactRadius - dt * minRadius * 0.42f);
        pair.filmThickness += (1.0f - pair.filmThickness) * std::min(1.0f, dt * 1.8f);
        pair.state = BubbleContactPair::State::Separated;
        a.state = DisplayBubble::State::Separated;
        b.state = DisplayBubble::State::Separated;
        if (!pair.persistentRenderPair)
        {
            pair.active = false;
        }
        return;
    }
    bool keepStableDouble =
        pair.outcome == BubbleContactPair::CoalescenceOutcome::StayDoubleBubble;
    if (keepStableDouble)
    {
        pair.filmThickness = std::max(pair.filmThickness, 0.31f);
    }
    float sharedFilmProgress = Smooth01(std::max(pair.contactTime - kSharedFilmTime, 0.0f) /
                                        std::max(kNeckFormationTime - kSharedFilmTime, 0.001f));
    float bondBlend = Smooth01(pair.contactTime / 0.30f);
    bool canCoalesce =
        pair.outcome == BubbleContactPair::CoalescenceOutcome::WillCoalesce ||
        (g_InteractionDemoActive && pair.outcome == BubbleContactPair::CoalescenceOutcome::Undecided);
    if (canCoalesce && pair.contactTime >= kNeckFormationTime &&
        !pair.fusionActive && PairCanBeginExclusiveFusion(pair))
    {
        pair.fusionActive = true;
        pair.fusionElapsed = 0.0f;
        pair.ruptureProgress = 0.0f;
        pair.neckProgress = 0.0f;
        pair.relaxationProgress = 0.0f;
        pair.fusionCompletionHold = 0.0f;
    }
    if (pair.fusionActive && !pair.fusionComplete)
    {
        pair.fusionElapsed += dt;
        pair.ruptureProgress = Smooth01(pair.fusionElapsed / kFilmRuptureDuration);
        pair.neckProgress = Smooth01(
            (pair.fusionElapsed - kNeckExpansionDelay) / kNeckExpansionDuration);
        pair.relaxationProgress = Smooth01(
            (pair.fusionElapsed - kRelaxationDelay) / kRelaxationDuration);
        pair.filmThickness = std::min(pair.filmThickness,
                                      glm::mix(kFusionFilmThickness, 0.08f,
                                               pair.ruptureProgress));
        pair.fusionCompletionHold = pair.relaxationProgress >= 0.999f
                                        ? pair.fusionCompletionHold + dt
                                        : 0.0f;
        pair.fusionComplete = pair.fusionCompletionHold >= kFusionCompletionHold;
    }

    // Film creation is history-dependent, while compression remains a
    // reversible response to current overlap, impact, and continuing forces.
    float baseFilmCompression = Smooth01(pair.contactTime / kNeckFormationTime);
    float effectiveFusionProgress = pair.fusionActive ? pair.neckProgress : 0.0f;
    float baseOverlapRatio = 0.020f * bondBlend + 0.105f * baseFilmCompression +
                             0.235f * effectiveFusionProgress;
    float actualOverlapRatio = (contactDistance - dist) / std::max(minRadius, 0.001f);
    float crowdingScale = 1.0f / std::sqrt((float)std::max(
                                     BubbleActiveContactCount(g_DisplayBubbles, g_ContactPairs, aIndex),
                                     BubbleActiveContactCount(g_DisplayBubbles, g_ContactPairs, bIndex)));
    float commandedOverlapRatio = baseOverlapRatio +
                                  0.240f * crowdingScale * pair.interactionCompression;
    float normalizedOverlap = Smooth01((actualOverlapRatio - commandedOverlapRatio) / 0.10f);
    float impactDrive = Smooth01(std::max(-relNormalSpeed, 0.0f) / 0.32f);
    float demoExternalDrive = 0.0f;
    if (g_InteractionDemoActive && pair.fusionActive)
    {
        float postMergeTime = pair.fusionElapsed;
        float forceEnvelope = Smooth01(postMergeTime / 1.6f);
        float forcePulse = 0.80f + 0.20f * std::sin(postMergeTime * 1.15f);
        demoExternalDrive = forceEnvelope * forcePulse;
    }
    float bondedCompressionDrive = pair.bonded
                                       ? 0.82f * crowdingScale *
                                             Smooth01(std::max(pair.contactTime - kSharedFilmTime, 0.0f) / 2.0f)
                                       : 0.0f;
    float compressionTarget = glm::clamp(std::max({0.72f * normalizedOverlap,
                                                   0.50f * impactDrive,
                                                   bondedCompressionDrive,
                                                   demoExternalDrive}),
                                         0.0f, 1.0f);
    float compressionAcceleration =
        (compressionTarget - pair.interactionCompression) * 13.0f -
        pair.interactionVelocity * 5.8f;
    pair.interactionVelocity += compressionAcceleration * dt;
    pair.interactionCompression = glm::clamp(
        pair.interactionCompression + pair.interactionVelocity * dt, 0.0f, 1.0f);
    float filmGrowth = std::max(
        std::pow(Smooth01(pair.contactTime / 1.05f), 0.55f),
        0.45f * pair.contactActivation);
    float crowdingRetention = std::pow(crowdingScale, 0.12f);
    float stableDoubleScale = keepStableDouble ? 0.82f : 1.0f;
    float targetContactRatio = (0.52f + 0.22f * pair.interactionCompression) *
                               stableDoubleScale *
                               crowdingRetention;
    float targetContactRadius = minRadius * targetContactRatio * filmGrowth;
    float radiusAcceleration = (targetContactRadius - pair.contactRadius) * 42.0f -
                               pair.contactRadiusVelocity * 14.0f;
    pair.contactRadiusVelocity += radiusAcceleration * dt;
    pair.contactRadius = glm::clamp(pair.contactRadius + pair.contactRadiusVelocity * dt,
                                    0.0f, minRadius * 0.78f);

    float planeOffsetA = std::sqrt(std::max(a.radius * a.radius -
                                                pair.contactRadius * pair.contactRadius,
                                            0.0f));
    glm::vec3 targetPlaneCenter = a.position + n * planeOffsetA;
    if (!pair.contactFrameInitialized)
    {
        pair.filteredNormal = n;
        pair.filteredPlaneCenter = targetPlaneCenter;
        pair.contactFrameInitialized = true;
    }
    else
    {
        float frameBlend = 1.0f - std::exp(-dt * 10.0f);
        glm::vec3 blendedNormal = glm::mix(pair.filteredNormal, n, frameBlend);
        if (glm::length(blendedNormal) > 1e-5f)
        {
            pair.filteredNormal = glm::normalize(blendedNormal);
        }
        pair.filteredPlaneCenter = glm::mix(pair.filteredPlaneCenter,
                                            targetPlaneCenter, frameBlend);
    }
    UpdateTransportedFusionFrame(pair, pair.filteredNormal);

    pair.bridgeStrength = Smooth01(pair.contactTime / 0.55f) * contactAmount;
    float geometryTarget = glm::mix(sharedFilmProgress, 1.0f, effectiveFusionProgress) * contactAmount;
    if (keepStableDouble)
    {
        geometryTarget = std::min(geometryTarget, 0.78f);
    }
    pair.geometryBlend = glm::clamp(geometryTarget, 0.0f, 1.0f);
    if (pair.contactTime < kSharedFilmTime || contactAmount < 0.10f)
    {
        pair.state = BubbleContactPair::State::Touch;
    }
    else if (keepStableDouble || !pair.fusionActive)
    {
        pair.state = BubbleContactPair::State::SharedFilm;
    }
    else if (pair.relaxationProgress <= 0.001f)
    {
        pair.state = BubbleContactPair::State::NeckForming;
    }
    else
    {
        pair.state = BubbleContactPair::State::Merged;
        pair.bridgeStrength = 1.0f;
        pair.geometryBlend = 1.0f;
        pair.filmThickness = std::min(pair.filmThickness, kFusionFilmThickness);
    }
    pair.neckRadius = pair.fusionActive
                          ? minRadius * glm::mix(0.025f, 0.78f,
                                                 std::pow(Smooth01(pair.neckProgress), 0.65f))
                          : PlateauContactRadius(pair, a, b);

    if (pair.state == BubbleContactPair::State::Touch)
    {
        a.state = DisplayBubble::State::Touch;
    }
    else if (pair.state == BubbleContactPair::State::SharedFilm)
    {
        a.state = DisplayBubble::State::SharedFilm;
    }
    else if (pair.state == BubbleContactPair::State::NeckForming)
    {
        a.state = DisplayBubble::State::NeckForming;
    }
    else
    {
        a.state = DisplayBubble::State::Merged;
    }
    b.state = a.state;
    float visualMergeProgress = std::max(pair.neckProgress * 0.55f,
                                         pair.relaxationProgress);
    a.mergeProgress = visualMergeProgress;
    b.mergeProgress = visualMergeProgress;
    a.contactTime = pair.contactTime;
    b.contactTime = pair.contactTime;
    float visibleContact = glm::clamp(pair.bridgeStrength * (0.55f + 0.65f * pair.geometryBlend), 0.0f, 1.0f);
    a.contactStrength = visibleContact;
    b.contactStrength = visibleContact;
    a.filmThickness = pair.filmThickness;
    b.filmThickness = pair.filmThickness;
    a.contactAxis = n;
    b.contactAxis = -n;
    if (pair.bridgeStrength > g_BridgeStrength)
    {
        g_BridgeStrength = pair.bridgeStrength;
        g_BridgeDirection = glm::normalize((a.position + b.position) * 0.5f);
        g_AutoTouchVelocity = 0.35f + pair.bridgeStrength * 0.45f;
    }
}

static void UpdateDisplayBubbleInteractions(float dt)
{
    dt = glm::clamp(dt, 0.0f, 1.0f / 30.0f);
    static double s_LastContactLogTime = -1.0;
    g_BridgeStrength = 0.0f;
    g_AutoTouchStrength *= expf(dt * -4.0f);
    g_AutoTouchVelocity *= expf(dt * -5.0f);

    if (g_SimPaused)
    {
        return;
    }

    std::unordered_map<uint64_t, glm::vec3> previousPositions;
    previousPositions.reserve(g_DisplayBubbles.size());
    for (const auto &bubble : g_DisplayBubbles)
    {
        previousPositions.emplace(bubble.id, bubble.position);
    }

    for (auto &pair : g_ContactPairs)
    {
        if (!pair.bonded)
        {
            pair.preContactProgress *= std::exp(-dt * 1.2f);
            pair.contactActivation *= std::exp(-dt * 8.0f);
        }
        int aIndex = -1;
        int bIndex = -1;
        if (pair.candidate && !pair.bonded &&
            ResolveContactPairIndices(g_DisplayBubbles, pair, aIndex, bIndex))
        {
            const DisplayBubble &a = g_DisplayBubbles[(size_t)aIndex];
            const DisplayBubble &b = g_DisplayBubbles[(size_t)bIndex];
            float minRadius = std::min(a.radius, b.radius);
            float releaseDistance = a.radius + b.radius + minRadius * 0.16f;
            if (glm::length(b.position - a.position) > releaseDistance)
            {
                pair.candidateExitTime += dt;
                if (pair.candidateExitTime > 0.28f)
                {
                    pair.candidate = false;
                    pair.candidateExitTime = 0.0f;
                }
            }
            else
            {
                pair.candidateExitTime = 0.0f;
            }
        }
        pair.active = pair.persistentRenderPair || pair.bonded || pair.candidate;
    }
    if (g_InteractionDemoActive)
    {
        for (auto &pair : g_ContactPairs)
        {
            int aIndex = -1;
            int bIndex = -1;
            if (ResolveContactPairIndices(g_DisplayBubbles, pair, aIndex, bIndex))
            {
                const DisplayBubble &a = g_DisplayBubbles[(size_t)aIndex];
                const DisplayBubble &b = g_DisplayBubbles[(size_t)bIndex];
                if (a.state == DisplayBubble::State::Burst ||
                    a.state == DisplayBubble::State::Dead ||
                    b.state == DisplayBubble::State::Burst ||
                    b.state == DisplayBubble::State::Dead)
                {
                    continue;
                }
                pair.active = true;
            }
        }
    }

    for (auto &bubble : g_DisplayBubbles)
    {
        if (bubble.state == DisplayBubble::State::Dead)
        {
            continue;
        }

    if (bubble.state == DisplayBubble::State::Burst)
    {
        // If we have a sim, physics drives the retraction 鈥?only manage
        // alpha fade.  Otherwise fall back to manual burstScale shrink.
        float desiredBurstElapsed = bubble.burstElapsed + dt;
        auto *sim = GetBubbleSim(bubble.id);
        bubble.burstElapsed = sim
            ? std::min(desiredBurstElapsed, LatestBurstVisualTime(bubble.id))
            : desiredBurstElapsed;
        float t = glm::clamp(bubble.burstElapsed / bubble.burstDuration,
                             0.0f, 1.0f);
        if (!sim)
        {
            float shrink = 1.0f - t * t * (3.0f - 2.0f * t);
            bubble.burstScale = std::max(shrink, 0.0f);
        }
        // Match Windows: show the clean opening and outward free-edge
        // retraction, then hide the sheet before late DBSTT motion becomes
        // visually chaotic.
        bubble.alpha = std::max(1.0f - t * 2.5f, 0.0f);
        bubble.velocity *= expf(dt * -2.5f);
        bubble.position += bubble.velocity * dt;
        if (t >= 1.0f)
            {
                bubble.state = DisplayBubble::State::Dead;
                bubble.alpha = 0.0f;
            }
            continue;
        }

        bool mergedBubble = bubble.volumeTransferred;
        bubble.state = mergedBubble
                           ? DisplayBubble::State::Merged
                           : DisplayBubble::State::Free;
        float swayScale = mergedBubble
                              ? 0.90f
                              : (g_InteractionDemoActive ? 0.45f : 1.0f);
        float swayAmplitude = (0.028f + bubble.floatAmplitude * 1.4f) * swayScale;
        glm::vec3 swayOffset = glm::vec3(
                                   sinf((float)g_Time * bubble.speed + bubble.phase) * 0.85f,
                                   cosf((float)g_Time * bubble.speed * 0.73f + bubble.phase * 1.31f),
                                   sinf((float)g_Time * bubble.speed * 0.57f + bubble.phase * 0.79f) * 0.48f) *
                               swayAmplitude;
        float swayPullRate = mergedBubble
                                 ? 0.22f
                                 : (g_InteractionDemoActive ? 0.16f : 0.34f);
        glm::vec3 swayPull =
            (bubble.basePosition + swayOffset - bubble.position) * swayPullRate;
        float windRadiusResponse = glm::clamp(std::sqrt(0.40f / std::max(bubble.radius, 0.08f)),
                                              0.58f, 1.35f);
        glm::vec3 globalWind = g_WindEnabled
                                   ? g_GlobalWindDirection * (g_GlobalWindStrength * windRadiusResponse)
                                   : glm::vec3(0.0f);
        bubble.velocity += (swayPull + globalWind) * dt;
        bubble.velocity *= expf(dt * (mergedBubble ? -0.72f : -0.48f));
        bubble.position += bubble.velocity * dt;
        if (mergedBubble)
        {
            bubble.contactStrength = 0.0f;
            bubble.surfaceDynamicsBlend = 0.0f;
        }
        else
        {
            bubble.contactStrength *= expf(dt * -3.0f);
        }
        if (bubble.targetVolume > 0.0f)
        {
            float targetRadius = RadiusFromVolume(bubble.targetVolume);
            bubble.radius += (targetRadius - bubble.radius) * std::min(1.0f, dt * 1.8f);
        }
        if (bubble.state == DisplayBubble::State::Free)
        {
            bubble.filmThickness += (1.0f - bubble.filmThickness) * std::min(1.0f, dt * 0.4f);
        }
    }

    // Remove bubbles that finished their burst animation.
    std::vector<uint64_t> deadBubbleIds;
    for (const auto &bubble : g_DisplayBubbles)
    {
        if (bubble.state == DisplayBubble::State::Dead)
        {
            deadBubbleIds.push_back(bubble.id);
        }
    }
    for (uint64_t id : deadBubbleIds)
    {
        RemoveBubble(id);
        std::cout << "[BubbleBurst] removed id=" << id
                  << " live=" << LiveDisplayBubbleCount() << std::endl;
    }

    // The interaction replay is a controlled visual experiment. Move the two
    // bubbles toward first contact at a deterministic rate so frame rate,
    // damping, and the weak ambient drift cannot stall the demonstration.
    if (g_InteractionDemoActive)
    {
        for (auto &pair : g_ContactPairs)
        {
            int aIndex = -1;
            int bIndex = -1;
            if (!pair.active || pair.contactTime > 0.0f ||
                !ResolveContactPairIndices(g_DisplayBubbles, pair, aIndex, bIndex))
            {
                continue;
            }
            DisplayBubble &a = g_DisplayBubbles[(size_t)aIndex];
            DisplayBubble &b = g_DisplayBubbles[(size_t)bIndex];
            if (a.state == DisplayBubble::State::Burst ||
                a.state == DisplayBubble::State::Dead ||
                b.state == DisplayBubble::State::Burst ||
                b.state == DisplayBubble::State::Dead)
            {
                continue;
            }
            glm::vec3 delta = b.position - a.position;
            float dist = glm::length(delta);
            if (dist <= 1e-5f)
            {
                continue;
            }
            glm::vec3 n = delta / dist;
            float targetDistance = a.radius + b.radius + std::min(a.radius, b.radius) * 0.006f;
            float remaining = dist - targetDistance;
            if (remaining > 0.0f)
            {
                float step = std::min(remaining, dt * 0.115f);
                float invMassA = BubbleInvMass(a);
                float invMassB = BubbleInvMass(b);
                float invMassSum = invMassA + invMassB;
                a.position += n * step * (invMassA / invMassSum);
                b.position -= n * step * (invMassB / invMassSum);
            }
        }
    }

    std::vector<std::pair<int, int>> broadPhasePairs = BuildBubbleBroadPhasePairs(g_DisplayBubbles);
    for (const auto &candidatePair : broadPhasePairs)
    {
        int i = candidatePair.first;
        int j = candidatePair.second;
        auto &a = g_DisplayBubbles[(size_t)i];
        auto &b = g_DisplayBubbles[(size_t)j];

        // Never create or reactivate contacts involving bursting/dead bubbles.
        if (a.state == DisplayBubble::State::Burst ||
            a.state == DisplayBubble::State::Dead ||
            b.state == DisplayBubble::State::Burst ||
            b.state == DisplayBubble::State::Dead)
        {
            continue;
        }

        int existingPairIndex = FindContactPair(g_ContactPairs, a.id, b.id);
        if (existingPairIndex >= 0)
        {
            const BubbleContactPair &existingPair =
                g_ContactPairs[(size_t)existingPairIndex];
            if (existingPair.outcome ==
                    BubbleContactPair::CoalescenceOutcome::SeparateAfterContact &&
                existingPair.separationElapsed < 1.25f)
            {
                continue;
            }
        }

        glm::vec3 delta = b.position - a.position;
        float dist = glm::length(delta);
        float contactDistance = a.radius + b.radius;
        float minRadius = std::min(a.radius, b.radius);
        float nearRange = minRadius * 0.080f;
        float contactEpsilon = minRadius * 0.012f;
        if (dist > 1e-4f && dist < contactDistance + nearRange)
        {
            glm::vec3 n = delta / dist;
            float gap = dist - contactDistance;
            float contactActivation = Smooth01((nearRange - gap) /
                                               std::max(nearRange, 0.001f));
            float capillaryPull = 0.100f * contactActivation;
            float invMassA = BubbleInvMass(a);
            float invMassB = BubbleInvMass(b);
            float invMassSum = std::max(invMassA + invMassB, 0.001f);
            float capillaryImpulse = 2.0f * capillaryPull * dt / invMassSum;
            a.velocity += n * capillaryImpulse * invMassA;
            b.velocity -= n * capillaryImpulse * invMassB;

            int pairIndex = EnsureContactPairByIndex(g_DisplayBubbles, g_ContactPairs, i, j);
            if (pairIndex < 0)
            {
                continue;
            }
            BubbleContactPair &pair = g_ContactPairs[(size_t)pairIndex];
            if (pair.outcome == BubbleContactPair::CoalescenceOutcome::Undecided &&
                !pair.outcomeRandomSampled)
            {
                // Sample once when this pair first enters the contact zone.
                // The stored value survives brief bounce/re-contact cycles and
                // is discarded only when the contact pair itself expires.
                pair.coalescenceRandom = NextContactOutcomeRandom01();
                pair.outcomeRandomSampled = true;
            }
            pair.active = true;
            pair.candidate = true;
            pair.candidateExitTime = 0.0f;
            pair.contactActivation = contactActivation;
            pair.preContactProgress = contactActivation;
            float preContactRadius = minRadius * 0.10f *
                                     contactActivation * contactActivation;
            pair.contactRadius = std::max(pair.contactRadius, preContactRadius);
            float planeOffsetA = std::sqrt(std::max(a.radius * a.radius -
                                                        pair.contactRadius * pair.contactRadius,
                                                    0.0f));
            glm::vec3 targetPlaneCenter = a.position + n * planeOffsetA;
            if (!pair.contactFrameInitialized)
            {
                pair.filteredNormal = n;
                pair.filteredPlaneCenter = targetPlaneCenter;
                pair.contactFrameInitialized = true;
            }
            else
            {
                float frameBlend = 1.0f - std::exp(-dt * 12.0f);
                glm::vec3 blendedNormal = glm::mix(pair.filteredNormal, n, frameBlend);
                if (glm::length(blendedNormal) > 1e-5f)
                {
                    pair.filteredNormal = glm::normalize(blendedNormal);
                }
                pair.filteredPlaneCenter = glm::mix(pair.filteredPlaneCenter,
                                                    targetPlaneCenter, frameBlend);
            }
            pair.restDistance = contactDistance;
            pair.targetVolume = a.targetVolume + b.targetVolume;
        }
        if (dist <= 1e-4f || dist > contactDistance + contactEpsilon)
        {
            continue;
        }

        int pairIndex = EnsureContactPairByIndex(g_DisplayBubbles, g_ContactPairs, i, j);
        if (pairIndex < 0)
        {
            continue;
        }
        BubbleContactPair &pair = g_ContactPairs[(size_t)pairIndex];
        bool firstBondFrame = !pair.bonded;
        pair.active = true;
        pair.bonded = true;
        pair.candidate = true;
        pair.candidateExitTime = 0.0f;
        if (firstBondFrame)
        {
            float inheritedContact = glm::clamp(pair.preContactProgress, 0.0f, 1.0f);
            glm::vec3 bondNormal = delta / dist;
            float incomingSpeed = std::max(-glm::dot(b.velocity - a.velocity,
                                                     bondNormal),
                                           0.0f);
            pair.contactTime = std::max(pair.contactTime, 0.20f * inheritedContact);
            pair.contactRadius = std::max(pair.contactRadius,
                                          minRadius * 0.14f * inheritedContact);
            pair.contactRadiusVelocity = std::max(pair.contactRadiusVelocity,
                                                  incomingSpeed * 0.22f);
            pair.interactionCompression = std::max(pair.interactionCompression,
                                                   0.10f * inheritedContact);
            pair.interactionVelocity = std::max(pair.interactionVelocity,
                                                incomingSpeed / std::max(minRadius, 0.001f) * 0.06f);
        }
        pair.restDistance = contactDistance;
        pair.targetVolume = a.targetVolume + b.targetVolume;
    }

    const int solverIterations = 6;
    const float mainRadius = g_MainBubbleVisualRadius;
    for (int iter = 0; iter < solverIterations; ++iter)
    {
        for (auto &pair : g_ContactPairs)
        {
            int aIndex = -1;
            int bIndex = -1;
            if (!pair.active || !ResolveContactPairIndices(g_DisplayBubbles, pair, aIndex, bIndex))
            {
                continue;
            }

            DisplayBubble &a = g_DisplayBubbles[(size_t)aIndex];
            DisplayBubble &b = g_DisplayBubbles[(size_t)bIndex];
            if (a.state == DisplayBubble::State::Burst ||
                a.state == DisplayBubble::State::Dead ||
                b.state == DisplayBubble::State::Burst ||
                b.state == DisplayBubble::State::Dead)
            {
                continue;
            }

            glm::vec3 delta = b.position - a.position;
            float dist = glm::length(delta);
            if (dist < 1e-5f)
            {
                delta = glm::vec3(1.0f, 0.0f, 0.0f);
                dist = 1.0f;
            }

            glm::vec3 n = delta / dist;
            float minRadius = std::min(a.radius, b.radius);
            float contactEpsilon = minRadius * 0.012f;
            if (!pair.bonded && dist > a.radius + b.radius + contactEpsilon)
            {
                continue;
            }
            float bondBlend = Smooth01(pair.contactTime / 0.30f);
            float contactRadius = glm::clamp(pair.contactRadius,
                                             0.0f, minRadius * 0.82f);
            float restDistance =
                std::sqrt(std::max(a.radius * a.radius - contactRadius * contactRadius, 0.0f)) +
                std::sqrt(std::max(b.radius * b.radius - contactRadius * contactRadius, 0.0f));
            float constraint = dist - restDistance;
            if (constraint >= 0.0f)
            {
                float contactMargin = minRadius * (pair.bonded ? 0.24f : 0.035f);
                float adhesionRange = contactMargin * 1.15f;
                float adhesion = Smooth01((restDistance + adhesionRange - dist) / std::max(adhesionRange, 0.001f));
                adhesion *= 0.030f + 0.090f * Smooth01(pair.contactTime / 0.85f);
                adhesion *= bondBlend;
                if (adhesion <= 0.0f)
                {
                    continue;
                }

                float invMassA = BubbleInvMass(a);
                float invMassB = BubbleInvMass(b);
                float correctionMagnitude = std::min(constraint * adhesion, minRadius * 0.0065f);
                glm::vec3 correction = n * correctionMagnitude;
                a.position += correction * invMassA / (invMassA + invMassB);
                b.position -= correction * invMassB / (invMassA + invMassB);
                continue;
            }

            float invMassA = BubbleInvMass(a);
            float invMassB = BubbleInvMass(b);
            if (pair.bonded && pair.contactTime < 0.24f)
            {
                // A soap film absorbs the initial overlap as local shape
                // deformation. Do not kick the centers apart before the
                // contact circle has had time to grow.
                continue;
            }
            float compliance = 0.0007f / std::max(dt * dt, 1e-5f);
            float correctionMagnitude = (-constraint) / (invMassA + invMassB + compliance);
            glm::vec3 correction = n * correctionMagnitude;
            a.position -= correction * invMassA;
            b.position += correction * invMassB;
        }

        for (auto &bubble : g_DisplayBubbles)
        {
            if (bubble.state == DisplayBubble::State::Dead || !g_ShowMainBubble)
            {
                continue;
            }

            glm::vec3 delta = bubble.position;
            float dist = glm::length(delta);
            if (dist < 1e-5f)
            {
                delta = glm::vec3(1.0f, 0.0f, 0.0f);
                dist = 1.0f;
            }

            glm::vec3 n = delta / dist;
            float contactProgress = Smooth01(bubble.contactTime / 1.0f);
            float restDistance = mainRadius + bubble.radius - bubble.radius * (0.025f + 0.075f * contactProgress);
            if (dist < restDistance)
            {
                bubble.position += n * (restDistance - dist);
            }
        }
    }

    if (dt > 1e-5f)
    {
        for (size_t i = 0; i < g_DisplayBubbles.size(); ++i)
        {
            auto &bubble = g_DisplayBubbles[i];
            if (bubble.state == DisplayBubble::State::Dead)
            {
                continue;
            }

            auto previousPosition = previousPositions.find(bubble.id);
            if (previousPosition == previousPositions.end())
            {
                continue;
            }
            bubble.velocity = (bubble.position - previousPosition->second) / dt;
            float speed = glm::length(bubble.velocity);
            if (speed > 1.20f)
            {
                bubble.velocity *= 1.20f / speed;
            }
        }
    }

    for (auto &pair : g_ContactPairs)
    {
        int aIndex = -1;
        int bIndex = -1;
        if (!pair.active || !ResolveContactPairIndices(g_DisplayBubbles, pair, aIndex, bIndex))
        {
            UpdateBubblePair(pair, dt);
            continue;
        }

        DisplayBubble &a = g_DisplayBubbles[(size_t)aIndex];
        DisplayBubble &b = g_DisplayBubbles[(size_t)bIndex];
        glm::vec3 delta = b.position - a.position;
        float dist = glm::length(delta);
        float contactDistance = a.radius + b.radius;
        float contactEpsilon = std::min(a.radius, b.radius) * 0.012f;
        bool resolveContactImpulse = pair.candidate || pair.bonded ||
                                     dist <= contactDistance + contactEpsilon;
        if (dist > 1e-5f && resolveContactImpulse)
        {
            glm::vec3 n = delta / dist;
            float relNormalSpeed = glm::dot(b.velocity - a.velocity, n);
            if (pair.bonded || relNormalSpeed < 0.0f)
            {
                float invMassA = BubbleInvMass(a);
                float invMassB = BubbleInvMass(b);
                float activation = glm::clamp(pair.contactActivation, 0.0f, 1.0f);
                float dampingRate = pair.bonded
                                        ? glm::mix(1.2f, 6.2f, activation)
                                        : glm::mix(0.5f, 3.5f, activation);
                float normalDamping = 1.0f - std::exp(-dampingRate * dt);
                float absorbedClosingSpeed = std::max(-relNormalSpeed, 0.0f) * normalDamping;
                if (absorbedClosingSpeed > 0.0f)
                {
                    float minRadius = std::max(std::min(a.radius, b.radius), 0.001f);
                    pair.contactRadiusVelocity += absorbedClosingSpeed * 0.25f;
                    pair.interactionVelocity += absorbedClosingSpeed / minRadius * 0.05f;
                }
                float impulseMagnitude = (-relNormalSpeed * normalDamping) /
                                         (invMassA + invMassB);
                glm::vec3 impulse = n * impulseMagnitude;
                a.velocity -= impulse * invMassA;
                b.velocity += impulse * invMassB;
            }
        }
        UpdateBubblePair(pair, dt);
    }

    for (auto &bubble : g_DisplayBubbles)
    {
        if (bubble.state == DisplayBubble::State::Dead || !g_ShowMainBubble)
        {
            continue;
        }

        glm::vec3 delta = bubble.position;
        float dist = glm::length(delta);
        if (dist < 1e-5f)
        {
            continue;
        }

        glm::vec3 n = delta / dist;
        float contactDistance = mainRadius + bubble.radius;
        float proximityDistance = contactDistance + bubble.radius * 0.28f;
        if (dist < proximityDistance)
        {
            float strength = Smooth01((proximityDistance - dist) / std::max(proximityDistance - contactDistance, 0.001f));
            bubble.contactTime += dt * (0.35f + 0.65f * strength);
            bubble.contactStrength = std::max(bubble.contactStrength, strength * 0.55f);
            bubble.filmThickness = std::max(0.32f, bubble.filmThickness - dt * 0.035f * strength);
            bubble.contactAxis = -n;
            bubble.state = strength > 0.65f ? DisplayBubble::State::SharedFilm : DisplayBubble::State::Touch;
            float relSpeed = glm::dot(bubble.velocity, n);
            if (relSpeed < 0.0f)
            {
                bubble.velocity -= n * relSpeed * 0.72f;
            }

            if (strength > g_BridgeStrength)
            {
                g_BridgeStrength = strength;
                g_BridgeDirection = n;
                g_AutoTouchVelocity = 0.25f + strength * 0.55f;
            }
        }
        else
        {
            bubble.contactTime = std::max(0.0f, bubble.contactTime - dt * 1.2f);
        }
    }

    UpdatePersistentSurfaceDynamics(dt);
    FinalizeCompletedFusions();

    for (const BubbleContactPair &pair : g_ContactPairs)
    {
        if (!pair.active && pair.contactTime <= 0.01f && pair.geometryBlend <= 0.01f)
        {
            g_BubbleSurfaces.RemoveContact(pair.a, pair.b);
        }
    }
    g_ContactPairs.erase(
        std::remove_if(g_ContactPairs.begin(), g_ContactPairs.end(), [](const BubbleContactPair &pair)
                       { return !pair.active && pair.contactTime <= 0.01f && pair.geometryBlend <= 0.01f; }),
        g_ContactPairs.end());

    if (g_LogContactDebug && !g_ContactPairs.empty() && g_Time - s_LastContactLogTime >= 1.0)
    {
        const auto &pair = g_ContactPairs.front();
        int aIndex = -1;
        int bIndex = -1;
        if (ResolveContactPairIndices(g_DisplayBubbles, pair, aIndex, bIndex))
        {
            const auto &a = g_DisplayBubbles[(size_t)aIndex];
            const auto &b = g_DisplayBubbles[(size_t)bIndex];
            float dist = glm::length(b.position - a.position);
            std::cout << "[BubbleContact] dist=" << dist
                      << " sumR=" << (a.radius + b.radius)
                      << " active=" << pair.active
                      << " bonded=" << pair.bonded
                      << " candidate=" << pair.candidate
                      << " state=" << (int)pair.state
                      << " t=" << pair.contactTime
                      << " bridge=" << pair.bridgeStrength
                      << " geom=" << pair.geometryBlend
                      << " compression=" << pair.interactionCompression
                      << " activation=" << pair.contactActivation
                      << " outcome=" << CoalescenceOutcomeName(pair.outcome)
                      << " coalesceScore=" << pair.coalescenceScore
                      << " contactR=" << pair.contactRadius
                      << " film=" << pair.filmThickness
                      << std::endl;
            s_LastContactLogTime = g_Time;
        }
    }
}
} // namespace windows_parity
