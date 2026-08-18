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
#include <stdint.h>

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
#define COLMAPKIT_METAL_DEVICE_NAME_CAPACITY 128

typedef enum ColmapKitStatus {
  COLMAPKIT_STATUS_OK = 0,
  COLMAPKIT_STATUS_INVALID_ARGUMENT = 1,
  COLMAPKIT_STATUS_UNSUPPORTED = 2,
  COLMAPKIT_STATUS_RUNTIME_ERROR = 3,
  COLMAPKIT_STATUS_CANCELLED = 4
} ColmapKitStatus;

typedef enum ColmapKitMatcherKind {
  COLMAPKIT_MATCHER_SEQUENTIAL = 0,
  COLMAPKIT_MATCHER_EXHAUSTIVE = 1,
  COLMAPKIT_MATCHER_SPATIAL = 2
} ColmapKitMatcherKind;

typedef enum ColmapKitModelOutputType {
  COLMAPKIT_MODEL_OUTPUT_TYPE_BIN = 0,
  COLMAPKIT_MODEL_OUTPUT_TYPE_TXT = 1
} ColmapKitModelOutputType;

typedef enum ColmapKitComputeBackend {
  COLMAPKIT_COMPUTE_BACKEND_UNKNOWN = 0,
  COLMAPKIT_COMPUTE_BACKEND_CPU = 1,
  COLMAPKIT_COMPUTE_BACKEND_METAL = 2
} ColmapKitComputeBackend;

typedef enum ColmapKitProgressStage {
  COLMAPKIT_PROGRESS_STAGE_PREPARING = 0,
  COLMAPKIT_PROGRESS_STAGE_FEATURE_EXTRACTION = 1,
  COLMAPKIT_PROGRESS_STAGE_MATCHING = 2,
  COLMAPKIT_PROGRESS_STAGE_MAPPING = 3,
  COLMAPKIT_PROGRESS_STAGE_SPARSE_TEXT_EXPORT = 4,
  COLMAPKIT_PROGRESS_STAGE_FINISHED = 5,
  COLMAPKIT_PROGRESS_STAGE_FAILED = 6,
  COLMAPKIT_PROGRESS_STAGE_CANCELLED = 7
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
  // Additive acceleration controls. Zero inherits num_threads. These fields
  // are ignored for v0.2.1-sized callers.
  int extraction_num_threads;
  int matching_num_threads;
  int mapper_num_threads;
  // When nonzero, a requested Metal path fails instead of falling back.
  int require_metal_sift;
  int require_metal_matching;
  // Optional mechanically readable JSON evidence output.
  const char* evidence_path;
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
  uint32_t sparse_reconstruction_abi_version;
  uint32_t no_fallback_satisfied;
  int requested_extraction_num_threads;
  int requested_matching_num_threads;
  int requested_mapper_num_threads;
  int effective_extraction_num_threads;
  int effective_matching_num_threads;
  int effective_geometric_verification_num_threads;
  int effective_mapper_num_threads;
  int effective_bundle_adjustment_num_threads;
  ColmapKitComputeBackend extraction_backend;
  ColmapKitComputeBackend matching_backend;
  ColmapKitComputeBackend mapping_backend;
  ColmapKitComputeBackend bundle_adjustment_backend;
  int metal_matching_compiled;
  int metal_matching_available;
  int metal_sift_compiled;
  int metal_sift_available;
  int metal_sift_requested;
  uint64_t metal_sift_operations;
  uint64_t metal_matching_operations;
  uint64_t metal_sift_fallbacks;
  uint64_t metal_matching_fallbacks;
  double extraction_seconds;
  double matching_seconds;
  double mapping_bundle_adjustment_seconds;
  double export_seconds;
  double total_seconds;
  uint64_t peak_resident_memory_bytes;
  char metal_device_name[COLMAPKIT_METAL_DEVICE_NAME_CAPACITY];
} ColmapKitSparseReconstructionResult;

typedef struct ColmapKitPointFilteringConfig {
  size_t struct_size;
  const char* input_path;
  const char* output_path;
  int min_track_len;
  double max_reproj_error;
  double min_tri_angle;
} ColmapKitPointFilteringConfig;

typedef struct ColmapKitPointFilteringResult {
  size_t struct_size;
  ColmapKitStatus status;
  size_t input_points;
  size_t output_points;
  size_t input_registered_images;
  size_t output_registered_images;
  size_t filtered_points;
  size_t filtered_observations;
  char message[COLMAPKIT_MESSAGE_CAPACITY];
} ColmapKitPointFilteringResult;

typedef struct ColmapKitModelCroppingConfig {
  size_t struct_size;
  const char* input_path;
  const char* output_path;
  double min_x;
  double min_y;
  double min_z;
  double max_x;
  double max_y;
  double max_z;
} ColmapKitModelCroppingConfig;

typedef struct ColmapKitModelCroppingResult {
  size_t struct_size;
  ColmapKitStatus status;
  size_t input_points;
  size_t output_points;
  size_t input_registered_images;
  size_t output_registered_images;
  size_t removed_points;
  size_t removed_registered_images;
  char message[COLMAPKIT_MESSAGE_CAPACITY];
} ColmapKitModelCroppingResult;

typedef struct ColmapKitModelConversionConfig {
  size_t struct_size;
  const char* input_path;
  const char* output_path;
  ColmapKitModelOutputType output_type;
} ColmapKitModelConversionConfig;

typedef struct ColmapKitModelConversionResult {
  size_t struct_size;
  ColmapKitStatus status;
  size_t input_points;
  size_t output_points;
  size_t input_registered_images;
  size_t output_registered_images;
  ColmapKitModelOutputType output_type;
  size_t files_written;
  char message[COLMAPKIT_MESSAGE_CAPACITY];
} ColmapKitModelConversionResult;

typedef struct ColmapKitSparseReconstructionJob
    ColmapKitSparseReconstructionJob;

// ColmapKit ABI V2 is strictly additive. Every V2 input and output structure
// starts with struct_size. Callers must set it to the number of initialized
// bytes. The library never reads or writes past that boundary.
#define COLMAPKIT_ABI_VERSION_V2 2u
#define COLMAPKIT_SHA256_CAPACITY 65
#define COLMAPKIT_RGB_PRIOR_VARIANT_V2_D 4u
#define COLMAPKIT_TRACKED_IMAGE_FLAG_V2_INCLUDED 1u

typedef enum ColmapKitTrackingStateV2 {
  COLMAPKIT_TRACKING_STATE_V2_NORMAL = 0,
  COLMAPKIT_TRACKING_STATE_V2_LIMITED = 1,
  COLMAPKIT_TRACKING_STATE_V2_UNAVAILABLE = 2
} ColmapKitTrackingStateV2;

typedef enum ColmapKitCameraModelV2 {
  COLMAPKIT_CAMERA_MODEL_V2_PINHOLE = 0,
  COLMAPKIT_CAMERA_MODEL_V2_SIMPLE_PINHOLE = 1,
  COLMAPKIT_CAMERA_MODEL_V2_OPENCV = 2
} ColmapKitCameraModelV2;

typedef enum ColmapKitProgressStageV2 {
  COLMAPKIT_PROGRESS_STAGE_V2_PREPARING = 0,
  COLMAPKIT_PROGRESS_STAGE_V2_FEATURE_EXTRACTION = 1,
  COLMAPKIT_PROGRESS_STAGE_V2_MATCHING = 2,
  COLMAPKIT_PROGRESS_STAGE_V2_TRIANGULATION = 3,
  COLMAPKIT_PROGRESS_STAGE_V2_BUNDLE_ADJUSTMENT = 4,
  COLMAPKIT_PROGRESS_STAGE_V2_EXPORT = 5,
  COLMAPKIT_PROGRESS_STAGE_V2_PRIOR_DENSIFICATION = 6,
  COLMAPKIT_PROGRESS_STAGE_V2_PRIOR_GEOMETRY = 7,
  COLMAPKIT_PROGRESS_STAGE_V2_FINISHED = 8,
  COLMAPKIT_PROGRESS_STAGE_V2_FAILED = 9,
  COLMAPKIT_PROGRESS_STAGE_V2_CANCELLED = 10
} ColmapKitProgressStageV2;

typedef struct ColmapKitProgressEventV2 {
  uint32_t struct_size;
  uint32_t stage;
  double fraction;
  uint64_t current;
  uint64_t total;
  double elapsed_seconds;
  const char* message;
  const char* detail;
} ColmapKitProgressEventV2;

typedef void (*ColmapKitProgressCallbackV2)(
    const ColmapKitProgressEventV2* event, void* user_data);

// One already-filtered RGB frame. V2 currently accepts only NORMAL tracking.
// world_from_camera is a finite, rigid, column-major ARKit transform in meters.
typedef struct ColmapKitTrackedImageV2 {
  uint32_t struct_size;
  uint32_t camera_model;
  uint64_t stable_id;
  uint32_t order_index;
  uint32_t encoded_width;
  uint32_t encoded_height;
  uint32_t num_camera_params;
  double camera_params[8];
  double world_from_camera[16];
  uint32_t tracking_state;
  int32_t tracking_reason;
  uint32_t inclusion_flags;
  uint32_t reserved0;
  double translation_weight;
  double rotation_weight;
  const char* image_path;
} ColmapKitTrackedImageV2;

typedef struct ColmapKitTrackedPoseConfigV2 {
  uint32_t struct_size;
  uint32_t flags;
  const ColmapKitTrackedImageV2* images;
  uint32_t num_images;
  uint32_t max_features_per_image;
  uint32_t temporal_neighbor_count;
  uint32_t max_revisit_neighbors_per_image;
  uint32_t max_image_pairs;
  uint32_t max_triangulation_passes;
  uint32_t max_bundle_adjustment_iterations;
  uint32_t random_seed;
  uint32_t num_threads;
  double revisit_min_translation_meters;
  double revisit_max_translation_meters;
  double revisit_max_rotation_degrees;
  double min_triangulation_angle_degrees;
  double max_reprojection_error_pixels;
  double max_allowed_scale_drift_ratio;
  const char* database_path;
  const char* output_model_path;
  const char* refined_pose_path;
  const char* evidence_path;
  ColmapKitProgressCallbackV2 progress_callback;
  void* progress_user_data;
  // Optional maximum dimension for the RGB bitmap used only by feature
  // extraction. Zero preserves full-resolution extraction. Keypoints are
  // rescaled back into the encoded image coordinate system before matching.
  uint32_t max_feature_image_size;
} ColmapKitTrackedPoseConfigV2;

typedef struct ColmapKitTrackedPoseResultV2 {
  uint32_t struct_size;
  uint32_t status;
  uint32_t registered_images;
  uint32_t matched_pairs;
  uint64_t sparse_points;
  uint64_t observations;
  double initial_mean_reprojection_error;
  double final_mean_reprojection_error;
  double max_translation_correction_meters;
  double max_rotation_correction_degrees;
  double measured_scale_drift_ratio;
  double feature_seconds;
  double matching_seconds;
  double triangulation_seconds;
  double bundle_adjustment_seconds;
  double export_seconds;
  char refined_pose_sha256[COLMAPKIT_SHA256_CAPACITY];
  char message[COLMAPKIT_MESSAGE_CAPACITY];
} ColmapKitTrackedPoseResultV2;

typedef struct ColmapKitRGBPriorConfigV2 {
  uint32_t struct_size;
  uint32_t flags;
  const ColmapKitTrackedImageV2* images;
  uint32_t num_images;
  uint32_t normal_neighbor_count;
  uint32_t max_points_per_spatial_cell;
  uint32_t max_output_gaussians;
  uint32_t minimum_densification_percent;
  uint32_t random_seed;
  double spatial_cell_size_meters;
  double min_spacing_meters;
  double max_spacing_meters;
  double tangent_scale_multiplier;
  double normal_scale_multiplier;
  double initial_opacity;
  const char* database_path;
  const char* refined_model_path;
  const char* refined_pose_path;
  const char* expected_refined_pose_sha256;
  const char* output_ply_path;
  const char* evidence_path;
  ColmapKitProgressCallbackV2 progress_callback;
  void* progress_user_data;
} ColmapKitRGBPriorConfigV2;

typedef struct ColmapKitRGBPriorResultV2 {
  uint32_t struct_size;
  uint32_t status;
  uint32_t variant;
  uint32_t sh_degree;
  uint64_t sparse_input_points;
  uint64_t correspondence_candidates;
  uint64_t rejected_candidates;
  uint64_t output_gaussians;
  uint64_t density_capped_points;
  double median_spacing_meters;
  double median_anisotropy_ratio;
  double densification_ratio;
  double densification_seconds;
  double geometry_seconds;
  double export_seconds;
  char input_pose_sha256[COLMAPKIT_SHA256_CAPACITY];
  char output_pose_sha256[COLMAPKIT_SHA256_CAPACITY];
  char output_ply_sha256[COLMAPKIT_SHA256_CAPACITY];
  char message[COLMAPKIT_MESSAGE_CAPACITY];
} ColmapKitRGBPriorResultV2;

typedef struct ColmapKitTrackedPoseJobV2 ColmapKitTrackedPoseJobV2;
typedef struct ColmapKitRGBPriorJobV2 ColmapKitRGBPriorJobV2;

COLMAPKIT_EXPORT const char* ColmapKitVersion(void);

COLMAPKIT_EXPORT uint32_t ColmapKitGetABIVersionV2(void);
COLMAPKIT_EXPORT const char* ColmapKitGetReleaseVersionV2(void);
COLMAPKIT_EXPORT const char* ColmapKitGetEngineBuildIdentityV2(void);

COLMAPKIT_EXPORT ColmapKitStatus
ColmapKitInitialize(const char* application_name);

COLMAPKIT_EXPORT ColmapKitStatus ColmapKitRunSparseReconstruction(
    const ColmapKitSparseReconstructionConfig* config,
    ColmapKitSparseReconstructionResult* result);

COLMAPKIT_EXPORT ColmapKitStatus ColmapKitStartSparseReconstruction(
    const ColmapKitSparseReconstructionConfig* config,
    ColmapKitSparseReconstructionJob** job);

COLMAPKIT_EXPORT ColmapKitStatus
ColmapKitCancelSparseReconstruction(ColmapKitSparseReconstructionJob* job);

COLMAPKIT_EXPORT ColmapKitStatus
ColmapKitWaitSparseReconstruction(ColmapKitSparseReconstructionJob* job,
                                  ColmapKitSparseReconstructionResult* result);

COLMAPKIT_EXPORT void ColmapKitReleaseSparseReconstructionJob(
    ColmapKitSparseReconstructionJob* job);

COLMAPKIT_EXPORT ColmapKitStatus
ColmapKitRunPointFiltering(const ColmapKitPointFilteringConfig* config,
                           ColmapKitPointFilteringResult* result);

COLMAPKIT_EXPORT ColmapKitStatus
ColmapKitRunModelCropping(const ColmapKitModelCroppingConfig* config,
                          ColmapKitModelCroppingResult* result);

COLMAPKIT_EXPORT ColmapKitStatus
ColmapKitRunModelConversion(const ColmapKitModelConversionConfig* config,
                            ColmapKitModelConversionResult* result);

COLMAPKIT_EXPORT ColmapKitStatus ColmapKitRunTrackedPoseReconstructionV2(
    const ColmapKitTrackedPoseConfigV2* config,
    ColmapKitTrackedPoseResultV2* result);
COLMAPKIT_EXPORT ColmapKitStatus ColmapKitStartTrackedPoseReconstructionV2(
    const ColmapKitTrackedPoseConfigV2* config,
    ColmapKitTrackedPoseJobV2** job);
COLMAPKIT_EXPORT ColmapKitStatus
ColmapKitCancelTrackedPoseReconstructionV2(ColmapKitTrackedPoseJobV2* job);
COLMAPKIT_EXPORT ColmapKitStatus ColmapKitWaitTrackedPoseReconstructionV2(
    ColmapKitTrackedPoseJobV2* job, ColmapKitTrackedPoseResultV2* result);
COLMAPKIT_EXPORT void ColmapKitReleaseTrackedPoseReconstructionJobV2(
    ColmapKitTrackedPoseJobV2* job);

COLMAPKIT_EXPORT ColmapKitStatus ColmapKitRunRGBGaussianPriorV2(
    const ColmapKitRGBPriorConfigV2* config, ColmapKitRGBPriorResultV2* result);
COLMAPKIT_EXPORT ColmapKitStatus ColmapKitStartRGBGaussianPriorV2(
    const ColmapKitRGBPriorConfigV2* config, ColmapKitRGBPriorJobV2** job);
COLMAPKIT_EXPORT ColmapKitStatus
ColmapKitCancelRGBGaussianPriorV2(ColmapKitRGBPriorJobV2* job);
COLMAPKIT_EXPORT ColmapKitStatus ColmapKitWaitRGBGaussianPriorV2(
    ColmapKitRGBPriorJobV2* job, ColmapKitRGBPriorResultV2* result);
COLMAPKIT_EXPORT void ColmapKitReleaseRGBGaussianPriorJobV2(
    ColmapKitRGBPriorJobV2* job);

#ifdef __cplusplus
}
#endif
