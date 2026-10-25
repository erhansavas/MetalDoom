// SPDX-License-Identifier: GPL-2.0-only
#include "Saves.hpp"
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace native_doom {
std::string savePath(const std::string &directory,int slot) { return directory+"/doomsav"+std::to_string(slot)+".dsg"; }
bool atomicWrite(const std::string &path,const void *data,size_t length,std::string &error) {
    std::string temporary=path+".XXXXXX";
    int fd=mkstemp(temporary.data());
    if(fd<0) { error=strerror(errno); return false; }
    const auto *bytes=static_cast<const char *>(data); size_t written=0;
    bool success=true;
    while(written<length) {
        ssize_t n=write(fd,bytes+written,length-written);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) { success=false; break; } written+=size_t(n);
    }
    if(success && fsync(fd)) success=false;
    int problem=errno;
    if(close(fd) && success) { success=false; problem=errno; }
    if(success && rename(temporary.c_str(),path.c_str())) { success=false; problem=errno; }
    if(!success) { unlink(temporary.c_str()); error=strerror(problem); return false; }
    // Persist the rename when the filesystem supports directory fsync.
    auto slash=path.find_last_of('/'); int directory=open(path.substr(0,slash).c_str(),O_RDONLY);
    if(directory>=0) { fsync(directory); close(directory); }
    return true;
}
bool validSave(const std::string &path) {
    struct stat st{}; if(stat(path.c_str(),&st) || st.st_size<51 || st.st_size>16*1024*1024) return false;
    FILE *f=fopen(path.c_str(),"rb"); if(!f) return false;
    std::array<char,40> header{};
    bool ok=fread(header.data(),1,header.size(),f)==header.size() && !memcmp(header.data()+24,"version 109\0\0\0\0\0",16);
    if(fseeko(f,-1,SEEK_END) || fgetc(f)!=0x1d) ok=false;
    fclose(f); return ok;
}
static int64_t modified(const struct stat &st) { return int64_t(st.st_mtimespec.tv_sec)*1000000000+st.st_mtimespec.tv_nsec; }
int latestSave(const std::string &directory) {
    int hint=-1; long long hintTime=0;
    if(FILE *f=fopen((directory+"/latest-save").c_str(),"r")) { if(fscanf(f,"%d %lld",&hint,&hintTime)!=2) hint=-1; fclose(f); }
    int latest=-1; int64_t newest=-1;
    for(int slot=0;slot<6;++slot) {
        const auto path=savePath(directory,slot); struct stat st{};
        if(!validSave(path) || stat(path.c_str(),&st)) continue;
        int64_t time=modified(st);
        if(time>newest || (time==newest && slot==hint && time==hintTime)) { latest=slot; newest=time; }
    }
    return latest;
}
bool writeSave(const std::string &directory,int slot,const void *bytes,size_t length,std::string &error) {
    if(slot<0 || slot>=6) { error="Invalid save slot."; return false; }
    const auto path=savePath(directory,slot);
    if(!atomicWrite(path,bytes,length,error)) return false;
    struct stat st{};
    if(!stat(path.c_str(),&st)) {
        std::string marker=std::to_string(slot)+" "+std::to_string(modified(st))+"\n",ignored;
        atomicWrite(directory+"/latest-save",marker.data(),marker.size(),ignored);
    }
    return true;
}
}
