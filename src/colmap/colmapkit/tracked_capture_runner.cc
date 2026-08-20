// A local benchmark runner for a retained ARKit capture manifest.

#include "colmap/colmapkit/colmapkit.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <sys/resource.h>

namespace {

template <typename T>
std::vector<T> ReadArray(const boost::property_tree::ptree& tree,
                         const std::string& key) {
  std::vector<T> values;
  for (const auto& item : tree.get_child(key)) {
    values.push_back(item.second.get_value<T>());
  }
  return values;
}

struct OwnedImage {
  ColmapKitTrackedImageV2 value{};
  std::string path;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4 && argc != 5) {
    std::cerr << "Usage: colmapkit_tracked_capture_runner "
                 "CAPTURE_MANIFEST OUTPUT_DIR MAX_IMAGE_PAIRS "
                 "[PAIR_GRAPH_TELEMETRY=1]\n";
    return 2;
  }
  const std::filesystem::path manifest_path(argv[1]);
  const std::filesystem::path output_dir(argv[2]);
  const uint32_t max_image_pairs = static_cast<uint32_t>(std::stoul(argv[3]));
  const bool pair_graph_telemetry = argc == 4 || std::stoul(argv[4]) != 0;
  std::filesystem::create_directories(output_dir);

  boost::property_tree::ptree manifest;
  boost::property_tree::read_json(manifest_path.string(), manifest);
  std::vector<OwnedImage> owned_images;
  uint32_t order_index = 0;
  for (const auto& item : manifest.get_child("frames")) {
    const auto& frame = item.second;
    OwnedImage image;
    image.path =
        (manifest_path.parent_path() / frame.get<std::string>("relativePath"))
            .string();
    const auto intrinsics = frame.get_child("intrinsics");
    const auto transform = ReadArray<double>(frame, "poseEvidence.transform");
    if (transform.size() != 16) {
      std::cerr << "Frame has an invalid pose transform: " << image.path
                << '\n';
      return 2;
    }
    image.value.struct_size = sizeof(image.value);
    image.value.camera_model = COLMAPKIT_CAMERA_MODEL_V2_PINHOLE;
    image.value.stable_id = frame.get<uint64_t>("captureOrder");
    image.value.order_index = order_index++;
    image.value.encoded_width = intrinsics.get<uint32_t>("imageWidth");
    image.value.encoded_height = intrinsics.get<uint32_t>("imageHeight");
    image.value.num_camera_params = 4;
    image.value.camera_params[0] = intrinsics.get<double>("focalLengthX");
    image.value.camera_params[1] = intrinsics.get<double>("focalLengthY");
    image.value.camera_params[2] = intrinsics.get<double>("principalPointX");
    image.value.camera_params[3] = intrinsics.get<double>("principalPointY");
    std::copy(
        transform.begin(), transform.end(), image.value.world_from_camera);
    image.value.tracking_state = COLMAPKIT_TRACKING_STATE_V2_NORMAL;
    image.value.inclusion_flags = COLMAPKIT_TRACKED_IMAGE_FLAG_V2_INCLUDED;
    image.value.translation_weight = 1.0;
    image.value.rotation_weight = 1.0;
    owned_images.push_back(std::move(image));
  }
  std::vector<ColmapKitTrackedImageV2> images;
  images.reserve(owned_images.size());
  for (auto& image : owned_images) {
    image.value.image_path = image.path.c_str();
    images.push_back(image.value);
  }

  const std::string database = (output_dir / "database.db").string();
  const std::string model = (output_dir / "sparse").string();
  const std::string poses = (output_dir / "refined-poses.json").string();
  const std::string evidence = (output_dir / "tracked-evidence.json").string();
  ColmapKitTrackedPoseConfigV2 config{};
  config.struct_size = sizeof(config);
  config.flags = pair_graph_telemetry
                     ? COLMAPKIT_TRACKED_POSE_FLAG_V2_PAIR_GRAPH_TELEMETRY
                     : 0;
  config.images = images.data();
  config.num_images = static_cast<uint32_t>(images.size());
  config.max_features_per_image = 4096;
  config.temporal_neighbor_count = 3;
  config.max_revisit_neighbors_per_image = 2;
  config.max_image_pairs = max_image_pairs;
  config.max_triangulation_passes = 2;
  config.max_bundle_adjustment_iterations = 40;
  config.random_seed = 0;
  config.num_threads = 4;
  config.revisit_min_translation_meters = 0.1;
  config.revisit_max_translation_meters = 2.0;
  config.revisit_max_rotation_degrees = 45.0;
  config.min_triangulation_angle_degrees = 0.1;
  config.max_reprojection_error_pixels = 4.0;
  config.max_allowed_scale_drift_ratio = 1e-9;
  config.database_path = database.c_str();
  config.output_model_path = model.c_str();
  config.refined_pose_path = poses.c_str();
  config.evidence_path = evidence.c_str();
  config.max_feature_image_size = 360;

  ColmapKitTrackedPoseResultV2 result{};
  result.struct_size = sizeof(result);
  const auto start = std::chrono::steady_clock::now();
  const auto status = ColmapKitRunTrackedPoseReconstructionV2(&config, &result);
  const double total_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  std::ofstream result_json(output_dir / "runner-result.json");
  result_json << std::setprecision(17)
              << "{\n  \"schema\": \"colmapkit.tracked-capture-runner.v1\",\n"
              << "  \"engine\": \"" << ColmapKitGetEngineBuildIdentityV2()
              << "\",\n  \"status\": " << status
              << ",\n  \"max_image_pairs\": " << max_image_pairs
              << ",\n  \"pair_graph_telemetry\": "
              << (pair_graph_telemetry ? "true" : "false")
              << ",\n  \"registered_images\": " << result.registered_images
              << ",\n  \"matched_pairs\": " << result.matched_pairs
              << ",\n  \"sparse_points\": " << result.sparse_points
              << ",\n  \"observations\": " << result.observations
              << ",\n  \"initial_mean_reprojection_error\": "
              << result.initial_mean_reprojection_error
              << ",\n  \"final_mean_reprojection_error\": "
              << result.final_mean_reprojection_error
              << ",\n  \"feature_seconds\": " << result.feature_seconds
              << ",\n  \"matching_seconds\": " << result.matching_seconds
              << ",\n  \"triangulation_seconds\": "
              << result.triangulation_seconds
              << ",\n  \"bundle_adjustment_seconds\": "
              << result.bundle_adjustment_seconds
              << ",\n  \"export_seconds\": " << result.export_seconds
              << ",\n  \"total_seconds\": " << total_seconds
              << ",\n  \"peak_resident_bytes\": " << usage.ru_maxrss << "\n}\n";
  std::cout << "status=" << status << '\n'
            << "message=" << result.message << '\n'
            << "registered_images=" << result.registered_images << '\n'
            << "matched_pairs=" << result.matched_pairs << '\n'
            << "sparse_points=" << result.sparse_points << '\n'
            << "observations=" << result.observations << '\n'
            << "final_mean_reprojection_error="
            << result.final_mean_reprojection_error << '\n'
            << "feature_seconds=" << result.feature_seconds << '\n'
            << "matching_seconds=" << result.matching_seconds << '\n'
            << "triangulation_seconds=" << result.triangulation_seconds << '\n'
            << "bundle_adjustment_seconds=" << result.bundle_adjustment_seconds
            << '\n'
            << "export_seconds=" << result.export_seconds << '\n'
            << "total_seconds=" << total_seconds << '\n'
            << "peak_resident_bytes=" << usage.ru_maxrss << '\n';
  return status == COLMAPKIT_STATUS_OK ? 0 : 1;
}
