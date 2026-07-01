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

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#if defined(COLMAPKIT_BUILDING_LIBRARY)
#define COLMAPKIT_EXPORT __declspec(dllexport)
#else
#define COLMAPKIT_EXPORT __declspec(dllimport)
#endif
#else
#define COLMAPKIT_EXPORT __attribute__((visibility("default")))
#endif

#define COLMAPKIT_MESSAGE_CAPACITY 1024

typedef enum ColmapKitStatus {
  COLMAPKIT_STATUS_OK = 0,
  COLMAPKIT_STATUS_INVALID_ARGUMENT = 1,
  COLMAPKIT_STATUS_UNSUPPORTED = 2,
  COLMAPKIT_STATUS_RUNTIME_ERROR = 3
} ColmapKitStatus;

typedef enum ColmapKitMatcherKind {
  COLMAPKIT_MATCHER_SEQUENTIAL = 0,
  COLMAPKIT_MATCHER_EXHAUSTIVE = 1,
  COLMAPKIT_MATCHER_SPATIAL = 2
} ColmapKitMatcherKind;

typedef enum ColmapKitProgressStage {
  COLMAPKIT_PROGRESS_STAGE_PREPARING = 0,
  COLMAPKIT_PROGRESS_STAGE_FEATURE_EXTRACTION = 1,
  COLMAPKIT_PROGRESS_STAGE_MATCHING = 2,
  COLMAPKIT_PROGRESS_STAGE_MAPPING = 3,
  COLMAPKIT_PROGRESS_STAGE_SPARSE_TEXT_EXPORT = 4,
  COLMAPKIT_PROGRESS_STAGE_FINISHED = 5,
  COLMAPKIT_PROGRESS_STAGE_FAILED = 6
} ColmapKitProgressStage;

typedef struct ColmapKitProgressEvent {
  size_t struct_size;
  ColmapKitProgressStage stage;
  double fraction;
  size_t current;
  size_t total;
  const char* message;
  const char* detail;
} ColmapKitProgressEvent;

typedef void (*ColmapKitProgressCallback)(const ColmapKitProgressEvent* event,
                                          void* user_data);

typedef struct ColmapKitSparseReconstructionConfig {
  size_t struct_size;
  const char* database_path;
  const char* image_path;
  const char* output_path;
  const char* sparse_text_output_path;
  const char* image_list_path;
  const char* camera_model;
  const char* camera_params;
  int single_camera;
  int max_image_size;
  int num_threads;
  int use_gpu;
  int use_metal_sift;
  int use_metal_matching;
  int estimate_affine_shape;
  int domain_size_pooling;
  ColmapKitMatcherKind matcher;
  int sequential_overlap;
  int mapper_min_num_matches;
  int mapper_min_model_size;
  int mapper_random_seed;
  int write_sparse_text;
  ColmapKitProgressCallback progress_callback;
  void* progress_user_data;
} ColmapKitSparseReconstructionConfig;

typedef struct ColmapKitSparseReconstructionResult {
  size_t struct_size;
  ColmapKitStatus status;
  size_t num_models;
  size_t largest_model_index;
  size_t registered_images;
  size_t sparse_points;
  size_t observations;
  double mean_reprojection_error;
  char message[COLMAPKIT_MESSAGE_CAPACITY];
} ColmapKitSparseReconstructionResult;

COLMAPKIT_EXPORT const char* ColmapKitVersion(void);

COLMAPKIT_EXPORT ColmapKitStatus
ColmapKitInitialize(const char* application_name);

COLMAPKIT_EXPORT ColmapKitStatus ColmapKitRunSparseReconstruction(
    const ColmapKitSparseReconstructionConfig* config,
    ColmapKitSparseReconstructionResult* result);

#ifdef __cplusplus
}
#endif
