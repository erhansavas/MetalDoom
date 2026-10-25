// SPDX-License-Identifier: GPL-2.0-only
// Adjacent original COLORMAP indices and their blend weight, independent of simulation.
#include "Native.h"
#include <vector>
#include <algorithm>
namespace { std::vector<uint16_t> pixels(320*200); }
uint16_t *Native_LightingBegin(unsigned width,unsigned height) {
    pixels.resize(size_t(width)*height);
    std::fill(pixels.begin(),pixels.end(),0);
    return pixels.data();
}
const uint16_t *Native_LightingPixels(unsigned width,unsigned height) {
    size_t size=size_t(width)*height;
    if(pixels.size()!=size) pixels.assign(size,0);
    return pixels.data();
}
