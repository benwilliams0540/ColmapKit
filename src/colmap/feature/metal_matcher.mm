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

#if defined(__APPLE__) && defined(COLMAP_METAL_ENABLED)

#include <algorithm>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

namespace colmap {
namespace {

constexpr NSUInteger kThreadsPerThreadgroup = 64;
constexpr NSUInteger kTrainTileSize = 32;

static_assert(sizeof(MetalSiftTop2Match) == 4 * sizeof(uint32_t),
              "MetalSiftTop2Match must stay Metal ABI-compatible");

struct MetalSiftKernelParams {
  uint32_t num_query_descriptors = 0;
  uint32_t num_train_descriptors = 0;
  uint32_t train_tile_size = 0;
  uint32_t threadgroup_size = 0;
};

NSString* MetalSiftKernelSource() {
  return [NSString stringWithUTF8String:R"METAL(
#include <metal_stdlib>
using namespace metal;

constant uint kDescriptorDim = 128;
constant uint kInvalidIndex = 0xffffffffu;

struct Top2Match {
  uint best_train_idx;
  uint second_best_train_idx;
  uint best_distance_squared;
  uint second_best_distance_squared;
};

struct Params {
  uint num_query_descriptors;
  uint num_train_descriptors;
  uint train_tile_size;
  uint threadgroup_size;
};

inline void ConsiderDistance(const uint distance_squared,
                             const uint train_idx,
                             thread Top2Match& top2) {
  if (distance_squared < top2.best_distance_squared ||
      (distance_squared == top2.best_distance_squared &&
       train_idx < top2.best_train_idx)) {
    if (train_idx != top2.best_train_idx) {
      top2.second_best_train_idx = top2.best_train_idx;
      top2.second_best_distance_squared = top2.best_distance_squared;
    }
    top2.best_train_idx = train_idx;
    top2.best_distance_squared = distance_squared;
    return;
  }

  if (train_idx == top2.best_train_idx) {
    return;
  }

  if (distance_squared < top2.second_best_distance_squared ||
      (distance_squared == top2.second_best_distance_squared &&
       train_idx < top2.second_best_train_idx)) {
    top2.second_best_train_idx = train_idx;
    top2.second_best_distance_squared = distance_squared;
  }
}

kernel void SiftTop2Kernel(device const uchar* query_descriptors [[buffer(0)]],
                           device const uchar* train_descriptors [[buffer(1)]],
                           device Top2Match* top2_matches [[buffer(2)]],
                           constant Params& params [[buffer(3)]],
                           threadgroup uchar* train_tile [[threadgroup(0)]],
                           uint query_idx [[thread_position_in_grid]],
                           uint local_idx [[thread_index_in_threadgroup]]) {
  Top2Match top2;
  top2.best_train_idx = kInvalidIndex;
  top2.second_best_train_idx = kInvalidIndex;
  top2.best_distance_squared = 0xffffffffu;
  top2.second_best_distance_squared = 0xffffffffu;

  for (uint tile_start = 0; tile_start < params.num_train_descriptors;
       tile_start += params.train_tile_size) {
    const uint tile_count = min(params.train_tile_size,
                                params.num_train_descriptors - tile_start);
    const uint tile_bytes = tile_count * kDescriptorDim;

    for (uint offset = local_idx; offset < tile_bytes;
         offset += params.threadgroup_size) {
      train_tile[offset] =
          train_descriptors[tile_start * kDescriptorDim + offset];
    }

    threadgroup_barrier(mem_flags::mem_threadgroup);

    if (query_idx < params.num_query_descriptors) {
      device const uchar* query =
          query_descriptors + query_idx * kDescriptorDim;
      for (uint tile_idx = 0; tile_idx < tile_count; ++tile_idx) {
        uint distance_squared = 0;
        threadgroup const uchar* train =
            train_tile + tile_idx * kDescriptorDim;
        for (uint dim = 0; dim < kDescriptorDim; ++dim) {
          const int diff = int(query[dim]) - int(train[dim]);
          distance_squared += uint(diff * diff);
        }
        ConsiderDistance(distance_squared, tile_start + tile_idx, top2);
      }
    }

    threadgroup_barrier(mem_flags::mem_threadgroup);
  }

  if (query_idx < params.num_query_descriptors) {
    top2_matches[query_idx] = top2;
  }
}
)METAL"];
}

struct MetalSiftKernel {
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> command_queue = nil;
  id<MTLComputePipelineState> pipeline = nil;
  bool available = false;

  MetalSiftKernel() {
    device = MTLCreateSystemDefaultDevice();
    if (device == nil) {
      return;
    }

    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:MetalSiftKernelSource()
                                                  options:nil
                                                    error:&error];
    if (library == nil) {
      return;
    }

    id<MTLFunction> function = [library newFunctionWithName:@"SiftTop2Kernel"];
    if (function == nil) {
#if !__has_feature(objc_arc)
      [library release];
#endif
      return;
    }

    pipeline = [device newComputePipelineStateWithFunction:function error:&error];
    command_queue = [device newCommandQueue];

#if !__has_feature(objc_arc)
    [function release];
    [library release];
#endif

    available = pipeline != nil && command_queue != nil;
  }

  ~MetalSiftKernel() {
#if !__has_feature(objc_arc)
    [pipeline release];
    [command_queue release];
    [device release];
#endif
  }
};

MetalSiftKernel& SharedMetalSiftKernel() {
  static MetalSiftKernel kernel;
  return kernel;
}

NSUInteger ThreadgroupSize(const MetalSiftKernel& kernel) {
  return std::min<NSUInteger>(kThreadsPerThreadgroup,
                              kernel.pipeline.maxTotalThreadsPerThreadgroup);
}

}  // namespace

bool IsMetalSiftMatcherAvailable() {
  @autoreleasepool {
    return SharedMetalSiftKernel().available;
  }
}

std::string GetMetalSiftMatcherDeviceName() {
  @autoreleasepool {
    MetalSiftKernel& kernel = SharedMetalSiftKernel();
    if (kernel.device == nil) {
      return {};
    }
    NSString* name = [kernel.device name];
    const char* utf8 = name == nil ? nullptr : [name UTF8String];
    return utf8 == nullptr ? std::string() : std::string(utf8);
  }
}

bool ComputeMetalSiftTop2Matches(const FeatureDescriptors& query_descriptors,
                                 const FeatureDescriptors& train_descriptors,
                                 std::vector<MetalSiftTop2Match>* top2_matches) {
  @autoreleasepool {
    if (top2_matches == nullptr) {
      return false;
    }

    MetalSiftKernel& kernel = SharedMetalSiftKernel();
    if (!kernel.available) {
      return false;
    }

    top2_matches->assign(static_cast<size_t>(query_descriptors.data.rows()), MetalSiftTop2Match());
    if (query_descriptors.data.rows() == 0 || train_descriptors.data.rows() == 0) {
      return true;
    }

    const NSUInteger num_query_descriptors = static_cast<NSUInteger>(query_descriptors.data.rows());
    const NSUInteger num_train_descriptors = static_cast<NSUInteger>(train_descriptors.data.rows());
    const NSUInteger descriptor_bytes = num_query_descriptors * kMetalSiftDescriptorDim;
    const NSUInteger train_descriptor_bytes = num_train_descriptors * kMetalSiftDescriptorDim;
    const NSUInteger top2_bytes = top2_matches->size() * sizeof(MetalSiftTop2Match);
    const NSUInteger threadgroup_size = ThreadgroupSize(kernel);

    id<MTLBuffer> query_buffer = [kernel.device newBufferWithBytes:query_descriptors.data.data()
                                                            length:descriptor_bytes
                                                           options:MTLResourceStorageModeShared];
    id<MTLBuffer> train_buffer = [kernel.device newBufferWithBytes:train_descriptors.data.data()
                                                            length:train_descriptor_bytes
                                                           options:MTLResourceStorageModeShared];
    id<MTLBuffer> output_buffer = [kernel.device newBufferWithLength:top2_bytes
                                                             options:MTLResourceStorageModeShared];
    MetalSiftKernelParams params;
    params.num_query_descriptors = static_cast<uint32_t>(num_query_descriptors);
    params.num_train_descriptors = static_cast<uint32_t>(num_train_descriptors);
    params.train_tile_size = static_cast<uint32_t>(kTrainTileSize);
    params.threadgroup_size = static_cast<uint32_t>(threadgroup_size);
    id<MTLBuffer> params_buffer = [kernel.device newBufferWithBytes:&params
                                                             length:sizeof(params)
                                                            options:MTLResourceStorageModeShared];

    if (query_buffer == nil || train_buffer == nil || output_buffer == nil ||
        params_buffer == nil) {
#if !__has_feature(objc_arc)
      [query_buffer release];
      [train_buffer release];
      [output_buffer release];
      [params_buffer release];
#endif
      return false;
    }

    id<MTLCommandBuffer> command_buffer = [kernel.command_queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
    [encoder setComputePipelineState:kernel.pipeline];
    [encoder setBuffer:query_buffer offset:0 atIndex:0];
    [encoder setBuffer:train_buffer offset:0 atIndex:1];
    [encoder setBuffer:output_buffer offset:0 atIndex:2];
    [encoder setBuffer:params_buffer offset:0 atIndex:3];
    [encoder setThreadgroupMemoryLength:kTrainTileSize * kMetalSiftDescriptorDim atIndex:0];

    const NSUInteger num_threadgroups =
        (num_query_descriptors + threadgroup_size - 1) / threadgroup_size;
    [encoder dispatchThreadgroups:MTLSizeMake(num_threadgroups, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(threadgroup_size, 1, 1)];
    [encoder endEncoding];

    [command_buffer commit];
    [command_buffer waitUntilCompleted];

    const bool succeeded = command_buffer.status == MTLCommandBufferStatusCompleted;
    if (succeeded) {
      const void* output_data = [output_buffer contents];
      std::copy_n(static_cast<const MetalSiftTop2Match*>(output_data),
                  top2_matches->size(),
                  top2_matches->data());
    }

#if !__has_feature(objc_arc)
    [query_buffer release];
    [train_buffer release];
    [output_buffer release];
    [params_buffer release];
#endif

    return succeeded;
  }
}

}  // namespace colmap

#endif  // __APPLE__ && COLMAP_METAL_ENABLED
