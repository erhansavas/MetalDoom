// SPDX-License-Identifier: GPL-2.0-only
#include "Portable.hpp"
#include <algorithm>
#include <cstring>
namespace native_doom {
bool decodeMIDI(const uint8_t *data,size_t size,Song &song,std::string &error) {
    song={};auto fail=[&](const char *why){song={};error=why;return false;};
    auto be16=[](const uint8_t *p){return unsigned(p[0])*256+p[1];};
    auto be32=[](const uint8_t *p){return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];};
    if(size<14 || size>16*1024*1024 || memcmp(data,"MThd",4))return fail("Invalid MIDI header.");
    uint32_t header=be32(data+4);if(header<6 || header>size-8)return fail("Invalid MIDI header bounds.");
    unsigned format=be16(data+8),tracks=be16(data+10),division=be16(data+12);
    if(format>1 || !tracks || tracks>256 || (format==0 && tracks!=1) || !division || division&0x8000)return fail("Unsupported MIDI track or timing format.");
    struct Raw {uint64_t tick;uint32_t tempo;uint8_t status,a,b;};std::vector<Raw> raw;
    size_t pos=8+header;uint64_t endTick=0;
    for(unsigned track=0;track<tracks;++track) {
        if(pos+8>size || memcmp(data+pos,"MTrk",4))return fail("Missing MIDI track.");
        uint32_t length=be32(data+pos+4);pos+=8;if(length>size-pos)return fail("Truncated MIDI track.");
        size_t end=pos+length;uint64_t tick=0;uint8_t running=0;
        auto vlq=[&](uint32_t &value){value=0;for(int i=0;i<4;++i){if(pos>=end)return false;uint8_t b=data[pos++];value=(value<<7)|(b&127);if(!(b&128))return true;}return false;};
        while(pos<end) {
            uint32_t delta;if(!vlq(delta) || tick+delta>UINT32_MAX)return fail("Invalid MIDI event time.");tick+=delta;
            if(pos>=end)return fail("Missing MIDI event.");uint8_t status=data[pos];
            if(status&128){++pos;if(status<0xf0)running=status;else running=0;}else {if(!running)return fail("Missing MIDI running status.");status=running;}
            if(status==0xff) {
                if(pos>=end)return fail("Truncated MIDI meta event.");uint8_t type=data[pos++];uint32_t n;if(!vlq(n) || n>end-pos)return fail("Invalid MIDI meta bounds.");
                if(type==0x51){if(n!=3)return fail("Invalid MIDI tempo.");uint32_t tempo=uint32_t(data[pos])<<16|uint32_t(data[pos+1])<<8|data[pos+2];if(!tempo)return fail("Zero MIDI tempo.");raw.push_back({tick,tempo,0,0,0});}
                pos+=n;if(type==0x2f){if(n)return fail("Invalid MIDI end marker.");pos=end;break;}
            } else if(status==0xf0 || status==0xf7) {uint32_t n;if(!vlq(n) || n>end-pos)return fail("Invalid MIDI system event.");pos+=n;}
            else {
                if(status>=0xf0)return fail("Unsupported MIDI system status.");unsigned n=(status&0xf0)==0xc0 || (status&0xf0)==0xd0?1:2;
                if(n>end-pos)return fail("Truncated MIDI channel event.");uint8_t a=data[pos++],b=n==2?data[pos++]:0;if(a>127 || b>127)return fail("Invalid MIDI channel data.");raw.push_back({tick,0,status,a,b});
            }
            if(raw.size()>1000000)return fail("Too many MIDI events.");
        }
        endTick=std::max(endTick,tick);
    }
    std::stable_sort(raw.begin(),raw.end(),[](const Raw &a,const Raw &b){return a.tick<b.tick;});
    uint64_t tick=0,scaledTime=0;uint32_t tempo=500000;
    for(const auto &e:raw) {scaledTime+=(e.tick-tick)*tempo;tick=e.tick;uint64_t at=scaledTime*140/(uint64_t(division)*1000000);if(at>140*86400)return fail("MIDI duration exceeds one day.");if(e.tempo)tempo=e.tempo;else song.events.push_back({at,e.status,e.a,e.b});}
    scaledTime+=(endTick-tick)*tempo;song.ticks=(scaledTime*140+uint64_t(division)*1000000-1)/(uint64_t(division)*1000000);
    if(song.ticks>140*86400)return fail("MIDI duration exceeds one day.");return true;
}
}
