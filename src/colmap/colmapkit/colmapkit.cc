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
#include "colmap/feature/metal_matcher.h"
#include "colmap/feature/sift.h"
#include "colmap/scene/reconstruction_manager.h"
#include "colmap/sensor/models.h"
#include "colmap/util/file.h"
#include "colmap/util/logging.h"
#include "colmap/util/oiio_utils.h"
#include "colmap/util/threading.h"
#include "colmap/util/version.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace {

using colmap::CreateDirIfNotExists;
using colmap::ExistsDir;
using colmap::ExistsFile;

void CreateParentDirIfNeeded(const std::filesystem::path& path);

constexpr size_t kCurrentResultSize =
    sizeof(ColmapKitSparseReconstructionResult);
constexpr size_t kLegacyConfigSize =
    offsetof(ColmapKitSparseReconstructionConfig, progress_user_data) +
    sizeof(void*);
constexpr size_t kMinimumConfigSize = kLegacyConfigSize;
constexpr size_t kLegacyResultSize = offsetof(
    ColmapKitSparseReconstructionResult, sparse_reconstruction_abi_version);
static_assert(kLegacyConfigSize == 136,
              "Released sparse reconstruction config ABI size changed.");
static_assert(kLegacyResultSize == 1088,
              "Released sparse reconstruction result ABI size changed.");
constexpr auto kProgressEmissionInterval = std::chrono::milliseconds(250);
constexpr auto kThreadPollInterval = std::chrono::milliseconds(25);

std::string CStringOrEmpty(const char* value) {
  return value == nullptr ? std::string() : std::string(value);
}

std::filesystem::path PathFromCString(const char* value) {
  return std::filesystem::path(CStringOrEmpty(value));
}

bool ConfigHasField(const ColmapKitSparseReconstructionConfig& config,
                    const size_t field_end) {
  const size_t available_size =
      config.struct_size == 0 ? kLegacyConfigSize : config.struct_size;
  return available_size >= field_end;
}

#define COLMAPKIT_CONFIG_HAS_FIELD(config, field)                       \
  ConfigHasField(config,                                                \
                 offsetof(ColmapKitSparseReconstructionConfig, field) + \
                     sizeof(config.field))

int EffectiveConfigThreadRequest(
    const ColmapKitSparseReconstructionConfig& config,
    const bool has_override,
    const int override_value) {
  if (has_override && override_value != 0) {
    return override_value;
  }
  return config.num_threads == 0 ? -1 : config.num_threads;
}

int ExtractionThreadRequest(const ColmapKitSparseReconstructionConfig& config) {
  return EffectiveConfigThreadRequest(
      config,
      COLMAPKIT_CONFIG_HAS_FIELD(config, extraction_num_threads),
      COLMAPKIT_CONFIG_HAS_FIELD(config, extraction_num_threads)
          ? config.extraction_num_threads
          : 0);
}

int MatchingThreadRequest(const ColmapKitSparseReconstructionConfig& config) {
  return EffectiveConfigThreadRequest(
      config,
      COLMAPKIT_CONFIG_HAS_FIELD(config, matching_num_threads),
      COLMAPKIT_CONFIG_HAS_FIELD(config, matching_num_threads)
          ? config.matching_num_threads
          : 0);
}

int MapperThreadRequest(const ColmapKitSparseReconstructionConfig& config) {
  return EffectiveConfigThreadRequest(
      config,
      COLMAPKIT_CONFIG_HAS_FIELD(config, mapper_num_threads),
      COLMAPKIT_CONFIG_HAS_FIELD(config, mapper_num_threads)
          ? config.mapper_num_threads
          : 0);
}

bool RequireMetalSift(const ColmapKitSparseReconstructionConfig& config) {
  return COLMAPKIT_CONFIG_HAS_FIELD(config, require_metal_sift) &&
         config.require_metal_sift != 0;
}

bool RequireMetalMatching(const ColmapKitSparseReconstructionConfig& config) {
  return COLMAPKIT_CONFIG_HAS_FIELD(config, require_metal_matching) &&
         config.require_metal_matching != 0;
}

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
  std::string evidence_path;

  bool has_sparse_text_output_path = false;
  bool has_image_list_path = false;
  bool has_camera_model = false;
  bool has_camera_params = false;
  bool has_evidence_path = false;

  static OwnedSparseReconstructionConfig CopyFrom(
      const ColmapKitSparseReconstructionConfig& source) {
    OwnedSparseReconstructionConfig owned;
    const size_t source_size =
        source.struct_size == 0 ? kLegacyConfigSize : source.struct_size;
    std::memcpy(
        &owned.config, &source, std::min(source_size, sizeof(owned.config)));
    owned.database_path = CStringOrEmpty(source.database_path);
    owned.image_path = CStringOrEmpty(source.image_path);
    owned.output_path = CStringOrEmpty(source.output_path);
    owned.sparse_text_output_path =
        CStringOrEmpty(source.sparse_text_output_path);
    owned.image_list_path = CStringOrEmpty(source.image_list_path);
    owned.camera_model = CStringOrEmpty(source.camera_model);
    owned.camera_params = CStringOrEmpty(source.camera_params);
    if (COLMAPKIT_CONFIG_HAS_FIELD(source, evidence_path)) {
      owned.evidence_path = CStringOrEmpty(source.evidence_path);
      owned.has_evidence_path = source.evidence_path != nullptr;
    }
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
    config.evidence_path = has_evidence_path ? evidence_path.c_str() : nullptr;
  }
};

ColmapKitSparseReconstructionConfig LegacyConfigCopy(
    const ColmapKitSparseReconstructionConfig& source) {
  ColmapKitSparseReconstructionConfig legacy = {};
  const size_t source_size =
      source.struct_size == 0 ? kLegacyConfigSize : source.struct_size;
  std::memcpy(&legacy, &source, std::min(source_size, kLegacyConfigSize));
  legacy.struct_size = kLegacyConfigSize;
  return legacy;
}

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

void CopyResultToCaller(const ColmapKitSparseReconstructionResult& source,
                        ColmapKitSparseReconstructionResult* destination,
                        const bool extended_semantics) {
  if (destination == nullptr) {
    return;
  }
  const size_t destination_size = destination->struct_size == 0
                                      ? kLegacyResultSize
                                      : destination->struct_size;
  const size_t reported_size =
      extended_semantics ? sizeof(source) : kLegacyResultSize;
  const size_t copy_size = std::min(destination_size, reported_size);
  std::memcpy(destination, &source, copy_size);
  destination->struct_size = reported_size;
}

bool UsesExtendedSparseConfig(
    const ColmapKitSparseReconstructionConfig& config) {
  return config.struct_size >= sizeof(config);
}

bool HasPartialSparseConfig(const ColmapKitSparseReconstructionConfig& config) {
  return config.struct_size > kLegacyConfigSize &&
         config.struct_size < sizeof(config);
}

bool HasPartialSparseResult(const ColmapKitSparseReconstructionResult& result) {
  return result.struct_size > kLegacyResultSize &&
         result.struct_size < sizeof(result);
}

uint64_t CurrentResidentMemoryBytes() {
#if defined(__APPLE__)
  mach_task_basic_info_data_t info = {};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(),
                MACH_TASK_BASIC_INFO,
                reinterpret_cast<task_info_t>(&info),
                &count) == KERN_SUCCESS) {
    return static_cast<uint64_t>(info.resident_size);
  }
#endif
  return 0;
}

class ResidentMemorySampler {
 public:
  ResidentMemorySampler() {
    Sample();
    worker_ = std::thread([this]() {
      while (!stopped_.load(std::memory_order_acquire)) {
        Sample();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      Sample();
    });
  }

  ~ResidentMemorySampler() { Stop(); }

  uint64_t Stop() {
    stopped_.store(true, std::memory_order_release);
    if (worker_.joinable()) {
      worker_.join();
    }
    return peak_.load(std::memory_order_relaxed);
  }

 private:
  void Sample() {
    const uint64_t resident = CurrentResidentMemoryBytes();
    uint64_t peak = peak_.load(std::memory_order_relaxed);
    while (resident > peak && !peak_.compare_exchange_weak(
                                  peak, resident, std::memory_order_relaxed)) {
    }
  }

  std::atomic<bool> stopped_{false};
  std::atomic<uint64_t> peak_{0};
  std::thread worker_;
};

class ScopedSeconds {
 public:
  explicit ScopedSeconds(double* output)
      : output_(output), start_(std::chrono::steady_clock::now()) {}
  ~ScopedSeconds() {
    if (output_ != nullptr) {
      *output_ += std::chrono::duration<double>(
                      std::chrono::steady_clock::now() - start_)
                      .count();
    }
  }

 private:
  double* output_;
  std::chrono::steady_clock::time_point start_;
};

std::string JsonEscape(const std::string& value) {
  std::ostringstream stream;
  for (const unsigned char ch : value) {
    switch (ch) {
      case '\\':
        stream << "\\\\";
        break;
      case '"':
        stream << "\\\"";
        break;
      case '\n':
        stream << "\\n";
        break;
      case '\r':
        stream << "\\r";
        break;
      case '\t':
        stream << "\\t";
        break;
      default:
        if (ch < 0x20) {
          stream << "?";
        } else {
          stream << ch;
        }
    }
  }
  return stream.str();
}

const char* BackendName(const ColmapKitComputeBackend backend) {
  switch (backend) {
    case COLMAPKIT_COMPUTE_BACKEND_CPU:
      return "cpu";
    case COLMAPKIT_COMPUTE_BACKEND_METAL:
      return "metal";
    case COLMAPKIT_COMPUTE_BACKEND_UNKNOWN:
    default:
      return "unknown";
  }
}

std::string RuntimeSourceRevision() {
#if defined(COLMAPKIT_SOURCE_REVISION)
  if (std::strlen(COLMAPKIT_SOURCE_REVISION) > 0) {
    return COLMAPKIT_SOURCE_REVISION;
  }
#endif
  return colmap::GetBuildInfo();
}

void WriteRuntimeEvidence(const ColmapKitSparseReconstructionConfig& config,
                          const ColmapKitSparseReconstructionResult& result) {
  if (!COLMAPKIT_CONFIG_HAS_FIELD(config, evidence_path) ||
      CStringOrEmpty(config.evidence_path).empty()) {
    return;
  }
  const std::filesystem::path output_path =
      PathFromCString(config.evidence_path);
  CreateParentDirIfNeeded(output_path);
  const std::filesystem::path temporary_path = output_path.string() + ".tmp";
  std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Could not open reconstruction evidence path.");
  }
  output << "{\n"
         << "  \"schemaVersion\": 1,\n"
         << "  \"engine\": {\"version\": \"" << JsonEscape(ColmapKitVersion())
         << "\", \"sourceRevision\": \"" << JsonEscape(RuntimeSourceRevision())
         << "\", \"artifactChecksum\": null, \"abi\": 2, "
            "\"compiledBackends\": [\"cpu\"";
#if defined(COLMAP_METAL_ENABLED)
  output << ", \"metal-matching\"";
#endif
#if defined(COLMAP_SIFT_METAL_ENABLED)
  output << ", \"metal-sift\"";
#endif
  output << "]},\n"
         << "  \"request\": {"
         << "\"legacyNumThreads\": " << config.num_threads << ", "
         << "\"featureExtractionNumThreads\": "
         << result.requested_extraction_num_threads << ", "
         << "\"featureMatchingNumThreads\": "
         << result.requested_matching_num_threads << ", "
         << "\"mapperNumThreads\": " << result.requested_mapper_num_threads
         << ", "
         << "\"bundleAdjustmentNumThreads\": "
         << result.requested_mapper_num_threads << ", "
         << "\"useGpu\": " << (config.use_gpu != 0 ? "true" : "false")
         << ", \"useMetalSift\": "
         << (config.use_metal_sift != 0 ? "true" : "false")
         << ", \"useMetalMatching\": "
         << (config.use_metal_matching != 0 ? "true" : "false")
         << ", \"requireMetalSiftNoFallback\": "
         << (RequireMetalSift(config) ? "true" : "false")
         << ", \"requireMetalMatchingNoFallback\": "
         << (RequireMetalMatching(config) ? "true" : "false") << "},\n"
         << "  \"effective\": {"
         << "\"featureExtractionNumThreads\": "
         << result.effective_extraction_num_threads << ", "
         << "\"featureMatchingNumThreads\": "
         << result.effective_matching_num_threads << ", "
         << "\"geometricVerificationNumThreads\": "
         << result.effective_geometric_verification_num_threads << ", "
         << "\"mapperNumThreads\": " << result.effective_mapper_num_threads
         << ", \"bundleAdjustmentNumThreads\": "
         << result.effective_bundle_adjustment_num_threads << ", "
         << "\"featureExtractionBackend\": \""
         << BackendName(result.extraction_backend) << "\", "
         << "\"featureMatchingBackend\": \""
         << BackendName(result.matching_backend) << "\", "
         << "\"mappingBackend\": \"" << BackendName(result.mapping_backend)
         << "\", \"bundleAdjustmentBackend\": \""
         << BackendName(result.bundle_adjustment_backend) << "\"},\n"
         << "  \"metal\": {\"deviceName\": \""
         << JsonEscape(result.metal_device_name) << "\", "
         << "\"siftCompiled\": "
         << (result.metal_sift_compiled ? "true" : "false") << ", "
         << "\"siftAvailable\": "
         << (result.metal_sift_available ? "true" : "false") << ", "
         << "\"siftActual\": "
         << (result.extraction_backend == COLMAPKIT_COMPUTE_BACKEND_METAL
                 ? "true"
                 : "false")
         << ", \"siftDispatchCount\": " << result.metal_sift_operations
         << ", \"siftFallbackCount\": " << result.metal_sift_fallbacks
         << ", \"matchingCompiled\": "
         << (result.metal_matching_compiled ? "true" : "false") << ", "
         << "\"matchingAvailable\": "
         << (result.metal_matching_available ? "true" : "false") << ", "
         << "\"matchingActual\": "
         << (result.matching_backend == COLMAPKIT_COMPUTE_BACKEND_METAL
                 ? "true"
                 : "false")
         << ", \"matchingDispatchCount\": " << result.metal_matching_operations
         << ", \"matchingFallbackCount\": " << result.metal_matching_fallbacks
         << "},\n"
         << "  \"timingsSeconds\": {\"featureExtraction\": "
         << result.extraction_seconds
         << ", \"featureMatching\": " << result.matching_seconds
         << ", \"mappingBundleAdjustment\": "
         << result.mapping_bundle_adjustment_seconds
         << ", \"bundleAdjustment\": null, \"export\": "
         << result.export_seconds << ", \"total\": " << result.total_seconds
         << "},\n"
         << "  \"resources\": {\"residentPeakBytes\": "
         << result.peak_resident_memory_bytes << "},\n"
         << "  \"output\": {\"numModels\": " << result.num_models
         << ", \"componentID\": " << result.largest_model_index
         << ", \"registeredImages\": " << result.registered_images
         << ", \"sparsePoints\": " << result.sparse_points
         << ", \"observations\": " << result.observations
         << ", \"meanReprojectionError\": " << result.mean_reprojection_error
         << "},\n"
         << "  \"strict\": {\"noFallbackSatisfied\": "
         << (result.no_fallback_satisfied ? "true" : "false")
         << ", \"failureReason\": "
         << (result.status == COLMAPKIT_STATUS_OK
                 ? "null"
                 : "\"" + JsonEscape(result.message) + "\"")
         << "},\n"
         << "  \"status\": " << static_cast<int>(result.status) << "\n"
         << "}\n";
  output.close();
  if (!output) {
    throw std::runtime_error("Could not write reconstruction evidence.");
  }
  std::error_code error;
  std::filesystem::rename(temporary_path, output_path, error);
  if (error) {
    std::filesystem::remove(output_path, error);
    error.clear();
    std::filesystem::rename(temporary_path, output_path, error);
  }
  if (error) {
    throw std::runtime_error("Could not finalize reconstruction evidence.");
  }
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

struct ProgressSnapshot {
  size_t current = 0;
  size_t total = 0;
  std::string detail;
  uint64_t revision = 0;
};

class StageProgressState {
 public:
  void Set(size_t current, size_t total, const std::string& detail = {}) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_ = current;
    total_ = total;
    detail_ = detail;
    ++revision_;
  }

  void Advance(size_t amount) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_ += amount;
    ++revision_;
  }

  ProgressSnapshot GetSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {current_, total_, detail_, revision_};
  }

 private:
  mutable std::mutex mutex_;
  size_t current_ = 0;
  size_t total_ = 0;
  std::string detail_;
  uint64_t revision_ = 0;
};

class StageProgressEmitter {
 public:
  StageProgressEmitter(const ColmapKitSparseReconstructionConfig& config,
                       ColmapKitProgressStage stage,
                       std::string message,
                       const StageProgressState& state)
      : config_(config),
        stage_(stage),
        message_(std::move(message)),
        state_(state) {}

  void Start() {
    const ProgressSnapshot snapshot = state_.GetSnapshot();
    Emit(snapshot, message_, /*stage_complete=*/false);
  }

  void EmitPending() {
    const ProgressSnapshot snapshot = state_.GetSnapshot();
    if (snapshot.revision == last_emitted_revision_) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last_emission_time_ < kProgressEmissionInterval) {
      return;
    }
    Emit(snapshot, message_, /*stage_complete=*/false);
  }

  void Finish(const std::string& message) {
    Emit(state_.GetSnapshot(), message, /*stage_complete=*/true);
  }

 private:
  void Emit(const ProgressSnapshot& snapshot,
            const std::string& message,
            bool stage_complete) {
    double fraction = -1.0;
    if (snapshot.total > 0) {
      fraction = std::min(1.0,
                          static_cast<double>(snapshot.current) /
                              static_cast<double>(snapshot.total));
    } else if (stage_complete) {
      fraction = 1.0;
    }
    EmitProgress(config_,
                 stage_,
                 message,
                 snapshot.detail,
                 fraction,
                 snapshot.current,
                 snapshot.total);
    last_emitted_revision_ = snapshot.revision;
    last_emission_time_ = std::chrono::steady_clock::now();
  }

  const ColmapKitSparseReconstructionConfig& config_;
  const ColmapKitProgressStage stage_;
  const std::string message_;
  const StageProgressState& state_;
  uint64_t last_emitted_revision_ = 0;
  std::chrono::steady_clock::time_point last_emission_time_ =
      std::chrono::steady_clock::time_point::min();
};

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
  if (ExtractionThreadRequest(config) < -1 ||
      MatchingThreadRequest(config) < -1 || MapperThreadRequest(config) < -1) {
    throw std::invalid_argument(
        "Thread counts must be -1, zero (inherit), or positive.");
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
    const ColmapKitSparseReconstructionConfig& config,
    const std::shared_ptr<colmap::MetalRuntimeTelemetry>& metal_telemetry) {
  colmap::FeatureExtractionOptions options;
  options.max_image_size =
      config.max_image_size <= 0 ? -1 : config.max_image_size;
  options.num_threads = ExtractionThreadRequest(config);
  options.use_gpu = config.use_gpu != 0 || config.use_metal_sift != 0;
  options.sift->use_metal = config.use_metal_sift != 0;
  options.sift->require_metal = RequireMetalSift(config);
  options.sift->metal_runtime_telemetry = metal_telemetry;
  options.sift->estimate_affine_shape = config.estimate_affine_shape != 0;
  options.sift->domain_size_pooling = config.domain_size_pooling != 0;
  if (options.sift->require_metal && !options.sift->use_metal) {
    throw std::invalid_argument("require_metal_sift requires use_metal_sift.");
  }
  if (options.sift->require_metal &&
      colmap::RequiresCovariantSiftExtractor(*options.sift)) {
    throw std::invalid_argument(
        "Strict Metal SIFT does not support covariant extraction options.");
  }
#if !defined(COLMAP_SIFT_METAL_ENABLED)
  if (options.sift->require_metal) {
    throw std::runtime_error(
        "Strict Metal SIFT was requested but this artifact was built without "
        "SIFT_METAL_ENABLED.");
  }
#endif
  if (!options.Check()) {
    throw std::invalid_argument("Invalid feature extraction options.");
  }
  if (options.RequiresOpenGL()) {
    throw std::runtime_error(
        "The requested feature extraction options require OpenGL, which is not "
        "supported by the ColmapKit facade.");
  }
#if defined(COLMAP_SIFT_METAL_ENABLED)
  if (options.sift->require_metal) {
    // The controller initializes extractors on worker threads and reports an
    // invalid setup asynchronously. Probe strict Metal synchronously so an
    // unavailable device or shader becomes a normal ABI failure with evidence
    // instead of entering the controller's fatal invalid-setup path.
    auto probe = colmap::CreateSiftFeatureExtractor(options);
    if (probe == nullptr) {
      const auto snapshot = metal_telemetry->Snapshot();
      throw std::runtime_error(
          snapshot.last_failure.empty()
              ? "Strict Metal SIFT was requested but no usable Metal "
                "extractor is available."
              : "Strict Metal SIFT is unavailable: " + snapshot.last_failure);
    }
  }
#endif
  return options;
}

colmap::FeatureMatchingOptions MakeMatchingOptions(
    const ColmapKitSparseReconstructionConfig& config,
    const std::shared_ptr<colmap::MetalRuntimeTelemetry>& metal_telemetry) {
  colmap::FeatureMatchingOptions options(
      colmap::FeatureMatcherType::SIFT_BRUTEFORCE);
  options.num_threads = MatchingThreadRequest(config);
  options.use_gpu = config.use_gpu != 0 || config.use_metal_matching != 0;
  options.sift->use_metal = config.use_metal_matching != 0;
  options.sift->require_metal = RequireMetalMatching(config);
  options.sift->metal_runtime_telemetry = metal_telemetry;
  if (options.sift->require_metal && !options.sift->use_metal) {
    throw std::invalid_argument(
        "require_metal_matching requires use_metal_matching.");
  }
  if (options.sift->require_metal && !colmap::IsMetalSiftMatcherAvailable()) {
    throw std::runtime_error(
        "Strict Metal matching was requested but no usable Metal matcher "
        "device or pipeline is available.");
  }
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

colmap::ExhaustivePairingOptions MakeExhaustivePairingOptions() {
  colmap::ExhaustivePairingOptions options;
  if (!options.Check()) {
    throw std::invalid_argument("Invalid exhaustive matcher options.");
  }
  return options;
}

colmap::SpatialPairingOptions MakeSpatialPairingOptions(
    const ColmapKitSparseReconstructionConfig& config) {
  colmap::SpatialPairingOptions options;
  options.num_threads = MatchingThreadRequest(config);
  if (!options.Check()) {
    throw std::invalid_argument("Invalid spatial matcher options.");
  }
  return options;
}

colmap::SequentialPairingOptions MakeSequentialPairingOptions(
    const ColmapKitSparseReconstructionConfig& config) {
  colmap::SequentialPairingOptions options;
  options.overlap =
      config.sequential_overlap <= 0 ? 10 : config.sequential_overlap;
  options.quadratic_overlap = true;
  options.loop_detection = false;
  options.num_threads = MatchingThreadRequest(config);
  if (!options.Check()) {
    throw std::invalid_argument("Invalid sequential matcher options.");
  }
  return options;
}

size_t CountGeneratedPairs(colmap::PairGenerator& pair_generator) {
  size_t total = 0;
  while (!pair_generator.HasFinished()) {
    total += pair_generator.Next().size();
  }
  return total;
}

size_t CountMatchingPairs(const ColmapKitSparseReconstructionConfig& config) {
  const auto database =
      colmap::Database::Open(PathFromCString(config.database_path));
  switch (config.matcher) {
    case COLMAPKIT_MATCHER_EXHAUSTIVE: {
      const size_t num_images = database->NumImages();
      return num_images < 2 ? 0 : num_images * (num_images - 1) / 2;
    }
    case COLMAPKIT_MATCHER_SPATIAL:
      return 0;
    case COLMAPKIT_MATCHER_SEQUENTIAL:
    default: {
      colmap::SequentialPairGenerator pair_generator(
          MakeSequentialPairingOptions(config), database);
      return CountGeneratedPairs(pair_generator);
    }
  }
}

std::unique_ptr<colmap::Thread> MakeMatcher(
    const ColmapKitSparseReconstructionConfig& config,
    const colmap::FeatureMatchingOptions& matching_options,
    colmap::FeatureMatchingProgressCallback progress_callback) {
  colmap::TwoViewGeometryOptions geometry_options;
  const auto database_path = PathFromCString(config.database_path);

  switch (config.matcher) {
    case COLMAPKIT_MATCHER_EXHAUSTIVE: {
      return colmap::CreateExhaustiveFeatureMatcher(
          MakeExhaustivePairingOptions(),
          matching_options,
          geometry_options,
          database_path,
          std::move(progress_callback));
    }
    case COLMAPKIT_MATCHER_SPATIAL: {
      return colmap::CreateSpatialFeatureMatcher(
          MakeSpatialPairingOptions(config),
          matching_options,
          geometry_options,
          database_path,
          std::move(progress_callback));
    }
    case COLMAPKIT_MATCHER_SEQUENTIAL:
    default: {
      return colmap::CreateSequentialFeatureMatcher(
          MakeSequentialPairingOptions(config),
          matching_options,
          geometry_options,
          database_path,
          std::move(progress_callback));
    }
  }
}

std::shared_ptr<colmap::IncrementalPipelineOptions> MakeMapperOptions(
    const ColmapKitSparseReconstructionConfig& config) {
  auto options = std::make_shared<colmap::IncrementalPipelineOptions>();
  options->num_threads = MapperThreadRequest(config);
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
               ColmapKitCancellationContext* cancellation,
               const std::function<void()>& poll_progress) {
  ThrowIfCancelled(cancellation);
  ScopedActiveThread active_thread(cancellation, thread);
  thread->Start();
  while (!thread->IsFinished()) {
    if (cancellation != nullptr && cancellation->IsCancellationRequested()) {
      thread->Stop();
      break;
    }
    if (poll_progress) {
      poll_progress();
    }
    std::this_thread::sleep_for(kThreadPollInterval);
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

size_t UpdateRegisteredImageIds(
    const colmap::ReconstructionManager& manager,
    std::unordered_set<colmap::image_t>* registered_image_ids) {
  for (size_t i = 0; i < manager.Size(); ++i) {
    for (const colmap::image_t image_id : manager.Get(i)->RegImageIds()) {
      registered_image_ids->insert(image_id);
    }
  }
  return registered_image_ids->size();
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

void PopulateRuntimeRequest(const ColmapKitSparseReconstructionConfig& config,
                            ColmapKitSparseReconstructionResult* result) {
  result->sparse_reconstruction_abi_version = 2;
  result->requested_extraction_num_threads = ExtractionThreadRequest(config);
  result->requested_matching_num_threads = MatchingThreadRequest(config);
  result->requested_mapper_num_threads = MapperThreadRequest(config);
  result->effective_extraction_num_threads =
      config.use_metal_sift != 0
          ? 1
          : colmap::GetEffectiveNumThreads(ExtractionThreadRequest(config));
  result->effective_matching_num_threads =
      config.use_metal_matching != 0
          ? 1
          : colmap::GetEffectiveNumThreads(MatchingThreadRequest(config));
  result->effective_geometric_verification_num_threads =
      colmap::GetEffectiveNumThreads(MatchingThreadRequest(config));
  result->effective_mapper_num_threads =
      colmap::GetEffectiveNumThreads(MapperThreadRequest(config));
  result->effective_bundle_adjustment_num_threads =
      result->effective_mapper_num_threads;
  result->extraction_backend = COLMAPKIT_COMPUTE_BACKEND_CPU;
  result->matching_backend = COLMAPKIT_COMPUTE_BACKEND_CPU;
  result->mapping_backend = COLMAPKIT_COMPUTE_BACKEND_CPU;
  result->bundle_adjustment_backend = COLMAPKIT_COMPUTE_BACKEND_CPU;
#if defined(COLMAP_METAL_ENABLED)
  result->metal_matching_compiled = 1;
  result->metal_matching_available =
      colmap::IsMetalSiftMatcherAvailable() ? 1 : 0;
#endif
#if defined(COLMAP_SIFT_METAL_ENABLED)
  result->metal_sift_compiled = 1;
#endif
  result->metal_sift_requested = config.use_metal_sift != 0 ? 1 : 0;
  result->no_fallback_satisfied = 1;
}

void PopulateMetalRuntimeResult(
    const ColmapKitSparseReconstructionConfig& config,
    const colmap::MetalRuntimeTelemetrySnapshot& telemetry,
    ColmapKitSparseReconstructionResult* result) {
  result->metal_sift_operations = telemetry.sift_extraction_operations;
  result->metal_sift_fallbacks = telemetry.sift_extraction_failures;
  result->metal_matching_operations = telemetry.sift_matching_operations;
  result->metal_matching_fallbacks = telemetry.sift_matching_fallbacks;
  result->metal_sift_available =
      telemetry.sift_extraction_operations > 0 ? 1 : 0;
  if (config.use_metal_sift != 0 && telemetry.sift_extraction_operations > 0 &&
      telemetry.sift_extraction_failures == 0) {
    result->extraction_backend = COLMAPKIT_COMPUTE_BACKEND_METAL;
  }
  if (config.use_metal_matching != 0 &&
      telemetry.sift_matching_operations > 0 &&
      telemetry.sift_matching_fallbacks == 0) {
    result->matching_backend = COLMAPKIT_COMPUTE_BACKEND_METAL;
  }
  const bool strict_sift_satisfied =
      !RequireMetalSift(config) || (telemetry.sift_extraction_operations > 0 &&
                                    telemetry.sift_extraction_failures == 0);
  const bool strict_matching_satisfied =
      !RequireMetalMatching(config) ||
      (telemetry.sift_matching_operations > 0 &&
       telemetry.sift_matching_fallbacks == 0);
  result->no_fallback_satisfied =
      telemetry.sift_extraction_failures == 0 &&
              telemetry.sift_matching_fallbacks == 0 && strict_sift_satisfied &&
              strict_matching_satisfied
          ? 1
          : 0;
  std::strncpy(result->metal_device_name,
               telemetry.device_name.c_str(),
               COLMAPKIT_METAL_DEVICE_NAME_CAPACITY);
  result->metal_device_name[COLMAPKIT_METAL_DEVICE_NAME_CAPACITY - 1] = '\0';
}

ColmapKitStatus RunSparseReconstructionImpl(
    const ColmapKitSparseReconstructionConfig& config,
    ColmapKitSparseReconstructionResult* result,
    ColmapKitCancellationContext* cancellation,
    const std::shared_ptr<colmap::MetalRuntimeTelemetry>& metal_telemetry) {
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

  auto extraction_options = MakeExtractionOptions(config, metal_telemetry);
  auto reader_options = MakeReaderOptions(config, extraction_options);
  // Resolve strict accelerator requirements before doing any expensive work.
  // This makes an unavailable no-fallback request fail at the capability gate
  // instead of after feature extraction has already completed.
  auto matching_options = MakeMatchingOptions(config, metal_telemetry);

  StageProgressState extraction_progress;
  auto feature_extractor = colmap::CreateFeatureExtractorController(
      database_path,
      reader_options,
      extraction_options,
      [&](size_t current, size_t total, const std::string& image_name) {
        extraction_progress.Set(current, total, image_name);
      });
  StageProgressEmitter extraction_emitter(
      config,
      COLMAPKIT_PROGRESS_STAGE_FEATURE_EXTRACTION,
      "Extracting SIFT features",
      extraction_progress);
  extraction_emitter.Start();
  {
    ScopedSeconds extraction_timer(&result->extraction_seconds);
    RunThread(feature_extractor.get(), cancellation, [&]() {
      extraction_emitter.EmitPending();
    });
  }
  extraction_emitter.Finish("Feature extraction complete");
  ThrowIfCancelled(cancellation);
  if (metal_telemetry != nullptr) {
    const auto snapshot = metal_telemetry->Snapshot();
    PopulateMetalRuntimeResult(config, snapshot, result);
    if (RequireMetalSift(config) && (snapshot.sift_extraction_operations == 0 ||
                                     snapshot.sift_extraction_failures != 0)) {
      throw std::runtime_error(
          snapshot.last_failure.empty()
              ? "Strict Metal SIFT completed without proven Metal dispatch."
              : snapshot.last_failure);
    }
  }

  StageProgressState matching_progress;
  matching_progress.Set(/*current=*/0, CountMatchingPairs(config));
  auto matcher = MakeMatcher(config, matching_options, [&](size_t num_pairs) {
    matching_progress.Advance(num_pairs);
  });
  StageProgressEmitter matching_emitter(config,
                                        COLMAPKIT_PROGRESS_STAGE_MATCHING,
                                        "Matching image features",
                                        matching_progress);
  matching_emitter.Start();
  {
    ScopedSeconds matching_timer(&result->matching_seconds);
    RunThread(
        matcher.get(), cancellation, [&]() { matching_emitter.EmitPending(); });
  }
  matching_emitter.Finish("Feature matching complete");
  ThrowIfCancelled(cancellation);
  if (metal_telemetry != nullptr) {
    const auto snapshot = metal_telemetry->Snapshot();
    PopulateMetalRuntimeResult(config, snapshot, result);
    if (RequireMetalMatching(config) &&
        (snapshot.sift_matching_operations == 0 ||
         snapshot.sift_matching_fallbacks != 0)) {
      throw std::runtime_error(
          snapshot.last_failure.empty()
              ? "Strict Metal matching completed without proven Metal dispatch."
              : snapshot.last_failure);
    }
  }

  auto mapper_options = MakeMapperOptions(config);
  auto reconstruction_manager =
      std::make_shared<colmap::ReconstructionManager>();
  const size_t num_mapping_images =
      mapper_options->image_names.empty()
          ? colmap::Database::Open(database_path)->NumImages()
          : mapper_options->image_names.size();
  size_t max_registered_images = 0;
  std::unordered_set<colmap::image_t> registered_image_ids;
  registered_image_ids.reserve(num_mapping_images);
  StageProgressState mapping_progress;
  mapping_progress.Set(/*current=*/0, num_mapping_images);
  StageProgressEmitter mapping_emitter(config,
                                       COLMAPKIT_PROGRESS_STAGE_MAPPING,
                                       "Running incremental mapper",
                                       mapping_progress);
  mapping_emitter.Start();
  const auto mapping_callback = [&]() {
    const size_t registered_images = UpdateRegisteredImageIds(
        *reconstruction_manager, &registered_image_ids);
    if (registered_images > max_registered_images) {
      max_registered_images = registered_images;
      mapping_progress.Set(max_registered_images, num_mapping_images);
      mapping_emitter.EmitPending();
    }
  };

  std::function<bool()> check_if_stopped;
  if (cancellation != nullptr) {
    check_if_stopped = [cancellation]() {
      return cancellation->IsCancellationRequested();
    };
  }

  bool mapper_ok = false;
  {
    ScopedSeconds mapping_timer(&result->mapping_bundle_adjustment_seconds);
    mapper_ok =
        colmap::RunIncrementalMapperImpl(database_path,
                                         PathFromCString(config.image_path),
                                         output_path,
                                         mapper_options,
                                         reconstruction_manager,
                                         mapping_callback,
                                         mapping_callback,
                                         check_if_stopped);
  }
  ThrowIfCancelled(cancellation);
  if (!mapper_ok) {
    throw std::runtime_error(
        "Incremental mapper failed to create a sparse model.");
  }
  max_registered_images = std::max(
      max_registered_images,
      UpdateRegisteredImageIds(*reconstruction_manager, &registered_image_ids));
  mapping_progress.Set(max_registered_images, num_mapping_images);
  mapping_emitter.Finish("Incremental mapping complete");

  {
    ScopedSeconds export_timer(&result->export_seconds);
    reconstruction_manager->Write(output_path);

    if (config.write_sparse_text != 0) {
      EmitProgress(config,
                   COLMAPKIT_PROGRESS_STAGE_SPARSE_TEXT_EXPORT,
                   "Writing COLMAP sparse text output");
      ThrowIfCancelled(cancellation);
      WriteSparseText(*reconstruction_manager, sparse_text_path);
    }
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
    ColmapKitCancellationContext* cancellation,
    const bool extended_semantics) {
  const auto total_start = std::chrono::steady_clock::now();
  std::shared_ptr<colmap::MetalRuntimeTelemetry> metal_telemetry;
  std::unique_ptr<ResidentMemorySampler> memory_sampler;
  if (extended_semantics) {
    PopulateRuntimeRequest(config, result);
    metal_telemetry = std::make_shared<colmap::MetalRuntimeTelemetry>();
    memory_sampler = std::make_unique<ResidentMemorySampler>();
  }
  try {
    RunSparseReconstructionImpl(config, result, cancellation, metal_telemetry);
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

  if (extended_semantics) {
    if (metal_telemetry != nullptr) {
      PopulateMetalRuntimeResult(config, metal_telemetry->Snapshot(), result);
    }
    result->total_seconds = std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - total_start)
                                .count();
    if (memory_sampler != nullptr) {
      result->peak_resident_memory_bytes = memory_sampler->Stop();
    }
    try {
      WriteRuntimeEvidence(config, *result);
    } catch (const std::exception& error) {
      SetFailure(COLMAPKIT_STATUS_RUNTIME_ERROR, error.what(), result);
    }
  }

  return result->status;
}

}  // namespace

struct SparseReconstructionJob {
  explicit SparseReconstructionJob(OwnedSparseReconstructionConfig owned_config,
                                   const bool use_extended_semantics)
      : config(std::move(owned_config)),
        extended_semantics(use_extended_semantics) {
    ResetResult(&result);
  }

  ~SparseReconstructionJob() {
    RequestCancel();
    Wait();
  }

  void Start() {
    worker = std::thread([this]() {
      ResetResult(&result);
      config.RefreshPointers();
      RunSparseReconstructionWithResult(
          config.config, &result, &cancellation, extended_semantics);
    });
  }

  void RequestCancel() { cancellation.RequestCancel(); }

  void Wait() {
    if (worker.joinable()) {
      worker.join();
    }
  }

  OwnedSparseReconstructionConfig config;
  bool extended_semantics = false;
  ColmapKitCancellationContext cancellation;
  ColmapKitSparseReconstructionResult result = {};
  std::thread worker;
};

struct ColmapKitSparseReconstructionJob : SparseReconstructionJob {
  using SparseReconstructionJob::SparseReconstructionJob;
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
  if (result == nullptr) {
    return COLMAPKIT_STATUS_INVALID_ARGUMENT;
  }
  ColmapKitSparseReconstructionResult internal_result = {};
  ResetResult(&internal_result);
  if (config == nullptr) {
    SetFailure(COLMAPKIT_STATUS_INVALID_ARGUMENT,
               "config is required.",
               &internal_result);
    CopyResultToCaller(internal_result, result, false);
    return internal_result.status;
  }

  if ((config->struct_size != 0 && config->struct_size < kMinimumConfigSize) ||
      HasPartialSparseConfig(*config) || HasPartialSparseResult(*result)) {
    SetFailure(COLMAPKIT_STATUS_INVALID_ARGUMENT,
               "Unsupported partial sparse reconstruction struct size.",
               &internal_result);
    CopyResultToCaller(internal_result, result, false);
    return internal_result.status;
  }

  const bool extended_semantics = UsesExtendedSparseConfig(*config);
  if (extended_semantics && result->struct_size < sizeof(*result)) {
    SetFailure(COLMAPKIT_STATUS_INVALID_ARGUMENT,
               "Extended sparse reconstruction requires the complete result "
               "structure.",
               &internal_result);
    CopyResultToCaller(internal_result, result, false);
    return internal_result.status;
  }

  const ColmapKitSparseReconstructionConfig run_config =
      extended_semantics ? *config : LegacyConfigCopy(*config);
  const ColmapKitStatus status = RunSparseReconstructionWithResult(
      run_config, &internal_result, nullptr, extended_semantics);
  CopyResultToCaller(internal_result, result, extended_semantics);
  return status;
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
  if ((config->struct_size != 0 && config->struct_size < kMinimumConfigSize) ||
      HasPartialSparseConfig(*config)) {
    return COLMAPKIT_STATUS_INVALID_ARGUMENT;
  }

  try {
    auto owned_config = OwnedSparseReconstructionConfig::CopyFrom(*config);
    const bool extended_semantics = UsesExtendedSparseConfig(*config);
    if (!extended_semantics) {
      owned_config.config.struct_size = kLegacyConfigSize;
    }
    auto new_job = std::make_unique<ColmapKitSparseReconstructionJob>(
        std::move(owned_config), extended_semantics);
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
  if (result == nullptr) {
    return COLMAPKIT_STATUS_INVALID_ARGUMENT;
  }
  if (job == nullptr) {
    ColmapKitSparseReconstructionResult internal_result = {};
    ResetResult(&internal_result);
    SetFailure(COLMAPKIT_STATUS_INVALID_ARGUMENT,
               "job is required.",
               &internal_result);
    CopyResultToCaller(internal_result, result, false);
    return internal_result.status;
  }

  job->Wait();
  if (HasPartialSparseResult(*result) ||
      (job->extended_semantics && result->struct_size < sizeof(*result))) {
    ColmapKitSparseReconstructionResult internal_result = {};
    ResetResult(&internal_result);
    SetFailure(COLMAPKIT_STATUS_INVALID_ARGUMENT,
               "Extended sparse reconstruction requires the complete result "
               "structure.",
               &internal_result);
    CopyResultToCaller(internal_result, result, false);
    return internal_result.status;
  }
  CopyResultToCaller(job->result, result, job->extended_semantics);
  return job->result.status;
}

void ColmapKitReleaseSparseReconstructionJob(
    ColmapKitSparseReconstructionJob* job) {
  delete job;
}
