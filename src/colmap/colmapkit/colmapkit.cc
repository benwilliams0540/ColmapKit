// Copyright (c), ETH Zurich and UNC Chapel Hill.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//
//     * Neither the name of ETH Zurich and UNC Chapel Hill nor the names of
//       its contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "colmap/colmapkit/colmapkit.h"

#include "colmap/controllers/feature_extraction.h"
#include "colmap/controllers/feature_matching.h"
#include "colmap/controllers/image_reader.h"
#include "colmap/controllers/incremental_pipeline.h"
#include "colmap/controllers/pairing.h"
#include "colmap/exe/sfm.h"
#include "colmap/feature/sift.h"
#include "colmap/scene/reconstruction_manager.h"
#include "colmap/sensor/models.h"
#include "colmap/util/file.h"
#include "colmap/util/logging.h"
#include "colmap/util/oiio_utils.h"
#include "colmap/util/version.h"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

using colmap::CreateDirIfNotExists;
using colmap::ExistsDir;
using colmap::ExistsFile;

constexpr size_t kCurrentResultSize =
    sizeof(ColmapKitSparseReconstructionResult);
constexpr size_t kMinimumConfigSize =
    offsetof(ColmapKitSparseReconstructionConfig, progress_user_data) +
    sizeof(void*);

std::string CStringOrEmpty(const char* value) {
  return value == nullptr ? std::string() : std::string(value);
}

std::filesystem::path PathFromCString(const char* value) {
  return std::filesystem::path(CStringOrEmpty(value));
}

bool ConfigHasField(const ColmapKitSparseReconstructionConfig& config,
                    const size_t field_end) {
  return config.struct_size == 0 || config.struct_size >= field_end;
}

#define COLMAPKIT_CONFIG_HAS_FIELD(config, field)                       \
  ConfigHasField(config,                                                \
                 offsetof(ColmapKitSparseReconstructionConfig, field) + \
                     sizeof(config.field))

class ColmapKitCancelledError : public std::runtime_error {
 public:
  ColmapKitCancelledError()
      : std::runtime_error("Sparse reconstruction cancelled.") {}
};

class ColmapKitCancellationContext {
 public:
  void RequestCancel() {
    cancellation_requested_.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_thread_ != nullptr) {
      active_thread_->Stop();
    }
  }

  bool IsCancellationRequested() const {
    return cancellation_requested_.load(std::memory_order_acquire);
  }

  void SetActiveThread(colmap::Thread* thread) {
    std::lock_guard<std::mutex> lock(mutex_);
    active_thread_ = thread;
    if (active_thread_ != nullptr && IsCancellationRequested()) {
      active_thread_->Stop();
    }
  }

  void ClearActiveThread(colmap::Thread* thread) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_thread_ == thread) {
      active_thread_ = nullptr;
    }
  }

 private:
  std::atomic<bool> cancellation_requested_{false};
  mutable std::mutex mutex_;
  colmap::Thread* active_thread_ = nullptr;
};

class ScopedActiveThread {
 public:
  ScopedActiveThread(ColmapKitCancellationContext* cancellation,
                     colmap::Thread* thread)
      : cancellation_(cancellation), thread_(thread) {
    if (cancellation_ != nullptr) {
      cancellation_->SetActiveThread(thread_);
    }
  }

  ~ScopedActiveThread() {
    if (cancellation_ != nullptr) {
      cancellation_->ClearActiveThread(thread_);
    }
  }

 private:
  ColmapKitCancellationContext* cancellation_;
  colmap::Thread* thread_;
};

void ThrowIfCancelled(ColmapKitCancellationContext* cancellation) {
  if (cancellation != nullptr && cancellation->IsCancellationRequested()) {
    throw ColmapKitCancelledError();
  }
}

struct OwnedSparseReconstructionConfig {
  ColmapKitSparseReconstructionConfig config = {};

  std::string database_path;
  std::string image_path;
  std::string output_path;
  std::string sparse_text_output_path;
  std::string image_list_path;
  std::string camera_model;
  std::string camera_params;

  bool has_sparse_text_output_path = false;
  bool has_image_list_path = false;
  bool has_camera_model = false;
  bool has_camera_params = false;

  static OwnedSparseReconstructionConfig CopyFrom(
      const ColmapKitSparseReconstructionConfig& source) {
    OwnedSparseReconstructionConfig owned;
    owned.config = source;
    owned.database_path = CStringOrEmpty(source.database_path);
    owned.image_path = CStringOrEmpty(source.image_path);
    owned.output_path = CStringOrEmpty(source.output_path);
    owned.sparse_text_output_path =
        CStringOrEmpty(source.sparse_text_output_path);
    owned.image_list_path = CStringOrEmpty(source.image_list_path);
    owned.camera_model = CStringOrEmpty(source.camera_model);
    owned.camera_params = CStringOrEmpty(source.camera_params);
    owned.has_sparse_text_output_path =
        source.sparse_text_output_path != nullptr;
    owned.has_image_list_path = source.image_list_path != nullptr;
    owned.has_camera_model = source.camera_model != nullptr;
    owned.has_camera_params = source.camera_params != nullptr;
    owned.RefreshPointers();
    return owned;
  }

  void RefreshPointers() {
    config.database_path = database_path.c_str();
    config.image_path = image_path.c_str();
    config.output_path = output_path.c_str();
    config.sparse_text_output_path =
        has_sparse_text_output_path ? sparse_text_output_path.c_str() : nullptr;
    config.image_list_path =
        has_image_list_path ? image_list_path.c_str() : nullptr;
    config.camera_model = has_camera_model ? camera_model.c_str() : nullptr;
    config.camera_params = has_camera_params ? camera_params.c_str() : nullptr;
  }
};

void CopyMessage(const std::string& message,
                 ColmapKitSparseReconstructionResult* result) {
  if (result == nullptr) {
    return;
  }
  std::strncpy(result->message, message.c_str(), COLMAPKIT_MESSAGE_CAPACITY);
  result->message[COLMAPKIT_MESSAGE_CAPACITY - 1] = '\0';
}

void ResetResult(ColmapKitSparseReconstructionResult* result) {
  if (result == nullptr) {
    return;
  }
  std::memset(result, 0, sizeof(*result));
  result->struct_size = kCurrentResultSize;
}

void SetFailure(ColmapKitStatus status,
                const std::string& message,
                ColmapKitSparseReconstructionResult* result) {
  if (result == nullptr) {
    return;
  }
  result->status = status;
  CopyMessage(message, result);
}

void SetCancelled(ColmapKitSparseReconstructionResult* result) {
  SetFailure(
      COLMAPKIT_STATUS_CANCELLED, "Sparse reconstruction cancelled.", result);
}

void EmitProgress(const ColmapKitSparseReconstructionConfig& config,
                  ColmapKitProgressStage stage,
                  const std::string& message,
                  const std::string& detail = {},
                  double fraction = -1.0,
                  size_t current = 0,
                  size_t total = 0) {
  if (config.progress_callback == nullptr) {
    return;
  }

  ColmapKitProgressEvent event = {};
  event.struct_size = sizeof(event);
  event.stage = stage;
  event.fraction = fraction;
  event.current = current;
  event.total = total;
  event.message = message.c_str();
  event.detail = detail.empty() ? nullptr : detail.c_str();
  config.progress_callback(&event, config.progress_user_data);
}

void ValidateConfig(const ColmapKitSparseReconstructionConfig& config) {
  if (config.struct_size != 0 && config.struct_size < kMinimumConfigSize) {
    throw std::invalid_argument("Unsupported ColmapKit config struct size.");
  }
  if (CStringOrEmpty(config.database_path).empty()) {
    throw std::invalid_argument("database_path is required.");
  }
  if (CStringOrEmpty(config.image_path).empty()) {
    throw std::invalid_argument("image_path is required.");
  }
  if (CStringOrEmpty(config.output_path).empty()) {
    throw std::invalid_argument("output_path is required.");
  }
  if (!ExistsDir(PathFromCString(config.image_path))) {
    throw std::invalid_argument("image_path must be an existing directory.");
  }
  if (config.image_list_path != nullptr &&
      !CStringOrEmpty(config.image_list_path).empty() &&
      !ExistsFile(PathFromCString(config.image_list_path))) {
    throw std::invalid_argument("image_list_path must be an existing file.");
  }
}

colmap::ImageReaderOptions MakeReaderOptions(
    const ColmapKitSparseReconstructionConfig& config,
    const colmap::FeatureExtractionOptions& extraction_options) {
  colmap::ImageReaderOptions reader_options;
  reader_options.image_path = PathFromCString(config.image_path);
  reader_options.camera_model = CStringOrEmpty(config.camera_model).empty()
                                    ? "SIMPLE_RADIAL"
                                    : CStringOrEmpty(config.camera_model);
  reader_options.camera_params = CStringOrEmpty(config.camera_params);
  reader_options.single_camera = config.single_camera != 0;
  reader_options.as_rgb = extraction_options.RequiresRGB();

  if (config.image_list_path != nullptr &&
      !CStringOrEmpty(config.image_list_path).empty()) {
    reader_options.image_names =
        colmap::ReadTextFileLines(PathFromCString(config.image_list_path));
  }

  if (!colmap::ExistsCameraModelWithName(reader_options.camera_model)) {
    throw std::invalid_argument("Camera model does not exist: " +
                                reader_options.camera_model);
  }
  if (!reader_options.Check()) {
    throw std::invalid_argument("Invalid image reader options.");
  }
  return reader_options;
}

colmap::FeatureExtractionOptions MakeExtractionOptions(
    const ColmapKitSparseReconstructionConfig& config) {
  colmap::FeatureExtractionOptions options;
  options.max_image_size =
      config.max_image_size <= 0 ? -1 : config.max_image_size;
  options.num_threads = config.num_threads == 0 ? -1 : config.num_threads;
  options.use_gpu = config.use_gpu != 0 || config.use_metal_sift != 0;
  options.sift->use_metal = config.use_metal_sift != 0;
  options.sift->estimate_affine_shape = config.estimate_affine_shape != 0;
  options.sift->domain_size_pooling = config.domain_size_pooling != 0;
  if (!options.Check()) {
    throw std::invalid_argument("Invalid feature extraction options.");
  }
  if (options.RequiresOpenGL()) {
    throw std::runtime_error(
        "The requested feature extraction options require OpenGL, which is not "
        "supported by the ColmapKit facade.");
  }
  return options;
}

colmap::FeatureMatchingOptions MakeMatchingOptions(
    const ColmapKitSparseReconstructionConfig& config) {
  colmap::FeatureMatchingOptions options(
      colmap::FeatureMatcherType::SIFT_BRUTEFORCE);
  options.num_threads = config.num_threads == 0 ? -1 : config.num_threads;
  options.use_gpu = config.use_gpu != 0 || config.use_metal_matching != 0;
  options.sift->use_metal = config.use_metal_matching != 0;
  if (!options.Check()) {
    throw std::invalid_argument("Invalid feature matching options.");
  }
  if (options.RequiresOpenGL()) {
    throw std::runtime_error(
        "The requested feature matching options require OpenGL, which is not "
        "supported by the ColmapKit facade.");
  }
  return options;
}

std::unique_ptr<colmap::Thread> MakeMatcher(
    const ColmapKitSparseReconstructionConfig& config,
    const colmap::FeatureMatchingOptions& matching_options) {
  colmap::TwoViewGeometryOptions geometry_options;
  const auto database_path = PathFromCString(config.database_path);

  switch (config.matcher) {
    case COLMAPKIT_MATCHER_EXHAUSTIVE: {
      colmap::ExhaustivePairingOptions pairing_options;
      if (!pairing_options.Check()) {
        throw std::invalid_argument("Invalid exhaustive matcher options.");
      }
      return colmap::CreateExhaustiveFeatureMatcher(
          pairing_options, matching_options, geometry_options, database_path);
    }
    case COLMAPKIT_MATCHER_SPATIAL: {
      colmap::SpatialPairingOptions pairing_options;
      pairing_options.num_threads =
          config.num_threads == 0 ? -1 : config.num_threads;
      if (!pairing_options.Check()) {
        throw std::invalid_argument("Invalid spatial matcher options.");
      }
      return colmap::CreateSpatialFeatureMatcher(
          pairing_options, matching_options, geometry_options, database_path);
    }
    case COLMAPKIT_MATCHER_SEQUENTIAL:
    default: {
      colmap::SequentialPairingOptions pairing_options;
      pairing_options.overlap =
          config.sequential_overlap <= 0 ? 10 : config.sequential_overlap;
      pairing_options.quadratic_overlap = true;
      pairing_options.loop_detection = false;
      pairing_options.num_threads =
          config.num_threads == 0 ? -1 : config.num_threads;
      if (!pairing_options.Check()) {
        throw std::invalid_argument("Invalid sequential matcher options.");
      }
      return colmap::CreateSequentialFeatureMatcher(
          pairing_options, matching_options, geometry_options, database_path);
    }
  }
}

std::shared_ptr<colmap::IncrementalPipelineOptions> MakeMapperOptions(
    const ColmapKitSparseReconstructionConfig& config) {
  auto options = std::make_shared<colmap::IncrementalPipelineOptions>();
  options->num_threads = config.num_threads == 0 ? -1 : config.num_threads;
  options->min_num_matches = config.mapper_min_num_matches <= 0
                                 ? options->min_num_matches
                                 : config.mapper_min_num_matches;
  options->min_model_size = config.mapper_min_model_size <= 0
                                ? options->min_model_size
                                : config.mapper_min_model_size;
  if (COLMAPKIT_CONFIG_HAS_FIELD(config, mapper_random_seed)) {
    options->random_seed = config.mapper_random_seed;
  }
  options->image_path = PathFromCString(config.image_path);
  if (config.image_list_path != nullptr &&
      !CStringOrEmpty(config.image_list_path).empty()) {
    options->image_names =
        colmap::ReadTextFileLines(PathFromCString(config.image_list_path));
  }
  if (!options->Check()) {
    throw std::invalid_argument("Invalid mapper options.");
  }
  return options;
}

void RunThread(colmap::Thread* thread,
               ColmapKitCancellationContext* cancellation) {
  ThrowIfCancelled(cancellation);
  ScopedActiveThread active_thread(cancellation, thread);
  thread->Start();
  if (cancellation != nullptr && cancellation->IsCancellationRequested()) {
    thread->Stop();
  }
  thread->Wait();
  ThrowIfCancelled(cancellation);
}

void CreateParentDirIfNeeded(const std::filesystem::path& path) {
  const auto parent = path.parent_path();
  if (!parent.empty()) {
    CreateDirIfNotExists(parent, true);
  }
}

void WriteSparseText(const colmap::ReconstructionManager& manager,
                     const std::filesystem::path& sparse_text_path) {
  CreateDirIfNotExists(sparse_text_path, true);
  for (size_t i = 0; i < manager.Size(); ++i) {
    const auto component_path = sparse_text_path / std::to_string(i);
    CreateDirIfNotExists(component_path, true);
    manager.Get(i)->WriteText(component_path);
  }
}

void FillResultMetrics(const colmap::ReconstructionManager& manager,
                       ColmapKitSparseReconstructionResult* result) {
  if (result == nullptr) {
    return;
  }

  result->num_models = manager.Size();
  size_t largest_model_index = 0;
  size_t largest_num_reg_images = 0;
  size_t largest_num_points = 0;

  for (size_t i = 0; i < manager.Size(); ++i) {
    const auto& reconstruction = *manager.Get(i);
    const size_t num_reg_images = reconstruction.NumRegImages();
    const size_t num_points = reconstruction.NumPoints3D();
    if (num_reg_images > largest_num_reg_images ||
        (num_reg_images == largest_num_reg_images &&
         num_points > largest_num_points)) {
      largest_model_index = i;
      largest_num_reg_images = num_reg_images;
      largest_num_points = num_points;
    }
  }

  result->largest_model_index = largest_model_index;
  if (manager.Size() == 0) {
    return;
  }

  const auto& reconstruction = *manager.Get(largest_model_index);
  result->registered_images = reconstruction.NumRegImages();
  result->sparse_points = reconstruction.NumPoints3D();
  result->observations = reconstruction.ComputeNumObservations();
  result->mean_reprojection_error =
      reconstruction.ComputeMeanReprojectionError();
}

std::string SummaryMessage(const ColmapKitSparseReconstructionResult& result) {
  std::ostringstream stream;
  stream << "Sparse reconstruction complete: " << result.num_models
         << " model(s), largest=" << result.largest_model_index
         << ", registered_images=" << result.registered_images
         << ", points=" << result.sparse_points
         << ", observations=" << result.observations
         << ", mean_reprojection_error=" << result.mean_reprojection_error;
  return stream.str();
}

ColmapKitStatus RunSparseReconstructionImpl(
    const ColmapKitSparseReconstructionConfig& config,
    ColmapKitSparseReconstructionResult* result,
    ColmapKitCancellationContext* cancellation) {
  colmap::EnsureOpenImageIOInitialized();
  ValidateConfig(config);
  ThrowIfCancelled(cancellation);

  const auto database_path = PathFromCString(config.database_path);
  const auto output_path = PathFromCString(config.output_path);
  const auto sparse_text_path =
      CStringOrEmpty(config.sparse_text_output_path).empty()
          ? output_path.parent_path() / "sparse-text"
          : PathFromCString(config.sparse_text_output_path);

  CreateParentDirIfNeeded(database_path);
  CreateDirIfNotExists(output_path, true);
  if (config.write_sparse_text != 0) {
    CreateDirIfNotExists(sparse_text_path, true);
  }

  EmitProgress(config,
               COLMAPKIT_PROGRESS_STAGE_PREPARING,
               "Preparing COLMAP sparse reconstruction");
  ThrowIfCancelled(cancellation);

  auto extraction_options = MakeExtractionOptions(config);
  auto reader_options = MakeReaderOptions(config, extraction_options);

  EmitProgress(config,
               COLMAPKIT_PROGRESS_STAGE_FEATURE_EXTRACTION,
               "Extracting SIFT features");
  auto feature_extractor = colmap::CreateFeatureExtractorController(
      database_path, reader_options, extraction_options);
  RunThread(feature_extractor.get(), cancellation);
  ThrowIfCancelled(cancellation);

  auto matching_options = MakeMatchingOptions(config);
  EmitProgress(
      config, COLMAPKIT_PROGRESS_STAGE_MATCHING, "Matching image features");
  auto matcher = MakeMatcher(config, matching_options);
  RunThread(matcher.get(), cancellation);
  ThrowIfCancelled(cancellation);

  auto mapper_options = MakeMapperOptions(config);
  auto reconstruction_manager =
      std::make_shared<colmap::ReconstructionManager>();
  size_t registered_callback_count = 0;

  EmitProgress(
      config, COLMAPKIT_PROGRESS_STAGE_MAPPING, "Running incremental mapper");
  const auto mapping_callback = [&]() {
    ++registered_callback_count;
    EmitProgress(config,
                 COLMAPKIT_PROGRESS_STAGE_MAPPING,
                 "Registered image",
                 {},
                 -1.0,
                 registered_callback_count,
                 0);
  };

  std::function<bool()> check_if_stopped;
  if (cancellation != nullptr) {
    check_if_stopped = [cancellation]() {
      return cancellation->IsCancellationRequested();
    };
  }

  const bool mapper_ok =
      colmap::RunIncrementalMapperImpl(database_path,
                                       PathFromCString(config.image_path),
                                       output_path,
                                       mapper_options,
                                       reconstruction_manager,
                                       mapping_callback,
                                       mapping_callback,
                                       check_if_stopped);
  ThrowIfCancelled(cancellation);
  if (!mapper_ok) {
    throw std::runtime_error(
        "Incremental mapper failed to create a sparse model.");
  }

  reconstruction_manager->Write(output_path);

  if (config.write_sparse_text != 0) {
    EmitProgress(config,
                 COLMAPKIT_PROGRESS_STAGE_SPARSE_TEXT_EXPORT,
                 "Writing COLMAP sparse text output");
    ThrowIfCancelled(cancellation);
    WriteSparseText(*reconstruction_manager, sparse_text_path);
  }
  ThrowIfCancelled(cancellation);

  FillResultMetrics(*reconstruction_manager, result);
  result->status = COLMAPKIT_STATUS_OK;
  CopyMessage(SummaryMessage(*result), result);
  EmitProgress(config,
               COLMAPKIT_PROGRESS_STAGE_FINISHED,
               "Sparse reconstruction complete",
               result->message,
               1.0);
  return COLMAPKIT_STATUS_OK;
}

ColmapKitStatus RunSparseReconstructionWithResult(
    const ColmapKitSparseReconstructionConfig& config,
    ColmapKitSparseReconstructionResult* result,
    ColmapKitCancellationContext* cancellation) {
  try {
    return RunSparseReconstructionImpl(config, result, cancellation);
  } catch (const ColmapKitCancelledError&) {
    SetCancelled(result);
    EmitProgress(config,
                 COLMAPKIT_PROGRESS_STAGE_CANCELLED,
                 "Sparse reconstruction cancelled",
                 result == nullptr ? "" : result->message);
  } catch (const std::invalid_argument& error) {
    SetFailure(COLMAPKIT_STATUS_INVALID_ARGUMENT, error.what(), result);
    EmitProgress(config,
                 COLMAPKIT_PROGRESS_STAGE_FAILED,
                 "Sparse reconstruction failed",
                 result == nullptr ? "" : result->message);
  } catch (const std::runtime_error& error) {
    SetFailure(COLMAPKIT_STATUS_RUNTIME_ERROR, error.what(), result);
    EmitProgress(config,
                 COLMAPKIT_PROGRESS_STAGE_FAILED,
                 "Sparse reconstruction failed",
                 result == nullptr ? "" : result->message);
  } catch (const std::exception& error) {
    SetFailure(COLMAPKIT_STATUS_RUNTIME_ERROR, error.what(), result);
    EmitProgress(config,
                 COLMAPKIT_PROGRESS_STAGE_FAILED,
                 "Sparse reconstruction failed",
                 result == nullptr ? "" : result->message);
  } catch (...) {
    SetFailure(COLMAPKIT_STATUS_RUNTIME_ERROR,
               "Unknown ColmapKit sparse reconstruction failure.",
               result);
    EmitProgress(config,
                 COLMAPKIT_PROGRESS_STAGE_FAILED,
                 "Sparse reconstruction failed",
                 result == nullptr ? "" : result->message);
  }

  return result == nullptr ? COLMAPKIT_STATUS_RUNTIME_ERROR : result->status;
}

}  // namespace

struct ColmapKitSparseReconstructionJob {
  explicit ColmapKitSparseReconstructionJob(
      OwnedSparseReconstructionConfig owned_config)
      : config(std::move(owned_config)) {
    ResetResult(&result);
  }

  ~ColmapKitSparseReconstructionJob() {
    RequestCancel();
    Wait();
  }

  void Start() {
    worker = std::thread([this]() {
      ResetResult(&result);
      config.RefreshPointers();
      RunSparseReconstructionWithResult(config.config, &result, &cancellation);
    });
  }

  void RequestCancel() { cancellation.RequestCancel(); }

  void Wait() {
    if (worker.joinable()) {
      worker.join();
    }
  }

  OwnedSparseReconstructionConfig config;
  ColmapKitCancellationContext cancellation;
  ColmapKitSparseReconstructionResult result = {};
  std::thread worker;
};

const char* ColmapKitVersion(void) {
  static const std::string version =
      colmap::GetVersionInfo() + " (" + colmap::GetBuildInfo() + ")";
  return version.c_str();
}

ColmapKitStatus ColmapKitInitialize(const char* application_name) {
  static std::once_flag init_once;
  static ColmapKitStatus init_status = COLMAPKIT_STATUS_OK;

  try {
    const std::string name = CStringOrEmpty(application_name).empty()
                                 ? "ColmapKit"
                                 : CStringOrEmpty(application_name);
    std::call_once(init_once, [&]() {
      static std::string app_name = name;
      char* argv[] = {app_name.data(), nullptr};
      colmap::InitializeGlog(argv);
      colmap::EnsureOpenImageIOInitialized();
    });
  } catch (...) {
    init_status = COLMAPKIT_STATUS_RUNTIME_ERROR;
  }

  return init_status;
}

ColmapKitStatus ColmapKitRunSparseReconstruction(
    const ColmapKitSparseReconstructionConfig* config,
    ColmapKitSparseReconstructionResult* result) {
  ResetResult(result);
  if (result == nullptr) {
    return COLMAPKIT_STATUS_INVALID_ARGUMENT;
  }
  if (config == nullptr) {
    SetFailure(
        COLMAPKIT_STATUS_INVALID_ARGUMENT, "config is required.", result);
    return result->status;
  }

  return RunSparseReconstructionWithResult(*config, result, nullptr);
}

ColmapKitStatus ColmapKitStartSparseReconstruction(
    const ColmapKitSparseReconstructionConfig* config,
    ColmapKitSparseReconstructionJob** job) {
  if (job == nullptr) {
    return COLMAPKIT_STATUS_INVALID_ARGUMENT;
  }
  *job = nullptr;
  if (config == nullptr) {
    return COLMAPKIT_STATUS_INVALID_ARGUMENT;
  }
  if (config->struct_size != 0 && config->struct_size < kMinimumConfigSize) {
    return COLMAPKIT_STATUS_INVALID_ARGUMENT;
  }

  try {
    auto owned_config = OwnedSparseReconstructionConfig::CopyFrom(*config);
    auto new_job = std::make_unique<ColmapKitSparseReconstructionJob>(
        std::move(owned_config));
    new_job->Start();
    *job = new_job.release();
    return COLMAPKIT_STATUS_OK;
  } catch (const std::invalid_argument&) {
    return COLMAPKIT_STATUS_INVALID_ARGUMENT;
  } catch (...) {
    return COLMAPKIT_STATUS_RUNTIME_ERROR;
  }
}

ColmapKitStatus ColmapKitCancelSparseReconstruction(
    ColmapKitSparseReconstructionJob* job) {
  if (job == nullptr) {
    return COLMAPKIT_STATUS_INVALID_ARGUMENT;
  }
  job->RequestCancel();
  return COLMAPKIT_STATUS_OK;
}

ColmapKitStatus ColmapKitWaitSparseReconstruction(
    ColmapKitSparseReconstructionJob* job,
    ColmapKitSparseReconstructionResult* result) {
  ResetResult(result);
  if (result == nullptr) {
    return COLMAPKIT_STATUS_INVALID_ARGUMENT;
  }
  if (job == nullptr) {
    SetFailure(COLMAPKIT_STATUS_INVALID_ARGUMENT, "job is required.", result);
    return result->status;
  }

  job->Wait();
  *result = job->result;
  return result->status;
}

void ColmapKitReleaseSparseReconstructionJob(
    ColmapKitSparseReconstructionJob* job) {
  delete job;
}
