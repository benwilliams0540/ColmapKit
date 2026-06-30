// SiftMetal.mm - Metal-accelerated SIFT feature extraction.
// Objective-C++ port of SIFTMetal Swift library by Luke Van In.

#include "SiftMetal.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

// Shared C headers for Metal shader parameter structs.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <numeric>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "include/ConvolutionSeries.h"
#include "include/NearestNeighbor.h"
#include "include/SIFTDescriptor.h"
#include "include/SIFTExtrema.h"
#include "include/SIFTInterpolate.h"
#include "include/SIFTOrientation.h"
#include <mach-o/dyld.h>

// Build-tree path to compiled metallib is set by CMake and used as one of
// several runtime candidates. Installed builds are resolved relative to the
// executable or application bundle below.
#ifndef SIFT_METAL_BUILD_METALLIB_PATH
#define SIFT_METAL_BUILD_METALLIB_PATH ""
#endif

namespace sift_metal {

static constexpr int kMinExtremaCapacity = 4096;
static constexpr int kMaxExtremaCapacity = 1 << 20;
static constexpr int kMaxExtremaThreadgroupSize = 1024;
static constexpr int kMinKeypointCapacity = 4096;
static constexpr int kMinDescriptorCapacity = 8192;
static constexpr int kMaxDescriptorCapacity = 1 << 16;
static constexpr int64_t kExtremaFeatureMultiplier = 8;
static constexpr int64_t kExtremaGridCellsPerCandidate = 256;

static std::string NSStringToString(NSString* string) {
  if (string == nil) return std::string();
  const char* utf8 = [string UTF8String];
  return utf8 == nullptr ? std::string() : std::string(utf8);
}

static std::string NSErrorToString(NSError* error) {
  if (error == nil) return std::string();
  std::ostringstream stream;
  stream << NSStringToString([error domain]) << "(" << [error code] << ")";
  const std::string description = NSStringToString([error localizedDescription]);
  if (!description.empty()) {
    stream << ": " << description;
  }
  const std::string failure_reason = NSStringToString([error localizedFailureReason]);
  if (!failure_reason.empty()) {
    stream << " reason=" << failure_reason;
  }
  const std::string recovery_suggestion = NSStringToString([error localizedRecoverySuggestion]);
  if (!recovery_suggestion.empty()) {
    stream << " suggestion=" << recovery_suggestion;
  }
  return stream.str();
}

static const char* StatusSeverityName(StatusSeverity severity) {
  switch (severity) {
    case StatusSeverity::kInfo:
      return "info";
    case StatusSeverity::kWarning:
      return "warning";
    case StatusSeverity::kError:
      return "error";
  }
  return "unknown";
}

static const char* CommandBufferStatusName(MTLCommandBufferStatus status) {
  switch (status) {
    case MTLCommandBufferStatusNotEnqueued:
      return "not_enqueued";
    case MTLCommandBufferStatusEnqueued:
      return "enqueued";
    case MTLCommandBufferStatusCommitted:
      return "committed";
    case MTLCommandBufferStatusScheduled:
      return "scheduled";
    case MTLCommandBufferStatusCompleted:
      return "completed";
    case MTLCommandBufferStatusError:
      return "error";
  }
  return "unknown";
}

static bool FileExists(const std::string& path) {
  if (path.empty()) return false;
  return [[NSFileManager defaultManager]
      fileExistsAtPath:[NSString stringWithUTF8String:path.c_str()]];
}

static void AppendPath(std::vector<std::string>* paths, NSString* path) {
  if (path != nil) {
    paths->push_back(NSStringToString(path));
  }
}

static std::vector<std::string> MetallibCandidatePaths() {
  std::vector<std::string> paths;

  if (const char* path = std::getenv("COLMAP_SIFT_METAL_METALLIB")) {
    paths.emplace_back(path);
  }
  if (const char* path = std::getenv("COLMAP_SIFT_METAL_METALLIB_PATH")) {
    paths.emplace_back(path);
  }

  if (NSBundle* bundle = [NSBundle mainBundle]) {
    if (NSString* resourcePath = [bundle resourcePath]) {
      AppendPath(&paths, [resourcePath stringByAppendingPathComponent:@"sift.metallib"]);
      AppendPath(&paths,
                 [resourcePath stringByAppendingPathComponent:@"../Resources/sift.metallib"]);
      AppendPath(
          &paths,
          [resourcePath stringByAppendingPathComponent:@"../share/colmap/metal/sift.metallib"]);
    }
  }

  uint32_t executable_path_size = 0;
  _NSGetExecutablePath(nullptr, &executable_path_size);
  std::vector<char> executable_path(executable_path_size + 1, '\0');
  if (_NSGetExecutablePath(executable_path.data(), &executable_path_size) == 0) {
    NSString* executablePath = [[NSFileManager defaultManager]
        stringWithFileSystemRepresentation:executable_path.data()
                                    length:std::strlen(executable_path.data())];
    NSString* executableDir = [executablePath stringByDeletingLastPathComponent];
    AppendPath(&paths, [executableDir stringByAppendingPathComponent:@"sift.metallib"]);
    AppendPath(
        &paths,
        [executableDir stringByAppendingPathComponent:@"../share/colmap/metal/sift.metallib"]);
    AppendPath(&paths, [executableDir stringByAppendingPathComponent:@"../lib/sift.metallib"]);
  }

  paths.emplace_back(SIFT_METAL_BUILD_METALLIB_PATH);

  paths.erase(std::remove(paths.begin(), paths.end(), std::string()), paths.end());
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  return paths;
}

// ---------------------------------------------------------------------------
// Helper: compute 1D Gaussian kernel weights.
// ---------------------------------------------------------------------------
static std::vector<float> GaussianWeights(float sigma) {
  int radius = static_cast<int>(std::ceil(4.0f * sigma));
  int size = radius * 2 + 1;
  std::vector<float> weights(size);
  float sum = 0.0f;
  float ss = sigma * sigma;
  for (int k = -radius; k <= radius; ++k) {
    float w = std::exp(-0.5f * (float(k * k) / ss));
    weights[k + radius] = w;
    sum += w;
  }
  for (auto& w : weights) w /= sum;
  return weights;
}

static int64_t ExtremaSearchSpaceSize(int w, int h, int num_scales) {
  if (w <= 2 || h <= 2 || num_scales <= 0) return 0;
  return static_cast<int64_t>(w - 2) * static_cast<int64_t>(h - 2) *
         static_cast<int64_t>(num_scales);
}

static uint32_t ComputeExtremaCapacity(const Options& options, int64_t search_space_size) {
  if (search_space_size <= 0) return 0;

  const int64_t requested_features =
      options.max_num_features > 0 ? options.max_num_features : kMinDescriptorCapacity;
  const int64_t option_scaled_capacity =
      std::max<int64_t>(kMinExtremaCapacity,
                        requested_features * kExtremaFeatureMultiplier);
  const int64_t image_scaled_capacity =
      std::max<int64_t>(kMinExtremaCapacity,
                        search_space_size / kExtremaGridCellsPerCandidate);
  const int64_t capacity = std::min<int64_t>(
      {search_space_size,
       kMaxExtremaCapacity,
       std::max(option_scaled_capacity, image_scaled_capacity)});
  return static_cast<uint32_t>(
      std::min<int64_t>(capacity, std::numeric_limits<uint32_t>::max()));
}

static uint32_t ComputeKeypointCapacity(const uint32_t extrema_capacity) {
  return static_cast<uint32_t>(
      std::max<int64_t>(kMinKeypointCapacity, extrema_capacity));
}

static uint32_t ComputeDescriptorCapacity(const Options& options,
                                          const uint32_t keypoint_capacity) {
  const int64_t max_orientations =
      options.upright ? 1 : std::max<int64_t>(1, options.max_num_orientations);
  const int64_t capacity =
      std::max<int64_t>(kMinDescriptorCapacity, keypoint_capacity * max_orientations);
  return static_cast<uint32_t>(
      std::min<int64_t>(capacity, kMaxDescriptorCapacity));
}

// ---------------------------------------------------------------------------
// Octave: manages textures and pipelines for one octave of the pyramid.
// ---------------------------------------------------------------------------
struct Octave {
  int o;                      // octave index
  float delta;                // sampling distance
  int width, height;          // dimensions at this octave
  int num_scales;             // scales per octave (typically 3)
  std::vector<float> sigmas;  // sigma values for each gaussian

  id<MTLTexture> gaussianTextures;    // 2DArray [num_scales+3]
  id<MTLTexture> differenceTextures;  // 2DArray [num_scales+2]
  id<MTLTexture> gradientTextures;    // 2DArray, rg32Float

  // Buffers for extrema detection
  id<MTLBuffer> extremaOutputBuffer;
  id<MTLBuffer> extremaIndexBuffer;
  id<MTLBuffer> extremaParamsBuffer;
  uint32_t extrema_capacity = 0;
  uint32_t extrema_linear_count = 0;
  uint32_t keypoint_capacity = 0;
  uint32_t descriptor_capacity = 0;

  // Buffers for interpolation
  id<MTLBuffer> interpolateInputBuffer;
  id<MTLBuffer> interpolateOutputBuffer;
  id<MTLBuffer> interpolateParamsBuffer;

  // Buffers for orientation
  id<MTLBuffer> orientationInputBuffer;
  id<MTLBuffer> orientationOutputBuffer;
  id<MTLBuffer> orientationParamsBuffer;

  // Buffers for descriptors
  id<MTLBuffer> descriptorInputBuffer;
  id<MTLBuffer> descriptorOutputBuffer;
  id<MTLBuffer> descriptorParamsBuffer;

  // Convolution kernel weights buffers for Gaussian series blur
  struct ConvPair {
    id<MTLBuffer> paramsX;
    id<MTLBuffer> paramsY;
  };
  std::vector<ConvPair> convPairs;
  id<MTLTexture> convWorkTexture;  // private storage 2DArray[1]
};

// ---------------------------------------------------------------------------
// SiftMetalExtractorImpl
// ---------------------------------------------------------------------------
class SiftMetalExtractorImpl {
 public:
  bool Init(const Options& opts, int max_w, int max_h);
  bool Extract(const uint8_t* data, int w, int h, ExtractResult* result);
  const StatusReport& LastStatus() const { return last_status_; }

 private:
  bool EnsureImageLayout(int w, int h);
  bool SetupOctaves(int w, int h);
  bool SetupOctave(Octave& oct,
                   int o,
                   float delta,
                   int w,
                   int h,
                   int num_scales,
                   const std::vector<float>& sigmas);
  void ResetStatus(const std::string& stage);
  void AddStatusMessage(StatusSeverity severity,
                        const std::string& stage,
                        const std::string& message,
                        const std::string& detail = std::string());
  void RecordCapacityDrop(const std::string& stage,
                          const std::string& resource,
                          int64_t dropped,
                          int64_t capacity,
                          int64_t observed);
  bool WaitForCommandBuffer(id<MTLCommandBuffer> cb, const std::string& stage);

  // Pipeline encoding helpers
  void EncodeGrayscaleUpload(id<MTLCommandBuffer> cb, int w, int h);
  void EncodeSeedTexture(id<MTLCommandBuffer> cb);
  bool EncodeOctave(id<MTLCommandBuffer> cb,
                    Octave& oct,
                    id<MTLTexture> inputTexture,
                    bool inputIs2D);
  void EncodeGaussianSeries(id<MTLCommandBuffer> cb, Octave& oct);
  void EncodeDifferences(id<MTLCommandBuffer> cb, Octave& oct);
  void EncodeGradients(id<MTLCommandBuffer> cb, Octave& oct);
  void EncodeExtrema(id<MTLCommandBuffer> cb,
                     Octave& oct,
                     uint32_t output_capacity,
                     uint32_t linear_start,
                     uint32_t linear_end);

  // Per-octave extraction
  int ReadExtremaCount(Octave& oct);
  bool CountExtremaInRange(Octave& oct, uint32_t linear_end, uint32_t* count);
  int CompactExtremaDeterministically(Octave& oct, uint32_t target_count);
  void SortExtrema(Octave& oct, int count);
  int InterpolateKeypoints(Octave& oct, int extrema_count);
  bool ComputeOrientations(Octave& oct,
                           const std::vector<Keypoint>& keypoints,
                           std::vector<std::pair<int, float>>& oriented);
  bool ComputeDescriptors(Octave& oct,
                          const std::vector<Keypoint>& keypoints,
                          const std::vector<std::pair<int, float>>& oriented,
                          ExtractResult* result);

  // Metal objects
  id<MTLDevice> device_;
  id<MTLCommandQueue> commandQueue_;
  id<MTLLibrary> library_;

  // Compute pipelines
  id<MTLComputePipelineState> bilinearUpScalePipeline_;
  id<MTLComputePipelineState> nearestNeighborDownScalePipeline_;
  id<MTLComputePipelineState> convolutionXPipeline_;
  id<MTLComputePipelineState> convolutionYPipeline_;
  id<MTLComputePipelineState> convolutionSeriesXPipeline_;
  id<MTLComputePipelineState> convolutionSeriesYPipeline_;
  id<MTLComputePipelineState> subtractPipeline_;
  id<MTLComputePipelineState> siftGradientPipeline_;
  id<MTLComputePipelineState> siftExtremaListPipeline_;
  id<MTLComputePipelineState> siftInterpolatePipeline_;
  id<MTLComputePipelineState> siftOrientationPipeline_;
  id<MTLComputePipelineState> siftDescriptorsPipeline_;

  // Seed textures
  id<MTLTexture> luminosityTexture_;    // R32Float, input size
  id<MTLTexture> scaledTexture_;        // R32Float, seed size (2x)
  id<MTLTexture> seedTexture_;          // R32Float, seed size (2x)
  id<MTLTexture> seedConvWorkTexture_;  // R32Float, seed size, private

  // Seed Gaussian blur convolution buffers
  id<MTLBuffer> seedConvWeightsBuffer_;
  id<MTLBuffer> seedConvParamsBuffer_;

  // Octaves
  std::vector<Octave> octaves_;

  // Options
  Options options_;
  float sigma_min_ = 0.8f;
  float delta_min_ = 0.5f;
  float sigma_input_ = 0.5f;
  int input_w_ = 0, input_h_ = 0;
  int seed_w_ = 0, seed_h_ = 0;

  // Upload buffer
  id<MTLBuffer> uploadBuffer_;

  StatusReport last_status_;
};

// ---------------------------------------------------------------------------
// Pipeline creation helper
// ---------------------------------------------------------------------------
static id<MTLComputePipelineState> MakePipeline(id<MTLDevice> device,
                                                id<MTLLibrary> library,
                                                const char* name,
                                                std::string* error_detail) {
  if (error_detail != nullptr) error_detail->clear();
  NSString* nsName = [NSString stringWithUTF8String:name];
  id<MTLFunction> func = [library newFunctionWithName:nsName];
  if (!func) {
    if (error_detail != nullptr) {
      *error_detail = "Metal function not found in loaded library";
    }
    return nil;
  }
  NSError* error = nil;
  id<MTLComputePipelineState> ps = [device newComputePipelineStateWithFunction:func error:&error];
  if (!ps && error_detail != nullptr) {
    *error_detail = NSErrorToString(error);
    if (error_detail->empty()) {
      *error_detail = "Metal did not return a pipeline state or NSError";
    }
  }
  return ps;
}

static id<MTLTexture> MakeTexture2D(
    id<MTLDevice> device, int w, int h, MTLPixelFormat fmt, MTLStorageMode storage) {
  MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
                                                                                  width:w
                                                                                 height:h
                                                                              mipmapped:NO];
  desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
  desc.storageMode = storage;
  return [device newTextureWithDescriptor:desc];
}

static id<MTLTexture> MakeTexture2DArray(
    id<MTLDevice> device, int w, int h, int arrayLen, MTLPixelFormat fmt, MTLStorageMode storage) {
  MTLTextureDescriptor* desc = [[MTLTextureDescriptor alloc] init];
  desc.textureType = MTLTextureType2DArray;
  desc.pixelFormat = fmt;
  desc.width = w;
  desc.height = h;
  desc.arrayLength = arrayLen;
  desc.mipmapLevelCount = 1;
  desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
  desc.storageMode = storage;
  return [device newTextureWithDescriptor:desc];
}

void SiftMetalExtractorImpl::ResetStatus(const std::string& stage) {
  last_status_ = StatusReport();
  last_status_.stage = stage;
  if (device_) {
    last_status_.device_name = NSStringToString([device_ name]);
  }
}

void SiftMetalExtractorImpl::AddStatusMessage(StatusSeverity severity,
                                              const std::string& stage,
                                              const std::string& message,
                                              const std::string& detail) {
  StatusMessage status_message;
  status_message.severity = severity;
  status_message.stage = stage;
  status_message.message = message;
  status_message.detail = detail;
  last_status_.messages.push_back(status_message);

  if (severity == StatusSeverity::kError) {
    last_status_.ok = false;
    last_status_.stage = stage;
    last_status_.message = message;
  } else if (last_status_.message.empty() && severity == StatusSeverity::kWarning) {
    last_status_.message = "Completed with warnings";
  }

  if (severity != StatusSeverity::kInfo) {
    NSLog(@"SiftMetal [%s] %s: %s%s%s",
          StatusSeverityName(severity),
          stage.c_str(),
          message.c_str(),
          detail.empty() ? "" : " - ",
          detail.c_str());
  }
}

void SiftMetalExtractorImpl::RecordCapacityDrop(const std::string& stage,
                                                const std::string& resource,
                                                int64_t dropped,
                                                int64_t capacity,
                                                int64_t observed) {
  if (dropped <= 0) return;

  if (resource == "extrema") {
    last_status_.capacity.dropped_extrema += dropped;
  } else if (resource == "keypoints") {
    last_status_.capacity.dropped_keypoints += dropped;
  } else if (resource == "orientations") {
    last_status_.capacity.dropped_orientations += dropped;
  } else if (resource == "descriptors") {
    last_status_.capacity.dropped_descriptors += dropped;
  } else if (resource == "features") {
    last_status_.capacity.dropped_features += dropped;
  }

  std::ostringstream detail;
  detail << "observed=" << observed << ", capacity=" << capacity << ", dropped=" << dropped;
  AddStatusMessage(StatusSeverity::kWarning,
                   stage,
                   "Dropped " + resource + " due to bounded capacity",
                   detail.str());
}

bool SiftMetalExtractorImpl::WaitForCommandBuffer(id<MTLCommandBuffer> cb,
                                                  const std::string& stage) {
  if (!cb) {
    AddStatusMessage(StatusSeverity::kError, stage, "Failed to create Metal command buffer");
    return false;
  }

  [cb commit];
  [cb waitUntilCompleted];

  if (cb.status == MTLCommandBufferStatusCompleted) {
    return true;
  }

  std::ostringstream detail;
  detail << "status=" << CommandBufferStatusName(cb.status);
  const std::string error = NSErrorToString(cb.error);
  if (!error.empty()) {
    detail << ", error=" << error;
  }

  AddStatusMessage(StatusSeverity::kError,
                   stage,
                   "Metal command buffer did not complete successfully",
                   detail.str());
  return false;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
bool SiftMetalExtractorImpl::Init(const Options& opts, int max_w, int max_h) {
  options_ = opts;

  device_ = nil;
  commandQueue_ = nil;
  library_ = nil;
  bilinearUpScalePipeline_ = nil;
  nearestNeighborDownScalePipeline_ = nil;
  convolutionXPipeline_ = nil;
  convolutionYPipeline_ = nil;
  convolutionSeriesXPipeline_ = nil;
  convolutionSeriesYPipeline_ = nil;
  subtractPipeline_ = nil;
  siftGradientPipeline_ = nil;
  siftExtremaListPipeline_ = nil;
  siftInterpolatePipeline_ = nil;
  siftOrientationPipeline_ = nil;
  siftDescriptorsPipeline_ = nil;
  luminosityTexture_ = nil;
  scaledTexture_ = nil;
  seedTexture_ = nil;
  seedConvWorkTexture_ = nil;
  seedConvWeightsBuffer_ = nil;
  seedConvParamsBuffer_ = nil;
  uploadBuffer_ = nil;
  octaves_.clear();
  input_w_ = 0;
  input_h_ = 0;
  seed_w_ = 0;
  seed_h_ = 0;

  ResetStatus("init");
  last_status_.requested_max_image_width = max_w;
  last_status_.requested_max_image_height = max_h;

  // Get the default Metal device.
  device_ = MTLCreateSystemDefaultDevice();
  if (!device_) {
    AddStatusMessage(StatusSeverity::kError, "init.device", "Metal default device is unavailable");
    return false;
  }
  last_status_.device_name = NSStringToString([device_ name]);

  commandQueue_ = [device_ newCommandQueue];
  if (!commandQueue_) {
    AddStatusMessage(StatusSeverity::kError,
                     "init.command_queue",
                     "Failed to create Metal command queue",
                     "device=" + last_status_.device_name);
    return false;
  }

  // Load the pre-compiled metal library.
  std::vector<std::string> candidate_paths = MetallibCandidatePaths();
  for (const std::string& path : candidate_paths) {
    MetallibPathAttempt attempt;
    attempt.path = path;
    attempt.exists = FileExists(path);
    if (!attempt.exists) {
      last_status_.metallib_path_attempts.push_back(std::move(attempt));
      continue;
    }

    NSError* error = nil;
    NSURL* libURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    library_ = [device_ newLibraryWithURL:libURL error:&error];
    attempt.loaded = library_ != nil;
    attempt.error = NSErrorToString(error);
    last_status_.metallib_path_attempts.push_back(std::move(attempt));
    if (library_) {
      break;
    }
  }
  if (!library_) {
    // Fallback: try default library.
    library_ = [device_ newDefaultLibrary];
    AddStatusMessage(
        library_ ? StatusSeverity::kInfo : StatusSeverity::kWarning,
        "init.library.default",
        library_ ? "Loaded default Metal library" : "Default Metal library was unavailable");
  }
  if (!library_) {
    std::ostringstream detail;
    detail << "device=" << last_status_.device_name
           << ", path_attempts=" << last_status_.metallib_path_attempts.size();
    for (const MetallibPathAttempt& attempt : last_status_.metallib_path_attempts) {
      detail << " [" << attempt.path << " exists=" << (attempt.exists ? "true" : "false");
      if (!attempt.error.empty()) {
        detail << " error=" << attempt.error;
      }
      detail << "]";
    }
    AddStatusMessage(
        StatusSeverity::kError, "init.library", "Failed to load Metal library", detail.str());
    return false;
  }

  // Create all compute pipelines.
  bool pipeline_ok = true;
  const auto make_pipeline = [&](const char* name) -> id<MTLComputePipelineState> {
    std::string error_detail;
    id<MTLComputePipelineState> pipeline = MakePipeline(device_, library_, name, &error_detail);
    if (pipeline == nil) {
      pipeline_ok = false;
      AddStatusMessage(StatusSeverity::kError,
                       std::string("init.pipeline.") + name,
                       "Failed to create Metal compute pipeline",
                       error_detail);
    }
    return pipeline;
  };
  bilinearUpScalePipeline_ = make_pipeline("bilinearUpScale");
  nearestNeighborDownScalePipeline_ = make_pipeline("nearestNeighborDownScale");
  convolutionXPipeline_ = make_pipeline("convolutionX");
  convolutionYPipeline_ = make_pipeline("convolutionY");
  convolutionSeriesXPipeline_ = make_pipeline("convolutionSeriesX");
  convolutionSeriesYPipeline_ = make_pipeline("convolutionSeriesY");
  subtractPipeline_ = make_pipeline("subtract");
  siftGradientPipeline_ = make_pipeline("siftGradient");
  siftExtremaListPipeline_ = make_pipeline("siftExtremaList");
  siftInterpolatePipeline_ = make_pipeline("siftInterpolate");
  siftOrientationPipeline_ = make_pipeline("siftOrientation");
  siftDescriptorsPipeline_ = make_pipeline("siftDescriptors");

  if (!pipeline_ok) {
    return false;
  }

  // Determine seed size based on first_octave.
  if (opts.first_octave == -1) {
    delta_min_ = 0.5f;
  } else {
    delta_min_ = 1.0f;
  }

  // Compute seed Gaussian blur kernel.
  float sigma_seed = std::sqrt(sigma_min_ * sigma_min_ - sigma_input_ * sigma_input_) / delta_min_;
  auto seedWeights = GaussianWeights(sigma_seed);
  seedConvWeightsBuffer_ = [device_ newBufferWithBytes:seedWeights.data()
                                                length:seedWeights.size() * sizeof(float)
                                               options:MTLResourceStorageModeShared];
  uint32_t seedWeightCount = static_cast<uint32_t>(seedWeights.size());
  seedConvParamsBuffer_ = [device_ newBufferWithBytes:&seedWeightCount
                                               length:sizeof(uint32_t)
                                              options:MTLResourceStorageModeShared];
  if (!seedConvWeightsBuffer_ || !seedConvParamsBuffer_) {
    AddStatusMessage(
        StatusSeverity::kError, "init.resources", "Failed to allocate seed convolution buffers");
    return false;
  }

  last_status_.message = "Initialized Metal SIFT runtime";
  return true;
}

// ---------------------------------------------------------------------------
// SetupOctaves
// ---------------------------------------------------------------------------
bool SiftMetalExtractorImpl::SetupOctaves(int w, int h) {
  int num_octaves = options_.num_octaves;
  if (num_octaves <= 0) {
    // Match SiftGPU's octave count: floor(log2(min(w,h))) - 3
    // But applied to the seed image dimensions (after upscaling).
    int seed_min =
        std::min(static_cast<int>(float(w) / delta_min_), static_cast<int>(float(h) / delta_min_));
    num_octaves = static_cast<int>(std::floor(std::log2(float(seed_min)))) - 3;
    num_octaves = std::max(1, num_octaves);
  }

  octaves_.clear();
  octaves_.reserve(num_octaves);
  for (int o = 0; o < num_octaves; ++o) {
    float delta = delta_min_ * std::pow(2.0f, float(o));
    int ow = static_cast<int>(float(w) / delta);
    int oh = static_cast<int>(float(h) / delta);
    if (ow < 8 || oh < 8) {
      break;
    }

    int ns = options_.scales_per_octave;
    std::vector<float> sigmas;
    for (int s = 0; s < ns + 3; ++s) {
      float ratio = delta / delta_min_;
      float scale = std::pow(2.0f, float(s) / float(ns));
      sigmas.push_back(ratio * sigma_min_ * scale);
    }

    Octave oct;
    if (!SetupOctave(oct, o, delta, ow, oh, ns, sigmas)) {
      octaves_.clear();
      return false;
    }
    octaves_.push_back(std::move(oct));
  }
  return true;
}

bool SiftMetalExtractorImpl::SetupOctave(Octave& oct,
                                         int o,
                                         float delta,
                                         int w,
                                         int h,
                                         int num_scales,
                                         const std::vector<float>& sigmas) {
  oct.o = o;
  oct.delta = delta;
  oct.width = w;
  oct.height = h;
  oct.num_scales = num_scales;
  oct.sigmas = sigmas;

  int numGaussians = num_scales + 3;
  int numDifferences = num_scales + 2;

  oct.gaussianTextures =
      MakeTexture2DArray(device_, w, h, numGaussians, MTLPixelFormatR32Float, MTLStorageModeShared);
  oct.differenceTextures = MakeTexture2DArray(
      device_, w, h, numDifferences, MTLPixelFormatR32Float, MTLStorageModeShared);
  oct.gradientTextures = MakeTexture2DArray(
      device_, w, h, numGaussians, MTLPixelFormatRG32Float, MTLStorageModeShared);

  // Convolution work texture (single-slice private).
  oct.convWorkTexture =
      MakeTexture2DArray(device_, w, h, 1, MTLPixelFormatR32Float, MTLStorageModePrivate);
  if (!oct.gaussianTextures || !oct.differenceTextures || !oct.gradientTextures ||
      !oct.convWorkTexture) {
    AddStatusMessage(StatusSeverity::kError,
                     "layout.octave",
                     "Failed to allocate octave textures",
                     "octave=" + std::to_string(o));
    return false;
  }

  // Build convolution parameter buffers for Gaussian series.
  oct.convPairs.resize(numGaussians - 1);
  for (int s = 1; s < numGaussians; ++s) {
    float sa = sigmas[s - 1];
    float sb = sigmas[s];
    float rho = std::sqrt(sb * sb - sa * sa) / delta;
    auto weights = GaussianWeights(rho);

    // X pass: read from slice [s-1], write to work slice [0]
    ConvolutionParameters paramsX = {};
    paramsX.inputDepth = static_cast<int32_t>(s - 1);
    paramsX.outputDepth = 0;
    paramsX.count = static_cast<int32_t>(weights.size());
    std::memcpy(paramsX.weights,
                weights.data(),
                std::min(weights.size(), (size_t)CONVOLUTION_WEIGHTS_LENGTH) * sizeof(float));
    oct.convPairs[s - 1].paramsX = [device_ newBufferWithBytes:&paramsX
                                                        length:sizeof(ConvolutionParameters)
                                                       options:MTLResourceStorageModeShared];

    // Y pass: read from work slice [0], write to slice [s]
    ConvolutionParameters paramsY = {};
    paramsY.inputDepth = 0;
    paramsY.outputDepth = static_cast<int32_t>(s);
    paramsY.count = static_cast<int32_t>(weights.size());
    std::memcpy(paramsY.weights,
                weights.data(),
                std::min(weights.size(), (size_t)CONVOLUTION_WEIGHTS_LENGTH) * sizeof(float));
    oct.convPairs[s - 1].paramsY = [device_ newBufferWithBytes:&paramsY
                                                        length:sizeof(ConvolutionParameters)
                                                       options:MTLResourceStorageModeShared];
    if (!oct.convPairs[s - 1].paramsX || !oct.convPairs[s - 1].paramsY) {
      AddStatusMessage(StatusSeverity::kError,
                       "layout.octave.convolution",
                       "Failed to allocate octave convolution buffers",
                       "octave=" + std::to_string(o) + ", scale=" + std::to_string(s));
      return false;
    }
  }

  const int64_t extrema_search_space = ExtremaSearchSpaceSize(w, h, num_scales);
  if (extrema_search_space > std::numeric_limits<uint32_t>::max()) {
    std::ostringstream detail;
    detail << "octave=" << o << ", search_space=" << extrema_search_space;
    AddStatusMessage(StatusSeverity::kError,
                     "layout.octave.extrema",
                     "Octave extrema search space exceeds Metal SIFT indexing capacity",
                     detail.str());
    return false;
  }
  oct.extrema_linear_count = static_cast<uint32_t>(extrema_search_space);
  oct.extrema_capacity = ComputeExtremaCapacity(options_, extrema_search_space);
  oct.keypoint_capacity = ComputeKeypointCapacity(oct.extrema_capacity);
  oct.descriptor_capacity = ComputeDescriptorCapacity(options_, oct.keypoint_capacity);

  // Extrema buffers
  oct.extremaOutputBuffer =
      [device_ newBufferWithLength:static_cast<NSUInteger>(oct.extrema_capacity) *
                                   sizeof(SIFTExtremaResult)
                           options:MTLResourceStorageModeShared];
  oct.extremaIndexBuffer = [device_ newBufferWithLength:sizeof(uint32_t)
                                                options:MTLResourceStorageModeShared];
  oct.extremaParamsBuffer = [device_ newBufferWithLength:sizeof(SIFTExtremaParameters)
                                                 options:MTLResourceStorageModeShared];
  if (!oct.extremaOutputBuffer || !oct.extremaIndexBuffer || !oct.extremaParamsBuffer) {
    AddStatusMessage(StatusSeverity::kError,
                     "layout.octave.extrema",
                     "Failed to allocate extrema buffers",
                     "octave=" + std::to_string(o));
    return false;
  }
  auto* extremaParams = static_cast<SIFTExtremaParameters*>(oct.extremaParamsBuffer.contents);
  extremaParams->outputCapacity = oct.extrema_capacity;
  extremaParams->gridWidth = static_cast<uint32_t>(w - 2);
  extremaParams->gridHeight = static_cast<uint32_t>(h - 2);
  extremaParams->linearStart = 0;
  extremaParams->linearEnd = oct.extrema_linear_count;

  // Interpolation buffers
  oct.interpolateInputBuffer =
      [device_ newBufferWithLength:static_cast<NSUInteger>(oct.keypoint_capacity) *
                                   sizeof(SIFTInterpolateInputKeypoint)
                           options:MTLResourceStorageModeShared];
  oct.interpolateOutputBuffer =
      [device_ newBufferWithLength:static_cast<NSUInteger>(oct.keypoint_capacity) *
                                   sizeof(SIFTInterpolateOutputKeypoint)
                           options:MTLResourceStorageModeShared];
  oct.interpolateParamsBuffer = [device_ newBufferWithLength:sizeof(SIFTInterpolateParameters)
                                                     options:MTLResourceStorageModeShared];
  if (!oct.interpolateInputBuffer || !oct.interpolateOutputBuffer || !oct.interpolateParamsBuffer) {
    AddStatusMessage(StatusSeverity::kError,
                     "layout.octave.interpolate",
                     "Failed to allocate interpolation buffers",
                     "octave=" + std::to_string(o));
    return false;
  }

  // Orientation buffers
  oct.orientationInputBuffer =
      [device_ newBufferWithLength:static_cast<NSUInteger>(oct.keypoint_capacity) *
                                   sizeof(SIFTOrientationKeypoint)
                           options:MTLResourceStorageModeShared];
  oct.orientationOutputBuffer =
      [device_ newBufferWithLength:static_cast<NSUInteger>(oct.keypoint_capacity) *
                                   sizeof(SIFTOrientationResult)
                           options:MTLResourceStorageModeShared];
  oct.orientationParamsBuffer = [device_ newBufferWithLength:sizeof(SIFTOrientationParameters)
                                                     options:MTLResourceStorageModeShared];
  if (!oct.orientationInputBuffer || !oct.orientationOutputBuffer || !oct.orientationParamsBuffer) {
    AddStatusMessage(StatusSeverity::kError,
                     "layout.octave.orientation",
                     "Failed to allocate orientation buffers",
                     "octave=" + std::to_string(o));
    return false;
  }

  // Descriptor buffers
  oct.descriptorInputBuffer =
      [device_ newBufferWithLength:static_cast<NSUInteger>(oct.descriptor_capacity) *
                                   sizeof(SIFTDescriptorInput)
                           options:MTLResourceStorageModeShared];
  oct.descriptorOutputBuffer =
      [device_ newBufferWithLength:static_cast<NSUInteger>(oct.descriptor_capacity) *
                                   sizeof(SIFTDescriptorResult)
                           options:MTLResourceStorageModeShared];
  oct.descriptorParamsBuffer = [device_ newBufferWithLength:sizeof(SIFTDescriptorParameters)
                                                    options:MTLResourceStorageModeShared];
  if (!oct.descriptorInputBuffer || !oct.descriptorOutputBuffer || !oct.descriptorParamsBuffer) {
    AddStatusMessage(StatusSeverity::kError,
                     "layout.octave.descriptor",
                     "Failed to allocate descriptor buffers",
                     "octave=" + std::to_string(o));
    return false;
  }
  return true;
}

bool SiftMetalExtractorImpl::EnsureImageLayout(int w, int h) {
  const int seed_w = static_cast<int>(float(w) / delta_min_);
  const int seed_h = static_cast<int>(float(h) / delta_min_);
  last_status_.image_width = w;
  last_status_.image_height = h;
  last_status_.seed_width = seed_w;
  last_status_.seed_height = seed_h;

  if (w == input_w_ && h == input_h_ && luminosityTexture_ && scaledTexture_ && seedTexture_ &&
      seedConvWorkTexture_ && uploadBuffer_) {
    return true;
  }

  input_w_ = w;
  input_h_ = h;
  seed_w_ = seed_w;
  seed_h_ = seed_h;

  luminosityTexture_ = MakeTexture2D(device_, w, h, MTLPixelFormatR32Float, MTLStorageModeShared);
  scaledTexture_ =
      MakeTexture2D(device_, seed_w_, seed_h_, MTLPixelFormatR32Float, MTLStorageModeShared);
  seedTexture_ =
      MakeTexture2D(device_, seed_w_, seed_h_, MTLPixelFormatR32Float, MTLStorageModeShared);
  seedConvWorkTexture_ =
      MakeTexture2D(device_, seed_w_, seed_h_, MTLPixelFormatR32Float, MTLStorageModePrivate);

  uploadBuffer_ = [device_ newBufferWithLength:static_cast<size_t>(w) * h * sizeof(float)
                                       options:MTLResourceStorageModeShared];

  if (!luminosityTexture_ || !scaledTexture_ || !seedTexture_ || !seedConvWorkTexture_ ||
      !uploadBuffer_) {
    std::ostringstream detail;
    detail << "image=" << w << "x" << h << ", seed=" << seed_w_ << "x" << seed_h_;
    AddStatusMessage(StatusSeverity::kError,
                     "layout.seed",
                     "Failed to allocate image layout resources",
                     detail.str());
    return false;
  }

  return SetupOctaves(w, h);
}

// ---------------------------------------------------------------------------
// Extract
// ---------------------------------------------------------------------------
bool SiftMetalExtractorImpl::Extract(const uint8_t* data, int w, int h, ExtractResult* result) {
  ResetStatus("extract");
  last_status_.image_width = w;
  last_status_.image_height = h;

  if (data == nullptr || result == nullptr || w <= 0 || h <= 0) {
    AddStatusMessage(StatusSeverity::kError,
                     "extract.input",
                     "Invalid image input",
                     "data=" + std::string(data == nullptr ? "null" : "set") +
                         ", result=" + std::string(result == nullptr ? "null" : "set") +
                         ", width=" + std::to_string(w) + ", height=" + std::to_string(h));
    if (result != nullptr) {
      result->status = last_status_;
    }
    return false;
  }

  result->keypoints.clear();
  result->descriptors.clear();
  result->status = StatusReport();

  if (!device_ || !commandQueue_ || !library_) {
    AddStatusMessage(
        StatusSeverity::kError, "extract.runtime", "Metal SIFT runtime is not initialized");
    result->status = last_status_;
    return false;
  }

  if (!EnsureImageLayout(w, h)) {
    result->status = last_status_;
    return false;
  }

  if (octaves_.empty()) {
    last_status_.message = "Extraction completed without octaves";
    result->status = last_status_;
    return true;
  }

  // Convert uint8 grayscale to float and upload to luminosity texture.
  float* uploadPtr = static_cast<float*>(uploadBuffer_.contents);
  size_t npixels = (size_t)w * h;
  for (size_t i = 0; i < npixels; ++i) {
    uploadPtr[i] = static_cast<float>(data[i]) / 255.0f;
  }
  MTLRegion region = MTLRegionMake2D(0, 0, w, h);
  [luminosityTexture_ replaceRegion:region
                        mipmapLevel:0
                          withBytes:uploadPtr
                        bytesPerRow:w * sizeof(float)];

  // Phase 1: Build scale-space pyramid (DoG + gradients + extrema).
  {
    id<MTLCommandBuffer> cb = [commandQueue_ commandBuffer];
    EncodeSeedTexture(cb);

    // First octave reads from seed texture (2D).
    if (!EncodeOctave(cb, octaves_[0], seedTexture_, true)) {
      result->status = last_status_;
      return false;
    }

    // Subsequent octaves read from previous octave's Gaussian textures.
    for (size_t i = 1; i < octaves_.size(); ++i) {
      if (!EncodeOctave(cb, octaves_[i], octaves_[i - 1].gaussianTextures, false)) {
        result->status = last_status_;
        return false;
      }
    }

    if (!WaitForCommandBuffer(cb, "extract.scale_space")) {
      result->status = last_status_;
      return false;
    }
  }

  // Phase 2: For each octave, read extrema, interpolate, orientate, describe.
  for (auto& oct : octaves_) {
    int extremaCount = ReadExtremaCount(oct);
    if (extremaCount < 0) {
      result->status = last_status_;
      return false;
    }
    if (extremaCount <= 0) continue;

    int interpolatedCount = InterpolateKeypoints(oct, extremaCount);
    if (interpolatedCount < 0) {
      result->status = last_status_;
      return false;
    }

    // Read interpolated keypoints.
    auto* interpOut =
        static_cast<SIFTInterpolateOutputKeypoint*>(oct.interpolateOutputBuffer.contents);
    float sigmaRatio = oct.sigmas[1] / oct.sigmas[0];

    std::vector<Keypoint> octKeypoints;
    for (int k = 0; k < interpolatedCount; ++k) {
      auto& p = interpOut[k];
      if (!p.converged) continue;

      Keypoint kp;
      kp.x = p.absoluteX;
      kp.y = p.absoluteY;
      kp.sigma = oct.sigmas[p.scale] * std::pow(sigmaRatio, p.subScale);
      kp.orientation = 0;  // Will be set during orientation pass.
      octKeypoints.push_back(kp);
    }

    if (octKeypoints.empty()) continue;

    // Compute orientations.
    std::vector<std::pair<int, float>> oriented;  // (keypoint_index, theta)
    if (!ComputeOrientations(oct, octKeypoints, oriented)) {
      result->status = last_status_;
      return false;
    }

    if (oriented.empty()) continue;

    // Compute descriptors.
    if (!ComputeDescriptors(oct, octKeypoints, oriented, result)) {
      result->status = last_status_;
      return false;
    }
  }

  // Sort by scale (descending) and truncate to max_num_features.
  if (options_.max_num_features > 0 && (int)result->keypoints.size() > options_.max_num_features) {
    const int64_t dropped_features =
        static_cast<int64_t>(result->keypoints.size()) - options_.max_num_features;
    last_status_.capacity.dropped_features += dropped_features;
    std::ostringstream detail;
    detail << "observed=" << result->keypoints.size()
           << ", max_num_features=" << options_.max_num_features
           << ", dropped=" << dropped_features;
    AddStatusMessage(StatusSeverity::kWarning,
                     "extract.feature_limit",
                     "Dropped features due to max_num_features limit",
                     detail.str());
    // Create index array, sort by sigma descending with deterministic tie
    // breaks, then restore the original extraction order among retained rows.
    std::vector<int> indices(result->keypoints.size());
    std::iota(indices.begin(), indices.end(), 0);
    std::sort(indices.begin(), indices.end(), [&](int a, int b) {
      const Keypoint& lhs = result->keypoints[a];
      const Keypoint& rhs = result->keypoints[b];
      if (lhs.sigma != rhs.sigma) return lhs.sigma > rhs.sigma;
      if (lhs.y != rhs.y) return lhs.y < rhs.y;
      if (lhs.x != rhs.x) return lhs.x < rhs.x;
      if (lhs.orientation != rhs.orientation) return lhs.orientation < rhs.orientation;
      return a < b;
    });
    indices.resize(options_.max_num_features);
    std::sort(indices.begin(), indices.end());  // Restore order.

    std::vector<Keypoint> newKp;
    std::vector<float> newDesc;
    newKp.reserve(options_.max_num_features);
    newDesc.reserve(options_.max_num_features * 128);
    for (int idx : indices) {
      newKp.push_back(result->keypoints[idx]);
      newDesc.insert(newDesc.end(),
                     result->descriptors.begin() + idx * 128,
                     result->descriptors.begin() + (idx + 1) * 128);
    }
    result->keypoints = std::move(newKp);
    result->descriptors = std::move(newDesc);
  }

  if (last_status_.message.empty()) {
    last_status_.message = "Extraction completed";
  }
  result->status = last_status_;
  return true;
}

// ---------------------------------------------------------------------------
// EncodeSeedTexture: upscale grayscale input + Gaussian blur.
// ---------------------------------------------------------------------------
void SiftMetalExtractorImpl::EncodeSeedTexture(id<MTLCommandBuffer> cb) {
  // Bilinear upscale luminosity → scaled.
  if (seed_w_ != input_w_ || seed_h_ != input_h_) {
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:bilinearUpScalePipeline_];
    [enc setTexture:scaledTexture_ atIndex:0];
    [enc setTexture:luminosityTexture_ atIndex:1];
    MTLSize tg = {16, 16, 1};
    MTLSize grid = {(NSUInteger)(seed_w_ + 15) / 16, (NSUInteger)(seed_h_ + 15) / 16, 1};
    [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
    [enc endEncoding];
  } else {
    // Same size: just copy.
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:luminosityTexture_ toTexture:scaledTexture_];
    [blit endEncoding];
  }

  // Gaussian blur scaled → seed (separable 1D convolution).
  {
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:convolutionXPipeline_];
    [enc setTexture:seedConvWorkTexture_ atIndex:0];
    [enc setTexture:scaledTexture_ atIndex:1];
    [enc setBuffer:seedConvWeightsBuffer_ offset:0 atIndex:0];
    [enc setBuffer:seedConvParamsBuffer_ offset:0 atIndex:1];
    MTLSize tg = {16, 16, 1};
    MTLSize grid = {(NSUInteger)(seed_w_ + 15) / 16, (NSUInteger)(seed_h_ + 15) / 16, 1};
    [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
    [enc endEncoding];
  }
  {
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:convolutionYPipeline_];
    [enc setTexture:seedTexture_ atIndex:0];
    [enc setTexture:seedConvWorkTexture_ atIndex:1];
    [enc setBuffer:seedConvWeightsBuffer_ offset:0 atIndex:0];
    [enc setBuffer:seedConvParamsBuffer_ offset:0 atIndex:1];
    MTLSize tg = {16, 16, 1};
    MTLSize grid = {(NSUInteger)(seed_w_ + 15) / 16, (NSUInteger)(seed_h_ + 15) / 16, 1};
    [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
    [enc endEncoding];
  }
}

// ---------------------------------------------------------------------------
// EncodeOctave
// ---------------------------------------------------------------------------
bool SiftMetalExtractorImpl::EncodeOctave(id<MTLCommandBuffer> cb,
                                          Octave& oct,
                                          id<MTLTexture> inputTexture,
                                          bool inputIs2D) {
  int w = oct.width;
  int h = oct.height;

  // Copy/scale input into gaussian slice 0.
  if (inputIs2D) {
    // 2D texture → first slice of 2DArray.
    if ((int)inputTexture.width == w && (int)inputTexture.height == h) {
      id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
      [blit copyFromTexture:inputTexture
                sourceSlice:0
                sourceLevel:0
               sourceOrigin:MTLOriginMake(0, 0, 0)
                 sourceSize:MTLSizeMake(w, h, 1)
                  toTexture:oct.gaussianTextures
           destinationSlice:0
           destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0)];
      [blit endEncoding];
    } else {
      std::ostringstream detail;
      detail << "input=" << (inputTexture ? inputTexture.width : 0) << "x"
             << (inputTexture ? inputTexture.height : 0) << ", octave=" << w << "x" << h;
      AddStatusMessage(StatusSeverity::kError,
                       "extract.scale_space.octave_" + std::to_string(oct.o),
                       "Octave seed texture dimensions do not match",
                       detail.str());
      return false;
    }
  } else {
    // Nearest-neighbor downscale from previous octave's gaussian[num_scales].
    id<MTLBuffer> paramsBuf = [device_ newBufferWithLength:sizeof(NearestNeighborScaleParameters)
                                                   options:MTLResourceStorageModeShared];
    if (!paramsBuf) {
      AddStatusMessage(StatusSeverity::kError,
                       "extract.scale_space.octave_" + std::to_string(oct.o),
                       "Failed to allocate octave downscale parameters");
      return false;
    }
    auto* p = static_cast<NearestNeighborScaleParameters*>(paramsBuf.contents);
    p->inputSlice = oct.num_scales;
    p->outputSlice = 0;

    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:nearestNeighborDownScalePipeline_];
    [enc setTexture:oct.gaussianTextures atIndex:0];
    [enc setTexture:inputTexture atIndex:1];
    [enc setBuffer:paramsBuf offset:0 atIndex:0];
    MTLSize tg = {16, 16, 1};
    MTLSize grid = {(NSUInteger)(w + 15) / 16, (NSUInteger)(h + 15) / 16, 1};
    [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
    [enc endEncoding];
  }

  // Gaussian series blur.
  EncodeGaussianSeries(cb, oct);
  // Differences.
  EncodeDifferences(cb, oct);
  // Gradients.
  EncodeGradients(cb, oct);
  // Extrema detection.
  EncodeExtrema(cb, oct, oct.extrema_capacity, 0, oct.extrema_linear_count);
  return true;
}

void SiftMetalExtractorImpl::EncodeGaussianSeries(id<MTLCommandBuffer> cb, Octave& oct) {
  int w = oct.width;
  int h = oct.height;
  MTLSize tg = {16, 16, 1};
  MTLSize grid = {(NSUInteger)(w + 15) / 16, (NSUInteger)(h + 15) / 16, 1};

  for (auto& pair : oct.convPairs) {
    // X pass: gaussian → work
    {
      id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
      [enc setComputePipelineState:convolutionSeriesXPipeline_];
      [enc setTexture:oct.convWorkTexture atIndex:0];
      [enc setTexture:oct.gaussianTextures atIndex:1];
      [enc setBuffer:pair.paramsX offset:0 atIndex:0];
      [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
      [enc endEncoding];
    }
    // Y pass: work → gaussian
    {
      id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
      [enc setComputePipelineState:convolutionSeriesYPipeline_];
      [enc setTexture:oct.gaussianTextures atIndex:0];
      [enc setTexture:oct.convWorkTexture atIndex:1];
      [enc setBuffer:pair.paramsY offset:0 atIndex:0];
      [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
      [enc endEncoding];
    }
  }
}

void SiftMetalExtractorImpl::EncodeDifferences(id<MTLCommandBuffer> cb, Octave& oct) {
  int w = oct.width;
  int h = oct.height;
  int numDiff = oct.num_scales + 2;

  id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
  [enc setComputePipelineState:subtractPipeline_];
  [enc setTexture:oct.differenceTextures atIndex:0];
  [enc setTexture:oct.gaussianTextures atIndex:1];
  MTLSize tg = {8, 8, 8};
  MTLSize grid = {(NSUInteger)(w + 7) / 8, (NSUInteger)(h + 7) / 8, (NSUInteger)(numDiff + 7) / 8};
  [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
  [enc endEncoding];
}

void SiftMetalExtractorImpl::EncodeGradients(id<MTLCommandBuffer> cb, Octave& oct) {
  int w = oct.width;
  int h = oct.height;
  int arrayLen = oct.num_scales + 3;

  id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
  [enc setComputePipelineState:siftGradientPipeline_];
  [enc setTexture:oct.gradientTextures atIndex:0];
  [enc setTexture:oct.gaussianTextures atIndex:1];
  MTLSize tg = {8, 8, 8};
  MTLSize grid = {(NSUInteger)(w + 7) / 8, (NSUInteger)(h + 7) / 8, (NSUInteger)(arrayLen + 7) / 8};
  [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
  [enc endEncoding];
}

void SiftMetalExtractorImpl::EncodeExtrema(id<MTLCommandBuffer> cb,
                                           Octave& oct,
                                           uint32_t output_capacity,
                                           uint32_t linear_start,
                                           uint32_t linear_end) {
  int w = oct.width;
  int h = oct.height;
  int numDiff = oct.num_scales + 2;

  auto* params = static_cast<SIFTExtremaParameters*>(oct.extremaParamsBuffer.contents);
  params->outputCapacity = output_capacity;
  params->gridWidth = static_cast<uint32_t>(w - 2);
  params->gridHeight = static_cast<uint32_t>(h - 2);
  params->linearStart = linear_start;
  params->linearEnd = linear_end;

  // Reset index counter.
  auto* idx = static_cast<uint32_t*>(oct.extremaIndexBuffer.contents);
  *idx = 0;

  id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
  [enc setComputePipelineState:siftExtremaListPipeline_];
  [enc setBuffer:oct.extremaOutputBuffer offset:0 atIndex:0];
  [enc setBuffer:oct.extremaIndexBuffer offset:0 atIndex:1];
  [enc setBuffer:oct.extremaParamsBuffer offset:0 atIndex:2];
  [enc setTexture:oct.differenceTextures atIndex:0];

  NSUInteger maxThreads =
      std::min<NSUInteger>(siftExtremaListPipeline_.maxTotalThreadsPerThreadgroup,
                           kMaxExtremaThreadgroupSize);
  NSUInteger dim = (NSUInteger)std::cbrt((double)maxThreads);
  MTLSize tg = {dim, dim, dim};
  MTLSize gridSize = {(NSUInteger)(w - 2), (NSUInteger)(h - 2), (NSUInteger)(numDiff - 2)};
  [enc dispatchThreads:gridSize threadsPerThreadgroup:tg];
  [enc endEncoding];
}

// ---------------------------------------------------------------------------
// ReadExtremaCount
// ---------------------------------------------------------------------------
int SiftMetalExtractorImpl::ReadExtremaCount(Octave& oct) {
  auto* idx = static_cast<uint32_t*>(oct.extremaIndexBuffer.contents);
  const uint32_t observed = *idx;
  last_status_.capacity.detected_extrema += observed;
  last_status_.capacity.extrema_capacity += oct.extrema_capacity;
  if (observed > oct.extrema_capacity) {
    RecordCapacityDrop("extract.extrema.octave_" + std::to_string(oct.o),
                       "extrema",
                       observed - oct.extrema_capacity,
                       oct.extrema_capacity,
                       observed);
    const int compacted_count = CompactExtremaDeterministically(oct, oct.extrema_capacity);
    if (compacted_count < 0) return -1;
    SortExtrema(oct, compacted_count);
    *idx = 0;
    return compacted_count;
  }

  int count = static_cast<int>(std::min(observed, oct.extrema_capacity));
  SortExtrema(oct, count);
  *idx = 0;
  return count;
}

bool SiftMetalExtractorImpl::CountExtremaInRange(Octave& oct,
                                                 uint32_t linear_end,
                                                 uint32_t* count) {
  if (count != nullptr) *count = 0;

  id<MTLCommandBuffer> cb = [commandQueue_ commandBuffer];
  EncodeExtrema(cb, oct, 0, 0, linear_end);
  if (!WaitForCommandBuffer(cb, "extract.extrema.compact_count.octave_" +
                                    std::to_string(oct.o))) {
    return false;
  }

  auto* idx = static_cast<uint32_t*>(oct.extremaIndexBuffer.contents);
  if (count != nullptr) *count = *idx;
  return true;
}

int SiftMetalExtractorImpl::CompactExtremaDeterministically(Octave& oct,
                                                            uint32_t target_count) {
  if (target_count == 0) return 0;

  uint32_t low = 0;
  uint32_t high = oct.extrema_linear_count;
  while (low < high) {
    const uint32_t mid = low + (high - low) / 2;
    uint32_t count = 0;
    if (!CountExtremaInRange(oct, mid, &count)) {
      return -1;
    }
    if (count >= target_count) {
      high = mid;
    } else {
      low = mid + 1;
    }
  }

  id<MTLCommandBuffer> cb = [commandQueue_ commandBuffer];
  EncodeExtrema(cb, oct, target_count, 0, low);
  if (!WaitForCommandBuffer(cb, "extract.extrema.compact_write.octave_" +
                                    std::to_string(oct.o))) {
    return -1;
  }

  auto* idx = static_cast<uint32_t*>(oct.extremaIndexBuffer.contents);
  const uint32_t compacted_count = std::min(*idx, target_count);
  if (compacted_count != target_count) {
    std::ostringstream detail;
    detail << "requested=" << target_count << ", compacted=" << compacted_count
           << ", cutoff=" << low;
    AddStatusMessage(StatusSeverity::kWarning,
                     "extract.extrema.compact.octave_" + std::to_string(oct.o),
                     "Deterministic extrema compaction produced fewer candidates than expected",
                     detail.str());
  }
  return static_cast<int>(compacted_count);
}

void SiftMetalExtractorImpl::SortExtrema(Octave& oct, int count) {
  if (count <= 1) return;
  auto* extrema = static_cast<SIFTExtremaResult*>(oct.extremaOutputBuffer.contents);
  std::sort(extrema, extrema + count, [](const SIFTExtremaResult& lhs,
                                         const SIFTExtremaResult& rhs) {
    return lhs.linearIndex < rhs.linearIndex;
  });
}

// ---------------------------------------------------------------------------
// InterpolateKeypoints
// ---------------------------------------------------------------------------
int SiftMetalExtractorImpl::InterpolateKeypoints(Octave& oct, int extremaCount) {
  const int capacity = static_cast<int>(oct.keypoint_capacity);
  int count = std::min(extremaCount, capacity);
  if (extremaCount > capacity) {
    RecordCapacityDrop("extract.interpolate.octave_" + std::to_string(oct.o),
                       "keypoints",
                       extremaCount - capacity,
                       capacity,
                       extremaCount);
  }

  // Copy extrema to interpolation input buffer.
  auto* extrema = static_cast<SIFTExtremaResult*>(oct.extremaOutputBuffer.contents);
  auto* interpIn = static_cast<SIFTInterpolateInputKeypoint*>(oct.interpolateInputBuffer.contents);
  for (int i = 0; i < count; ++i) {
    interpIn[i].x = extrema[i].x;
    interpIn[i].y = extrema[i].y;
    interpIn[i].scale = extrema[i].scale;
  }

  // Set interpolation parameters.
  auto* params = static_cast<SIFTInterpolateParameters*>(oct.interpolateParamsBuffer.contents);
  params->dogThreshold = options_.peak_threshold;
  params->maxIterations = 5;
  params->maxOffset = 0.6f;
  params->width = static_cast<int32_t>(oct.width);
  params->height = static_cast<int32_t>(oct.height);
  params->octaveDelta = oct.delta;
  params->edgeThreshold = options_.edge_threshold;
  params->numberOfScales = static_cast<int32_t>(oct.num_scales);

  id<MTLCommandBuffer> cb = [commandQueue_ commandBuffer];
  id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
  [enc setComputePipelineState:siftInterpolatePipeline_];
  [enc setBuffer:oct.interpolateOutputBuffer offset:0 atIndex:0];
  [enc setBuffer:oct.interpolateInputBuffer offset:0 atIndex:1];
  [enc setBuffer:oct.interpolateParamsBuffer offset:0 atIndex:2];
  [enc setTexture:oct.differenceTextures atIndex:0];

  NSUInteger maxThreads = siftInterpolatePipeline_.maxTotalThreadsPerThreadgroup;
  MTLSize tg = {maxThreads, 1, 1};
  MTLSize gridSize = {(NSUInteger)count, 1, 1};
  [enc dispatchThreads:gridSize threadsPerThreadgroup:tg];
  [enc endEncoding];

  if (!WaitForCommandBuffer(cb, "extract.interpolate.octave_" + std::to_string(oct.o))) {
    return -1;
  }
  return count;
}

// ---------------------------------------------------------------------------
// ComputeOrientations
// ---------------------------------------------------------------------------
bool SiftMetalExtractorImpl::ComputeOrientations(Octave& oct,
                                                 const std::vector<Keypoint>& keypoints,
                                                 std::vector<std::pair<int, float>>& oriented) {
  oriented.clear();

  float delta = oct.delta;
  float lambda = 1.5f;
  float orientThreshold = 0.8f;
  float minX = 1.0f, minY = 1.0f;
  float maxX = float(oct.width - 2);
  float maxY = float(oct.height - 2);

  auto* params = static_cast<SIFTOrientationParameters*>(oct.orientationParamsBuffer.contents);
  params->delta = delta;
  params->lambda = lambda;
  params->orientationThreshold = orientThreshold;

  auto* orientIn = static_cast<SIFTOrientationKeypoint*>(oct.orientationInputBuffer.contents);

  int validCount = 0;
  int droppedByCapacity = 0;
  for (int k = 0; k < (int)keypoints.size(); ++k) {
    const auto& kp = keypoints[k];
    float x = kp.x / delta;
    float y = kp.y / delta;
    float sigma = kp.sigma / delta;
    float r = std::ceil(3.0f * lambda * sigma);

    if (std::floor(x - r) < minX || std::ceil(x + r) > maxX || std::floor(y - r) < minY ||
        std::ceil(y + r) > maxY) {
      continue;
    }

    if (validCount >= static_cast<int>(oct.keypoint_capacity)) {
      ++droppedByCapacity;
      continue;
    }

    // Determine the scale index by finding the closest sigma.
    int scaleIdx = 0;
    float bestDiff = 1e30f;
    for (int s = 0; s < (int)oct.sigmas.size(); ++s) {
      float diff = std::abs(oct.sigmas[s] - kp.sigma);
      if (diff < bestDiff) {
        bestDiff = diff;
        scaleIdx = s;
      }
    }

    orientIn[validCount].index = static_cast<int32_t>(k);
    orientIn[validCount].absoluteX = static_cast<int32_t>(kp.x);
    orientIn[validCount].absoluteY = static_cast<int32_t>(kp.y);
    orientIn[validCount].scale = static_cast<int32_t>(scaleIdx);
    orientIn[validCount].sigma = kp.sigma;
    ++validCount;
  }

  if (droppedByCapacity > 0) {
    RecordCapacityDrop("extract.orientation.octave_" + std::to_string(oct.o),
                       "keypoints",
                       droppedByCapacity,
                       oct.keypoint_capacity,
                       validCount + droppedByCapacity);
  }

  if (validCount == 0) return true;

  id<MTLCommandBuffer> cb = [commandQueue_ commandBuffer];
  id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
  [enc setComputePipelineState:siftOrientationPipeline_];
  [enc setBuffer:oct.orientationOutputBuffer offset:0 atIndex:0];
  [enc setBuffer:oct.orientationInputBuffer offset:0 atIndex:1];
  [enc setBuffer:oct.orientationParamsBuffer offset:0 atIndex:2];
  [enc setTexture:oct.gradientTextures atIndex:0];

  NSUInteger maxThreads = siftOrientationPipeline_.maxTotalThreadsPerThreadgroup;
  MTLSize tg = {maxThreads, 1, 1};
  MTLSize gridSize = {(NSUInteger)validCount, 1, 1};
  [enc dispatchThreads:gridSize threadsPerThreadgroup:tg];
  [enc endEncoding];

  if (!WaitForCommandBuffer(cb, "extract.orientation.octave_" + std::to_string(oct.o))) {
    return false;
  }

  // Read orientation results.
  auto* orientOut = static_cast<SIFTOrientationResult*>(oct.orientationOutputBuffer.contents);
  int droppedOrientations = 0;
  int observedOrientations = 0;
  const int maxOrient = options_.upright ? 1 : std::max(0, options_.max_num_orientations);
  for (int k = 0; k < validCount; ++k) {
    auto& res = orientOut[k];
    int kpIdx = static_cast<int>(res.keypoint);
    int count = static_cast<int>(res.count);
    observedOrientations += count;
    if (count > maxOrient) {
      droppedOrientations += count - maxOrient;
    }
    count = std::min(count, maxOrient);
    float* oris = reinterpret_cast<float*>(&res.orientations);
    for (int i = 0; i < count; ++i) {
      float theta = options_.upright ? 0.0f : oris[i];
      oriented.emplace_back(kpIdx, theta);
    }
  }
  if (droppedOrientations > 0) {
    last_status_.capacity.dropped_orientations += droppedOrientations;
    std::ostringstream detail;
    detail << "observed=" << observedOrientations << ", max_num_orientations=" << maxOrient
           << ", dropped=" << droppedOrientations;
    AddStatusMessage(StatusSeverity::kWarning,
                     "extract.orientation.octave_" + std::to_string(oct.o),
                     "Dropped orientations due to max_num_orientations limit",
                     detail.str());
  }
  return true;
}

// ---------------------------------------------------------------------------
// ComputeDescriptors
// ---------------------------------------------------------------------------
bool SiftMetalExtractorImpl::ComputeDescriptors(Octave& oct,
                                                const std::vector<Keypoint>& keypoints,
                                                const std::vector<std::pair<int, float>>& oriented,
                                                ExtractResult* result) {
  const int descriptor_capacity = static_cast<int>(oct.descriptor_capacity);
  int count = std::min((int)oriented.size(), descriptor_capacity);
  if ((int)oriented.size() > static_cast<int>(oct.descriptor_capacity)) {
    RecordCapacityDrop("extract.descriptor.octave_" + std::to_string(oct.o),
                       "descriptors",
                       static_cast<int64_t>(oriented.size()) - oct.descriptor_capacity,
                       oct.descriptor_capacity,
                       static_cast<int64_t>(oriented.size()));
  }
  if (count == 0) return true;

  auto* params = static_cast<SIFTDescriptorParameters*>(oct.descriptorParamsBuffer.contents);
  params->delta = oct.delta;
  params->scalesPerOctave = static_cast<int32_t>(oct.num_scales);
  params->width = static_cast<int32_t>(oct.width);
  params->height = static_cast<int32_t>(oct.height);

  auto* descIn = static_cast<SIFTDescriptorInput*>(oct.descriptorInputBuffer.contents);
  for (int i = 0; i < count; ++i) {
    int kpIdx = oriented[i].first;
    float theta = oriented[i].second;
    const auto& kp = keypoints[kpIdx];

    // Determine scale index.
    int scaleIdx = 0;
    float bestDiff = 1e30f;
    for (int s = 0; s < (int)oct.sigmas.size(); ++s) {
      float diff = std::abs(oct.sigmas[s] - kp.sigma);
      if (diff < bestDiff) {
        bestDiff = diff;
        scaleIdx = s;
      }
    }
    // Compute subScale.
    float sigmaRatio = oct.sigmas[1] / oct.sigmas[0];
    float subScale = std::log(kp.sigma / oct.sigmas[scaleIdx]) / std::log(sigmaRatio);

    descIn[i].keypoint = static_cast<int32_t>(kpIdx);
    descIn[i].absoluteX = static_cast<int32_t>(kp.x);
    descIn[i].absoluteY = static_cast<int32_t>(kp.y);
    descIn[i].scale = static_cast<int32_t>(scaleIdx);
    descIn[i].subScale = subScale;
    descIn[i].theta = theta;
  }

  id<MTLCommandBuffer> cb = [commandQueue_ commandBuffer];
  id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
  [enc setComputePipelineState:siftDescriptorsPipeline_];
  [enc setBuffer:oct.descriptorOutputBuffer offset:0 atIndex:0];
  [enc setBuffer:oct.descriptorInputBuffer offset:0 atIndex:1];
  [enc setBuffer:oct.descriptorParamsBuffer offset:0 atIndex:2];
  [enc setTexture:oct.gradientTextures atIndex:0];

  NSUInteger maxThreads = siftDescriptorsPipeline_.maxTotalThreadsPerThreadgroup;
  MTLSize tg = {maxThreads, 1, 1};
  MTLSize gridSize = {(NSUInteger)count, 1, 1};
  [enc dispatchThreads:gridSize threadsPerThreadgroup:tg];
  [enc endEncoding];

  if (!WaitForCommandBuffer(cb, "extract.descriptor.octave_" + std::to_string(oct.o))) {
    return false;
  }

  // Read descriptors.
  auto* descOut = static_cast<SIFTDescriptorResult*>(oct.descriptorOutputBuffer.contents);
  for (int i = 0; i < count; ++i) {
    auto& dr = descOut[i];
    if (!dr.valid) continue;

    int kpIdx = oriented[i].first;
    const auto& kp = keypoints[kpIdx];

    Keypoint finalKp;
    finalKp.x = kp.x;
    finalKp.y = kp.y;
    finalKp.sigma = kp.sigma;
    finalKp.orientation = dr.theta;
    result->keypoints.push_back(finalKp);

    // Keep standard SIFT floats for COLMAP's normalization pipeline.
    for (int j = 0; j < 128; ++j) {
      result->descriptors.push_back(dr.features[j]);
    }
  }
  return true;
}

// ===========================================================================
// Public API
// ===========================================================================

SiftMetalExtractor::SiftMetalExtractor() : impl_(std::make_unique<SiftMetalExtractorImpl>()) {}

SiftMetalExtractor::~SiftMetalExtractor() = default;

bool SiftMetalExtractor::Init(const Options& options, int max_w, int max_h) {
  return impl_->Init(options, max_w, max_h);
}

bool SiftMetalExtractor::Extract(const uint8_t* data, int w, int h, ExtractResult* result) {
  return impl_->Extract(data, w, h, result);
}

const StatusReport& SiftMetalExtractor::LastStatus() const { return impl_->LastStatus(); }

}  // namespace sift_metal
