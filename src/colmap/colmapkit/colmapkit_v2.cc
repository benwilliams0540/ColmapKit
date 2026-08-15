// Copyright (c), ETH Zurich and UNC Chapel Hill.
// All rights reserved.

#include "colmap/colmapkit/colmapkit.h"

#include "colmap/controllers/feature_extraction.h"
#include "colmap/controllers/feature_matching.h"
#include "colmap/controllers/image_reader.h"
#include "colmap/controllers/pairing.h"
#include "colmap/estimators/bundle_adjustment.h"
#include "colmap/estimators/bundle_adjustment_ceres.h"
#include "colmap/feature/sift.h"
#include "colmap/geometry/triangulation.h"
#include "colmap/scene/database_cache.h"
#include "colmap/scene/database_sqlite.h"
#include "colmap/scene/projection.h"
#include "colmap/scene/reconstruction.h"
#include "colmap/sensor/bitmap.h"
#include "colmap/sensor/models.h"
#include "colmap/sfm/incremental_triangulator.h"
#include "colmap/sfm/observation_manager.h"
#include "colmap/util/file.h"
#include "colmap/util/version.h"

#include <Eigen/Core>
#include <Eigen/Eigenvalues>
#include <Eigen/Geometry>

#include <ceres/autodiff_cost_function.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <jpeglib.h>

namespace {

using Clock = std::chrono::steady_clock;
using Seconds = std::chrono::duration<double>;

constexpr uint32_t kTrackedImageMinimumSize =
    offsetof(ColmapKitTrackedImageV2, image_path) + sizeof(const char*);
constexpr uint32_t kTrackedConfigMinimumSize =
    offsetof(ColmapKitTrackedPoseConfigV2, evidence_path) + sizeof(const char*);
constexpr uint32_t kPriorConfigMinimumSize =
    offsetof(ColmapKitRGBPriorConfigV2, evidence_path) + sizeof(const char*);
constexpr uint32_t kResultMinimumSize =
    offsetof(ColmapKitTrackedPoseResultV2, status) + sizeof(uint32_t);
constexpr uint32_t kPriorResultMinimumSize =
    offsetof(ColmapKitRGBPriorResultV2, status) + sizeof(uint32_t);
constexpr double kRigidTolerance = 1e-5;
constexpr double kAnchorTolerance = 1e-10;
constexpr char kReleaseVersion[] = "0.3.0-dev";

class CancelledError : public std::runtime_error {
 public:
  CancelledError() : std::runtime_error("ColmapKit V2 operation cancelled.") {}
};

struct Cancellation {
  std::atomic<bool> requested{false};
  std::mutex mutex;
  colmap::Thread* active_thread = nullptr;

  void Cancel() {
    requested.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(mutex);
    if (active_thread != nullptr) active_thread->Stop();
  }
  void ThrowIfRequested() const {
    if (requested.load(std::memory_order_acquire)) throw CancelledError();
  }
  void RunThread(colmap::Thread& thread) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      active_thread = &thread;
      if (requested.load(std::memory_order_acquire)) thread.Stop();
    }
    thread.Start();
    thread.Wait();
    {
      std::lock_guard<std::mutex> lock(mutex);
      active_thread = nullptr;
    }
    ThrowIfRequested();
  }
};

template <typename Struct, typename Field>
bool HasField(const Struct& value, Field Struct::*field) {
  const auto offset = reinterpret_cast<const char*>(&(value.*field)) -
                      reinterpret_cast<const char*>(&value);
  return value.struct_size >= offset + sizeof(value.*field);
}

template <typename Result>
bool PrepareResult(Result* caller, uint32_t minimum_size) {
  return caller != nullptr && caller->struct_size >= minimum_size;
}

template <typename Result>
void CopyResult(const Result& source, Result* caller) {
  const uint32_t caller_size = caller->struct_size;
  std::memcpy(caller, &source, std::min<size_t>(caller_size, sizeof(Result)));
  caller->struct_size = caller_size;
}

template <typename Input>
Input BoundedInputCopy(const Input& source) {
  Input copy{};
  std::memcpy(&copy, &source,
              std::min<size_t>(source.struct_size, sizeof(Input)));
  return copy;
}

template <typename Result>
void CopyText(char* target, size_t capacity, const std::string& text) {
  if (capacity == 0) return;
  const size_t count = std::min(capacity - 1, text.size());
  std::memcpy(target, text.data(), count);
  target[count] = '\0';
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("Cannot read file: " + path.string());
  return std::string(std::istreambuf_iterator<char>(stream),
                     std::istreambuf_iterator<char>());
}

struct JpegErrorState {
  jpeg_error_mgr manager;
  std::jmp_buf jump;
};

void HandleJpegError(j_common_ptr info) {
  auto* state = reinterpret_cast<JpegErrorState*>(info->err);
  std::longjmp(state->jump, 1);
}

bool HasJpegExtension(const std::filesystem::path& path) {
  std::string extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return extension == ".jpg" || extension == ".jpeg";
}

// libjpeg can discard high-frequency DCT coefficients while decoding at 1/2,
// 1/4, or 1/8 resolution. Use the smallest native decode that still covers
// the requested feature bound, then apply the existing exact thumbnail step.
// This avoids materializing full-resolution RGB solely to discard it before
// SIFT. The caller retains the encoded dimensions for camera coordinates.
bool ReadBoundedJpeg(const std::filesystem::path& path,
                     const int max_image_size,
                     const bool as_rgb,
                     colmap::Bitmap* bitmap,
                     int* encoded_width,
                     int* encoded_height) {
  if (max_image_size <= 0 || bitmap == nullptr || encoded_width == nullptr ||
      encoded_height == nullptr || !HasJpegExtension(path)) {
    return false;
  }

  std::FILE* file = std::fopen(path.string().c_str(), "rb");
  if (file == nullptr) return false;

  jpeg_decompress_struct info{};
  JpegErrorState error{};
  info.err = jpeg_std_error(&error.manager);
  error.manager.error_exit = HandleJpegError;
  volatile bool created = false;
  uint8_t* volatile pixels = nullptr;
  if (setjmp(error.jump)) {
    if (pixels != nullptr) std::free(const_cast<uint8_t*>(pixels));
    if (created) jpeg_destroy_decompress(&info);
    std::fclose(file);
    return false;
  }

  jpeg_create_decompress(&info);
  created = true;
  jpeg_stdio_src(&info, file);
  if (jpeg_read_header(&info, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&info);
    std::fclose(file);
    return false;
  }
  *encoded_width = static_cast<int>(info.image_width);
  *encoded_height = static_cast<int>(info.image_height);
  info.out_color_space = JCS_RGB;

  if (std::max(*encoded_width, *encoded_height) > max_image_size) {
    for (const unsigned int denominator : {8u, 4u, 2u, 1u}) {
      info.scale_num = 1;
      info.scale_denom = denominator;
      jpeg_calc_output_dimensions(&info);
      if (std::max(info.output_width, info.output_height) >=
              static_cast<unsigned int>(max_image_size) ||
          denominator == 1) {
        break;
      }
    }
  }

  jpeg_start_decompress(&info);
  const size_t row_bytes =
      static_cast<size_t>(info.output_width) * info.output_components;
  const size_t byte_count = row_bytes * info.output_height;
  pixels = static_cast<uint8_t*>(std::malloc(byte_count));
  if (pixels == nullptr) {
    jpeg_destroy_decompress(&info);
    std::fclose(file);
    return false;
  }
  while (info.output_scanline < info.output_height) {
    JSAMPROW row = const_cast<uint8_t*>(pixels) +
                   static_cast<size_t>(info.output_scanline) * row_bytes;
    jpeg_read_scanlines(&info, &row, 1);
  }
  const int decoded_width = static_cast<int>(info.output_width);
  const int decoded_height = static_cast<int>(info.output_height);
  jpeg_finish_decompress(&info);
  jpeg_destroy_decompress(&info);
  created = false;
  std::fclose(file);

  colmap::Bitmap decoded(decoded_width, decoded_height, true);
  std::memcpy(decoded.RowMajorData().data(), const_cast<uint8_t*>(pixels),
              byte_count);
  std::free(const_cast<uint8_t*>(pixels));
  pixels = nullptr;
  if (!as_rgb) decoded = decoded.CloneAsGrey();
  decoded.Thumbnail(max_image_size);
  *bitmap = std::move(decoded);
  return true;
}

uint32_t RotateRight(uint32_t value, uint32_t count) {
  return (value >> count) | (value << (32 - count));
}

std::string SHA256(const std::string_view input) {
  static constexpr std::array<uint32_t, 64> k = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
      0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
      0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
      0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
      0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
      0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
      0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
      0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
      0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
      0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
      0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
      0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  std::vector<uint8_t> message(input.begin(), input.end());
  const uint64_t bit_length = static_cast<uint64_t>(message.size()) * 8;
  message.push_back(0x80);
  while ((message.size() % 64) != 56) message.push_back(0);
  for (int shift = 56; shift >= 0; shift -= 8) {
    message.push_back(static_cast<uint8_t>(bit_length >> shift));
  }
  std::array<uint32_t, 8> hash = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                  0xa54ff53a, 0x510e527f, 0x9b05688c,
                                  0x1f83d9ab, 0x5be0cd19};
  for (size_t offset = 0; offset < message.size(); offset += 64) {
    std::array<uint32_t, 64> w{};
    for (int i = 0; i < 16; ++i) {
      const size_t p = offset + i * 4;
      w[i] = (static_cast<uint32_t>(message[p]) << 24) |
             (static_cast<uint32_t>(message[p + 1]) << 16) |
             (static_cast<uint32_t>(message[p + 2]) << 8) |
             static_cast<uint32_t>(message[p + 3]);
    }
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = RotateRight(w[i - 15], 7) ^
                          RotateRight(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = RotateRight(w[i - 2], 17) ^
                          RotateRight(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    auto state = hash;
    for (int i = 0; i < 64; ++i) {
      const uint32_t sum1 = RotateRight(state[4], 6) ^
                            RotateRight(state[4], 11) ^
                            RotateRight(state[4], 25);
      const uint32_t choice =
          (state[4] & state[5]) ^ (~state[4] & state[6]);
      const uint32_t temp1 = state[7] + sum1 + choice + k[i] + w[i];
      const uint32_t sum0 = RotateRight(state[0], 2) ^
                            RotateRight(state[0], 13) ^
                            RotateRight(state[0], 22);
      const uint32_t majority = (state[0] & state[1]) ^
                                (state[0] & state[2]) ^
                                (state[1] & state[2]);
      const uint32_t temp2 = sum0 + majority;
      state = {temp1 + temp2, state[0], state[1], state[2],
               state[3] + temp1, state[4], state[5], state[6]};
    }
    for (int i = 0; i < 8; ++i) hash[i] += state[i];
  }
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (const uint32_t word : hash) out << std::setw(8) << word;
  return out.str();
}

std::string FileSHA256(const std::filesystem::path& path) {
  return SHA256(ReadFile(path));
}

void EnsureParent(const std::filesystem::path& path) {
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path());
  }
}

std::filesystem::path ComparablePath(const std::filesystem::path& path) {
  if (path.empty()) throw std::invalid_argument("V2 paths must not be empty.");
  std::error_code error;
  auto comparable = std::filesystem::weakly_canonical(
      std::filesystem::absolute(path, error), error);
  if (error) {
    throw std::invalid_argument("Cannot resolve V2 path: " + path.string());
  }
  return comparable.lexically_normal();
}

bool IsPathWithin(const std::filesystem::path& path,
                  const std::filesystem::path& directory) {
  auto path_it = path.begin();
  auto directory_it = directory.begin();
  for (; directory_it != directory.end(); ++directory_it, ++path_it) {
    if (path_it == path.end() || *path_it != *directory_it) return false;
  }
  return true;
}

void ValidateDistinctPaths(
    const std::vector<std::filesystem::path>& input_files,
    const std::vector<std::filesystem::path>& output_files,
    const std::vector<std::filesystem::path>& output_directories = {},
    const std::vector<std::filesystem::path>& protected_directories = {}) {
  std::vector<std::filesystem::path> inputs, outputs, output_dirs, protected_dirs;
  for (const auto& path : input_files) inputs.push_back(ComparablePath(path));
  for (const auto& path : output_files) outputs.push_back(ComparablePath(path));
  for (const auto& path : output_directories) {
    output_dirs.push_back(ComparablePath(path));
  }
  for (const auto& path : protected_directories) {
    protected_dirs.push_back(ComparablePath(path));
  }
  std::set<std::filesystem::path> unique_outputs;
  for (const auto& output : outputs) {
    if (!unique_outputs.insert(output).second) {
      throw std::invalid_argument("V2 output paths must be distinct.");
    }
    if (std::find(inputs.begin(), inputs.end(), output) != inputs.end()) {
      throw std::invalid_argument("A V2 output path aliases an input artifact.");
    }
    for (const auto& directory : protected_dirs) {
      if (IsPathWithin(output, directory)) {
        throw std::invalid_argument(
            "A V2 output path is inside a protected input directory.");
      }
    }
  }
  for (const auto& output_dir : output_dirs) {
    for (const auto& input : inputs) {
      if (IsPathWithin(input, output_dir)) {
        throw std::invalid_argument(
            "A V2 output directory contains an input artifact.");
      }
    }
    for (const auto& output : outputs) {
      if (IsPathWithin(output, output_dir)) {
        throw std::invalid_argument(
            "V2 file outputs must not overlap the model output directory.");
      }
    }
  }
}

std::string JsonEscape(const std::string& value) {
  std::ostringstream out;
  for (const unsigned char c : value) {
    switch (c) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (c < 0x20) {
          out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
              << static_cast<int>(c) << std::dec;
        } else {
          out << c;
        }
    }
  }
  return out.str();
}

void WriteDeterministicText(const std::filesystem::path& path,
                            const std::string& content) {
  EnsureParent(path);
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) throw std::runtime_error("Cannot write file: " + path.string());
  stream.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!stream) throw std::runtime_error("Failed writing file: " + path.string());
}

std::string CameraModelName(uint32_t model) {
  switch (model) {
    case COLMAPKIT_CAMERA_MODEL_V2_PINHOLE: return "PINHOLE";
    case COLMAPKIT_CAMERA_MODEL_V2_SIMPLE_PINHOLE: return "SIMPLE_PINHOLE";
    case COLMAPKIT_CAMERA_MODEL_V2_OPENCV: return "OPENCV";
    default: throw std::invalid_argument("Unsupported V2 camera model.");
  }
}

uint32_t ExpectedCameraParams(uint32_t model) {
  switch (model) {
    case COLMAPKIT_CAMERA_MODEL_V2_PINHOLE: return 4;
    case COLMAPKIT_CAMERA_MODEL_V2_SIMPLE_PINHOLE: return 3;
    case COLMAPKIT_CAMERA_MODEL_V2_OPENCV: return 8;
    default: return 0;
  }
}

// ARKit uses +X right, +Y up, -Z forward. COLMAP uses +X right, +Y down,
// +Z forward. This basis change is centralized here and covered by V2 tests.
colmap::Rigid3d ARKitWorldFromCameraToColmapCameraFromWorld(
    const double values[16]) {
  Eigen::Matrix4d world_from_arkit_camera;
  for (int col = 0; col < 4; ++col) {
    for (int row = 0; row < 4; ++row) {
      world_from_arkit_camera(row, col) = values[col * 4 + row];
    }
  }
  const Eigen::Matrix3d rotation =
      world_from_arkit_camera.block<3, 3>(0, 0);
  if (!world_from_arkit_camera.allFinite() ||
      std::abs(world_from_arkit_camera(3, 0)) > kRigidTolerance ||
      std::abs(world_from_arkit_camera(3, 1)) > kRigidTolerance ||
      std::abs(world_from_arkit_camera(3, 2)) > kRigidTolerance ||
      std::abs(world_from_arkit_camera(3, 3) - 1.0) > kRigidTolerance ||
      (rotation.transpose() * rotation - Eigen::Matrix3d::Identity()).norm() >
          kRigidTolerance ||
      std::abs(rotation.determinant() - 1.0) > kRigidTolerance) {
    throw std::invalid_argument(
        "world_from_camera must be a finite rigid column-major transform.");
  }
  Eigen::Matrix4d colmap_camera_from_arkit_camera = Eigen::Matrix4d::Identity();
  colmap_camera_from_arkit_camera(1, 1) = -1.0;
  colmap_camera_from_arkit_camera(2, 2) = -1.0;
  const Eigen::Matrix4d camera_from_world =
      colmap_camera_from_arkit_camera * world_from_arkit_camera.inverse();
  Eigen::Quaterniond camera_rotation(
      camera_from_world.block<3, 3>(0, 0));
  camera_rotation.normalize();
  return colmap::Rigid3d(
      camera_rotation,
      camera_from_world.block<3, 1>(0, 3));
}

double RotationDifferenceDegrees(const colmap::Rigid3d& a,
                                 const colmap::Rigid3d& b) {
  const Eigen::Quaterniond delta = a.rotation() * b.rotation().conjugate();
  return 2.0 * std::acos(std::clamp(std::abs(delta.w()), 0.0, 1.0)) *
         180.0 / M_PI;
}

Eigen::Vector3d CameraCenter(const colmap::Rigid3d& cam_from_world) {
  return colmap::Inverse(cam_from_world).translation();
}

struct WeightedPosePriorCost {
  WeightedPosePriorCost(const colmap::Rigid3d& prior,
                        double translation_weight,
                        double rotation_weight)
      : prior_coeffs(prior.rotation().coeffs()),
        prior_center(CameraCenter(prior)),
        translation_weight(translation_weight),
        rotation_weight(rotation_weight) {}

  template <typename T>
  bool operator()(const T* const pose, T* residuals) const {
    const Eigen::Map<const Eigen::Quaternion<T>> rotation(pose);
    const Eigen::Map<const Eigen::Matrix<T, 3, 1>> translation(pose + 4);
    const Eigen::Quaternion<T> prior_rotation(
        static_cast<T>(prior_coeffs[3]), static_cast<T>(prior_coeffs[0]),
        static_cast<T>(prior_coeffs[1]), static_cast<T>(prior_coeffs[2]));
    const Eigen::Quaternion<T> delta = rotation * prior_rotation.conjugate();
    residuals[0] = T(2.0 * rotation_weight) * delta.x();
    residuals[1] = T(2.0 * rotation_weight) * delta.y();
    residuals[2] = T(2.0 * rotation_weight) * delta.z();
    const Eigen::Matrix<T, 3, 1> center = rotation.conjugate() * (-translation);
    for (int i = 0; i < 3; ++i) {
      residuals[3 + i] =
          T(translation_weight) * (center[i] - T(prior_center[i]));
    }
    return true;
  }

  Eigen::Vector4d prior_coeffs;
  Eigen::Vector3d prior_center;
  double translation_weight;
  double rotation_weight;
};

struct OwnedImage {
  ColmapKitTrackedImageV2 value{};
  std::string path;
};

struct OwnedTrackedConfig {
  ColmapKitTrackedPoseConfigV2 value{};
  std::vector<OwnedImage> images;
  std::vector<ColmapKitTrackedImageV2> image_values;
  std::string database_path, output_model_path, refined_pose_path, evidence_path;

  static OwnedTrackedConfig Copy(const ColmapKitTrackedPoseConfigV2& source) {
    OwnedTrackedConfig out;
    out.value = BoundedInputCopy(source);
    out.images.resize(out.value.num_images);
    out.image_values.resize(out.value.num_images);
    for (uint32_t i = 0; i < out.value.num_images; ++i) {
      const auto& image = out.value.images[i];
      if (image.struct_size < kTrackedImageMinimumSize) {
        throw std::invalid_argument("Each V2 image must provide the full required prefix.");
      }
      out.images[i].value = image;
      out.images[i].path = image.image_path == nullptr ? "" : image.image_path;
      out.images[i].value.image_path = out.images[i].path.c_str();
      out.image_values[i] = out.images[i].value;
    }
    auto copy_path = [](const char* p) { return p == nullptr ? std::string() : std::string(p); };
    out.database_path = copy_path(out.value.database_path);
    out.output_model_path = copy_path(out.value.output_model_path);
    out.refined_pose_path = copy_path(out.value.refined_pose_path);
    out.evidence_path = copy_path(out.value.evidence_path);
    out.Refresh();
    return out;
  }
  void Refresh() {
    for (size_t i = 0; i < images.size(); ++i) {
      images[i].value.image_path = images[i].path.c_str();
      image_values[i] = images[i].value;
    }
    value.images = image_values.data();
    value.database_path = database_path.c_str();
    value.output_model_path = output_model_path.c_str();
    value.refined_pose_path = refined_pose_path.c_str();
    value.evidence_path = evidence_path.c_str();
  }
};

struct OwnedPriorConfig {
  ColmapKitRGBPriorConfigV2 value{};
  std::vector<OwnedImage> images;
  std::vector<ColmapKitTrackedImageV2> image_values;
  std::string database_path, refined_model_path, refined_pose_path;
  std::string expected_pose_sha, output_ply_path, evidence_path;

  static OwnedPriorConfig Copy(const ColmapKitRGBPriorConfigV2& source) {
    OwnedPriorConfig out;
    out.value = BoundedInputCopy(source);
    out.images.resize(out.value.num_images);
    out.image_values.resize(out.value.num_images);
    for (uint32_t i = 0; i < out.value.num_images; ++i) {
      const auto& image = out.value.images[i];
      if (image.struct_size < kTrackedImageMinimumSize) {
        throw std::invalid_argument("Each V2 image must provide the full required prefix.");
      }
      out.images[i].value = image;
      out.images[i].path = image.image_path == nullptr ? "" : image.image_path;
      out.images[i].value.image_path = out.images[i].path.c_str();
      out.image_values[i] = out.images[i].value;
    }
    auto s = [](const char* p) { return p == nullptr ? std::string() : std::string(p); };
    out.database_path = s(out.value.database_path);
    out.refined_model_path = s(out.value.refined_model_path);
    out.refined_pose_path = s(out.value.refined_pose_path);
    out.expected_pose_sha = s(out.value.expected_refined_pose_sha256);
    out.output_ply_path = s(out.value.output_ply_path);
    out.evidence_path = s(out.value.evidence_path);
    out.Refresh();
    return out;
  }
  void Refresh() {
    for (size_t i = 0; i < images.size(); ++i) {
      images[i].value.image_path = images[i].path.c_str();
      image_values[i] = images[i].value;
    }
    value.images = image_values.data();
    value.database_path = database_path.c_str();
    value.refined_model_path = refined_model_path.c_str();
    value.refined_pose_path = refined_pose_path.c_str();
    value.expected_refined_pose_sha256 = expected_pose_sha.c_str();
    value.output_ply_path = output_ply_path.c_str();
    value.evidence_path = evidence_path.c_str();
  }
};

void Emit(const ColmapKitProgressCallbackV2 callback,
          void* user_data,
          uint32_t stage,
          const Clock::time_point start,
          const char* message,
          double fraction = -1.0,
          uint64_t current = 0,
          uint64_t total = 0,
          const char* detail = "") {
  if (callback == nullptr) return;
  ColmapKitProgressEventV2 event{};
  event.struct_size = sizeof(event);
  event.stage = stage;
  event.fraction = fraction;
  event.current = current;
  event.total = total;
  event.elapsed_seconds = Seconds(Clock::now() - start).count();
  event.message = message;
  event.detail = detail;
  callback(&event, user_data);
}

void ValidateImage(const ColmapKitTrackedImageV2& image) {
  if (image.struct_size < kTrackedImageMinimumSize || image.image_path == nullptr ||
      image.image_path[0] == '\0' || !std::filesystem::is_regular_file(image.image_path)) {
    throw std::invalid_argument("Each V2 image needs a readable image_path.");
  }
  if (image.encoded_width == 0 || image.encoded_height == 0) {
    throw std::invalid_argument("Encoded image dimensions must be nonzero.");
  }
  if (image.tracking_state != COLMAPKIT_TRACKING_STATE_V2_NORMAL) {
    throw std::invalid_argument(
        "V2 accepts only caller-filtered ARKit frames with NORMAL tracking.");
  }
  if ((image.inclusion_flags & COLMAPKIT_TRACKED_IMAGE_FLAG_V2_INCLUDED) == 0) {
    throw std::invalid_argument(
        "Each V2 image must be explicitly marked included by the caller.");
  }
  if (image.translation_weight <= 0 || image.rotation_weight <= 0 ||
      !std::isfinite(image.translation_weight) ||
      !std::isfinite(image.rotation_weight)) {
    throw std::invalid_argument("Pose weights must be finite and positive.");
  }
  if (image.num_camera_params != ExpectedCameraParams(image.camera_model)) {
    throw std::invalid_argument("Camera parameter count does not match its model.");
  }
  for (uint32_t i = 0; i < image.num_camera_params; ++i) {
    if (!std::isfinite(image.camera_params[i])) {
      throw std::invalid_argument("Camera parameters must be finite.");
    }
  }
  const bool positive_focal_length =
      image.camera_params[0] > 0 &&
      (image.camera_model == COLMAPKIT_CAMERA_MODEL_V2_SIMPLE_PINHOLE ||
       image.camera_params[1] > 0);
  if (!positive_focal_length) {
    throw std::invalid_argument("Camera focal lengths must be positive.");
  }
  (void)ARKitWorldFromCameraToColmapCameraFromWorld(image.world_from_camera);
}

std::vector<uint32_t> StableImageOrder(const ColmapKitTrackedImageV2* images,
                                       uint32_t count) {
  std::vector<uint32_t> indices(count);
  std::iota(indices.begin(), indices.end(), 0);
  std::sort(indices.begin(), indices.end(), [&](uint32_t a, uint32_t b) {
    return std::tie(images[a].order_index, images[a].stable_id) <
           std::tie(images[b].order_index, images[b].stable_id);
  });
  std::set<uint64_t> stable_ids;
  std::set<uint32_t> order_indices;
  std::set<std::string> basenames;
  for (uint32_t i = 0; i < count; ++i) {
    if (!stable_ids.insert(images[i].stable_id).second ||
        !order_indices.insert(images[i].order_index).second ||
        !basenames
             .insert(std::filesystem::path(images[i].image_path)
                         .filename()
                         .string())
             .second) {
      throw std::invalid_argument(
          "V2 image stable IDs, order indices, and basenames must be unique.");
    }
  }
  return indices;
}

std::vector<std::pair<uint32_t, uint32_t>> BuildPairs(
    const ColmapKitTrackedPoseConfigV2& config,
    const std::vector<uint32_t>& order,
    const std::vector<colmap::Rigid3d>& poses) {
  std::set<std::pair<uint32_t, uint32_t>> pairs;
  const uint32_t temporal = std::max(1u, config.temporal_neighbor_count);
  for (uint32_t p = 0; p < order.size(); ++p) {
    for (uint32_t delta = 1; delta <= temporal && p + delta < order.size(); ++delta) {
      pairs.emplace(std::min(order[p], order[p + delta]),
                    std::max(order[p], order[p + delta]));
    }
  }
  for (uint32_t p = 0; p < order.size(); ++p) {
    std::vector<std::tuple<double, uint32_t, uint32_t>> candidates;
    const Eigen::Vector3d center = CameraCenter(poses[order[p]]);
    for (uint32_t q = 0; q < order.size(); ++q) {
      if (p == q || (p > q ? p - q : q - p) <= temporal) continue;
      const double distance = (center - CameraCenter(poses[order[q]])).norm();
      const double angle = RotationDifferenceDegrees(poses[order[p]], poses[order[q]]);
      if (distance >= config.revisit_min_translation_meters &&
          distance <= config.revisit_max_translation_meters &&
          angle <= config.revisit_max_rotation_degrees) {
        candidates.emplace_back(distance, config.images[order[q]].order_index,
                                order[q]);
      }
    }
    std::sort(candidates.begin(), candidates.end());
    const size_t limit = std::min<size_t>(
        config.max_revisit_neighbors_per_image, candidates.size());
    for (size_t i = 0; i < limit; ++i) {
      const uint32_t q = std::get<2>(candidates[i]);
      pairs.emplace(std::min(order[p], q), std::max(order[p], q));
    }
  }
  std::vector<std::pair<uint32_t, uint32_t>> bounded(pairs.begin(), pairs.end());
  if (bounded.size() > config.max_image_pairs) bounded.resize(config.max_image_pairs);
  return bounded;
}

std::string RelativeImageName(const ColmapKitTrackedImageV2& image) {
  return std::filesystem::path(image.image_path).filename().string();
}

std::string RGBManifestSHA(const ColmapKitTrackedImageV2* images,
                           const std::vector<uint32_t>& order) {
  std::ostringstream manifest;
  manifest.imbue(std::locale::classic());
  for (const uint32_t i : order) {
    manifest << images[i].stable_id << '\t' << images[i].order_index << '\t'
             << RelativeImageName(images[i]) << '\t'
             << FileSHA256(images[i].image_path) << '\n';
  }
  return SHA256(manifest.str());
}

void ExtractColorsFromBoundedBitmaps(
    const ColmapKitTrackedPoseConfigV2& config,
    const std::unordered_map<std::string, uint32_t>& input_by_name,
    const std::vector<colmap::Bitmap>& bitmaps,
    colmap::Reconstruction* reconstruction) {
  struct ColorData {
    Eigen::Vector3d sum = Eigen::Vector3d::Zero();
    uint32_t count = 0;
  };
  std::unordered_map<colmap::point3D_t, ColorData> colors;
  for (const auto image_id : reconstruction->RegImageIds()) {
    const auto& image = reconstruction->Image(image_id);
    const uint32_t input_index = input_by_name.at(image.Name());
    const auto& bitmap = bitmaps[input_index];
    if (bitmap.IsEmpty()) continue;
    const double scale_x =
        static_cast<double>(bitmap.Width()) / config.images[input_index].encoded_width;
    const double scale_y = static_cast<double>(bitmap.Height()) /
                           config.images[input_index].encoded_height;
    for (const auto& point2D : image.Points2D()) {
      if (!point2D.HasPoint3D()) continue;
      const auto color = bitmap.InterpolateBilinear(
          point2D.xy(0) * scale_x - 0.5, point2D.xy(1) * scale_y - 0.5);
      if (!color.has_value()) continue;
      auto& aggregate = colors[point2D.point3D_id];
      aggregate.sum += Eigen::Vector3d(color->r, color->g, color->b);
      ++aggregate.count;
    }
  }
  for (const auto point3D_id : reconstruction->Point3DIds()) {
    auto& point3D = reconstruction->Point3D(point3D_id);
    const auto color = colors.find(point3D_id);
    if (color == colors.end() || color->second.count == 0) {
      point3D.color = Eigen::Vector3ub::Zero();
      continue;
    }
    Eigen::Vector3d mean = color->second.sum / color->second.count;
    for (Eigen::Index channel = 0; channel < mean.size(); ++channel) {
      mean[channel] = std::round(std::clamp(mean[channel], 0.0, 255.0));
    }
    point3D.color = mean.cast<uint8_t>();
  }
}

void ValidateTrackedConfig(const ColmapKitTrackedPoseConfigV2& config) {
  if (config.struct_size < kTrackedConfigMinimumSize || config.images == nullptr ||
      config.num_images < 3 || config.database_path == nullptr ||
      config.output_model_path == nullptr || config.refined_pose_path == nullptr ||
      config.evidence_path == nullptr) {
    throw std::invalid_argument(
        "Tracked-pose V2 requires its full prefix, at least three images, and all output paths.");
  }
  if (config.max_features_per_image == 0 || config.max_image_pairs == 0 ||
      config.max_triangulation_passes == 0 ||
      config.max_bundle_adjustment_iterations == 0 ||
      config.revisit_min_translation_meters < 0 ||
      config.revisit_max_translation_meters <=
          config.revisit_min_translation_meters ||
      config.revisit_max_rotation_degrees <= 0 ||
      config.min_triangulation_angle_degrees <= 0 ||
      config.max_reprojection_error_pixels <= 0 ||
      config.max_allowed_scale_drift_ratio < 0) {
    throw std::invalid_argument("Tracked-pose V2 bounds are invalid or zero.");
  }
  for (uint32_t i = 0; i < config.num_images; ++i) ValidateImage(config.images[i]);
  (void)StableImageOrder(config.images, config.num_images);
  const auto parent = std::filesystem::path(config.images[0].image_path).parent_path();
  for (uint32_t i = 1; i < config.num_images; ++i) {
    if (std::filesystem::path(config.images[i].image_path).parent_path() != parent) {
      throw std::invalid_argument(
          "This bounded V2 slice requires all RGB inputs in one directory.");
    }
  }
  std::vector<std::filesystem::path> input_paths;
  input_paths.reserve(config.num_images);
  for (uint32_t i = 0; i < config.num_images; ++i) {
    input_paths.emplace_back(config.images[i].image_path);
  }
  ValidateDistinctPaths(
      input_paths,
      {config.database_path,
       std::string(config.database_path) + ".v2-pairs.txt",
       config.refined_pose_path,
       config.evidence_path},
      {config.output_model_path});
}

std::vector<colmap::Rigid3d> InitialPoses(
    const ColmapKitTrackedPoseConfigV2& config) {
  std::vector<colmap::Rigid3d> poses;
  poses.reserve(config.num_images);
  for (uint32_t i = 0; i < config.num_images; ++i) {
    poses.push_back(ARKitWorldFromCameraToColmapCameraFromWorld(
        config.images[i].world_from_camera));
  }
  return poses;
}

std::string PosesJSON(const ColmapKitTrackedPoseConfigV2& config,
                      const std::vector<uint32_t>& order,
                      const std::vector<colmap::Rigid3d>& refined,
                      const std::vector<colmap::Rigid3d>& initial) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(17);
  out << "{\n  \"schema\": \"colmapkit.refined-poses.v2\",\n"
         "  \"coordinate_system\": \"arkit_world_meters\",\n"
         "  \"pose_convention\": \"column_major_world_from_camera\",\n"
         "  \"gauge\": \"two_arkit_camera_frames_fixed_during_ba\",\n";
  out << "  \"rgb_manifest_sha256\": \""
      << RGBManifestSHA(config.images, order) << "\",\n"
      << "  \"images\": [\n";
  const Eigen::Matrix4d basis =
      (Eigen::Vector4d(1.0, -1.0, -1.0, 1.0)).asDiagonal();
  for (size_t p = 0; p < order.size(); ++p) {
    const uint32_t i = order[p];
    const colmap::Rigid3d world_from_colmap_camera = colmap::Inverse(refined[i]);
    Eigen::Matrix4d world_from_arkit_camera = Eigen::Matrix4d::Identity();
    world_from_arkit_camera.block<3, 4>(0, 0) =
        world_from_colmap_camera.ToMatrix();
    world_from_arkit_camera = world_from_arkit_camera * basis;
    out << "    {\"stable_id\":" << config.images[i].stable_id
        << ",\"order_index\":" << config.images[i].order_index
        << ",\"image\":\"" << JsonEscape(RelativeImageName(config.images[i]))
        << "\",\"rgb_sha256\":\"" << FileSHA256(config.images[i].image_path)
        << "\",\"translation_weight\":" << config.images[i].translation_weight
        << ",\"rotation_weight\":" << config.images[i].rotation_weight
        << ",\"translation_correction_meters\":"
        << (CameraCenter(refined[i]) - CameraCenter(initial[i])).norm()
        << ",\"rotation_correction_degrees\":"
        << RotationDifferenceDegrees(refined[i], initial[i])
        << ",\"world_from_camera\":[";
    bool first = true;
    for (int col = 0; col < 4; ++col) {
      for (int row = 0; row < 4; ++row) {
        if (!first) out << ',';
        first = false;
        out << world_from_arkit_camera(row, col);
      }
    }
    out << "]}" << (p + 1 == order.size() ? "\n" : ",\n");
  }
  out << "  ]\n}\n";
  return out.str();
}

uint32_t CountMatchedPairs(const colmap::Database& database) {
  uint32_t count = 0;
  for (const auto& [_, inliers] : database.ReadTwoViewGeometryNumInliers()) {
    if (inliers > 0) ++count;
  }
  return count;
}

ColmapKitStatus RunTracked(const ColmapKitTrackedPoseConfigV2& config,
                           Cancellation* cancellation,
                           ColmapKitTrackedPoseResultV2* result) {
  ColmapKitTrackedPoseResultV2 local{};
  local.struct_size = sizeof(local);
  local.status = COLMAPKIT_STATUS_RUNTIME_ERROR;
  const auto overall_start = Clock::now();
  try {
    ValidateTrackedConfig(config);
    cancellation->ThrowIfRequested();
    const auto order = StableImageOrder(config.images, config.num_images);
    const auto initial_poses = InitialPoses(config);
    Emit(config.progress_callback, config.progress_user_data,
         COLMAPKIT_PROGRESS_STAGE_V2_PREPARING, overall_start,
         "Preparing bounded tracked-pose reconstruction.");

    const std::filesystem::path database_path(config.database_path);
    const std::filesystem::path model_path(config.output_model_path);
    const std::filesystem::path pose_path(config.refined_pose_path);
    const std::filesystem::path evidence_path(config.evidence_path);
    EnsureParent(database_path);
    std::filesystem::remove(database_path);
    auto database = colmap::Database::Open(database_path);

    std::vector<colmap::camera_t> camera_ids(config.num_images);
    std::vector<colmap::image_t> image_ids(config.num_images);
    for (uint32_t i = 0; i < config.num_images; ++i) {
      const auto& input = config.images[i];
      colmap::Camera camera;
      camera.model_id = colmap::CameraModelNameToId(CameraModelName(input.camera_model));
      camera.width = input.encoded_width;
      camera.height = input.encoded_height;
      camera.params.assign(input.camera_params,
                           input.camera_params + input.num_camera_params);
      camera.has_prior_focal_length = true;
      camera.camera_id = database->WriteCamera(camera);
      camera_ids[i] = camera.camera_id;
      colmap::Rig rig;
      rig.AddRefSensor(camera.SensorId());
      database->WriteRig(rig);
      colmap::Image image;
      image.SetName(RelativeImageName(input));
      image.SetCameraId(camera.camera_id);
      image.SetImageId(database->WriteImage(image));
      image_ids[i] = image.ImageId();
    }

    colmap::FeatureExtractionOptions extraction_options;
    extraction_options.num_threads = config.num_threads == 0
                                         ? -1
                                         : static_cast<int>(config.num_threads);
    extraction_options.use_gpu = false;
    extraction_options.sift->max_num_features =
        static_cast<int>(config.max_features_per_image);
    const auto feature_start = Clock::now();
    Emit(config.progress_callback, config.progress_user_data,
         COLMAPKIT_PROGRESS_STAGE_V2_FEATURE_EXTRACTION, overall_start,
         "Extracting bounded RGB features.");
    auto extractor = colmap::FeatureExtractor::Create(extraction_options);
    std::vector<colmap::Bitmap> bounded_color_bitmaps(config.num_images);
    for (size_t p = 0; p < order.size(); ++p) {
      cancellation->ThrowIfRequested();
      const uint32_t i = order[p];
      colmap::Bitmap color_bitmap;
      int encoded_width = 0;
      int encoded_height = 0;
      const bool read_bounded_jpeg = ReadBoundedJpeg(
          config.images[i].image_path,
          static_cast<int>(config.max_feature_image_size),
          true, &color_bitmap, &encoded_width, &encoded_height);
      if (!read_bounded_jpeg &&
          !color_bitmap.Read(config.images[i].image_path, true)) {
        throw std::runtime_error("Cannot read RGB input: " +
                                 std::string(config.images[i].image_path));
      }
      if (!read_bounded_jpeg) {
        encoded_width = color_bitmap.Width();
        encoded_height = color_bitmap.Height();
      }
      if (encoded_width != static_cast<int>(config.images[i].encoded_width) ||
          encoded_height != static_cast<int>(config.images[i].encoded_height)) {
        throw std::invalid_argument(
            "Encoded dimensions do not match the decoded RGB image.");
      }
      if (!read_bounded_jpeg && config.max_feature_image_size > 0) {
        color_bitmap.Thumbnail(static_cast<int>(config.max_feature_image_size));
      }
      colmap::Bitmap bitmap = extraction_options.RequiresRGB()
                                  ? color_bitmap.Clone()
                                  : color_bitmap.CloneAsGrey();
      bounded_color_bitmaps[i] = std::move(color_bitmap);
      colmap::FeatureKeypoints keypoints;
      colmap::FeatureDescriptors descriptors;
      if (!extractor->Extract(bitmap, &keypoints, &descriptors)) {
        throw std::runtime_error("Feature extraction failed for: " +
                                 std::string(config.images[i].image_path));
      }
      if (bitmap.Width() != static_cast<int>(config.images[i].encoded_width) ||
          bitmap.Height() != static_cast<int>(config.images[i].encoded_height)) {
        const float scale_x = static_cast<float>(config.images[i].encoded_width) /
                              bitmap.Width();
        const float scale_y = static_cast<float>(config.images[i].encoded_height) /
                              bitmap.Height();
        for (auto& keypoint : keypoints) keypoint.Rescale(scale_x, scale_y);
      }
      database->WriteKeypoints(image_ids[i], keypoints);
      database->WriteDescriptors(image_ids[i], descriptors);
      Emit(config.progress_callback, config.progress_user_data,
           COLMAPKIT_PROGRESS_STAGE_V2_FEATURE_EXTRACTION, overall_start,
           "Extracting bounded RGB features.",
           static_cast<double>(p + 1) / order.size(), p + 1, order.size(),
           RelativeImageName(config.images[i]).c_str());
    }
    local.feature_seconds = Seconds(Clock::now() - feature_start).count();

    const auto pairs = BuildPairs(config, order, initial_poses);
    if (pairs.empty()) throw std::runtime_error("Bounded pair selection produced no pairs.");
    const std::filesystem::path pair_path = database_path.string() + ".v2-pairs.txt";
    std::ostringstream pair_text;
    for (const auto& [a, b] : pairs) {
      pair_text << RelativeImageName(config.images[a]) << ' '
                << RelativeImageName(config.images[b]) << '\n';
    }
    WriteDeterministicText(pair_path, pair_text.str());

    colmap::ImportedPairingOptions pairing_options;
    pairing_options.match_list_path = pair_path;
    pairing_options.block_size = std::min<int>(1225, std::max<int>(1, pairs.size()));
    colmap::FeatureMatchingOptions matching_options;
    matching_options.num_threads = extraction_options.num_threads;
    matching_options.use_gpu = false;
    matching_options.max_num_matches =
        static_cast<int>(config.max_features_per_image);
    colmap::TwoViewGeometryOptions geometry_options;
    geometry_options.min_num_inliers = 8;
    geometry_options.ransac_options.max_error =
        config.max_reprojection_error_pixels;
    geometry_options.ransac_options.random_seed =
        static_cast<int>(config.random_seed);
    const auto matching_start = Clock::now();
    Emit(config.progress_callback, config.progress_user_data,
         COLMAPKIT_PROGRESS_STAGE_V2_MATCHING, overall_start,
         "Matching the explicit bounded pair set.");
    auto matcher = colmap::CreateImagePairsFeatureMatcher(
        pairing_options, matching_options, geometry_options, database_path);
    cancellation->RunThread(*matcher);
    local.matching_seconds = Seconds(Clock::now() - matching_start).count();
    local.matched_pairs = CountMatchedPairs(*database);
    if (local.matched_pairs == 0) {
      throw std::runtime_error("No bounded image pair passed geometric verification.");
    }

    colmap::DatabaseCache::Options cache_options;
    cache_options.min_num_matches = 1;
    cache_options.load_all_images = true;
    auto cache = colmap::DatabaseCache::Create(*database, cache_options);
    colmap::Reconstruction reconstruction;
    reconstruction.Load(*cache);
    std::unordered_map<std::string, uint32_t> input_by_name;
    for (uint32_t i = 0; i < config.num_images; ++i) {
      input_by_name.emplace(RelativeImageName(config.images[i]), i);
    }
    for (const auto& [image_id, _] : reconstruction.Images()) {
      auto& image = reconstruction.Image(image_id);
      const uint32_t input_index = input_by_name.at(image.Name());
      image.FramePtr()->SetRigFromWorld(initial_poses[input_index]);
      reconstruction.RegisterFrame(image.FrameId());
    }

    const auto triangulation_start = Clock::now();
    Emit(config.progress_callback, config.progress_user_data,
         COLMAPKIT_PROGRESS_STAGE_V2_TRIANGULATION, overall_start,
         "Triangulating from frozen ARKit-seeded cameras.");
    colmap::IncrementalTriangulator::Options triangulation_options;
    // The sparse C-style prior remains genuinely sparse. Variant D separately
    // recovers valid two-view RGB correspondences with these same frozen poses.
    triangulation_options.ignore_two_view_tracks = true;
    triangulation_options.min_angle = config.min_triangulation_angle_degrees;
    triangulation_options.create_max_angle_error =
        config.max_reprojection_error_pixels;
    triangulation_options.continue_max_angle_error =
        config.max_reprojection_error_pixels;
    triangulation_options.merge_max_reproj_error =
        config.max_reprojection_error_pixels;
    triangulation_options.complete_max_reproj_error =
        config.max_reprojection_error_pixels;
    triangulation_options.random_seed = static_cast<int>(config.random_seed);
    colmap::IncrementalTriangulator triangulator(cache->CorrespondenceGraph(),
                                                 reconstruction);
    std::vector<colmap::image_t> stable_image_ids;
    for (const uint32_t i : order) {
      stable_image_ids.push_back(
          reconstruction.FindImageWithName(RelativeImageName(config.images[i]))
              ->ImageId());
    }
    for (uint32_t pass = 0; pass < config.max_triangulation_passes; ++pass) {
      cancellation->ThrowIfRequested();
      size_t changes = 0;
      for (const auto image_id : stable_image_ids) {
        changes += triangulator.TriangulateImage(triangulation_options, image_id);
      }
      changes += triangulator.CompleteAllTracks(triangulation_options);
      changes += triangulator.MergeAllTracks(triangulation_options);
      if (changes == 0) break;
    }
    if (reconstruction.NumPoints3D() == 0) {
      throw std::runtime_error("Tracked-pose triangulation produced no sparse points.");
    }
    reconstruction.UpdatePoint3DErrors();
    local.initial_mean_reprojection_error =
        reconstruction.ComputeMeanReprojectionError();
    local.triangulation_seconds =
        Seconds(Clock::now() - triangulation_start).count();

    colmap::BundleAdjustmentOptions ba_options;
    ba_options.refine_focal_length = false;
    ba_options.refine_principal_point = false;
    ba_options.refine_extra_params = false;
    ba_options.refine_sensor_from_rig = false;
    ba_options.refine_rig_from_world = true;
    ba_options.print_summary = false;
    ba_options.ceres->loss_function_type =
        colmap::CeresBundleAdjustmentOptions::LossFunctionType::SOFT_L1;
    ba_options.ceres->loss_function_scale =
        config.max_reprojection_error_pixels;
    ba_options.ceres->solver_options.max_num_iterations =
        static_cast<int>(config.max_bundle_adjustment_iterations);
    ba_options.ceres->solver_options.num_threads =
        config.num_threads == 0 ? 1 : static_cast<int>(config.num_threads);
    colmap::BundleAdjustmentConfig ba_config;
    for (const auto image_id : stable_image_ids) {
      ba_config.AddImage(image_id);
      ba_config.SetConstantCamIntrinsics(reconstruction.Image(image_id).CameraId());
    }
    // Two full ARKit camera frames are fixed in the optimization. This pins
    // origin, orientation, and metric baseline instead of recovering them with
    // a post-hoc similarity transform.
    ba_config.SetConstantRigFromWorldPose(
        reconstruction.Image(stable_image_ids.front()).FrameId());
    ba_config.SetConstantRigFromWorldPose(
        reconstruction.Image(stable_image_ids.back()).FrameId());
    ba_config.FixGauge(colmap::BundleAdjustmentGauge::TWO_CAMS_FROM_WORLD);
    const auto ba_start = Clock::now();
    Emit(config.progress_callback, config.progress_user_data,
         COLMAPKIT_PROGRESS_STAGE_V2_BUNDLE_ADJUSTMENT, overall_start,
         "Refining while holding the ARKit gauge and metric baseline.");
    auto adjuster = colmap::CreateDefaultCeresBundleAdjuster(
        ba_options, ba_config, reconstruction);
    for (size_t p = 1; p + 1 < stable_image_ids.size(); ++p) {
      const auto image_id = stable_image_ids[p];
      auto& image = reconstruction.Image(image_id);
      const uint32_t i = input_by_name.at(image.Name());
      auto* cost = new ceres::AutoDiffCostFunction<WeightedPosePriorCost, 6, 7>(
          new WeightedPosePriorCost(initial_poses[i],
                                    config.images[i].translation_weight,
                                    config.images[i].rotation_weight));
      adjuster->Problem()->AddResidualBlock(
          cost, nullptr, image.FramePtr()->RigFromWorld().params.data());
    }
    const auto summary = adjuster->Solve();
    cancellation->ThrowIfRequested();
    if (!summary->IsSolutionUsable()) {
      throw std::runtime_error("Constrained bundle adjustment failed: " +
                               summary->BriefReport());
    }
    reconstruction.UpdatePoint3DErrors();
    local.bundle_adjustment_seconds = Seconds(Clock::now() - ba_start).count();
    local.final_mean_reprojection_error =
        reconstruction.ComputeMeanReprojectionError();

    std::vector<colmap::Rigid3d> refined(config.num_images);
    for (const auto image_id : reconstruction.RegImageIds()) {
      const auto& image = reconstruction.Image(image_id);
      const uint32_t i = input_by_name.at(image.Name());
      refined[i] = image.CamFromWorld();
      local.max_translation_correction_meters = std::max(
          local.max_translation_correction_meters,
          (CameraCenter(refined[i]) - CameraCenter(initial_poses[i])).norm());
      local.max_rotation_correction_degrees = std::max(
          local.max_rotation_correction_degrees,
          RotationDifferenceDegrees(refined[i], initial_poses[i]));
    }
    const uint32_t first = order.front(), last = order.back();
    const double initial_anchor_baseline =
        (CameraCenter(initial_poses[first]) - CameraCenter(initial_poses[last])).norm();
    if (initial_anchor_baseline <= 1e-8) {
      throw std::invalid_argument(
          "The selected ARKit gauge anchors need a nonzero metric baseline.");
    }
    const double refined_anchor_baseline =
        (CameraCenter(refined[first]) - CameraCenter(refined[last])).norm();
    local.measured_scale_drift_ratio =
        std::abs(refined_anchor_baseline / initial_anchor_baseline - 1.0);
    if ((refined[first].ToMatrix() - initial_poses[first].ToMatrix()).norm() >
            kAnchorTolerance ||
        (refined[last].ToMatrix() - initial_poses[last].ToMatrix()).norm() >
            kAnchorTolerance ||
        local.measured_scale_drift_ratio > config.max_allowed_scale_drift_ratio) {
      throw std::runtime_error(
          "Constrained refinement violated the ARKit gauge or metric-scale tolerance.");
    }

    const auto export_start = Clock::now();
    Emit(config.progress_callback, config.progress_user_data,
         COLMAPKIT_PROGRESS_STAGE_V2_EXPORT, overall_start,
         "Writing canonical reconstruction and pose evidence.");
    std::filesystem::create_directories(model_path);
    ExtractColorsFromBoundedBitmaps(
        config, input_by_name, bounded_color_bitmaps, &reconstruction);
    reconstruction.Write(model_path);
    const std::string pose_json = PosesJSON(config, order, refined, initial_poses);
    WriteDeterministicText(pose_path, pose_json);
    const std::string pose_sha = SHA256(pose_json);
    CopyText<ColmapKitTrackedPoseResultV2>(
        local.refined_pose_sha256, sizeof(local.refined_pose_sha256), pose_sha);
    local.registered_images = static_cast<uint32_t>(reconstruction.NumRegImages());
    local.sparse_points = reconstruction.NumPoints3D();
    local.observations = reconstruction.ComputeNumObservations();
    std::ostringstream evidence;
    evidence.imbue(std::locale::classic());
    evidence << std::setprecision(17)
             << "{\n  \"schema\": \"colmapkit.tracked-pose-evidence.v2\",\n"
             << "  \"route\": \"tracked_pose_bounded_v2\",\n"
             << "  \"fallback_used\": false,\n"
             << "  \"arkit_world_frame_preserved\": true,\n"
             << "  \"arkit_metric_scale_preserved\": true,\n"
             << "  \"pose_weights\": \"caller_supplied\",\n"
             << "  \"refined_pose_sha256\": \"" << pose_sha << "\",\n"
             << "  \"registered_images\": " << local.registered_images << ",\n"
             << "  \"matched_pairs\": " << local.matched_pairs << ",\n"
             << "  \"sparse_points\": " << local.sparse_points << ",\n"
             << "  \"observations\": " << local.observations << ",\n"
             << "  \"initial_mean_reprojection_error\": "
             << local.initial_mean_reprojection_error << ",\n"
             << "  \"final_mean_reprojection_error\": "
             << local.final_mean_reprojection_error << ",\n"
             << "  \"max_translation_correction_meters\": "
             << local.max_translation_correction_meters << ",\n"
             << "  \"max_rotation_correction_degrees\": "
             << local.max_rotation_correction_degrees << ",\n"
             << "  \"measured_scale_drift_ratio\": "
             << local.measured_scale_drift_ratio << "\n}\n";
    WriteDeterministicText(evidence_path, evidence.str());
    local.export_seconds = Seconds(Clock::now() - export_start).count();
    local.status = COLMAPKIT_STATUS_OK;
    CopyText<ColmapKitTrackedPoseResultV2>(local.message, sizeof(local.message),
                                          "Tracked-pose V2 reconstruction completed.");
    Emit(config.progress_callback, config.progress_user_data,
         COLMAPKIT_PROGRESS_STAGE_V2_FINISHED, overall_start,
         "Tracked-pose V2 reconstruction completed.", 1.0);
  } catch (const CancelledError& error) {
    local.status = COLMAPKIT_STATUS_CANCELLED;
    CopyText<ColmapKitTrackedPoseResultV2>(local.message, sizeof(local.message), error.what());
    Emit(config.progress_callback, config.progress_user_data,
         COLMAPKIT_PROGRESS_STAGE_V2_CANCELLED, overall_start, error.what());
  } catch (const std::invalid_argument& error) {
    local.status = COLMAPKIT_STATUS_INVALID_ARGUMENT;
    CopyText<ColmapKitTrackedPoseResultV2>(local.message, sizeof(local.message), error.what());
    Emit(config.progress_callback, config.progress_user_data,
         COLMAPKIT_PROGRESS_STAGE_V2_FAILED, overall_start, error.what());
  } catch (const std::exception& error) {
    local.status = COLMAPKIT_STATUS_RUNTIME_ERROR;
    CopyText<ColmapKitTrackedPoseResultV2>(local.message, sizeof(local.message), error.what());
    Emit(config.progress_callback, config.progress_user_data,
         COLMAPKIT_PROGRESS_STAGE_V2_FAILED, overall_start, error.what());
  }
  *result = local;
  return static_cast<ColmapKitStatus>(local.status);
}

struct PriorPoint {
  Eigen::Vector3d xyz = Eigen::Vector3d::Zero();
  Eigen::Vector3d rgb = Eigen::Vector3d::Zero();
  double confidence = 0.0;
  uint32_t provenance = 0;  // 0 = refined sparse track, 1 = RGB correspondence.
  double spacing = 0.0;
  Eigen::Quaterniond rotation = Eigen::Quaterniond::Identity();
  Eigen::Vector3d scales = Eigen::Vector3d::Ones();
};

struct CellKey {
  int64_t x, y, z;
  bool operator<(const CellKey& other) const {
    return std::tie(x, y, z) < std::tie(other.x, other.y, other.z);
  }
};

CellKey Quantize(const Eigen::Vector3d& xyz, double size) {
  return {static_cast<int64_t>(std::floor(xyz.x() / size)),
          static_cast<int64_t>(std::floor(xyz.y() / size)),
          static_cast<int64_t>(std::floor(xyz.z() / size))};
}

Eigen::Vector3d SampleColor(const colmap::Bitmap& bitmap,
                            const Eigen::Vector2d& xy) {
  if (const auto color = bitmap.InterpolateBilinear(xy.x() - 0.5,
                                                     xy.y() - 0.5)) {
    return Eigen::Vector3d(color->r, color->g, color->b);
  }
  return Eigen::Vector3d::Zero();
}

std::vector<PriorPoint> CorrespondenceCandidates(
    const ColmapKitRGBPriorConfigV2& config,
    const colmap::Reconstruction& reconstruction,
    const colmap::Database& database,
    Cancellation* cancellation,
    uint64_t* total_candidates,
    uint64_t* rejected_candidates) {
  std::unordered_map<std::string, std::filesystem::path> input_paths;
  for (uint32_t i = 0; i < config.num_images; ++i) {
    input_paths.emplace(RelativeImageName(config.images[i]),
                        config.images[i].image_path);
  }
  std::unordered_map<colmap::image_t, colmap::Bitmap> bitmaps;
  for (const auto image_id : reconstruction.RegImageIds()) {
    const auto& image = reconstruction.Image(image_id);
    auto path_it = input_paths.find(image.Name());
    if (path_it == input_paths.end()) {
      throw std::invalid_argument("Prior RGB inputs do not match the refined model.");
    }
    colmap::Bitmap bitmap;
    if (!bitmap.Read(path_it->second, true)) {
      throw std::runtime_error("Cannot read RGB prior input: " + path_it->second.string());
    }
    bitmaps.emplace(image_id, std::move(bitmap));
  }

  std::vector<PriorPoint> points;
  points.reserve(reconstruction.NumPoints3D() * 2);
  std::vector<colmap::point3D_t> sparse_ids(reconstruction.Point3DIds().begin(),
                                            reconstruction.Point3DIds().end());
  std::sort(sparse_ids.begin(), sparse_ids.end());
  for (const auto point_id : sparse_ids) {
    const auto& point = reconstruction.Point3D(point_id);
    PriorPoint candidate;
    candidate.xyz = point.xyz;
    candidate.rgb = point.color.cast<double>();
    candidate.confidence = std::min(1.0, point.track.Length() / 5.0);
    candidate.provenance = 0;
    points.push_back(candidate);
  }

  auto geometries = database.ReadTwoViewGeometries();
  std::sort(geometries.begin(), geometries.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  for (const auto& [pair_id, geometry] : geometries) {
    cancellation->ThrowIfRequested();
    const auto [image_id1, image_id2] = colmap::PairIdToImagePair(pair_id);
    if (!reconstruction.ExistsImage(image_id1) ||
        !reconstruction.ExistsImage(image_id2)) continue;
    const auto& image1 = reconstruction.Image(image_id1);
    const auto& image2 = reconstruction.Image(image_id2);
    const auto& camera1 = *image1.CameraPtr();
    const auto& camera2 = *image2.CameraPtr();
    const auto& keys1 = database.ReadKeypoints(image_id1);
    const auto& keys2 = database.ReadKeypoints(image_id2);
    const auto pose1 = image1.CamFromWorld();
    const auto pose2 = image2.CamFromWorld();
    const double baseline = (CameraCenter(pose1) - CameraCenter(pose2)).norm();
    for (const auto& match : geometry.inlier_matches) {
      ++*total_candidates;
      if (match.point2D_idx1 >= keys1.size() ||
          match.point2D_idx2 >= keys2.size()) {
        ++*rejected_candidates;
        continue;
      }
      const Eigen::Vector2d xy1(keys1[match.point2D_idx1].x,
                                keys1[match.point2D_idx1].y);
      const Eigen::Vector2d xy2(keys2[match.point2D_idx2].x,
                                keys2[match.point2D_idx2].y);
      const auto cam1 = camera1.CamFromImg(xy1);
      const auto cam2 = camera2.CamFromImg(xy2);
      Eigen::Vector3d xyz;
      if (!cam1.has_value() || !cam2.has_value() ||
          !colmap::TriangulatePoint(pose1.ToMatrix(), pose2.ToMatrix(),
                                    *cam1, *cam2, &xyz) ||
          !xyz.allFinite() ||
          !colmap::HasPointPositiveDepth(pose1.ToMatrix(), xyz) ||
          !colmap::HasPointPositiveDepth(pose2.ToMatrix(), xyz) ||
          colmap::CalculateSquaredReprojectionError(xy1, xyz, pose1, camera1) > 16.0 ||
          colmap::CalculateSquaredReprojectionError(xy2, xyz, pose2, camera2) > 16.0) {
        ++*rejected_candidates;
        continue;
      }
      const Eigen::Vector3d rgb =
          0.5 * (SampleColor(bitmaps.at(image_id1), xy1) +
                 SampleColor(bitmaps.at(image_id2), xy2));
      PriorPoint candidate;
      candidate.xyz = xyz;
      candidate.rgb = rgb;
      candidate.confidence = std::clamp(baseline / (baseline + 0.1), 0.1, 1.0);
      candidate.provenance = 1;
      points.push_back(candidate);
    }
  }
  return points;
}

std::vector<PriorPoint> ApplySpatialCaps(std::vector<PriorPoint> points,
                                         const ColmapKitRGBPriorConfigV2& config,
                                         uint64_t* capped) {
  std::stable_sort(points.begin(), points.end(), [](const PriorPoint& a,
                                                    const PriorPoint& b) {
    if (a.provenance != b.provenance) return a.provenance < b.provenance;
    if (a.confidence != b.confidence) return a.confidence > b.confidence;
    return std::tie(a.xyz.x(), a.xyz.y(), a.xyz.z()) <
           std::tie(b.xyz.x(), b.xyz.y(), b.xyz.z());
  });
  std::map<CellKey, uint32_t> cell_counts;
  std::set<CellKey> dedupe;
  std::vector<PriorPoint> output;
  output.reserve(std::min<size_t>(points.size(), config.max_output_gaussians));
  const double dedupe_size = std::max(config.min_spacing_meters * 0.25, 1e-7);
  for (auto& point : points) {
    const CellKey duplicate_key = Quantize(point.xyz, dedupe_size);
    const CellKey density_key = Quantize(point.xyz, config.spatial_cell_size_meters);
    if (!dedupe.insert(duplicate_key).second ||
        cell_counts[density_key] >= config.max_points_per_spatial_cell ||
        output.size() >= config.max_output_gaussians) {
      ++*capped;
      continue;
    }
    ++cell_counts[density_key];
    output.push_back(std::move(point));
  }
  return output;
}

void EstimateSurfaceGeometry(std::vector<PriorPoint>* points,
                             const ColmapKitRGBPriorConfigV2& config,
                             Cancellation* cancellation) {
  if (points->size() < 4) {
    throw std::runtime_error("Too few points for surface-aware prior geometry.");
  }
  const size_t k = std::min<size_t>(config.normal_neighbor_count,
                                    points->size() - 1);
  for (size_t i = 0; i < points->size(); ++i) {
    if ((i & 255u) == 0) cancellation->ThrowIfRequested();
    std::vector<std::pair<double, size_t>> distances;
    distances.reserve(points->size() - 1);
    for (size_t j = 0; j < points->size(); ++j) {
      if (i == j) continue;
      distances.emplace_back(((*points)[j].xyz - (*points)[i].xyz).squaredNorm(), j);
    }
    std::partial_sort(distances.begin(), distances.begin() + k, distances.end());
    const size_t median_index = (k - 1) / 2;
    std::nth_element(distances.begin(), distances.begin() + median_index,
                     distances.begin() + k);
    const double median_squared_distance = distances[median_index].first;
    const double robust_limit = std::max(1e-18, 6.25 * median_squared_distance);
    std::vector<size_t> robust_neighbors;
    robust_neighbors.reserve(k);
    for (size_t n = 0; n < k; ++n) {
      if (distances[n].first <= robust_limit) {
        robust_neighbors.push_back(distances[n].second);
      }
    }
    if (robust_neighbors.size() < 3) {
      robust_neighbors.clear();
      for (size_t n = 0; n < std::min<size_t>(3, k); ++n) {
        robust_neighbors.push_back(distances[n].second);
      }
    }
    Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
    for (const size_t neighbor : robust_neighbors) {
      centroid += (*points)[neighbor].xyz;
    }
    centroid /= static_cast<double>(robust_neighbors.size());
    Eigen::Matrix3d covariance = Eigen::Matrix3d::Zero();
    for (const size_t neighbor : robust_neighbors) {
      const Eigen::Vector3d delta = (*points)[neighbor].xyz - centroid;
      covariance += delta * delta.transpose();
    }
    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> eigensolver(covariance);
    if (eigensolver.info() != Eigen::Success) {
      throw std::runtime_error("Neighborhood normal estimation failed.");
    }
    Eigen::Vector3d normal = eigensolver.eigenvectors().col(0).normalized();
    Eigen::Vector3d tangent0 = eigensolver.eigenvectors().col(2).normalized();
    if (normal.z() < 0 || (normal.z() == 0 && normal.y() < 0) ||
        (normal.z() == 0 && normal.y() == 0 && normal.x() < 0)) {
      normal = -normal;
    }
    if (tangent0.x() < 0 ||
        (tangent0.x() == 0 && tangent0.y() < 0)) tangent0 = -tangent0;
    Eigen::Vector3d tangent1 = normal.cross(tangent0).normalized();
    tangent0 = tangent1.cross(normal).normalized();
    Eigen::Matrix3d orientation;
    orientation.col(0) = tangent0;
    orientation.col(1) = tangent1;
    orientation.col(2) = normal;
    (*points)[i].rotation = Eigen::Quaterniond(orientation).normalized();
    const double spacing = std::clamp(std::sqrt(median_squared_distance),
                                      config.min_spacing_meters,
                                      config.max_spacing_meters);
    (*points)[i].spacing = spacing;
    (*points)[i].scales = Eigen::Vector3d(
        spacing * config.tangent_scale_multiplier,
        spacing * config.tangent_scale_multiplier,
        spacing * config.normal_scale_multiplier);
  }
}

template <typename T>
void WriteBinary(std::ofstream& stream, const T& value) {
  stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void WriteGaussianPLY(const std::filesystem::path& path,
                      const std::vector<PriorPoint>& points,
                      double opacity) {
  EnsureParent(path);
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) throw std::runtime_error("Cannot write prior PLY: " + path.string());
  stream.imbue(std::locale::classic());
  stream << "ply\nformat binary_little_endian 1.0\n"
         << "comment ColmapKit Variant D bounded RGB surface prior\n"
         << "comment sh_degree 0\n"
         << "element vertex " << points.size() << "\n"
         << "property float x\nproperty float y\nproperty float z\n"
         << "property float scale_0\nproperty float scale_1\nproperty float scale_2\n"
         << "property float opacity\n"
         << "property float rot_0\nproperty float rot_1\nproperty float rot_2\nproperty float rot_3\n"
         << "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\n"
         << "end_header\n";
  constexpr double kSHC0 = 0.28209479177387814;
  const float opacity_logit = static_cast<float>(std::log(opacity / (1.0 - opacity)));
  for (const auto& point : points) {
    const std::array<float, 14> fields = {
        static_cast<float>(point.xyz.x()), static_cast<float>(point.xyz.y()),
        static_cast<float>(point.xyz.z()),
        static_cast<float>(std::log(point.scales.x())),
        static_cast<float>(std::log(point.scales.y())),
        static_cast<float>(std::log(point.scales.z())), opacity_logit,
        static_cast<float>(point.rotation.w()),
        static_cast<float>(point.rotation.x()),
        static_cast<float>(point.rotation.y()),
        static_cast<float>(point.rotation.z()),
        static_cast<float>((point.rgb.x() / 255.0 - 0.5) / kSHC0),
        static_cast<float>((point.rgb.y() / 255.0 - 0.5) / kSHC0),
        static_cast<float>((point.rgb.z() / 255.0 - 0.5) / kSHC0)};
    for (const float value : fields) WriteBinary(stream, value);
  }
  if (!stream) throw std::runtime_error("Failed writing prior PLY: " + path.string());
}

void ValidatePriorConfig(const ColmapKitRGBPriorConfigV2& config) {
  if (config.struct_size < kPriorConfigMinimumSize || config.images == nullptr ||
      config.num_images < 3 || config.database_path == nullptr ||
      config.refined_model_path == nullptr || config.refined_pose_path == nullptr ||
      config.expected_refined_pose_sha256 == nullptr ||
      config.output_ply_path == nullptr || config.evidence_path == nullptr) {
    throw std::invalid_argument("RGB prior V2 is missing a required prefix or path.");
  }
  if (std::strlen(config.expected_refined_pose_sha256) != 64 ||
      config.normal_neighbor_count < 3 || config.max_points_per_spatial_cell == 0 ||
      config.max_output_gaussians == 0 || config.minimum_densification_percent == 0 ||
      config.spatial_cell_size_meters <= 0 || config.min_spacing_meters <= 0 ||
      config.max_spacing_meters < config.min_spacing_meters ||
      config.tangent_scale_multiplier <= config.normal_scale_multiplier ||
      config.normal_scale_multiplier <= 0 || config.initial_opacity <= 0 ||
      config.initial_opacity >= 1) {
    throw std::invalid_argument("RGB prior V2 bounds are invalid.");
  }
  for (uint32_t i = 0; i < config.num_images; ++i) ValidateImage(config.images[i]);
  (void)StableImageOrder(config.images, config.num_images);
  std::vector<std::filesystem::path> input_paths = {
      config.database_path, config.refined_pose_path};
  input_paths.reserve(config.num_images + 2);
  for (uint32_t i = 0; i < config.num_images; ++i) {
    input_paths.emplace_back(config.images[i].image_path);
  }
  ValidateDistinctPaths(input_paths,
                        {config.output_ply_path, config.evidence_path},
                        {},
                        {config.refined_model_path});
}

ColmapKitStatus RunPrior(const ColmapKitRGBPriorConfigV2& config,
                         Cancellation* cancellation,
                         ColmapKitRGBPriorResultV2* result) {
  ColmapKitRGBPriorResultV2 local{};
  local.struct_size = sizeof(local);
  local.status = COLMAPKIT_STATUS_RUNTIME_ERROR;
  local.variant = COLMAPKIT_RGB_PRIOR_VARIANT_V2_D;
  local.sh_degree = 0;
  const auto overall_start = Clock::now();
  try {
    ValidatePriorConfig(config);
    cancellation->ThrowIfRequested();
    const std::filesystem::path pose_path(config.refined_pose_path);
    const std::string before_pose_sha = FileSHA256(pose_path);
    CopyText<ColmapKitRGBPriorResultV2>(local.input_pose_sha256,
                                       sizeof(local.input_pose_sha256),
                                       before_pose_sha);
    if (before_pose_sha != config.expected_refined_pose_sha256) {
      throw std::invalid_argument("Refined pose checksum does not match the frozen input.");
    }
    const auto order = StableImageOrder(config.images, config.num_images);
    const std::string rgb_manifest_sha = RGBManifestSHA(config.images, order);
    const std::string pose_json = ReadFile(pose_path);
    if (pose_json.find("\"rgb_manifest_sha256\": \"" + rgb_manifest_sha +
                       "\"") == std::string::npos) {
      throw std::invalid_argument(
          "Prior RGB inputs do not match the frozen refined-pose RGB manifest.");
    }
    colmap::Reconstruction reconstruction;
    reconstruction.Read(config.refined_model_path);
    auto database = colmap::Database::Open(config.database_path);
    reconstruction.TranscribeImageIdsToDatabase(*database);
    local.sparse_input_points = reconstruction.NumPoints3D();
    if (local.sparse_input_points == 0) {
      throw std::runtime_error("Refined model contains no sparse points.");
    }

    const auto densify_start = Clock::now();
    Emit(config.progress_callback, config.progress_user_data,
         COLMAPKIT_PROGRESS_STAGE_V2_PRIOR_DENSIFICATION, overall_start,
         "Triangulating deterministic verified RGB correspondences with frozen cameras.");
    auto points = CorrespondenceCandidates(config, reconstruction, *database,
                                           cancellation,
                                           &local.correspondence_candidates,
                                           &local.rejected_candidates);
    points = ApplySpatialCaps(std::move(points), config,
                             &local.density_capped_points);
    local.densification_seconds = Seconds(Clock::now() - densify_start).count();
    local.output_gaussians = points.size();
    local.densification_ratio = static_cast<double>(points.size()) /
                                local.sparse_input_points;
    const double minimum_ratio =
        1.0 + static_cast<double>(config.minimum_densification_percent) / 100.0;
    if (local.densification_ratio < minimum_ratio) {
      throw std::runtime_error(
          "Bounded RGB correspondences did not meet the configured material-densification gate; Variant D was not emitted.");
    }

    const auto geometry_start = Clock::now();
    Emit(config.progress_callback, config.progress_user_data,
         COLMAPKIT_PROGRESS_STAGE_V2_PRIOR_GEOMETRY, overall_start,
         "Estimating robust local spacing, normals, and tangent anisotropy.");
    EstimateSurfaceGeometry(&points, config, cancellation);
    std::vector<double> spacings, anisotropies;
    spacings.reserve(points.size());
    anisotropies.reserve(points.size());
    for (const auto& point : points) {
      spacings.push_back(point.spacing);
      anisotropies.push_back(point.scales.x() / point.scales.z());
    }
    const size_t median = points.size() / 2;
    std::nth_element(spacings.begin(), spacings.begin() + median, spacings.end());
    std::nth_element(anisotropies.begin(), anisotropies.begin() + median,
                     anisotropies.end());
    local.median_spacing_meters = spacings[median];
    local.median_anisotropy_ratio = anisotropies[median];
    local.geometry_seconds = Seconds(Clock::now() - geometry_start).count();

    const auto export_start = Clock::now();
    WriteGaussianPLY(config.output_ply_path, points, config.initial_opacity);
    const std::string ply_sha = FileSHA256(config.output_ply_path);
    const std::string after_pose_sha = FileSHA256(pose_path);
    CopyText<ColmapKitRGBPriorResultV2>(local.output_pose_sha256,
                                       sizeof(local.output_pose_sha256),
                                       after_pose_sha);
    CopyText<ColmapKitRGBPriorResultV2>(local.output_ply_sha256,
                                       sizeof(local.output_ply_sha256), ply_sha);
    if (after_pose_sha != before_pose_sha) {
      throw std::runtime_error("Frozen refined pose artifact changed during prior generation.");
    }
    uint64_t sparse_count = 0, correspondence_count = 0;
    double confidence_sum = 0.0;
    for (const auto& point : points) {
      point.provenance == 0 ? ++sparse_count : ++correspondence_count;
      confidence_sum += point.confidence;
    }
    std::ostringstream evidence;
    evidence.imbue(std::locale::classic());
    evidence << std::setprecision(17)
             << "{\n  \"schema\": \"colmapkit.rgb-surface-prior-evidence.v2\",\n"
             << "  \"variant\": \"D\",\n"
             << "  \"route\": \"bounded_multiview_rgb_correspondence_densification\",\n"
             << "  \"plane_sweep_used\": false,\n"
             << "  \"dense_mvs_used\": false,\n"
             << "  \"depth_used\": false,\n"
             << "  \"poses_frozen\": true,\n"
             << "  \"input_pose_sha256\": \"" << before_pose_sha << "\",\n"
             << "  \"output_pose_sha256\": \"" << after_pose_sha << "\",\n"
             << "  \"rgb_manifest_sha256\": \"" << rgb_manifest_sha << "\",\n"
             << "  \"output_ply_sha256\": \"" << ply_sha << "\",\n"
             << "  \"sh_degree\": 0,\n"
             << "  \"color_encoding\": \"f_dc_rgb\",\n"
             << "  \"higher_sh_bands\": \"absent_zero_pad_downstream\",\n"
             << "  \"sparse_input_points\": " << local.sparse_input_points << ",\n"
             << "  \"output_gaussians\": " << local.output_gaussians << ",\n"
             << "  \"densification_ratio\": " << local.densification_ratio << ",\n"
             << "  \"provenance_sparse\": " << sparse_count << ",\n"
             << "  \"provenance_rgb_correspondence\": " << correspondence_count << ",\n"
             << "  \"mean_confidence\": " << confidence_sum / points.size() << ",\n"
             << "  \"median_spacing_meters\": " << local.median_spacing_meters << ",\n"
             << "  \"median_anisotropy_ratio\": " << local.median_anisotropy_ratio << ",\n"
             << "  \"density_capped_points\": " << local.density_capped_points << ",\n"
             << "  \"random_seed\": " << config.random_seed << "\n}\n";
    WriteDeterministicText(config.evidence_path, evidence.str());
    local.export_seconds = Seconds(Clock::now() - export_start).count();
    local.status = COLMAPKIT_STATUS_OK;
    CopyText<ColmapKitRGBPriorResultV2>(local.message, sizeof(local.message),
                                       "Variant D RGB surface-aware Gaussian prior completed.");
    Emit(config.progress_callback, config.progress_user_data,
         COLMAPKIT_PROGRESS_STAGE_V2_FINISHED, overall_start,
         "Variant D RGB surface-aware Gaussian prior completed.", 1.0);
  } catch (const CancelledError& error) {
    local.status = COLMAPKIT_STATUS_CANCELLED;
    CopyText<ColmapKitRGBPriorResultV2>(local.message, sizeof(local.message), error.what());
    Emit(config.progress_callback, config.progress_user_data,
         COLMAPKIT_PROGRESS_STAGE_V2_CANCELLED, overall_start, error.what());
  } catch (const std::invalid_argument& error) {
    local.status = COLMAPKIT_STATUS_INVALID_ARGUMENT;
    CopyText<ColmapKitRGBPriorResultV2>(local.message, sizeof(local.message), error.what());
    Emit(config.progress_callback, config.progress_user_data,
         COLMAPKIT_PROGRESS_STAGE_V2_FAILED, overall_start, error.what());
  } catch (const std::exception& error) {
    local.status = COLMAPKIT_STATUS_RUNTIME_ERROR;
    CopyText<ColmapKitRGBPriorResultV2>(local.message, sizeof(local.message), error.what());
    Emit(config.progress_callback, config.progress_user_data,
         COLMAPKIT_PROGRESS_STAGE_V2_FAILED, overall_start, error.what());
  }
  *result = local;
  return static_cast<ColmapKitStatus>(local.status);
}

}  // namespace

struct ColmapKitTrackedPoseJobV2 {
  OwnedTrackedConfig config;
  Cancellation cancellation;
  ColmapKitTrackedPoseResultV2 result{};
  std::thread worker;
};

struct ColmapKitRGBPriorJobV2 {
  OwnedPriorConfig config;
  Cancellation cancellation;
  ColmapKitRGBPriorResultV2 result{};
  std::thread worker;
};

extern "C" {

uint32_t ColmapKitGetABIVersionV2(void) { return COLMAPKIT_ABI_VERSION_V2; }

const char* ColmapKitGetReleaseVersionV2(void) { return kReleaseVersion; }

const char* ColmapKitGetEngineBuildIdentityV2(void) {
  static const std::string identity =
      colmap::GetVersionInfo() + " (" + colmap::GetBuildInfo() + ")";
  return identity.c_str();
}

ColmapKitStatus ColmapKitRunTrackedPoseReconstructionV2(
    const ColmapKitTrackedPoseConfigV2* config,
    ColmapKitTrackedPoseResultV2* result) {
  if (config == nullptr || !PrepareResult(result, kResultMinimumSize)) {
    return COLMAPKIT_STATUS_INVALID_ARGUMENT;
  }
  ColmapKitTrackedPoseResultV2 local{};
  Cancellation cancellation;
  const auto bounded_config = BoundedInputCopy(*config);
  const ColmapKitStatus status =
      RunTracked(bounded_config, &cancellation, &local);
  CopyResult(local, result);
  return status;
}

ColmapKitStatus ColmapKitStartTrackedPoseReconstructionV2(
    const ColmapKitTrackedPoseConfigV2* config, ColmapKitTrackedPoseJobV2** job) {
  if (config == nullptr || job == nullptr ||
      config->struct_size < kTrackedConfigMinimumSize || config->images == nullptr) {
    return COLMAPKIT_STATUS_INVALID_ARGUMENT;
  }
  try {
    auto owned = std::make_unique<ColmapKitTrackedPoseJobV2>();
    owned->config = OwnedTrackedConfig::Copy(*config);
    auto* raw = owned.get();
    owned->worker = std::thread([raw]() {
      raw->config.Refresh();
      RunTracked(raw->config.value, &raw->cancellation, &raw->result);
    });
    *job = owned.release();
    return COLMAPKIT_STATUS_OK;
  } catch (const std::exception&) {
    return COLMAPKIT_STATUS_INVALID_ARGUMENT;
  }
}

ColmapKitStatus ColmapKitCancelTrackedPoseReconstructionV2(
    ColmapKitTrackedPoseJobV2* job) {
  if (job == nullptr) return COLMAPKIT_STATUS_INVALID_ARGUMENT;
  job->cancellation.Cancel();
  return COLMAPKIT_STATUS_OK;
}

ColmapKitStatus ColmapKitWaitTrackedPoseReconstructionV2(
    ColmapKitTrackedPoseJobV2* job, ColmapKitTrackedPoseResultV2* result) {
  if (job == nullptr || !PrepareResult(result, kResultMinimumSize)) {
    return COLMAPKIT_STATUS_INVALID_ARGUMENT;
  }
  if (job->worker.joinable()) job->worker.join();
  CopyResult(job->result, result);
  return static_cast<ColmapKitStatus>(job->result.status);
}

void ColmapKitReleaseTrackedPoseReconstructionJobV2(
    ColmapKitTrackedPoseJobV2* job) {
  if (job == nullptr) return;
  if (job->worker.joinable()) job->worker.join();
  delete job;
}

ColmapKitStatus ColmapKitRunRGBGaussianPriorV2(
    const ColmapKitRGBPriorConfigV2* config,
    ColmapKitRGBPriorResultV2* result) {
  if (config == nullptr || !PrepareResult(result, kPriorResultMinimumSize)) {
    return COLMAPKIT_STATUS_INVALID_ARGUMENT;
  }
  ColmapKitRGBPriorResultV2 local{};
  Cancellation cancellation;
  const auto bounded_config = BoundedInputCopy(*config);
  const ColmapKitStatus status =
      RunPrior(bounded_config, &cancellation, &local);
  CopyResult(local, result);
  return status;
}

ColmapKitStatus ColmapKitStartRGBGaussianPriorV2(
    const ColmapKitRGBPriorConfigV2* config, ColmapKitRGBPriorJobV2** job) {
  if (config == nullptr || job == nullptr ||
      config->struct_size < kPriorConfigMinimumSize || config->images == nullptr) {
    return COLMAPKIT_STATUS_INVALID_ARGUMENT;
  }
  try {
    auto owned = std::make_unique<ColmapKitRGBPriorJobV2>();
    owned->config = OwnedPriorConfig::Copy(*config);
    auto* raw = owned.get();
    owned->worker = std::thread([raw]() {
      raw->config.Refresh();
      RunPrior(raw->config.value, &raw->cancellation, &raw->result);
    });
    *job = owned.release();
    return COLMAPKIT_STATUS_OK;
  } catch (const std::exception&) {
    return COLMAPKIT_STATUS_INVALID_ARGUMENT;
  }
}

ColmapKitStatus ColmapKitCancelRGBGaussianPriorV2(ColmapKitRGBPriorJobV2* job) {
  if (job == nullptr) return COLMAPKIT_STATUS_INVALID_ARGUMENT;
  job->cancellation.Cancel();
  return COLMAPKIT_STATUS_OK;
}

ColmapKitStatus ColmapKitWaitRGBGaussianPriorV2(
    ColmapKitRGBPriorJobV2* job, ColmapKitRGBPriorResultV2* result) {
  if (job == nullptr || !PrepareResult(result, kPriorResultMinimumSize)) {
    return COLMAPKIT_STATUS_INVALID_ARGUMENT;
  }
  if (job->worker.joinable()) job->worker.join();
  CopyResult(job->result, result);
  return static_cast<ColmapKitStatus>(job->result.status);
}

void ColmapKitReleaseRGBGaussianPriorJobV2(ColmapKitRGBPriorJobV2* job) {
  if (job == nullptr) return;
  if (job->worker.joinable()) job->worker.join();
  delete job;
}

}  // extern "C"
