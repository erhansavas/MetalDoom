// SPDX-License-Identifier: GPL-2.0-only
#include "Portable.hpp"
#include "StickCurve.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sys/stat.h>

namespace native_doom {
static uint16_t le16(const uint8_t *p) { return p[0] | (uint16_t(p[1]) << 8); }
static uint32_t le32(const uint8_t *p) { return le16(p) | (uint32_t(le16(p+2)) << 16); }
// Independent decoder of the published MUS wire format; no DMX or port code.
bool decodeMUS(const uint8_t *d, size_t size, Song &song, std::string &error) {
    song = {};
    auto fail = [&](const char *s) { song = {}; error = s; return false; };
    if (size < 16 || memcmp(d, "MUS\x1a", 4)) return fail("Invalid MUS header");
    const size_t len=le16(d+4), start=le16(d+6), instruments=le16(d+12);
    if (start < 16+instruments*2 || start > size || len > size-start || !len || le16(d+8)>15)
        return fail("Invalid MUS score bounds");
    size_t p=start, end=start+len;
    uint8_t velocities[16]; std::fill_n(velocities,16,64);
    int channels[16]; std::fill_n(channels,16,-1); channels[15]=9;
    int next=0;
    uint64_t tick=0;
    const uint8_t controllers[] = {0,0,1,7,10,11,91,93,64,67,120,123,126,127,121};
    auto byte = [&](uint8_t &v) { if (p>=end) return false; v=d[p++]; return true; };
    while (p<end) {
        uint8_t desc=d[p++], type=(desc>>4)&7, mus=desc&15, a=0, b=0, status=0;
        if (type==6) { song.ticks=tick; return true; }
        if (channels[mus]<0) { if (next==9) ++next; if(next>15) return fail("Too many MUS channels"); channels[mus]=next++; }
        const uint8_t ch=channels[mus];
        if (!byte(a)) return fail("Truncated MUS event");
        switch (type) {
        case 0: if(a>127) return fail("Invalid note"); status=0x80|ch; b=64; break;
        case 1:
            if(a&128) { if(!byte(b) || b>127) return fail("Invalid note velocity"); velocities[mus]=b; }
            a &=127; b=velocities[mus]; status=0x90|ch; break;
        case 2: { uint16_t pitch=uint16_t(a)<<6; a=pitch&127; b=pitch>>7; status=0xe0|ch; break; }
        case 3:
            if (a<10 || a>14) return fail("Invalid system event");
            b=(a==12)?uint8_t(le16(d+8)+1):0; a=controllers[a]; status=0xb0|ch; break;
        case 4:
            if(a>9 || !byte(b) || b>127) return fail("Invalid controller event");
            if(a==0) { status=0xc0|ch; a=b; b=0; }
            else { status=0xb0|ch; a=controllers[a]; }
            break;
        default: return fail("Unsupported MUS event");
        }
        song.events.push_back({tick,status,a,b});
        if (desc&128) {
            uint64_t delay=0; unsigned n=0; uint8_t v;
            do {
                if(!byte(v) || ++n>5) return fail("Invalid MUS delay");
                delay=delay*128+(v&127);
            } while(v&128);
            tick+=delay;
            if(tick>uint64_t(140)*60*60*24) return fail("MUS score exceeds one day");
        }
    }
    return fail("Missing MUS end event");
}

bool decodeSound(const uint8_t *d, size_t size, Sample &s) {
    s={};
    if(size<8 || le16(d)!=3) return false;
    uint32_t rate=le16(d+2), n=le32(d+4);
    if(rate<1000 || rate>48000 || n<=32 || n>size-8) return false;
    s.rate=rate;
    // DMX sample guards are part of the file, not the audible sample.
    s.pcm.reserve(n-32);
    for(uint32_t i=16;i<n-16;++i) s.pcm.push_back((int(d[8+i])-128)/128.0f);
    return true;
}
uint64_t Clock::nanos(uint64_t now) const { return (suspended?pauseAt:now)-epoch-excluded; }
int Clock::tics(uint64_t now) const { uint64_t n=nanos(now); return int((n/1000000000)*35+(n%1000000000)*35/1000000000); }
void Clock::suspend(bool value,uint64_t now) {
    if(value==suspended) return;
    if(value) pauseAt=now; else excluded+=now-pauseAt;
    suspended=value;
}
Viewport viewport(unsigned w,unsigned h,bool integerScale) {
    // 320x200 samples displayed at the original 4:3 aspect (320x240).
    double scale=std::min(w/320.0,h/240.0);
    if(integerScale && scale>=1) scale=std::floor(scale);
    return {(w-320*scale)/2,(h-240*scale)/2,320*scale,240*scale};
}
float deadzone(float v,float t) {
    if(!std::isfinite(v)) return 0;
    if(!std::isfinite(t)) t=.18f;
    t=std::clamp(t,0.0f,.95f);
    v=std::clamp(v,-1.0f,1.0f);
    return std::fabs(v)<=t?0:std::copysign((std::fabs(v)-t)/(1-t),v);
}
PadCommand padCommand(const PadState &s,float threshold,float sensitivity) {
    PadCommand c{};
    float forward=deadzone(s.forward,threshold),side=deadzone(s.side,threshold);
    // Keep diagonal analog movement at the same maximum speed as cardinal
    // movement. The original command limits each axis independently, which
    // otherwise makes a full diagonal about 41% faster.
    float magnitude=std::hypot(forward,side);
    if(magnitude>1) { forward/=magnitude;side/=magnitude; }
    c.forward=std::lround(stick::moveRate(forward,25,50,s.run));
    c.side=std::lround(stick::moveRate(side,24,40,s.run));
    if(!std::isfinite(sensitivity)) sensitivity=1;
    sensitivity=std::clamp(sensitivity,0.0f,2.0f);
    // Full tilt at sensitivity 1 turns 4 degrees per tic (280 deg/s); the
    // sweet-spot curve keeps the inner range precise for aiming.
    float turn=deadzone(s.turn,threshold);
    c.turn=std::lround(-std::copysign(stick::turnCurve(std::fabs(turn)),turn)*1456*sensitivity);
    c.buttons=(s.fire?1:0)|(s.use?2:0);
    if(s.weapon>=0 && s.weapon<8) c.buttons|=4|(s.weapon<<3);
    return c;
}
void stereoGains(int vol,int sep,float &left,float &right) {
    vol=std::clamp(vol,0,15); sep=std::clamp(sep,0,255);
    int s=sep+1;
    left=(vol-((vol*s*s)>>16))/15.0f;
    s-=257; right=(vol-((vol*s*s)>>16))/15.0f;
}
}
