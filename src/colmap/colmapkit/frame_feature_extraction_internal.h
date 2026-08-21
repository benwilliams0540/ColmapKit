// Copyright (c), ETH Zurich and UNC Chapel Hill.
// All rights reserved.

#pragma once

#include "colmap/colmapkit/colmapkit.h"
#include "colmap/feature/types.h"

#include <string>
#include <string_view>

namespace colmap::internal {

std::string FrameFeatureSHA256(std::string_view input);

// Apply the FrameFeatureExtractionV1 hard terminal row cap while preserving
// keypoint/descriptor alignment. Exact-bound payloads remain unchanged.
void ApplyFrameFeatureTerminalRowLimitV1(uint32_t max_num_features,
                                         FeatureKeypoints* keypoints,
                                         FeatureDescriptors* descriptors);

struct FrameFeatureArtifactDataV1 {
  ColmapKitFrameFeatureResultV1 result{};
  ColmapKitFrameMetadataV1 metadata{};
  FeatureKeypoints keypoints;
  FeatureDescriptors descriptors;
};

FrameFeatureArtifactDataV1 LoadFrameFeatureArtifactV1(
    const ColmapKitFrameFeatureExtractorV1* extractor,
    const ColmapKitFrameFeatureArtifactExpectationV1& expectation);

}  // namespace colmap::internal
