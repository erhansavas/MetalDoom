// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace native_doom {
struct IWADInfo { bool registered = false; bool ultimateData = false; uint32_t lumps = 0; };
bool validateIWAD(const char *path, IWADInfo &info, std::string &error);
struct MIDIEvent { uint64_t tick; uint8_t status, a, b; };
struct Song { std::vector<MIDIEvent> events; uint64_t ticks = 0; };
bool decodeMIDI(const uint8_t *data,size_t size,Song &song,std::string &error);
bool decodeMUS(const uint8_t *data, size_t size, Song &song, std::string &error);
struct Sample { std::vector<float> pcm; uint32_t rate = 0; };
bool decodeSound(const uint8_t *data, size_t size, Sample &sample);
struct Clock {
    uint64_t epoch, excluded = 0, pauseAt = 0;
    bool suspended = false;
    explicit Clock(uint64_t now) : epoch(now) {}
    uint64_t nanos(uint64_t now) const;
    int tics(uint64_t now) const;
    void suspend(bool value, uint64_t now);
};
struct Viewport { double x, y, width, height; };
Viewport viewport(unsigned width, unsigned height, bool integerScale = true);
float deadzone(float value, float threshold);
struct PadState {
    float forward = 0, side = 0, turn = 0;
    bool fire = false, use = false, run = false;
    int weapon = -1;
};
struct PadCommand { int8_t forward, side; int16_t turn; uint8_t buttons; };
PadCommand padCommand(const PadState &state, float threshold, float sensitivity);
void stereoGains(int volume, int separation, float &left, float &right);
}
