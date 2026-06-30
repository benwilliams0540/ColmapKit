// SiftMetal.h - Metal-accelerated SIFT feature extraction for macOS.
// Ported from SIFTMetal (Swift) by Luke Van In, adapted for COLMAP integration.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sift_metal {

struct Keypoint {
  float x;            // Absolute x coordinate in original image
  float y;            // Absolute y coordinate in original image
  float sigma;        // Scale (blur level)
  float orientation;  // Dominant orientation in radians
};

enum class StatusSeverity {
  kInfo,
  kWarning,
  kError,
};

struct StatusMessage {
  StatusSeverity severity = StatusSeverity::kInfo;
  std::string stage;
  std::string message;
  std::string detail;
};

struct MetallibPathAttempt {
  std::string path;
  bool exists = false;
  bool loaded = false;
  std::string error;
};

struct CapacityStatus {
  int64_t detected_extrema = 0;
  int64_t dropped_extrema = 0;
  int64_t dropped_keypoints = 0;
  int64_t dropped_orientations = 0;
  int64_t dropped_descriptors = 0;
  int64_t dropped_features = 0;
};

struct StatusReport {
  bool ok = true;
  std::string stage;
  std::string message;
  std::string device_name;
  int requested_max_image_width = 0;
  int requested_max_image_height = 0;
  int image_width = 0;
  int image_height = 0;
  int seed_width = 0;
  int seed_height = 0;
  CapacityStatus capacity;
  std::vector<MetallibPathAttempt> metallib_path_attempts;
  std::vector<StatusMessage> messages;
};

struct ExtractResult {
  std::vector<Keypoint> keypoints;
  // 128-dimensional descriptors, one row per keypoint.
  // Values are float (pre-normalization), suitable for COLMAP's
  // L1_ROOT or L2 normalization pipeline.
  std::vector<float> descriptors;  // size = keypoints.size() * 128
  StatusReport status;
};

struct Options {
  // Number of octaves. -1 = auto (based on image size).
  int num_octaves = -1;
  // Number of scales per octave.
  int scales_per_octave = 3;
  // First octave index. -1 means 2x upscaling of input.
  int first_octave = -1;
  // Peak threshold for DoG detection.
  float peak_threshold = 0.0133f;
  // Edge threshold (ratio of principal curvatures).
  float edge_threshold = 10.0f;
  // Maximum number of features to retain (0 = unlimited).
  int max_num_features = 8192;
  // Maximum number of orientations per keypoint.
  int max_num_orientations = 2;
  // Fix orientation to 0 for upright features.
  bool upright = false;
};

// Opaque implementation handle.
class SiftMetalExtractorImpl;

class SiftMetalExtractor {
 public:
  SiftMetalExtractor();
  ~SiftMetalExtractor();

  // Initialize the Metal pipeline. Returns false if Metal is unavailable.
  bool Init(const Options& options, int max_image_width, int max_image_height);

  // Extract SIFT features from a grayscale image.
  // data: row-major uint8 grayscale pixels
  // width, height: image dimensions
  bool Extract(const uint8_t* data,
               int width,
               int height,
               ExtractResult* result);

  // Status for the most recent Init or Extract call.
  const StatusReport& LastStatus() const;

 private:
  std::unique_ptr<SiftMetalExtractorImpl> impl_;
};

}  // namespace sift_metal
