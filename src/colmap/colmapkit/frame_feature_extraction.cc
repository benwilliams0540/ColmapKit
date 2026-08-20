// Copyright (c), ETH Zurich and UNC Chapel Hill.
// All rights reserved.

#include "colmap/colmapkit/colmapkit.h"
#include "colmap/colmapkit/frame_feature_extraction_internal.h"
#include "colmap/feature/extractor.h"
#include "colmap/feature/sift.h"
#include "colmap/sensor/bitmap.h"
#include "colmap/util/version.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <jpeglib.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace colmap::internal {
namespace {

uint32_t RotateRight(const uint32_t value, const uint32_t count) {
  return (value >> count) | (value << (32 - count));
}

}  // namespace

std::string FrameFeatureSHA256(const std::string_view input) {
  static constexpr std::array<uint32_t, 64> k = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
      0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
      0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
      0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
      0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
      0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
      0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
      0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  std::vector<uint8_t> message(input.begin(), input.end());
  const uint64_t bit_length = static_cast<uint64_t>(message.size()) * 8;
  message.push_back(0x80);
  while ((message.size() % 64) != 56) message.push_back(0);
  for (int shift = 56; shift >= 0; shift -= 8) {
    message.push_back(static_cast<uint8_t>(bit_length >> shift));
  }
  std::array<uint32_t, 8> hash = {0x6a09e667,
                                  0xbb67ae85,
                                  0x3c6ef372,
                                  0xa54ff53a,
                                  0x510e527f,
                                  0x9b05688c,
                                  0x1f83d9ab,
                                  0x5be0cd19};
  for (size_t offset = 0; offset < message.size(); offset += 64) {
    std::array<uint32_t, 64> words{};
    for (int i = 0; i < 16; ++i) {
      const size_t position = offset + i * 4;
      words[i] = (static_cast<uint32_t>(message[position]) << 24) |
                 (static_cast<uint32_t>(message[position + 1]) << 16) |
                 (static_cast<uint32_t>(message[position + 2]) << 8) |
                 static_cast<uint32_t>(message[position + 3]);
    }
    for (int i = 16; i < 64; ++i) {
      const uint32_t sigma0 = RotateRight(words[i - 15], 7) ^
                              RotateRight(words[i - 15], 18) ^
                              (words[i - 15] >> 3);
      const uint32_t sigma1 = RotateRight(words[i - 2], 17) ^
                              RotateRight(words[i - 2], 19) ^
                              (words[i - 2] >> 10);
      words[i] = words[i - 16] + sigma0 + words[i - 7] + sigma1;
    }
    auto state = hash;
    for (int i = 0; i < 64; ++i) {
      const uint32_t sum1 = RotateRight(state[4], 6) ^
                            RotateRight(state[4], 11) ^
                            RotateRight(state[4], 25);
      const uint32_t choice = (state[4] & state[5]) ^ (~state[4] & state[6]);
      const uint32_t temporary1 = state[7] + sum1 + choice + k[i] + words[i];
      const uint32_t sum0 = RotateRight(state[0], 2) ^
                            RotateRight(state[0], 13) ^
                            RotateRight(state[0], 22);
      const uint32_t majority =
          (state[0] & state[1]) ^ (state[0] & state[2]) ^ (state[1] & state[2]);
      const uint32_t temporary2 = sum0 + majority;
      state = {temporary1 + temporary2,
               state[0],
               state[1],
               state[2],
               state[3] + temporary1,
               state[4],
               state[5],
               state[6]};
    }
    for (int i = 0; i < 8; ++i) hash[i] += state[i];
  }
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const uint32_t word : hash) output << std::setw(8) << word;
  return output.str();
}

}  // namespace colmap::internal

namespace {

using Clock = std::chrono::steady_clock;
using Seconds = std::chrono::duration<double>;

constexpr std::array<uint8_t, 8> kArtifactMagic = {
    'C', 'K', 'F', 'E', 'A', 'T', '1', 0};
constexpr std::array<uint8_t, 8> kCompletionMarker = {
    'C', 'K', 'D', 'O', 'N', 'E', '1', 0};
constexpr uint32_t kArtifactCompleteFlag = 1;
constexpr uint32_t kSiftExtractorType = 1;
constexpr uint32_t kKeypointColumns = 6;
constexpr uint32_t kDescriptorDimensions = 128;
constexpr uint32_t kDescriptorScalarUint8 = 1;
constexpr uint64_t kMaximumEncodedImageBytes = 512ULL * 1024ULL * 1024ULL;
constexpr uint32_t kMaximumImageDimension = 16384;
constexpr uint32_t kMaximumFeatures = 65536;
constexpr size_t kHashHexLength = 64;
constexpr size_t kArtifactFooterSize =
    kHashHexLength + kCompletionMarker.size();
constexpr char kDecoderIdentity[] =
    "libjpeg-native-scale-rgb-to-gray-bilinear-v1";

class CancelledError : public std::runtime_error {
 public:
  CancelledError()
      : std::runtime_error("Frame feature extraction cancelled.") {}
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
  if (value.size() != kHashHexLength) return false;
  return std::all_of(value.begin(), value.end(), [](const char character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'a' && character <= 'f');
  });
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
    throw std::invalid_argument("Frame feature string is too large.");
  }
  AppendLittleEndian<uint32_t>(bytes, static_cast<uint32_t>(value.size()));
  bytes->insert(bytes->end(), value.begin(), value.end());
}

class ByteReader {
 public:
  ByteReader(const std::vector<uint8_t>& bytes, const size_t limit)
      : bytes_(bytes), limit_(limit) {
    if (limit_ > bytes_.size()) {
      throw std::invalid_argument("Truncated frame feature artifact.");
    }
  }

  void Expect(const std::array<uint8_t, 8>& expected,
              const std::string_view description) {
    Require(expected.size());
    if (!std::equal(
            expected.begin(), expected.end(), bytes_.begin() + offset_)) {
      throw std::invalid_argument(std::string("Invalid frame feature ") +
                                  std::string(description) + ".");
    }
    offset_ += expected.size();
  }

  template <typename T>
  T Read() {
    static_assert(std::is_integral_v<T> || std::is_floating_point_v<T>);
    Require(sizeof(T));
    std::array<uint8_t, sizeof(T)> encoded{};
    std::copy_n(bytes_.begin() + offset_, sizeof(T), encoded.begin());
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    std::reverse(encoded.begin(), encoded.end());
#endif
    T value;
    std::memcpy(&value, encoded.data(), sizeof(T));
    offset_ += sizeof(T);
    return value;
  }

  std::string ReadString() {
    const uint32_t size = Read<uint32_t>();
    Require(size);
    const std::string value(bytes_.begin() + offset_,
                            bytes_.begin() + offset_ + size);
    offset_ += size;
    return value;
  }

  std::vector<uint8_t> ReadBytes(const size_t size) {
    Require(size);
    std::vector<uint8_t> value(bytes_.begin() + offset_,
                               bytes_.begin() + offset_ + size);
    offset_ += size;
    return value;
  }

  size_t Offset() const { return offset_; }

 private:
  void Require(const size_t size) const {
    if (size > limit_ - std::min(limit_, offset_)) {
      throw std::invalid_argument("Truncated frame feature artifact.");
    }
  }

  const std::vector<uint8_t>& bytes_;
  size_t limit_;
  size_t offset_ = 0;
};

std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path,
                                   const uint64_t maximum_bytes) {
  std::error_code error;
  const uint64_t size = std::filesystem::file_size(path, error);
  if (error) {
    throw std::invalid_argument("Cannot inspect frame feature artifact.");
  }
  if (size > maximum_bytes) {
    throw std::invalid_argument(
        "Frame feature artifact exceeds the memory bound.");
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::invalid_argument("Cannot read frame feature artifact.");
  }
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream),
                              std::istreambuf_iterator<char>());
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

std::string SourceIdentity() {
  return std::string(ColmapKitGetEngineBuildIdentityV2());
}

std::vector<uint8_t> CanonicalConfigBytes(
    const ColmapKitFrameFeatureExtractorConfigV1& config,
    const std::string_view source_identity) {
  std::vector<uint8_t> bytes;
  AppendLittleEndian<uint32_t>(&bytes, config.requested_backend);
  AppendLittleEndian<uint32_t>(&bytes, config.worker_count);
  AppendLittleEndian<uint32_t>(&bytes, config.max_image_size);
  AppendLittleEndian<uint32_t>(&bytes, config.max_num_features);
  AppendLittleEndian<int32_t>(&bytes, config.first_octave);
  AppendLittleEndian<uint32_t>(&bytes, config.num_octaves);
  AppendLittleEndian<uint32_t>(&bytes, config.octave_resolution);
  AppendLittleEndian<uint32_t>(&bytes, config.max_num_orientations);
  AppendLittleEndian<uint32_t>(&bytes, config.upright);
  AppendLittleEndian<uint32_t>(&bytes, config.normalization);
  AppendLittleEndian<double>(&bytes, config.peak_threshold);
  AppendLittleEndian<double>(&bytes, config.edge_threshold);
  AppendString(&bytes, kDecoderIdentity);
  AppendString(&bytes, source_identity);
  return bytes;
}

std::vector<uint8_t> CanonicalMetadataBytes(
    const ColmapKitFrameMetadataV1& metadata) {
  std::vector<uint8_t> bytes;
  AppendLittleEndian<uint32_t>(&bytes, metadata.image_format);
  AppendLittleEndian<uint32_t>(&bytes, metadata.encoded_width);
  AppendLittleEndian<uint32_t>(&bytes, metadata.encoded_height);
  AppendLittleEndian<uint32_t>(&bytes, metadata.orientation);
  AppendLittleEndian<uint32_t>(&bytes, metadata.camera_model);
  AppendLittleEndian<uint32_t>(&bytes, metadata.num_camera_params);
  for (const double parameter : metadata.camera_params) {
    AppendLittleEndian<double>(&bytes, parameter);
  }
  return bytes;
}

std::string HashBytes(const std::vector<uint8_t>& bytes) {
  return colmap::internal::FrameFeatureSHA256(std::string_view(
      reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

void ValidateConfig(const ColmapKitFrameFeatureExtractorConfigV1& config) {
  if (config.struct_size < sizeof(config) ||
      config.abi_version != COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1) {
    throw std::invalid_argument(
        "Invalid frame feature extractor ABI version or size.");
  }
  if (config.requested_backend != COLMAPKIT_FRAME_FEATURE_BACKEND_V1_CPU) {
    throw std::domain_error("FrameFeatureExtractionV1 supports CPU SIFT only.");
  }
  if (config.worker_count != 1) {
    throw std::invalid_argument(
        "FrameFeatureExtractionV1 requires exactly one worker.");
  }
  if (config.max_encoded_image_bytes == 0 ||
      config.max_encoded_image_bytes > kMaximumEncodedImageBytes ||
      config.memory_admission_budget_bytes == 0) {
    throw std::invalid_argument("Invalid frame feature memory bounds.");
  }
  if (config.max_image_size == 0 ||
      config.max_image_size > kMaximumImageDimension ||
      config.max_num_features == 0 ||
      config.max_num_features > kMaximumFeatures ||
      config.max_num_orientations == 0 || config.max_num_orientations > 4 ||
      config.upright > 1 || config.num_octaves == 0 ||
      config.octave_resolution == 0 || !std::isfinite(config.peak_threshold) ||
      config.peak_threshold <= 0 || !std::isfinite(config.edge_threshold) ||
      config.edge_threshold <= 0 ||
      (config.normalization != COLMAPKIT_SIFT_NORMALIZATION_V1_L1_ROOT &&
       config.normalization != COLMAPKIT_SIFT_NORMALIZATION_V1_L2)) {
    throw std::invalid_argument(
        "Invalid CPU SIFT frame feature configuration.");
  }
  if (std::any_of(std::begin(config.reserved),
                  std::end(config.reserved),
                  [](const uint32_t value) { return value != 0; })) {
    throw std::invalid_argument(
        "Reserved frame feature config fields must be zero.");
  }

  colmap::FeatureExtractionOptions options(colmap::FeatureExtractorType::SIFT);
  options.use_gpu = false;
  options.num_threads = 1;
  options.max_image_size = static_cast<int>(config.max_image_size);
  options.sift->max_num_features = static_cast<int>(config.max_num_features);
  options.sift->first_octave = config.first_octave;
  options.sift->num_octaves = static_cast<int>(config.num_octaves);
  options.sift->octave_resolution = static_cast<int>(config.octave_resolution);
  options.sift->peak_threshold = config.peak_threshold;
  options.sift->edge_threshold = config.edge_threshold;
  options.sift->max_num_orientations =
      static_cast<int>(config.max_num_orientations);
  options.sift->upright = config.upright != 0;
  options.sift->normalization =
      config.normalization == COLMAPKIT_SIFT_NORMALIZATION_V1_L2
          ? colmap::SiftExtractionOptions::Normalization::L2
          : colmap::SiftExtractionOptions::Normalization::L1_ROOT;
  if (!options.Check()) {
    throw std::invalid_argument(
        "CPU SIFT rejected the frame feature configuration.");
  }
}

void ValidateMetadata(const ColmapKitFrameMetadataV1& metadata) {
  if (metadata.struct_size < sizeof(metadata) ||
      metadata.abi_version != COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1) {
    throw std::invalid_argument("Invalid frame metadata ABI version or size.");
  }
  if (metadata.image_format != COLMAPKIT_FRAME_IMAGE_FORMAT_V1_JPEG) {
    throw std::domain_error(
        "FrameFeatureExtractionV1 supports JPEG input only.");
  }
  uint32_t required_camera_params = 0;
  switch (metadata.camera_model) {
    case COLMAPKIT_CAMERA_MODEL_V2_PINHOLE:
      required_camera_params = 4;
      break;
    case COLMAPKIT_CAMERA_MODEL_V2_SIMPLE_PINHOLE:
      required_camera_params = 3;
      break;
    case COLMAPKIT_CAMERA_MODEL_V2_OPENCV:
      required_camera_params = 8;
      break;
    default:
      throw std::invalid_argument("Unknown frame camera model.");
  }
  if (metadata.encoded_width == 0 || metadata.encoded_height == 0 ||
      metadata.encoded_width > kMaximumImageDimension ||
      metadata.encoded_height > kMaximumImageDimension ||
      metadata.orientation != COLMAPKIT_FRAME_ORIENTATION_V1_UP ||
      metadata.num_camera_params != required_camera_params) {
    throw std::invalid_argument("Invalid canonical-upright frame metadata.");
  }
  for (uint32_t i = 0; i < metadata.num_camera_params; ++i) {
    if (!std::isfinite(metadata.camera_params[i])) {
      throw std::invalid_argument("Frame camera parameters must be finite.");
    }
  }
  for (uint32_t i = metadata.num_camera_params;
       i < std::size(metadata.camera_params);
       ++i) {
    if (metadata.camera_params[i] != 0.0) {
      throw std::invalid_argument(
          "Unused frame camera parameters must be canonical zeroes.");
    }
  }
  if (std::any_of(std::begin(metadata.reserved),
                  std::end(metadata.reserved),
                  [](const uint32_t value) { return value != 0; })) {
    throw std::invalid_argument("Reserved frame metadata fields must be zero.");
  }
}

uint64_t CheckedMultiply(const uint64_t left, const uint64_t right) {
  if (left != 0 && right > std::numeric_limits<uint64_t>::max() / left) {
    throw std::invalid_argument("Frame feature memory estimate overflowed.");
  }
  return left * right;
}

uint64_t CheckedAdd(const uint64_t left, const uint64_t right) {
  if (right > std::numeric_limits<uint64_t>::max() - left) {
    throw std::invalid_argument("Frame feature memory estimate overflowed.");
  }
  return left + right;
}

uint64_t EstimateAdmittedMemory(
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
  const uint64_t pixels = CheckedMultiply(width, height);
  const uint64_t first_octave_factor = config.first_octave < 0 ? 4 : 1;
  const uint64_t image_working_set =
      CheckedMultiply(CheckedMultiply(pixels, first_octave_factor), 96);
  const uint64_t feature_working_set = CheckedMultiply(
      CheckedMultiply(config.max_num_features, config.max_num_orientations),
      256);
  return CheckedAdd(
      CheckedAdd(CheckedMultiply(encoded_size, 2), image_working_set),
      CheckedAdd(feature_working_set, 16ULL * 1024ULL * 1024ULL));
}

struct JpegErrorState {
  jpeg_error_mgr manager;
  std::jmp_buf jump;
  uint8_t* pixels = nullptr;
};

void HandleJpegError(j_common_ptr info) {
  auto* state = reinterpret_cast<JpegErrorState*>(info->err);
  std::free(state->pixels);
  state->pixels = nullptr;
  std::longjmp(state->jump, 1);
}

colmap::Bitmap DecodeJpeg(const std::vector<uint8_t>& encoded,
                          const uint32_t maximum_dimension,
                          const uint32_t expected_width,
                          const uint32_t expected_height,
                          int* encoded_width,
                          int* encoded_height) {
  jpeg_decompress_struct info{};
  JpegErrorState error{};
  info.err = jpeg_std_error(&error.manager);
  error.manager.error_exit = HandleJpegError;
  volatile bool created = false;
  if (setjmp(error.jump)) {
    if (created) jpeg_destroy_decompress(&info);
    throw std::invalid_argument("Cannot decode the encoded JPEG frame.");
  }

  jpeg_create_decompress(&info);
  created = true;
  jpeg_mem_src(&info,
               const_cast<unsigned char*>(encoded.data()),
               static_cast<unsigned long>(encoded.size()));
  if (jpeg_read_header(&info, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&info);
    throw std::invalid_argument("Cannot read the encoded JPEG frame header.");
  }
  *encoded_width = static_cast<int>(info.image_width);
  *encoded_height = static_cast<int>(info.image_height);
  if (info.image_width != expected_width ||
      info.image_height != expected_height) {
    jpeg_destroy_decompress(&info);
    throw std::invalid_argument(
        "Decoded JPEG dimensions do not match metadata.");
  }
  info.out_color_space = JCS_RGB;
  if (std::max(*encoded_width, *encoded_height) >
      static_cast<int>(maximum_dimension)) {
    for (const unsigned int denominator : {8u, 4u, 2u, 1u}) {
      info.scale_num = 1;
      info.scale_denom = denominator;
      jpeg_calc_output_dimensions(&info);
      if (std::max(info.output_width, info.output_height) >=
              maximum_dimension ||
          denominator == 1) {
        break;
      }
    }
  }
  jpeg_start_decompress(&info);
  const size_t row_bytes =
      static_cast<size_t>(info.output_width) * info.output_components;
  if (info.output_height != 0 &&
      row_bytes > std::numeric_limits<size_t>::max() / info.output_height) {
    jpeg_destroy_decompress(&info);
    throw std::invalid_argument("Decoded JPEG size overflowed.");
  }
  const size_t pixel_bytes = row_bytes * info.output_height;
  error.pixels = static_cast<uint8_t*>(std::malloc(pixel_bytes));
  if (error.pixels == nullptr) {
    jpeg_destroy_decompress(&info);
    throw std::runtime_error("Cannot allocate the bounded JPEG decode buffer.");
  }
  while (info.output_scanline < info.output_height) {
    JSAMPROW row =
        error.pixels + static_cast<size_t>(info.output_scanline) * row_bytes;
    jpeg_read_scanlines(&info, &row, 1);
  }
  const int output_width = static_cast<int>(info.output_width);
  const int output_height = static_cast<int>(info.output_height);
  jpeg_finish_decompress(&info);
  jpeg_destroy_decompress(&info);
  std::unique_ptr<uint8_t, decltype(&std::free)> pixels(error.pixels,
                                                        &std::free);
  error.pixels = nullptr;
  colmap::Bitmap rgb(output_width, output_height, true);
  if (rgb.RowMajorData().size() != pixel_bytes) {
    throw std::runtime_error("Unexpected bounded JPEG decode layout.");
  }
  std::copy_n(pixels.get(), pixel_bytes, rgb.RowMajorData().data());
  return rgb;
}

colmap::FeatureExtractionOptions MakeOptions(
    const ColmapKitFrameFeatureExtractorConfigV1& config) {
  colmap::FeatureExtractionOptions options(colmap::FeatureExtractorType::SIFT);
  options.max_image_size = static_cast<int>(config.max_image_size);
  options.num_threads = 1;
  options.use_gpu = false;
  options.sift->max_num_features = static_cast<int>(config.max_num_features);
  options.sift->first_octave = config.first_octave;
  options.sift->num_octaves = static_cast<int>(config.num_octaves);
  options.sift->octave_resolution = static_cast<int>(config.octave_resolution);
  options.sift->peak_threshold = config.peak_threshold;
  options.sift->edge_threshold = config.edge_threshold;
  options.sift->max_num_orientations =
      static_cast<int>(config.max_num_orientations);
  options.sift->upright = config.upright != 0;
  options.sift->normalization =
      config.normalization == COLMAPKIT_SIFT_NORMALIZATION_V1_L2
          ? colmap::SiftExtractionOptions::Normalization::L2
          : colmap::SiftExtractionOptions::Normalization::L1_ROOT;
  return options;
}

struct ExtractorState {
  explicit ExtractorState(const ColmapKitFrameFeatureExtractorConfigV1& value)
      : config(value), source_identity(SourceIdentity()) {
    profile_sha256 = HashBytes(CanonicalConfigBytes(config, source_identity));
  }

  ColmapKitFrameFeatureExtractorConfigV1 config{};
  std::string source_identity;
  std::string profile_sha256;
  std::mutex mutex;
  bool active = false;
};

struct OwnedInput {
  uint64_t stable_frame_id = 0;
  uint64_t frame_revision = 0;
  std::vector<uint8_t> encoded;
  std::string expected_image_sha256;
  ColmapKitFrameMetadataV1 metadata{};
  std::filesystem::path output_path;
  ColmapKitFrameFeatureProgressCallbackV1 progress_callback = nullptr;
  void* progress_user_data = nullptr;
  uint64_t admitted_memory_bytes = 0;
};

void InitializeResult(ColmapKitFrameFeatureResultV1* result) {
  *result = {};
  result->struct_size = sizeof(*result);
  result->abi_version = COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1;
  result->actual_backend = COLMAPKIT_FRAME_FEATURE_BACKEND_V1_CPU;
  result->no_fallback_satisfied = 1;
  result->effective_worker_count = 1;
}

void FillError(ColmapKitFrameFeatureErrorV1* error,
               const ColmapKitStatus status,
               const std::string& message) {
  if (error == nullptr ||
      error->struct_size <
          offsetof(ColmapKitFrameFeatureErrorV1, message) + 1 ||
      error->abi_version != COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1) {
    return;
  }
  const uint32_t caller_size = error->struct_size;
  ColmapKitFrameFeatureErrorV1 local{};
  local.struct_size = caller_size;
  local.abi_version = COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1;
  local.error_code = static_cast<uint32_t>(status);
  CopyText(local.message, sizeof(local.message), message);
  std::memcpy(error, &local, std::min<size_t>(caller_size, sizeof(local)));
  error->struct_size = caller_size;
}

void EmitProgress(const OwnedInput& input,
                  const uint32_t stage,
                  const Clock::time_point start,
                  const double fraction,
                  const char* message,
                  const char* detail = "") {
  if (input.progress_callback == nullptr) return;
  ColmapKitFrameFeatureProgressEventV1 event{};
  event.struct_size = sizeof(event);
  event.abi_version = COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1;
  event.stage = stage;
  event.fraction = fraction;
  event.elapsed_seconds = Seconds(Clock::now() - start).count();
  event.current = stage == COLMAPKIT_FRAME_FEATURE_PROGRESS_V1_FINISHED ? 1 : 0;
  event.total = 1;
  event.admitted_memory_bytes = input.admitted_memory_bytes;
  event.message = message;
  event.detail = detail;
  try {
    input.progress_callback(&event, input.progress_user_data);
  } catch (...) {
    // Foreign callbacks are not permitted to throw across the C ABI. Contain
    // an accidental C++ exception so it cannot invalidate a completed artifact.
  }
}

void ThrowIfCancelled(const std::atomic<bool>& cancelled) {
  if (cancelled.load(std::memory_order_acquire)) throw CancelledError();
}

void ScaleKeypoints(const int processed_width,
                    const int processed_height,
                    const uint32_t encoded_width,
                    const uint32_t encoded_height,
                    colmap::FeatureKeypoints* keypoints) {
  const float scale_x =
      static_cast<float>(encoded_width) / static_cast<float>(processed_width);
  const float scale_y =
      static_cast<float>(encoded_height) / static_cast<float>(processed_height);
  if (scale_x != 1.0f || scale_y != 1.0f) {
    for (auto& keypoint : *keypoints) keypoint.Rescale(scale_x, scale_y);
  }
}

std::vector<uint8_t> SerializeArtifact(
    const ExtractorState& state,
    const OwnedInput& input,
    const uint32_t processed_width,
    const uint32_t processed_height,
    const std::string_view image_sha256,
    const std::string_view metadata_sha256,
    const colmap::FeatureKeypoints& keypoints,
    const colmap::FeatureDescriptors& descriptors,
    std::string* payload_sha256) {
  std::vector<uint8_t> bytes;
  bytes.insert(bytes.end(), kArtifactMagic.begin(), kArtifactMagic.end());
  AppendLittleEndian<uint32_t>(
      &bytes, COLMAPKIT_FRAME_FEATURE_ARTIFACT_SCHEMA_VERSION_V1);
  AppendLittleEndian<uint32_t>(&bytes, COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1);
  AppendLittleEndian<uint32_t>(&bytes, kArtifactCompleteFlag);
  AppendLittleEndian<uint64_t>(&bytes, input.stable_frame_id);
  AppendLittleEndian<uint64_t>(&bytes, input.frame_revision);
  AppendLittleEndian<uint32_t>(&bytes, input.metadata.encoded_width);
  AppendLittleEndian<uint32_t>(&bytes, input.metadata.encoded_height);
  AppendLittleEndian<uint32_t>(&bytes, processed_width);
  AppendLittleEndian<uint32_t>(&bytes, processed_height);
  AppendLittleEndian<uint32_t>(&bytes, input.metadata.orientation);
  AppendLittleEndian<uint32_t>(&bytes, input.metadata.camera_model);
  AppendLittleEndian<uint32_t>(&bytes, input.metadata.num_camera_params);
  for (const double parameter : input.metadata.camera_params) {
    AppendLittleEndian<double>(&bytes, parameter);
  }
  AppendLittleEndian<uint32_t>(&bytes, COLMAPKIT_FRAME_FEATURE_BACKEND_V1_CPU);
  AppendLittleEndian<uint32_t>(&bytes, kSiftExtractorType);
  AppendLittleEndian<uint32_t>(&bytes, state.config.normalization);
  AppendLittleEndian<uint32_t>(&bytes, state.config.max_image_size);
  AppendLittleEndian<uint32_t>(&bytes, state.config.max_num_features);
  AppendLittleEndian<int32_t>(&bytes, state.config.first_octave);
  AppendLittleEndian<uint32_t>(&bytes, state.config.num_octaves);
  AppendLittleEndian<uint32_t>(&bytes, state.config.octave_resolution);
  AppendLittleEndian<uint32_t>(&bytes, state.config.max_num_orientations);
  AppendLittleEndian<uint32_t>(&bytes, state.config.upright);
  AppendLittleEndian<double>(&bytes, state.config.peak_threshold);
  AppendLittleEndian<double>(&bytes, state.config.edge_threshold);
  AppendLittleEndian<uint32_t>(&bytes, kKeypointColumns);
  AppendLittleEndian<uint32_t>(&bytes, kDescriptorDimensions);
  AppendLittleEndian<uint32_t>(&bytes, kDescriptorScalarUint8);
  AppendLittleEndian<uint64_t>(&bytes, keypoints.size());
  AppendLittleEndian<uint64_t>(&bytes, descriptors.data.size());
  AppendString(&bytes, image_sha256);
  AppendString(&bytes, metadata_sha256);
  AppendString(&bytes, state.profile_sha256);
  AppendString(&bytes, state.source_identity);
  for (const auto& keypoint : keypoints) {
    AppendLittleEndian<float>(&bytes, keypoint.x);
    AppendLittleEndian<float>(&bytes, keypoint.y);
    AppendLittleEndian<float>(&bytes, keypoint.a11);
    AppendLittleEndian<float>(&bytes, keypoint.a12);
    AppendLittleEndian<float>(&bytes, keypoint.a21);
    AppendLittleEndian<float>(&bytes, keypoint.a22);
  }
  bytes.insert(bytes.end(),
               descriptors.data.data(),
               descriptors.data.data() + descriptors.data.size());
  *payload_sha256 = HashBytes(bytes);
  bytes.insert(bytes.end(), payload_sha256->begin(), payload_sha256->end());
  bytes.insert(bytes.end(), kCompletionMarker.begin(), kCompletionMarker.end());
  return bytes;
}

std::filesystem::path UniqueTemporaryPath(
    const std::filesystem::path& output_path) {
  static std::atomic<uint64_t> sequence{0};
  for (int attempt = 0; attempt < 100; ++attempt) {
    const auto suffix = sequence.fetch_add(1, std::memory_order_relaxed);
    const std::filesystem::path candidate =
        output_path.string() + ".tmp." + std::to_string(suffix);
    if (!std::filesystem::exists(candidate)) return candidate;
  }
  throw std::runtime_error("Cannot allocate a frame feature temporary path.");
}

void WriteArtifactAtomically(const std::filesystem::path& output_path,
                             const std::vector<uint8_t>& bytes,
                             const std::atomic<bool>& cancelled) {
  if (output_path.empty() || output_path.extension() != ".ckfeatures") {
    throw std::invalid_argument(
        "Frame feature output must use the .ckfeatures extension.");
  }
  if (std::filesystem::exists(output_path)) {
    throw std::invalid_argument("Frame feature output already exists.");
  }
  if (!output_path.parent_path().empty()) {
    std::filesystem::create_directories(output_path.parent_path());
  }
  const std::filesystem::path temporary_path = UniqueTemporaryPath(output_path);
  try {
    {
      std::ofstream stream(temporary_path, std::ios::binary | std::ios::trunc);
      if (!stream) {
        throw std::runtime_error(
            "Cannot create frame feature temporary output.");
      }
      stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
      stream.close();
      if (!stream) {
        throw std::runtime_error(
            "Cannot finish frame feature temporary output.");
      }
    }
    ThrowIfCancelled(cancelled);
    if (std::filesystem::exists(output_path)) {
      throw std::invalid_argument(
          "Frame feature output appeared during extraction.");
    }
    std::error_code error;
    std::filesystem::create_hard_link(temporary_path, output_path, error);
    if (error) {
      throw std::runtime_error(
          "Cannot atomically finalize frame feature output without overwrite.");
    }
    std::filesystem::remove(temporary_path, error);
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temporary_path, ignored);
    throw;
  }
}

colmap::internal::FrameFeatureArtifactDataV1 ParseArtifact(
    const ExtractorState& state,
    const std::filesystem::path& path,
    const uint64_t expected_stable_frame_id,
    const uint64_t expected_frame_revision,
    const std::string_view expected_image_sha256,
    const std::string_view expected_metadata_sha256) {
  const uint64_t maximum_artifact_bytes = CheckedAdd(
      state.config.memory_admission_budget_bytes, 16ULL * 1024ULL * 1024ULL);
  const std::vector<uint8_t> bytes =
      ReadFileBytes(path, maximum_artifact_bytes);
  if (bytes.size() < kArtifactFooterSize + kArtifactMagic.size()) {
    throw std::invalid_argument("Truncated frame feature artifact.");
  }
  const size_t payload_size = bytes.size() - kArtifactFooterSize;
  ByteReader reader(bytes, payload_size);
  reader.Expect(kArtifactMagic, "magic");
  const uint32_t schema_version = reader.Read<uint32_t>();
  if (schema_version != COLMAPKIT_FRAME_FEATURE_ARTIFACT_SCHEMA_VERSION_V1) {
    throw std::invalid_argument(
        "Unknown frame feature artifact schema version.");
  }
  if (reader.Read<uint32_t>() != COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1) {
    throw std::invalid_argument("Unknown frame feature artifact ABI version.");
  }
  if (reader.Read<uint32_t>() != kArtifactCompleteFlag) {
    throw std::invalid_argument("Frame feature artifact is not complete.");
  }

  colmap::internal::FrameFeatureArtifactDataV1 parsed;
  InitializeResult(&parsed.result);
  parsed.metadata.struct_size = sizeof(parsed.metadata);
  parsed.metadata.abi_version = COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1;
  parsed.metadata.image_format = COLMAPKIT_FRAME_IMAGE_FORMAT_V1_JPEG;
  parsed.result.stable_frame_id = reader.Read<uint64_t>();
  parsed.result.frame_revision = reader.Read<uint64_t>();
  parsed.result.encoded_width = reader.Read<uint32_t>();
  parsed.result.encoded_height = reader.Read<uint32_t>();
  parsed.result.processed_width = reader.Read<uint32_t>();
  parsed.result.processed_height = reader.Read<uint32_t>();
  const uint32_t orientation = reader.Read<uint32_t>();
  const uint32_t camera_model = reader.Read<uint32_t>();
  const uint32_t num_camera_params = reader.Read<uint32_t>();
  std::array<double, 8> camera_params{};
  for (double& parameter : camera_params) parameter = reader.Read<double>();
  const bool valid_camera_model =
      (camera_model == COLMAPKIT_CAMERA_MODEL_V2_PINHOLE &&
       num_camera_params == 4) ||
      (camera_model == COLMAPKIT_CAMERA_MODEL_V2_SIMPLE_PINHOLE &&
       num_camera_params == 3) ||
      (camera_model == COLMAPKIT_CAMERA_MODEL_V2_OPENCV &&
       num_camera_params == 8);
  if (orientation != COLMAPKIT_FRAME_ORIENTATION_V1_UP || !valid_camera_model) {
    throw std::invalid_argument("Invalid frame metadata in feature artifact.");
  }
  for (uint32_t i = 0; i < num_camera_params; ++i) {
    if (!std::isfinite(camera_params[i])) {
      throw std::invalid_argument(
          "Non-finite camera metadata in feature artifact.");
    }
  }
  for (uint32_t i = num_camera_params; i < camera_params.size(); ++i) {
    if (camera_params[i] != 0.0) {
      throw std::invalid_argument(
          "Non-canonical camera metadata in feature artifact.");
    }
  }
  parsed.metadata.encoded_width = parsed.result.encoded_width;
  parsed.metadata.encoded_height = parsed.result.encoded_height;
  parsed.metadata.orientation = orientation;
  parsed.metadata.camera_model = camera_model;
  parsed.metadata.num_camera_params = num_camera_params;
  std::copy(camera_params.begin(),
            camera_params.end(),
            parsed.metadata.camera_params);
  if (reader.Read<uint32_t>() != COLMAPKIT_FRAME_FEATURE_BACKEND_V1_CPU ||
      reader.Read<uint32_t>() != kSiftExtractorType ||
      reader.Read<uint32_t>() != state.config.normalization ||
      reader.Read<uint32_t>() != state.config.max_image_size ||
      reader.Read<uint32_t>() != state.config.max_num_features ||
      reader.Read<int32_t>() != state.config.first_octave ||
      reader.Read<uint32_t>() != state.config.num_octaves ||
      reader.Read<uint32_t>() != state.config.octave_resolution ||
      reader.Read<uint32_t>() != state.config.max_num_orientations ||
      reader.Read<uint32_t>() != state.config.upright ||
      reader.Read<double>() != state.config.peak_threshold ||
      reader.Read<double>() != state.config.edge_threshold) {
    throw std::invalid_argument(
        "Frame feature artifact configuration mismatch.");
  }
  if (reader.Read<uint32_t>() != kKeypointColumns ||
      reader.Read<uint32_t>() != kDescriptorDimensions ||
      reader.Read<uint32_t>() != kDescriptorScalarUint8) {
    throw std::invalid_argument("Unsupported frame feature payload format.");
  }
  parsed.result.feature_count = reader.Read<uint64_t>();
  parsed.result.descriptor_bytes = reader.Read<uint64_t>();
  if (parsed.result.feature_count == 0 ||
      parsed.result.feature_count >
          CheckedMultiply(state.config.max_num_features,
                          state.config.max_num_orientations) ||
      parsed.result.descriptor_bytes !=
          CheckedMultiply(parsed.result.feature_count, kDescriptorDimensions)) {
    throw std::invalid_argument("Invalid frame feature counts.");
  }
  const std::string image_sha256 = reader.ReadString();
  const std::string metadata_sha256 = reader.ReadString();
  const std::string profile_sha256 = reader.ReadString();
  const std::string source_identity = reader.ReadString();
  if (!IsLowerHexSHA256(image_sha256) || !IsLowerHexSHA256(metadata_sha256) ||
      profile_sha256 != state.profile_sha256 ||
      source_identity != state.source_identity) {
    throw std::invalid_argument("Frame feature artifact provenance mismatch.");
  }
  if (parsed.result.stable_frame_id != expected_stable_frame_id ||
      parsed.result.frame_revision != expected_frame_revision ||
      image_sha256 != expected_image_sha256 ||
      metadata_sha256 != expected_metadata_sha256) {
    throw std::invalid_argument("Frame feature artifact expectation mismatch.");
  }
  if (parsed.result.encoded_width == 0 || parsed.result.encoded_height == 0 ||
      parsed.result.processed_width == 0 ||
      parsed.result.processed_height == 0) {
    throw std::invalid_argument("Invalid frame feature image dimensions.");
  }
  parsed.keypoints.reserve(parsed.result.feature_count);
  for (uint64_t i = 0; i < parsed.result.feature_count; ++i) {
    std::array<float, kKeypointColumns> values{};
    for (float& value : values) value = reader.Read<float>();
    if (!std::all_of(values.begin(), values.end(), [](const float value) {
          return std::isfinite(value);
        })) {
      throw std::invalid_argument(
          "Non-finite keypoint in frame feature artifact.");
    }
    parsed.keypoints.emplace_back(
        values[0], values[1], values[2], values[3], values[4], values[5]);
  }
  const std::vector<uint8_t> descriptor_bytes =
      reader.ReadBytes(static_cast<size_t>(parsed.result.descriptor_bytes));
  parsed.descriptors.type = colmap::FeatureExtractorType::SIFT;
  parsed.descriptors.data.resize(parsed.result.feature_count,
                                 kDescriptorDimensions);
  std::copy(descriptor_bytes.begin(),
            descriptor_bytes.end(),
            parsed.descriptors.data.data());
  if (reader.Offset() != payload_size) {
    throw std::invalid_argument(
        "Unexpected trailing frame feature payload data.");
  }
  const std::string stored_payload_sha256(
      bytes.begin() + payload_size,
      bytes.begin() + payload_size + kHashHexLength);
  if (!IsLowerHexSHA256(stored_payload_sha256) ||
      stored_payload_sha256 !=
          colmap::internal::FrameFeatureSHA256(std::string_view(
              reinterpret_cast<const char*>(bytes.data()), payload_size))) {
    throw std::invalid_argument("Frame feature payload checksum mismatch.");
  }
  if (!std::equal(kCompletionMarker.begin(),
                  kCompletionMarker.end(),
                  bytes.begin() + payload_size + kHashHexLength)) {
    throw std::invalid_argument("Missing frame feature completion marker.");
  }

  parsed.result.status = COLMAPKIT_STATUS_OK;
  CopyText(parsed.result.image_sha256,
           sizeof(parsed.result.image_sha256),
           image_sha256);
  CopyText(parsed.result.metadata_sha256,
           sizeof(parsed.result.metadata_sha256),
           metadata_sha256);
  CopyText(parsed.result.profile_sha256,
           sizeof(parsed.result.profile_sha256),
           profile_sha256);
  CopyText(parsed.result.payload_sha256,
           sizeof(parsed.result.payload_sha256),
           stored_payload_sha256);
  CopyText(parsed.result.artifact_sha256,
           sizeof(parsed.result.artifact_sha256),
           HashBytes(bytes));
  CopyText(parsed.result.source_identity,
           sizeof(parsed.result.source_identity),
           source_identity);
  CopyText(parsed.result.message,
           sizeof(parsed.result.message),
           "Frame feature artifact is valid.");
  return parsed;
}

void CopyResult(const ColmapKitFrameFeatureResultV1& source,
                ColmapKitFrameFeatureResultV1* destination) {
  const uint32_t caller_size = destination->struct_size;
  std::memcpy(
      destination, &source, std::min<size_t>(caller_size, sizeof(source)));
  destination->struct_size = caller_size;
}

bool PrepareResult(ColmapKitFrameFeatureResultV1* result) {
  return result != nullptr &&
         result->struct_size >=
             offsetof(ColmapKitFrameFeatureResultV1, actual_backend) &&
         result->abi_version == COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1;
}

ColmapKitStatus StatusForException(const std::exception& exception) {
  if (dynamic_cast<const CancelledError*>(&exception) != nullptr) {
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

}  // namespace

struct ColmapKitFrameFeatureExtractorV1 {
  std::shared_ptr<ExtractorState> state;
};

struct ColmapKitFrameFeatureJobV1 {
  std::shared_ptr<ExtractorState> state;
  OwnedInput input;
  std::atomic<bool> cancelled{false};
  ColmapKitFrameFeatureResultV1 result{};
  std::thread worker;
};

namespace colmap::internal {

FrameFeatureArtifactDataV1 LoadFrameFeatureArtifactV1(
    const ColmapKitFrameFeatureExtractorV1* extractor,
    const ColmapKitFrameFeatureArtifactExpectationV1& expectation) {
  if (extractor == nullptr || extractor->state == nullptr ||
      expectation.struct_size < sizeof(expectation) ||
      expectation.abi_version != COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1 ||
      expectation.expected_image_sha256 == nullptr ||
      expectation.expected_metadata_sha256 == nullptr ||
      expectation.artifact_path == nullptr) {
    throw std::invalid_argument("Invalid frame feature loading expectation.");
  }
  const std::string image_sha256(expectation.expected_image_sha256);
  const std::string metadata_sha256(expectation.expected_metadata_sha256);
  if (!IsLowerHexSHA256(image_sha256) || !IsLowerHexSHA256(metadata_sha256)) {
    throw std::invalid_argument(
        "Expected artifact hashes must be lowercase hex.");
  }
  return ParseArtifact(*extractor->state,
                       expectation.artifact_path,
                       expectation.stable_frame_id,
                       expectation.frame_revision,
                       image_sha256,
                       metadata_sha256);
}

}  // namespace colmap::internal

namespace {

void FinishJob(ColmapKitFrameFeatureJobV1* job) {
  std::lock_guard<std::mutex> lock(job->state->mutex);
  job->state->active = false;
}

void RunJob(ColmapKitFrameFeatureJobV1* job) {
  InitializeResult(&job->result);
  job->result.stable_frame_id = job->input.stable_frame_id;
  job->result.frame_revision = job->input.frame_revision;
  job->result.admitted_memory_bytes = job->input.admitted_memory_bytes;
  CopyText(job->result.profile_sha256,
           sizeof(job->result.profile_sha256),
           job->state->profile_sha256);
  CopyText(job->result.source_identity,
           sizeof(job->result.source_identity),
           job->state->source_identity);
  const auto start = Clock::now();
  uint64_t peak_resident_memory = ResidentMemoryBytes();
  try {
    EmitProgress(job->input,
                 COLMAPKIT_FRAME_FEATURE_PROGRESS_V1_QUEUED,
                 start,
                 0.0,
                 "Frame feature job accepted.");
    ThrowIfCancelled(job->cancelled);
    EmitProgress(job->input,
                 COLMAPKIT_FRAME_FEATURE_PROGRESS_V1_VALIDATING,
                 start,
                 0.1,
                 "Validating immutable frame identity.");
    ThrowIfCancelled(job->cancelled);
    const std::string image_sha256 =
        colmap::internal::FrameFeatureSHA256(std::string_view(
            reinterpret_cast<const char*>(job->input.encoded.data()),
            job->input.encoded.size()));
    if (image_sha256 != job->input.expected_image_sha256) {
      throw std::invalid_argument("Encoded frame SHA-256 does not match.");
    }
    const std::string metadata_sha256 =
        HashBytes(CanonicalMetadataBytes(job->input.metadata));
    CopyText(job->result.image_sha256,
             sizeof(job->result.image_sha256),
             image_sha256);
    CopyText(job->result.metadata_sha256,
             sizeof(job->result.metadata_sha256),
             metadata_sha256);

    EmitProgress(job->input,
                 COLMAPKIT_FRAME_FEATURE_PROGRESS_V1_DECODING,
                 start,
                 0.2,
                 "Decoding canonical-upright JPEG frame.");
    ThrowIfCancelled(job->cancelled);
    const auto decode_start = Clock::now();
    int encoded_width = 0;
    int encoded_height = 0;
    colmap::Bitmap rgb = DecodeJpeg(job->input.encoded,
                                    job->state->config.max_image_size,
                                    job->input.metadata.encoded_width,
                                    job->input.metadata.encoded_height,
                                    &encoded_width,
                                    &encoded_height);
    job->result.decode_seconds = Seconds(Clock::now() - decode_start).count();
    job->result.encoded_width = job->input.metadata.encoded_width;
    job->result.encoded_height = job->input.metadata.encoded_height;
    peak_resident_memory =
        std::max(peak_resident_memory, ResidentMemoryBytes());

    EmitProgress(job->input,
                 COLMAPKIT_FRAME_FEATURE_PROGRESS_V1_PREPROCESSING,
                 start,
                 0.35,
                 "Preparing deterministic grayscale feature bitmap.");
    ThrowIfCancelled(job->cancelled);
    const auto preprocess_start = Clock::now();
    colmap::Bitmap bitmap = rgb.CloneAsGrey();
    bitmap.Thumbnail(static_cast<int>(job->state->config.max_image_size));
    job->result.processed_width = static_cast<uint32_t>(bitmap.Width());
    job->result.processed_height = static_cast<uint32_t>(bitmap.Height());
    job->result.preprocess_seconds =
        Seconds(Clock::now() - preprocess_start).count();
    rgb = colmap::Bitmap();
    peak_resident_memory =
        std::max(peak_resident_memory, ResidentMemoryBytes());

    EmitProgress(job->input,
                 COLMAPKIT_FRAME_FEATURE_PROGRESS_V1_EXTRACTING,
                 start,
                 0.5,
                 "Extracting one-worker CPU SIFT features.");
    ThrowIfCancelled(job->cancelled);
    const auto extraction_start = Clock::now();
    auto extractor =
        colmap::FeatureExtractor::Create(MakeOptions(job->state->config));
    if (extractor == nullptr) {
      throw std::runtime_error("Cannot create the CPU SIFT extractor.");
    }
    colmap::FeatureKeypoints keypoints;
    colmap::FeatureDescriptors descriptors;
    if (!extractor->Extract(bitmap, &keypoints, &descriptors)) {
      throw std::runtime_error("CPU SIFT feature extraction failed.");
    }
    job->result.extraction_seconds =
        Seconds(Clock::now() - extraction_start).count();
    ThrowIfCancelled(job->cancelled);
    if (keypoints.empty()) {
      throw std::runtime_error("CPU SIFT produced zero features.");
    }
    if (descriptors.type != colmap::FeatureExtractorType::SIFT ||
        descriptors.data.rows() !=
            static_cast<Eigen::Index>(keypoints.size()) ||
        descriptors.data.cols() !=
            static_cast<Eigen::Index>(kDescriptorDimensions)) {
      throw std::runtime_error(
          "CPU SIFT returned an invalid descriptor payload.");
    }
    ScaleKeypoints(bitmap.Width(),
                   bitmap.Height(),
                   job->input.metadata.encoded_width,
                   job->input.metadata.encoded_height,
                   &keypoints);
    for (const auto& keypoint : keypoints) {
      if (!std::isfinite(keypoint.x) || !std::isfinite(keypoint.y) ||
          !std::isfinite(keypoint.a11) || !std::isfinite(keypoint.a12) ||
          !std::isfinite(keypoint.a21) || !std::isfinite(keypoint.a22)) {
        throw std::runtime_error("CPU SIFT produced a non-finite keypoint.");
      }
    }
    job->result.feature_count = keypoints.size();
    job->result.descriptor_bytes = descriptors.data.size();
    peak_resident_memory =
        std::max(peak_resident_memory, ResidentMemoryBytes());

    EmitProgress(job->input,
                 COLMAPKIT_FRAME_FEATURE_PROGRESS_V1_SERIALIZING,
                 start,
                 0.85,
                 "Serializing versioned frame feature artifact.");
    ThrowIfCancelled(job->cancelled);
    const auto serialization_start = Clock::now();
    std::string payload_sha256;
    const std::vector<uint8_t> artifact = SerializeArtifact(*job->state,
                                                            job->input,
                                                            bitmap.Width(),
                                                            bitmap.Height(),
                                                            image_sha256,
                                                            metadata_sha256,
                                                            keypoints,
                                                            descriptors,
                                                            &payload_sha256);
    ThrowIfCancelled(job->cancelled);
    WriteArtifactAtomically(job->input.output_path, artifact, job->cancelled);
    job->result.serialization_seconds =
        Seconds(Clock::now() - serialization_start).count();
    CopyText(job->result.payload_sha256,
             sizeof(job->result.payload_sha256),
             payload_sha256);
    CopyText(job->result.artifact_sha256,
             sizeof(job->result.artifact_sha256),
             HashBytes(artifact));
    job->result.status = COLMAPKIT_STATUS_OK;
    CopyText(job->result.message,
             sizeof(job->result.message),
             "CPU frame feature artifact completed.");
    EmitProgress(job->input,
                 COLMAPKIT_FRAME_FEATURE_PROGRESS_V1_FINISHED,
                 start,
                 1.0,
                 "CPU frame feature artifact completed.");
  } catch (const std::exception& exception) {
    job->result.status = StatusForException(exception);
    CopyText(
        job->result.message, sizeof(job->result.message), exception.what());
    EmitProgress(job->input,
                 job->result.status == COLMAPKIT_STATUS_CANCELLED
                     ? COLMAPKIT_FRAME_FEATURE_PROGRESS_V1_CANCELLED
                     : COLMAPKIT_FRAME_FEATURE_PROGRESS_V1_FAILED,
                 start,
                 1.0,
                 exception.what());
  }
  job->result.total_seconds = Seconds(Clock::now() - start).count();
  job->result.peak_resident_memory_bytes =
      std::max(peak_resident_memory, ResidentMemoryBytes());
  FinishJob(job);
}

}  // namespace

extern "C" {

ColmapKitStatus ColmapKitCreateFrameFeatureExtractorV1(
    const ColmapKitFrameFeatureExtractorConfigV1* config,
    ColmapKitFrameFeatureExtractorV1** extractor,
    ColmapKitFrameFeatureErrorV1* error) {
  if (extractor != nullptr) *extractor = nullptr;
  if (config == nullptr || extractor == nullptr) {
    FillError(error,
              COLMAPKIT_STATUS_INVALID_ARGUMENT,
              "Frame feature config and output context are required.");
    return COLMAPKIT_STATUS_INVALID_ARGUMENT;
  }
  try {
    ColmapKitFrameFeatureExtractorConfigV1 bounded{};
    std::memcpy(&bounded,
                config,
                std::min<size_t>(config->struct_size, sizeof(bounded)));
    ValidateConfig(bounded);
    auto owned = std::make_unique<ColmapKitFrameFeatureExtractorV1>();
    owned->state = std::make_shared<ExtractorState>(bounded);
    *extractor = owned.release();
    FillError(
        error, COLMAPKIT_STATUS_OK, "CPU frame feature extractor created.");
    return COLMAPKIT_STATUS_OK;
  } catch (const std::exception& exception) {
    const ColmapKitStatus status = StatusForException(exception);
    FillError(error, status, exception.what());
    return status;
  }
}

ColmapKitStatus ColmapKitStartFrameFeatureExtractionV1(
    ColmapKitFrameFeatureExtractorV1* extractor,
    const ColmapKitFrameFeatureInputV1* input,
    ColmapKitFrameFeatureJobV1** job,
    ColmapKitFrameFeatureErrorV1* error) {
  if (job != nullptr) *job = nullptr;
  if (extractor == nullptr || extractor->state == nullptr || input == nullptr ||
      job == nullptr) {
    FillError(error,
              COLMAPKIT_STATUS_INVALID_ARGUMENT,
              "Frame feature extractor, input, and job output are required.");
    return COLMAPKIT_STATUS_INVALID_ARGUMENT;
  }
  try {
    if (input->struct_size < sizeof(*input) ||
        input->abi_version != COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1 ||
        input->encoded_image_bytes == nullptr ||
        input->encoded_image_size == 0 ||
        input->encoded_image_size >
            extractor->state->config.max_encoded_image_bytes ||
        input->expected_image_sha256 == nullptr ||
        input->output_artifact_path == nullptr) {
      throw std::invalid_argument(
          "Invalid frame feature input ABI or payload.");
    }
    ValidateMetadata(input->metadata);
    const std::string expected_sha256(input->expected_image_sha256);
    if (!IsLowerHexSHA256(expected_sha256)) {
      throw std::invalid_argument(
          "Expected frame SHA-256 must be lowercase hex.");
    }
    const std::filesystem::path output_path(input->output_artifact_path);
    if (output_path.empty() || output_path.extension() != ".ckfeatures") {
      throw std::invalid_argument(
          "Frame feature output must use the .ckfeatures extension.");
    }
    if (std::filesystem::exists(output_path)) {
      throw std::invalid_argument("Frame feature output already exists.");
    }
    const uint64_t admitted_memory = EstimateAdmittedMemory(
        extractor->state->config, input->metadata, input->encoded_image_size);
    if (admitted_memory >
        extractor->state->config.memory_admission_budget_bytes) {
      throw std::invalid_argument(
          "Frame feature job exceeds the configured memory admission budget.");
    }

    {
      std::lock_guard<std::mutex> lock(extractor->state->mutex);
      if (extractor->state->active) {
        throw std::runtime_error(
            "The one-worker frame feature extractor is busy.");
      }
      extractor->state->active = true;
    }

    auto owned = std::make_unique<ColmapKitFrameFeatureJobV1>();
    try {
      owned->state = extractor->state;
      owned->input.stable_frame_id = input->stable_frame_id;
      owned->input.frame_revision = input->frame_revision;
      owned->input.encoded.assign(
          input->encoded_image_bytes,
          input->encoded_image_bytes + input->encoded_image_size);
      owned->input.expected_image_sha256 = expected_sha256;
      owned->input.metadata = input->metadata;
      owned->input.output_path = output_path;
      owned->input.progress_callback = input->progress_callback;
      owned->input.progress_user_data = input->progress_user_data;
      owned->input.admitted_memory_bytes = admitted_memory;
      auto* raw = owned.get();
      owned->worker = std::thread([raw]() { RunJob(raw); });
      *job = owned.release();
    } catch (...) {
      std::lock_guard<std::mutex> lock(extractor->state->mutex);
      extractor->state->active = false;
      throw;
    }
    FillError(error, COLMAPKIT_STATUS_OK, "Frame feature job started.");
    return COLMAPKIT_STATUS_OK;
  } catch (const std::exception& exception) {
    const ColmapKitStatus status = StatusForException(exception);
    FillError(error, status, exception.what());
    return status;
  }
}

ColmapKitStatus ColmapKitCancelFrameFeatureExtractionV1(
    ColmapKitFrameFeatureJobV1* job, ColmapKitFrameFeatureErrorV1* error) {
  if (job == nullptr) {
    FillError(error,
              COLMAPKIT_STATUS_INVALID_ARGUMENT,
              "Frame feature job is required.");
    return COLMAPKIT_STATUS_INVALID_ARGUMENT;
  }
  job->cancelled.store(true, std::memory_order_release);
  FillError(
      error, COLMAPKIT_STATUS_OK, "Frame feature cancellation requested.");
  return COLMAPKIT_STATUS_OK;
}

ColmapKitStatus ColmapKitWaitFrameFeatureExtractionV1(
    ColmapKitFrameFeatureJobV1* job, ColmapKitFrameFeatureResultV1* result) {
  if (job == nullptr || !PrepareResult(result)) {
    return COLMAPKIT_STATUS_INVALID_ARGUMENT;
  }
  if (job->worker.joinable()) job->worker.join();
  CopyResult(job->result, result);
  return static_cast<ColmapKitStatus>(job->result.status);
}

void ColmapKitReleaseFrameFeatureJobV1(ColmapKitFrameFeatureJobV1* job) {
  if (job == nullptr) return;
  if (job->worker.joinable()) {
    job->cancelled.store(true, std::memory_order_release);
    job->worker.join();
  }
  delete job;
}

void ColmapKitReleaseFrameFeatureExtractorV1(
    ColmapKitFrameFeatureExtractorV1* extractor) {
  delete extractor;
}

ColmapKitStatus ColmapKitValidateFrameFeatureArtifactV1(
    const ColmapKitFrameFeatureExtractorV1* extractor,
    const ColmapKitFrameFeatureArtifactExpectationV1* expectation,
    ColmapKitFrameFeatureResultV1* result) {
  if (extractor == nullptr || extractor->state == nullptr ||
      expectation == nullptr || !PrepareResult(result)) {
    return COLMAPKIT_STATUS_INVALID_ARGUMENT;
  }
  ColmapKitFrameFeatureResultV1 local{};
  InitializeResult(&local);
  try {
    if (expectation->struct_size < sizeof(*expectation) ||
        expectation->abi_version != COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1 ||
        expectation->expected_image_sha256 == nullptr ||
        expectation->expected_metadata_sha256 == nullptr ||
        expectation->artifact_path == nullptr) {
      throw std::invalid_argument(
          "Invalid frame feature validation expectation.");
    }
    const std::string image_sha256(expectation->expected_image_sha256);
    const std::string metadata_sha256(expectation->expected_metadata_sha256);
    if (!IsLowerHexSHA256(image_sha256) || !IsLowerHexSHA256(metadata_sha256)) {
      throw std::invalid_argument(
          "Expected artifact hashes must be lowercase hex.");
    }
    const auto parsed = ParseArtifact(*extractor->state,
                                      expectation->artifact_path,
                                      expectation->stable_frame_id,
                                      expectation->frame_revision,
                                      image_sha256,
                                      metadata_sha256);
    local = parsed.result;
  } catch (const std::exception& exception) {
    local.status = StatusForException(exception);
    CopyText(local.message, sizeof(local.message), exception.what());
  }
  CopyResult(local, result);
  return static_cast<ColmapKitStatus>(local.status);
}

}  // extern "C"
