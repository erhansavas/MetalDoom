// SPDX-License-Identifier: GPL-2.0-only
#import <Foundation/Foundation.h>
#include "Session.hpp"
#include "Saves.hpp"
#include "Native.h"
#include <array>
#include <cstdio>
#include <filesystem>
extern "C" {
#include "doomstat.h"
#include "info.h"
}
native_doom::ContentProfile sessionProfile;
static std::string saveDirectory,profileConfig;
static std::map<std::string,std::pair<uint32_t,uint32_t>> resources;
static std::array<std::string,6> slotPaths;
bool Native_ConfigureProfile(const std::string &base,const std::vector<std::string> &patches,std::string &error) {
    if(!native_doom::contentProfile(base,patches,sessionProfile,error)) return false;
    if(!sessionProfile.supported) { error=sessionProfile.reason; return false; }
    resources=sessionProfile.base.lumps;
    return true;
}
void Native_ConfigureSaves(const char *support,bool diagnostic) {
    namespace fs=std::filesystem;
    fs::path root(support);
    saveDirectory=diagnostic?root.string():(root/"profiles"/sessionProfile.saveKey).string();
    fs::create_directories(saveDirectory);
    profileConfig=(fs::path(saveDirectory)/"default.cfg").string();
    if(!fs::exists(profileConfig) && fs::exists(root/"default.cfg")) fs::copy_file(root/"default.cfg",profileConfig);
    for(int i=0;i<6;++i) slotPaths[i]=native_doom::savePath(saveDirectory,i);
    fprintf(stdout,"Session: %s; classic LinuxDOOM/1.9 rules; saves %s\n",sessionProfile.title.c_str(),saveDirectory.c_str());
}
extern "C" {
int Native_GameMode(void) { return sessionProfile.mode; }
int Native_GameMission(void) { return 0; } // doom: the original DOOM mission
const char *Native_GameTitle(void) { return sessionProfile.title.c_str(); }
const char *Native_ProfileConfig(void) { return profileConfig.c_str(); }
const char *Native_SavePath(int slot) { return slot>=0 && slot<6?slotPaths[slot].c_str():""; }
int Native_LatestSave(void) { return native_doom::latestSave(saveDirectory); }
int Native_WriteSave(int slot,const void *data,unsigned length) {
    std::string error;
    bool ok=native_doom::writeSave(saveDirectory,slot,data,length,error);
    if(!ok) fprintf(stderr,"Save failed (previous file preserved): %s\n",error.c_str());
    return ok;
}
int Native_SaveExists(int slot) { return native_doom::validSave(Native_SavePath(slot)); }
int Native_SaveFileValid(const char *path) { return path && native_doom::validSave(path); }
int Native_ValidLevel(int episode,int map) {
    if(episode<1 || map<1) return 0;
    char marker[9];
    snprintf(marker,sizeof(marker),"E%dM%d",episode,map);
    return resources.count(marker)!=0;
}
}
