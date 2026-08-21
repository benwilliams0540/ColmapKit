// Copyright (c), ETH Zurich and UNC Chapel Hill.
// All rights reserved.

#include "colmap/colmapkit/colmapkit.h"
#include "colmap/colmapkit/frame_feature_extraction_internal.h"
#include "colmap/feature/extractor.h"
#include "colmap/feature/sift.h"
#include "colmap/sensor/bitmap.h"
#include "colmap/util/testing.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace colmap {
namespace {

constexpr uint32_t kWidth = 320;
constexpr uint32_t kHeight = 240;

struct GuardedFrameFeatureError {
  ColmapKitFrameFeatureErrorV1 error{};
  std::array<uint8_t, 32> guard{};
};

struct GuardedFrameFeatureResult {
  ColmapKitFrameFeatureResultV1 result{};
  std::array<uint8_t, 32> guard{};
};

std::vector<uint8_t> ReadBytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream),
                              std::istreambuf_iterator<char>());
}

void WriteBytes(const std::filesystem::path& path,
                const std::vector<uint8_t>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::string HashBytes(const std::vector<uint8_t>& bytes) {
  return internal::FrameFeatureSHA256(std::string_view(
      reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

uint64_t ExpectedAdmissionBytes(
    const ColmapKitFrameFeatureExtractorConfigV1& config,
    const ColmapKitFrameMetadataV1& metadata,
    const uint64_t encoded_size) {
  const double scale =
      std::min(1.0,
               static_cast<double>(config.max_image_size) /
                   std::max(metadata.encoded_width, metadata.encoded_height));
  const uint64_t width = std::max<uint64_t>(
      1, static_cast<uint64_t>(std::ceil(metadata.encoded_width * scale)));
  const uint64_t height = std::max<uint64_t>(
      1, static_cast<uint64_t>(std::ceil(metadata.encoded_height * scale)));
  const uint64_t first_octave_factor = config.first_octave < 0 ? 4 : 1;
  return encoded_size * 2 + width * height * first_octave_factor * 96 +
         static_cast<uint64_t>(config.max_num_features) * 256 +
         16ULL * 1024ULL * 1024ULL;
}

ColmapKitFrameFeatureExtractorConfigV1 DefaultConfig() {
  ColmapKitFrameFeatureExtractorConfigV1 config{};
  config.struct_size = sizeof(config);
  config.abi_version = COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1;
  config.requested_backend = COLMAPKIT_FRAME_FEATURE_BACKEND_V1_CPU;
  config.worker_count = 1;
  config.max_encoded_image_bytes = 16ULL * 1024ULL * 1024ULL;
  config.memory_admission_budget_bytes = 256ULL * 1024ULL * 1024ULL;
  config.max_image_size = kWidth;
  config.max_num_features = 2048;
  config.first_octave = -1;
  config.num_octaves = 4;
  config.octave_resolution = 3;
  config.max_num_orientations = 2;
  config.upright = 0;
  config.normalization = COLMAPKIT_SIFT_NORMALIZATION_V1_L1_ROOT;
  config.peak_threshold = 0.006666666666666667;
  config.edge_threshold = 10.0;
  return config;
}

ColmapKitFrameFeatureErrorV1 MakeError() {
  ColmapKitFrameFeatureErrorV1 error{};
  error.struct_size = sizeof(error);
  error.abi_version = COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1;
  return error;
}

ColmapKitFrameFeatureResultV1 MakeResult() {
  ColmapKitFrameFeatureResultV1 result{};
  result.struct_size = sizeof(result);
  result.abi_version = COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1;
  return result;
}

struct FrameFixture {
  explicit FrameFixture(const bool textured = true)
      : directory(CreateTestDir()) {
    Bitmap bitmap(kWidth, kHeight, true);
    if (textured) {
      for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
          const uint8_t checker = ((x / 12 + y / 12) % 2) ? 210 : 35;
          const uint8_t detail = static_cast<uint8_t>((x * 17 + y * 31) % 43);
          bitmap.SetPixel(
              x,
              y,
              BitmapColor<uint8_t>(
                  checker,
                  static_cast<uint8_t>((checker + detail) % 256),
                  static_cast<uint8_t>((255 - checker + detail) % 256)));
        }
      }
    } else {
      bitmap.Fill(BitmapColor<uint8_t>(127));
    }
    bitmap.SetJpegQuality(100);
    const auto jpeg_path = directory / "frame.jpg";
    EXPECT_TRUE(bitmap.Write(jpeg_path));
    encoded = ReadBytes(jpeg_path);
    image_sha256 = HashBytes(encoded);

    metadata.struct_size = sizeof(metadata);
    metadata.abi_version = COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1;
    metadata.image_format = COLMAPKIT_FRAME_IMAGE_FORMAT_V1_JPEG;
    metadata.encoded_width = kWidth;
    metadata.encoded_height = kHeight;
    metadata.orientation = COLMAPKIT_FRAME_ORIENTATION_V1_UP;
    metadata.camera_model = COLMAPKIT_CAMERA_MODEL_V2_PINHOLE;
    metadata.num_camera_params = 4;
    metadata.camera_params[0] = 280.0;
    metadata.camera_params[1] = 280.0;
    metadata.camera_params[2] = kWidth / 2.0;
    metadata.camera_params[3] = kHeight / 2.0;
  }

  ColmapKitFrameFeatureInputV1 Input(
      const std::filesystem::path& output) const {
    output_storage = output.string();
    ColmapKitFrameFeatureInputV1 input{};
    input.struct_size = sizeof(input);
    input.abi_version = COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1;
    input.stable_frame_id = 42;
    input.frame_revision = 7;
    input.encoded_image_bytes = encoded.data();
    input.encoded_image_size = encoded.size();
    input.expected_image_sha256 = image_sha256.c_str();
    input.metadata = metadata;
    input.output_artifact_path = output_storage.c_str();
    return input;
  }

  ColmapKitStatus Run(ColmapKitFrameFeatureExtractorV1* extractor,
                      ColmapKitFrameFeatureInputV1* input,
                      ColmapKitFrameFeatureResultV1* result) const {
    ColmapKitFrameFeatureJobV1* job = nullptr;
    auto error = MakeError();
    const ColmapKitStatus start =
        ColmapKitStartFrameFeatureExtractionV1(extractor, input, &job, &error);
    if (start != COLMAPKIT_STATUS_OK) return start;
    const ColmapKitStatus wait =
        ColmapKitWaitFrameFeatureExtractionV1(job, result);
    ColmapKitReleaseFrameFeatureJobV1(job);
    return wait;
  }

  std::filesystem::path directory;
  std::vector<uint8_t> encoded;
  std::string image_sha256;
  ColmapKitFrameMetadataV1 metadata{};
  mutable std::string output_storage;
};

ColmapKitFrameFeatureExtractorV1* CreateExtractor(
    const ColmapKitFrameFeatureExtractorConfigV1& config) {
  ColmapKitFrameFeatureExtractorV1* extractor = nullptr;
  auto error = MakeError();
  EXPECT_EQ(ColmapKitCreateFrameFeatureExtractorV1(&config, &extractor, &error),
            COLMAPKIT_STATUS_OK)
      << error.message;
  return extractor;
}

ColmapKitStatus Validate(ColmapKitFrameFeatureExtractorV1* extractor,
                         const std::filesystem::path& path,
                         const FrameFixture& fixture,
                         const std::string& metadata_sha256,
                         ColmapKitFrameFeatureResultV1* result) {
  const std::string path_storage = path.string();
  ColmapKitFrameFeatureArtifactExpectationV1 expectation{};
  expectation.struct_size = sizeof(expectation);
  expectation.abi_version = COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1;
  expectation.stable_frame_id = 42;
  expectation.frame_revision = 7;
  expectation.expected_image_sha256 = fixture.image_sha256.c_str();
  expectation.expected_metadata_sha256 = metadata_sha256.c_str();
  expectation.artifact_path = path_storage.c_str();
  return ColmapKitValidateFrameFeatureArtifactV1(
      extractor, &expectation, result);
}

TEST(FrameFeatureExtractionV1, PreservesLegacySizesAndRejectsInvalidABI) {
  EXPECT_EQ(internal::FrameFeatureSHA256("abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(
      offsetof(ColmapKitSparseReconstructionConfig, extraction_num_threads),
      136u);
  EXPECT_EQ(offsetof(ColmapKitSparseReconstructionResult,
                     sparse_reconstruction_abi_version),
            1088u);

  auto config = DefaultConfig();
  auto error = MakeError();
  ColmapKitFrameFeatureExtractorV1* extractor = nullptr;
  config.struct_size = sizeof(config) - 1;
  EXPECT_EQ(ColmapKitCreateFrameFeatureExtractorV1(&config, &extractor, &error),
            COLMAPKIT_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(extractor, nullptr);

  config = DefaultConfig();
  config.requested_backend = COLMAPKIT_FRAME_FEATURE_BACKEND_V1_METAL;
  GuardedFrameFeatureError guarded_error;
  guarded_error.guard.fill(0xa5);
  guarded_error.error.struct_size =
      sizeof(guarded_error.error) + guarded_error.guard.size();
  guarded_error.error.abi_version = COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1;
  EXPECT_EQ(ColmapKitCreateFrameFeatureExtractorV1(
                &config, &extractor, &guarded_error.error),
            COLMAPKIT_STATUS_UNSUPPORTED);
  EXPECT_TRUE(std::all_of(guarded_error.guard.begin(),
                          guarded_error.guard.end(),
                          [](const uint8_t value) { return value == 0xa5; }));

  config = DefaultConfig();
  config.worker_count = 2;
  error = MakeError();
  EXPECT_EQ(ColmapKitCreateFrameFeatureExtractorV1(&config, &extractor, &error),
            COLMAPKIT_STATUS_INVALID_ARGUMENT);
}

TEST(FrameFeatureExtractionV1, AppliesHardAlignedTerminalRowLimit) {
  FeatureKeypoints exact_keypoints;
  exact_keypoints.emplace_back(0.0f, 0.0f, 1.0f, 0.0f);
  exact_keypoints.emplace_back(1.0f, 1.0f, 4.0f, 0.0f);
  exact_keypoints.emplace_back(2.0f, 2.0f, 3.0f, 0.0f);
  exact_keypoints.emplace_back(3.0f, 3.0f, 2.0f, 0.0f);
  FeatureDescriptors exact_descriptors;
  exact_descriptors.type = FeatureExtractorType::SIFT;
  exact_descriptors.data.resize(4, 128);
  for (Eigen::Index row = 0; row < exact_descriptors.data.rows(); ++row) {
    exact_descriptors.data.row(row).setConstant(static_cast<uint8_t>(row));
  }

  auto bounded_keypoints = exact_keypoints;
  auto bounded_descriptors = exact_descriptors;
  internal::ApplyFrameFeatureTerminalRowLimitV1(
      4, &bounded_keypoints, &bounded_descriptors);
  EXPECT_EQ(bounded_keypoints.size(), exact_keypoints.size());
  EXPECT_TRUE(
      (bounded_descriptors.data.array() == exact_descriptors.data.array())
          .all());

  internal::ApplyFrameFeatureTerminalRowLimitV1(
      2, &bounded_keypoints, &bounded_descriptors);
  ASSERT_EQ(bounded_keypoints.size(), 2u);
  ASSERT_EQ(bounded_descriptors.data.rows(), 2);
  EXPECT_FLOAT_EQ(bounded_keypoints[0].ComputeScale(), 4.0f);
  EXPECT_FLOAT_EQ(bounded_keypoints[1].ComputeScale(), 3.0f);
  EXPECT_EQ(bounded_descriptors.data(0, 0), 1);
  EXPECT_EQ(bounded_descriptors.data(1, 0), 2);
}

TEST(FrameFeatureExtractionV1,
     CapsHighTextureOutputAndValidatesAtTerminalBoundary) {
  FrameFixture fixture;
  auto config = DefaultConfig();
  config.max_num_features = 64;

  Bitmap raw_bitmap;
  ASSERT_TRUE(raw_bitmap.Read(fixture.directory / "frame.jpg", /*as_rgb=*/false));
  raw_bitmap.Thumbnail(config.max_image_size);
  FeatureExtractionOptions raw_options(FeatureExtractorType::SIFT);
  raw_options.use_gpu = false;
  raw_options.num_threads = 1;
  raw_options.max_image_size = config.max_image_size;
  raw_options.sift->max_num_features = config.max_num_features;
  raw_options.sift->first_octave = config.first_octave;
  raw_options.sift->num_octaves = config.num_octaves;
  raw_options.sift->octave_resolution = config.octave_resolution;
  raw_options.sift->max_num_orientations = config.max_num_orientations;
  raw_options.sift->upright = config.upright != 0;
  raw_options.sift->peak_threshold = config.peak_threshold;
  raw_options.sift->edge_threshold = config.edge_threshold;
  auto raw_extractor = FeatureExtractor::Create(raw_options);
  ASSERT_NE(raw_extractor, nullptr);
  FeatureKeypoints raw_keypoints;
  FeatureDescriptors raw_descriptors;
  ASSERT_TRUE(raw_extractor->Extract(
      raw_bitmap, &raw_keypoints, &raw_descriptors));
  ASSERT_GT(raw_keypoints.size(), config.max_num_features);
  ASSERT_EQ(raw_descriptors.data.rows(), raw_keypoints.size());

  auto* extractor = CreateExtractor(config);
  ASSERT_NE(extractor, nullptr);

  const auto output1 = fixture.directory / "bounded-first.ckfeatures";
  auto input1 = fixture.Input(output1);
  auto result1 = MakeResult();
  ASSERT_EQ(fixture.Run(extractor, &input1, &result1), COLMAPKIT_STATUS_OK)
      << result1.message;
  EXPECT_EQ(result1.feature_count, config.max_num_features);
  EXPECT_EQ(result1.descriptor_bytes, config.max_num_features * 128u);

  auto validated = MakeResult();
  ASSERT_EQ(
      Validate(
          extractor, output1, fixture, result1.metadata_sha256, &validated),
      COLMAPKIT_STATUS_OK)
      << validated.message;
  EXPECT_EQ(validated.feature_count, config.max_num_features);
  EXPECT_EQ(validated.descriptor_bytes, config.max_num_features * 128u);

  const auto output2 = fixture.directory / "bounded-second.ckfeatures";
  auto input2 = fixture.Input(output2);
  auto result2 = MakeResult();
  ASSERT_EQ(fixture.Run(extractor, &input2, &result2), COLMAPKIT_STATUS_OK)
      << result2.message;
  EXPECT_EQ(ReadBytes(output1), ReadBytes(output2));
  ColmapKitReleaseFrameFeatureExtractorV1(extractor);
}

TEST(FrameFeatureExtractionV1, AdmissionUsesHardTerminalRowBound) {
  FrameFixture fixture;
  auto config = DefaultConfig();
  const uint64_t admitted =
      ExpectedAdmissionBytes(config, fixture.metadata, fixture.encoded.size());

  config.memory_admission_budget_bytes = admitted - 1;
  auto* rejected_extractor = CreateExtractor(config);
  ASSERT_NE(rejected_extractor, nullptr);
  const auto rejected_output =
      fixture.directory / "admission-rejected.ckfeatures";
  auto rejected_input = fixture.Input(rejected_output);
  ColmapKitFrameFeatureJobV1* rejected_job = nullptr;
  auto error = MakeError();
  EXPECT_EQ(ColmapKitStartFrameFeatureExtractionV1(
                rejected_extractor, &rejected_input, &rejected_job, &error),
            COLMAPKIT_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(rejected_job, nullptr);
  EXPECT_FALSE(std::filesystem::exists(rejected_output));
  ColmapKitReleaseFrameFeatureExtractorV1(rejected_extractor);

  config.memory_admission_budget_bytes = admitted;
  auto* admitted_extractor = CreateExtractor(config);
  ASSERT_NE(admitted_extractor, nullptr);
  const auto admitted_output = fixture.directory / "admission-exact.ckfeatures";
  auto admitted_input = fixture.Input(admitted_output);
  auto result = MakeResult();
  ASSERT_EQ(fixture.Run(admitted_extractor, &admitted_input, &result),
            COLMAPKIT_STATUS_OK)
      << result.message;
  EXPECT_EQ(result.admitted_memory_bytes, admitted);
  ColmapKitReleaseFrameFeatureExtractorV1(admitted_extractor);
}

TEST(FrameFeatureExtractionV1, ExtractsValidArtifactAndRepeatsByteIdentically) {
  FrameFixture fixture;
  const auto config = DefaultConfig();
  auto* extractor = CreateExtractor(config);
  ASSERT_NE(extractor, nullptr);

  const auto output1 = fixture.directory / "first.ckfeatures";
  auto input1 = fixture.Input(output1);
  GuardedFrameFeatureResult guarded_result;
  guarded_result.guard.fill(0x5a);
  guarded_result.result.struct_size =
      sizeof(guarded_result.result) + guarded_result.guard.size();
  guarded_result.result.abi_version = COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1;
  auto& result1 = guarded_result.result;
  ASSERT_EQ(fixture.Run(extractor, &input1, &result1), COLMAPKIT_STATUS_OK)
      << result1.message;
  EXPECT_TRUE(std::all_of(guarded_result.guard.begin(),
                          guarded_result.guard.end(),
                          [](const uint8_t value) { return value == 0x5a; }));
  EXPECT_EQ(result1.actual_backend, COLMAPKIT_FRAME_FEATURE_BACKEND_V1_CPU);
  EXPECT_EQ(result1.effective_worker_count, 1u);
  EXPECT_EQ(result1.no_fallback_satisfied, 1u);
  EXPECT_GT(result1.feature_count, 0u);
  EXPECT_EQ(result1.descriptor_bytes, result1.feature_count * 128);
  EXPECT_EQ(result1.encoded_width, kWidth);
  EXPECT_EQ(result1.encoded_height, kHeight);
  EXPECT_EQ(std::string(result1.image_sha256), fixture.image_sha256);

  auto validated = MakeResult();
  ASSERT_EQ(
      Validate(
          extractor, output1, fixture, result1.metadata_sha256, &validated),
      COLMAPKIT_STATUS_OK)
      << validated.message;
  EXPECT_STREQ(validated.artifact_sha256, result1.artifact_sha256);

  const auto output2 = fixture.directory / "second.ckfeatures";
  auto input2 = fixture.Input(output2);
  auto result2 = MakeResult();
  ASSERT_EQ(fixture.Run(extractor, &input2, &result2), COLMAPKIT_STATUS_OK)
      << result2.message;
  EXPECT_EQ(ReadBytes(output1), ReadBytes(output2));
  EXPECT_STREQ(result1.artifact_sha256, result2.artifact_sha256);
  EXPECT_STREQ(result1.payload_sha256, result2.payload_sha256);
  EXPECT_STREQ(result1.profile_sha256, result2.profile_sha256);

  ColmapKitReleaseFrameFeatureExtractorV1(extractor);
}

TEST(FrameFeatureExtractionV1, RejectsCorruptionVersionsAndConfigurationDrift) {
  FrameFixture fixture;
  auto config = DefaultConfig();
  auto* extractor = CreateExtractor(config);
  ASSERT_NE(extractor, nullptr);
  const auto output = fixture.directory / "source.ckfeatures";
  auto input = fixture.Input(output);
  auto extracted = MakeResult();
  ASSERT_EQ(fixture.Run(extractor, &input, &extracted), COLMAPKIT_STATUS_OK);
  const auto original = ReadBytes(output);

  auto truncated = original;
  truncated.pop_back();
  const auto truncated_path = fixture.directory / "truncated.ckfeatures";
  WriteBytes(truncated_path, truncated);
  auto result = MakeResult();
  EXPECT_EQ(Validate(extractor,
                     truncated_path,
                     fixture,
                     extracted.metadata_sha256,
                     &result),
            COLMAPKIT_STATUS_INVALID_ARGUMENT);

  auto unknown = original;
  unknown[8] = 2;
  const auto unknown_path = fixture.directory / "unknown.ckfeatures";
  WriteBytes(unknown_path, unknown);
  result = MakeResult();
  EXPECT_EQ(
      Validate(
          extractor, unknown_path, fixture, extracted.metadata_sha256, &result),
      COLMAPKIT_STATUS_INVALID_ARGUMENT);
  EXPECT_NE(std::string(result.message).find("Unknown"), std::string::npos);

  auto corrupt = original;
  corrupt[corrupt.size() / 2] ^= 0x80;
  const auto corrupt_path = fixture.directory / "corrupt.ckfeatures";
  WriteBytes(corrupt_path, corrupt);
  result = MakeResult();
  EXPECT_EQ(
      Validate(
          extractor, corrupt_path, fixture, extracted.metadata_sha256, &result),
      COLMAPKIT_STATUS_INVALID_ARGUMENT);

  auto over_bound = original;
  constexpr size_t kFeatureCountOffset = 196;
  constexpr size_t kDescriptorBytesOffset = 204;
  const uint64_t over_bound_count = config.max_num_features + 1;
  const uint64_t over_bound_descriptor_bytes = over_bound_count * 128;
  ASSERT_LE(kDescriptorBytesOffset + sizeof(uint64_t), over_bound.size());
  std::memcpy(over_bound.data() + kFeatureCountOffset,
              &over_bound_count,
              sizeof(over_bound_count));
  std::memcpy(over_bound.data() + kDescriptorBytesOffset,
              &over_bound_descriptor_bytes,
              sizeof(over_bound_descriptor_bytes));
  const auto over_bound_path = fixture.directory / "over-bound.ckfeatures";
  WriteBytes(over_bound_path, over_bound);
  result = MakeResult();
  EXPECT_EQ(Validate(extractor,
                     over_bound_path,
                     fixture,
                     extracted.metadata_sha256,
                     &result),
            COLMAPKIT_STATUS_INVALID_ARGUMENT);
  EXPECT_NE(std::string(result.message).find("counts"), std::string::npos);

  config.max_num_features += 1;
  auto* drifted_extractor = CreateExtractor(config);
  ASSERT_NE(drifted_extractor, nullptr);
  result = MakeResult();
  EXPECT_EQ(Validate(drifted_extractor,
                     output,
                     fixture,
                     extracted.metadata_sha256,
                     &result),
            COLMAPKIT_STATUS_INVALID_ARGUMENT);
  EXPECT_NE(std::string(result.message).find("configuration"),
            std::string::npos);

  result = MakeResult();
  EXPECT_EQ(Validate(extractor, output, fixture, std::string(64, '0'), &result),
            COLMAPKIT_STATUS_INVALID_ARGUMENT);

  ColmapKitReleaseFrameFeatureExtractorV1(drifted_extractor);
  ColmapKitReleaseFrameFeatureExtractorV1(extractor);
}

TEST(FrameFeatureExtractionV1, RejectsNonfiniteKeypoints) {
  FrameFixture fixture;
  auto* extractor = CreateExtractor(DefaultConfig());
  ASSERT_NE(extractor, nullptr);
  const auto output = fixture.directory / "source.ckfeatures";
  auto input = fixture.Input(output);
  auto extracted = MakeResult();
  ASSERT_EQ(fixture.Run(extractor, &input, &extracted), COLMAPKIT_STATUS_OK);

  auto bytes = ReadBytes(output);
  size_t offset = 212;
  for (int i = 0; i < 4; ++i) {
    ASSERT_LE(offset + sizeof(uint32_t), bytes.size());
    uint32_t length = 0;
    std::memcpy(&length, bytes.data() + offset, sizeof(length));
    offset += sizeof(length) + length;
  }
  ASSERT_LE(offset + sizeof(float), bytes.size());
  const float nan = std::numeric_limits<float>::quiet_NaN();
  std::memcpy(bytes.data() + offset, &nan, sizeof(nan));
  const auto nonfinite_path = fixture.directory / "nonfinite.ckfeatures";
  WriteBytes(nonfinite_path, bytes);

  auto result = MakeResult();
  EXPECT_EQ(Validate(extractor,
                     nonfinite_path,
                     fixture,
                     extracted.metadata_sha256,
                     &result),
            COLMAPKIT_STATUS_INVALID_ARGUMENT);
  EXPECT_NE(std::string(result.message).find("Non-finite"), std::string::npos);
  ColmapKitReleaseFrameFeatureExtractorV1(extractor);
}

struct BlockingProgress {
  std::mutex mutex;
  std::condition_variable condition;
  bool reached_extraction = false;
  bool release = false;
};

void BlockAtExtraction(const ColmapKitFrameFeatureProgressEventV1* event,
                       void* user_data) {
  if (event->stage != COLMAPKIT_FRAME_FEATURE_PROGRESS_V1_EXTRACTING) return;
  auto* progress = static_cast<BlockingProgress*>(user_data);
  std::unique_lock<std::mutex> lock(progress->mutex);
  progress->reached_extraction = true;
  progress->condition.notify_all();
  progress->condition.wait(lock, [&] { return progress->release; });
}

TEST(FrameFeatureExtractionV1, CancellationCleansUpAndContextStaysBounded) {
  FrameFixture fixture;
  auto* extractor = CreateExtractor(DefaultConfig());
  ASSERT_NE(extractor, nullptr);
  BlockingProgress progress;
  const auto output = fixture.directory / "cancelled.ckfeatures";
  auto input = fixture.Input(output);
  input.progress_callback = BlockAtExtraction;
  input.progress_user_data = &progress;
  ColmapKitFrameFeatureJobV1* job = nullptr;
  auto error = MakeError();
  ASSERT_EQ(
      ColmapKitStartFrameFeatureExtractionV1(extractor, &input, &job, &error),
      COLMAPKIT_STATUS_OK);

  {
    std::unique_lock<std::mutex> lock(progress.mutex);
    progress.condition.wait(lock, [&] { return progress.reached_extraction; });
  }
  auto second_input = fixture.Input(fixture.directory / "busy.ckfeatures");
  ColmapKitFrameFeatureJobV1* second_job = nullptr;
  error = MakeError();
  EXPECT_EQ(ColmapKitStartFrameFeatureExtractionV1(
                extractor, &second_input, &second_job, &error),
            COLMAPKIT_STATUS_RUNTIME_ERROR);
  EXPECT_EQ(second_job, nullptr);

  error = MakeError();
  EXPECT_EQ(ColmapKitCancelFrameFeatureExtractionV1(job, &error),
            COLMAPKIT_STATUS_OK);
  {
    std::lock_guard<std::mutex> lock(progress.mutex);
    progress.release = true;
  }
  progress.condition.notify_all();
  auto result = MakeResult();
  EXPECT_EQ(ColmapKitWaitFrameFeatureExtractionV1(job, &result),
            COLMAPKIT_STATUS_CANCELLED);
  EXPECT_FALSE(std::filesystem::exists(output));
  for (const auto& entry :
       std::filesystem::directory_iterator(fixture.directory)) {
    EXPECT_EQ(
        entry.path().filename().string().find("cancelled.ckfeatures.tmp."),
        std::string::npos);
  }
  ColmapKitReleaseFrameFeatureJobV1(job);
  ColmapKitReleaseFrameFeatureExtractorV1(extractor);
}

TEST(FrameFeatureExtractionV1, FailuresProduceNoOutputAndPreserveExistingFile) {
  FrameFixture uniform_fixture(false);
  auto* extractor = CreateExtractor(DefaultConfig());
  ASSERT_NE(extractor, nullptr);
  const auto zero_output = uniform_fixture.directory / "zero.ckfeatures";
  auto zero_input = uniform_fixture.Input(zero_output);
  auto result = MakeResult();
  EXPECT_EQ(uniform_fixture.Run(extractor, &zero_input, &result),
            COLMAPKIT_STATUS_RUNTIME_ERROR);
  EXPECT_FALSE(std::filesystem::exists(zero_output));

  FrameFixture mismatched_fixture;
  const auto mismatch_output =
      mismatched_fixture.directory / "mismatch.ckfeatures";
  auto mismatch_input = mismatched_fixture.Input(mismatch_output);
  const std::string wrong_sha(64, '0');
  mismatch_input.expected_image_sha256 = wrong_sha.c_str();
  result = MakeResult();
  EXPECT_EQ(mismatched_fixture.Run(extractor, &mismatch_input, &result),
            COLMAPKIT_STATUS_INVALID_ARGUMENT);
  EXPECT_FALSE(std::filesystem::exists(mismatch_output));

  auto corrupt_encoded = mismatched_fixture.encoded;
  std::fill(corrupt_encoded.begin(), corrupt_encoded.end(), 0);
  const auto corrupt_input_output =
      mismatched_fixture.directory / "corrupt-input.ckfeatures";
  auto corrupt_input = mismatched_fixture.Input(corrupt_input_output);
  const std::string corrupt_sha = HashBytes(corrupt_encoded);
  corrupt_input.encoded_image_bytes = corrupt_encoded.data();
  corrupt_input.encoded_image_size = corrupt_encoded.size();
  corrupt_input.expected_image_sha256 = corrupt_sha.c_str();
  result = MakeResult();
  EXPECT_EQ(mismatched_fixture.Run(extractor, &corrupt_input, &result),
            COLMAPKIT_STATUS_INVALID_ARGUMENT);
  EXPECT_FALSE(std::filesystem::exists(corrupt_input_output));

  auto bounded_config = DefaultConfig();
  bounded_config.memory_admission_budget_bytes = 1;
  auto* bounded_extractor = CreateExtractor(bounded_config);
  ASSERT_NE(bounded_extractor, nullptr);
  const auto bounded_output =
      mismatched_fixture.directory / "over-budget.ckfeatures";
  auto bounded_input = mismatched_fixture.Input(bounded_output);
  ColmapKitFrameFeatureJobV1* bounded_job = nullptr;
  auto bounded_error = MakeError();
  EXPECT_EQ(
      ColmapKitStartFrameFeatureExtractionV1(
          bounded_extractor, &bounded_input, &bounded_job, &bounded_error),
      COLMAPKIT_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(bounded_job, nullptr);
  EXPECT_FALSE(std::filesystem::exists(bounded_output));
  ColmapKitReleaseFrameFeatureExtractorV1(bounded_extractor);

  FrameFixture textured_fixture;
  const auto existing = textured_fixture.directory / "existing.ckfeatures";
  const std::vector<uint8_t> sentinel = {1, 2, 3, 4};
  WriteBytes(existing, sentinel);
  auto existing_input = textured_fixture.Input(existing);
  ColmapKitFrameFeatureJobV1* job = nullptr;
  auto error = MakeError();
  EXPECT_EQ(ColmapKitStartFrameFeatureExtractionV1(
                extractor, &existing_input, &job, &error),
            COLMAPKIT_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(job, nullptr);
  EXPECT_EQ(ReadBytes(existing), sentinel);

  ColmapKitReleaseFrameFeatureExtractorV1(extractor);
}

}  // namespace
}  // namespace colmap
