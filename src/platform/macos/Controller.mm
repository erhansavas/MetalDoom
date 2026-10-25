// SPDX-License-Identifier: GPL-2.0-only
// One game controller: play, menus, haptics and DualSense adaptive triggers.
#import <AppKit/AppKit.h>
#import <GameController/GameController.h>
#import <CoreHaptics/CoreHaptics.h>
#include "Native.h"
#include "Portable.hpp"
#include "Feedback.hpp"
#include "ControllerTrigger.hpp"
#include "Rumble.hpp"
extern "C" {
#include "doomstat.h"
#include "d_main.h"
#include "i_system.h"
extern boolean menuactive;
extern int messageToPrint;
}
#include <algorithm>
#include <cmath>

namespace {
// Fixed layout (DualSense names / Xbox names):
//   Cross/A Use, Circle/B Back, Square/X Run, Triangle/Y Next Weapon,
//   L1 Previous Weapon, R1 Next Weapon, L2 Strafe, R2 Fire,
//   Options/Menu Menu, Create/View Pause, Touchpad Automap,
//   D-pad Up/Right/Down/Left Weapons 1-4, L3 Run, R3 Use.
//   Left stick moves and strafes, right stick turns.
enum Input { Cross, Circle, Square, Triangle, L1, R1, L2, R2, MenuButton, PauseButton, Touchpad, Up, Right, Down, Left, L3, R3, InputCount };
GCController *controller=nil;
rumble::Device rumbleDevice;
bool previous[InputCount]{},triggerHeld[2]{};
native_doom::FireGate fireGate;
native_doom::FeedbackStream stream;
bool feedbackActive=false,commandFiring=false,respawnHeld=true,cycling=false;
int queuedWeapon=-1,repeatAt=0,previousDirection=-1;
int triggerWeapon=-1;
native_doom::TriggerState triggerState=native_doom::TriggerState::Ready;
double triggerRetryAt=0;
// Direction of the last hit (-1 left .. 1 right) for the next damage rumble.
float balance=0; double balanceUntil=0;
// Late-latched stick turn: the last command's turn and when it was built.
int lastTurn=0; double turnBuiltAt=0;
// Saved Controller settings.
float zone=.18f,sensitivity=1,hapticIntensity=1,triggerIntensity=1;
bool hapticsEnabled=true,triggersEnabled=true;

double uptime() { return NSProcessInfo.processInfo.systemUptime; }
GCDualSenseGamepad *dualPad() {
    GCPhysicalInputProfile *profile=controller.physicalInputProfile;
    return [profile isKindOfClass:GCDualSenseGamepad.class]?(GCDualSenseGamepad *)profile:nil;
}
float hapticScale() { return hapticsEnabled?hapticIntensity:0; }
float triggerScale() { return triggersEnabled?triggerIntensity:0; }
bool adaptiveTriggers() { return dualPad() && triggerScale()>0; }
constexpr int FireMask=2; // R2 fires
void triggersOff() {
    if(GCDualSenseGamepad *pad=dualPad()) { [pad.leftTrigger setModeOff]; [pad.rightTrigger setModeOff]; }
    triggerWeapon=-1;
}
void keyPulse(int key) { event_t e{}; e.type=ev_keydown; e.data1=key; D_PostEvent(&e); e.type=ev_keyup; D_PostEvent(&e); }
void stopFeedback() {
    fireGate.reset(); commandFiring=false; cycling=false;
    stream.reset(); rumbleDevice.stop(); triggersOff();
    feedbackActive=false;
}
void resetPad() {
    respawnHeld=true;
    stopFeedback(); std::fill_n(previous,InputCount,false); std::fill_n(triggerHeld,2,false); queuedWeapon=-1; previousDirection=-1;
}
void selectController(GCController *c) {
    resetPad();
    controller=c;
    rumbleDevice.attach(c);
    if(c) {
        c.handlerQueue=dispatch_get_main_queue(); c.playerIndex=GCControllerPlayerIndexUnset;
        fprintf(stderr,"Controller: %s; DualSense=%d; haptics=%d\n",c.vendorName.UTF8String,dualPad()!=nil,c.haptics!=nil);
    }
}
// The connected controller's button names as printed on it: PlayStation
// and Xbox by name, any other by the name the system gives each button.
static const char *buttonName(GCController *c,int button) {
    GCExtendedGamepad *p=c.extendedGamepad;
    if(!p || button<0 || button>NATIVE_BUTTON_TOUCHPAD) return nullptr;
    GCPhysicalInputProfile *profile=c.physicalInputProfile;
    bool sense=[profile isKindOfClass:GCDualSenseGamepad.class],shock=[profile isKindOfClass:GCDualShockGamepad.class],xbox=[profile isKindOfClass:GCXboxGamepad.class];
    GCControllerElement *elements[]={nil,p.buttonA,p.buttonB,p.buttonX,p.buttonY,p.leftShoulder,p.rightShoulder,p.leftTrigger,p.rightTrigger,p.buttonMenu,p.buttonOptions,nil};
    static const char *playstation[]={"","Cross","Circle","Square","Triangle","L1","R1","L2","R2","Options","Create",""};
    static const char *box[]={"","A","B","X","Y","LB","RB","LT","RT","Menu","View",""};
    static const char *plain[]={"","A","B","X","Y","L1","R1","L2","R2","Menu","Options",""};
    NSString *name=nil;
    if(button==NATIVE_BUTTON_NAME) name=c.vendorName.length?c.vendorName:@"Controller";
    else if(button==NATIVE_BUTTON_TOUCHPAD) { if(!sense) return nullptr; name=@"touchpad"; }
    else if(!elements[button]) return nullptr;
    else if(sense || shock) name=shock && button==NATIVE_BUTTON_PAUSE?@"Share":@(playstation[button]);
    else if(xbox) name=@(box[button]);
    else {
        name=elements[button].localizedName;
        if([name hasSuffix:@" Button"]) name=[name substringToIndex:name.length-7];
        if(!name.length) name=@(plain[button]);
    }
    static char text[NATIVE_BUTTON_TOUCHPAD+1][64];
    snprintf(text[button],sizeof(text[button]),"%s",name.UTF8String);
    return text[button];
}
// 0 none, 1 DualSense (haptics, adaptive triggers), 2 any other controller.
static int padKind(GCController *c) {
    if(!c.extendedGamepad) return 0;
    return [c.physicalInputProfile isKindOfClass:GCDualSenseGamepad.class]?1:2;
}
static bool padTriggerEffects(GCController *c) {
    return padKind(c)==1 || [c.haptics.supportedLocalities containsObject:GCHapticsLocalityRightTrigger];
}
// A controller in use: a button or trigger pressed, or a stick or the d-pad
// pushed past halfway. Stick drift at rest does not count.
static bool engaged(GCController *c) {
    GCExtendedGamepad *p=c.extendedGamepad; if(!p) return false;
    if(p.buttonA.pressed || p.buttonB.pressed || p.buttonX.pressed || p.buttonY.pressed || p.leftShoulder.pressed || p.rightShoulder.pressed ||
       p.buttonMenu.pressed || p.buttonOptions.pressed || p.leftThumbstickButton.pressed || p.rightThumbstickButton.pressed) return true;
    if([c.physicalInputProfile isKindOfClass:GCDualSenseGamepad.class] && ((GCDualSenseGamepad *)c.physicalInputProfile).touchpadButton.pressed) return true;
    if(p.leftTrigger.value>.3f || p.rightTrigger.value>.3f) return true;
    auto pushed=[](GCControllerDirectionPad *d) { return std::hypot(d.xAxis.value,d.yAxis.value)>.5f; };
    return pushed(p.leftThumbstick) || pushed(p.rightThumbstick) || pushed(p.dpad);
}
// With more than one controller connected, the one last used plays: another
// takes over when it is used while the playing one is idle, so two in use at
// once never trade control back and forth.
static GCController *takeover(GCController *playing) {
    if(engaged(playing)) return nil;
    for(GCController *c in GCController.controllers) if(c!=playing && engaged(c)) return c;
    return nil;
}
// The first connected game controller plays until another is used; another
// takes over if it leaves.
void chooseController() {
    if(controller && [GCController.controllers containsObject:controller]) return;
    GCController *next=nil;
    for(GCController *c in GCController.controllers) if(c.extendedGamepad) { next=c; break; }
    selectController(next);
}
void buttons(bool *b) {
    GCExtendedGamepad *p=controller.extendedGamepad;
    if(!p) { std::fill_n(b,InputCount,false); return; }
    b[Cross]=p.buttonA.pressed; b[Circle]=p.buttonB.pressed; b[Square]=p.buttonX.pressed; b[Triangle]=p.buttonY.pressed;
    b[L1]=p.leftShoulder.pressed; b[R1]=p.rightShoulder.pressed;
    // Fixed input travel; never depend on asynchronous adaptive-trigger status.
    // Fire registers just past the DualSense resistance wall, or at a light
    // 15% press on triggers without one; release has hysteresis.
    bool adaptive=adaptiveTriggers();
    for(int side=0;side<2;++side) {
        float value=side?p.rightTrigger.value:p.leftTrigger.value;
        bool fire=side==1;
        float press=fire?Native_FireThreshold(adaptive):.15f,release=fire?Native_FireRelease(adaptive):.10f;
        triggerHeld[side]=value>(triggerHeld[side]?release:press);
        b[L2+side]=triggerHeld[side];
    }
    b[MenuButton]=p.buttonMenu.pressed; b[PauseButton]=p.buttonOptions.pressed;
    b[Touchpad]=dualPad().touchpadButton.pressed;
    b[Up]=p.dpad.up.pressed; b[Right]=p.dpad.right.pressed; b[Down]=p.dpad.down.pressed; b[Left]=p.dpad.left.pressed;
    b[L3]=p.leftThumbstickButton.pressed; b[R3]=p.rightThumbstickButton.pressed;
}
// Slot order, with the selection rules P_PlayerThink applies to BT_CHANGE:
// fist is unreachable while a chainsaw is owned without berserk, and the super
// shotgun exists only in commercial missions.
bool selectableWeapon(int weapon) {
    const player_t &player=players[consoleplayer];
    if(weapon<0 || weapon>=NUMWEAPONS || !player.weaponowned[weapon]) return false;
    if(weapon==wp_supershotgun && gamemode!=commercial) return false;
    if((weapon==wp_plasma || weapon==wp_bfg) && gamemode==shareware) return false;
    return weapon!=wp_fist || !player.weaponowned[wp_chainsaw] || player.powers[pw_strength];
}
int cycleWeapon(int direction) {
    static const int order[]={wp_fist,wp_chainsaw,wp_pistol,wp_shotgun,wp_supershotgun,wp_chaingun,wp_missile,wp_plasma,wp_bfg};
    constexpr int count=int(sizeof(order)/sizeof(order[0]));
    const player_t &player=players[consoleplayer];
    // Continue from a weapon still being raised so repeated presses advance.
    int start=queuedWeapon>=0?queuedWeapon:player.pendingweapon!=wp_nochange?player.pendingweapon:player.readyweapon;
    int position=0;
    while(position<count && order[position]!=start) ++position;
    if(position==count) return start;
    for(int i=1;i<count;++i) { int w=order[(position+direction*i+count*2)%count]; if(selectableWeapon(w)) return w; }
    return start;
}
// Menu steps tick the hand directly; play feedback stays gated on gameplay.
void menuTick(int kind) { rumbleDevice.play(native_doom::hapticEffect(kind,0),hapticScale(),0,0); }
void pollPad() {
    if(!Native_IsActive()) return;
    if(GCController *next=takeover(controller)) selectController(next);
    if(!controller) return;
    bool b[InputCount]; buttons(b);
    GCExtendedGamepad *p=controller.extendedGamepad;
    auto pressed=[&](int i) { return b[i] && !previous[i]; };
    if(paused && !menuactive && !messageToPrint && (pressed(Cross) || pressed(PauseButton))) keyPulse(KEY_PAUSE);
    else if(menuactive || messageToPrint) {
        if(pressed(Cross)) { keyPulse(KEY_ENTER); menuTick(native_doom::MenuSelect); }
        if(pressed(Circle) || pressed(MenuButton)) { keyPulse(messageToPrint?'n':KEY_ESCAPE); menuTick(native_doom::MenuBack); }
        int direction=-1;
        float x=p.dpad.xAxis.value+p.leftThumbstick.xAxis.value,y=p.dpad.yAxis.value+p.leftThumbstick.yAxis.value;
        if(y>.5) direction=KEY_UPARROW; else if(y<-.5) direction=KEY_DOWNARROW;
        else if(x>.5) direction=KEY_RIGHTARROW; else if(x<-.5) direction=KEY_LEFTARROW;
        int now=Native_Time();
        if(direction>=0 && (direction!=previousDirection || now>=repeatAt)) {
            keyPulse(direction);
            menuTick(direction==KEY_LEFTARROW || direction==KEY_RIGHTARROW?native_doom::MenuToggle:native_doom::MenuMove);
            repeatAt=now+(direction!=previousDirection?12:5);
        }
        previousDirection=direction;
    } else {
        if(pressed(MenuButton) || pressed(Circle)) { keyPulse(KEY_ESCAPE); menuTick(native_doom::MenuSelect); }
        if(pressed(PauseButton)) keyPulse(KEY_PAUSE);
        if(pressed(Touchpad)) keyPulse(KEY_TAB);
        if(pressed(L1)) queuedWeapon=cycleWeapon(-1);
        if(pressed(R1) || pressed(Triangle)) queuedWeapon=cycleWeapon(1);
        if(pressed(Up)) queuedWeapon=wp_fist;
        if(pressed(Right)) queuedWeapon=wp_pistol;
        if(pressed(Down)) queuedWeapon=wp_shotgun;
        if(pressed(Left)) queuedWeapon=wp_chaingun;
        previousDirection=-1;
    }
    std::copy_n(b,InputCount,previous);
}
ticcmd_t *padCommand(void) {
    static ticcmd_t cmd; memset(&cmd,0,sizeof(cmd));
    bool live=Native_IsActive() && controller && !menuactive && !messageToPrint && !paused && !demoplayback && gamestate==GS_LEVEL;
    const player_t &player=players[consoleplayer];
    bool b[InputCount]; buttons(b);
    lastTurn=0;
    if(live && player.playerstate==PST_DEAD) {
        // The original core respawns only on BT_USE; a fresh Use or Fire press
        // sends it, so a press held through the death does not skip the death view.
        bool press=b[Cross] || b[R3] || b[R2];
        if(press && !respawnHeld) cmd.buttons=BT_USE;
        respawnHeld=press;
        fireGate.sample(false,player.readyweapon,false);
        commandFiring=false; return &cmd;
    }
    if(!live || player.health<=0) {
        respawnHeld=true;
        fireGate.sample(b[R2],player.readyweapon,false);
        commandFiring=false; return &cmd;
    }
    GCExtendedGamepad *p=controller.extendedGamepad;
    native_doom::PadState s;
    s.side=p.leftThumbstick.xAxis.value; s.forward=p.leftThumbstick.yAxis.value; s.turn=p.rightThumbstick.xAxis.value;
    s.use=b[Cross] || b[R3]; s.run=b[Square] || b[L3];
    respawnHeld=b[R2] || s.use;
    s.fire=fireGate.sample(b[R2],player.readyweapon,true);
    commandFiring=s.fire;
    if(b[L2]) { s.side+=s.turn; s.turn=0; }
    if(queuedWeapon==wp_supershotgun) {
        if(player.readyweapon==wp_supershotgun) { queuedWeapon=-1; s.weapon=-1; }
        // The core never raises it here; one plain slot-3 request, not an endless one.
        else if(!selectableWeapon(wp_supershotgun)) { queuedWeapon=-1; s.weapon=wp_shotgun; }
        else s.weapon=wp_shotgun; // Original slot-3 toggle, retained through both ready ticks.
    } else { s.weapon=queuedWeapon; queuedWeapon=-1; }
    auto c=native_doom::padCommand(s,zone,sensitivity);
    cmd.forwardmove=c.forward; cmd.sidemove=c.side; cmd.angleturn=c.turn; cmd.buttons=c.buttons;
    lastTurn=c.turn; turnBuiltAt=Native_ClockTics();
    return &cmd;
}
extern "C" void A_WeaponReady(player_t *player,pspdef_t *psp);
bool weaponReady(const player_t &player) {
    const state_t *state=player.psprites[ps_weapon].state;
    return state && state->action.acp2==(actionf_p2)A_WeaponReady;
}
bool outOfAmmo(const player_t &player,int weapon) {
    if(weapon<0 || weapon>=NUMWEAPONS) return false;
    ammotype_t ammo=weaponinfo[weapon].ammo;
    if(ammo==am_noammo) return false;
    return player.ammo[ammo]<(weapon==wp_bfg?40:weapon==wp_supershotgun?2:1);
}
void applyTriggers(int weapon,native_doom::TriggerState state) {
    triggerWeapon=weapon; triggerState=state; triggerRetryAt=uptime()+.05;
    GCDualSenseGamepad *p=dualPad(); if(!p) return;
    [p.leftTrigger setModeOff];
    if(triggerScale()<=0) [p.rightTrigger setModeOff];
    else Native_ApplyTrigger(p.rightTrigger,native_doom::triggerProfile(weapon,state),triggerScale());
}
// The trigger follows the gun: a wall to break when ready, locked solid
// while a pump, break-action or launcher cycles, buzzing under automatic
// fire and slack when the gun is empty. Reprogrammed only on a change; a
// profile the system dropped is restored at most 20 times a second.
void updateTriggers() {
    if(!dualPad()) return;
    const player_t &player=players[consoleplayer];
    int weapon=player.readyweapon;
    if(cycling && (weaponReady(player) || weapon!=triggerWeapon)) cycling=false;
    using native_doom::TriggerState;
    bool automatic=weapon==wp_chaingun || weapon==wp_plasma || weapon==wp_chainsaw;
    TriggerState state=outOfAmmo(player,weapon)?TriggerState::Empty:cycling?TriggerState::Cycling:
        automatic && commandFiring && player.pendingweapon==wp_nochange?TriggerState::Firing:TriggerState::Ready;
    if(weapon!=triggerWeapon || state!=triggerState) { applyTriggers(weapon,state); return; }
    double now=uptime();
    if(now<triggerRetryAt) return;
    triggerRetryAt=now+.05;
    auto desired=triggerScale()>0?Native_TriggerMode(native_doom::triggerProfile(weapon,state)):GCDualSenseAdaptiveTriggerModeOff;
    GCDualSenseGamepad *pad=dualPad();
    if(pad.rightTrigger.mode!=desired || pad.leftTrigger.mode!=GCDualSenseAdaptiveTriggerModeOff) applyTriggers(weapon,state);
}
bool feedbackLive() {
    return Native_IsActive() && controller && gamestate==GS_LEVEL && !menuactive && !messageToPrint && !paused && !demoplayback && players[consoleplayer].health>0;
}
void storeSettings() {
    NSUserDefaults *p=NSUserDefaults.standardUserDefaults;
    [p setFloat:zone forKey:@"Deadzone"]; [p setFloat:sensitivity forKey:@"TurnSensitivity"];
    [p setFloat:hapticIntensity forKey:@"HapticIntensity"]; [p setFloat:triggerIntensity forKey:@"TriggerIntensity"];
    [p setBool:hapticsEnabled forKey:@"HapticsEnabled"]; [p setBool:triggersEnabled forKey:@"TriggersEnabled"];
}
void readSettings() {
    NSUserDefaults *p=NSUserDefaults.standardUserDefaults;
    auto number=[&](NSString *key,float fallback,float low,float high) {
        float value=[p objectForKey:key]?[p floatForKey:key]:fallback;
        return std::isfinite(value)?std::clamp(value,low,high):fallback;
    };
    zone=number(@"Deadzone",.18f,.05f,.5f); sensitivity=number(@"TurnSensitivity",1.f,.25f,2.f);
    hapticIntensity=number(@"HapticIntensity",1.f,0.f,1.f); triggerIntensity=number(@"TriggerIntensity",1.f,0.f,1.f);
    if([p objectForKey:@"HapticsEnabled"]) hapticsEnabled=[p boolForKey:@"HapticsEnabled"];
    if([p objectForKey:@"TriggersEnabled"]) triggersEnabled=[p boolForKey:@"TriggersEnabled"];
}
}

void Native_ControllerReset(void) { resetPad(); }
void Native_ControllersInit(void) {
    readSettings();
    chooseController();
    NSNotificationCenter *nc=NSNotificationCenter.defaultCenter;
    [nc addObserverForName:GCControllerDidConnectNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *) { chooseController(); }];
    [nc addObserverForName:GCControllerDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) {
        if(n.object!=controller) return;
        // The controller leaving mid-game pauses it until play resumes.
        if(gamestate==GS_LEVEL && usergame && !paused && !menuactive && !demoplayback) keyPulse(KEY_PAUSE);
        selectController(nil); chooseController();
    }];
    [GCController startWirelessControllerDiscoveryWithCompletionHandler:nil];
}
void Native_ControllerPoll(void) { pollPad(); }
extern "C" ticcmd_t *I_BaseTiccmd(void) { return padCommand(); }
// Late-latched stick turn for the local view: the last command's turn is shown
// at once rather than blended in over the next tic, and the turn the stick is
// asking for now is shown in proportion to the time since that command, so
// aiming tracks the stick at display rate. Presentation only.
int Native_ControllerLastTurn(void) { return controller && Native_IsActive()?lastTurn:0; }
int Native_ControllerPendingTurn(void) {
    GCExtendedGamepad *p=controller.extendedGamepad;
    if(!p || !feedbackLive() || previous[L2]) return 0;
    native_doom::PadState s; s.turn=p.rightThumbstick.xAxis.value;
    double elapsed=std::clamp(Native_PresentationTics()-turnBuiltAt,0.0,1.0);
    return int(std::lround(native_doom::padCommand(s,zone,sensitivity).turn*elapsed*65536.0));
}
void Native_FeedbackUpdate(void) {
    if(!feedbackLive()) { if(feedbackActive) stopFeedback(); return; }
    feedbackActive=true;
    const player_t &player=players[consoleplayer];
    if(stream.update(player.readyweapon,commandFiring && player.pendingweapon==wp_nochange,true,uptime())) rumbleDevice.stopStream();
    updateTriggers();
}
void Native_Feedback(int kind,int detail) {
    if(!feedbackLive()) { if(feedbackActive) stopFeedback(); return; }
    feedbackActive=true;
    auto effect=native_doom::hapticEffect(kind,detail);
    if(kind==native_doom::WeaponFire && native_doom::cyclesAfterShot(detail)) { cycling=true; updateTriggers(); }
    if(effect.stream) {
        // One hum per burst of continuous fire, refreshed by each shot.
        if(stream.shot(detail,uptime())) { rumbleDevice.stopStream(); rumbleDevice.stream(effect,hapticScale(),triggerScale(),FireMask); }
        return;
    }
    rumbleDevice.play(effect,hapticScale(),triggerScale(),FireMask,kind==native_doom::Damage && uptime()<balanceUntil?balance:0);
}
// Only the local player's events reach the hand.
void Native_FeedbackPlayer(int player,int kind,int detail) { if(player==consoleplayer) Native_Feedback(kind,detail); }
// Where the damage came from, relative to where the player faces.
void Native_FeedbackSource(int player,int32_t x,int32_t y) {
    if(player!=consoleplayer || !players[player].mo) return;
    const mobj_t *mo=players[player].mo;
    double toward=std::atan2(double(y)-mo->y,double(x)-mo->x)-mo->angle*(2*M_PI/4294967296.0);
    balance=float(-std::sin(toward)); balanceUntil=uptime()+.1;
}
int Native_PadKind(void) { return padKind(controller); }
const char *Native_PadButton(int button) { return buttonName(controller,button); }
int Native_PadVibration(void) { return controller.haptics!=nil; }
int Native_PadTriggerEffects(void) { return padTriggerEffects(controller); }
int Native_PadValue(int setting) {
    switch(setting) {
    case NATIVE_PAD_DEADZONE: return int(std::lround(zone*100));
    case NATIVE_PAD_TURN: return int(std::lround(sensitivity*100));
    case NATIVE_PAD_HAPTICS: return hapticsEnabled;
    case NATIVE_PAD_HAPTIC_STRENGTH: return int(std::lround(hapticIntensity*100));
    case NATIVE_PAD_TRIGGERS: return triggersEnabled;
    case NATIVE_PAD_TRIGGER_STRENGTH: return int(std::lround(triggerIntensity*100));
    }
    return 0;
}
void Native_PadAdjust(int setting,int direction) {
    int step=direction>0?5:-5;
    switch(setting) {
    case NATIVE_PAD_DEADZONE: zone=std::clamp(std::lround(zone*100)+step,5L,50L)/100.f; break;
    case NATIVE_PAD_TURN: sensitivity=std::clamp(std::lround(sensitivity*100)+step,25L,200L)/100.f; break;
    case NATIVE_PAD_HAPTICS: hapticsEnabled=!hapticsEnabled; break;
    case NATIVE_PAD_HAPTIC_STRENGTH: hapticIntensity=std::clamp(std::lround(hapticIntensity*100)+step,0L,100L)/100.f; break;
    case NATIVE_PAD_TRIGGERS: triggersEnabled=!triggersEnabled; break;
    case NATIVE_PAD_TRIGGER_STRENGTH: triggerIntensity=std::clamp(std::lround(triggerIntensity*100)+step,0L,100L)/100.f; break;
    }
    storeSettings(); resetPad();
}
