// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "Content.hpp"
extern native_doom::ContentProfile sessionProfile;
bool Native_ConfigureProfile(const std::string &base,const std::vector<std::string> &patches,std::string &error);
void Native_ConfigureSaves(const char *support,bool diagnostic);
bool Native_RunLibrary(int argc,char **argv);
