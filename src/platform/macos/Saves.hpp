// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstddef>
#include <string>
namespace native_doom {
bool atomicWrite(const std::string &path,const void *bytes,size_t length,std::string &error);
bool validSave(const std::string &path);
int latestSave(const std::string &directory);
bool writeSave(const std::string &directory,int slot,const void *bytes,size_t length,std::string &error);
std::string savePath(const std::string &directory,int slot);
}
