#pragma once

// ============================================================
// ksword/string/string.h
// 命名空间：ks::str
// 作用：
// - 提供跨模块复用的字符串/时间转换工具；
// - 避免业务层重复书写 WideChar/MultiByte API；
// - 作为 ksword 可移植工具包中的基础能力模块。
// ============================================================

#include <cstdint> // std::uint64_t：用于 FILETIME 数值转换。
#include <string>  // std::string/std::wstring：文本传输基础类型。

namespace ks::str
{
    // Utf16ToUtf8 作用：
    // - 把 UTF-16 宽字符串（Windows 原生）转换为 UTF-8；
    // 参数 utf16Text：待转换文本；
    // 返回值：UTF-8 编码字符串，失败时返回空串。
    std::string Utf16ToUtf8(const std::wstring& utf16Text);

    // Utf8ToUtf16 作用：
    // - 把 UTF-8 文本转换为 UTF-16 宽字符串；
    // 参数 utf8Text：待转换文本；
    // 返回值：UTF-16 编码宽字符串，失败时返回空串。
    std::wstring Utf8ToUtf16(const std::string& utf8Text);

    // TrimCopy 作用：
    // - 去掉字符串首尾空白字符（空格/制表/换行）；
    // 参数 textValue：原始文本；
    // 返回值：去除首尾空白后的副本。
    std::string TrimCopy(const std::string& textValue);

    // FileTimeToUint64 作用：
    // - 把 FILETIME 的高低位拼接为 64 位整数（100ns 基准）；
    // 参数 highPart/lowPart：FILETIME 的两部分；
    // 返回值：合并后的 64 位值。
    std::uint64_t FileTimeToUint64(std::uint32_t highPart, std::uint32_t lowPart);

    // FileTime100nsToLocalText 作用：
    // - 把 FILETIME 100ns 数值转换为本地时间文本；
    // 参数 fileTime100ns：自 1601-01-01 起的 100ns 计数；
    // 返回值：格式化后的 "YYYY-MM-DD HH:MM:SS" 文本，失败返回空串。
    std::string FileTime100nsToLocalText(std::uint64_t fileTime100ns);

    // ReplaceAllInPlace 作用：
    // - 就地替换字符串中全部匹配片段；
    // 参数 textValue：目标字符串引用；
    // 参数 fromText：被替换内容；
    // 参数 toText：替换后的内容；
    // 返回值：无（直接修改 textValue）。
    void ReplaceAllInPlace(std::string& textValue, const std::string& fromText, const std::string& toText);
}

