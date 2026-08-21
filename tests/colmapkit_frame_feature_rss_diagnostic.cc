#include <ColmapKit/colmapkit.h>

#include <mach/mach.h>
#include <mach/task_info.h>
#include <malloc/malloc.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Sample {
  std::string label;
  double elapsed_seconds = 0.0;
  uint64_t basic_resident = 0;
  uint64_t vm_resident = 0;
  uint64_t vm_resident_peak = 0;
  uint64_t physical_footprint = 0;
  uint64_t internal = 0;
  uint64_t reusable = 0;
  uint64_t compressed = 0;
  uint64_t region_count = 0;
  uint64_t malloc_blocks_in_use = 0;
  uint64_t malloc_size_in_use = 0;
  uint64_t malloc_max_size_in_use = 0;
  uint64_t malloc_size_allocated = 0;
};

class Sampler {
 public:
  Sampler() : start_(std::chrono::steady_clock::now()) {}

  void Add(std::string label) {
    Sample sample;
    sample.label = std::move(label);
    sample.elapsed_seconds = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - start_)
                                 .count();

    mach_task_basic_info_data_t basic{};
    mach_msg_type_number_t basic_count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(),
                  MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&basic),
                  &basic_count) == KERN_SUCCESS) {
      sample.basic_resident = basic.resident_size;
    }

    task_vm_info_data_t vm{};
    mach_msg_type_number_t vm_count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(),
                  TASK_VM_INFO,
                  reinterpret_cast<task_info_t>(&vm),
                  &vm_count) == KERN_SUCCESS) {
      sample.vm_resident = vm.resident_size;
      sample.vm_resident_peak = vm.resident_size_peak;
      sample.physical_footprint = vm.phys_footprint;
      sample.internal = vm.internal;
      sample.reusable = vm.reusable;
      sample.compressed = vm.compressed;
      sample.region_count = static_cast<uint64_t>(vm.region_count);
    }

    malloc_statistics_t malloc_stats{};
    malloc_zone_statistics(nullptr, &malloc_stats);
    sample.malloc_blocks_in_use = malloc_stats.blocks_in_use;
    sample.malloc_size_in_use = malloc_stats.size_in_use;
    sample.malloc_max_size_in_use = malloc_stats.max_size_in_use;
    sample.malloc_size_allocated = malloc_stats.size_allocated;

    std::lock_guard<std::mutex> lock(mutex_);
    samples_.push_back(std::move(sample));
  }

  std::vector<Sample> Samples() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return samples_;
  }

 private:
  std::chrono::steady_clock::time_point start_;
  mutable std::mutex mutex_;
  std::vector<Sample> samples_;
};

std::string StageName(const uint32_t stage) {
  switch (stage) {
    case COLMAPKIT_FRAME_FEATURE_PROGRESS_V1_QUEUED:
      return "queued";
    case COLMAPKIT_FRAME_FEATURE_PROGRESS_V1_VALIDATING:
      return "validating";
    case COLMAPKIT_FRAME_FEATURE_PROGRESS_V1_DECODING:
      return "decoding";
    case COLMAPKIT_FRAME_FEATURE_PROGRESS_V1_PREPROCESSING:
      return "preprocessing";
    case COLMAPKIT_FRAME_FEATURE_PROGRESS_V1_EXTRACTING:
      return "extracting";
    case COLMAPKIT_FRAME_FEATURE_PROGRESS_V1_SERIALIZING:
      return "serializing";
    case COLMAPKIT_FRAME_FEATURE_PROGRESS_V1_FINISHED:
      return "finished";
    case COLMAPKIT_FRAME_FEATURE_PROGRESS_V1_CANCELLED:
      return "cancelled";
    case COLMAPKIT_FRAME_FEATURE_PROGRESS_V1_FAILED:
      return "failed";
    default:
      return "unknown";
  }
}

struct ProgressContext {
  Sampler* sampler = nullptr;
  int run = 0;
};

void ProgressCallback(const ColmapKitFrameFeatureProgressEventV1* event,
                      void* user_data) {
  if (event == nullptr || user_data == nullptr) return;
  auto* context = static_cast<ProgressContext*>(user_data);
  context->sampler->Add("run" + std::to_string(context->run) + "-progress-" +
                        StageName(event->stage));
}

ColmapKitFrameFeatureExtractorConfigV1 Config() {
  ColmapKitFrameFeatureExtractorConfigV1 config{};
  config.struct_size = sizeof(config);
  config.abi_version = COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1;
  config.requested_backend = COLMAPKIT_FRAME_FEATURE_BACKEND_V1_CPU;
  config.worker_count = 1;
  config.max_encoded_image_bytes = 16ULL * 1024ULL * 1024ULL;
  config.memory_admission_budget_bytes = 320ULL * 1024ULL * 1024ULL;
  config.max_image_size = 1024;
  config.max_num_features = 4096;
  config.first_octave = -1;
  config.num_octaves = 4;
  config.octave_resolution = 3;
  config.max_num_orientations = 2;
  config.upright = 0;
  config.normalization = COLMAPKIT_SIFT_NORMALIZATION_V1_L1_ROOT;
  config.peak_threshold = 0.006666666666666667;
  config.edge_threshold = 10.0;
  return config;
}

ColmapKitFrameFeatureErrorV1 Error() {
  ColmapKitFrameFeatureErrorV1 error{};
  error.struct_size = sizeof(error);
  error.abi_version = COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1;
  return error;
}

ColmapKitFrameFeatureResultV1 Result() {
  ColmapKitFrameFeatureResultV1 result{};
  result.struct_size = sizeof(result);
  result.abi_version = COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1;
  return result;
}

void PrintEscaped(const std::string& value) {
  for (const char character : value) {
    if (character == '\\' || character == '"') std::cout << '\\';
    std::cout << character;
  }
}

void PrintSample(const Sample& sample, const bool last) {
  std::cout << "    {\"label\":\"";
  PrintEscaped(sample.label);
  std::cout << "\",\"elapsedSeconds\":" << std::fixed
            << std::setprecision(6) << sample.elapsed_seconds
            << ",\"basicResident\":" << sample.basic_resident
            << ",\"vmResident\":" << sample.vm_resident
            << ",\"vmResidentPeak\":" << sample.vm_resident_peak
            << ",\"physicalFootprint\":" << sample.physical_footprint
            << ",\"internal\":" << sample.internal
            << ",\"reusable\":" << sample.reusable
            << ",\"compressed\":" << sample.compressed
            << ",\"regionCount\":" << sample.region_count
            << ",\"mallocBlocksInUse\":" << sample.malloc_blocks_in_use
            << ",\"mallocSizeInUse\":" << sample.malloc_size_in_use
            << ",\"mallocMaxSizeInUse\":"
            << sample.malloc_max_size_in_use
            << ",\"mallocSizeAllocated\":" << sample.malloc_size_allocated
            << "}" << (last ? "\n" : ",\n");
}

struct RunReceipt {
  uint32_t wait_status = 0;
  uint32_t result_status = 0;
  uint32_t actual_backend = 0;
  uint32_t no_fallback = 0;
  uint32_t workers = 0;
  uint64_t features = 0;
  uint64_t descriptor_bytes = 0;
  uint64_t admitted = 0;
  uint64_t peak_resident = 0;
  double total_seconds = 0.0;
  std::string profile;
  std::string artifact;
  std::string payload;
};

RunReceipt Run(const int run,
               const std::vector<uint8_t>& bytes,
               const std::string& output_path,
               Sampler* sampler) {
  std::remove(output_path.c_str());
  auto config = Config();
  auto error = Error();
  ColmapKitFrameFeatureExtractorV1* extractor = nullptr;
  auto status = ColmapKitCreateFrameFeatureExtractorV1(
      &config, &extractor, &error);
  if (status != COLMAPKIT_STATUS_OK || extractor == nullptr) {
    throw std::runtime_error(std::string("create failed: ") + error.message);
  }
  sampler->Add("run" + std::to_string(run) + "-extractor-created");

  ProgressContext progress{sampler, run};
  ColmapKitFrameFeatureInputV1 input{};
  input.struct_size = sizeof(input);
  input.abi_version = COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1;
  input.stable_frame_id = 10;
  input.frame_revision = 1;
  input.encoded_image_bytes = bytes.data();
  input.encoded_image_size = bytes.size();
  input.expected_image_sha256 =
      "2b22ced55baccd6493e49d8b5086367c34c952ec63399ea5e96c3dd52ace1767";
  input.metadata.struct_size = sizeof(input.metadata);
  input.metadata.abi_version = COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1;
  input.metadata.image_format = COLMAPKIT_FRAME_IMAGE_FORMAT_V1_JPEG;
  input.metadata.encoded_width = 1024;
  input.metadata.encoded_height = 768;
  input.metadata.orientation = COLMAPKIT_FRAME_ORIENTATION_V1_UP;
  input.metadata.camera_model = COLMAPKIT_CAMERA_MODEL_V2_PINHOLE;
  input.metadata.num_camera_params = 4;
  input.metadata.camera_params[0] = 900.0;
  input.metadata.camera_params[1] = 900.0;
  input.metadata.camera_params[2] = 512.0;
  input.metadata.camera_params[3] = 384.0;
  input.output_artifact_path = output_path.c_str();
  input.progress_callback = ProgressCallback;
  input.progress_user_data = &progress;

  ColmapKitFrameFeatureJobV1* job = nullptr;
  status = ColmapKitStartFrameFeatureExtractionV1(
      extractor, &input, &job, &error);
  if (status != COLMAPKIT_STATUS_OK || job == nullptr) {
    ColmapKitReleaseFrameFeatureExtractorV1(extractor);
    throw std::runtime_error(std::string("start failed: ") + error.message);
  }
  sampler->Add("run" + std::to_string(run) + "-job-started");

  auto result = Result();
  status = ColmapKitWaitFrameFeatureExtractionV1(job, &result);
  sampler->Add("run" + std::to_string(run) + "-wait-returned");
  if (status != COLMAPKIT_STATUS_OK || result.status != COLMAPKIT_STATUS_OK) {
    ColmapKitReleaseFrameFeatureJobV1(job);
    ColmapKitReleaseFrameFeatureExtractorV1(extractor);
    throw std::runtime_error(std::string("wait failed: ") + result.message);
  }

  ColmapKitFrameFeatureArtifactExpectationV1 expectation{};
  expectation.struct_size = sizeof(expectation);
  expectation.abi_version = COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1;
  expectation.stable_frame_id = 10;
  expectation.frame_revision = 1;
  expectation.expected_image_sha256 = input.expected_image_sha256;
  expectation.expected_metadata_sha256 = result.metadata_sha256;
  expectation.artifact_path = output_path.c_str();
  auto validation = Result();
  const auto validation_status = ColmapKitValidateFrameFeatureArtifactV1(
      extractor, &expectation, &validation);
  sampler->Add("run" + std::to_string(run) + "-validated");
  if (validation_status != COLMAPKIT_STATUS_OK ||
      validation.status != COLMAPKIT_STATUS_OK ||
      validation.feature_count != result.feature_count) {
    ColmapKitReleaseFrameFeatureJobV1(job);
    ColmapKitReleaseFrameFeatureExtractorV1(extractor);
    throw std::runtime_error(std::string("validation failed: ") +
                             validation.message);
  }

  RunReceipt receipt;
  receipt.wait_status = status;
  receipt.result_status = result.status;
  receipt.actual_backend = result.actual_backend;
  receipt.no_fallback = result.no_fallback_satisfied;
  receipt.workers = result.effective_worker_count;
  receipt.features = result.feature_count;
  receipt.descriptor_bytes = result.descriptor_bytes;
  receipt.admitted = result.admitted_memory_bytes;
  receipt.peak_resident = result.peak_resident_memory_bytes;
  receipt.total_seconds = result.total_seconds;
  receipt.profile = result.profile_sha256;
  receipt.artifact = result.artifact_sha256;
  receipt.payload = result.payload_sha256;

  ColmapKitReleaseFrameFeatureJobV1(job);
  sampler->Add("run" + std::to_string(run) + "-job-released");
  ColmapKitReleaseFrameFeatureExtractorV1(extractor);
  sampler->Add("run" + std::to_string(run) + "-extractor-released");
  return receipt;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: diagnostic <fixture.jpg> <evidence-directory>\n";
    return 64;
  }
  try {
    Sampler sampler;
    sampler.Add("process-start");
    std::ifstream stream(argv[1], std::ios::binary);
    std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(stream), {});
    if (bytes.empty()) throw std::runtime_error("fixture read failed");
    sampler.Add("encoded-loaded");

    const std::string root(argv[2]);
    const auto first = Run(1, bytes, root + "/run1.ckfeatures", &sampler);
    const auto second = Run(2, bytes, root + "/run2.ckfeatures", &sampler);
    std::vector<uint8_t>().swap(bytes);
    sampler.Add("encoded-released");

    const auto samples = sampler.Samples();
    std::cout << "{\n"
              << "  \"schemaVersion\":1,\n"
              << "  \"releaseVersion\":\""
              << ColmapKitGetReleaseVersionV2() << "\",\n"
              << "  \"engineIdentity\":\"";
    PrintEscaped(ColmapKitGetEngineBuildIdentityV2());
    std::cout << "\",\n  \"runs\":[\n";
    const RunReceipt receipts[] = {first, second};
    for (int index = 0; index < 2; ++index) {
      const auto& receipt = receipts[index];
      std::cout << "    {\"run\":" << (index + 1)
                << ",\"waitStatus\":" << receipt.wait_status
                << ",\"resultStatus\":" << receipt.result_status
                << ",\"actualBackend\":" << receipt.actual_backend
                << ",\"noFallback\":" << receipt.no_fallback
                << ",\"workers\":" << receipt.workers
                << ",\"features\":" << receipt.features
                << ",\"descriptorBytes\":" << receipt.descriptor_bytes
                << ",\"admitted\":" << receipt.admitted
                << ",\"peakResident\":" << receipt.peak_resident
                << ",\"totalSeconds\":" << std::fixed
                << std::setprecision(6) << receipt.total_seconds
                << ",\"profile\":\"" << receipt.profile
                << "\",\"artifact\":\"" << receipt.artifact
                << "\",\"payload\":\"" << receipt.payload << "\"}"
                << (index == 1 ? "\n" : ",\n");
    }
    std::cout << "  ],\n  \"samples\":[\n";
    for (size_t index = 0; index < samples.size(); ++index) {
      PrintSample(samples[index], index + 1 == samples.size());
    }
    std::cout << "  ]\n}\n";

    if (first.features != 4096 || second.features != 4096 ||
        first.actual_backend != COLMAPKIT_FRAME_FEATURE_BACKEND_V1_CPU ||
        second.actual_backend != COLMAPKIT_FRAME_FEATURE_BACKEND_V1_CPU ||
        first.no_fallback != 1 || second.no_fallback != 1 ||
        first.workers != 1 || second.workers != 1 ||
        first.admitted != 325534076 || second.admitted != 325534076 ||
        first.artifact != second.artifact || first.payload != second.payload) {
      return 3;
    }
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << exception.what() << "\n";
    return 1;
  }
}
