// SPDX-License-Identifier: GPL-2.0-only
#import <Foundation/Foundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#include "Native.h"
#include "Portable.hpp"
#include "GoreSound.hpp"
extern "C" {
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"
#include "m_argv.h"
#include "doomstat.h"
}
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>

namespace {
constexpr double rate=48000;
constexpr unsigned slots=32, queueSize=1024;
struct Voice { const native_doom::Sample *sample=nullptr; int handle=0; double position=0, step=1; float left=0,right=0; };
enum Operation { Start, Stop, Params, StartMusic, StopMusic };
struct Command { Operation operation; int handle=0; const native_doom::Sample *sample=nullptr; const native_doom::Song *song=nullptr; double step=1; float left=0,right=0; bool loop=false; };
std::array<Command,queueSize> commands;
std::atomic<unsigned> readIndex{0}, writeIndex{0};
std::array<std::atomic<int>,slots> playing{};
std::array<Voice,slots> voices;
std::array<std::unique_ptr<native_doom::Sample>,NUMSFX> samples;
// Gore events (GoreSound.cpp): premixed layer stacks, four variants per kind.
std::array<std::unique_ptr<native_doom::Sample>,gore_sound::Kinds*gore_sound::Variants> goreSounds;
unsigned variantSeed=0x9e3779b9u;
std::array<int,gore_sound::Kinds> lastGore;
std::array<double,slots> producerRates{};
std::array<float,8192> synthLeft, synthRight;
std::vector<std::unique_ptr<native_doom::Song>> songs;
std::atomic<bool> suspended{false}, deviceChanged{false}, musicMuffled{false};
std::atomic<float> musicVolume{.5};
AudioUnit output=nullptr, synth=nullptr;
// Clean playback (2026-10-09). Every WAD sound is prepared once at load,
// deterministically and without external recordings:
//  - DC offset removed.
//  - Windowed-sinc resampling to 48 kHz: no imaging grit from 11 kHz audio.
//  - Clickless edges, silent tails trimmed, original loudness restored so the
//    balance between sounds stays as designed; peaks kept below 0.9.
double rms(const std::vector<float> &v) { double e=0; for(float x:v) e+=double(x)*x; return v.empty()?0:std::sqrt(e/v.size()); }
void remaster(native_doom::Sample &s,const char *name) {
    if(s.pcm.empty() || !s.rate) return;
    std::vector<float> in=s.pcm;
    double mean=0; for(float x:in) mean+=x; mean/=in.size(); for(float &x:in) x-=float(mean);
    double loudness=rms(in);
    // Windowed-sinc resampling (Blackman, 32 zero crossings).
    const double out=48000,ratio=out/s.rate,cut=std::min(1.0,ratio)*.95; const int taps=32;
    size_t length=size_t(std::ceil(in.size()*ratio));
    std::vector<float> up(length);
    for(size_t i=0;i<length;++i) {
        double t=i/ratio; long c=long(std::floor(t)); double sum=0;
        for(long k=c-taps+1;k<=c+taps;++k) {
            if(k<0 || k>=long(in.size())) continue;
            double d=t-k,x=M_PI*d*cut,sinc=std::abs(d)<1e-9?1:std::sin(x)/x;
            double w=.42+.5*std::cos(M_PI*d/taps)+.08*std::cos(2*M_PI*d/taps);
            if(std::abs(d)<taps) sum+=in[size_t(k)]*cut*sinc*w;
        }
        up[i]=float(sum);
    }
    // Trim trailing silence (-66 dB) and fade both edges without clicks.
    size_t end=up.size(); while(end>1 && std::abs(up[end-1])<.0005f) --end;
    up.resize(std::max<size_t>(end,1));
    size_t fadeIn=std::min<size_t>(48,up.size()),fadeOut=std::min<size_t>(480,up.size());
    for(size_t i=0;i<fadeIn;++i) up[i]*=float(i)/fadeIn;
    for(size_t i=0;i<fadeOut;++i) up[up.size()-1-i]*=float(i)/fadeOut;
    double now=rms(up); float gain=now>1e-6?float(loudness/now):1.f;
    for(float &x:up) x*=gain;
    // Look-ahead peak limiter (2 ms ahead, 60 ms release, ceiling 0.9): only
    // transient peaks are tamed, so the matched loudness stays intact.
    { const float ceiling=.9f; const size_t ahead=96; const float release=std::exp(-1.f/(.06f*48000));
      std::vector<float> want(up.size());
      for(size_t i=0;i<up.size();++i) want[i]=std::min(1.f,ceiling/std::max(std::abs(up[i]),1e-9f));
      // Minimum gain over the look-ahead window; fast glide down, smooth release.
      std::deque<size_t> window; float level=1;
      auto push=[&](size_t k) { while(!window.empty() && want[window.back()]>=want[k]) window.pop_back(); window.push_back(k); };
      for(size_t k=0;k<std::min(ahead,up.size());++k) push(k);
      for(size_t i=0;i<up.size();++i) {
          if(i+ahead<up.size()) push(i+ahead);
          while(window.front()<i) window.pop_front();
          float target=want[window.front()];
          level=target<level?level+(target-level)/24.f:target+(level-target)*release;
          level=std::min(level,want[i]);
          up[i]*=level;
      } }
    s.pcm=std::move(up); s.rate=uint32_t(out);
}
const native_doom::Sample *originalSample(int id) {
    if(id<=0 || id>=NUMSFX) return nullptr;
    if(!samples[id]) {
        sfxinfo_t *s=&S_sfx[id]; if(s->link) s=s->link;
        int lump=I_GetSfxLumpNum(s),length=W_LumpLength(lump);
        auto sample=std::make_unique<native_doom::Sample>();
        if(!native_doom::decodeSound((const uint8_t *)W_CacheLumpNum(lump,PU_CACHE),length,*sample)) return nullptr;
        remaster(*sample,s->name);
        samples[id]=std::move(sample);
    }
    return samples[id].get();
}
void buildGore() {
    const native_doom::Sample *slop=originalSample(sfx_slop);
    lastGore.fill(-1);
    for(int k=0;k<gore_sound::Kinds;++k) for(int v=0;v<gore_sound::Variants;++v) {
        auto sample=std::make_unique<native_doom::Sample>(); sample->rate=gore_sound::Rate;
        sample->pcm=gore_sound::build(gore_sound::Kind(k),v,slop?slop->pcm.data():nullptr,slop?slop->pcm.size():0,slop?slop->rate:0);
        goreSounds[size_t(k*gore_sound::Variants+v)]=std::move(sample);
    }
}
bool listenerInstalled=false, soundDisabled=false, musicDisabled=false;
int nextHandle=0;
const native_doom::Song *currentSong=nullptr;
size_t songIndex=0;
uint64_t songFrame=0, synthFrame=0;
bool looping=false;
struct MIDICheck { unsigned status, offset; };
std::array<MIDICheck,16> midiChecks{};
unsigned midiCheckCount=0;
bool checkingMIDI=false;
void dispatchMIDI(const native_doom::MIDIEvent &event,unsigned offset) {
    if(checkingMIDI && midiCheckCount<midiChecks.size()) midiChecks[midiCheckCount++]={event.status,offset};
    MusicDeviceMIDIEvent(synth,event.status,event.a,event.b,offset);
}

const AudioObjectPropertyAddress outputAddress={kAudioHardwarePropertyDefaultOutputDevice,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
bool enqueue(Command c) {
    unsigned w=writeIndex.load(std::memory_order_relaxed);
    if(w-readIndex.load(std::memory_order_acquire)>=queueSize) { fprintf(stderr,"Audio command queue full\n"); return false; }
    commands[w%queueSize]=c; writeIndex.store(w+1,std::memory_order_release); return true;
}
void allNotesOff() {
    if(!synth) return;
    for(int ch=0;ch<16;++ch) { MusicDeviceMIDIEvent(synth,0xb0|ch,120,0,0); MusicDeviceMIDIEvent(synth,0xb0|ch,121,0,0); }
}
static void resetMIDI() {
    allNotesOff();
    if(synth) for(int ch=0;ch<16;++ch) { MusicDeviceMIDIEvent(synth,0xc0|ch,0,0,0); MusicDeviceMIDIEvent(synth,0xe0|ch,0,64,0); }
}
void applyCommands() {
    unsigned r=readIndex.load(std::memory_order_relaxed), w=writeIndex.load(std::memory_order_acquire);
    while(r!=w) {
        const Command &c=commands[r++%queueSize];
        if(c.operation==StartMusic) { resetMIDI(); currentSong=c.song; songIndex=0; songFrame=0; looping=c.loop; }
        else if(c.operation==StopMusic) { allNotesOff(); currentSong=nullptr; }
        else {
            Voice &v=voices[unsigned(c.handle)%slots];
            if(c.operation==Start) v={c.sample,c.handle,0,c.step,c.left,c.right};
            else if(v.handle==c.handle) {
                if(c.operation==Stop) v.sample=nullptr;
                else { if(c.step>0) v.step=c.step; v.left=c.left; v.right=c.right; }
            }
        }
    }
    readIndex.store(r,std::memory_order_release);
}
void scheduleMusic(unsigned frames) {
    if(!synth || !currentSong) return;
    uint64_t end=songFrame+frames;
    const uint64_t length=std::max<uint64_t>(1,currentSong->ticks*uint64_t(rate)/140);
    // Split loops at their exact sample boundary; event offsets are relative to this callback.
    uint64_t origin=songFrame, segmentEnd=end; unsigned offset=0;
    while(currentSong) {
        segmentEnd=std::min(end,length);
        while(songIndex<currentSong->events.size()) {
            auto e=currentSong->events[songIndex]; uint64_t at=e.tick*uint64_t(rate)/140;
            if(at>=segmentEnd && !(at==length && segmentEnd==length && end>length)) break;
            if(at>=origin) dispatchMIDI(e,offset+unsigned(at-origin));
            ++songIndex;
        }
        // A boundary exactly at callback end belongs to the next callback.
        // Keep terminal note-offs before the first events of the next loop.
        if(end<=length) { songFrame=end; break; }
        offset+=unsigned(length-origin);
        if(!looping) { currentSong=nullptr; break; }
        // Do not reset voices at loop boundaries; the score's own note-offs are authoritative.
        end-=length; origin=0; songIndex=0; songFrame=0;
        if(!end) break;
    }
}
OSStatus renderSynth(AudioUnitRenderActionFlags *flags,const AudioTimeStamp *time,unsigned frames,float *dst) {
    if(frames>8192) return kAudio_ParamError;
    struct { UInt32 count; AudioBuffer buffers[2]; } planar={2,{{1,UInt32(frames*4),synthLeft.data()},{1,UInt32(frames*4),synthRight.data()}}};
    OSStatus status=AudioUnitRender(synth,flags,time,0,frames,(AudioBufferList *)&planar);
    if(!status) for(unsigned i=0;i<frames;++i) { dst[i*2]=synthLeft[i]; dst[i*2+1]=synthRight[i]; }
    return status;
}
OSStatus render(void *,AudioUnitRenderActionFlags *flags,const AudioTimeStamp *time,UInt32,UInt32 frames,AudioBufferList *data) {
    applyCommands();
    if(data->mNumberBuffers!=1 || data->mBuffers[0].mDataByteSize<frames*2*sizeof(float)) return kAudio_ParamError;
    float *dst=(float *)data->mBuffers[0].mData;
    std::fill_n(dst,frames*2,0);
    if(suspended.load(std::memory_order_relaxed)) return noErr;
    scheduleMusic(frames);
    if(synth) {
        AudioTimeStamp stamp=*time; stamp.mFlags=kAudioTimeStampSampleTimeValid; stamp.mSampleTime=synthFrame;
        if(renderSynth(flags,&stamp,frames,dst)!=noErr) std::fill_n(dst,frames*2,0);
        synthFrame+=frames;
        // Paused or in a menu, the score keeps playing as if heard through
        // a wall from the next room: two low-pass poles sweep down to about
        // 1 kHz and the level drops to 75%, gliding over a quarter second.
        static float muffle=0,low[2][2];
        float gain=musicVolume.load(std::memory_order_relaxed),target=musicMuffled.load(std::memory_order_relaxed)?1:0;
        float glide=1-std::exp(-1/(.25f*float(rate)));
        float cutoff=1000*std::pow(18000/1000.f,1-muffle),pole=1-std::exp(-2*float(M_PI)*cutoff/float(rate));
        for(unsigned f=0;f<frames;++f) {
            muffle+=(target-muffle)*glide;
            float level=gain*(1-.25f*muffle);
            for(int c=0;c<2;++c) {
                low[c][0]+=(dst[f*2+c]-low[c][0])*pole; low[c][1]+=(low[c][0]-low[c][1])*pole;
                dst[f*2+c]=low[c][1]*level;
            }
        }
    }
    for(Voice &v:voices) {
        if(!v.sample) continue;
        for(unsigned f=0;f<frames;++f) {
            if(v.position>=v.sample->pcm.size()) {
                int expected=v.handle; playing[unsigned(v.handle)%slots].compare_exchange_strong(expected,0); v.sample=nullptr; break;
            }
            size_t k=size_t(v.position); float frac=float(v.position-k);
            float s=v.sample->pcm[k]+(k+1<v.sample->pcm.size()?v.sample->pcm[k+1]-v.sample->pcm[k]:-v.sample->pcm[k])*frac;
            dst[f*2]+=s*v.left; dst[f*2+1]+=s*v.right; v.position+=v.step;
        }
        if(v.sample && v.position>=v.sample->pcm.size()) {
            int expected=v.handle; playing[unsigned(v.handle)%slots].compare_exchange_strong(expected,0); v.sample=nullptr;
        }
    }
    // Soft limiter instead of hard clipping: instant attack, 120 ms release,
    // ceiling -1 dBFS; a gentle saturator catches anything left over.
    static float envelope=0; const float release=std::exp(-1.f/(.12f*float(rate))),ceiling=.89f;
    for(unsigned f=0;f<frames;++f) {
        float peak=std::max(std::abs(dst[f*2]),std::abs(dst[f*2+1]));
        envelope=peak>envelope?peak:envelope*release+peak*(1-release);
        float gain=envelope>ceiling?ceiling/envelope:1.f;
        for(int c=0;c<2;++c) { float x=dst[f*2+c]*gain; dst[f*2+c]=std::abs(x)<.8f?x:std::copysign(.8f+.2f*std::tanh((std::abs(x)-.8f)/.2f),x); }
    }
    return noErr;
}
AudioStreamBasicDescription format() {
    AudioStreamBasicDescription f{}; f.mSampleRate=rate; f.mFormatID=kAudioFormatLinearPCM;
    f.mFormatFlags=kAudioFormatFlagsNativeFloatPacked; f.mBytesPerPacket=f.mBytesPerFrame=8;
    f.mFramesPerPacket=1; f.mChannelsPerFrame=2; f.mBitsPerChannel=32; return f;
}
bool check(OSStatus status,const char *operation) { if(status) fprintf(stderr,"Audio %s: OSStatus %d\n",operation,int(status)); return !status; }
bool createSynth() {
    AudioComponentDescription d{}; d.componentType=kAudioUnitType_MusicDevice; d.componentSubType=kAudioUnitSubType_DLSSynth; d.componentManufacturer=kAudioUnitManufacturer_Apple;
    AudioComponent c=AudioComponentFindNext(nullptr,&d); if(!c) return false;
    if(!check(AudioComponentInstanceNew(c,&synth),"create MIDI synth")) return false;
    auto f=format(); f.mFormatFlags|=kAudioFormatFlagIsNonInterleaved; f.mBytesPerFrame=f.mBytesPerPacket=4; UInt32 maxFrames=8192;
    if(!check(AudioUnitSetProperty(synth,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Output,0,&f,sizeof(f)),"synth format") ||
       !check(AudioUnitSetProperty(synth,kAudioUnitProperty_MaximumFramesPerSlice,kAudioUnitScope_Global,0,&maxFrames,sizeof(maxFrames)),"synth slice") ||
       !check(AudioUnitInitialize(synth),"initialize synth")) { AudioComponentInstanceDispose(synth); synth=nullptr; return false; }
    return true;
}
OSStatus outputChanged(AudioObjectID,UInt32,const AudioObjectPropertyAddress *,void *) { deviceChanged.store(true,std::memory_order_release); return noErr; }
bool createOutput() {
    AudioComponentDescription d{}; d.componentType=kAudioUnitType_Output; d.componentSubType=kAudioUnitSubType_DefaultOutput; d.componentManufacturer=kAudioUnitManufacturer_Apple;
    AudioComponent c=AudioComponentFindNext(nullptr,&d); if(!c) return false;
    if(!check(AudioComponentInstanceNew(c,&output),"create output")) return false;
    auto f=format(); AURenderCallbackStruct callback={render,nullptr}; UInt32 maxFrames=8192;
    bool ok=check(AudioUnitSetProperty(output,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Input,0,&f,sizeof(f)),"output format") &&
        check(AudioUnitSetProperty(output,kAudioUnitProperty_MaximumFramesPerSlice,kAudioUnitScope_Global,0,&maxFrames,sizeof(maxFrames)),"output slice") &&
        check(AudioUnitSetProperty(output,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&callback,sizeof(callback)),"callback") &&
        check(AudioUnitInitialize(output),"initialize output") && check(AudioOutputUnitStart(output),"start output");
    if(!ok) { AudioComponentInstanceDispose(output); output=nullptr; }
    return ok;
}
}
// Stereo placement of a map point as the original mixer places sounds:
// separation by the angle from the player's view (S_AdjustSoundParams).
int separationAt(double x,double y) {
    const mobj_t *ear=players[consoleplayer].mo;
    if(!ear) return 128;
    double dx=x-ear->x/double(FRACUNIT),dy=y-ear->y/double(FRACUNIT);
    if(dx==0 && dy==0) return 128;
    double angle=std::atan2(dy,dx)-ear->angle*(M_PI/2147483648.0);
    return int(std::lround(128-96*std::sin(angle)));
}
int playGore(gore_sound::Kind kind,int volume,int separation) {
    int &last=lastGore[size_t(kind)];
    variantSeed=variantSeed*1664525u+1013904223u;
    int variant=int((variantSeed>>8)%unsigned(gore_sound::Variants-1)); if(variant>=last) ++variant;
    last=variant;
    const native_doom::Sample *sample=goreSounds[size_t(kind*gore_sound::Variants+variant)].get();
    if(!sample || sample->pcm.empty()) return 0;
    if(nextHandle==INT32_MAX) nextHandle=0;
    int handle=++nextHandle;
    Command c{Start,handle,sample};
    producerRates[unsigned(handle)%slots]=c.sample->rate;
    c.step=double(c.sample->rate)/rate;
    native_doom::stereoGains(volume,separation,c.left,c.right);
    playing[unsigned(handle)%slots].store(handle,std::memory_order_release);
    if(!enqueue(c)) { playing[unsigned(handle)%slots].store(0); return 0; }
    return handle;
}
// A shot or blow landing in a bleeding monster (2026-10-09). DOOM has no such
// sound; this plays a layered gore event at the target, with the
// original distance falloff and a level that follows the damage. All pellets
// of one blast on one target in a tic are one impact. Audio only.
void Native_FleshImpact(void *targetPointer,int damage) {
    const auto *t=static_cast<const mobj_t *>(targetPointer);
    const mobj_t *ear=players[consoleplayer].mo;
    if(soundDisabled || !output || gamestate!=GS_LEVEL || !t || !ear || t->player
       || (t->flags&MF_NOBLOOD) || !(t->flags&MF_SHOOTABLE) || damage<=0) return;
    // Event by outcome: a gib, a kill, a heavy hit (35+) or a light one.
    // Pellets of one blast on one target in a tic are one event; a later
    // pellet that kills upgrades it.
    int after=t->health-damage;
    gore_sound::Kind kind=after<=0?((t->info && after< -t->info->spawnhealth && t->info->xdeathstate) || damage>=1000?gore_sound::Gib:gore_sound::Kill)
        :damage>=35?gore_sound::Heavy:gore_sound::Hit;
    static int tic=-1,count=0; static const void *heard[8]; static gore_sound::Kind heardKind[8];
    if(tic!=leveltime) { tic=leveltime; count=0; }
    int slot=0; while(slot<count && heard[slot]!=t) ++slot;
    if(slot<count && kind<=heardKind[slot]) return;
    if(slot==count) { if(count>=8) return; heard[count++]=t; }
    heardKind[slot]=kind;
    double distance=std::hypot(double(t->x-ear->x),double(t->y-ear->y))/FRACUNIT;
    if(distance>=1200) return;
    double level=distance<=160?1:(1200-distance)/1040;
    level*=std::min(1.0,.45+damage/30.0);
    int volume=int(std::lround(snd_SfxVolume*level));
    if(volume<1) return;
    if(kind>=gore_sound::Kill) volume=std::max(volume,int(std::lround(snd_SfxVolume*std::min(1.0,level*1.4))));
    playGore(kind,volume,separationAt(t->x/double(FRACUNIT),t->y/double(FRACUNIT)));
}
// Gore sounds without a hit: meat landing and arterial spurts at a map
// point (Gore.cpp), with the original 160/1200 distance falloff.
void Native_GoreSound(int kind,double x,double y,double z,double strength) {
    const mobj_t *ear=players[consoleplayer].mo;
    if(soundDisabled || !output || gamestate!=GS_LEVEL || kind<0 || kind>=gore_sound::Kinds || !ear) return;
    double distance=std::hypot(ear->x/double(FRACUNIT)-x,ear->y/double(FRACUNIT)-y);
    if(distance>=1200) return;
    double level=(distance<=160?1:(1200-distance)/1040)*std::clamp(strength,0.0,1.0);
    int volume=int(std::lround(snd_SfxVolume*level));
    if(volume<1) return;
    playGore(gore_sound::Kind(kind),volume,separationAt(x,y));
}
void Native_AudioSuspend(int value) { suspended.store(value!=0,std::memory_order_release); }
void Native_AudioService(void) {
    if(!deviceChanged.exchange(false) || (soundDisabled && musicDisabled)) return;
    if(output) { AudioOutputUnitStop(output); AudioUnitUninitialize(output); AudioComponentInstanceDispose(output); output=nullptr; }
    if(!createOutput()) fprintf(stderr,"Audio device unavailable; reconnect an output device to retry\n");
}

extern "C" {
void I_InitSound(void) {
    soundDisabled=M_CheckParm((char *)"-nosound") || M_CheckParm((char *)"-nosfx");
    musicDisabled=M_CheckParm((char *)"-nosound") || M_CheckParm((char *)"-nomusic");
    if(!soundDisabled || !musicDisabled) {
        if(!musicDisabled && !createSynth()) fprintf(stderr,"Native MIDI synthesizer unavailable\n");
        if(!createOutput()) fprintf(stderr,"Native audio output unavailable\n");
        listenerInstalled=check(AudioObjectAddPropertyListener(kAudioObjectSystemObject,&outputAddress,outputChanged,nullptr),"device listener");
    }
}
void I_InitMusic(void) {}
void I_SetChannels(void) {
    if(soundDisabled) return;
    // Prepare every sound now, not on its first use in play.
    for(int id=1;id<NUMSFX;++id) {
        const sfxinfo_t *s=&S_sfx[id]; if(s->link) s=s->link;
        char name[9]; snprintf(name,sizeof(name),"ds%.6s",s->name);
        if(W_CheckNumForName(name)>=0) originalSample(id);
    }
    buildGore();
}
void I_UpdateSound(void) {
    musicMuffled.store(paused || menuactive,std::memory_order_relaxed);
}
void I_SubmitSound(void) {}
int I_GetSfxLumpNum(sfxinfo_t *s) { char name[9]; snprintf(name,sizeof(name),"ds%.6s",s->name); return W_GetNumForName(name); }
int I_StartSound(int id,int volume,int separation,int pitch,int priority) {
    if(soundDisabled || !output || id<=0 || id>=NUMSFX) return 0;
    const native_doom::Sample *sample=originalSample(id);
    if(!sample) return 0;
    if(nextHandle==INT32_MAX) nextHandle=0;
    int handle=++nextHandle;
    Command c{Start,handle,sample};
    producerRates[unsigned(handle)%slots]=c.sample->rate;
    c.step=double(c.sample->rate)/rate*std::pow(2.0,(std::clamp(pitch,0,255)-128)/64.0);
    native_doom::stereoGains(volume,separation,c.left,c.right);
    playing[unsigned(handle)%slots].store(handle,std::memory_order_release);
    if(!enqueue(c)) { playing[unsigned(handle)%slots].store(0); return 0; }
    return handle;
}
void I_StopSound(int handle) { if(handle<=0) return; enqueue({Stop,handle}); int expected=handle; playing[unsigned(handle)%slots].compare_exchange_strong(expected,0); }
int I_SoundIsPlaying(int handle) { return handle>0 && playing[unsigned(handle)%slots].load(std::memory_order_acquire)==handle; }
void I_UpdateSoundParams(int handle,int volume,int separation,int pitch) {
    if(!I_SoundIsPlaying(handle)) return;
    Command c{Params,handle}; native_doom::stereoGains(volume,separation,c.left,c.right);
    c.step=producerRates[unsigned(handle)%slots]/rate*std::pow(2.0,(std::clamp(pitch,0,255)-128)/64.0); enqueue(c);
}
void I_ShutdownSound(void) {
    if(listenerInstalled) { AudioObjectRemovePropertyListener(kAudioObjectSystemObject,&outputAddress,outputChanged,nullptr); listenerInstalled=false; }
    if(output) { AudioOutputUnitStop(output); AudioUnitUninitialize(output); AudioComponentInstanceDispose(output); output=nullptr; }
}
void I_ShutdownMusic(void) { if(synth) { AudioUnitUninitialize(synth); AudioComponentInstanceDispose(synth); synth=nullptr; } }
// Music sits about 2 dB lower than its setting (owner's balance, 2026-10-10).
void I_SetMusicVolume(int volume) { musicVolume.store(std::clamp(volume,0,15)/15.0f*.8f); }
int I_RegisterSong(void *data,int length) {
    auto song=std::make_unique<native_doom::Song>(); std::string error;
    if(length<0 || !(length>=4 && !memcmp(data,"MThd",4)?native_doom::decodeMIDI((const uint8_t *)data,length,*song,error):native_doom::decodeMUS((const uint8_t *)data,length,*song,error))) { fprintf(stderr,"Music decode: %s\n",error.c_str()); return 0; }
    if(!song->ticks) { fprintf(stderr,"Music score has no duration\n"); return 0; }
    // Revisited levels reuse stable scores; pointers stay valid on the audio thread.
    for(size_t i=0;i<songs.size();++i) if(song->ticks==songs[i]->ticks && song->events.size()==songs[i]->events.size() &&
        std::equal(song->events.begin(),song->events.end(),songs[i]->events.begin(),[](const auto &a,const auto &b) {
            return a.tick==b.tick && a.status==b.status && a.a==b.a && a.b==b.b;
        })) return int(i+1);
    songs.push_back(std::move(song)); return int(songs.size());
}
void I_PlaySong(int handle,int loop) { if(handle<=0 || size_t(handle)>songs.size() || musicDisabled) return; Command c{StartMusic}; c.song=songs[handle-1].get(); c.loop=loop; enqueue(c); }
void I_StopSong(int handle) { enqueue({StopMusic}); }
void I_UnRegisterSong(int handle) { /* Retain tiny scores until shutdown: render thread may still hold a pointer. */ }
// Pause no longer silences the score; I_UpdateSound muffles it instead.
void I_PauseSong(int handle) {}
void I_ResumeSong(int handle) {}
}

int Native_AudioTest(void) {
    // Exercise the actual callback without an output device or MIDI contribution.
    native_doom::Sample sample; sample.rate=48000; sample.pcm={1,.5f,-1,-.5f};
    std::array<float,8> mixed{}; AudioBufferList buffers{};
    buffers.mNumberBuffers=1; buffers.mBuffers[0]={2,UInt32(mixed.size()*sizeof(float)),mixed.data()};
    AudioTimeStamp t{}; t.mFlags=kAudioTimeStampSampleTimeValid; AudioUnitRenderActionFlags flags=0;
    playing[1]=1; playing[2]=2;
    enqueue({Start,1,&sample,nullptr,1,.25f,0}); enqueue({Start,2,&sample,nullptr,1,0,.5f});
    if(render(nullptr,&flags,&t,0,4,&buffers)) return 1;
    for(size_t i=0;i<4;++i) if(mixed[i*2]!=sample.pcm[i]*.25f || mixed[i*2+1]!=sample.pcm[i]*.5f) return 1;
    if(I_SoundIsPlaying(1) || I_SoundIsPlaying(2)) return 1;
    playing[3]=3; enqueue({Start,3,&sample,nullptr,.5,1,1});
    suspended=true;
    if(render(nullptr,&flags,&t,0,4,&buffers) || voices[3].position!=0) return 1;
    for(float s:mixed) if(s!=0) return 1;
    suspended=false;
    enqueue({Params,3,nullptr,nullptr,1,.5f,.25f});
    if(render(nullptr,&flags,&t,0,2,&buffers) || mixed[0]!=.5f || mixed[1]!=.25f || mixed[2]!=.25f) return 1;
    I_StopSound(3);
    if(render(nullptr,&flags,&t,0,4,&buffers)) return 1;
    for(float s:mixed) if(s!=0) return 1;
    printf("Native SFX callback: simultaneous stereo voices, completion, pitch/parameter update, suspension and stop passed\n");
    if(!createSynth()) return 1;
    native_doom::Song boundary; boundary.ticks=140; boundary.events={{0,0x90,60,100},{140,0x80,60,0}};
    currentSong=&boundary; songFrame=0; songIndex=0; looping=true; checkingMIDI=true; midiCheckCount=0;
    scheduleMusic(47999); scheduleMusic(1); scheduleMusic(8);
    if(midiCheckCount!=3 || midiChecks[0].status!=0x90 || midiChecks[1].status!=0x80 ||
       midiChecks[2].status!=0x90 || midiChecks[1].offset!=0 || midiChecks[2].offset!=0 || songFrame!=8) return 1;
    songFrame=48000; songIndex=1; looping=false; midiCheckCount=0;
    scheduleMusic(8);
    if(currentSong || midiCheckCount!=1 || midiChecks[0].status!=0x80 || midiChecks[0].offset!=0) return 1;
    checkingMIDI=false;
    printf("Native MUS scheduler: final note-off preserved at exact callback/loop boundary and non-looping completion passed\n");
    resetMIDI(); MusicDeviceMIDIEvent(synth,0x90,69,100,0);
    std::vector<float> data(4096*2);
    OSStatus status=renderSynth(&flags,&t,4096,data.data());
    double energy=0; for(float s:data) { if(!std::isfinite(s)) return 1; energy+=s*s; }
    allNotesOff(); I_ShutdownMusic();
    printf("Native MIDI synth: status=%d, finite stereo samples=8192, energy=%.8f\n",int(status),energy);
    return status || energy<.0001;
}
