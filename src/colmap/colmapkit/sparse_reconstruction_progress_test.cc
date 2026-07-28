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

#include "colmap/math/random.h"
#include "colmap/scene/database.h"
#include "colmap/scene/reconstruction.h"
#include "colmap/scene/synthetic.h"
#include "colmap/sensor/bitmap.h"
#include "colmap/util/file.h"
#include "colmap/util/testing.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace colmap {
namespace {

struct RecordedProgressEvent {
  ColmapKitProgressStage stage;
  double fraction;
  size_t current;
  size_t total;
  std::string message;
  std::string detail;
  std::thread::id callback_thread;
};

struct CancellationProgressState {
  std::mutex mutex;
  std::condition_variable condition;
  bool saw_extraction_progress = false;
  ColmapKitProgressStage last_stage = COLMAPKIT_PROGRESS_STAGE_PREPARING;
};

void RecordProgress(const ColmapKitProgressEvent* event, void* user_data) {
  ASSERT_NE(event, nullptr);
  ASSERT_NE(user_data, nullptr);
  EXPECT_EQ(event->struct_size, sizeof(*event));
  auto* events = static_cast<std::vector<RecordedProgressEvent>*>(user_data);
  events->push_back(
      {event->stage,
       event->fraction,
       event->current,
       event->total,
       event->message == nullptr ? "" : event->message,
       event->detail == nullptr ? "" : event->detail,
       std::this_thread::get_id()});
}

void RecordCancellationProgress(const ColmapKitProgressEvent* event,
                                void* user_data) {
  if (event == nullptr || user_data == nullptr) {
    return;
  }
  auto* state = static_cast<CancellationProgressState*>(user_data);
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->last_stage = event->stage;
    if (event->stage == COLMAPKIT_PROGRESS_STAGE_FEATURE_EXTRACTION &&
        event->current > 0) {
      state->saw_extraction_progress = true;
    }
  }
  state->condition.notify_all();
}

std::vector<RecordedProgressEvent> EventsForStage(
    const std::vector<RecordedProgressEvent>& events,
    ColmapKitProgressStage stage) {
  std::vector<RecordedProgressEvent> stage_events;
  std::copy_if(
      events.begin(),
      events.end(),
      std::back_inserter(stage_events),
      [stage](const RecordedProgressEvent& event) {
        return event.stage == stage;
      });
  return stage_events;
}

size_t FirstStageIndex(const std::vector<RecordedProgressEvent>& events,
                       ColmapKitProgressStage stage) {
  const auto it =
      std::find_if(events.begin(), events.end(), [stage](const auto& event) {
        return event.stage == stage;
      });
  EXPECT_NE(it, events.end());
  return static_cast<size_t>(std::distance(events.begin(), it));
}

void ExpectMonotonicStage(
    const std::vector<RecordedProgressEvent>& events,
    size_t expected_total) {
  ASSERT_GE(events.size(), 2);
  EXPECT_EQ(events.front().current, 0);
  EXPECT_EQ(events.front().total, expected_total);
  for (size_t i = 0; i < events.size(); ++i) {
    EXPECT_EQ(events[i].total, expected_total);
    EXPECT_LE(events[i].current, expected_total);
    if (i > 0) {
      EXPECT_LE(events[i - 1].current, events[i].current);
    }
  }
}

TEST(ColmapKitSparseReconstructionProgress,
     EmitsMonotonicCountsAndResetsAtStageBoundaries) {
  const auto test_dir = CreateTestDir();
  const auto database_path = test_dir / "database.db";
  const auto image_path = test_dir / "images";
  const auto output_path = test_dir / "sparse";
  CreateDirIfNotExists(image_path);

  constexpr size_t kNumImages = 6;
  SetPRNGSeed(0);
  SyntheticDatasetOptions options;
  options.num_rigs = 1;
  options.num_cameras_per_rig = 1;
  options.num_frames_per_rig = kNumImages;
  options.num_points3D = 120;
  options.num_points2D_without_point3D = 0;

  auto database = Database::Open(database_path);
  Reconstruction unused_reconstruction;
  SynthesizeDataset(options, &unused_reconstruction, database.get());

  Bitmap bitmap(32, 32, /*as_rgb=*/false);
  bitmap.Fill(BitmapColor<uint8_t>(0));
  for (const Image& image : database->ReadAllImages()) {
    ASSERT_TRUE(bitmap.Write(image_path / image.Name()));
  }
  database.reset();

  std::vector<RecordedProgressEvent> events;
  ColmapKitSparseReconstructionConfig config = {};
  config.struct_size = sizeof(config);
  config.database_path = database_path.c_str();
  config.image_path = image_path.c_str();
  config.output_path = output_path.c_str();
  config.single_camera = 1;
  config.max_image_size = -1;
  config.num_threads = 1;
  config.use_gpu = 0;
  config.matcher = COLMAPKIT_MATCHER_EXHAUSTIVE;
  config.mapper_min_num_matches = 15;
  config.mapper_min_model_size = 3;
  config.mapper_random_seed = 0;
  config.write_sparse_text = 0;
  config.progress_callback = RecordProgress;
  config.progress_user_data = &events;

  ColmapKitSparseReconstructionResult result = {};
  result.struct_size = sizeof(result);
  ASSERT_EQ(ColmapKitRunSparseReconstruction(&config, &result),
            COLMAPKIT_STATUS_OK)
      << result.message;

  constexpr size_t kNumPairs = kNumImages * (kNumImages - 1) / 2;
  const auto extraction_events =
      EventsForStage(events, COLMAPKIT_PROGRESS_STAGE_FEATURE_EXTRACTION);
  const auto matching_events =
      EventsForStage(events, COLMAPKIT_PROGRESS_STAGE_MATCHING);
  const auto mapping_events =
      EventsForStage(events, COLMAPKIT_PROGRESS_STAGE_MAPPING);

  ExpectMonotonicStage(extraction_events, kNumImages);
  EXPECT_EQ(extraction_events.back().current, kNumImages);
  EXPECT_DOUBLE_EQ(extraction_events.back().fraction, 1.0);
  EXPECT_EQ(extraction_events.back().message, "Feature extraction complete");
  EXPECT_FALSE(extraction_events.back().detail.empty());

  ExpectMonotonicStage(matching_events, kNumPairs);
  EXPECT_EQ(matching_events.back().current, kNumPairs);
  EXPECT_DOUBLE_EQ(matching_events.back().fraction, 1.0);
  EXPECT_EQ(matching_events.back().message, "Feature matching complete");

  ExpectMonotonicStage(mapping_events, kNumImages);
  EXPECT_EQ(mapping_events.back().current, result.registered_images);
  EXPECT_GT(mapping_events.back().current, 0);
  EXPECT_EQ(mapping_events.back().message, "Incremental mapping complete");

  EXPECT_LT(
      FirstStageIndex(events, COLMAPKIT_PROGRESS_STAGE_FEATURE_EXTRACTION),
      FirstStageIndex(events, COLMAPKIT_PROGRESS_STAGE_MATCHING));
  EXPECT_LT(FirstStageIndex(events, COLMAPKIT_PROGRESS_STAGE_MATCHING),
            FirstStageIndex(events, COLMAPKIT_PROGRESS_STAGE_MAPPING));

  const std::thread::id run_thread = std::this_thread::get_id();
  for (const RecordedProgressEvent& event : events) {
    EXPECT_EQ(event.callback_thread, run_thread);
  }
}

TEST(ColmapKitSparseReconstructionProgress,
     CancellationStillStopsActiveFeatureExtraction) {
  const auto test_dir = CreateTestDir();
  const auto database_path = test_dir / "database.db";
  const auto image_path = test_dir / "images";
  const auto output_path = test_dir / "sparse";
  CreateDirIfNotExists(image_path);

  constexpr int kNumImages = 24;
  Bitmap bitmap(512, 384, /*as_rgb=*/false);
  bitmap.Fill(BitmapColor<uint8_t>(0));
  for (int y = 12; y < bitmap.Height(); y += 18) {
    for (int x = 12; x < bitmap.Width(); x += 18) {
      for (int offset_y = -3; offset_y <= 3; ++offset_y) {
        for (int offset_x = -3; offset_x <= 3; ++offset_x) {
          bitmap.SetPixel(x + offset_x,
                          y + offset_y,
                          BitmapColor<uint8_t>(255));
        }
      }
    }
  }
  for (int i = 0; i < kNumImages; ++i) {
    ASSERT_TRUE(
        bitmap.Write(image_path / ("frame_" + std::to_string(i) + ".png")));
  }

  CancellationProgressState progress_state;
  ColmapKitSparseReconstructionConfig config = {};
  config.struct_size = sizeof(config);
  config.database_path = database_path.c_str();
  config.image_path = image_path.c_str();
  config.output_path = output_path.c_str();
  config.camera_model = "SIMPLE_PINHOLE";
  config.camera_params = "480,256,192";
  config.single_camera = 1;
  config.max_image_size = 512;
  config.num_threads = 1;
  config.use_gpu = 0;
  config.matcher = COLMAPKIT_MATCHER_SEQUENTIAL;
  config.sequential_overlap = 4;
  config.write_sparse_text = 0;
  config.progress_callback = RecordCancellationProgress;
  config.progress_user_data = &progress_state;

  ColmapKitSparseReconstructionJob* job = nullptr;
  ASSERT_EQ(ColmapKitStartSparseReconstruction(&config, &job),
            COLMAPKIT_STATUS_OK);
  ASSERT_NE(job, nullptr);

  {
    std::unique_lock<std::mutex> lock(progress_state.mutex);
    ASSERT_TRUE(progress_state.condition.wait_for(
        lock, std::chrono::seconds(30), [&]() {
          return progress_state.saw_extraction_progress;
        }));
  }

  EXPECT_EQ(ColmapKitCancelSparseReconstruction(job), COLMAPKIT_STATUS_OK);
  ColmapKitSparseReconstructionResult result = {};
  result.struct_size = sizeof(result);
  EXPECT_EQ(ColmapKitWaitSparseReconstruction(job, &result),
            COLMAPKIT_STATUS_CANCELLED);
  EXPECT_EQ(result.status, COLMAPKIT_STATUS_CANCELLED);
  ColmapKitReleaseSparseReconstructionJob(job);

  std::lock_guard<std::mutex> lock(progress_state.mutex);
  EXPECT_EQ(progress_state.last_stage, COLMAPKIT_PROGRESS_STAGE_CANCELLED);
}

}  // namespace
}  // namespace colmap
