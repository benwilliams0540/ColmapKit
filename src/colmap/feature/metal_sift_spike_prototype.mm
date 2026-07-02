// Copyright (c), ETH Zurich and UNC Chapel Hill.
// All rights reserved.
//
// Spike-only Metal SIFT extraction sketch. This file is intentionally not wired
// into COLMAP's build graph or production feature extraction factory.
//
// Build on macOS:
//   xcrun clang++ -std=c++17 -fobjc-arc -framework Foundation -framework Metal \
//     src/colmap/feature/metal_sift_spike_prototype.mm \
//     -o /tmp/metal_sift_spike_prototype
//   /tmp/metal_sift_spike_prototype

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

namespace {

constexpr int kDescriptorDim = 128;

struct SpikeOptions {
  uint32_t width = 256;
  uint32_t height = 192;
  uint32_t num_octaves = 3;
  uint32_t octave_resolution = 3;
  uint32_t max_num_features = 512;
  float peak_threshold = 0.01f;
  float edge_threshold = 10.0f;
};

struct MetalBlurParams {
  uint32_t width;
  uint32_t height;
  uint32_t radius;
  uint32_t input_level;
  uint32_t output_level;
};

struct MetalDogParams {
  uint32_t width;
  uint32_t height;
  uint32_t level;
};

struct MetalExtremaParams {
  uint32_t width;
  uint32_t height;
  uint32_t num_dog_levels;
  uint32_t max_candidates;
  float threshold;
  float edge_threshold;
};

struct MetalOrientationParams {
  uint32_t width;
  uint32_t height;
  uint32_t num_candidates;
  uint32_t num_gaussian_levels;
};

struct MetalDescriptorParams {
  uint32_t width;
  uint32_t height;
  uint32_t num_keypoints;
  uint32_t num_gaussian_levels;
};

struct MetalCandidate {
  uint32_t x;
  uint32_t y;
  uint32_t level;
  float response;
  float sigma;
};

struct MetalKeypoint {
  float x;
  float y;
  float scale;
  float orientation;
  float response;
};

struct SpikeKeypoint {
  float x;
  float y;
  float scale;
  float orientation;
  float response;
};

struct SpikeFeature {
  SpikeKeypoint keypoint;
  std::array<uint8_t, kDescriptorDim> descriptor;
};

struct Pipelines {
  id<MTLComputePipelineState> gaussian_blur_h;
  id<MTLComputePipelineState> gaussian_blur_v;
  id<MTLComputePipelineState> dog_difference;
  id<MTLComputePipelineState> detect_extrema;
  id<MTLComputePipelineState> assign_orientation;
  id<MTLComputePipelineState> make_descriptor;
};

const char* kMetalSource = R"METAL(
#include <metal_stdlib>
using namespace metal;

constant float kPi = 3.14159265358979323846f;
constant uint kDescriptorDim = 128;

struct BlurParams {
  uint width;
  uint height;
  uint radius;
  uint input_level;
  uint output_level;
};

struct DogParams {
  uint width;
  uint height;
  uint level;
};

struct ExtremaParams {
  uint width;
  uint height;
  uint num_dog_levels;
  uint max_candidates;
  float threshold;
  float edge_threshold;
};

struct OrientationParams {
  uint width;
  uint height;
  uint num_candidates;
  uint num_gaussian_levels;
};

struct DescriptorParams {
  uint width;
  uint height;
  uint num_keypoints;
  uint num_gaussian_levels;
};

struct Candidate {
  uint x;
  uint y;
  uint level;
  float response;
  float sigma;
};

struct Keypoint {
  float x;
  float y;
  float scale;
  float orientation;
  float response;
};

kernel void gaussian_blur_h(device const float* gaussian_stack [[buffer(0)]],
                            device float* temp [[buffer(1)]],
                            constant BlurParams& params [[buffer(2)]],
                            device const float* weights [[buffer(3)]],
                            uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= params.width || gid.y >= params.height) {
    return;
  }
  const uint area = params.width * params.height;
  const uint src_base = params.input_level * area;
  float sum = 0.0f;
  for (int k = -int(params.radius); k <= int(params.radius); ++k) {
    const int xx = clamp(int(gid.x) + k, 0, int(params.width) - 1);
    const uint idx = gid.y * params.width + uint(xx);
    sum += gaussian_stack[src_base + idx] *
           weights[uint(k + int(params.radius))];
  }
  temp[gid.y * params.width + gid.x] = sum;
}

kernel void gaussian_blur_v(device float* gaussian_stack [[buffer(0)]],
                            device const float* temp [[buffer(1)]],
                            constant BlurParams& params [[buffer(2)]],
                            device const float* weights [[buffer(3)]],
                            uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= params.width || gid.y >= params.height) {
    return;
  }
  const uint area = params.width * params.height;
  const uint dst_base = params.output_level * area;
  float sum = 0.0f;
  for (int k = -int(params.radius); k <= int(params.radius); ++k) {
    const int yy = clamp(int(gid.y) + k, 0, int(params.height) - 1);
    const uint idx = uint(yy) * params.width + gid.x;
    sum += temp[idx] * weights[uint(k + int(params.radius))];
  }
  gaussian_stack[dst_base + gid.y * params.width + gid.x] = sum;
}

kernel void dog_difference(device const float* gaussian_stack [[buffer(0)]],
                           device float* dog_stack [[buffer(1)]],
                           constant DogParams& params [[buffer(2)]],
                           uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= params.width || gid.y >= params.height) {
    return;
  }
  const uint area = params.width * params.height;
  const uint idx = gid.y * params.width + gid.x;
  dog_stack[params.level * area + idx] =
      gaussian_stack[(params.level + 1) * area + idx] -
      gaussian_stack[params.level * area + idx];
}

kernel void detect_extrema(device const float* dog_stack [[buffer(0)]],
                           device Candidate* candidates [[buffer(1)]],
                           device atomic_uint* candidate_count [[buffer(2)]],
                           device const float* sigmas [[buffer(3)]],
                           constant ExtremaParams& params [[buffer(4)]],
                           uint3 gid [[thread_position_in_grid]]) {
  const uint x = gid.x;
  const uint y = gid.y;
  const uint level = gid.z;
  if (x == 0 || y == 0 || x + 1 >= params.width ||
      y + 1 >= params.height || level == 0 ||
      level + 1 >= params.num_dog_levels) {
    return;
  }

  const uint area = params.width * params.height;
  const uint idx = level * area + y * params.width + x;
  const float center = dog_stack[idx];
  if (fabs(center) < params.threshold) {
    return;
  }

  bool is_max = center > 0.0f;
  bool is_min = center < 0.0f;
  for (int dl = -1; dl <= 1; ++dl) {
    for (int dy = -1; dy <= 1; ++dy) {
      for (int dx = -1; dx <= 1; ++dx) {
        if (dx == 0 && dy == 0 && dl == 0) {
          continue;
        }
        const uint n_level = uint(int(level) + dl);
        const uint n_y = uint(int(y) + dy);
        const uint n_x = uint(int(x) + dx);
        const float neighbor =
            dog_stack[n_level * area + n_y * params.width + n_x];
        is_max = is_max && center > neighbor;
        is_min = is_min && center < neighbor;
      }
    }
  }
  if (!is_max && !is_min) {
    return;
  }

  const float dxx = dog_stack[level * area + y * params.width + x + 1] +
                    dog_stack[level * area + y * params.width + x - 1] -
                    2.0f * center;
  const float dyy = dog_stack[level * area + (y + 1) * params.width + x] +
                    dog_stack[level * area + (y - 1) * params.width + x] -
                    2.0f * center;
  const float dxy =
      0.25f * (dog_stack[level * area + (y + 1) * params.width + x + 1] -
               dog_stack[level * area + (y + 1) * params.width + x - 1] -
               dog_stack[level * area + (y - 1) * params.width + x + 1] +
               dog_stack[level * area + (y - 1) * params.width + x - 1]);
  const float trace = dxx + dyy;
  const float det = dxx * dyy - dxy * dxy;
  const float edge_limit =
      ((params.edge_threshold + 1.0f) * (params.edge_threshold + 1.0f)) /
      params.edge_threshold;
  if (det <= 0.0f || trace * trace / det >= edge_limit) {
    return;
  }

  const uint slot =
      atomic_fetch_add_explicit(candidate_count, 1u, memory_order_relaxed);
  if (slot >= params.max_candidates) {
    return;
  }
  candidates[slot] = Candidate{x, y, level, center, sigmas[level]};
}

kernel void assign_orientation(device const float* gaussian_stack [[buffer(0)]],
                               device const Candidate* candidates [[buffer(1)]],
                               device Keypoint* keypoints [[buffer(2)]],
                               constant OrientationParams& params [[buffer(3)]],
                               uint gid [[thread_position_in_grid]]) {
  if (gid >= params.num_candidates) {
    return;
  }

  const Candidate candidate = candidates[gid];
  const uint area = params.width * params.height;
  const uint level = min(candidate.level + 1, params.num_gaussian_levels - 1);
  const uint base = level * area;
  const int cx = int(candidate.x);
  const int cy = int(candidate.y);
  const float sigma = max(candidate.sigma, 1.0f);
  const int radius = int(min(18.0f, max(3.0f, 3.0f * sigma)));
  float hist[36];
  for (uint i = 0; i < 36; ++i) {
    hist[i] = 0.0f;
  }

  for (int dy = -radius; dy <= radius; ++dy) {
    const int y = cy + dy;
    if (y <= 0 || y + 1 >= int(params.height)) {
      continue;
    }
    for (int dx = -radius; dx <= radius; ++dx) {
      const int x = cx + dx;
      if (x <= 0 || x + 1 >= int(params.width)) {
        continue;
      }
      const float r2 = float(dx * dx + dy * dy);
      const float gaussian_weight = exp(-r2 / (2.0f * sigma * sigma));
      const uint idx = uint(y) * params.width + uint(x);
      const float gx = gaussian_stack[base + idx + 1] -
                       gaussian_stack[base + idx - 1];
      const float gy = gaussian_stack[base + idx + params.width] -
                       gaussian_stack[base + idx - params.width];
      const float magnitude = sqrt(gx * gx + gy * gy);
      const float angle = atan2(gy, gx);
      const uint bin =
          min(uint(floor((angle + kPi) * (36.0f / (2.0f * kPi)))), 35u);
      hist[bin] += gaussian_weight * magnitude;
    }
  }

  uint best_bin = 0;
  float best_value = hist[0];
  for (uint i = 1; i < 36; ++i) {
    if (hist[i] > best_value) {
      best_value = hist[i];
      best_bin = i;
    }
  }

  keypoints[gid] = Keypoint{float(candidate.x) + 0.5f,
                            float(candidate.y) + 0.5f,
                            candidate.sigma,
                            (float(best_bin) + 0.5f) *
                                    (2.0f * kPi / 36.0f) -
                                kPi,
                            candidate.response};
}

kernel void make_descriptor(device const float* gaussian_stack [[buffer(0)]],
                            device const Keypoint* keypoints [[buffer(1)]],
                            device float* descriptors [[buffer(2)]],
                            constant DescriptorParams& params [[buffer(3)]],
                            uint gid [[thread_position_in_grid]]) {
  if (gid >= params.num_keypoints) {
    return;
  }

  const Keypoint keypoint = keypoints[gid];
  float hist[kDescriptorDim];
  for (uint i = 0; i < kDescriptorDim; ++i) {
    hist[i] = 0.0f;
  }

  const uint area = params.width * params.height;
  const uint level = min(uint(max(0.0f, round(log2(max(keypoint.scale, 1.0f))))),
                         params.num_gaussian_levels - 1);
  const uint base = level * area;
  const float cos_o = cos(keypoint.orientation);
  const float sin_o = sin(keypoint.orientation);
  const float sample_scale = max(1.0f, keypoint.scale / 1.6f);

  for (int sy = -8; sy < 8; ++sy) {
    for (int sx = -8; sx < 8; ++sx) {
      const float rx = sample_scale * (cos_o * float(sx) -
                                       sin_o * float(sy));
      const float ry = sample_scale * (sin_o * float(sx) +
                                       cos_o * float(sy));
      const int x = int(round(keypoint.x + rx));
      const int y = int(round(keypoint.y + ry));
      if (x <= 0 || y <= 0 || x + 1 >= int(params.width) ||
          y + 1 >= int(params.height)) {
        continue;
      }

      const int cell_x = clamp((sx + 8) / 4, 0, 3);
      const int cell_y = clamp((sy + 8) / 4, 0, 3);
      const uint idx = uint(y) * params.width + uint(x);
      const float gx = gaussian_stack[base + idx + 1] -
                       gaussian_stack[base + idx - 1];
      const float gy = gaussian_stack[base + idx + params.width] -
                       gaussian_stack[base + idx - params.width];
      float rel_angle = atan2(gy, gx) - keypoint.orientation;
      while (rel_angle < 0.0f) {
        rel_angle += 2.0f * kPi;
      }
      while (rel_angle >= 2.0f * kPi) {
        rel_angle -= 2.0f * kPi;
      }
      const uint orientation_bin =
          min(uint(floor(rel_angle * (8.0f / (2.0f * kPi)))), 7u);
      const uint hist_idx =
          uint((cell_y * 4 + cell_x) * 8) + orientation_bin;
      hist[hist_idx] += sqrt(gx * gx + gy * gy);
    }
  }

  float norm = 1.0e-12f;
  for (uint i = 0; i < kDescriptorDim; ++i) {
    norm += hist[i] * hist[i];
  }
  norm = sqrt(norm);
  for (uint i = 0; i < kDescriptorDim; ++i) {
    hist[i] = min(hist[i] / norm, 0.2f);
  }

  norm = 1.0e-12f;
  for (uint i = 0; i < kDescriptorDim; ++i) {
    norm += hist[i] * hist[i];
  }
  norm = sqrt(norm);
  for (uint i = 0; i < kDescriptorDim; ++i) {
    descriptors[gid * kDescriptorDim + i] = hist[i] / norm;
  }
}
)METAL";

std::string ToString(NSString* string) {
  return string == nil ? std::string() : std::string([string UTF8String]);
}

void ThrowIfNSError(NSError* error, const std::string& context) {
  if (error != nil) {
    throw std::runtime_error(context + ": " + ToString([error localizedDescription]));
  }
}

id<MTLBuffer> NewBuffer(id<MTLDevice> device, size_t num_bytes, const void* data = nullptr) {
  id<MTLBuffer> buffer = nil;
  if (data == nullptr) {
    buffer = [device newBufferWithLength:num_bytes options:MTLResourceStorageModeShared];
  } else {
    buffer = [device newBufferWithBytes:data length:num_bytes options:MTLResourceStorageModeShared];
  }
  if (buffer == nil) {
    throw std::runtime_error("Failed to allocate Metal buffer");
  }
  return buffer;
}

id<MTLComputePipelineState> NewPipeline(id<MTLDevice> device,
                                        id<MTLLibrary> library,
                                        NSString* name) {
  id<MTLFunction> function = [library newFunctionWithName:name];
  if (function == nil) {
    throw std::runtime_error("Missing Metal function: " + ToString(name));
  }
  NSError* error = nil;
  id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function
                                                                               error:&error];
  ThrowIfNSError(error, "Failed to compile Metal pipeline " + ToString(name));
  return pipeline;
}

Pipelines NewPipelines(id<MTLDevice> device) {
  NSError* error = nil;
  NSString* source = [NSString stringWithUTF8String:kMetalSource];
  id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
  ThrowIfNSError(error, "Failed to compile Metal source");
  if (library == nil) {
    throw std::runtime_error("Failed to create Metal library");
  }

  return Pipelines{
      NewPipeline(device, library, @"gaussian_blur_h"),
      NewPipeline(device, library, @"gaussian_blur_v"),
      NewPipeline(device, library, @"dog_difference"),
      NewPipeline(device, library, @"detect_extrema"),
      NewPipeline(device, library, @"assign_orientation"),
      NewPipeline(device, library, @"make_descriptor"),
  };
}

void CheckCommandBuffer(id<MTLCommandBuffer> command_buffer) {
  NSError* error = [command_buffer error];
  ThrowIfNSError(error, "Metal command buffer failed");
}

void Dispatch2D(id<MTLCommandQueue> command_queue,
                id<MTLComputePipelineState> pipeline,
                uint32_t width,
                uint32_t height,
                const std::function<void(id<MTLComputeCommandEncoder>)>& bind) {
  id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
  id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
  [encoder setComputePipelineState:pipeline];
  bind(encoder);
  const MTLSize grid = MTLSizeMake(width, height, 1);
  const MTLSize threads = MTLSizeMake(16, 16, 1);
  [encoder dispatchThreads:grid threadsPerThreadgroup:threads];
  [encoder endEncoding];
  [command_buffer commit];
  [command_buffer waitUntilCompleted];
  CheckCommandBuffer(command_buffer);
}

void Dispatch3D(id<MTLCommandQueue> command_queue,
                id<MTLComputePipelineState> pipeline,
                uint32_t width,
                uint32_t height,
                uint32_t depth,
                const std::function<void(id<MTLComputeCommandEncoder>)>& bind) {
  id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
  id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
  [encoder setComputePipelineState:pipeline];
  bind(encoder);
  const MTLSize grid = MTLSizeMake(width, height, depth);
  const MTLSize threads = MTLSizeMake(8, 8, 2);
  [encoder dispatchThreads:grid threadsPerThreadgroup:threads];
  [encoder endEncoding];
  [command_buffer commit];
  [command_buffer waitUntilCompleted];
  CheckCommandBuffer(command_buffer);
}

void Dispatch1D(id<MTLCommandQueue> command_queue,
                id<MTLComputePipelineState> pipeline,
                uint32_t count,
                const std::function<void(id<MTLComputeCommandEncoder>)>& bind) {
  if (count == 0) {
    return;
  }
  id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
  id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
  [encoder setComputePipelineState:pipeline];
  bind(encoder);
  const MTLSize grid = MTLSizeMake(count, 1, 1);
  const MTLSize threads = MTLSizeMake(128, 1, 1);
  [encoder dispatchThreads:grid threadsPerThreadgroup:threads];
  [encoder endEncoding];
  [command_buffer commit];
  [command_buffer waitUntilCompleted];
  CheckCommandBuffer(command_buffer);
}

std::vector<float> GaussianWeights(float sigma, uint32_t* radius) {
  *radius = static_cast<uint32_t>(std::ceil(3.0f * sigma));
  std::vector<float> weights(2 * (*radius) + 1);
  float sum = 0.0f;
  for (int i = -static_cast<int>(*radius); i <= static_cast<int>(*radius); ++i) {
    const float value = std::exp(-0.5f * float(i * i) / (sigma * sigma));
    weights[i + *radius] = value;
    sum += value;
  }
  for (float& weight : weights) {
    weight /= sum;
  }
  return weights;
}

std::vector<float> MakeSyntheticImage(uint32_t width, uint32_t height) {
  std::vector<float> image(width * height);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const bool checker = ((x / 16 + y / 16) % 2) == 0;
      const float dx1 = (float(x) - 0.32f * float(width)) / float(width);
      const float dy1 = (float(y) - 0.38f * float(height)) / float(height);
      const float dx2 = (float(x) - 0.70f * float(width)) / float(width);
      const float dy2 = (float(y) - 0.62f * float(height)) / float(height);
      const float blob1 = std::exp(-(dx1 * dx1 + dy1 * dy1) / 0.004f);
      const float blob2 = std::exp(-(dx2 * dx2 + dy2 * dy2) / 0.008f);
      image[y * width + x] = (checker ? 0.72f : 0.28f) + 0.20f * blob1 - 0.18f * blob2;
    }
  }
  return image;
}

std::vector<float> DownsampleHalf(const std::vector<float>& image,
                                  uint32_t width,
                                  uint32_t height) {
  const uint32_t next_width = std::max(1u, width / 2);
  const uint32_t next_height = std::max(1u, height / 2);
  std::vector<float> next(next_width * next_height);
  for (uint32_t y = 0; y < next_height; ++y) {
    for (uint32_t x = 0; x < next_width; ++x) {
      const uint32_t x0 = std::min(width - 1, 2 * x);
      const uint32_t y0 = std::min(height - 1, 2 * y);
      const uint32_t x1 = std::min(width - 1, x0 + 1);
      const uint32_t y1 = std::min(height - 1, y0 + 1);
      next[y * next_width + x] = 0.25f * (image[y0 * width + x0] + image[y0 * width + x1] +
                                          image[y1 * width + x0] + image[y1 * width + x1]);
    }
  }
  return next;
}

std::array<uint8_t, kDescriptorDim> QuantizeRootSiftDescriptor(const float* descriptor) {
  std::array<float, kDescriptorDim> normalized;
  float l1 = 1.0e-12f;
  for (int i = 0; i < kDescriptorDim; ++i) {
    l1 += std::max(0.0f, descriptor[i]);
  }
  for (int i = 0; i < kDescriptorDim; ++i) {
    normalized[i] = std::sqrt(std::max(0.0f, descriptor[i]) / l1);
  }

  std::array<uint8_t, kDescriptorDim> quantized;
  for (int i = 0; i < kDescriptorDim; ++i) {
    const int scaled = static_cast<int>(std::round(512.0f * normalized[i]));
    quantized[i] = static_cast<uint8_t>(std::clamp(scaled, 0, 255));
  }
  return quantized;
}

std::vector<SpikeFeature> LimitTopScaleFeatures(std::vector<SpikeFeature> features,
                                                size_t max_num_features) {
  if (features.size() <= max_num_features) {
    return features;
  }

  std::partial_sort(features.begin(),
                    features.begin() + max_num_features,
                    features.end(),
                    [](const SpikeFeature& lhs, const SpikeFeature& rhs) {
                      if (lhs.keypoint.scale != rhs.keypoint.scale) {
                        return lhs.keypoint.scale > rhs.keypoint.scale;
                      }
                      return std::fabs(lhs.keypoint.response) > std::fabs(rhs.keypoint.response);
                    });
  features.resize(max_num_features);
  return features;
}

std::vector<SpikeFeature> ExtractOctave(id<MTLDevice> device,
                                        id<MTLCommandQueue> command_queue,
                                        const Pipelines& pipelines,
                                        const std::vector<float>& octave_image,
                                        uint32_t width,
                                        uint32_t height,
                                        uint32_t octave_idx,
                                        const SpikeOptions& options) {
  const uint32_t num_gaussian_levels = options.octave_resolution + 3;
  const uint32_t num_dog_levels = num_gaussian_levels - 1;
  const uint32_t area = width * height;
  const size_t stack_bytes = size_t(num_gaussian_levels) * area * sizeof(float);
  id<MTLBuffer> gaussian_stack = NewBuffer(device, stack_bytes);
  std::memset([gaussian_stack contents], 0, stack_bytes);
  std::memcpy([gaussian_stack contents], octave_image.data(), octave_image.size() * sizeof(float));

  id<MTLBuffer> temp = NewBuffer(device, area * sizeof(float));
  const float k = std::pow(2.0f, 1.0f / float(options.octave_resolution));
  std::vector<float> dog_sigmas(num_dog_levels);
  float prev_sigma = 1.0f;
  for (uint32_t level = 1; level < num_gaussian_levels; ++level) {
    const float sigma = 1.6f * std::pow(k, float(level));
    const float incremental_sigma =
        std::sqrt(std::max(0.01f, sigma * sigma - prev_sigma * prev_sigma));
    prev_sigma = sigma;

    uint32_t radius = 0;
    const std::vector<float> weights = GaussianWeights(incremental_sigma, &radius);
    id<MTLBuffer> weights_buffer =
        NewBuffer(device, weights.size() * sizeof(float), weights.data());
    const MetalBlurParams blur_params{width, height, radius, level - 1, level};

    Dispatch2D(command_queue,
               pipelines.gaussian_blur_h,
               width,
               height,
               ^(id<MTLComputeCommandEncoder> encoder) {
                 [encoder setBuffer:gaussian_stack offset:0 atIndex:0];
                 [encoder setBuffer:temp offset:0 atIndex:1];
                 [encoder setBytes:&blur_params length:sizeof(blur_params) atIndex:2];
                 [encoder setBuffer:weights_buffer offset:0 atIndex:3];
               });
    Dispatch2D(command_queue,
               pipelines.gaussian_blur_v,
               width,
               height,
               ^(id<MTLComputeCommandEncoder> encoder) {
                 [encoder setBuffer:gaussian_stack offset:0 atIndex:0];
                 [encoder setBuffer:temp offset:0 atIndex:1];
                 [encoder setBytes:&blur_params length:sizeof(blur_params) atIndex:2];
                 [encoder setBuffer:weights_buffer offset:0 atIndex:3];
               });
    dog_sigmas[level - 1] = sigma;
  }

  id<MTLBuffer> dog_stack = NewBuffer(device, size_t(num_dog_levels) * area * sizeof(float));
  for (uint32_t level = 0; level < num_dog_levels; ++level) {
    const MetalDogParams dog_params{width, height, level};
    Dispatch2D(command_queue,
               pipelines.dog_difference,
               width,
               height,
               ^(id<MTLComputeCommandEncoder> encoder) {
                 [encoder setBuffer:gaussian_stack offset:0 atIndex:0];
                 [encoder setBuffer:dog_stack offset:0 atIndex:1];
                 [encoder setBytes:&dog_params length:sizeof(dog_params) atIndex:2];
               });
  }

  const uint32_t max_candidates = std::max<uint32_t>(options.max_num_features * 4, 1024);
  id<MTLBuffer> candidates = NewBuffer(device, size_t(max_candidates) * sizeof(MetalCandidate));
  id<MTLBuffer> candidate_count = NewBuffer(device, sizeof(uint32_t));
  *static_cast<uint32_t*>([candidate_count contents]) = 0;
  id<MTLBuffer> sigma_buffer =
      NewBuffer(device, dog_sigmas.size() * sizeof(float), dog_sigmas.data());
  const MetalExtremaParams extrema_params{width,
                                          height,
                                          num_dog_levels,
                                          max_candidates,
                                          options.peak_threshold,
                                          options.edge_threshold};
  Dispatch3D(command_queue,
             pipelines.detect_extrema,
             width,
             height,
             num_dog_levels,
             ^(id<MTLComputeCommandEncoder> encoder) {
               [encoder setBuffer:dog_stack offset:0 atIndex:0];
               [encoder setBuffer:candidates offset:0 atIndex:1];
               [encoder setBuffer:candidate_count offset:0 atIndex:2];
               [encoder setBuffer:sigma_buffer offset:0 atIndex:3];
               [encoder setBytes:&extrema_params length:sizeof(extrema_params) atIndex:4];
             });

  const uint32_t raw_count = *static_cast<uint32_t*>([candidate_count contents]);
  const uint32_t num_candidates = std::min(raw_count, max_candidates);
  if (num_candidates == 0) {
    return {};
  }

  id<MTLBuffer> keypoints = NewBuffer(device, size_t(num_candidates) * sizeof(MetalKeypoint));
  const MetalOrientationParams orientation_params{
      width, height, num_candidates, num_gaussian_levels};
  Dispatch1D(command_queue,
             pipelines.assign_orientation,
             num_candidates,
             ^(id<MTLComputeCommandEncoder> encoder) {
               [encoder setBuffer:gaussian_stack offset:0 atIndex:0];
               [encoder setBuffer:candidates offset:0 atIndex:1];
               [encoder setBuffer:keypoints offset:0 atIndex:2];
               [encoder setBytes:&orientation_params length:sizeof(orientation_params) atIndex:3];
             });

  id<MTLBuffer> descriptors =
      NewBuffer(device, size_t(num_candidates) * kDescriptorDim * sizeof(float));
  const MetalDescriptorParams descriptor_params{width, height, num_candidates, num_gaussian_levels};
  Dispatch1D(command_queue,
             pipelines.make_descriptor,
             num_candidates,
             ^(id<MTLComputeCommandEncoder> encoder) {
               [encoder setBuffer:gaussian_stack offset:0 atIndex:0];
               [encoder setBuffer:keypoints offset:0 atIndex:1];
               [encoder setBuffer:descriptors offset:0 atIndex:2];
               [encoder setBytes:&descriptor_params length:sizeof(descriptor_params) atIndex:3];
             });

  const auto* keypoint_data = static_cast<const MetalKeypoint*>([keypoints contents]);
  const auto* descriptor_data = static_cast<const float*>([descriptors contents]);
  const float octave_scale = float(1u << octave_idx);
  std::vector<SpikeFeature> features;
  features.reserve(num_candidates);
  for (uint32_t i = 0; i < num_candidates; ++i) {
    SpikeFeature feature;
    feature.keypoint = SpikeKeypoint{
        keypoint_data[i].x * octave_scale,
        keypoint_data[i].y * octave_scale,
        keypoint_data[i].scale * octave_scale,
        keypoint_data[i].orientation,
        keypoint_data[i].response,
    };
    feature.descriptor = QuantizeRootSiftDescriptor(descriptor_data + i * kDescriptorDim);
    features.push_back(feature);
  }
  return features;
}

std::vector<SpikeFeature> ExtractMetalSiftSpike(const SpikeOptions& options) {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (device == nil) {
    throw std::runtime_error("No Metal device is available");
  }
  id<MTLCommandQueue> command_queue = [device newCommandQueue];
  if (command_queue == nil) {
    throw std::runtime_error("Failed to create Metal command queue");
  }
  const Pipelines pipelines = NewPipelines(device);

  std::vector<float> octave_image = MakeSyntheticImage(options.width, options.height);
  uint32_t width = options.width;
  uint32_t height = options.height;
  std::vector<SpikeFeature> all_features;
  for (uint32_t octave = 0; octave < options.num_octaves; ++octave) {
    if (width < 24 || height < 24) {
      break;
    }
    std::vector<SpikeFeature> octave_features = ExtractOctave(
        device, command_queue, pipelines, octave_image, width, height, octave, options);
    all_features.insert(all_features.end(), octave_features.begin(), octave_features.end());
    octave_image = DownsampleHalf(octave_image, width, height);
    width = std::max(1u, width / 2);
    height = std::max(1u, height / 2);
  }
  return LimitTopScaleFeatures(std::move(all_features), options.max_num_features);
}

}  // namespace

int main() {
  @autoreleasepool {
    try {
      const SpikeOptions options;
      const std::vector<SpikeFeature> features = ExtractMetalSiftSpike(options);

      std::cout << "metal_sift_spike_prototype\n";
      std::cout << "features: " << features.size() << "\n";
      std::cout << "descriptor_dim: " << kDescriptorDim << "\n";
      std::cout << "descriptor_storage: uint8 RootSIFT-like rows\n";
      const size_t preview_count = std::min<size_t>(features.size(), 5);
      for (size_t i = 0; i < preview_count; ++i) {
        const SpikeKeypoint& keypoint = features[i].keypoint;
        const int descriptor_l1 =
            std::accumulate(features[i].descriptor.begin(), features[i].descriptor.end(), 0);
        std::cout << "kp[" << i << "] x=" << keypoint.x << " y=" << keypoint.y
                  << " scale=" << keypoint.scale << " orientation=" << keypoint.orientation
                  << " response=" << keypoint.response << " descriptor_l1=" << descriptor_l1
                  << "\n";
      }
    } catch (const std::exception& error) {
      std::cerr << "metal_sift_spike_prototype failed: " << error.what() << "\n";
      return 1;
    }
  }
  return 0;
}
