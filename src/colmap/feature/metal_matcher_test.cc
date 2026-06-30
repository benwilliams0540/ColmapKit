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
#include "colmap/feature/sift.h"

#include <array>
#include <initializer_list>
#include <limits>
#include <memory>

#include <gtest/gtest.h>

namespace colmap {
namespace {

FeatureDescriptors MakeSiftDescriptors(
    const std::initializer_list<uint8_t> first_dim_values) {
  FeatureDescriptors descriptors;
  descriptors.type = FeatureExtractorType::SIFT;
  descriptors.data =
      FeatureDescriptorsData(static_cast<Eigen::Index>(first_dim_values.size()),
                             kMetalSiftDescriptorDim);
  descriptors.data.setZero();

  Eigen::Index row = 0;
  for (const uint8_t value : first_dim_values) {
    descriptors.data(row, 0) = value;
    ++row;
  }

  return descriptors;
}

FeatureDescriptors MakeSparseSiftDescriptors(
    const std::initializer_list<std::array<int, 4>> active_bins_per_row) {
  FeatureDescriptors descriptors;
  descriptors.type = FeatureExtractorType::SIFT;
  descriptors.data = FeatureDescriptorsData(
      static_cast<Eigen::Index>(active_bins_per_row.size()),
      kMetalSiftDescriptorDim);
  descriptors.data.setZero();

  Eigen::Index row = 0;
  for (const std::array<int, 4>& active_bins : active_bins_per_row) {
    for (const int bin : active_bins) {
      descriptors.data(row, bin) = 255;
    }
    ++row;
  }

  return descriptors;
}

MetalSiftMatchingOptions TestOptions() {
  MetalSiftMatchingOptions options;
  options.use_metal = false;
  options.cross_check = false;
  options.max_distance_squared = std::numeric_limits<uint32_t>::max();
  return options;
}

FeatureMatches MatchWithSiftBruteForce(const FeatureDescriptors& query,
                                       const FeatureDescriptors& train,
                                       const bool cross_check) {
  FeatureMatchingOptions options(FeatureMatcherType::SIFT_BRUTEFORCE);
  options.use_gpu = false;
  options.sift->cpu_brute_force_matcher = true;
  options.sift->cross_check = cross_check;
  const std::unique_ptr<FeatureMatcher> matcher =
      CreateSiftFeatureMatcher(options);

  const FeatureMatcher::Image image1 = {
      /*image_id=*/1,
      /*camera=*/nullptr,
      /*keypoints=*/nullptr,
      std::make_shared<FeatureDescriptors>(query)};
  const FeatureMatcher::Image image2 = {
      /*image_id=*/2,
      /*camera=*/nullptr,
      /*keypoints=*/nullptr,
      std::make_shared<FeatureDescriptors>(train)};

  FeatureMatches matches;
  matcher->Match(image1, image2, &matches);
  return matches;
}

FeatureMatches MatchWithMetalMatcher(const FeatureDescriptors& query,
                                     const FeatureDescriptors& train,
                                     const bool cross_check) {
  MetalSiftMatchingOptions options;
  options.use_metal = false;
  options.cross_check = cross_check;

  FeatureMatches matches;
  MetalSiftDescriptorMatcher(options).Match(query, train, &matches);
  return matches;
}

void ExpectEqualMatches(const FeatureMatches& expected,
                        const FeatureMatches& actual) {
  ASSERT_EQ(actual.size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(actual[i], expected[i]);
  }
}

TEST(MetalSiftDescriptorMatcher, ComputesTop2WithStableTieBreaking) {
  const FeatureDescriptors query = MakeSiftDescriptors({0});
  const FeatureDescriptors train = MakeSiftDescriptors({0, 0, 1});

  const std::vector<MetalSiftTop2Match> top2 =
      ComputeSiftTop2MatchesCPU(query, train);

  ASSERT_EQ(top2.size(), 1);
  EXPECT_EQ(top2[0].best_train_idx, 0);
  EXPECT_EQ(top2[0].second_best_train_idx, 1);
  EXPECT_EQ(top2[0].best_distance_squared, 0);
  EXPECT_EQ(top2[0].second_best_distance_squared, 0);
}

TEST(MetalSiftDescriptorMatcher, RatioRejectsEqualNearestNeighbors) {
  MetalSiftMatchingOptions options = TestOptions();
  options.max_ratio = 0.8;

  FeatureMatches matches;
  MetalSiftDescriptorMatcher(options).Match(
      MakeSiftDescriptors({0}), MakeSiftDescriptors({0, 0}), &matches);

  EXPECT_TRUE(matches.empty());
}

TEST(MetalSiftDescriptorMatcher, DistanceThresholdRejectsFarMatches) {
  MetalSiftMatchingOptions options = TestOptions();
  options.max_distance_squared = 24;

  FeatureMatches matches;
  MetalSiftDescriptorMatcher(options).Match(
      MakeSiftDescriptors({0}), MakeSiftDescriptors({5}), &matches);

  EXPECT_TRUE(matches.empty());
}

TEST(MetalSiftDescriptorMatcher, CrossCheckKeepsOnlyMutualBestMatches) {
  MetalSiftMatchingOptions options = TestOptions();
  options.cross_check = true;

  FeatureMatches matches;
  MetalSiftDescriptorMatcher(options).Match(
      MakeSiftDescriptors({0, 1}), MakeSiftDescriptors({0}), &matches);

  ASSERT_EQ(matches.size(), 1);
  EXPECT_EQ(matches[0].point2D_idx1, 0);
  EXPECT_EQ(matches[0].point2D_idx2, 0);
}

TEST(MetalSiftDescriptorMatcher, ProducesQueryOrderedMatches) {
  MetalSiftMatchingOptions options = TestOptions();

  FeatureMatches matches;
  MetalSiftDescriptorMatcher(options).Match(
      MakeSiftDescriptors({3, 1, 2}), MakeSiftDescriptors({1, 2, 3}), &matches);

  ASSERT_EQ(matches.size(), 3);
  EXPECT_EQ(matches[0], FeatureMatch(0, 2));
  EXPECT_EQ(matches[1], FeatureMatch(1, 0));
  EXPECT_EQ(matches[2], FeatureMatch(2, 1));
}

TEST(MetalSiftDescriptorMatcher, MatchesExistingSiftBruteForceMatcher) {
  const FeatureDescriptors query =
      MakeSparseSiftDescriptors({{0, 1, 2, 3},
                                 {16, 17, 18, 19},
                                 {32, 33, 34, 35},
                                 {48, 49, 50, 51}});
  const FeatureDescriptors train =
      MakeSparseSiftDescriptors({{32, 33, 34, 35},
                                 {0, 1, 2, 3},
                                 {48, 49, 50, 51},
                                 {16, 17, 18, 19}});

  ExpectEqualMatches(MatchWithSiftBruteForce(query, train, false),
                     MatchWithMetalMatcher(query, train, false));
  ExpectEqualMatches(MatchWithSiftBruteForce(query, train, true),
                     MatchWithMetalMatcher(query, train, true));
}

TEST(MetalSiftDescriptorMatcher, RequestedMetalMatchesCPUFallback) {
  MetalSiftMatchingOptions options = TestOptions();
  options.use_metal = true;

  const FeatureDescriptors query = MakeSiftDescriptors({0, 1, 3});
  const FeatureDescriptors train = MakeSiftDescriptors({3, 1, 0, 2});

  const std::vector<MetalSiftTop2Match> cpu_top2 =
      ComputeSiftTop2MatchesCPU(query, train);
  const std::vector<MetalSiftTop2Match> requested_metal_top2 =
      MetalSiftDescriptorMatcher(options).ComputeTop2(query, train);

  ASSERT_EQ(requested_metal_top2.size(), cpu_top2.size());
  for (size_t i = 0; i < cpu_top2.size(); ++i) {
    EXPECT_EQ(requested_metal_top2[i].best_train_idx,
              cpu_top2[i].best_train_idx);
    EXPECT_EQ(requested_metal_top2[i].second_best_train_idx,
              cpu_top2[i].second_best_train_idx);
    EXPECT_EQ(requested_metal_top2[i].best_distance_squared,
              cpu_top2[i].best_distance_squared);
    EXPECT_EQ(requested_metal_top2[i].second_best_distance_squared,
              cpu_top2[i].second_best_distance_squared);
  }
}

TEST(MetalSiftDescriptorMatcher, MetalBackendMatchesCPUWhenAvailable) {
  MetalSiftMatchingOptions options = TestOptions();
  options.use_metal = true;

  const MetalSiftDescriptorMatcher matcher(options);
  if (!matcher.IsMetalAvailable()) {
    GTEST_SKIP() << "Metal SIFT matcher backend is unavailable";
  }

  const FeatureDescriptors query = MakeSiftDescriptors({0, 1, 3});
  const FeatureDescriptors train = MakeSiftDescriptors({3, 1, 0, 2});

  const std::vector<MetalSiftTop2Match> cpu_top2 =
      ComputeSiftTop2MatchesCPU(query, train);
  const std::vector<MetalSiftTop2Match> metal_top2 =
      matcher.ComputeTop2(query, train);

  ASSERT_EQ(metal_top2.size(), cpu_top2.size());
  for (size_t i = 0; i < cpu_top2.size(); ++i) {
    EXPECT_EQ(metal_top2[i].best_train_idx, cpu_top2[i].best_train_idx);
    EXPECT_EQ(metal_top2[i].second_best_train_idx,
              cpu_top2[i].second_best_train_idx);
    EXPECT_EQ(metal_top2[i].best_distance_squared,
              cpu_top2[i].best_distance_squared);
    EXPECT_EQ(metal_top2[i].second_best_distance_squared,
              cpu_top2[i].second_best_distance_squared);
  }
}

}  // namespace
}  // namespace colmap
