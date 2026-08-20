// Copyright (c), ETH Zurich and UNC Chapel Hill.
// All rights reserved.

#include "colmap/colmapkit/colmapkit.h"
#include "colmap/colmapkit/frame_feature_extraction_internal.h"
#include "colmap/scene/camera.h"
#include "colmap/scene/database.h"
#include "colmap/scene/image.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <fcntl.h>
#include <mach/mach.h>
#include <sys/stdio.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;
using Seconds = std::chrono::duration<double>;

constexpr char kDatabaseFilename[] = "database.db";
constexpr char kReceiptFilename[] = "import-receipt.json";
constexpr uint64_t kMaximumItems = 100000;
constexpr uint64_t kMaximumBound = 1ULL << 40;

class ImportCancelledError : public std::runtime_error {
 public:
  ImportCancelledError()
      : std::runtime_error("Frame feature import cancelled.") {}
};

template <typename Value>
void CopyText(char* destination, const size_t capacity, const Value& value) {
  if (destination == nullptr || capacity == 0) return;
  const std::string text(value);
  const size_t count = std::min(capacity - 1, text.size());
  std::memcpy(destination, text.data(), count);
  destination[count] = '\0';
}

bool IsLowerHexSHA256(const std::string_view value) {
  if (value.size() != 64) return false;
  return std::all_of(value.begin(), value.end(), [](const char character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'a' && character <= 'f');
  });
}

uint64_t CheckedAdd(const uint64_t left, const uint64_t right) {
  if (right > std::numeric_limits<uint64_t>::max() - left) {
    throw std::invalid_argument("Frame feature import bound overflowed.");
  }
  return left + right;
}

uint64_t CheckedMultiply(const uint64_t left, const uint64_t right) {
  if (left != 0 && right > std::numeric_limits<uint64_t>::max() / left) {
    throw std::invalid_argument("Frame feature import bound overflowed.");
  }
  return left * right;
}

template <typename T>
void AppendLittleEndian(std::vector<uint8_t>* bytes, const T value) {
  static_assert(std::is_integral_v<T> || std::is_floating_point_v<T>);
  std::array<uint8_t, sizeof(T)> encoded{};
  std::memcpy(encoded.data(), &value, sizeof(T));
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  std::reverse(encoded.begin(), encoded.end());
#endif
  bytes->insert(bytes->end(), encoded.begin(), encoded.end());
}

void AppendString(std::vector<uint8_t>* bytes, const std::string_view value) {
  if (value.size() > std::numeric_limits<uint32_t>::max()) {
    throw std::invalid_argument("Frame feature import string is too large.");
  }
  AppendLittleEndian<uint32_t>(bytes, static_cast<uint32_t>(value.size()));
  bytes->insert(bytes->end(), value.begin(), value.end());
}

std::string HashBytes(const std::vector<uint8_t>& bytes) {
  return colmap::internal::FrameFeatureSHA256(std::string_view(
      reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path,
                                   const uint64_t maximum_bytes) {
  std::error_code error;
  const uint64_t size = std::filesystem::file_size(path, error);
  if (error || size > maximum_bytes) {
    throw std::invalid_argument(
        "Frame feature import file is missing or exceeds its bound.");
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::invalid_argument("Cannot read frame feature import file.");
  }
  if (size > std::numeric_limits<size_t>::max()) {
    throw std::invalid_argument("Frame feature import file is too large.");
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  stream.read(reinterpret_cast<char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  if (stream.gcount() != static_cast<std::streamsize>(bytes.size())) {
    throw std::invalid_argument(
        "Frame feature import file changed while it was read.");
  }
  char extra = 0;
  if (stream.read(&extra, 1)) {
    throw std::invalid_argument(
        "Frame feature import file grew while it was read.");
  }
  return bytes;
}

std::string FileSHA256(const std::filesystem::path& path,
                       const uint64_t maximum_bytes) {
  return HashBytes(ReadFileBytes(path, maximum_bytes));
}

uint64_t ResidentMemoryBytes() {
#if defined(__APPLE__)
  mach_task_basic_info_data_t info{};
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

std::string JsonEscape(const std::string_view text) {
  std::ostringstream output;
  for (const unsigned char character : text) {
    switch (character) {
      case '"':
        output << "\\\"";
        break;
      case '\\':
        output << "\\\\";
        break;
      case '\b':
        output << "\\b";
        break;
      case '\f':
        output << "\\f";
        break;
      case '\n':
        output << "\\n";
        break;
      case '\r':
        output << "\\r";
        break;
      case '\t':
        output << "\\t";
        break;
      default:
        if (character < 0x20) {
          output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                 << static_cast<int>(character) << std::dec;
        } else {
          output << static_cast<char>(character);
        }
    }
  }
  return output.str();
}

void ValidateImageName(const std::string& name) {
  const std::filesystem::path path(name);
  if (name.empty() || path.is_absolute() || path.has_root_path() ||
      name.find('\\') != std::string::npos) {
    throw std::invalid_argument(
        "Sealed image names must be non-empty relative POSIX paths.");
  }
  for (const auto& component : path) {
    if (component == "." || component == ".." || component.empty()) {
      throw std::invalid_argument(
          "Sealed image names cannot contain empty, dot, or parent "
          "components.");
    }
  }
}

void ThrowIfCancelled(const std::atomic<bool>& cancelled) {
  if (cancelled.load(std::memory_order_acquire)) {
    throw ImportCancelledError();
  }
}

ColmapKitStatus StatusForException(const std::exception& exception) {
  if (dynamic_cast<const ImportCancelledError*>(&exception) != nullptr) {
    return COLMAPKIT_STATUS_CANCELLED;
  }
  if (dynamic_cast<const std::domain_error*>(&exception) != nullptr) {
    return COLMAPKIT_STATUS_UNSUPPORTED;
  }
  if (dynamic_cast<const std::invalid_argument*>(&exception) != nullptr) {
    return COLMAPKIT_STATUS_INVALID_ARGUMENT;
  }
  return COLMAPKIT_STATUS_RUNTIME_ERROR;
}

void FillError(ColmapKitFrameFeatureErrorV1* error,
               const ColmapKitStatus status,
               const std::string& message) {
  if (error == nullptr ||
      error->struct_size <
          offsetof(ColmapKitFrameFeatureErrorV1, message) + 1 ||
      error->abi_version != COLMAPKIT_FRAME_FEATURE_IMPORT_ABI_VERSION_V1) {
    return;
  }
  const uint32_t caller_size = error->struct_size;
  ColmapKitFrameFeatureErrorV1 local{};
  local.struct_size = caller_size;
  local.abi_version = COLMAPKIT_FRAME_FEATURE_IMPORT_ABI_VERSION_V1;
  local.error_code = static_cast<uint32_t>(status);
  CopyText(local.message, sizeof(local.message), message);
  std::memcpy(error, &local, std::min<size_t>(caller_size, sizeof(local)));
  error->struct_size = caller_size;
}

void InitializeResult(ColmapKitFrameFeatureImportResultV1* result) {
  *result = {};
  result->struct_size = sizeof(*result);
  result->abi_version = COLMAPKIT_FRAME_FEATURE_IMPORT_ABI_VERSION_V1;
  result->no_fallback_satisfied = 1;
  result->effective_worker_count = 1;
}

bool PrepareResult(ColmapKitFrameFeatureImportResultV1* result) {
  return result != nullptr &&
         result->struct_size >=
             offsetof(ColmapKitFrameFeatureImportResultV1, mode) &&
         result->abi_version == COLMAPKIT_FRAME_FEATURE_IMPORT_ABI_VERSION_V1;
}

void CopyResult(const ColmapKitFrameFeatureImportResultV1& source,
                ColmapKitFrameFeatureImportResultV1* destination) {
  const uint32_t caller_size = destination->struct_size;
  std::memcpy(
      destination, &source, std::min<size_t>(caller_size, sizeof(source)));
  destination->struct_size = caller_size;
}

struct OwnedImportItem {
  uint64_t stable_frame_id = 0;
  uint64_t frame_revision = 0;
  std::string image_name;
  std::filesystem::path image_path;
  std::filesystem::path artifact_path;
  std::string expected_image_sha256;
  std::string expected_metadata_sha256;
  std::string expected_artifact_sha256;
};

struct OwnedImportConfig {
  uint32_t mode = 0;
  std::vector<OwnedImportItem> items;
  ColmapKitFrameFeatureExtractorConfigV1 extractor_config{};
  uint64_t max_total_artifact_bytes = 0;
  uint64_t max_total_image_bytes = 0;
  uint64_t max_total_features = 0;
  uint64_t max_base_database_bytes = 0;
  std::filesystem::path output_bundle_path;
  std::filesystem::path base_database_path;
  ColmapKitFrameFeatureImportProgressCallbackV1 progress_callback = nullptr;
  void* progress_user_data = nullptr;
  uint64_t admitted_memory_bytes = 0;
};

struct LoadedImportItem {
  OwnedImportItem input;
  colmap::internal::FrameFeatureArtifactDataV1 artifact;
  uint32_t camera_id = 0;
  uint32_t image_id = 0;
};

void EmitProgress(const OwnedImportConfig& config,
                  const uint32_t stage,
                  const Clock::time_point start,
                  const uint64_t current,
                  const uint64_t total,
                  const double fraction,
                  const char* message,
                  const char* detail = "") {
  if (config.progress_callback == nullptr) return;
  ColmapKitFrameFeatureImportProgressEventV1 event{};
  event.struct_size = sizeof(event);
  event.abi_version = COLMAPKIT_FRAME_FEATURE_IMPORT_ABI_VERSION_V1;
  event.stage = stage;
  event.fraction = fraction;
  event.elapsed_seconds = Seconds(Clock::now() - start).count();
  event.current = current;
  event.total = total;
  event.admitted_memory_bytes = config.admitted_memory_bytes;
  event.message = message;
  event.detail = detail;
  try {
    config.progress_callback(&event, config.progress_user_data);
  } catch (...) {
    // Foreign callbacks cannot throw across the C ABI.
  }
}

std::filesystem::path UniqueTemporaryBundle(
    const std::filesystem::path& output_path) {
  static std::atomic<uint64_t> sequence{0};
  for (int attempt = 0; attempt < 100; ++attempt) {
    const std::filesystem::path candidate =
        output_path.string() + ".tmp." +
        std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    if (!std::filesystem::exists(candidate)) return candidate;
  }
  throw std::runtime_error("Cannot allocate a seal import staging directory.");
}

void PublishDirectoryExclusive(const std::filesystem::path& source,
                               const std::filesystem::path& destination) {
#if defined(__APPLE__)
  if (renameatx_np(AT_FDCWD,
                   source.c_str(),
                   AT_FDCWD,
                   destination.c_str(),
                   RENAME_EXCL) == 0) {
    return;
  }
#elif defined(__linux__)
  if (syscall(SYS_renameat2,
              AT_FDCWD,
              source.c_str(),
              AT_FDCWD,
              destination.c_str(),
              RENAME_NOREPLACE) == 0) {
    return;
  }
#else
  throw std::domain_error(
      "Atomic no-overwrite seal bundle publication is unsupported.");
#endif
  if (errno == EEXIST || errno == ENOTEMPTY) {
    throw std::invalid_argument(
        "Seal import output appeared before atomic publication.");
  }
  throw std::runtime_error(
      "Cannot atomically publish the seal import bundle without overwrite.");
}

bool DatabaseIsEmpty(const colmap::Database& database) {
  return database.NumRigs() == 0 && database.NumCameras() == 0 &&
         database.NumFrames() == 0 && database.NumImages() == 0 &&
         database.NumPosePriors() == 0 && database.NumKeypoints() == 0 &&
         database.NumDescriptors() == 0 &&
         database.NumMatchedImagePairs() == 0 &&
         database.NumVerifiedImagePairs() == 0;
}

colmap::CameraModelId CameraModelId(const uint32_t model) {
  switch (model) {
    case COLMAPKIT_CAMERA_MODEL_V2_PINHOLE:
      return colmap::CameraModelId::kPinhole;
    case COLMAPKIT_CAMERA_MODEL_V2_SIMPLE_PINHOLE:
      return colmap::CameraModelId::kSimplePinhole;
    case COLMAPKIT_CAMERA_MODEL_V2_OPENCV:
      return colmap::CameraModelId::kOpenCV;
    default:
      throw std::invalid_argument("Unsupported sealed camera model.");
  }
}

colmap::Camera MakeCamera(const ColmapKitFrameMetadataV1& metadata,
                          const colmap::camera_t camera_id) {
  colmap::Camera camera;
  camera.camera_id = camera_id;
  camera.model_id = CameraModelId(metadata.camera_model);
  camera.width = metadata.encoded_width;
  camera.height = metadata.encoded_height;
  camera.params.assign(metadata.camera_params,
                       metadata.camera_params + metadata.num_camera_params);
  camera.has_prior_focal_length = true;
  if (!camera.VerifyParams()) {
    throw std::invalid_argument("Sealed camera parameters are invalid.");
  }
  return camera;
}

std::string SealedSetSHA256(const std::vector<LoadedImportItem>& items,
                            const std::string_view source_identity,
                            const std::string_view profile_sha256,
                            const std::string_view base_database_sha256) {
  std::vector<uint8_t> bytes;
  AppendLittleEndian<uint32_t>(
      &bytes, COLMAPKIT_FRAME_FEATURE_IMPORT_RECEIPT_SCHEMA_VERSION_V1);
  AppendString(&bytes, source_identity);
  AppendString(&bytes, profile_sha256);
  AppendString(&bytes, base_database_sha256);
  AppendLittleEndian<uint64_t>(&bytes, items.size());
  for (const LoadedImportItem& item : items) {
    AppendLittleEndian<uint64_t>(&bytes, item.input.stable_frame_id);
    AppendLittleEndian<uint64_t>(&bytes, item.input.frame_revision);
    AppendString(&bytes, item.input.image_name);
    AppendString(&bytes, item.artifact.result.image_sha256);
    AppendString(&bytes, item.artifact.result.metadata_sha256);
    AppendString(&bytes, item.artifact.result.artifact_sha256);
    AppendLittleEndian<uint64_t>(&bytes, item.artifact.result.feature_count);
  }
  return HashBytes(bytes);
}

std::string ReceiptJSON(const std::vector<LoadedImportItem>& items,
                        const uint32_t mode,
                        const uint64_t num_cameras,
                        const uint64_t num_keypoints,
                        const uint64_t descriptor_bytes,
                        const std::string_view source_identity,
                        const std::string_view profile_sha256,
                        const std::string_view base_database_sha256,
                        const std::string_view sealed_set_sha256,
                        const std::string_view database_sha256) {
  std::ostringstream output;
  output << "{\n"
         << "  \"schemaVersion\": "
         << COLMAPKIT_FRAME_FEATURE_IMPORT_RECEIPT_SCHEMA_VERSION_V1 << ",\n"
         << "  \"abiVersion\": "
         << COLMAPKIT_FRAME_FEATURE_IMPORT_ABI_VERSION_V1 << ",\n"
         << "  \"mode\": " << mode << ",\n"
         << "  \"engineSourceIdentity\": \"" << JsonEscape(source_identity)
         << "\",\n"
         << "  \"backend\": \"cpu\",\n"
         << "  \"profileSHA256\": \"" << profile_sha256 << "\",\n"
         << "  \"baseDatabaseSHA256\": \"" << base_database_sha256 << "\",\n"
         << "  \"sealedSetSHA256\": \"" << sealed_set_sha256 << "\",\n"
         << "  \"database\": {\"file\": \"" << kDatabaseFilename
         << "\", \"sha256\": \"" << database_sha256
         << "\", \"cameras\": " << num_cameras
         << ", \"images\": " << items.size()
         << ", \"keypoints\": " << num_keypoints
         << ", \"descriptorBytes\": " << descriptor_bytes << "},\n"
         << "  \"items\": [\n";
  for (size_t index = 0; index < items.size(); ++index) {
    const LoadedImportItem& item = items[index];
    output << "    {\"orderIndex\": " << index
           << ", \"stableFrameID\": " << item.input.stable_frame_id
           << ", \"frameRevision\": " << item.input.frame_revision
           << ", \"cameraID\": " << item.camera_id
           << ", \"imageID\": " << item.image_id << ", \"imageName\": \""
           << JsonEscape(item.input.image_name) << "\", \"imageSHA256\": \""
           << item.artifact.result.image_sha256 << "\", \"metadataSHA256\": \""
           << item.artifact.result.metadata_sha256
           << "\", \"artifactSHA256\": \""
           << item.artifact.result.artifact_sha256
           << "\", \"features\": " << item.artifact.result.feature_count
           << ", \"descriptorBytes\": " << item.artifact.result.descriptor_bytes
           << "}" << (index + 1 == items.size() ? "\n" : ",\n");
  }
  output << "  ]\n}\n";
  return output.str();
}

void WriteTextFile(const std::filesystem::path& path,
                   const std::string_view text) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream)
    throw std::runtime_error("Cannot create the seal import receipt.");
  stream.write(text.data(), text.size());
  stream.close();
  if (!stream)
    throw std::runtime_error("Cannot finish the seal import receipt.");
}

void ValidateBaseSidecars(const std::filesystem::path& path) {
  for (const char* suffix : {"-journal", "-wal", "-shm"}) {
    if (std::filesystem::exists(path.string() + suffix)) {
      throw std::invalid_argument(
          "Declared base database must be closed and sidecar-free.");
    }
  }
}

void RevalidateSealedInputs(const OwnedImportConfig& config,
                            const std::atomic<bool>& cancelled) {
  for (const OwnedImportItem& item : config.items) {
    ThrowIfCancelled(cancelled);
    if (FileSHA256(item.image_path, config.max_total_image_bytes) !=
            item.expected_image_sha256 ||
        FileSHA256(item.artifact_path, config.max_total_artifact_bytes) !=
            item.expected_artifact_sha256) {
      throw std::invalid_argument(
          "Sealed image or feature artifact changed during import.");
    }
  }
}

}  // namespace

struct ColmapKitFrameFeatureImportJobV1 {
  OwnedImportConfig config;
  ColmapKitFrameFeatureExtractorV1* extractor = nullptr;
  std::atomic<bool> cancelled{false};
  ColmapKitFrameFeatureImportResultV1 result{};
  std::thread worker;
};

namespace {

void RunImport(ColmapKitFrameFeatureImportJobV1* job) {
  InitializeResult(&job->result);
  job->result.mode = job->config.mode;
  job->result.admitted_memory_bytes = job->config.admitted_memory_bytes;
  const auto start = Clock::now();
  uint64_t peak_resident_memory = ResidentMemoryBytes();
  std::filesystem::path staging_path;
  bool published = false;
  try {
    const auto validation_start = Clock::now();
    std::vector<LoadedImportItem> items;
    items.reserve(job->config.items.size());
    uint64_t actual_artifact_bytes = 0;
    uint64_t actual_image_bytes = 0;
    uint64_t total_features = 0;
    uint64_t total_descriptor_bytes = 0;
    std::string profile_sha256;
    std::string source_identity;

    for (size_t index = 0; index < job->config.items.size(); ++index) {
      ThrowIfCancelled(job->cancelled);
      const OwnedImportItem& input = job->config.items[index];
      const uint64_t artifact_size =
          std::filesystem::file_size(input.artifact_path);
      const uint64_t image_size = std::filesystem::file_size(input.image_path);
      actual_artifact_bytes = CheckedAdd(actual_artifact_bytes, artifact_size);
      actual_image_bytes = CheckedAdd(actual_image_bytes, image_size);
      if (actual_artifact_bytes > job->config.max_total_artifact_bytes ||
          actual_image_bytes > job->config.max_total_image_bytes) {
        throw std::invalid_argument(
            "Sealed inputs changed beyond their admitted byte bounds.");
      }
      if (FileSHA256(input.image_path, job->config.max_total_image_bytes) !=
          input.expected_image_sha256) {
        throw std::invalid_argument(
            "Sealed image bytes are stale or do not match extraction.");
      }

      ColmapKitFrameFeatureArtifactExpectationV1 expectation{};
      expectation.struct_size = sizeof(expectation);
      expectation.abi_version = COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1;
      expectation.stable_frame_id = input.stable_frame_id;
      expectation.frame_revision = input.frame_revision;
      expectation.expected_image_sha256 = input.expected_image_sha256.c_str();
      expectation.expected_metadata_sha256 =
          input.expected_metadata_sha256.c_str();
      const std::string artifact_path = input.artifact_path.string();
      expectation.artifact_path = artifact_path.c_str();

      LoadedImportItem loaded;
      loaded.input = input;
      loaded.artifact = colmap::internal::LoadFrameFeatureArtifactV1(
          job->extractor, expectation);
      if (loaded.artifact.result.artifact_sha256 !=
          input.expected_artifact_sha256) {
        throw std::invalid_argument(
            "Sealed artifact bytes do not match the declared artifact hash.");
      }
      if (index == 0) {
        profile_sha256 = loaded.artifact.result.profile_sha256;
        source_identity = loaded.artifact.result.source_identity;
      } else if (profile_sha256 != loaded.artifact.result.profile_sha256 ||
                 source_identity != loaded.artifact.result.source_identity ||
                 loaded.artifact.result.actual_backend !=
                     COLMAPKIT_FRAME_FEATURE_BACKEND_V1_CPU) {
        throw std::invalid_argument(
            "Sealed artifacts contain mixed provenance or backends.");
      }
      total_features =
          CheckedAdd(total_features, loaded.artifact.result.feature_count);
      total_descriptor_bytes = CheckedAdd(
          total_descriptor_bytes, loaded.artifact.result.descriptor_bytes);
      if (total_features > job->config.max_total_features) {
        throw std::invalid_argument(
            "Sealed artifacts exceed the admitted feature bound.");
      }
      items.push_back(std::move(loaded));
      peak_resident_memory =
          std::max(peak_resident_memory, ResidentMemoryBytes());
      EmitProgress(job->config,
                   COLMAPKIT_FRAME_FEATURE_IMPORT_PROGRESS_V1_VALIDATING,
                   start,
                   index + 1,
                   job->config.items.size(),
                   0.45 * (index + 1) / job->config.items.size(),
                   "Validated sealed frame feature artifact.",
                   input.image_name.c_str());
    }
    job->result.validation_seconds =
        Seconds(Clock::now() - validation_start).count();
    CopyText(job->result.profile_sha256,
             sizeof(job->result.profile_sha256),
             profile_sha256);
    CopyText(job->result.source_identity,
             sizeof(job->result.source_identity),
             source_identity);

    ThrowIfCancelled(job->cancelled);
    EmitProgress(job->config,
                 COLMAPKIT_FRAME_FEATURE_IMPORT_PROGRESS_V1_STAGING,
                 start,
                 0,
                 items.size(),
                 0.5,
                 "Preparing private seal import staging bundle.");
    staging_path = UniqueTemporaryBundle(job->config.output_bundle_path);
    std::filesystem::create_directory(staging_path);
    const std::filesystem::path database_path =
        staging_path / kDatabaseFilename;
    std::string base_database_sha256 = "none";
    if (job->config.mode ==
        COLMAPKIT_FRAME_FEATURE_IMPORT_MODE_V1_COPY_EMPTY_BASE) {
      ValidateBaseSidecars(job->config.base_database_path);
      base_database_sha256 = FileSHA256(job->config.base_database_path,
                                        job->config.max_base_database_bytes);
      std::filesystem::copy_file(job->config.base_database_path, database_path);
      if (FileSHA256(database_path, job->config.max_base_database_bytes) !=
          base_database_sha256) {
        throw std::invalid_argument(
            "Declared base database changed while it was copied.");
      }
    }

    const auto database_start = Clock::now();
    auto database = colmap::Database::Open(database_path);
    if (!DatabaseIsEmpty(*database)) {
      throw std::invalid_argument(
          "Declared base database contains incompatible existing rows.");
    }

    std::map<std::string, colmap::camera_t> camera_ids;
    {
      colmap::DatabaseTransaction transaction(database.get());
      for (size_t index = 0; index < items.size(); ++index) {
        ThrowIfCancelled(job->cancelled);
        LoadedImportItem& item = items[index];
        const std::string metadata_sha256 =
            item.artifact.result.metadata_sha256;
        auto camera = camera_ids.find(metadata_sha256);
        if (camera == camera_ids.end()) {
          const colmap::camera_t camera_id =
              static_cast<colmap::camera_t>(camera_ids.size() + 1);
          database->WriteCamera(MakeCamera(item.artifact.metadata, camera_id),
                                true);
          camera = camera_ids.emplace(metadata_sha256, camera_id).first;
        }
        item.camera_id = camera->second;
        item.image_id = static_cast<uint32_t>(index + 1);
        colmap::Image image;
        image.SetImageId(item.image_id);
        image.SetCameraId(item.camera_id);
        image.SetName(item.input.image_name);
        database->WriteImage(image, true);
        database->WriteKeypoints(item.image_id, item.artifact.keypoints);
        database->WriteDescriptors(item.image_id, item.artifact.descriptors);
        EmitProgress(job->config,
                     COLMAPKIT_FRAME_FEATURE_IMPORT_PROGRESS_V1_IMPORTING,
                     start,
                     index + 1,
                     items.size(),
                     0.5 + 0.3 * (index + 1) / items.size(),
                     "Imported sealed frame features into staging database.",
                     item.input.image_name.c_str());
        ThrowIfCancelled(job->cancelled);
      }
    }
    if (database->NumCameras() != camera_ids.size() ||
        database->NumImages() != items.size() ||
        database->NumKeypoints() != total_features ||
        database->NumDescriptors() != total_features) {
      throw std::runtime_error(
          "Staged database row counts do not match the sealed set.");
    }
    database->Close();
    database.reset();
    job->result.database_seconds =
        Seconds(Clock::now() - database_start).count();

    ThrowIfCancelled(job->cancelled);
    const auto receipt_start = Clock::now();
    EmitProgress(job->config,
                 COLMAPKIT_FRAME_FEATURE_IMPORT_PROGRESS_V1_RECEIPT,
                 start,
                 items.size(),
                 items.size(),
                 0.85,
                 "Writing transactional seal import receipt.");
    const uint64_t maximum_database_bytes = CheckedAdd(
        job->config.max_base_database_bytes,
        CheckedAdd(actual_artifact_bytes, 64ULL * 1024ULL * 1024ULL));
    const std::string database_sha256 =
        FileSHA256(database_path, maximum_database_bytes);
    const std::string sealed_set_sha256 = SealedSetSHA256(
        items, source_identity, profile_sha256, base_database_sha256);
    const std::string receipt = ReceiptJSON(items,
                                            job->config.mode,
                                            camera_ids.size(),
                                            total_features,
                                            total_descriptor_bytes,
                                            source_identity,
                                            profile_sha256,
                                            base_database_sha256,
                                            sealed_set_sha256,
                                            database_sha256);
    const std::filesystem::path receipt_path = staging_path / kReceiptFilename;
    WriteTextFile(receipt_path, receipt);
    const std::string receipt_sha256 =
        colmap::internal::FrameFeatureSHA256(receipt);
    job->result.receipt_seconds = Seconds(Clock::now() - receipt_start).count();

    job->result.imported_items = items.size();
    job->result.imported_cameras = camera_ids.size();
    job->result.imported_keypoints = total_features;
    job->result.imported_descriptor_bytes = total_descriptor_bytes;
    CopyText(job->result.sealed_set_sha256,
             sizeof(job->result.sealed_set_sha256),
             sealed_set_sha256);
    CopyText(job->result.database_sha256,
             sizeof(job->result.database_sha256),
             database_sha256);
    CopyText(job->result.receipt_sha256,
             sizeof(job->result.receipt_sha256),
             receipt_sha256);

    ThrowIfCancelled(job->cancelled);
    EmitProgress(job->config,
                 COLMAPKIT_FRAME_FEATURE_IMPORT_PROGRESS_V1_PUBLISHING,
                 start,
                 items.size(),
                 items.size(),
                 0.95,
                 "Atomically publishing sealed feature database bundle.");
    RevalidateSealedInputs(job->config, job->cancelled);
    PublishDirectoryExclusive(staging_path, job->config.output_bundle_path);
    published = true;
    job->result.status = COLMAPKIT_STATUS_OK;
    CopyText(job->result.message,
             sizeof(job->result.message),
             "Sealed frame feature database bundle committed atomically.");
    EmitProgress(job->config,
                 COLMAPKIT_FRAME_FEATURE_IMPORT_PROGRESS_V1_FINISHED,
                 start,
                 items.size(),
                 items.size(),
                 1.0,
                 "Sealed frame feature database bundle committed atomically.");
  } catch (const std::exception& exception) {
    job->result.status = StatusForException(exception);
    CopyText(
        job->result.message, sizeof(job->result.message), exception.what());
    EmitProgress(job->config,
                 job->result.status == COLMAPKIT_STATUS_CANCELLED
                     ? COLMAPKIT_FRAME_FEATURE_IMPORT_PROGRESS_V1_CANCELLED
                     : COLMAPKIT_FRAME_FEATURE_IMPORT_PROGRESS_V1_FAILED,
                 start,
                 0,
                 job->config.items.size(),
                 1.0,
                 exception.what());
  }
  if (!published && !staging_path.empty()) {
    std::error_code ignored;
    std::filesystem::remove_all(staging_path, ignored);
  }
  job->result.total_seconds = Seconds(Clock::now() - start).count();
  job->result.peak_resident_memory_bytes =
      std::max(peak_resident_memory, ResidentMemoryBytes());
}

void ValidateImportConfig(const ColmapKitFrameFeatureImportConfigV1& config) {
  if (config.struct_size < sizeof(config) ||
      config.abi_version != COLMAPKIT_FRAME_FEATURE_IMPORT_ABI_VERSION_V1 ||
      config.worker_count != 1 || config.items == nullptr ||
      config.num_items == 0 || config.num_items > kMaximumItems ||
      config.num_items >= colmap::kMaxNumImages ||
      config.output_bundle_path == nullptr ||
      config.max_total_artifact_bytes == 0 ||
      config.max_total_image_bytes == 0 || config.max_total_features == 0 ||
      config.max_total_artifact_bytes > kMaximumBound ||
      config.max_total_image_bytes > kMaximumBound ||
      config.max_total_features > kMaximumBound ||
      config.max_base_database_bytes > kMaximumBound) {
    throw std::invalid_argument("Invalid frame feature import ABI or bounds.");
  }
  if (config.mode != COLMAPKIT_FRAME_FEATURE_IMPORT_MODE_V1_CREATE_NEW &&
      config.mode != COLMAPKIT_FRAME_FEATURE_IMPORT_MODE_V1_COPY_EMPTY_BASE) {
    throw std::invalid_argument("Unknown frame feature import mode.");
  }
  if (config.mode == COLMAPKIT_FRAME_FEATURE_IMPORT_MODE_V1_COPY_EMPTY_BASE &&
      config.max_base_database_bytes == 0) {
    throw std::invalid_argument(
        "Copy-empty-base import requires a nonzero database byte bound.");
  }
  if (std::any_of(std::begin(config.reserved),
                  std::end(config.reserved),
                  [](const uint32_t value) { return value != 0; })) {
    throw std::invalid_argument(
        "Reserved frame feature import fields must be zero.");
  }
}

}  // namespace

extern "C" {

ColmapKitStatus ColmapKitStartFrameFeatureImportV1(
    const ColmapKitFrameFeatureImportConfigV1* config,
    ColmapKitFrameFeatureImportJobV1** job,
    ColmapKitFrameFeatureErrorV1* error) {
  if (job != nullptr) *job = nullptr;
  if (config == nullptr || job == nullptr) {
    FillError(error,
              COLMAPKIT_STATUS_INVALID_ARGUMENT,
              "Frame feature import config and job output are required.");
    return COLMAPKIT_STATUS_INVALID_ARGUMENT;
  }
  try {
    ValidateImportConfig(*config);
    auto owned = std::make_unique<ColmapKitFrameFeatureImportJobV1>();
    owned->config.mode = config->mode;
    owned->config.extractor_config = config->extractor_config;
    owned->config.max_total_artifact_bytes = config->max_total_artifact_bytes;
    owned->config.max_total_image_bytes = config->max_total_image_bytes;
    owned->config.max_total_features = config->max_total_features;
    owned->config.max_base_database_bytes = config->max_base_database_bytes;
    owned->config.output_bundle_path = config->output_bundle_path;
    owned->config.progress_callback = config->progress_callback;
    owned->config.progress_user_data = config->progress_user_data;
    if (owned->config.output_bundle_path.empty() ||
        !owned->config.output_bundle_path.is_absolute() ||
        !std::filesystem::is_directory(
            owned->config.output_bundle_path.parent_path()) ||
        owned->config.output_bundle_path.extension() != ".ckseal" ||
        std::filesystem::exists(owned->config.output_bundle_path)) {
      throw std::invalid_argument(
          "Seal import output must be a nonexistent .ckseal path.");
    }
    if (config->mode == COLMAPKIT_FRAME_FEATURE_IMPORT_MODE_V1_CREATE_NEW) {
      if (config->base_database_path != nullptr &&
          config->base_database_path[0] != '\0') {
        throw std::invalid_argument(
            "Create-new seal import cannot declare a base database.");
      }
    } else {
      if (config->base_database_path == nullptr ||
          config->base_database_path[0] == '\0') {
        throw std::invalid_argument(
            "Copy-empty-base seal import requires a base database.");
      }
      owned->config.base_database_path = config->base_database_path;
      if (!owned->config.base_database_path.is_absolute() ||
          !std::filesystem::is_regular_file(owned->config.base_database_path) ||
          std::filesystem::file_size(owned->config.base_database_path) >
              owned->config.max_base_database_bytes) {
        throw std::invalid_argument(
            "Declared base database is missing or exceeds its bound.");
      }
      ValidateBaseSidecars(owned->config.base_database_path);
    }

    std::set<std::string> image_names;
    std::set<uint64_t> stable_frame_ids;
    std::set<std::string> artifact_paths;
    uint64_t artifact_bytes = 0;
    uint64_t image_bytes = 0;
    owned->config.items.reserve(config->num_items);
    for (uint64_t index = 0; index < config->num_items; ++index) {
      const ColmapKitFrameFeatureImportItemV1& item = config->items[index];
      if (item.struct_size < sizeof(item) ||
          item.abi_version != COLMAPKIT_FRAME_FEATURE_IMPORT_ABI_VERSION_V1 ||
          item.image_name == nullptr || item.image_path == nullptr ||
          item.artifact_path == nullptr ||
          item.expected_image_sha256 == nullptr ||
          item.expected_metadata_sha256 == nullptr ||
          item.expected_artifact_sha256 == nullptr ||
          std::any_of(
              std::begin(item.reserved),
              std::end(item.reserved),
              [](const uint32_t value) { return value != 0; })) {
        throw std::invalid_argument("Invalid sealed import item ABI.");
      }
      OwnedImportItem copy;
      copy.stable_frame_id = item.stable_frame_id;
      copy.frame_revision = item.frame_revision;
      copy.image_name = item.image_name;
      copy.image_path = item.image_path;
      copy.artifact_path = item.artifact_path;
      copy.expected_image_sha256 = item.expected_image_sha256;
      copy.expected_metadata_sha256 = item.expected_metadata_sha256;
      copy.expected_artifact_sha256 = item.expected_artifact_sha256;
      ValidateImageName(copy.image_name);
      if (!IsLowerHexSHA256(copy.expected_image_sha256) ||
          !IsLowerHexSHA256(copy.expected_metadata_sha256) ||
          !IsLowerHexSHA256(copy.expected_artifact_sha256) ||
          !copy.image_path.is_absolute() || !copy.artifact_path.is_absolute() ||
          !std::filesystem::is_regular_file(copy.image_path) ||
          !std::filesystem::is_regular_file(copy.artifact_path) ||
          !image_names.insert(copy.image_name).second ||
          !stable_frame_ids.insert(copy.stable_frame_id).second ||
          !artifact_paths.insert(copy.artifact_path.string()).second) {
        throw std::invalid_argument(
            "Sealed import items are missing, duplicated, or invalid.");
      }
      artifact_bytes = CheckedAdd(
          artifact_bytes, std::filesystem::file_size(copy.artifact_path));
      image_bytes =
          CheckedAdd(image_bytes, std::filesystem::file_size(copy.image_path));
      owned->config.items.push_back(std::move(copy));
    }
    if (artifact_bytes > owned->config.max_total_artifact_bytes ||
        image_bytes > owned->config.max_total_image_bytes) {
      throw std::invalid_argument(
          "Sealed import inputs exceed their configured byte bounds.");
    }
    const uint64_t feature_memory =
        CheckedMultiply(owned->config.max_total_features,
                        sizeof(colmap::FeatureKeypoint) + 128);
    owned->config.admitted_memory_bytes =
        CheckedAdd(CheckedAdd(artifact_bytes, image_bytes), feature_memory);

    ColmapKitFrameFeatureErrorV1 extractor_error{};
    extractor_error.struct_size = sizeof(extractor_error);
    extractor_error.abi_version = COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1;
    const ColmapKitStatus extractor_status =
        ColmapKitCreateFrameFeatureExtractorV1(&owned->config.extractor_config,
                                               &owned->extractor,
                                               &extractor_error);
    if (extractor_status != COLMAPKIT_STATUS_OK) {
      FillError(error, extractor_status, extractor_error.message);
      return extractor_status;
    }
    auto* raw = owned.get();
    try {
      owned->worker = std::thread([raw]() { RunImport(raw); });
    } catch (...) {
      ColmapKitReleaseFrameFeatureExtractorV1(owned->extractor);
      owned->extractor = nullptr;
      throw;
    }
    *job = owned.release();
    FillError(error, COLMAPKIT_STATUS_OK, "Frame feature import job started.");
    return COLMAPKIT_STATUS_OK;
  } catch (const std::exception& exception) {
    const ColmapKitStatus status = StatusForException(exception);
    FillError(error, status, exception.what());
    return status;
  }
}

ColmapKitStatus ColmapKitCancelFrameFeatureImportV1(
    ColmapKitFrameFeatureImportJobV1* job,
    ColmapKitFrameFeatureErrorV1* error) {
  if (job == nullptr) {
    FillError(error,
              COLMAPKIT_STATUS_INVALID_ARGUMENT,
              "Frame feature import job is required.");
    return COLMAPKIT_STATUS_INVALID_ARGUMENT;
  }
  job->cancelled.store(true, std::memory_order_release);
  FillError(error,
            COLMAPKIT_STATUS_OK,
            "Frame feature import cancellation requested.");
  return COLMAPKIT_STATUS_OK;
}

ColmapKitStatus ColmapKitWaitFrameFeatureImportV1(
    ColmapKitFrameFeatureImportJobV1* job,
    ColmapKitFrameFeatureImportResultV1* result) {
  if (job == nullptr || !PrepareResult(result)) {
    return COLMAPKIT_STATUS_INVALID_ARGUMENT;
  }
  if (job->worker.joinable()) job->worker.join();
  CopyResult(job->result, result);
  return static_cast<ColmapKitStatus>(job->result.status);
}

void ColmapKitReleaseFrameFeatureImportJobV1(
    ColmapKitFrameFeatureImportJobV1* job) {
  if (job == nullptr) return;
  if (job->worker.joinable()) {
    job->cancelled.store(true, std::memory_order_release);
    job->worker.join();
  }
  ColmapKitReleaseFrameFeatureExtractorV1(job->extractor);
  delete job;
}

}  // extern "C"
