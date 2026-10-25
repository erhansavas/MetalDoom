// SPDX-License-Identifier: GPL-2.0-only
// The macOS application: game data selection, window and fullscreen, Metal
// presentation, frame pacing, keyboard and mouse input and the native menu.
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CAMetalDisplayLink.h>
#import <QuartzCore/CATransaction.h>
#import <Metal/Metal.h>
#import <GameController/GameController.h>
#include "Portable.hpp"
#include "Session.hpp"
#include "MenuOverlay.hpp"
#include <csignal>
#include "Native.h"
#include "PresentationMetrics.hpp"
#include "RenderScale.hpp"
extern "C" {
#include "doomstat.h"
#include "doomdata.h"
#include "d_main.h"
#include "m_argv.h"
#include "i_system.h"
#include "v_video.h"
#include "r_main.h"
#include "r_state.h"
#include "d_items.h"
#include "w_wad.h"
#include "z_zone.h"
#include "g_game.h"
#include "m_menu.h"
#include "st_stuff.h"
extern boolean menuactive;
extern int messageToPrint;
extern int screenblocks, detailLevel;
void M_ClearMenus(void);
}
#include <mach/mach_time.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>

static uint64_t nowNanos() {
    static mach_timebase_info_data_t base=[] { mach_timebase_info_data_t b; mach_timebase_info(&b); return b; }();
    uint64_t t=mach_absolute_time(); return (t/base.denom)*base.numer+(t%base.denom)*base.numer/base.denom;
}
static native_doom::Clock gameClock(nowNanos());
static NSWindow *window;
static NSWindow *windowedWindow;
static NSView *fullscreenView;
static NSApplicationPresentationOptions windowedPresentationOptions;
static CAMetalLayer *metalLayer;
static id<MTLDevice> device;
static id<MTLCommandQueue> queue;
static id<MTLRenderPipelineState> pipeline,statusPipeline;
static id<MTLTexture> indices[2],viewmodels[2],lighting[2],statusbars[2];
static dispatch_semaphore_t uploadSemaphore;
static unsigned uploadIndex=0;
static std::atomic<bool> gpuFailed{false};
int native_render_width=320,native_render_height=200;
static unsigned requestedWidth=320,requestedHeight=200;
// Adaptive render resolution (RenderScale.hpp).
static native_render::RenderScale renderScale;
static unsigned scaledSize(double native) { return std::max(1u,unsigned(std::lround(native*renderScale.scale()))); }
static std::vector<uint8_t> renderPixels;
static uint8_t palette[256][4];
static float linearPalette[256][4];
static int musicEnabled=-1;
int Native_MusicEnabled(void) {
    if(musicEnabled<0) { NSUserDefaults *d=NSUserDefaults.standardUserDefaults; musicEnabled=[d objectForKey:@"Music"]?[d boolForKey:@"Music"]:1; }
    return musicEnabled;
}
void Native_SetMusicEnabled(int on) { musicEnabled=on!=0; [NSUserDefaults.standardUserDefaults setBool:musicEnabled forKey:@"Music"]; }
static void updateLinearPalette() {
    for(unsigned i=0;i<256;i++) {
        for(unsigned c=0;c<3;c++) { float v=palette[i][c]/255.f; linearPalette[i][c]=v<=.04045f?v/12.92f:std::pow((v+.055f)/1.055f,2.4f); }
        linearPalette[i][3]=1;
    }
}
static bool focused=true, awake=true, engineStarted=false, captured=false, terminating=false;
static bool heldKeys[256];
// Engine key posted by each physical key, so release matches press even if
// Shift/Option changed the produced character in between.
static int physicalKeys[128];
static NSString *iwadPath, *configPath;
static native_doom::IWADInfo iwadInfo;
static bool diagnostic=false;
void Native_RenderSize(int blocks) {
    bool wide=!diagnostic && blocks==11;
    native_render_width=wide?requestedWidth:320;
    native_render_height=wide?requestedHeight:200;
    if(native_render_width!=320 || native_render_height!=200)
        renderPixels.resize(size_t(native_render_width)*native_render_height);
}
uint8_t *Native_RenderPixels(void) {
    return (native_render_width!=320 || native_render_height!=200)?renderPixels.data():screens[0];
}
static bool fullscreen=false, systemFullscreenReady=false;
static volatile sig_atomic_t interruptRequested=0;
static void requestInterrupt(int) { interruptRequested=1; }
// The original status bar, drawn as a card at the foot of the native view.
static bool statusBar=true;
int Native_StatusBar(void) { return statusBar; }
void Native_SetStatusBar(int on) {
    statusBar=on!=0; [NSUserDefaults.standardUserDefaults setBool:statusBar forKey:@"StatusBarHUD"];
}
static int skipTransitions=-1;
int Native_SkipTransitions(void) {
    if(skipTransitions<0) { NSUserDefaults *d=NSUserDefaults.standardUserDefaults; skipTransitions=!diagnostic && ([d objectForKey:@"SkipTransitions"]?[d boolForKey:@"SkipTransitions"]:YES); }
    return skipTransitions;
}
void Native_SetSkipTransitions(int on) { skipTransitions=on!=0; [NSUserDefaults.standardUserDefaults setBool:skipTransitions forKey:@"SkipTransitions"]; }
void Native_ToggleHUD(void) { if(engineStarted && gamestate==GS_LEVEL && !menuactive && !paused) Native_SetStatusBar(!statusBar); }

static id displayLink;
static id<CAMetalDrawable> readyDrawable,frameDrawable;
static CFTimeInterval readyPresentationTimestamp=0,framePresentationTimestamp=0;
static CFTimeInterval calibratedClockLead=0,readyClockLead=0,frameClockLead=0;
static bool presentationClockAligned=false;
static void (*scheduledFrame)(void)=nullptr;
static bool runningFrame=false;
int Native_ScheduleFrames(void (*frame)(void)) {
    if(!displayLink || diagnostic) return 0;
    scheduledFrame=frame;
    return 1;
}
// The display's own rate, or half of it while the adaptive resolution paces a
// fast display at 60 Hz.
static void applyFrameRate(NSScreen *screen) {
    if(@available(macOS 14.0,*)) {
        float rate=std::max(1L,screen.maximumFramesPerSecond);
        float target=renderScale.intervalMs()>0?std::min(rate,float(1000/renderScale.intervalMs())):rate;
        [(CAMetalDisplayLink *)displayLink setPreferredFrameRateRange:CAFrameRateRangeMake(std::min(60.0f,target),target,target)];
    }
}
// After each frame the display link ran: its CPU time and the software
// render inside it steer the render size (RenderScale.hpp).
static void governRenderScale(double workMs,NSScreen *screen) {
    double renderMs=Native_MetricsTakeRenderMs();
    static int lastMap=-1,lastEpisode=-1,lastTime=0,thermal=0,poll=0; static bool lowPower=false;
    // A level's first second carries its loading work.
    if(gamestate!=GS_LEVEL) lastMap=-1;
    else if(gamemap!=lastMap || gameepisode!=lastEpisode || leveltime<lastTime) { renderScale.settle(60); lastMap=gamemap; lastEpisode=gameepisode; }
    lastTime=leveltime;
    if(poll--<=0) { poll=30; thermal=int(NSProcessInfo.processInfo.thermalState); lowPower=NSProcessInfo.processInfo.lowPowerModeEnabled; }
    double interval=renderScale.intervalMs();
    if(!renderScale.frame(workMs,renderMs,1000.0/std::max(1L,screen.maximumFramesPerSecond),thermal,lowPower)) return;
    if(renderScale.intervalMs()!=interval) applyFrameRate(screen);
}

static NativeMenuOverlay menuOverlay;
static int mainArgc;
static char **mainArgv;
extern void Native_ControllersInit(void);
extern int Native_AudioTest(void);
extern "C" int Native_CoreTest(void);

// Decode the original title only at runtime from the user's IWAD. No game art
// enters the app bundle, and the resulting small layer image is cached once.
static id menuTitleArtwork() {
    static id artwork=nil;
    static bool tried=false; if(tried) return artwork; tried=true;
    int lump=W_CheckNumForName((char *)"M_DOOM");
    int colors=W_CheckNumForName((char *)"PLAYPAL");
    if(lump<0 || colors<0 || W_LumpLength(colors)<768) return nil;
    size_t length=W_LumpLength(lump); if(length<8) return nil;
    const auto *data=static_cast<const uint8_t *>(W_CacheLumpNum(lump,PU_CACHE));
    auto word=[&](size_t i) { return unsigned(data[i])|(unsigned(data[i+1])<<8); };
    unsigned w=word(0),h=word(2);
    if(!w || !h || w>1024 || h>512 || 8+size_t(w)*4>length) return nil;
    const auto *colorsRGB=static_cast<const uint8_t *>(W_CacheLumpNum(colors,PU_CACHE));
    std::vector<uint8_t> rgba(size_t(w)*h*4,0);
    for(unsigned x=0;x<w;++x) {
        size_t i=8+x*4;
        size_t post=unsigned(data[i])|(unsigned(data[i+1])<<8)|(unsigned(data[i+2])<<16)|(unsigned(data[i+3])<<24);
        while(post<length && data[post]!=255) {
            if(post+4>length || post+size_t(data[post+1])+4>length) return nil;
            for(unsigned y=0;y<data[post+1] && unsigned(data[post])+y<h;++y) {
                uint8_t *dest=rgba.data()+(size_t(data[post]+y)*w+x)*4;
                memcpy(dest,colorsRGB+data[post+3+y]*3,3); dest[3]=255;
            }
            post+=size_t(data[post+1])+4;
        }
    }
    NSData *bytes=[NSData dataWithBytes:rgba.data() length:rgba.size()];
    CGDataProviderRef provider=CGDataProviderCreateWithCFData((__bridge CFDataRef)bytes);
    CGColorSpaceRef cs=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef image=CGImageCreate(w,h,8,32,w*4,cs,kCGImageAlphaPremultipliedLast,provider,NULL,false,kCGRenderingIntentDefault);
    artwork=CFBridgingRelease(image); CGColorSpaceRelease(cs); CGDataProviderRelease(provider);
    return artwork;
}

const char *Native_IWADPath(void) { return iwadPath.fileSystemRepresentation; }
const char *Native_ConfigPath(void) { return configPath.fileSystemRepresentation; }
int Native_RegisteredIWAD(void) { return iwadInfo.registered; }
int Native_Time(void) { return gameClock.tics(nowNanos()); }
double Native_ClockTics(void) { return gameClock.nanos(nowNanos())*35.0/1e9; }
double Native_PresentationTics(void) {
    uint64_t target=framePresentationTimestamp>0?uint64_t((framePresentationTimestamp-frameClockLead)*1e9):nowNanos();
    target=std::max(target,gameClock.epoch+gameClock.excluded);
    return gameClock.nanos(target)*35.0/1e9;
}
int Native_IsActive(void) { return focused && awake; }
int Native_Enhanced(void) { return !diagnostic; }

static void postKey(int k,bool down) {
    if(k<0 || k>255) return;
    heldKeys[k]=down;
    event_t e{}; e.type=down?ev_keydown:ev_keyup; e.data1=k; D_PostEvent(&e);
}
static void releaseInput() {
    for(int k=0;k<256;++k) if(heldKeys[k]) postKey(k,false);
    std::fill_n(physicalKeys,128,0);
    event_t e{}; e.type=ev_mouse; D_PostEvent(&e);
    Native_ControllerReset();
}
static void centerCapturedCursor() {
    if(!window || !focused) return;
    NSRect bounds=window.contentView.bounds;
    NSPoint local=[window.contentView convertPoint:NSMakePoint(NSMidX(bounds),NSMidY(bounds)) toView:nil];
    NSPoint screen=[window convertPointToScreen:local];
    CGFloat primaryTop=NSMaxY(NSScreen.screens.firstObject.frame);
    CGWarpMouseCursorPosition(CGPointMake(screen.x,primaryTop-screen.y));
}
static void captureMouse(bool value) {
    if(captured==value) return;
    captured=value;
    CGAssociateMouseAndMouseCursorPosition(!value);
    if(value) { centerCapturedCursor(); [NSCursor hide]; } else [NSCursor unhide];
}
static void setFocus(bool value) {
    focused=value;
    if(@available(macOS 14.0,*)) [(CAMetalDisplayLink *)displayLink setPaused:!focused || !awake];
    gameClock.suspend(!focused || !awake,nowNanos());
    if(engineStarted) Native_AudioSuspend(!focused || !awake);
    if(!value) { readyDrawable=frameDrawable=nil; framePresentationTimestamp=0; presentationClockAligned=false; captureMouse(false); if(engineStarted) releaseInput(); }
}
static int keyForEvent(NSEvent *e) {
    switch(e.keyCode) {
    case 123:return KEY_LEFTARROW; case 124:return KEY_RIGHTARROW;
    case 125:return KEY_DOWNARROW; case 126:return KEY_UPARROW;
    case 36:case 76:return KEY_ENTER; case 53:return KEY_ESCAPE;
    case 51:case 117:return KEY_BACKSPACE; case 48:return KEY_TAB;
    case 122:return KEY_F1; case 120:return KEY_F2; case 99:return KEY_F3;
    case 118:return KEY_F4; case 96:return KEY_F5; case 97:return KEY_F6;
    case 98:return KEY_F7; case 100:return KEY_F8; case 101:return KEY_F9;
    case 109:return KEY_F10; case 103:return KEY_F11; case 111:return KEY_F12;
    // Number row by position, like the original scancodes: Shift (run) must not
    // turn weapon keys into punctuation.
    case 18:return '1'; case 19:return '2'; case 20:return '3'; case 21:return '4';
    case 23:return '5'; case 22:return '6'; case 26:return '7'; case 28:return '8';
    case 25:return '9'; case 29:return '0'; case 27:return KEY_MINUS; case 24:return KEY_EQUALS;
    }
    NSString *s=e.charactersIgnoringModifiers.lowercaseString;
    if(s.length && [s characterAtIndex:0]<128) return [s characterAtIndex:0];
    return -1;
}

// The pointer is blank over the game while nothing is pointed with; menus
// led by the mouse keep it.
static bool pointerBlank=false;
static NSCursor *blankCursor() {
    static NSCursor *cursor;
    if(!cursor) {
        NSImage *image=[NSImage imageWithSize:NSMakeSize(16,16) flipped:NO drawingHandler:^BOOL(NSRect) { return YES; }];
        cursor=[[NSCursor alloc] initWithImage:image hotSpot:NSMakePoint(0,0)];
    }
    return cursor;
}
@interface DoomView : NSView {
    NSTrackingArea *trackingArea;
}
@end

@interface DoomWindow : NSWindow
@end
@implementation DoomWindow
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }
@end

@implementation DoomView
- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (void)resetCursorRects {
    if(pointerBlank) [self addCursorRect:self.bounds cursor:blankCursor()];
}
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if(trackingArea) [self removeTrackingArea:trackingArea];
    trackingArea=[[NSTrackingArea alloc] initWithRect:self.bounds options:NSTrackingMouseMoved|NSTrackingActiveInKeyWindow|NSTrackingInVisibleRect owner:self userInfo:nil];
    [self addTrackingArea:trackingArea];
}
- (void)keyDown:(NSEvent *)e {
    if(e.modifierFlags&NSEventModifierFlagCommand) {
        NSString *key=e.charactersIgnoringModifiers.lowercaseString;
        if(!e.isARepeat && [key isEqual:@"q"] && engineStarted && !terminating) M_NativeQuitPrompt();
        if(!e.isARepeat && [key isEqual:@"f"]) Native_ToggleFullscreen();
        if(!e.isARepeat && [key isEqual:@"h"]) Native_ToggleHUD();
        return;
    }
    int k=keyForEvent(e);
    if(e.keyCode<128) {
        if(physicalKeys[e.keyCode] && physicalKeys[e.keyCode]!=k) postKey(physicalKeys[e.keyCode],false);
        physicalKeys[e.keyCode]=k>0?k:0;
    }
    postKey(k,true);
}
- (void)keyUp:(NSEvent *)e {
    int k=keyForEvent(e);
    if(e.keyCode<128) {
        if(physicalKeys[e.keyCode]) k=physicalKeys[e.keyCode];
        physicalKeys[e.keyCode]=0;
        if(k>0 && std::find(physicalKeys,physicalKeys+128,k)!=physicalKeys+128) return; // e.g. Return + keypad Enter
    }
    postKey(k,false);
}
- (void)flagsChanged:(NSEvent *)e {
    switch(e.keyCode) {
    case 56:case 60:postKey(KEY_RSHIFT,(e.modifierFlags&NSEventModifierFlagShift)!=0);break;
    case 58:case 61:postKey(KEY_RALT,(e.modifierFlags&NSEventModifierFlagOption)!=0);break;
    case 59:case 62:postKey(KEY_RCTRL,(e.modifierFlags&NSEventModifierFlagControl)!=0);break;
    }
}
- (void)mouseEvent:(NSEvent *)e {
    if(!focused) return;
    event_t d{}; d.type=ev_mouse;
    NSUInteger b=[NSEvent pressedMouseButtons];
    d.data1=(b&1?1:0)|(b&2?2:0)|(b&4?4:0);
    double dx=std::isfinite(e.deltaX)?std::clamp(double(e.deltaX),-8192.0,8191.75):0;
    double dy=std::isfinite(e.deltaY)?std::clamp(double(e.deltaY),-8191.75,8192.0):0;
    d.data2=int(std::lround(dx*4));
    d.data3=int(std::lround(-dy*4));
    D_PostEvent(&d);
}
- (void)mouseDown:(NSEvent *)e {
    if(menuOverlay.shown()) {
        NSPoint p=[self convertPoint:e.locationInWindow fromView:nil];
        int row=menuOverlay.hit(CGPointMake(p.x,p.y));
        if(row>=0) { Native_MenuSelect(row); postKey(KEY_ENTER,true); postKey(KEY_ENTER,false); }
        return;
    }
    [self mouseEvent:e];
}
- (void)mouseUp:(NSEvent *)e { [self mouseEvent:e]; }
- (void)rightMouseDown:(NSEvent *)e { [self mouseEvent:e]; }
- (void)rightMouseUp:(NSEvent *)e { [self mouseEvent:e]; }
- (void)otherMouseDown:(NSEvent *)e { [self mouseEvent:e]; }
- (void)otherMouseUp:(NSEvent *)e { [self mouseEvent:e]; }
- (void)mouseMoved:(NSEvent *)e {
    if(menuOverlay.shown()) return; // Native_Present follows the pointer over the menu.
    [self mouseEvent:e];
}
- (void)mouseDragged:(NSEvent *)e { if(!menuOverlay.shown()) [self mouseEvent:e]; }
- (void)rightMouseDragged:(NSEvent *)e { if(!menuOverlay.shown()) [self mouseEvent:e]; }
- (void)otherMouseDragged:(NSEvent *)e { if(!menuOverlay.shown()) [self mouseEvent:e]; }
@end

// The app presents 10-bit; the self-test renders to an 8-bit sRGB target.
static MTLPixelFormat outputFormat=MTLPixelFormatBGR10A2Unorm;
static bool initMetal() {
    device=MTLCreateSystemDefaultDevice();
    if(!device) return false;
    queue=[device newCommandQueue];
    NSString *path=[[NSBundle mainBundle] pathForResource:@"doom" ofType:@"metallib"];
    if(!path) return false;
    NSError *error=nil;
    id<MTLLibrary> library=[device newLibraryWithURL:[NSURL fileURLWithPath:path] error:&error];
    if(!library) { fprintf(stderr,"Metal library: %s\n",error.localizedDescription.UTF8String); return false; }
    MTLRenderPipelineDescriptor *desc=[MTLRenderPipelineDescriptor new];
    desc.vertexFunction=[library newFunctionWithName:@"doom_vertex"];
    MTLFunctionConstantValues *constants=[MTLFunctionConstantValues new];
    bool encode=outputFormat==MTLPixelFormatBGR10A2Unorm;
    [constants setConstantValue:&encode type:MTLDataTypeBool atIndex:0];
    desc.fragmentFunction=[library newFunctionWithName:@"doom_fragment" constantValues:constants error:&error];
    desc.colorAttachments[0].pixelFormat=outputFormat;
    pipeline=[device newRenderPipelineStateWithDescriptor:desc error:&error];
    desc.fragmentFunction=[library newFunctionWithName:@"doom_status_fragment" constantValues:constants error:&error];
    desc.colorAttachments[0].blendingEnabled=YES;
    desc.colorAttachments[0].sourceRGBBlendFactor=MTLBlendFactorSourceAlpha;
    desc.colorAttachments[0].destinationRGBBlendFactor=MTLBlendFactorOneMinusSourceAlpha;
    desc.colorAttachments[0].sourceAlphaBlendFactor=MTLBlendFactorOne;
    desc.colorAttachments[0].destinationAlphaBlendFactor=MTLBlendFactorOneMinusSourceAlpha;
    statusPipeline=[device newRenderPipelineStateWithDescriptor:desc error:&error];
    if(!menuOverlay.setupMetal(device,library,outputFormat,encode)) return false;
    // One submitted frame at a time; two drawable/texture sets permit CPU work
    // to overlap presentation without building a queue of stale input frames.
    uploadSemaphore=dispatch_semaphore_create(2);
    return queue && pipeline && statusPipeline;
}
// Streaming frame textures are linear and backed by shared buffers, so an
// upload is a plain row copy (spread across cores) instead of the CPU
// tiling and compression that replaceRegion performs.
static void uploadTexture(id<MTLTexture> __strong &texture,MTLPixelFormat format,unsigned width,unsigned height,const void *data,unsigned bytes) {
    if(texture.width!=width || texture.height!=height || !texture.buffer) {
        NSUInteger align=[device minimumLinearTextureAlignmentForPixelFormat:format];
        NSUInteger rowBytes=(NSUInteger(width)*bytes+align-1)/align*align;
        id<MTLBuffer> buffer=[device newBufferWithLength:rowBytes*height options:MTLResourceStorageModeShared];
        MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width height:height mipmapped:NO];
        td.storageMode=MTLStorageModeShared; td.usage=MTLTextureUsageShaderRead;
        texture=[buffer newTextureWithDescriptor:td offset:0 bytesPerRow:rowBytes];
    }
    uint8_t *destination=static_cast<uint8_t *>(texture.buffer.contents);
    const uint8_t *source=static_cast<const uint8_t *>(data);
    const size_t rowBytes=texture.bufferBytesPerRow,length=size_t(width)*bytes;
    if(rowBytes==length) { memcpy(destination,source,length*height); return; }
    constexpr unsigned Band=64;
    dispatch_apply((height+Band-1)/Band,dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE,0),^(size_t band) {
        for(unsigned y=unsigned(band)*Band;y<std::min(height,unsigned(band+1)*Band);++y)
            memcpy(destination+y*rowBytes,source+size_t(y)*length,length);
    });
}
// A gameplay view rendered below its native size by the adaptive resolution;
// classic 320x200 screens always keep whole pixels.
static bool reducedView(unsigned width,unsigned height,native_doom::Viewport v) {
    if(width==320 && height==200) return false;
    double clamp=std::min({1.0,NATIVE_MAX_WIDTH/double(v.width),NATIVE_MAX_HEIGHT/double(v.height)});
    return width+1<std::llround(v.width*clamp) || height+1<std::llround(v.height*clamp);
}
static id<MTLCommandBuffer> encodeFrame(id<MTLTexture> target,const uint8_t *pixels,native_doom::Viewport v,unsigned width=320,unsigned height=200) {
    unsigned slot=uploadIndex++%2;
    if(dispatch_semaphore_wait(uploadSemaphore,dispatch_time(DISPATCH_TIME_NOW,2*NSEC_PER_SEC)))
        I_Error((char *)"Metal presentation did not complete within two seconds");
    MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture=target; pass.colorAttachments[0].loadAction=MTLLoadActionClear;
    pass.colorAttachments[0].storeAction=MTLStoreActionStore; pass.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,1);
    id<MTLCommandBuffer> cmd=[queue commandBuffer];
    bool level=!diagnostic && gamestate==GS_LEVEL && !automapactive && height!=200;
    updateLinearPalette();
    uploadTexture(indices[slot],MTLPixelFormatR8Uint,width,height,pixels,1);
    uploadTexture(lighting[slot],MTLPixelFormatR16Uint,width,height,Native_LightingPixels(width,height),2);
    uploadTexture(viewmodels[slot],MTLPixelFormatR16Uint,320,200,Native_ViewmodelPixels(),2);
    // The status bar card at the foot of the view: the bar at a whole-number
    // scale (every original pixel an equal square), as large as fits in 600
    // points and 80% of the width; the weapon art rises above it.
    float hudWidth=0,hudHeight=0,hudMargin=0;
    if(level && statusBar) {
        double scale=window?window.backingScaleFactor:1;
        // The largest whole multiple of the original bar within 55% of the
        // width and an eighth of the height: readable, never in the way.
        double k=std::max(1.0,std::floor(std::min(v.width*.55/ST_WIDTH,v.height*.125/ST_HEIGHT)));
        hudWidth=float(ST_WIDTH*k); hudHeight=float(ST_HEIGHT*k); hudMargin=std::round(8*scale);
    }
    id<MTLRenderCommandEncoder> enc=[cmd renderCommandEncoderWithDescriptor:pass];
    [enc setRenderPipelineState:pipeline]; [enc setViewport:(MTLViewport){v.x,v.y,v.width,v.height,0,1}];
    [enc setFragmentTexture:indices[slot] atIndex:0]; [enc setFragmentBytes:linearPalette length:sizeof(linearPalette) atIndex:0];
    [enc setFragmentTexture:lighting[slot] atIndex:2];
    float scale=std::min(float(v.height)/240.0f,float(v.width)/320.0f);
    float rect[]={float(v.x+(v.width-320*scale)/2),float(v.y+v.height-240*scale-hudHeight-hudMargin),level?320*scale:0,level?240*scale:0};
    uint8_t fallback[256]={}; const uint8_t *fuzz=Native_ViewmodelFuzzMap();
    [enc setFragmentTexture:viewmodels[slot] atIndex:1]; [enc setFragmentBytes:rect length:sizeof(rect) atIndex:1];
    [enc setFragmentBytes:fuzz?fuzz:fallback length:256 atIndex:2];
    // The 32 light levels of COLORMAP, so the shader can confirm an RGB-lit
    // pixel was not overdrawn before lighting its stored texel.
    static id<MTLBuffer> colormapBuffer; static const void *colormapSource=nullptr;
    if(colormaps!=colormapSource || !colormapBuffer) {
        colormapSource=colormaps;
        std::vector<uint8_t> table(32*256,0); if(colormaps) memcpy(table.data(),colormaps,table.size());
        colormapBuffer=[device newBufferWithBytes:table.data() length:table.size() options:MTLResourceStorageModeShared];
    }
    [enc setFragmentBuffer:colormapBuffer offset:0 atIndex:3];
    uint32_t filtered=reducedView(width,height,v);
    [enc setFragmentBytes:&filtered length:sizeof(filtered) atIndex:4];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    if(hudHeight>0) {
        uploadTexture(statusbars[slot],MTLPixelFormatR8Uint,ST_WIDTH,ST_HEIGHT,ST_NativeStatus(),1);
        [enc setRenderPipelineState:statusPipeline];
        [enc setViewport:(MTLViewport){std::floor(v.x+(v.width-hudWidth)/2),v.y+v.height-hudHeight-hudMargin,hudWidth,hudHeight,0,1}];
        [enc setFragmentTexture:statusbars[slot] atIndex:0]; [enc setFragmentBytes:linearPalette length:sizeof(linearPalette) atIndex:0];
        float shape[]={hudWidth,hudHeight,float(6*(window?window.backingScaleFactor:1)),0};
        [enc setFragmentBytes:shape length:sizeof(shape) atIndex:1];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    menuOverlay.encode(enc,target.width,target.height);
    [enc endEncoding];
    [cmd addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        if(completed.status==MTLCommandBufferStatusError) { gpuFailed=true; fprintf(stderr,"Metal error: %s\n",completed.error.localizedDescription.UTF8String); }
        dispatch_semaphore_signal(uploadSemaphore);
    }];
    return cmd;
}

void Native_SetPalette(const uint8_t *p) {
    for(int i=0;i<256;++i) { for(int c=0;c<3;++c) palette[i][c]=gammatable[usegamma][p[i*3+c]]; palette[i][3]=255; }
}
void Native_BeginFrame(void) {
    if(!window || !Native_IsActive() || !window.visible || window.miniaturized) return;
    NSRect bounds=[window.contentView convertRectToBacking:window.contentView.bounds];
    if(bounds.size.width<1 || bounds.size.height<1) return;
    metalLayer.contentsScale=window.backingScaleFactor; metalLayer.drawableSize=bounds.size;
    // Clamp the whole image uniformly on displays beyond the renderer's capacity.
    double scale=std::min({1.0,NATIVE_MAX_WIDTH/bounds.size.width,NATIVE_MAX_HEIGHT/bounds.size.height});
    unsigned w=scaledSize(std::max(1.0,double(std::llround(bounds.size.width*scale))));
    unsigned h=scaledSize(std::max(1.0,double(std::llround(bounds.size.height*scale))));
    if(!diagnostic && (w!=requestedWidth || h!=requestedHeight)) {
        requestedWidth=w; requestedHeight=h; R_SetViewSize(screenblocks,detailLevel);
    }
    if(frameDrawable) return; // The display-link callback owns this frame's drawable.
    if(displayLink) {
        while(!readyDrawable && Native_IsActive()) {
            Native_Pump();
            // Short slices: the display-link update does not always end a
            // run-loop wait early, and a 50 ms slice cost three refreshes.
            if(!readyDrawable && Native_IsActive())
                [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:.002]];
        }
        frameDrawable=readyDrawable; framePresentationTimestamp=readyPresentationTimestamp;
        frameClockLead=readyClockLead; readyDrawable=nil;
    } else { frameDrawable=[metalLayer nextDrawable]; framePresentationTimestamp=0; }
}
void Native_Present(const uint8_t *pixels) {
    @autoreleasepool {
        if(!focused || !awake || !window.visible || window.miniaturized) return;
        NSRect bounds=[window.contentView convertRectToBacking:window.contentView.bounds];
        [CATransaction begin]; [CATransaction setDisableActions:YES];
        char title[1024]; int count=Native_MenuTitle(title,sizeof(title));
        if(count<0) menuOverlay.close();
        else {
            NativeMenuPage page; page.identity=title; page.selected=Native_MenuSelection();
            bool notice=messageToPrint || count==0;
            const char *more=strchr(title,'\n');
            for(int i=0;i<count;++i) {
                char label[128]; int status=Native_MenuRow(i,label,sizeof(label)); if(status<0) continue;
                page.rows.push_back({[NSString stringWithUTF8String:label],status,i});
            }
            if(notice) { page.body=[NSString stringWithUTF8String:title]; page.heading=@"DOOM"; }
            else if(more) {
                // A page with notes: its name, then the notes above the rows.
                page.heading=[[NSString alloc] initWithBytes:title length:size_t(more-title) encoding:NSUTF8StringEncoding];
                page.body=[NSString stringWithUTF8String:more+1];
            } else {
                page.heading=[NSString stringWithUTF8String:title];
                if(!strcmp(title,Native_GameTitle())) page.logo=menuTitleArtwork();
            }
            // The connected controller's button names, or the keys alone.
            const char *yes=Native_PadButton(NATIVE_BUTTON_SOUTH),*no=Native_PadButton(NATIVE_BUTTON_EAST);
            NSString *ok=yes?[NSString stringWithFormat:@"ENTER / %s",yes]:@"ENTER",*back=no?[NSString stringWithFormat:@"ESC / %s",no]:@"ESC";
            page.hint=(count==0?[NSString stringWithFormat:@"%@  ·  %@",ok,back]:[NSString stringWithFormat:@"%@  ·  %@  ·  %@",yes?@"↑↓ / STICK":@"↑↓",ok,back]).uppercaseString;
            menuOverlay.present(window.contentView.bounds,window.screen.safeAreaInsets.top,window.backingScaleFactor,page);
            // The pointer selects the row it moves onto; a still pointer
            // leaves keyboard and controller selection alone.
            static NSPoint lastPointer={-1,-1};
            NSPoint pointer=[window.contentView convertPoint:window.mouseLocationOutsideOfEventStream fromView:nil];
            if(!NSEqualPoints(pointer,lastPointer)) {
                bool moved=lastPointer.x>=0; lastPointer=pointer;
                int row=menuOverlay.hit(CGPointMake(pointer.x,pointer.y));
                if(moved && row>=0) Native_MenuSelect(row);
            }
        }
        [CATransaction commit];
        if(bounds.size.width<1 || bounds.size.height<1) return;
        if(!frameDrawable) Native_BeginFrame(); // Wipes can present more than once per game loop.
        id<CAMetalDrawable> drawable=frameDrawable; frameDrawable=nil;
        if(!drawable) return;
        native_doom::Viewport viewport=diagnostic?native_doom::viewport(bounds.size.width,bounds.size.height,true):native_doom::Viewport{0,0,bounds.size.width,bounds.size.height};
        bool high=gamestate==GS_LEVEL && !automapactive && gametic && screenblocks==11 && (native_render_width!=320 || native_render_height!=200);
        id<MTLCommandBuffer> cmd=encodeFrame(drawable.texture,high?Native_RenderPixels():pixels,viewport,high?native_render_width:320,high?native_render_height:200);
        [cmd presentDrawable:drawable]; [cmd commit];
        if(gpuFailed) I_Error((char *)"Metal presentation failed");
    }
}
void Native_QuitFade(void) { NativeFadeOutWindow(window,.9); }
void Native_Shutdown(void) {
    if(terminating) return; terminating=true;
    scheduledFrame=nullptr;
    Native_ControllerReset(); captureMouse(false);
    if(@available(macOS 14.0,*)) [displayLink invalidate]; readyDrawable=frameDrawable=nil;
    [GCController stopWirelessControllerDiscovery];
    if(queue) { id<MTLCommandBuffer> drain=[queue commandBuffer]; [drain commit]; [drain waitUntilCompleted]; }
    if(fullscreenView) {
        [fullscreenView exitFullScreenModeWithOptions:nil];
        NSApp.presentationOptions=windowedPresentationOptions;
        window=windowedWindow; windowedWindow=nil; fullscreenView=nil;
    }
    [window orderOut:nil];
}
void Native_Error(const char *message) {
    captureMouse(false);
    if(diagnostic) return;
    NSAlert *alert=[NSAlert new]; alert.messageText=@"DOOM could not continue";
    alert.informativeText=[NSString stringWithUTF8String:message]; [alert runModal];
}
void Native_Pump(void) {
    if(interruptRequested && engineStarted) I_Quit();
    if(!engineStarted) return;
    @autoreleasepool {
        do {
            if(interruptRequested) I_Quit();
            NSEvent *e;
            while((e=[NSApp nextEventMatchingMask:NSEventMaskAny untilDate:NSDate.distantPast inMode:NSDefaultRunLoopMode dequeue:YES])) [NSApp sendEvent:e];
            [NSApp updateWindows];
            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:(focused && awake)?[NSDate dateWithTimeIntervalSinceNow:.00005]:[NSDate dateWithTimeIntervalSinceNow:.05]];
        } while(!focused || !awake);
        captureMouse(focused && gamestate==GS_LEVEL && !menuactive && !paused && !demoplayback);
        bool blank=focused && !menuactive;
        if(blank!=pointerBlank) {
            pointerBlank=blank;
            [window invalidateCursorRectsForView:window.contentView];
            if(blank) [NSCursor setHiddenUntilMouseMoves:YES];
        }
    }
}

// Renders indexed frames through the real pipeline into an 8-bit target and
// compares them with the palette: orientation, colors and light blends.
static int metalTest() {
    outputFormat=MTLPixelFormatBGRA8Unorm_sRGB;
    diagnostic=true;
    if(!initMetal()) { fprintf(stderr,"No Metal device or pipeline\n"); return 1; }
    std::vector<uint8_t> src(320*200), expected(320*200*4), out(expected.size());
    MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm_sRGB width:320 height:200 mipmapped:NO];
    td.usage=MTLTextureUsageRenderTarget; td.storageMode=MTLStorageModeShared;
    id<MTLTexture> target=[device newTextureWithDescriptor:td];
    Native_LightingBegin(320,200);
    for(unsigned pass=0;pass<3;++pass) {
        for(unsigned i=0;i<256;++i) { palette[i][0]=(i+pass*37)%256; palette[i][1]=255-i; palette[i][2]=i^0x55; palette[i][3]=255; }
        for(unsigned i=0;i<src.size();++i) { src[i]=(i+(i/320)*13)%256; for(int c=0;c<4;++c) expected[i*4+c]=palette[src[i]][c==0?2:(c==2?0:c)]; }
        id<MTLCommandBuffer> cmd=encodeFrame(target,src.data(),{0,0,320,200}); [cmd commit]; [cmd waitUntilCompleted];
        if(cmd.status==MTLCommandBufferStatusError) return 1;
        [target getBytes:out.data() bytesPerRow:320*4 fromRegion:MTLRegionMake2D(0,0,320,200) mipmapLevel:0];
        // Ordered dither adds up to half a step, so allow one code value.
        for(size_t i=0;i<out.size();++i) if(std::abs(int(out[i])-int(expected[i]))>1) { fprintf(stderr,"Palette/orientation mismatch on pass %u at byte %zu: %d vs %d\n",pass,i,int(out[i]),int(expected[i])); return 1; }
    }
    printf("Metal: 192000 indexed pixels verified across three palettes on %s\n",device.name.UTF8String);
    uint16_t *shade=Native_LightingBegin(320,200);
    for(unsigned i=0;i<src.size();i++) shade[i]=((i%3)*63<<8)|((src[i]+17)&255); // weights 0, 63, 126; bit 15 is the RGB-lit flag
    id<MTLCommandBuffer> cmd=encodeFrame(target,src.data(),{0,0,320,200}); [cmd commit]; [cmd waitUntilCompleted];
    if(cmd.status==MTLCommandBufferStatusError) return 1;
    [target getBytes:out.data() bytesPerRow:320*4 fromRegion:MTLRegionMake2D(0,0,320,200) mipmapLevel:0];
    for(unsigned i=0;i<src.size();i++) for(unsigned c=0;c<3;c++) {
        unsigned channel=c==0?2:(c==2?0:c); double weight=((shade[i]>>8)&127)/127.0; // 7-bit blend weight, as the shader decodes it
        double value=linearPalette[src[i]][channel]*(1-weight)+linearPalette[shade[i]&255][channel]*weight;
        double encoded=value<=.0031308?value*12.92:1.055*std::pow(value,1/2.4)-.055;
        if(std::abs(int(out[i*4+c])-int(std::lround(encoded*255)))>1) { fprintf(stderr,"Linear light blend mismatch at %u\n",i); return 1; }
    }
    printf("Metal: 64000 adjacent light blends verified\n");
    return 0;
}

// Settings and saves: Application Support/MetalDoom, or the folder given
// with --home (a portable or test setup).
static NSString *homeFolder=nil;
static NSString *supportFolder() { return homeFolder?:[NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/MetalDoom"]; }
// Game data: the DOOM.WAD chosen before (remembered), one in the support
// folder or a known Steam location, or one the player selects.
static bool usableIWAD(NSString *path,std::string &error) {
    native_doom::ContentProfile profile;
    return path && native_doom::contentProfile(path.fileSystemRepresentation,{},profile,error) && profile.supported;
}
static NSString *findIWAD() {
    NSUserDefaults *prefs=NSUserDefaults.standardUserDefaults;
    std::string error;
    NSMutableArray<NSString *> *candidates=[NSMutableArray array];
    if(NSString *saved=[prefs stringForKey:@"IWADPath"]) [candidates addObject:saved];
    for(NSString *name in @[@"DOOM.WAD",@"doom.wad",@"DOOMU.WAD",@"doomu.wad",@"DOOM1.WAD",@"doom1.wad"]) [candidates addObject:[supportFolder() stringByAppendingPathComponent:name]];
    NSString *steam=[NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/Steam/steamapps/common"];
    for(NSString *game in @[@"Ultimate Doom/base",@"Ultimate Doom/rerelease",@"Ultimate Doom",@"DOOM + DOOM II/base",@"Doom/base",@"Doom"]) {
        [candidates addObject:[[steam stringByAppendingPathComponent:game] stringByAppendingPathComponent:@"DOOM.WAD"]];
        [candidates addObject:[[steam stringByAppendingPathComponent:game] stringByAppendingPathComponent:@"doom.wad"]];
    }
    for(NSString *path in candidates) if([NSFileManager.defaultManager isReadableFileAtPath:path] && usableIWAD(path,error)) return path;
    // Ask once; a refused file explains why and asks again.
    [NSApp activateIgnoringOtherApps:YES];
    for(;;) {
        NSOpenPanel *panel=[NSOpenPanel openPanel];
        panel.canChooseFiles=YES; panel.canChooseDirectories=NO; panel.allowsMultipleSelection=NO;
        panel.title=@"Select DOOM.WAD";
        panel.message=@"MetalDoom needs the game data from your copy of DOOM or The Ultimate DOOM (DOOM.WAD). The file stays where it is.";
        panel.prompt=@"Play";
        if([panel runModal]!=NSModalResponseOK) return nil;
        NSString *path=panel.URL.path;
        error.clear();
        if(usableIWAD(path,error)) { [prefs setObject:path forKey:@"IWADPath"]; return path; }
        NSAlert *alert=[NSAlert new]; alert.messageText=@"This file cannot be used";
        alert.informativeText=[NSString stringWithFormat:@"%s\n\nSelect DOOM.WAD from DOOM, The Ultimate DOOM or the DOOM shareware release.",error.empty()?"It is not original DOOM game data.":error.c_str()];
        [alert runModal];
    }
}

@interface DoomApp : NSObject <NSApplicationDelegate,NSWindowDelegate,CAMetalDisplayLinkDelegate>
- (void)metalDisplayLink:(CAMetalDisplayLink *)link needsUpdate:(CAMetalDisplayLinkUpdate *)update API_AVAILABLE(macos(14.0));
@end
@implementation DoomApp
- (void)metalDisplayLink:(CAMetalDisplayLink *)link needsUpdate:(CAMetalDisplayLinkUpdate *)update {
    if(Native_IsActive() && window.visible && !window.miniaturized) {
        // The scheduler predicts presentation several refresh intervals ahead.
        // Align that timeline with completed 35 Hz states once, then advance it
        // exclusively by presentation timestamps. Rebase only if the scheduler
        // shifts its lead by half a refresh interval (startup or recovery).
        CFTimeInterval lead=update.targetPresentationTimestamp-nowNanos()/1e9;
        double rate=std::max(1L,window.screen.maximumFramesPerSecond);
        if(!presentationClockAligned || fabs(lead-calibratedClockLead)>.5/rate) {
            calibratedClockLead=lead; presentationClockAligned=true;
        }
        readyDrawable=update.drawable; readyPresentationTimestamp=update.targetPresentationTimestamp;
        readyClockLead=calibratedClockLead;
        if(scheduledFrame && !runningFrame) {
            // Finish encoding inside the update callback, before its deadline.
            // Holding updates for the outer game loop can submit obsolete drawables.
            frameDrawable=readyDrawable; framePresentationTimestamp=readyPresentationTimestamp;
            frameClockLead=readyClockLead; readyDrawable=nil;
            double start=CACurrentMediaTime();
            runningFrame=true; scheduledFrame(); runningFrame=false;
            governRenderScale((CACurrentMediaTime()-start)*1000,window.screen);
        }
    }
}
- (void)startEngine {
    if(fullscreen && !systemFullscreenReady) { [self performSelector:@selector(startEngine) withObject:nil afterDelay:.02]; return; }
    myargc=mainArgc; myargv=mainArgv; gameClock=native_doom::Clock(nowNanos());
    engineStarted=true; D_DoomMain();
}
// Command-Q and closing the window ask with the game's quit confirmation;
// confirming saves settings and quits (I_Quit).
- (void)quit:(id)sender {
    if(engineStarted && !terminating) { M_NativeQuitPrompt(); return; }
    if(engineStarted) I_Quit(); [NSApp terminate:nil];
}
- (void)fullscreen:(id)sender {
    captureMouse(false);
    if(engineStarted) releaseInput();
    if(windowedWindow) {
        [fullscreenView exitFullScreenModeWithOptions:nil];
        window=windowedWindow; windowedWindow=nil; fullscreenView=nil;
        NSApp.presentationOptions=windowedPresentationOptions;
        fullscreen=false; systemFullscreenReady=false;
        [window makeKeyAndOrderFront:nil]; [window makeFirstResponder:window.contentView];
        presentationClockAligned=false;
    } else if(NSScreen *screen=window.screen?:NSScreen.mainScreen; screen.safeAreaInsets.top>0 && !(window.styleMask&NSWindowStyleMaskFullScreen)) {
        // A notched display: the view covers the whole screen, notch area included.
        NSView *view=window.contentView; NSWindow *original=window;
        windowedPresentationOptions=NSApp.presentationOptions;
        NSApp.presentationOptions=NSApplicationPresentationHideDock|NSApplicationPresentationHideMenuBar;
        if([view enterFullScreenMode:screen withOptions:@{NSFullScreenModeAllScreens:@NO,NSFullScreenModeApplicationPresentationOptions:@(NSApp.presentationOptions)}]) {
            windowedWindow=original; fullscreenView=view; window=view.window;
            window.delegate=self; window.opaque=YES; window.backgroundColor=NSColor.blackColor;
            window.hasShadow=NO; window.acceptsMouseMovedEvents=YES;
            [window setFrame:screen.frame display:YES];
            [original orderOut:nil]; [window makeKeyAndOrderFront:nil]; [window makeFirstResponder:view];
            fullscreen=true; systemFullscreenReady=true;
            presentationClockAligned=false;
        } else NSApp.presentationOptions=windowedPresentationOptions;
    } else { fullscreen=!(window.styleMask&NSWindowStyleMaskFullScreen); systemFullscreenReady=false; [window toggleFullScreen:nil]; }
}
- (void)applicationDidResignActive:(NSNotification *)n { setFocus(false); }
- (void)applicationDidBecomeActive:(NSNotification *)n { setFocus(true); }
- (void)windowDidMiniaturize:(NSNotification *)n { setFocus(false); }
- (void)windowDidDeminiaturize:(NSNotification *)n { setFocus(NSApp.active); }
- (NSSize)window:(NSWindow *)w willUseFullScreenContentSize:(NSSize)proposed { return w.screen.frame.size; }
- (NSApplicationPresentationOptions)window:(NSWindow *)w willUseFullScreenPresentationOptions:(NSApplicationPresentationOptions)options {
    return (options&~(NSApplicationPresentationAutoHideMenuBar|NSApplicationPresentationAutoHideDock))|NSApplicationPresentationHideMenuBar|NSApplicationPresentationHideDock;
}
- (void)windowWillEnterFullScreen:(NSNotification *)n { fullscreen=true; systemFullscreenReady=false; }
- (void)windowDidEnterFullScreen:(NSNotification *)n {
    fullscreen=true; systemFullscreenReady=true;
    [window makeFirstResponder:window.contentView]; presentationClockAligned=false;
}
- (void)windowWillExitFullScreen:(NSNotification *)n { fullscreen=false; systemFullscreenReady=false; }
- (void)windowDidFailToEnterFullScreen:(NSWindow *)w { fullscreen=false; systemFullscreenReady=false; }
- (void)windowDidExitFullScreen:(NSNotification *)n { fullscreen=false; [window makeFirstResponder:window.contentView]; presentationClockAligned=false; }
- (void)windowDidResize:(NSNotification *)n { if(captured) centerCapturedCursor(); }
- (void)windowDidChangeScreen:(NSNotification *)n {
    if(captured) centerCapturedCursor();
    presentationClockAligned=false;
    applyFrameRate(window.screen);
}
- (BOOL)windowShouldClose:(NSWindow *)w { [self quit:nil]; return NO; }
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { return NO; }
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app { if(engineStarted) I_Quit(); return NSTerminateNow; }
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    NSUserDefaults *prefs=NSUserDefaults.standardUserDefaults;
    [prefs registerDefaults:@{@"StatusBarHUD":@YES}];
    statusBar=[prefs boolForKey:@"StatusBarHUD"];
    // Every option lives in the in-game menu; the menu bar stays empty.
    NSApp.mainMenu=[NSMenu new];
    NSString *chosen=nil;
    auto absolute=[](NSString *path) {
        path=path.stringByStandardizingPath;
        return path.isAbsolutePath?path:[NSFileManager.defaultManager.currentDirectoryPath stringByAppendingPathComponent:path];
    };
    for(int i=1;i<mainArgc;++i) {
        if(!strcmp(mainArgv[i],"-iwad") && i+1<mainArgc) chosen=absolute([NSString stringWithUTF8String:mainArgv[++i]]);
        else if(!strcmp(mainArgv[i],"--home") && i+1<mainArgc) homeFolder=absolute([NSString stringWithUTF8String:mainArgv[++i]]);
    }
    if(!chosen && !(chosen=findIWAD())) exit(0);
    std::string error;
    if(!Native_ConfigureProfile(chosen.fileSystemRepresentation,{},error)) { Native_Error(error.c_str()); exit(1); }
    iwadPath=chosen.copy;
    iwadInfo.registered=sessionProfile.base.episodes>1; iwadInfo.ultimateData=sessionProfile.base.episodes==4;
    NSString *support=supportFolder();
    NSError *errorObj=nil;
    if(![NSFileManager.defaultManager createDirectoryAtPath:support withIntermediateDirectories:YES attributes:nil error:&errorObj]) { Native_Error(errorObj.localizedDescription.UTF8String); exit(1); }
    try { Native_ConfigureSaves(support.fileSystemRepresentation,false); }
    catch(const std::exception &e) { Native_Error(e.what()); exit(1); }
    configPath=[NSString stringWithUTF8String:Native_ProfileConfig()];
    if(chdir(support.fileSystemRepresentation)) { Native_Error("Cannot open application support directory"); exit(1); }
    if(!initMetal()) { Native_Error("A Metal device and the app's compiled shaders are required."); exit(1); }
    window=[[DoomWindow alloc] initWithContentRect:NSMakeRect(0,0,960,720) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskResizable|NSWindowStyleMaskMiniaturizable backing:NSBackingStoreBuffered defer:NO];
    window.title=[NSString stringWithUTF8String:Native_GameTitle()]; window.delegate=self; window.opaque=YES; window.backgroundColor=NSColor.blackColor; window.hasShadow=NO;
    window.styleMask|=NSWindowStyleMaskFullSizeContentView; window.titlebarAppearsTransparent=YES; window.titleVisibility=NSWindowTitleHidden;
    window.contentMinSize=NSMakeSize(320,240);
    window.collectionBehavior=NSWindowCollectionBehaviorFullScreenPrimary;
    DoomView *view=[[DoomView alloc] initWithFrame:window.contentView.bounds];
    metalLayer=[CAMetalLayer layer]; metalLayer.device=device;
    metalLayer.opaque=YES; metalLayer.backgroundColor=NSColor.blackColor.CGColor;
    view.wantsLayer=YES; view.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;
    metalLayer.pixelFormat=outputFormat;
    CGColorSpaceRef colorSpace=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    metalLayer.colorspace=colorSpace; CGColorSpaceRelease(colorSpace);
    metalLayer.wantsExtendedDynamicRangeContent=NO; metalLayer.framebufferOnly=YES; metalLayer.presentsWithTransaction=NO;
    metalLayer.displaySyncEnabled=YES; metalLayer.maximumDrawableCount=2; view.layer=metalLayer;
    menuOverlay.build(NativeMenuTheme::doom());
    window.contentView=view; window.acceptsMouseMovedEvents=YES;
    [window center]; [window makeKeyAndOrderFront:nil]; [window makeFirstResponder:view];
    [NSApp activateIgnoringOtherApps:YES];
    // Games start fullscreen.
    if(window.screen.safeAreaInsets.top>0) [self fullscreen:nil];
    else { fullscreen=true; systemFullscreenReady=false; [window toggleFullScreen:nil]; }
    if(@available(macOS 14.0,*)) {
        CAMetalDisplayLink *link=[[CAMetalDisplayLink alloc] initWithMetalLayer:metalLayer]; displayLink=link;
        link.delegate=self; link.preferredFrameLatency=1;
        float rate=window.screen.maximumFramesPerSecond;
        link.preferredFrameRateRange=CAFrameRateRangeMake(std::min(60.0f,rate),rate,rate);
        [link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    }
    Native_ControllersInit();
    NSNotificationCenter *nc=NSWorkspace.sharedWorkspace.notificationCenter;
    [nc addObserverForName:NSWorkspaceWillSleepNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { awake=false; setFocus(focused); captureMouse(false); Native_ControllerReset(); }];
    [nc addObserverForName:NSWorkspaceDidWakeNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { awake=true; setFocus(NSApp.active); }];
    // Enter outside a dispatch-main callback, so nested event pumping can service
    // GameController, Core Haptics and lifecycle callbacks on the main queue.
    [self performSelector:@selector(startEngine) withObject:nil afterDelay:0];
}
@end

int Native_Fullscreen(void) { return fullscreen; }
void Native_ToggleFullscreen(void) { [(DoomApp *)NSApp.delegate fullscreen:nil]; }

int main(int argc,char **argv) {
    @autoreleasepool {
        for(int i=1;i<argc;++i) {
            if(!strcmp(argv[i],"--metal-test")) return metalTest();
            if(!strcmp(argv[i],"--audio-test")) return Native_AudioTest();
            if(!strcmp(argv[i],"--core-test")) return Native_CoreTest();
            if(!strcmp(argv[i],"--validate-iwad") && i+1<argc) {
                native_doom::ContentProfile profile; std::string error;
                if(!native_doom::contentProfile(argv[i+1],{},profile,error) || !profile.supported) { fprintf(stderr,"%s\n",(error.empty()?profile.reason:error).c_str()); return 1; }
                printf("%s: %zu lumps; profile %s\n",profile.title.c_str(),profile.base.lumps.size(),profile.saveKey.c_str()); return 0;
            }
        }
        mainArgc=argc; mainArgv=argv;
        signal(SIGINT,requestInterrupt); signal(SIGTERM,requestInterrupt);
        [NSApplication sharedApplication]; [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        DoomApp *delegate=[DoomApp new]; NSApp.delegate=delegate; [NSApp run];
    }
    return 0;
}
