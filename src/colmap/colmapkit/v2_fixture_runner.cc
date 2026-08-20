// A deterministic local/Simulator validation runner for ColmapKit ABI V2.

#include "colmap/colmapkit/colmapkit.h"

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

Eigen::Matrix4d ARKitWorldFromCamera(size_t index, size_t count) {
  const double fraction = count == 1 ? 0.0 : static_cast<double>(index) / (count - 1);
  const Eigen::Vector3d center(-0.65 + 1.3 * fraction,
                               0.04 * std::sin(static_cast<double>(index)), 0.0);
  const Eigen::Vector3d target(0.0, 0.0, 4.6);
  const Eigen::Vector3d z = (target - center).normalized();
  const Eigen::Vector3d x = z.cross(Eigen::Vector3d::UnitY()).normalized();
  const Eigen::Vector3d y = z.cross(x);
  Eigen::Matrix3d colmap_camera_from_world_rotation;
  colmap_camera_from_world_rotation.row(0) = x;
  colmap_camera_from_world_rotation.row(1) = y;
  colmap_camera_from_world_rotation.row(2) = z;
  Eigen::Matrix4d world_from_colmap_camera = Eigen::Matrix4d::Identity();
  world_from_colmap_camera.block<3, 3>(0, 0) =
      colmap_camera_from_world_rotation.transpose();
  world_from_colmap_camera.block<3, 1>(0, 3) = center;
  Eigen::Matrix4d basis = Eigen::Matrix4d::Identity();
  basis(1, 1) = -1;
  basis(2, 2) = -1;
  return world_from_colmap_camera * basis;
}

struct ProgressState {
  double last_elapsed_seconds = 0.0;
  bool monotonic = true;
  bool finished = false;
};

void Progress(const ColmapKitProgressEventV2* event, void* user_data) {
  if (event == nullptr) return;
  auto* state = static_cast<ProgressState*>(user_data);
  if (state != nullptr) {
    state->monotonic = state->monotonic &&
                       event->elapsed_seconds >= state->last_elapsed_seconds;
    state->last_elapsed_seconds = event->elapsed_seconds;
    state->finished = event->stage == COLMAPKIT_PROGRESS_STAGE_V2_FINISHED;
  }
  std::cerr << "stage=" << event->stage << " fraction=" << event->fraction
            << " message=" << (event->message == nullptr ? "" : event->message)
            << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "Usage: colmapkit_v2_fixture_runner IMAGE_DIR OUTPUT_DIR\n";
    return 2;
  }
  constexpr size_t kCount = 8;
  const std::filesystem::path image_dir(argv[1]);
  const std::filesystem::path output_dir(argv[2]);
  std::filesystem::create_directories(output_dir);
  std::vector<std::string> paths(kCount);
  std::vector<ColmapKitTrackedImageV2> images(kCount);
  for (size_t i = 0; i < kCount; ++i) {
    std::ostringstream name;
    name << "frame_" << std::setw(3) << std::setfill('0') << i << ".pgm";
    paths[i] = (image_dir / name.str()).string();
    if (!std::filesystem::is_regular_file(paths[i])) {
      std::cerr << "Missing fixture image: " << paths[i] << '\n';
      return 2;
    }
    images[i].struct_size = sizeof(images[i]);
    images[i].camera_model = COLMAPKIT_CAMERA_MODEL_V2_SIMPLE_PINHOLE;
    images[i].stable_id = 1000 + i;
    images[i].order_index = static_cast<uint32_t>(i);
    images[i].encoded_width = 1024;
    images[i].encoded_height = 768;
    images[i].num_camera_params = 3;
    images[i].camera_params[0] = 900;
    images[i].camera_params[1] = 512;
    images[i].camera_params[2] = 384;
    const Eigen::Matrix4d transform = ARKitWorldFromCamera(i, kCount);
    for (int col = 0; col < 4; ++col) {
      for (int row = 0; row < 4; ++row) {
        images[i].world_from_camera[col * 4 + row] = transform(row, col);
      }
    }
    images[i].tracking_state = COLMAPKIT_TRACKING_STATE_V2_NORMAL;
    images[i].inclusion_flags = COLMAPKIT_TRACKED_IMAGE_FLAG_V2_INCLUDED;
    images[i].translation_weight = 1.0;
    images[i].rotation_weight = 1.0;
    images[i].image_path = paths[i].c_str();
  }

  const std::string database = (output_dir / "database.db").string();
  const std::string model = (output_dir / "sparse").string();
  const std::string poses = (output_dir / "refined-poses.json").string();
  const std::string tracked_evidence =
      (output_dir / "tracked-evidence.json").string();
  ColmapKitTrackedPoseConfigV2 tracked{};
  tracked.struct_size = sizeof(tracked);
  tracked.flags = COLMAPKIT_TRACKED_POSE_FLAG_V2_PAIR_GRAPH_TELEMETRY;
  tracked.images = images.data();
  tracked.num_images = images.size();
  tracked.max_features_per_image = 4096;
  tracked.temporal_neighbor_count = 3;
  tracked.max_revisit_neighbors_per_image = 2;
  tracked.max_image_pairs = 28;
  tracked.max_triangulation_passes = 2;
  tracked.max_bundle_adjustment_iterations = 40;
  tracked.random_seed = 7;
  tracked.num_threads = 1;
  tracked.revisit_min_translation_meters = 0.1;
  tracked.revisit_max_translation_meters = 2.0;
  tracked.revisit_max_rotation_degrees = 45;
  tracked.min_triangulation_angle_degrees = 0.1;
  tracked.max_reprojection_error_pixels = 4;
  tracked.max_allowed_scale_drift_ratio = 1e-9;
  tracked.database_path = database.c_str();
  tracked.output_model_path = model.c_str();
  tracked.refined_pose_path = poses.c_str();
  tracked.evidence_path = tracked_evidence.c_str();
  ProgressState tracked_progress;
  tracked.progress_callback = Progress;
  tracked.progress_user_data = &tracked_progress;
  ColmapKitTrackedPoseResultV2 tracked_result{};
  tracked_result.struct_size = sizeof(tracked_result);
  if (ColmapKitRunTrackedPoseReconstructionV2(&tracked, &tracked_result) !=
      COLMAPKIT_STATUS_OK) {
    std::cerr << "Tracked V2 failed: " << tracked_result.message << '\n';
    return 1;
  }
  if (!tracked_progress.monotonic || !tracked_progress.finished) {
    std::cerr << "Tracked V2 progress was not monotonic and terminal.\n";
    return 1;
  }
  std::ifstream tracked_evidence_stream(tracked_evidence);
  const std::string tracked_evidence_text{
      std::istreambuf_iterator<char>(tracked_evidence_stream),
      std::istreambuf_iterator<char>()};
  if (tracked_evidence_text.find("\"pair_graph_telemetry\"") ==
      std::string::npos) {
    std::cerr << "Tracked V2 did not write requested pair telemetry.\n";
    return 1;
  }

  const std::string ply = (output_dir / "init.ply").string();
  const std::string prior_evidence = (output_dir / "prior-evidence.json").string();
  ColmapKitRGBPriorConfigV2 prior{};
  prior.struct_size = sizeof(prior);
  prior.images = images.data();
  prior.num_images = images.size();
  prior.normal_neighbor_count = 12;
  prior.max_points_per_spatial_cell = 96;
  prior.max_output_gaussians = 12000;
  prior.minimum_densification_percent = 10;
  prior.random_seed = 7;
  prior.spatial_cell_size_meters = 0.08;
  prior.min_spacing_meters = 0.001;
  prior.max_spacing_meters = 0.25;
  prior.tangent_scale_multiplier = 0.8;
  prior.normal_scale_multiplier = 0.2;
  prior.initial_opacity = 0.1;
  prior.database_path = database.c_str();
  prior.refined_model_path = model.c_str();
  prior.refined_pose_path = poses.c_str();
  prior.expected_refined_pose_sha256 = tracked_result.refined_pose_sha256;
  prior.output_ply_path = ply.c_str();
  prior.evidence_path = prior_evidence.c_str();
  ProgressState prior_progress;
  prior.progress_callback = Progress;
  prior.progress_user_data = &prior_progress;
  ColmapKitRGBPriorResultV2 prior_result{};
  prior_result.struct_size = sizeof(prior_result);
  if (ColmapKitRunRGBGaussianPriorV2(&prior, &prior_result) !=
      COLMAPKIT_STATUS_OK) {
    std::cerr << "Prior V2 failed: " << prior_result.message << '\n';
    return 1;
  }
  if (!prior_progress.monotonic || !prior_progress.finished) {
    std::cerr << "Prior V2 progress was not monotonic and terminal.\n";
    return 1;
  }
  std::cout << "tracked_sha256=" << tracked_result.refined_pose_sha256 << '\n'
            << "registered_images=" << tracked_result.registered_images << '\n'
            << "sparse_points=" << tracked_result.sparse_points << '\n'
            << "output_gaussians=" << prior_result.output_gaussians << '\n'
            << "densification_ratio=" << prior_result.densification_ratio << '\n'
            << "ply_sha256=" << prior_result.output_ply_sha256 << '\n';
  return 0;
}
