//
//  SIFTOrientation.metal
//  SkyLight
//
//  Created by Luke Van In on 2023/01/07.
//

#include <metal_stdlib>

#include "Common.hpp"
#include "../include/SIFTOrientation.h"

using namespace metal;


constant int kMaxPrincipalOrientations = 4;


int wrapBin(int bin, int bins) {
    while (bin < 0) {
        bin += bins;
    }
    while (bin >= bins) {
        bin -= bins;
    }
    return bin;
}


float orientationFromBin(float bin) {
    const int n = SIFT_ORIENTATION_HISTOGRAM_BINS;
    float t = (bin + 0.5) / (float)n;
    float tau = 2 * M_PI_F;
    float orientation = t * tau;
    if (orientation < 0) {
        orientation += tau;
    }
    if (orientation >= tau) {
        orientation -= tau;
    }
    return orientation;
}


float interpolatePeak(float h1, float h2, float h3) {
    const float denominator = 2 * (h1 + h3 - 2 * h2);
    if (abs(denominator) < 1e-20f) {
        return 0;
    }
    return (h1 - h3) / denominator;
}


void insertPrincipalOrientation(
    float orientation,
    float weight,
    thread int & orientationsCount,
    thread float * orientations,
    thread float * orientationWeights
) {
    int insertIndex = orientationsCount;
    while (insertIndex > 0 &&
           (weight > orientationWeights[insertIndex - 1] ||
            (weight == orientationWeights[insertIndex - 1] &&
             orientation < orientations[insertIndex - 1]))) {
        insertIndex -= 1;
    }

    if (insertIndex >= kMaxPrincipalOrientations) {
        return;
    }

    const int lastIndex = min(orientationsCount, kMaxPrincipalOrientations - 1);
    for (int i = lastIndex; i > insertIndex; i--) {
        orientations[i] = orientations[i - 1];
        orientationWeights[i] = orientationWeights[i - 1];
    }

    orientations[insertIndex] = orientation;
    orientationWeights[insertIndex] = weight;
    orientationsCount = min(orientationsCount + 1, kMaxPrincipalOrientations);
}
    
    
void getPrincipalOrientations(
    thread float * histogram,
    float orientationThreshold,
    thread int & orientationsCount,
    thread float * orientations
) {
    const int bins = SIFT_ORIENTATION_HISTOGRAM_BINS;
    
    float maximum = 0;
    for (int i = 0; i < bins; i++) {
        maximum = max(maximum, histogram[i]);
    }
    
    const float threshold = orientationThreshold * maximum;
    
    orientationsCount = 0;
    float orientationWeights[kMaxPrincipalOrientations];
    for (int i = 0; i < kMaxPrincipalOrientations; i++) {
        orientationWeights[i] = 0;
    }
    
    for (int i = 0; i < bins; i++) {
        float hm = histogram[((i - 1) + bins) % bins];
        float h0 = histogram[i];
        float hp = histogram[(i + 1) % bins];
        if ((h0 > threshold) && (h0 > hm) && (h0 > hp)) {
            float offset = interpolatePeak(hm, h0, hp);
            float orientation = orientationFromBin((float)i + offset);
            insertPrincipalOrientation(
                orientation,
                h0,
                orientationsCount,
                orientations,
                orientationWeights
            );
        }
    }
}


void smoothHistogram(
    thread float * histogram,
    int iterations
) {
    const int n = SIFT_ORIENTATION_HISTOGRAM_BINS;
    float temp[n];
    for (int j = 0; j < iterations; j++) {
        for (int i = 0; i < n; i++) {
            temp[i] = histogram[i];
        }
        for (int i = 0; i < n; i++) {
            float h0 = temp[((i - 1) + n) % n];
            float h1 = temp[i];
            float h2 = temp[(i + 1) % n];
            float v = (h0 + h1 + h2) / 3.0;
            histogram[i] = v;
        }
    }
}


void getOrientationsHistogram(
    texture2d_array<float, access::read> g,
    int absoluteX,
    int absoluteY,
    int scale,
    float keypointSigma,
    float delta,
    float lambda,
    thread float * histogram
) {
    const int bins = SIFT_ORIENTATION_HISTOGRAM_BINS;
    const float x = (float)absoluteX / delta;
    const float y = (float)absoluteY / delta;
    const int xi = (int)floor(x + 0.5);
    const int yi = (int)floor(y + 0.5);
    const float sigma = keypointSigma / delta;

    const float sigmaWindow = lambda * sigma;
    const float exponentDenominator = 2.0 * sigmaWindow * sigmaWindow;
    const float tau = 2 * M_PI_F;
    
    const int radius = max((int)floor(3.0 * sigmaWindow), 1);
    const int minX = max(-radius, -xi);
    const int maxX = min(radius, (int)g.get_width() - 1 - xi);
    const int minY = max(-radius, -yi);
    const int maxY = min(radius, (int)g.get_height() - 1 - yi);

    for (int j = minY; j <= maxY; j++) {
        for (int i = minX; i <= maxX; i++) {
            const float dx = (float)(xi + i) - x;
            const float dy = (float)(yi + j) - y;
            const float r2 = dx * dx + dy * dy;
            if (r2 >= (float)(radius * radius) + 0.6) {
                continue;
            }

            // Gaussian weighting
            float w = exp(-r2 / exponentDenominator);

            // Gradient orientation
            float2 gradient = g.read(ushort2(xi + i, yi + j), scale).rg;
            float orientation = gradient.x;
            float magnitude = gradient.y;
            
            // Add to histogram
            float fbin = (float)bins * orientation / tau;
            int bin = (int)floor(fbin - 0.5);
            float binWeight = fbin - (float)bin - 0.5;

            float m = w * magnitude;
            
            histogram[wrapBin(bin, bins)] += (1.0 - binWeight) * m;
            histogram[wrapBin(bin + 1, bins)] += binWeight * m;
        }
    }
}



kernel void siftOrientation(
    device SIFTOrientationResult * results [[buffer(0)]],
    device SIFTOrientationKeypoint * keypoints [[buffer(1)]],
    device SIFTOrientationParameters & parameters [[buffer(2)]],
    texture2d_array<float, access::read> gradientTextures [[texture(0)]],
    ushort gid [[thread_position_in_grid]]
) {
    const int bins = SIFT_ORIENTATION_HISTOGRAM_BINS;
    const SIFTOrientationKeypoint keypoint = keypoints[gid];
    SIFTOrientationResult result;
    result.keypoint = keypoint.index;
    result.count = 0;
    for (int i = 0; i < bins; i++) {
        result.orientations[i] = 0;
    }
    
    float histogram[bins];
    for (int i = 0; i < bins; i++) {
        histogram[i] = 0;
    }
    
    getOrientationsHistogram(
        gradientTextures,
        keypoint.absoluteX,
        keypoint.absoluteY,
        keypoint.scale,
        keypoint.sigma,
        parameters.delta,
        parameters.lambda,
        histogram
    );
    smoothHistogram(histogram, 6);
    getPrincipalOrientations(
        histogram,
        parameters.orientationThreshold,
        result.count,
        result.orientations
    );
    results[gid] = result;
}
