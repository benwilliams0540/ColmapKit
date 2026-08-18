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

#include "colmap/feature/metal_matcher.h"

#include "colmap/util/logging.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace colmap {

#if defined(__APPLE__) && defined(COLMAP_METAL_ENABLED)
bool ComputeMetalSiftTop2Matches(const FeatureDescriptors& query_descriptors,
                                 const FeatureDescriptors& train_descriptors,
                                 std::vector<MetalSiftTop2Match>* top2_matches);
#else
namespace {
bool ComputeMetalSiftTop2Matches(
    const FeatureDescriptors& query_descriptors,
    const FeatureDescriptors& train_descriptors,
    std::vector<MetalSiftTop2Match>* top2_matches) {
  (void)query_descriptors;
  (void)train_descriptors;
  (void)top2_matches;
  return false;
}
}  // namespace

bool IsMetalSiftMatcherAvailable() { return false; }

std::string GetMetalSiftMatcherDeviceName() { return {}; }
#endif

void MetalRuntimeTelemetry::RecordDeviceName(const std::string& value) {
  if (value.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(strings_mutex_);
  device_name_ = value;
}

void MetalRuntimeTelemetry::RecordSiftExtraction(const bool succeeded,
                                                 const std::string& device,
                                                 const std::string& failure) {
  RecordDeviceName(device);
  if (succeeded) {
    sift_extraction_operations_.fetch_add(1, std::memory_order_relaxed);
  } else {
    sift_extraction_failures_.fetch_add(1, std::memory_order_relaxed);
  }
  if (!failure.empty()) {
    std::lock_guard<std::mutex> lock(strings_mutex_);
    last_failure_ = failure;
  }
}

void MetalRuntimeTelemetry::RecordSiftMatching(const bool used_metal,
                                               const std::string& device,
                                               const std::string& failure) {
  RecordDeviceName(device);
  if (used_metal) {
    sift_matching_operations_.fetch_add(1, std::memory_order_relaxed);
  } else {
    sift_matching_fallbacks_.fetch_add(1, std::memory_order_relaxed);
  }
  if (!failure.empty()) {
    std::lock_guard<std::mutex> lock(strings_mutex_);
    last_failure_ = failure;
  }
}

MetalRuntimeTelemetrySnapshot MetalRuntimeTelemetry::Snapshot() const {
  MetalRuntimeTelemetrySnapshot snapshot;
  snapshot.sift_extraction_operations =
      sift_extraction_operations_.load(std::memory_order_relaxed);
  snapshot.sift_extraction_failures =
      sift_extraction_failures_.load(std::memory_order_relaxed);
  snapshot.sift_matching_operations =
      sift_matching_operations_.load(std::memory_order_relaxed);
  snapshot.sift_matching_fallbacks =
      sift_matching_fallbacks_.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(strings_mutex_);
    snapshot.device_name = device_name_;
    snapshot.last_failure = last_failure_;
  }
  return snapshot;
}

namespace {

void CheckSiftDescriptors(const FeatureDescriptors& descriptors) {
  if (descriptors.type != FeatureExtractorType::UNDEFINED) {
    THROW_CHECK_EQ(descriptors.type, FeatureExtractorType::SIFT);
  }
  THROW_CHECK_LT(static_cast<uint64_t>(descriptors.data.rows()),
                 static_cast<uint64_t>(kInvalidPoint2DIdx));
  if (descriptors.data.rows() > 0 || descriptors.data.cols() > 0) {
    THROW_CHECK_EQ(descriptors.data.cols(), kMetalSiftDescriptorDim);
  }
}

void ConsiderDistance(const uint32_t distance_squared,
                      const point2D_t train_idx,
                      MetalSiftTop2Match* top2) {
  if (distance_squared < top2->best_distance_squared ||
      (distance_squared == top2->best_distance_squared &&
       train_idx < top2->best_train_idx)) {
    if (train_idx != top2->best_train_idx) {
      top2->second_best_train_idx = top2->best_train_idx;
      top2->second_best_distance_squared = top2->best_distance_squared;
    }
    top2->best_train_idx = train_idx;
    top2->best_distance_squared = distance_squared;
    return;
  }

  if (train_idx == top2->best_train_idx) {
    return;
  }

  if (distance_squared < top2->second_best_distance_squared ||
      (distance_squared == top2->second_best_distance_squared &&
       train_idx < top2->second_best_train_idx)) {
    top2->second_best_train_idx = train_idx;
    top2->second_best_distance_squared = distance_squared;
  }
}

bool PassesMatchFilters(const MetalSiftTop2Match& top2,
                        const MetalSiftMatchingOptions& options) {
  if (top2.best_train_idx == kInvalidPoint2DIdx) {
    return false;
  }

  if (top2.best_distance_squared > options.max_distance_squared) {
    return false;
  }

  if (top2.second_best_train_idx == kInvalidPoint2DIdx) {
    return true;
  }

  const double ratio_squared = options.max_ratio * options.max_ratio;
  return static_cast<double>(top2.best_distance_squared) <
         ratio_squared * static_cast<double>(top2.second_best_distance_squared);
}

std::vector<point2D_t> ExtractAcceptedBestMatches(
    const std::vector<MetalSiftTop2Match>& top2_matches,
    const MetalSiftMatchingOptions& options) {
  std::vector<point2D_t> accepted(top2_matches.size(), kInvalidPoint2DIdx);
  for (size_t idx = 0; idx < top2_matches.size(); ++idx) {
    if (PassesMatchFilters(top2_matches[idx], options)) {
      accepted[idx] = top2_matches[idx].best_train_idx;
    }
  }
  return accepted;
}

void WarnMetalFallbackOnce() {
  static std::once_flag once;
  std::call_once(once, []() {
    LOG(WARNING) << "Requested Metal SIFT descriptor matching, but the Metal "
                    "backend is unavailable or failed at runtime; falling "
                    "back to deterministic CPU matching.";
  });
}

}  // namespace

bool MetalSiftMatchingOptions::Check() const {
  CHECK_OPTION(std::isfinite(max_ratio));
  CHECK_OPTION_GT(max_ratio, 0.0);
  return true;
}

std::vector<MetalSiftTop2Match> ComputeSiftTop2MatchesCPU(
    const FeatureDescriptors& query_descriptors,
    const FeatureDescriptors& train_descriptors) {
  CheckSiftDescriptors(query_descriptors);
  CheckSiftDescriptors(train_descriptors);

  std::vector<MetalSiftTop2Match> top2_matches(
      static_cast<size_t>(query_descriptors.data.rows()));
  if (query_descriptors.data.rows() == 0 ||
      train_descriptors.data.rows() == 0) {
    return top2_matches;
  }

  for (Eigen::Index query_idx = 0; query_idx < query_descriptors.data.rows();
       ++query_idx) {
    const uint8_t* query =
        query_descriptors.data.data() + query_idx * kMetalSiftDescriptorDim;
    MetalSiftTop2Match& top2 = top2_matches[query_idx];

    for (Eigen::Index train_idx = 0; train_idx < train_descriptors.data.rows();
         ++train_idx) {
      const uint8_t* train =
          train_descriptors.data.data() + train_idx * kMetalSiftDescriptorDim;
      uint32_t distance_squared = 0;
      for (int dim = 0; dim < kMetalSiftDescriptorDim; ++dim) {
        const int diff =
            static_cast<int>(query[dim]) - static_cast<int>(train[dim]);
        distance_squared += static_cast<uint32_t>(diff * diff);
      }

      ConsiderDistance(
          distance_squared, static_cast<point2D_t>(train_idx), &top2);
    }
  }

  return top2_matches;
}

MetalSiftDescriptorMatcher::MetalSiftDescriptorMatcher(
    const MetalSiftMatchingOptions& options)
    : options_(options) {
  THROW_CHECK(options_.Check());
}

const MetalSiftMatchingOptions& MetalSiftDescriptorMatcher::Options() const {
  return options_;
}

bool MetalSiftDescriptorMatcher::IsMetalAvailable() const {
  return IsMetalSiftMatcherAvailable();
}

std::vector<MetalSiftTop2Match> MetalSiftDescriptorMatcher::ComputeTop2(
    const FeatureDescriptors& query_descriptors,
    const FeatureDescriptors& train_descriptors) const {
  CheckSiftDescriptors(query_descriptors);
  CheckSiftDescriptors(train_descriptors);

  if (options_.use_metal) {
    std::vector<MetalSiftTop2Match> top2_matches;
    if (ComputeMetalSiftTop2Matches(
            query_descriptors, train_descriptors, &top2_matches)) {
      if (options_.runtime_telemetry != nullptr) {
        options_.runtime_telemetry->RecordSiftMatching(
            true, GetMetalSiftMatcherDeviceName());
      }
      return top2_matches;
    }
    const std::string failure =
        "Metal SIFT descriptor matching was unavailable or failed at runtime.";
    if (options_.runtime_telemetry != nullptr) {
      options_.runtime_telemetry->RecordSiftMatching(
          false, GetMetalSiftMatcherDeviceName(), failure);
    }
    if (options_.require_metal) {
      throw std::runtime_error(failure);
    }
    WarnMetalFallbackOnce();
  }

  return ComputeSiftTop2MatchesCPU(query_descriptors, train_descriptors);
}

void MetalSiftDescriptorMatcher::Match(
    const FeatureDescriptors& query_descriptors,
    const FeatureDescriptors& train_descriptors,
    FeatureMatches* matches) const {
  THROW_CHECK_NOTNULL(matches);
  CheckSiftDescriptors(query_descriptors);
  CheckSiftDescriptors(train_descriptors);

  matches->clear();
  if (query_descriptors.data.rows() == 0 ||
      train_descriptors.data.rows() == 0) {
    return;
  }

  const std::vector<point2D_t> matches_1to2 = ExtractAcceptedBestMatches(
      ComputeTop2(query_descriptors, train_descriptors), options_);

  if (!options_.cross_check) {
    matches->reserve(std::count_if(matches_1to2.begin(),
                                   matches_1to2.end(),
                                   [](const point2D_t match_idx) {
                                     return match_idx != kInvalidPoint2DIdx;
                                   }));
    for (size_t query_idx = 0; query_idx < matches_1to2.size(); ++query_idx) {
      if (matches_1to2[query_idx] != kInvalidPoint2DIdx) {
        matches->emplace_back(static_cast<point2D_t>(query_idx),
                              matches_1to2[query_idx]);
      }
    }
    return;
  }

  const std::vector<point2D_t> matches_2to1 = ExtractAcceptedBestMatches(
      ComputeTop2(train_descriptors, query_descriptors), options_);

  matches->reserve(std::count_if(
      matches_1to2.begin(), matches_1to2.end(), [&](const point2D_t train_idx) {
        return train_idx != kInvalidPoint2DIdx &&
               matches_2to1[train_idx] != kInvalidPoint2DIdx;
      }));
  for (size_t query_idx = 0; query_idx < matches_1to2.size(); ++query_idx) {
    const point2D_t train_idx = matches_1to2[query_idx];
    if (train_idx != kInvalidPoint2DIdx &&
        matches_2to1[train_idx] == static_cast<point2D_t>(query_idx)) {
      matches->emplace_back(static_cast<point2D_t>(query_idx), train_idx);
    }
  }
}

void MatchMetalSiftDescriptors(const FeatureDescriptors& query_descriptors,
                               const FeatureDescriptors& train_descriptors,
                               const MetalSiftMatchingOptions& options,
                               FeatureMatches* matches) {
  MetalSiftDescriptorMatcher(options).Match(
      query_descriptors, train_descriptors, matches);
}

}  // namespace colmap
