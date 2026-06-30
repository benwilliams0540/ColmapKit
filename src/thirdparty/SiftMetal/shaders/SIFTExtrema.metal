//
//  SIFTExtrema.metal
//  SkyLight
//
//  Created by Luke Van In on 2023/01/07.
//

#include <metal_stdlib>

#include "../include/SIFTExtrema.h"

using namespace metal;


constant uint kMaxThreadgroupExtrema = 1024;

constant int3 neighborOffsets[] = {
    int3(-1, -1, -1),
    int3( 0, -1, -1),
    int3(+1, -1, -1),
    int3(-1,  0, -1),
    int3( 0,  0, -1),
    int3(+1,  0, -1),
    int3(-1, +1, -1),
    int3( 0, +1, -1),
    int3(+1, +1, -1),
    
    int3(-1, -1,  0),
    int3( 0, -1,  0),
    int3(+1, -1,  0),
    int3(-1,  0,  0),
    
    int3(+1,  0,  0),
    int3(-1, +1,  0),
    int3( 0, +1,  0),
    int3(+1, +1,  0),
    
    int3(-1, -1, +1),
    int3( 0, -1, +1),
    int3(+1, -1, +1),
    int3(-1,  0, +1),
    int3( 0,  0, +1),
    int3(+1,  0, +1),
    int3(-1, +1, +1),
    int3( 0, +1, +1),
    int3(+1, +1, +1),
};


static inline float fetch(
    texture2d_array<float, access::read> texture [[texture(0)]],
    const int2 g,
    const int s,
    const int i
) {
    const int3 neighborOffset = neighborOffsets[i];
    const int2 neighborDelta = g + neighborOffset.xy;
    const int textureIndex = s + neighborOffset.z;
    const float neighborValue = texture.read((ushort2)neighborDelta, (short)textureIndex).r;
    return neighborValue;
}


kernel void siftExtremaList(
    device SIFTExtremaResult * output [[buffer(0)]],
    device atomic_uint * outputCount [[buffer(1)]],
    device const SIFTExtremaParameters & parameters [[buffer(2)]],
    texture2d_array<float, access::read> inputTexture [[texture(0)]],
    ushort3 gid [[thread_position_in_grid]],
    ushort3 lid [[thread_position_in_threadgroup]],
    ushort tid [[thread_index_in_threadgroup]]
) {
    // Thread group runs [0...output.width - 2][0...output.height - 2]
    threadgroup SIFTExtremaResult localResults[kMaxThreadgroupExtrema];
    threadgroup atomic_uint localCount;
    atomic_store_explicit(&localCount, 0u, memory_order_relaxed);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    
    const int2 g = (int2)gid.xy + 1;
    const int s = (int)gid.z + 1;
    const uint linearIndex =
        (((uint)gid.z * parameters.gridHeight) + (uint)gid.y) *
            parameters.gridWidth +
        (uint)gid.x;
    const bool inRange =
        linearIndex >= parameters.linearStart && linearIndex < parameters.linearEnd;

    if (inRange) {
        const float value = inputTexture.read((ushort2)g, (ushort)s).r;

        float minimum = +INFINITY;
        float maximum = -INFINITY;

        for (int i = 0; i < 26; i++) {
            float neighborValue = fetch(inputTexture, g, s, i);
            minimum = min(minimum, neighborValue);
            maximum = max(maximum, neighborValue);
        }

        if ((value < minimum) || (value > maximum)) {
            const uint i = atomic_fetch_add_explicit(&localCount, 1u, memory_order_relaxed);
            if (i < kMaxThreadgroupExtrema) {
                SIFTExtremaResult result;
                result.x = g.x;
                result.y = g.y;
                result.scale = s;
                result.linearIndex = linearIndex;
                localResults[i] = result;
            }
        }
    }
    
    // Copy local results to output
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (tid == 0) {
        const uint count = min(
            atomic_load_explicit(&localCount, memory_order_relaxed),
            kMaxThreadgroupExtrema
        );
        if (count > 0) {
            const uint b = atomic_fetch_add_explicit(outputCount, count, memory_order_relaxed);
            if (b < parameters.outputCapacity) {
                const uint copyCount = min(count, parameters.outputCapacity - b);
                for (uint i = 0; i < copyCount; i++) {
                    output[b + i] = localResults[i];
                }
            }
        }
    }
}


kernel void siftExtrema(
    texture2d_array<float, access::write> outputTexture [[texture(0)]],
    texture2d_array<float, access::read> inputTexture [[texture(1)]],
    ushort3 gid [[thread_position_in_grid]],
    ushort3 threadPositionInThreadGroup [[thread_position_in_threadgroup]],
    ushort3 threadsPerThreadGroup [[threads_per_threadgroup]]
) {
    // Thread group runs [0...output.width - 2][0...output.height - 2]
    
    const int2 g = int2(gid.xy) + 1;
    const int s = (int)gid.z + 1;
    const float value = inputTexture.read((ushort2)g, (ushort)s).r;
    
    float minValue = +INFINITY;
    float maxValue = -INFINITY;

    for (int i = 0; i < 26; i++) {
        const int3 neighborOffset = neighborOffsets[i];
        const int2 coordinate = g + neighborOffset.xy;
        const int textureIndex = s + neighborOffset.z;
        float neighborValue = inputTexture.read((ushort2)coordinate, (ushort)textureIndex).r;

        minValue = min(minValue, neighborValue);
        maxValue = max(maxValue, neighborValue);
    }
    
    float result = 0;
    
    if ((value < minValue) || (value > maxValue)) {
        result = 1;
    }

    outputTexture.write(float4(result, 0, 0, 1), gid.xy + 1, gid.z);
}
