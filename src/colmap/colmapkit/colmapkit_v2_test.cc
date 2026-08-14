// Copyright (c), ETH Zurich and UNC Chapel Hill.
// All rights reserved.

#include "colmap/colmapkit/colmapkit.h"

#include "colmap/sensor/bitmap.h"
#include "colmap/util/testing.h"

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace colmap {
namespace {

struct GuardedTrackedResult {
  ColmapKitTrackedPoseResultV2 result{};
  std::array<uint8_t, 32> guard{};
};

struct GuardedPriorResult {
  ColmapKitRGBPriorResultV2 result{};
  std::array<uint8_t, 32> guard{};
};

std::vector<std::string> CreateImages(const std::filesystem::path& directory) {
  std::filesystem::create_directories(directory);
  Bitmap bitmap(32, 24, true);
  bitmap.Fill(BitmapColor<uint8_t>(64, 128, 192));
  std::vector<std::string> paths;
  for (int i = 0; i < 3; ++i) {
    const auto path = directory / ("frame_" + std::to_string(i) + ".png");
    EXPECT_TRUE(bitmap.Write(path));
    paths.push_back(path.string());
  }
  return paths;
}

std::vector<ColmapKitTrackedImageV2> MakeImages(
    const std::vector<std::string>& paths) {
  std::vector<ColmapKitTrackedImageV2> images(paths.size());
  for (size_t i = 0; i < images.size(); ++i) {
    images[i].struct_size = sizeof(images[i]);
    images[i].camera_model = COLMAPKIT_CAMERA_MODEL_V2_PINHOLE;
    images[i].stable_id = 100 + i;
    images[i].order_index = static_cast<uint32_t>(i);
    images[i].encoded_width = 32;
    images[i].encoded_height = 24;
    images[i].num_camera_params = 4;
    images[i].camera_params[0] = 30;
    images[i].camera_params[1] = 30;
    images[i].camera_params[2] = 16;
    images[i].camera_params[3] = 12;
    images[i].world_from_camera[0] = 1;
    images[i].world_from_camera[5] = 1;
    images[i].world_from_camera[10] = 1;
    images[i].world_from_camera[15] = 1;
    images[i].world_from_camera[12] = 0.1 * i;
    images[i].tracking_state = COLMAPKIT_TRACKING_STATE_V2_NORMAL;
    images[i].inclusion_flags = COLMAPKIT_TRACKED_IMAGE_FLAG_V2_INCLUDED;
    images[i].translation_weight = 1;
    images[i].rotation_weight = 1;
    images[i].image_path = paths[i].c_str();
  }
  return images;
}

ColmapKitTrackedPoseConfigV2 InvalidFastConfig(
    const std::vector<ColmapKitTrackedImageV2>& images,
    const std::filesystem::path& directory) {
  static std::string database, model, poses, evidence;
  database = (directory / "database.db").string();
  model = (directory / "model").string();
  poses = (directory / "poses.json").string();
  evidence = (directory / "evidence.json").string();
  ColmapKitTrackedPoseConfigV2 config{};
  config.struct_size = sizeof(config);
  config.images = images.data();
  config.num_images = static_cast<uint32_t>(images.size());
  // A zero feature bound makes validation fail before any work begins.
  config.max_features_per_image = 0;
  config.temporal_neighbor_count = 1;
  config.max_revisit_neighbors_per_image = 1;
  config.max_image_pairs = 3;
  config.max_triangulation_passes = 1;
  config.max_bundle_adjustment_iterations = 1;
  config.num_threads = 1;
  config.revisit_min_translation_meters = 0;
  config.revisit_max_translation_meters = 2;
  config.revisit_max_rotation_degrees = 90;
  config.min_triangulation_angle_degrees = 0.5;
  config.max_reprojection_error_pixels = 4;
  config.max_allowed_scale_drift_ratio = 1e-9;
  config.database_path = database.c_str();
  config.output_model_path = model.c_str();
  config.refined_pose_path = poses.c_str();
  config.evidence_path = evidence.c_str();
  return config;
}

TEST(ColmapKitV2, ReportsSeparateABIReleaseAndEngineIdentity) {
  EXPECT_EQ(ColmapKitGetABIVersionV2(), 2u);
  EXPECT_STREQ(ColmapKitGetReleaseVersionV2(), "0.3.0-dev");
  ASSERT_NE(ColmapKitGetEngineBuildIdentityV2(), nullptr);
  EXPECT_NE(std::strlen(ColmapKitGetEngineBuildIdentityV2()), 0u);
  // Legacy behavior remains the underlying engine version.
  EXPECT_NE(std::string(ColmapKitVersion()).find("COLMAP"), std::string::npos);
}

TEST(ColmapKitV2, RejectsZeroAndTruncatedInputSizes) {
  ColmapKitTrackedPoseConfigV2 config{};
  ColmapKitTrackedPoseResultV2 result{};
  result.struct_size = sizeof(result);
  EXPECT_EQ(ColmapKitRunTrackedPoseReconstructionV2(&config, &result),
            COLMAPKIT_STATUS_INVALID_ARGUMENT);

  config.struct_size =
      offsetof(ColmapKitTrackedPoseConfigV2, evidence_path);
  result = {};
  result.struct_size = sizeof(result);
  EXPECT_EQ(ColmapKitRunTrackedPoseReconstructionV2(&config, &result),
            COLMAPKIT_STATUS_INVALID_ARGUMENT);

  ColmapKitRGBPriorConfigV2 prior{};
  ColmapKitRGBPriorResultV2 prior_result{};
  prior_result.struct_size = sizeof(prior_result);
  EXPECT_EQ(ColmapKitRunRGBGaussianPriorV2(&prior, &prior_result),
            COLMAPKIT_STATUS_INVALID_ARGUMENT);
}

TEST(ColmapKitV2, WritesNoMoreThanCallerResultSize) {
  GuardedTrackedResult guarded;
  guarded.guard.fill(0xa5);
  guarded.result.struct_size = sizeof(guarded.result) + guarded.guard.size();
  ColmapKitTrackedPoseConfigV2 config{};
  config.struct_size = sizeof(config);
  EXPECT_EQ(ColmapKitRunTrackedPoseReconstructionV2(&config, &guarded.result),
            COLMAPKIT_STATUS_INVALID_ARGUMENT);
  EXPECT_TRUE(std::all_of(guarded.guard.begin(), guarded.guard.end(),
                          [](uint8_t value) { return value == 0xa5; }));

  GuardedPriorResult prior_guarded;
  prior_guarded.guard.fill(0x5a);
  prior_guarded.result.struct_size =
      sizeof(prior_guarded.result) + prior_guarded.guard.size();
  ColmapKitRGBPriorConfigV2 prior{};
  prior.struct_size = sizeof(prior);
  EXPECT_EQ(ColmapKitRunRGBGaussianPriorV2(&prior, &prior_guarded.result),
            COLMAPKIT_STATUS_INVALID_ARGUMENT);
  EXPECT_TRUE(std::all_of(prior_guarded.guard.begin(), prior_guarded.guard.end(),
                          [](uint8_t value) { return value == 0x5a; }));
}

TEST(ColmapKitV2, SupportsMinimumOutputPrefixWithoutOverwritingCanary) {
  struct TinyResult {
    uint32_t struct_size;
    uint32_t status;
    uint64_t canary;
  } output{offsetof(TinyResult, canary), 0, 0x1122334455667788ULL};
  ColmapKitTrackedPoseConfigV2 config{};
  config.struct_size = sizeof(config);
  EXPECT_EQ(ColmapKitRunTrackedPoseReconstructionV2(
                &config,
                reinterpret_cast<ColmapKitTrackedPoseResultV2*>(&output)),
            COLMAPKIT_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(output.struct_size, offsetof(TinyResult, canary));
  EXPECT_EQ(output.canary, 0x1122334455667788ULL);
}

TEST(ColmapKitV2, RejectsNonRigidARKitTransform) {
  const auto directory = CreateTestDir();
  auto paths = CreateImages(directory / "images");
  auto images = MakeImages(paths);
  images[1].world_from_camera[0] = 2.0;
  auto config = InvalidFastConfig(images, directory);
  config.max_features_per_image = 128;
  ColmapKitTrackedPoseResultV2 result{};
  result.struct_size = sizeof(result);
  EXPECT_EQ(ColmapKitRunTrackedPoseReconstructionV2(&config, &result),
            COLMAPKIT_STATUS_INVALID_ARGUMENT);
  EXPECT_NE(std::string(result.message).find("rigid"), std::string::npos);
}

TEST(ColmapKitV2, RejectsOutputPathAliasingAnRGBInputWithoutMutation) {
  const auto directory = CreateTestDir();
  auto paths = CreateImages(directory / "images");
  auto images = MakeImages(paths);
  auto config = InvalidFastConfig(images, directory);
  config.max_features_per_image = 128;
  config.database_path = paths[0].c_str();
  std::ifstream stream(paths[0], std::ios::binary);
  const std::string before((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
  ColmapKitTrackedPoseResultV2 result{};
  result.struct_size = sizeof(result);
  EXPECT_EQ(ColmapKitRunTrackedPoseReconstructionV2(&config, &result),
            COLMAPKIT_STATUS_INVALID_ARGUMENT);
  std::ifstream after_stream(paths[0], std::ios::binary);
  const std::string after((std::istreambuf_iterator<char>(after_stream)),
                          std::istreambuf_iterator<char>());
  EXPECT_EQ(after, before);
  EXPECT_NE(std::string(result.message).find("aliases"), std::string::npos);
}

TEST(ColmapKitV2, AsyncJobOwnsInputsAndWaitIsRepeatable) {
  const auto directory = CreateTestDir();
  auto paths = CreateImages(directory / "images");
  auto images = MakeImages(paths);
  auto config = InvalidFastConfig(images, directory);
  ColmapKitTrackedPoseJobV2* job = nullptr;
  ASSERT_EQ(ColmapKitStartTrackedPoseReconstructionV2(&config, &job),
            COLMAPKIT_STATUS_OK);
  ASSERT_NE(job, nullptr);
  paths.clear();
  images.clear();
  ColmapKitTrackedPoseResultV2 first{};
  first.struct_size = sizeof(first);
  EXPECT_EQ(ColmapKitWaitTrackedPoseReconstructionV2(job, &first),
            COLMAPKIT_STATUS_INVALID_ARGUMENT);
  ColmapKitTrackedPoseResultV2 second{};
  second.struct_size = sizeof(second);
  EXPECT_EQ(ColmapKitWaitTrackedPoseReconstructionV2(job, &second),
            COLMAPKIT_STATUS_INVALID_ARGUMENT);
  EXPECT_STREQ(first.message, second.message);
  ColmapKitReleaseTrackedPoseReconstructionJobV2(job);
}

}  // namespace
}  // namespace colmap
