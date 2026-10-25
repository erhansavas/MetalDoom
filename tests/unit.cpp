// SPDX-License-Identifier: GPL-2.0-only
#include "Portable.hpp"
#include "Content.hpp"
#include "Feedback.hpp"
#include "Saves.hpp"
#include "RenderScale.hpp"
#include <filesystem>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <random>
using namespace native_doom;
static void put16(std::vector<uint8_t> &v,size_t p,unsigned n) { v[p]=n; v[p+1]=n>>8; }
static void put32(std::vector<uint8_t> &v,size_t p,unsigned n) { put16(v,p,n); put16(v,p+2,n>>16); }
static std::vector<uint8_t> mus(std::vector<uint8_t> score) {
    std::vector<uint8_t> d(16); memcpy(d.data(),"MUS\x1a",4); put16(d,4,score.size()); put16(d,6,16); put16(d,8,2);
    d.insert(d.end(),score.begin(),score.end()); return d;
}
int main() {
    std::string error;
    // SMF format 0: tempo, running-status note, second tempo, note-off.
    std::vector<uint8_t> midi={'M','T','h','d',0,0,0,6,0,0,0,1,0,96,'M','T','r','k',0,0,0,24,0,0xff,0x51,3,7,0xa1,0x20,0,0x90,60,100,96,60,0,0,0xff,0x51,3,15,0x42,0x40,96,0xff,0x2f,0};
    midi[21]=midi.size()-22;Song midiSong;
    assert(decodeMIDI(midi.data(),midi.size(),midiSong,error));
    assert(midiSong.events.size()==2 && midiSong.events[0].tick==0 && midiSong.events[1].tick==70 && midiSong.ticks==210);
    for(size_t n=0;n<midi.size();++n)assert(!decodeMIDI(midi.data(),n,midiSong,error));
    midi[13]=0;assert(!decodeMIDI(midi.data(),midi.size(),midiSong,error));
    Clock clock(123); assert(clock.tics(123)==0); assert(clock.tics(1000000123)==35);
    assert(clock.tics(28571551+123)==1);
    clock.suspend(true,1000000123); assert(clock.tics(100000000123)==35);
    clock.suspend(false,100000000123); assert(clock.tics(101000000123)==70);
    auto v=viewport(1920,1080); assert(v.width==1280 && v.height==960 && v.x==320 && v.y==60);
    v=viewport(640,480); assert(v.width==640 && v.height==480 && v.x==0);
    PadState s; s.forward=-1; s.side=1; s.turn=1; s.fire=true; s.run=true;
    auto cmd=padCommand(s,.18,1); assert(cmd.forward==-35 && cmd.side==28 && cmd.turn==-1456 && cmd.buttons==1);
    cmd=padCommand(s,.18,2); assert(cmd.turn==-2912);
    s.forward=.95f; s.side=0; s.run=false; cmd=padCommand(s,0,1); assert(cmd.forward==38); // past 90% eases into running
    s={}; s.forward=.1; s.weapon=7; cmd=padCommand(s,.18,1); assert(cmd.forward==0 && cmd.buttons==(4|(7<<3)));
    assert(deadzone(.18,.18)==0 && deadzone(-1,.18)==-1);
    assert(deadzone(std::numeric_limits<float>::quiet_NaN(),.18f)==0);
    float l,r; stereoGains(15,128,l,r); assert(l>0 && r>0 && std::abs(l-r)<.1);
    stereoGains(15,0,l,r); assert(l>.9 && r==0);
    auto d=mus({0x90,0xbc,100,0x81,0x0c, 0x80,60,0, 0x60});
    Song song; assert(decodeMUS(d.data(),d.size(),song,error));
    assert(song.events.size()==2 && song.events[0].status==0x90 && song.events[0].a==60 && song.events[0].b==100);
    assert(song.events[1].tick==140 && song.events[1].status==0x80 && song.ticks==140);
    d=mus({0x1f,0xbc,90,0x4f,0,40,0xaf,128,1,0x60});
    assert(decodeMUS(d.data(),d.size(),song,error)); assert(song.events[0].status==0x99 && song.events[1].status==0xc9);
    assert(song.events[2].status==0xe9 && song.events[2].a==0 && song.events[2].b==64);
    for(size_t n=0;n<d.size();++n) assert(!decodeMUS(d.data(),n,song,error));
    d=mus({0xf0,0}); assert(!decodeMUS(d.data(),d.size(),song,error));
    std::vector<uint8_t> sound(8+36,128); put16(sound,0,3); put16(sound,2,11025); put32(sound,4,36);
    sound[24]=0; sound[25]=255; Sample sample; assert(decodeSound(sound.data(),sound.size(),sample));
    assert(sample.rate==11025 && sample.pcm.size()==4 && sample.pcm[0]==-1 && sample.pcm[1]==127/128.f);
    put32(sound,4,99999); assert(!decodeSound(sound.data(),sound.size(),sample));
    std::mt19937 rng(1993);
    for(unsigned n=0;n<10000;++n) {
        std::vector<uint8_t> fuzz(rng()%200); for(auto &b:fuzz) b=rng();
        decodeMUS(fuzz.data(),fuzz.size(),song,error); decodeSound(fuzz.data(),fuzz.size(),sample);
        auto score=mus(fuzz); decodeMUS(score.data(),score.size(),song,error);
    }
    // Format-only fixtures contain no game artwork or commercial maps.
    std::filesystem::create_directories("build/tests/unit-saves");
    std::vector<std::string> names={"E1M1","THINGS","LINEDEFS","SIDEDEFS","VERTEXES","SEGS","SSECTORS","NODES","SECTORS","REJECT","BLOCKMAP","PLAYPAL"};
    std::vector<uint8_t> wad(12+14*768+names.size()*16,0); memcpy(wad.data(),"IWAD",4); put32(wad,4,names.size()); put32(wad,8,12+14*768);
    size_t directory=12+14*768;
    for(size_t i=0;i<names.size();++i) { put32(wad,directory+i*16,12); memcpy(wad.data()+directory+i*16+8,names[i].data(),names[i].size()); }
    put32(wad,directory+11*16+4,14*768);
    auto write=[&](const char *path) { std::ofstream f(path,std::ios::binary); f.write((const char *)wad.data(),wad.size()); };
    IWADInfo info; write("build/tests/format.wad"); assert(validateIWAD("build/tests/format.wad",info,error) && !info.registered);
    ContentProfile original,renamed,commercial;
    assert(contentProfile("build/tests/format.wad",{},original,error));
    write("build/tests/renamed.wad"); assert(contentProfile("build/tests/renamed.wad",{},renamed,error));
    assert(original.saveKey==renamed.saveKey && original.title==renamed.title);
    memcpy(wad.data()+directory+8,"MAP01",5); write("build/tests/commercial.wad");
    // DOOM II and other IWADs, and add-on WADs, are refused.
    assert(!validateIWAD("build/tests/commercial.wad",info,error));
    assert(!contentProfile("build/tests/commercial.wad",{},commercial,error));
    memcpy(wad.data()+directory+8,"E1M1\0",5); memcpy(wad.data(),"PWAD",4); write("build/tests/addon.wad");
    assert(contentProfile("build/tests/format.wad",{"build/tests/addon.wad"},commercial,error) && !commercial.supported);
    memcpy(wad.data(),"IWAD",4);
    put32(wad,directory+11*16+4,UINT32_MAX); write("build/tests/broken.wad"); assert(!validateIWAD("build/tests/broken.wad",info,error));
    {
        // Four complete episodes: The Ultimate DOOM, the retail game mode.
        std::vector<std::string> maps={"E1M1","E2M1","E3M1","E4M1"},lumps;
        for(const auto &m:maps) { lumps.push_back(m); for(const char *n:{"THINGS","LINEDEFS","SIDEDEFS","VERTEXES","SEGS","SSECTORS","NODES","SECTORS","REJECT","BLOCKMAP"}) lumps.push_back(n); }
        lumps.push_back("PLAYPAL");
        std::vector<uint8_t> four(12+14*768+lumps.size()*16,0); memcpy(four.data(),"IWAD",4); put32(four,4,lumps.size()); put32(four,8,12+14*768);
        for(size_t i=0;i<lumps.size();++i) { put32(four,12+14*768+i*16,12); memcpy(four.data()+12+14*768+i*16+8,lumps[i].data(),lumps[i].size()); }
        put32(four,12+14*768+(lumps.size()-1)*16+4,14*768);
        { std::ofstream f("build/tests/ultimate.wad",std::ios::binary); f.write((const char *)four.data(),four.size()); }
        ContentProfile ultimate;
        assert(contentProfile("build/tests/ultimate.wad",{},ultimate,error) && ultimate.supported && ultimate.mode==3 && ultimate.title=="The Ultimate DOOM");
    }
    FireGate gate;
    for(int weapon=0;weapon<9;++weapon) {
        gate={}; assert(!gate.sample(false,weapon,true)); assert(gate.sample(true,weapon,true));
        for(int tic=0;tic<100;++tic) assert(gate.sample(true,weapon,true)==(firePolicy(weapon)==FirePolicy::Held));
        assert(!gate.sample(false,weapon,true)); assert(gate.sample(true,weapon,true));
        assert(!gate.sample(true,weapon,false)); assert(!gate.sample(true,weapon,true));
        assert(!gate.sample(false,weapon,true)); assert(gate.sample(true,weapon,true));
    }
    gate={}; assert(gate.sample(true,1,true)); assert(!gate.sample(true,6,true));
    gate.reset(); assert(!gate.sample(true,1,true)); assert(!gate.sample(false,1,true)); assert(gate.sample(true,1,true));
    FeedbackStream stream;
    assert(stream.shot(5,1)); assert(!stream.shot(5,1.08)); assert(!stream.update(5,true,true,1.15));
    assert(stream.update(5,false,true,1.16) && stream.weapon==-1);
    assert(stream.shot(5,2)); assert(stream.update(3,true,true,2.05));
    assert(stream.shot(5,3)); assert(stream.update(5,true,false,3.01));
    assert(stream.shot(5,4)); assert(stream.update(5,true,true,4.3));
    assert(hapticEffect(Damage,5).body.strength<hapticEffect(Damage,50).body.strength);
    assert(hapticEffect(Damage,5).body.decay<hapticEffect(Damage,50).body.decay);
    assert(hapticEffect(WeaponFire,5).stream && !hapticEffect(WeaponFire,8).stream);
    // Every kick drives a rumble motor long enough to spin up (>=45 ms with
    // the device floor) and two-handed weapons kick both triggers.
    for(int weapon=0;weapon<9;++weapon) { auto e=hapticEffect(WeaponFire,weapon); assert(e.stream || e.body.hold>=.045f); }
    assert(hapticEffect(WeaponFire,2).bothTriggers && !hapticEffect(WeaponFire,1).bothTriggers);
    // Trigger walls peak just before the 40% fire point and break after it;
    // a cycling shotgun is locked; an empty gun has no wall.
    auto pistol=triggerProfile(1,TriggerState::Ready);
    assert(pistol.zones[3]==1 && pistol.zones[1]<pistol.zones[2] && pistol.zones[4]<pistol.zones[3]);
    auto locked=triggerProfile(2,TriggerState::Cycling); for(float v:locked.zones) assert(v==1);
    auto empty=triggerProfile(2,TriggerState::Empty); for(float v:empty.zones) assert(v<.1f);
    assert(triggerProfile(3,TriggerState::Firing).vibration && !triggerProfile(1,TriggerState::Cycling).vibration);
    std::vector<uint8_t> save(51,0); memcpy(save.data()+24,"version 109",11); save.back()=0x1d;
    std::string saveDir="build/tests/unit-saves";
    for(int slot=0;slot<6;++slot) std::filesystem::remove(savePath(saveDir,slot));
    assert(latestSave(saveDir)==-1);
    assert(writeSave(saveDir,4,save.data(),save.size(),error)); assert(latestSave(saveDir)==4);
    assert(writeSave(saveDir,0,save.data(),save.size(),error)); assert(latestSave(saveDir)==0);
    assert(writeSave(saveDir,4,save.data(),save.size(),error)); assert(latestSave(saveDir)==4);
    assert(!writeSave(saveDir,6,save.data(),save.size(),error)); assert(latestSave(saveDir)==4); // slots 0-5
    assert(!atomicWrite(saveDir+"/missing/slot",save.data(),save.size(),error)); assert(validSave(savePath(saveDir,4)));
    std::filesystem::remove(savePath(saveDir,4)); assert(latestSave(saveDir)==0);
    // Adaptive resolution: a lone slow frame keeps native size; two slow
    // frames, or one near miss, drop straight to the largest size that fits;
    // recovery is one step after a second of headroom; outside stalls are
    // ignored; heat and Low Power Mode shrink the budget; a 120 Hz display
    // paces at 60 before resolution is given up.
    {
        using native_render::RenderScale;
        const double hz60=1000/60.0,hz120=1000/120.0;
        RenderScale scale;
        for(int i=0;i<30;++i) assert(!scale.frame(8,7,hz60,0,false) && scale.sixteenths()==16);
        assert(!scale.frame(15,14,hz60,0,false) && scale.sixteenths()==16);
        assert(!scale.frame(8,7,hz60,0,false) && !scale.frame(8,7,hz60,0,false) && !scale.frame(14,13,hz60,0,false) && scale.sixteenths()==16);
        assert(scale.frame(14,13,hz60,0,false) && scale.sixteenths()==14);
        assert(!scale.frame(6,5,hz60,0,false) && !scale.frame(6,5,hz60,0,false)); // the resize settles
        int frames=0; while(!scale.frame(6,5,hz60,0,false)) ++frames;
        assert(scale.sixteenths()==15 && frames>=55 && frames<=60);
        RenderScale urgent;
        assert(urgent.frame(16,15,hz60,0,false) && urgent.sixteenths()==13);
        RenderScale floor;
        assert(floor.frame(100,99,hz60,0,false) && floor.sixteenths()==RenderScale::Lowest);
        RenderScale stall;
        for(int i=0;i<5;++i) assert(!stall.frame(40,5,hz60,0,false) && stall.sixteenths()==16);
        stall.settle(3); for(int i=0;i<3;++i) assert(!stall.frame(100,99,hz60,0,false));
        assert(RenderScale::budgetShare(3,false)<RenderScale::budgetShare(2,false) && RenderScale::budgetShare(2,false)<RenderScale::budgetShare(0,false));
        assert(RenderScale::budgetShare(0,true)==RenderScale::budgetShare(2,false));
        RenderScale hot; for(int i=0;i<4;++i) hot.frame(9,8,hz60,3,false);
        assert(hot.sixteenths()<16); // 9 ms fits at nominal, not under critical heat
        RenderScale fast;
        assert(fast.frame(9,8,hz120,0,false) && fast.sixteenths()==16 && fast.intervalMs()==2*hz120);
    }
    printf("Unit: MUS/MIDI decoding, clock, controller commands, stereo gains, DOOM/Ultimate DOOM content gate, WAD bounds, weapon fire gates, haptic streams and envelopes, trigger profiles, atomic saves and adaptive render scale passed\n");
}
