#pragma once
#include "../../KswordTotalHead.h"
#include <string>
#include <cstdint>

// 内存相关结构体定义
struct KswordModuleInfo
{
    std::string path;
    uintptr_t baseAddr;
    size_t size;
    std::string version;
    std::string loadTime;
    bool isSigned;
    bool is64Bit;
    uint32_t checksum;
};

struct KswordThreadInfo
{
    DWORD id;
    std::string status;
    int priority;
    std::string entryAddr;
    std::string moduleName;
};

struct KswordMemoryRegionInfo
{
    uintptr_t baseAddr;
    size_t size;
    std::string type;
    std::string protection;
    std::string state;
};

struct KswordBreakpointInfo
{
    uintptr_t address;
    std::string module;
    std::string symbol;
    bool enabled;
    int hitCount;
    uint8_t originalByte;
};

// 函数声明
void KswordMemoryMain();


