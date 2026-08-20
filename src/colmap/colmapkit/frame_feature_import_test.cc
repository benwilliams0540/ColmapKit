// Copyright (c), ETH Zurich and UNC Chapel Hill.
// All rights reserved.

#include "colmap/colmapkit/colmapkit.h"
#include "colmap/colmapkit/frame_feature_extraction_internal.h"
#include "colmap/scene/camera.h"
#include "colmap/scene/database.h"
#include "colmap/sensor/bitmap.h"
#include "colmap/util/testing.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace colmap {
namespace {

constexpr uint32_t kWidth = 320;
constexpr uint32_t kHeight = 240;

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

std::string HashFile(const std::filesystem::path& path) {
  const std::vector<uint8_t> bytes = ReadBytes(path);
  return internal::FrameFeatureSHA256(std::string_view(
      reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

ColmapKitFrameFeatureExtractorConfigV1 DefaultExtractorConfig() {
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
  config.normalization = COLMAPKIT_SIFT_NORMALIZATION_V1_L1_ROOT;
  config.peak_threshold = 0.006666666666666667;
  config.edge_threshold = 10.0;
  return config;
}

ColmapKitFrameFeatureErrorV1 MakeError() {
  ColmapKitFrameFeatureErrorV1 error{};
  error.struct_size = sizeof(error);
  error.abi_version = COLMAPKIT_FRAME_FEATURE_IMPORT_ABI_VERSION_V1;
  return error;
}

ColmapKitFrameFeatureResultV1 MakeExtractionResult() {
  ColmapKitFrameFeatureResultV1 result{};
  result.struct_size = sizeof(result);
  result.abi_version = COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1;
  return result;
}

ColmapKitFrameFeatureImportResultV1 MakeImportResult() {
  ColmapKitFrameFeatureImportResultV1 result{};
  result.struct_size = sizeof(result);
  result.abi_version = COLMAPKIT_FRAME_FEATURE_IMPORT_ABI_VERSION_V1;
  return result;
}

struct GuardedImportResult {
  ColmapKitFrameFeatureImportResultV1 result{};
  std::array<uint8_t, 32> guard{};
};

struct ExtractedFrame {
  uint64_t stable_frame_id = 0;
  uint64_t frame_revision = 0;
  std::string image_name;
  std::string image_path;
  std::string artifact_path;
  std::string image_sha256;
  std::string metadata_sha256;
  std::string artifact_sha256;
  uint64_t feature_count = 0;
};

class ImportFixture {
 public:
  ImportFixture()
      : directory(CreateTestDir()),
        extractor_config(DefaultExtractorConfig()) {}

  ExtractedFrame Extract(
      const uint64_t stable_frame_id,
      const uint64_t frame_revision,
      const int seed,
      const double focal_length = 280.0,
      const ColmapKitFrameFeatureExtractorConfigV1* config = nullptr) {
    const std::filesystem::path image_path =
        directory / ("frame-" + std::to_string(stable_frame_id) + ".jpg");
    const std::filesystem::path artifact_path =
        directory /
        ("frame-" + std::to_string(stable_frame_id) + ".ckfeatures");
    Bitmap bitmap(kWidth, kHeight, true);
    for (uint32_t y = 0; y < kHeight; ++y) {
      for (uint32_t x = 0; x < kWidth; ++x) {
        const uint8_t checker =
            ((x / (10 + seed) + y / (11 + seed)) % 2) ? 218 : 28;
        const uint8_t detail =
            static_cast<uint8_t>((x * (13 + seed) + y * 29) % 47);
        bitmap.SetPixel(
            x,
            y,
            BitmapColor<uint8_t>(
                checker,
                static_cast<uint8_t>((checker + detail) % 256),
                static_cast<uint8_t>((255 - checker + detail) % 256)));
      }
    }
    bitmap.SetJpegQuality(100);
    EXPECT_TRUE(bitmap.Write(image_path));
    const std::string image_sha256 = HashFile(image_path);

    ColmapKitFrameMetadataV1 metadata{};
    metadata.struct_size = sizeof(metadata);
    metadata.abi_version = COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1;
    metadata.image_format = COLMAPKIT_FRAME_IMAGE_FORMAT_V1_JPEG;
    metadata.encoded_width = kWidth;
    metadata.encoded_height = kHeight;
    metadata.orientation = COLMAPKIT_FRAME_ORIENTATION_V1_UP;
    metadata.camera_model = COLMAPKIT_CAMERA_MODEL_V2_PINHOLE;
    metadata.num_camera_params = 4;
    metadata.camera_params[0] = focal_length;
    metadata.camera_params[1] = focal_length;
    metadata.camera_params[2] = kWidth / 2.0;
    metadata.camera_params[3] = kHeight / 2.0;

    ColmapKitFrameFeatureExtractorV1* extractor = nullptr;
    auto error = MakeError();
    const auto& selected_config =
        config == nullptr ? extractor_config : *config;
    EXPECT_EQ(ColmapKitCreateFrameFeatureExtractorV1(
                  &selected_config, &extractor, &error),
              COLMAPKIT_STATUS_OK)
        << error.message;
    EXPECT_NE(extractor, nullptr);

    const std::vector<uint8_t> encoded = ReadBytes(image_path);
    const std::string artifact_storage = artifact_path.string();
    ColmapKitFrameFeatureInputV1 input{};
    input.struct_size = sizeof(input);
    input.abi_version = COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1;
    input.stable_frame_id = stable_frame_id;
    input.frame_revision = frame_revision;
    input.encoded_image_bytes = encoded.data();
    input.encoded_image_size = encoded.size();
    input.expected_image_sha256 = image_sha256.c_str();
    input.metadata = metadata;
    input.output_artifact_path = artifact_storage.c_str();

    ColmapKitFrameFeatureJobV1* job = nullptr;
    const ColmapKitStatus start =
        ColmapKitStartFrameFeatureExtractionV1(extractor, &input, &job, &error);
    EXPECT_EQ(start, COLMAPKIT_STATUS_OK) << error.message;
    if (start != COLMAPKIT_STATUS_OK) {
      ColmapKitReleaseFrameFeatureExtractorV1(extractor);
      throw std::runtime_error(error.message);
    }
    auto result = MakeExtractionResult();
    EXPECT_EQ(ColmapKitWaitFrameFeatureExtractionV1(job, &result),
              COLMAPKIT_STATUS_OK)
        << result.message;
    ColmapKitReleaseFrameFeatureJobV1(job);
    ColmapKitReleaseFrameFeatureExtractorV1(extractor);

    return ExtractedFrame{
        stable_frame_id,
        frame_revision,
        "capture/frame-" + std::to_string(stable_frame_id) + ".jpg",
        image_path.string(),
        artifact_path.string(),
        image_sha256,
        result.metadata_sha256,
        result.artifact_sha256,
        result.feature_count};
  }

  std::vector<ColmapKitFrameFeatureImportItemV1> Items(
      const std::vector<ExtractedFrame>& frames) const {
    std::vector<ColmapKitFrameFeatureImportItemV1> items;
    for (const ExtractedFrame& frame : frames) {
      ColmapKitFrameFeatureImportItemV1 item{};
      item.struct_size = sizeof(item);
      item.abi_version = COLMAPKIT_FRAME_FEATURE_IMPORT_ABI_VERSION_V1;
      item.stable_frame_id = frame.stable_frame_id;
      item.frame_revision = frame.frame_revision;
      item.image_name = frame.image_name.c_str();
      item.image_path = frame.image_path.c_str();
      item.artifact_path = frame.artifact_path.c_str();
      item.expected_image_sha256 = frame.image_sha256.c_str();
      item.expected_metadata_sha256 = frame.metadata_sha256.c_str();
      item.expected_artifact_sha256 = frame.artifact_sha256.c_str();
      items.push_back(item);
    }
    return items;
  }

  ColmapKitFrameFeatureImportConfigV1 Config(
      const std::vector<ColmapKitFrameFeatureImportItemV1>& items,
      const std::string& output,
      const uint32_t mode = COLMAPKIT_FRAME_FEATURE_IMPORT_MODE_V1_CREATE_NEW,
      const char* base_database = nullptr) const {
    ColmapKitFrameFeatureImportConfigV1 config{};
    config.struct_size = sizeof(config);
    config.abi_version = COLMAPKIT_FRAME_FEATURE_IMPORT_ABI_VERSION_V1;
    config.mode = mode;
    config.worker_count = 1;
    config.items = items.data();
    config.num_items = items.size();
    config.extractor_config = extractor_config;
    config.max_total_artifact_bytes = 64ULL * 1024ULL * 1024ULL;
    config.max_total_image_bytes = 64ULL * 1024ULL * 1024ULL;
    config.max_total_features = 100000;
    config.max_base_database_bytes = 64ULL * 1024ULL * 1024ULL;
    config.output_bundle_path = output.c_str();
    config.base_database_path = base_database;
    return config;
  }

  ColmapKitStatus Run(ColmapKitFrameFeatureImportConfigV1* config,
                      ColmapKitFrameFeatureImportResultV1* result) const {
    ColmapKitFrameFeatureImportJobV1* job = nullptr;
    auto error = MakeError();
    const ColmapKitStatus start =
        ColmapKitStartFrameFeatureImportV1(config, &job, &error);
    if (start != COLMAPKIT_STATUS_OK) return start;
    const ColmapKitStatus wait = ColmapKitWaitFrameFeatureImportV1(job, result);
    ColmapKitReleaseFrameFeatureImportJobV1(job);
    return wait;
  }

  std::filesystem::path directory;
  ColmapKitFrameFeatureExtractorConfigV1 extractor_config;
};

TEST(FrameFeatureImportV1, PreservesLegacyABIAndRejectsInvalidLayouts) {
  EXPECT_EQ(
      offsetof(ColmapKitSparseReconstructionConfig, extraction_num_threads),
      136u);
  EXPECT_EQ(offsetof(ColmapKitSparseReconstructionResult,
                     sparse_reconstruction_abi_version),
            1088u);

  ImportFixture fixture;
  const std::vector<ExtractedFrame> frames = {fixture.Extract(10, 1, 1)};
  auto items = fixture.Items(frames);
  const std::string output = (fixture.directory / "invalid.ckseal").string();
  auto config = fixture.Config(items, output);
  config.struct_size -= 1;
  ColmapKitFrameFeatureImportJobV1* job = nullptr;
  auto error = MakeError();
  EXPECT_EQ(ColmapKitStartFrameFeatureImportV1(&config, &job, &error),
            COLMAPKIT_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(job, nullptr);
  EXPECT_FALSE(std::filesystem::exists(output));

  config = fixture.Config(items, output);
  items[0].struct_size -= 1;
  EXPECT_EQ(ColmapKitStartFrameFeatureImportV1(&config, &job, &error),
            COLMAPKIT_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(job, nullptr);
  EXPECT_FALSE(std::filesystem::exists(output));
}

TEST(FrameFeatureImportV1, CommitsOrderedDatabaseAndReceiptRepeatably) {
  ImportFixture fixture;
  const std::vector<ExtractedFrame> frames = {
      fixture.Extract(30, 1, 1),
      fixture.Extract(10, 4, 2),
      fixture.Extract(20, 2, 3, 300.0),
  };
  auto items = fixture.Items(frames);
  const std::string output1 = (fixture.directory / "first.ckseal").string();
  auto config1 = fixture.Config(items, output1);
  GuardedImportResult guarded_result;
  guarded_result.guard.fill(0x5a);
  guarded_result.result.struct_size =
      sizeof(guarded_result.result) + guarded_result.guard.size();
  guarded_result.result.abi_version =
      COLMAPKIT_FRAME_FEATURE_IMPORT_ABI_VERSION_V1;
  auto& result1 = guarded_result.result;
  ASSERT_EQ(fixture.Run(&config1, &result1), COLMAPKIT_STATUS_OK)
      << result1.message;
  EXPECT_TRUE(std::all_of(guarded_result.guard.begin(),
                          guarded_result.guard.end(),
                          [](const uint8_t value) { return value == 0x5a; }));
  EXPECT_EQ(result1.no_fallback_satisfied, 1u);
  EXPECT_EQ(result1.effective_worker_count, 1u);
  EXPECT_EQ(result1.imported_items, 3u);
  EXPECT_EQ(result1.imported_cameras, 2u);
  EXPECT_EQ(result1.imported_keypoints,
            frames[0].feature_count + frames[1].feature_count +
                frames[2].feature_count);

  auto database =
      Database::Open(std::filesystem::path(output1) / "database.db");
  ASSERT_EQ(database->NumImages(), 3u);
  ASSERT_EQ(database->NumCameras(), 2u);
  const std::vector<Image> images = database->ReadAllImages();
  ASSERT_EQ(images.size(), 3u);
  EXPECT_EQ(images[0].ImageId(), 1u);
  EXPECT_EQ(images[0].Name(), frames[0].image_name);
  EXPECT_EQ(images[0].CameraId(), 1u);
  EXPECT_EQ(images[1].ImageId(), 2u);
  EXPECT_EQ(images[1].Name(), frames[1].image_name);
  EXPECT_EQ(images[1].CameraId(), 1u);
  EXPECT_EQ(images[2].ImageId(), 3u);
  EXPECT_EQ(images[2].Name(), frames[2].image_name);
  EXPECT_EQ(images[2].CameraId(), 2u);
  for (size_t index = 0; index < frames.size(); ++index) {
    EXPECT_EQ(database->ReadKeypoints(index + 1).size(),
              frames[index].feature_count);
    const FeatureDescriptors descriptors = database->ReadDescriptors(index + 1);
    EXPECT_EQ(descriptors.type, FeatureExtractorType::SIFT);
    EXPECT_EQ(descriptors.data.rows(), frames[index].feature_count);
    EXPECT_EQ(descriptors.data.cols(), 128);
  }
  database->Close();
  database.reset();

  const std::string output2 = (fixture.directory / "second.ckseal").string();
  auto config2 = fixture.Config(items, output2);
  auto result2 = MakeImportResult();
  ASSERT_EQ(fixture.Run(&config2, &result2), COLMAPKIT_STATUS_OK)
      << result2.message;
  EXPECT_EQ(ReadBytes(std::filesystem::path(output1) / "import-receipt.json"),
            ReadBytes(std::filesystem::path(output2) / "import-receipt.json"));
  EXPECT_STREQ(result1.sealed_set_sha256, result2.sealed_set_sha256);
  EXPECT_STREQ(result1.database_sha256, result2.database_sha256);
  EXPECT_STREQ(result1.receipt_sha256, result2.receipt_sha256);
}

struct BlockingImportProgress {
  std::mutex mutex;
  std::condition_variable condition;
  bool reached_import = false;
  bool release = false;
};

void BlockAfterFirstImport(
    const ColmapKitFrameFeatureImportProgressEventV1* event, void* user_data) {
  if (event->stage != COLMAPKIT_FRAME_FEATURE_IMPORT_PROGRESS_V1_IMPORTING ||
      event->current != 1) {
    return;
  }
  auto* progress = static_cast<BlockingImportProgress*>(user_data);
  std::unique_lock<std::mutex> lock(progress->mutex);
  progress->reached_import = true;
  progress->condition.notify_all();
  progress->condition.wait(lock, [&] { return progress->release; });
}

TEST(FrameFeatureImportV1, CancellationRollsBackVisibleOutputAndStaging) {
  ImportFixture fixture;
  const std::vector<ExtractedFrame> frames = {fixture.Extract(1, 1, 1),
                                              fixture.Extract(2, 1, 2)};
  auto items = fixture.Items(frames);
  const std::string output = (fixture.directory / "cancelled.ckseal").string();
  auto config = fixture.Config(items, output);
  BlockingImportProgress progress;
  config.progress_callback = BlockAfterFirstImport;
  config.progress_user_data = &progress;

  ColmapKitFrameFeatureImportJobV1* job = nullptr;
  auto error = MakeError();
  ASSERT_EQ(ColmapKitStartFrameFeatureImportV1(&config, &job, &error),
            COLMAPKIT_STATUS_OK)
      << error.message;
  {
    std::unique_lock<std::mutex> lock(progress.mutex);
    progress.condition.wait(lock, [&] { return progress.reached_import; });
  }
  EXPECT_EQ(ColmapKitCancelFrameFeatureImportV1(job, &error),
            COLMAPKIT_STATUS_OK);
  {
    std::lock_guard<std::mutex> lock(progress.mutex);
    progress.release = true;
  }
  progress.condition.notify_all();
  auto result = MakeImportResult();
  EXPECT_EQ(ColmapKitWaitFrameFeatureImportV1(job, &result),
            COLMAPKIT_STATUS_CANCELLED);
  ColmapKitReleaseFrameFeatureImportJobV1(job);
  EXPECT_FALSE(std::filesystem::exists(output));
  for (const auto& entry :
       std::filesystem::directory_iterator(fixture.directory)) {
    EXPECT_EQ(entry.path().filename().string().find("cancelled.ckseal.tmp."),
              std::string::npos);
  }
}

TEST(FrameFeatureImportV1, RejectsDuplicateMixedStaleAndPreexistingInputs) {
  ImportFixture fixture;
  std::vector<ExtractedFrame> frames = {fixture.Extract(1, 1, 1),
                                        fixture.Extract(2, 1, 2)};
  auto items = fixture.Items(frames);
  items[1].image_name = items[0].image_name;
  const std::string duplicate_output =
      (fixture.directory / "duplicate.ckseal").string();
  auto config = fixture.Config(items, duplicate_output);
  ColmapKitFrameFeatureImportJobV1* job = nullptr;
  auto error = MakeError();
  EXPECT_EQ(ColmapKitStartFrameFeatureImportV1(&config, &job, &error),
            COLMAPKIT_STATUS_INVALID_ARGUMENT);
  EXPECT_FALSE(std::filesystem::exists(duplicate_output));

  auto drifted = fixture.extractor_config;
  drifted.max_num_features += 1;
  frames[1] = fixture.Extract(22, 1, 2, 280.0, &drifted);
  items = fixture.Items(frames);
  const std::string mixed_output =
      (fixture.directory / "mixed.ckseal").string();
  config = fixture.Config(items, mixed_output);
  auto result = MakeImportResult();
  EXPECT_EQ(fixture.Run(&config, &result), COLMAPKIT_STATUS_INVALID_ARGUMENT);
  EXPECT_FALSE(std::filesystem::exists(mixed_output));

  frames = {fixture.Extract(3, 1, 3)};
  items = fixture.Items(frames);
  WriteBytes(frames[0].image_path, {1, 2, 3, 4});
  const std::string stale_output =
      (fixture.directory / "stale.ckseal").string();
  config = fixture.Config(items, stale_output);
  result = MakeImportResult();
  EXPECT_EQ(fixture.Run(&config, &result), COLMAPKIT_STATUS_INVALID_ARGUMENT);
  EXPECT_FALSE(std::filesystem::exists(stale_output));

  frames = {fixture.Extract(33, 1, 3)};
  items = fixture.Items(frames);
  std::vector<uint8_t> corrupt_artifact = ReadBytes(frames[0].artifact_path);
  corrupt_artifact[corrupt_artifact.size() / 2] ^= 0x40;
  WriteBytes(frames[0].artifact_path, corrupt_artifact);
  const std::string corrupt_output =
      (fixture.directory / "corrupt.ckseal").string();
  config = fixture.Config(items, corrupt_output);
  result = MakeImportResult();
  EXPECT_EQ(fixture.Run(&config, &result), COLMAPKIT_STATUS_INVALID_ARGUMENT);
  EXPECT_FALSE(std::filesystem::exists(corrupt_output));

  frames = {fixture.Extract(4, 1, 4)};
  items = fixture.Items(frames);
  const std::filesystem::path existing = fixture.directory / "existing.ckseal";
  std::filesystem::create_directory(existing);
  WriteBytes(existing / "sentinel", {9, 8, 7});
  const std::string existing_output = existing.string();
  config = fixture.Config(items, existing_output);
  EXPECT_EQ(ColmapKitStartFrameFeatureImportV1(&config, &job, &error),
            COLMAPKIT_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(ReadBytes(existing / "sentinel"), (std::vector<uint8_t>{9, 8, 7}));
}

struct PublishingCollision {
  std::filesystem::path output;
};

void CreateCompetingOutput(
    const ColmapKitFrameFeatureImportProgressEventV1* event, void* user_data) {
  if (event->stage != COLMAPKIT_FRAME_FEATURE_IMPORT_PROGRESS_V1_PUBLISHING) {
    return;
  }
  auto* collision = static_cast<PublishingCollision*>(user_data);
  std::filesystem::create_directory(collision->output);
  WriteBytes(collision->output / "sentinel", {4, 3, 2, 1});
}

TEST(FrameFeatureImportV1, AtomicPublishNeverOverwritesRacingOutput) {
  ImportFixture fixture;
  const std::vector<ExtractedFrame> frames = {fixture.Extract(1, 1, 1)};
  auto items = fixture.Items(frames);
  PublishingCollision collision{fixture.directory / "collision.ckseal"};
  const std::string output = collision.output.string();
  auto config = fixture.Config(items, output);
  config.progress_callback = CreateCompetingOutput;
  config.progress_user_data = &collision;
  auto result = MakeImportResult();
  EXPECT_EQ(fixture.Run(&config, &result), COLMAPKIT_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(ReadBytes(collision.output / "sentinel"),
            (std::vector<uint8_t>{4, 3, 2, 1}));
  EXPECT_FALSE(std::filesystem::exists(collision.output / "database.db"));
  for (const auto& entry :
       std::filesystem::directory_iterator(fixture.directory)) {
    EXPECT_EQ(entry.path().filename().string().find("collision.ckseal.tmp."),
              std::string::npos);
  }
}

TEST(FrameFeatureImportV1, CopiesOnlyEmptyClosedBaseDatabase) {
  ImportFixture fixture;
  const std::vector<ExtractedFrame> frames = {fixture.Extract(1, 1, 1)};
  auto items = fixture.Items(frames);
  const std::filesystem::path empty_base = fixture.directory / "empty.db";
  {
    auto database = Database::Open(empty_base);
    database->Close();
  }
  const std::string empty_base_hash = HashFile(empty_base);
  const std::string copied_output =
      (fixture.directory / "copied.ckseal").string();
  const std::string empty_base_storage = empty_base.string();
  auto config =
      fixture.Config(items,
                     copied_output,
                     COLMAPKIT_FRAME_FEATURE_IMPORT_MODE_V1_COPY_EMPTY_BASE,
                     empty_base_storage.c_str());
  auto result = MakeImportResult();
  ASSERT_EQ(fixture.Run(&config, &result), COLMAPKIT_STATUS_OK)
      << result.message;
  EXPECT_EQ(HashFile(empty_base), empty_base_hash);

  const std::filesystem::path nonempty_base = fixture.directory / "nonempty.db";
  {
    auto database = Database::Open(nonempty_base);
    Camera camera = Camera::CreateFromModelId(
        kInvalidCameraId, CameraModelId::kPinhole, 280.0, kWidth, kHeight);
    database->WriteCamera(camera);
    database->Close();
  }
  const std::string nonempty_hash = HashFile(nonempty_base);
  const std::string rejected_output =
      (fixture.directory / "rejected.ckseal").string();
  const std::string nonempty_storage = nonempty_base.string();
  config =
      fixture.Config(items,
                     rejected_output,
                     COLMAPKIT_FRAME_FEATURE_IMPORT_MODE_V1_COPY_EMPTY_BASE,
                     nonempty_storage.c_str());
  result = MakeImportResult();
  EXPECT_EQ(fixture.Run(&config, &result), COLMAPKIT_STATUS_INVALID_ARGUMENT);
  EXPECT_FALSE(std::filesystem::exists(rejected_output));
  EXPECT_EQ(HashFile(nonempty_base), nonempty_hash);
}

}  // namespace
}  // namespace colmap
