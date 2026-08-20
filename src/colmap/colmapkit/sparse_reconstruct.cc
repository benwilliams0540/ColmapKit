// Copyright (c), ETH Zurich and UNC Chapel Hill.
// All rights reserved.

#include "colmap/colmapkit/colmapkit.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

void PrintUsage(const char* argv0) {
  std::cerr
      << "Usage:\n"
      << "  " << argv0
      << " --database_path DATABASE --image_path IMAGES --output_path SPARSE"
      << " [--sparse_text_output_path SPARSE_TEXT]"
      << " [--image_list_path LIST]"
      << " [--camera_model SIMPLE_RADIAL]"
      << " [--max_image_size N]"
      << " [--matcher sequential|exhaustive|spatial]"
      << " [--sequential_overlap N]"
      << " [--num_threads N]"
      << " [--extraction_num_threads N]"
      << " [--matching_num_threads N]"
      << " [--mapper_num_threads N]"
      << " [--mapper_random_seed N]"
      << " [--use_metal_matching 0|1]"
      << " [--use_metal_sift 0|1]"
      << " [--require_metal_matching 0|1]"
      << " [--require_metal_sift 0|1]"
      << " [--evidence_path FILE]"
      << " [--cancel_after_first_progress 0|1]\n";
}

std::unordered_map<std::string, std::string> ParseArgs(int argc, char** argv) {
  std::unordered_map<std::string, std::string> args;
  for (int i = 1; i < argc; ++i) {
    std::string key = argv[i];
    if (key == "-h" || key == "--help") {
      args["help"] = "1";
      continue;
    }
    if (key.rfind("--", 0) != 0) {
      throw std::invalid_argument("Expected option beginning with --: " + key);
    }
    key = key.substr(2);
    const size_t equals = key.find('=');
    if (equals != std::string::npos) {
      args[key.substr(0, equals)] = key.substr(equals + 1);
      continue;
    }
    if (i + 1 >= argc) {
      throw std::invalid_argument("Missing value for --" + key);
    }
    args[key] = argv[++i];
  }
  return args;
}

const char* OptionalCString(
    const std::unordered_map<std::string, std::string>& args,
    const std::string& key) {
  const auto it = args.find(key);
  return it == args.end() || it->second.empty() ? nullptr : it->second.c_str();
}

int OptionalInt(const std::unordered_map<std::string, std::string>& args,
                const std::string& key,
                int default_value) {
  const auto it = args.find(key);
  return it == args.end() ? default_value : std::atoi(it->second.c_str());
}

ColmapKitMatcherKind MatcherKind(const std::string& matcher) {
  if (matcher.empty() || matcher == "sequential") {
    return COLMAPKIT_MATCHER_SEQUENTIAL;
  }
  if (matcher == "exhaustive") {
    return COLMAPKIT_MATCHER_EXHAUSTIVE;
  }
  if (matcher == "spatial") {
    return COLMAPKIT_MATCHER_SPATIAL;
  }
  throw std::invalid_argument("Unsupported matcher: " + matcher);
}

struct ProgressState {
  std::mutex mutex;
  std::condition_variable condition;
  bool saw_progress = false;
  std::chrono::steady_clock::time_point start_time =
      std::chrono::steady_clock::now();
};

void ProgressCallback(const ColmapKitProgressEvent* event, void* user_data) {
  if (event == nullptr) {
    return;
  }
  int64_t elapsed_ms = -1;
  if (user_data != nullptr) {
    auto* state = static_cast<ProgressState*>(user_data);
    elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - state->start_time)
                     .count();
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->saw_progress = true;
    }
    state->condition.notify_all();
  }
  std::cout << "[ColmapKit progress]"
            << " elapsed_ms=" << elapsed_ms
            << " stage=" << static_cast<int>(event->stage)
            << " fraction=" << event->fraction << " current=" << event->current
            << " total=" << event->total << " message="
            << std::quoted(event->message == nullptr ? "" : event->message)
            << " detail="
            << std::quoted(event->detail == nullptr ? "" : event->detail)
            << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto args = ParseArgs(argc, argv);
    if (args.count("help") > 0) {
      PrintUsage(argv[0]);
      return EXIT_SUCCESS;
    }
    if (args.count("database_path") == 0 || args.count("image_path") == 0 ||
        args.count("output_path") == 0) {
      PrintUsage(argv[0]);
      return EXIT_FAILURE;
    }

    ColmapKitInitialize(argv[0]);

    ColmapKitSparseReconstructionConfig config = {};
    config.struct_size = sizeof(config);
    config.database_path = OptionalCString(args, "database_path");
    config.image_path = OptionalCString(args, "image_path");
    config.output_path = OptionalCString(args, "output_path");
    config.sparse_text_output_path =
        OptionalCString(args, "sparse_text_output_path");
    config.image_list_path = OptionalCString(args, "image_list_path");
    config.camera_model = OptionalCString(args, "camera_model");
    config.camera_params = OptionalCString(args, "camera_params");
    config.single_camera = OptionalInt(args, "single_camera", 1);
    config.max_image_size = OptionalInt(args, "max_image_size", -1);
    config.num_threads = OptionalInt(args, "num_threads", -1);
    config.use_gpu = OptionalInt(args, "use_gpu", 0);
    config.use_metal_sift = OptionalInt(args, "use_metal_sift", 0);
    config.use_metal_matching = OptionalInt(args, "use_metal_matching", 0);
    config.estimate_affine_shape =
        OptionalInt(args, "estimate_affine_shape", 0);
    config.domain_size_pooling = OptionalInt(args, "domain_size_pooling", 0);
    config.matcher =
        MatcherKind(args.count("matcher") == 0 ? "" : args.at("matcher"));
    config.sequential_overlap = OptionalInt(args, "sequential_overlap", 10);
    config.mapper_min_num_matches =
        OptionalInt(args, "mapper_min_num_matches", -1);
    config.mapper_min_model_size =
        OptionalInt(args, "mapper_min_model_size", -1);
    config.mapper_random_seed = OptionalInt(args, "mapper_random_seed", 0);
    config.write_sparse_text = OptionalInt(args, "write_sparse_text", 1);
    config.extraction_num_threads =
        OptionalInt(args, "extraction_num_threads", 0);
    config.matching_num_threads = OptionalInt(args, "matching_num_threads", 0);
    config.mapper_num_threads = OptionalInt(args, "mapper_num_threads", 0);
    config.require_metal_sift = OptionalInt(args, "require_metal_sift", 0);
    config.require_metal_matching =
        OptionalInt(args, "require_metal_matching", 0);
    config.evidence_path = OptionalCString(args, "evidence_path");
    ProgressState progress_state;
    config.progress_callback = ProgressCallback;
    config.progress_user_data = &progress_state;

    ColmapKitSparseReconstructionResult result = {};
    result.struct_size = sizeof(result);
    const bool cancel_after_first_progress =
        OptionalInt(args, "cancel_after_first_progress", 0) != 0;
    ColmapKitSparseReconstructionJob* job = nullptr;
    ColmapKitStatus status = ColmapKitStartSparseReconstruction(&config, &job);
    if (status != COLMAPKIT_STATUS_OK) {
      std::cerr << "Failed to start ColmapKit job: " << status << '\n';
      return EXIT_FAILURE;
    }
    if (cancel_after_first_progress) {
      std::unique_lock<std::mutex> lock(progress_state.mutex);
      progress_state.condition.wait_for(lock, std::chrono::seconds(30), [&]() {
        return progress_state.saw_progress;
      });
      ColmapKitCancelSparseReconstruction(job);
    }
    status = ColmapKitWaitSparseReconstruction(job, &result);
    ColmapKitReleaseSparseReconstructionJob(job);
    if (cancel_after_first_progress && status == COLMAPKIT_STATUS_CANCELLED) {
      std::cout << result.message << '\n';
      return EXIT_SUCCESS;
    }
    if (status != COLMAPKIT_STATUS_OK) {
      std::cerr << result.message << '\n';
      return EXIT_FAILURE;
    }

    std::cout << result.message << '\n';
    if (result.sparse_reconstruction_abi_version >= 2) {
      std::cout << "[ColmapKit evidence]"
                << " abi=" << result.sparse_reconstruction_abi_version
                << " extraction_threads="
                << result.effective_extraction_num_threads
                << " matching_threads=" << result.effective_matching_num_threads
                << " geometric_verification_threads="
                << result.effective_geometric_verification_num_threads
                << " mapper_threads=" << result.effective_mapper_num_threads
                << " metal_sift_operations=" << result.metal_sift_operations
                << " metal_matching_operations="
                << result.metal_matching_operations
                << " metal_sift_fallbacks=" << result.metal_sift_fallbacks
                << " metal_matching_fallbacks="
                << result.metal_matching_fallbacks
                << " total_seconds=" << result.total_seconds
                << " peak_resident_bytes=" << result.peak_resident_memory_bytes
                << '\n';
    }
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
