// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "Portable.hpp"
#include <map>
namespace native_doom {
enum class BaseGame { Doom, Unknown };
struct WADInfo {
    bool iwad=false, palette=false, supported=true;
    BaseGame game=BaseGame::Unknown;
    int episodes=0;
    std::vector<std::string> maps;
    std::string sha256, title, reason;
    std::map<std::string,std::pair<uint32_t,uint32_t>> lumps;
};
struct ContentProfile {
    WADInfo base;
    std::string iwad, title, saveKey, reason;
    std::vector<std::string> patches;
    int mode=0;
    bool supported=true;
};
bool inspectWAD(const char *path,WADInfo &info,std::string &error);
bool contentProfile(const std::string &base,const std::vector<std::string> &patches,
                    ContentProfile &profile,std::string &error);
std::string fingerprint(const std::string &value);
const char *baseTitle(BaseGame game,int episodes);
}
