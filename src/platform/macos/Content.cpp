// SPDX-License-Identifier: GPL-2.0-only
// Content classification and byte identities. No game data or source-port code.
#include "Content.hpp"
#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sys/stat.h>
namespace native_doom {
static uint32_t le32(const uint8_t *p) { return p[0]|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24; }
static std::string hex(const uint8_t *bytes,size_t count) {
    std::string result; for(size_t i=0;i<count;++i) { char pair[3]; snprintf(pair,3,"%02x",bytes[i]); result+=pair; } return result;
}
std::string fingerprint(const std::string &value) {
    uint8_t digest[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(value.data(),CC_LONG(value.size()),digest); return hex(digest,sizeof(digest));
}
const char *baseTitle(BaseGame game,int episodes) {
    if(game==BaseGame::Doom) return episodes==4?"The Ultimate DOOM":(episodes==1?"DOOM Shareware":"DOOM");
    return "Unknown game data";
}
static bool mapName(const std::string &n) {
    return (n.size()==5 && n.compare(0,3,"MAP")==0 && isdigit(n[3]) && isdigit(n[4])) ||
           (n.size()==4 && n[0]=='E' && isdigit(n[1]) && n[2]=='M' && isdigit(n[3]));
}
bool inspectWAD(const char *path,WADInfo &info,std::string &error) {
    info={}; error.clear();
    FILE *f=fopen(path,"rb"); if(!f) { error="Cannot open game data."; return false; }
    auto fail=[&](const char *why) { error=why; fclose(f); return false; };
    struct stat st{}; uint8_t header[12];
    if(fstat(fileno(f),&st) || !S_ISREG(st.st_mode) || st.st_size<12 || st.st_size>INT32_MAX || fread(header,1,12,f)!=12)
        return fail("Invalid or oversized WAD file.");
    info.iwad=!memcmp(header,"IWAD",4);
    if(!info.iwad && memcmp(header,"PWAD",4)) return fail("Select a classic WAD file.");
    uint32_t count=le32(header+4),offset=le32(header+8);
    if(!count || count>200000 || offset<12 || uint64_t(offset)+uint64_t(count)*16>uint64_t(st.st_size))
        return fail("Invalid WAD directory bounds.");
    if(fseeko(f,offset,SEEK_SET)) return fail("Cannot read WAD directory.");
    std::vector<std::string> names; names.reserve(count);
    for(uint32_t i=0;i<count;++i) {
        uint8_t e[16]; if(fread(e,1,16,f)!=16) return fail("Truncated WAD directory.");
        uint32_t pos=le32(e),size=le32(e+4);
        if(uint64_t(pos)+size>uint64_t(st.st_size)) return fail("A WAD lump extends beyond the file.");
        std::string name; for(int j=0;j<8 && e[8+j];++j) name+=std::toupper(e[8+j]);
        info.lumps[name]={pos,size}; names.push_back(name);
        if(mapName(name)) info.maps.push_back(name);
    }
    auto contains=[&](const char *s) { return info.lumps.count(s)!=0; };
    auto block=[&](const char *s) { if(info.supported) { info.supported=false; info.reason=s; } };
    if(!info.iwad) block("Add-on WADs are not supported; select DOOM.WAD.");
    if(contains("TEXTMAP") || contains("BEHAVIOR")) block("This content uses UDMF or Hexen-format maps, which this classic core cannot run.");
    // Refuse known unsupported map features before starting the original loader.
    for(size_t i=0;i<names.size();++i) if(mapName(names[i])) {
        if(i+10>=names.size()) { block("Incomplete classic map directory."); continue; }
        static const char *order[]={"THINGS","LINEDEFS","SIDEDEFS","VERTEXES","SEGS","SSECTORS","NODES","SECTORS","REJECT","BLOCKMAP"};
        for(int k=0;k<10;++k) if(names[i+1+k]!=order[k]) block("Unsupported or incomplete classic map format.");
    }
    // A classic directory alone does not imply classic simulation rules.
    for(size_t i=0;i<names.size();++i)if(mapName(names[i]) && i+10<names.size()) {
        // Directory entries may have repeated names; inspect this map's records.
        uint8_t entry[16];
        for(auto check:std::vector<std::array<unsigned,4>>{{2,14,6,141},{8,26,22,17}}) {
            fseeko(f,offset+(i+check[0])*16,SEEK_SET);if(fread(entry,1,16,f)!=16)continue;
            unsigned at=le32(entry),length=le32(entry+4),stride=check[1],field=check[2];
            if(length%stride){block("Malformed classic map records.");continue;}
            if(length>64*1024*1024){block("Oversized classic map records.");continue;}
            std::vector<uint8_t> data(length);fseeko(f,at,SEEK_SET);if(fread(data.data(),1,length,f)!=length){block("Truncated map data.");continue;}
            for(size_t p=0;p<length;p+=stride)if(unsigned value=unsigned(data[p+field])+unsigned(data[p+field+1])*256;value>check[3] && !(field==6 && value==65535)){block("This map uses non-vanilla line or sector specials; Boom/MBF rules are not implemented.");break;}
        }
        fseeko(f,offset+(i+7)*16,SEEK_SET);if(fread(entry,1,16,f)==16 && le32(entry+4)>=4) {
            char magic[4];fseeko(f,le32(entry),SEEK_SET);if(fread(magic,1,4,f)==4 && (!memcmp(magic,"XNOD",4) || !memcmp(magic,"ZNOD",4)))block("Extended/compressed nodes are not implemented.");
        }
    }
    // All data contributes to save identity; paths and filenames do not.
    rewind(f); CC_SHA256_CTX sha; CC_SHA256_Init(&sha);
    std::array<uint8_t,65536> bytes; size_t n;
    while((n=fread(bytes.data(),1,bytes.size(),f))) CC_SHA256_Update(&sha,bytes.data(),CC_LONG(n));
    if(ferror(f)) return fail("Cannot fingerprint game data.");
    uint8_t sd[32]; CC_SHA256_Final(sd,&sha);
    info.sha256=hex(sd,32);
    info.palette=contains("PLAYPAL") && info.lumps["PLAYPAL"].second>=768*14;
    if(info.iwad) {
        bool episode=contains("E1M1"),commercial=contains("MAP01");
        if(episode && commercial) block("Ambiguous base game map namespaces.");
        if(episode) {
            info.game=BaseGame::Doom; info.episodes=1;
            if(contains("E2M1")!=contains("E3M1")) block("Incomplete DOOM base data.");
            if(contains("E2M1") && contains("E3M1")) info.episodes=contains("E4M1")?4:3;
        } else if(commercial) block("DOOM II and Final DOOM are not supported; select DOOM.WAD from DOOM or The Ultimate DOOM.");
        if(info.game==BaseGame::Unknown || !info.palette) block("Missing original DOOM resources or an unsupported game.");
        if(contains("FREEDOOM") || contains("HACX-R") || contains("CHEX")) block("This standalone game requires its own compatibility profile.");
        info.title=baseTitle(info.game,info.episodes);
    } else info.title=std::filesystem::path(path).stem().string();
    fclose(f); return true;
}
bool validateIWAD(const char *path,IWADInfo &info,std::string &error) {
    WADInfo wad; info={};
    if(!inspectWAD(path,wad,error)) return false;
    if(!wad.iwad || wad.game==BaseGame::Unknown || !wad.palette) { error="Select complete classic DOOM base game data."; return false; }
    if(!wad.supported) { error=wad.reason; return false; }
    info.registered=wad.episodes>1; info.ultimateData=wad.episodes==4; info.lumps=uint32_t(wad.lumps.size());
    return true;
}
bool contentProfile(const std::string &base,const std::vector<std::string> &patches,ContentProfile &p,std::string &error) {
    p={}; p.iwad=base; p.patches=patches;
    if(!inspectWAD(base.c_str(),p.base,error)) return false;
    if(!p.base.iwad || p.base.game!=BaseGame::Doom || !p.base.palette) { error="Original DOOM game data is required."; return false; }
    p.title=p.base.title; p.supported=p.base.supported; p.reason=p.base.reason;
    if(!patches.empty()) { p.supported=false; p.reason="Add-on WADs are not supported."; }
    p.mode=p.base.episodes==4?3:(p.base.episodes==3?1:0);
    // Unchanged identity format (mode:mission:campaign), so existing saves keep their profile.
    std::string identity="native-classic-v1\n"+p.base.sha256+"\n"+std::to_string(p.mode)+":0:0";
    p.saveKey=fingerprint(identity);
    return true;
}
}
