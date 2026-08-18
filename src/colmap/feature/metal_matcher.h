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

#pragma once

#include "colmap/feature/types.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace colmap {

inline constexpr int kMetalSiftDescriptorDim = 128;
inline constexpr uint32_t kMetalSiftDescriptorNorm = 512;
inline constexpr uint32_t kMetalSiftDescriptorNormSquared =
    kMetalSiftDescriptorNorm * kMetalSiftDescriptorNorm;

inline uint32_t SiftNormalizedDistanceToSquaredL2(
    const double normalized_distance) {
  if (!std::isfinite(normalized_distance) || normalized_distance <= 0.0) {
    return std::numeric_limits<uint32_t>::max();
  }
  const double distance_squared =
      normalized_distance * normalized_distance *
      static_cast<double>(kMetalSiftDescriptorNormSquared);
  if (distance_squared >=
      static_cast<double>(std::numeric_limits<uint32_t>::max())) {
    return std::numeric_limits<uint32_t>::max();
  }
  return static_cast<uint32_t>(distance_squared);
}

struct MetalSiftTop2Match {
  point2D_t best_train_idx = kInvalidPoint2DIdx;
  point2D_t second_best_train_idx = kInvalidPoint2DIdx;
  uint32_t best_distance_squared = std::numeric_limits<uint32_t>::max();
  uint32_t second_best_distance_squared = std::numeric_limits<uint32_t>::max();
};

struct MetalSiftMatchingOptions {
  // Lowe ratio test applied in squared L2 space:
  // best_distance_squared < max_ratio^2 * second_best_distance_squared.
  double max_ratio = 0.8;

  // Squared L2 threshold in the raw uint8 SIFT descriptor space. The default
  // mirrors COLMAP's normalized SIFT max_distance = 0.7.
  uint32_t max_distance_squared = SiftNormalizedDistanceToSquaredL2(0.7);

  // Require that the reverse best match accepts the original query descriptor.
  bool cross_check = true;

  // Prefer the Metal backend when it is compiled and available; otherwise use
  // the deterministic CPU fallback.
  bool use_metal = true;

  // Fail the operation instead of falling back when Metal is unavailable or a
  // command buffer fails. Legacy callers leave this false.
  bool require_metal = false;

  std::shared_ptr<struct MetalRuntimeTelemetry> runtime_telemetry;

  bool Check() const;
};

struct MetalRuntimeTelemetrySnapshot {
  uint64_t sift_extraction_operations = 0;
  uint64_t sift_extraction_failures = 0;
  uint64_t sift_matching_operations = 0;
  uint64_t sift_matching_fallbacks = 0;
  std::string device_name;
  std::string last_failure;
};

// Run-scoped telemetry shared by facade options and controller worker copies.
// It is intentionally opt-in so ordinary COLMAP and legacy ColmapKit callers
// do not pay for or observe it.
struct MetalRuntimeTelemetry {
  void RecordDeviceName(const std::string& value);
  void RecordSiftExtraction(bool succeeded,
                            const std::string& device,
                            const std::string& failure = {});
  void RecordSiftMatching(bool used_metal,
                          const std::string& device,
                          const std::string& failure = {});
  MetalRuntimeTelemetrySnapshot Snapshot() const;

 private:
  std::atomic<uint64_t> sift_extraction_operations_{0};
  std::atomic<uint64_t> sift_extraction_failures_{0};
  std::atomic<uint64_t> sift_matching_operations_{0};
  std::atomic<uint64_t> sift_matching_fallbacks_{0};
  mutable std::mutex strings_mutex_;
  std::string device_name_;
  std::string last_failure_;
};

bool IsMetalSiftMatcherAvailable();

std::string GetMetalSiftMatcherDeviceName();

std::vector<MetalSiftTop2Match> ComputeSiftTop2MatchesCPU(
    const FeatureDescriptors& query_descriptors,
    const FeatureDescriptors& train_descriptors);

class MetalSiftDescriptorMatcher {
 public:
  explicit MetalSiftDescriptorMatcher(
      const MetalSiftMatchingOptions& options = MetalSiftMatchingOptions());

  const MetalSiftMatchingOptions& Options() const;

  bool IsMetalAvailable() const;

  std::vector<MetalSiftTop2Match> ComputeTop2(
      const FeatureDescriptors& query_descriptors,
      const FeatureDescriptors& train_descriptors) const;

  void Match(const FeatureDescriptors& query_descriptors,
             const FeatureDescriptors& train_descriptors,
             FeatureMatches* matches) const;

 private:
  MetalSiftMatchingOptions options_;
};

void MatchMetalSiftDescriptors(const FeatureDescriptors& query_descriptors,
                               const FeatureDescriptors& train_descriptors,
                               const MetalSiftMatchingOptions& options,
                               FeatureMatches* matches);

}  // namespace colmap
