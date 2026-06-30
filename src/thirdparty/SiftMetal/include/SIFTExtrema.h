//
//  SIFTExtrema.h
//  SkyLight
//
//  Created by Luke Van In on 2023/01/10.
//

#include <simd/simd.h>

#ifndef SIFTExtrema_h
#define SIFTExtrema_h

struct SIFTExtremaResult {
    int32_t x;
    int32_t y;
    int32_t scale;
    uint32_t linearIndex;
};

struct SIFTExtremaParameters {
    uint32_t outputCapacity;
    uint32_t gridWidth;
    uint32_t gridHeight;
    uint32_t linearStart;
    uint32_t linearEnd;
};

#endif /* SIFTExtrema_h */
