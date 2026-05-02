/*++ // 声明驱动源文件头注释块。

Module Name: // 描述模块名称字段。

    callback_remove_external.c // 标记当前实现文件名。

Abstract: // 描述功能摘要字段。

    Implements IOCTL for removing external notify callbacks by callback function address. // 说明本模块用于按回调地址移除外部回调。

Environment: // 描述运行环境字段。

    Kernel-mode Driver Framework // 说明代码运行在内核驱动框架环境。

--*/ // 结束头注释块。

#include "callback_internal.h" // 引入回调内部共享声明。
#define KSWORD_ARK_CALLBACK_EXTERNAL_ENABLE_FULL 1 // 启用外部回调完整移除声明。
#include "callback_external_core.h" // 引入外部回调安全移除扩展声明。

#define SystemModuleInformation 11 // 声明 ZwQuerySystemInformation 的系统模块信息类别值。

NTSYSAPI // 声明内核导出的 ZwQuerySystemInformation 例程。
NTSTATUS // 声明函数返回 NTSTATUS 状态码。
NTAPI // 声明函数使用 NTAPI 调用约定。
ZwQuerySystemInformation( // 查询系统级信息缓冲。
    _In_ ULONG SystemInformationClass, // 输入系统信息类别。
    _Out_writes_bytes_opt_(SystemInformationLength) PVOID SystemInformation, // 输出信息缓冲区。
    _In_ ULONG SystemInformationLength, // 输入信息缓冲区字节数。
    _Out_opt_ PULONG ReturnLength // 可选输出所需字节数。
    ); // 结束 ZwQuerySystemInformation 声明。

typedef struct _KSWORD_ARK_SYSTEM_MODULE_ENTRY // 定义系统模块单项结构体。
{ // 开始系统模块单项结构体定义。
    HANDLE Section; // 记录模块节对象句柄。
    PVOID MappedBase; // 记录模块映射基址。
    PVOID ImageBase; // 记录模块镜像基址。
    ULONG ImageSize; // 记录模块镜像大小。
    ULONG Flags; // 记录模块标志位。
    USHORT LoadOrderIndex; // 记录加载顺序索引。
    USHORT InitOrderIndex; // 记录初始化顺序索引。
    USHORT LoadCount; // 记录加载计数。
    USHORT OffsetToFileName; // 记录文件名偏移。
    UCHAR FullPathName[256]; // 保存模块完整路径的 ANSI 缓冲。
} KSWORD_ARK_SYSTEM_MODULE_ENTRY; // 结束系统模块单项结构体定义。

typedef struct _KSWORD_ARK_SYSTEM_MODULE_INFORMATION // 定义系统模块信息结构体。
{ // 开始系统模块信息结构体定义。
    ULONG NumberOfModules; // 记录系统模块数量。
    KSWORD_ARK_SYSTEM_MODULE_ENTRY Modules[1]; // 声明模块数组首元素占位。
} KSWORD_ARK_SYSTEM_MODULE_INFORMATION; // 结束系统模块信息结构体定义。

typedef VOID // 定义进程回调函数类型返回值。
(*KSWORD_ARK_PROCESS_NOTIFY_EX)( // 定义扩展进程回调函数指针类型。
    _Inout_ PEPROCESS Process, // 声明进程对象参数。
    _In_ HANDLE ProcessId, // 声明进程标识参数。
    _Inout_opt_ PPS_CREATE_NOTIFY_INFO CreateInfo // 声明创建信息参数。
    ); // 结束扩展进程回调函数类型定义。

typedef VOID // 定义线程回调函数类型返回值。
(*KSWORD_ARK_THREAD_NOTIFY)( // 定义线程回调函数指针类型。
    _In_ HANDLE ProcessId, // 声明进程标识参数。
    _In_ HANDLE ThreadId, // 声明线程标识参数。
    _In_ BOOLEAN Create // 声明创建/退出标记参数。
    ); // 结束线程回调函数类型定义。

typedef VOID // 定义镜像回调函数类型返回值。
(*KSWORD_ARK_IMAGE_NOTIFY)( // 定义镜像回调函数指针类型。
    _In_opt_ PUNICODE_STRING FullImageName, // 声明镜像完整路径参数。
    _In_ HANDLE ProcessId, // 声明进程标识参数。
    _In_ PIMAGE_INFO ImageInfo // 声明镜像信息参数。
    ); // 结束镜像回调函数类型定义。

_Must_inspect_result_ // 要求调用方检查返回值。
static // 限定该函数仅在当前编译单元可见。
NTSTATUS // 声明函数返回 NTSTATUS。
KswordArkCallbackResolveModuleByAddress( // 按回调地址解析所属模块。
    _In_ ULONG64 callbackAddress, // 输入回调函数地址。
    _Out_writes_(modulePathChars) PWCHAR modulePathBuffer, // 输出模块路径缓冲区。
    _In_ size_t modulePathChars, // 输入模块路径缓冲区字符数。
    _Out_opt_ ULONG64* moduleBaseOut, // 可选输出模块基址。
    _Out_opt_ ULONG* moduleSizeOut // 可选输出模块大小。
    ) // 结束函数参数列表。
{ // 开始模块解析函数体。
    NTSTATUS status = STATUS_SUCCESS; // 初始化函数状态值。
    ULONG requiredBytes = 0; // 初始化查询所需字节数。
    KSWORD_ARK_SYSTEM_MODULE_INFORMATION* moduleInfo = NULL; // 初始化模块信息缓冲指针。
    ULONG moduleIndex = 0; // 初始化模块遍历索引。
    PVOID callbackPointer = (PVOID)(ULONG_PTR)callbackAddress; // 将地址转换为指针用于比较。

    if (modulePathBuffer == NULL || modulePathChars == 0U) { // 校验模块路径输出缓冲区是否合法。
        return STATUS_INVALID_PARAMETER; // 缓冲区无效时返回参数错误。
    } // 结束输出缓冲区参数校验分支。
    modulePathBuffer[0] = L'\0'; // 默认清空输出路径首字符。
    if (moduleBaseOut != NULL) { // 判断是否提供模块基址输出参数。
        *moduleBaseOut = 0ULL; // 未解析时默认输出 0 基址。
    } // 结束模块基址默认值分支。
    if (moduleSizeOut != NULL) { // 判断是否提供模块大小输出参数。
        *moduleSizeOut = 0UL; // 未解析时默认输出 0 大小。
    } // 结束模块大小默认值分支。

    status = ZwQuerySystemInformation(SystemModuleInformation, NULL, 0UL, &requiredBytes); // 首次查询模块信息长度。
    if (status != STATUS_INFO_LENGTH_MISMATCH || requiredBytes == 0UL) { // 判断长度探测是否成功。
        return STATUS_UNSUCCESSFUL; // 长度探测异常时返回失败。
    } // 结束长度探测结果判断分支。

    moduleInfo = (KSWORD_ARK_SYSTEM_MODULE_INFORMATION*)KswordArkAllocateNonPaged( // 分配非分页池缓冲存储模块表。
        requiredBytes, // 传入所需缓冲区字节数。
        KSWORD_ARK_CALLBACK_TAG_RUNTIME); // 传入内存分配标签。
    if (moduleInfo == NULL) { // 判断分配模块信息缓冲是否成功。
        return STATUS_INSUFFICIENT_RESOURCES; // 分配失败时返回资源不足。
    } // 结束内存分配结果判断分支。

    status = ZwQuerySystemInformation( // 再次查询系统模块完整信息。
        SystemModuleInformation, // 指定查询类型为系统模块信息。
        moduleInfo, // 输出缓冲区为已分配模块信息。
        requiredBytes, // 提供缓冲区大小。
        &requiredBytes); // 返回实际写入或所需字节数。
    if (!NT_SUCCESS(status)) { // 判断查询系统模块信息是否成功。
        ExFreePool(moduleInfo); // 查询失败时释放已分配缓冲。
        return status; // 返回系统查询失败状态。
    } // 结束模块信息查询结果分支。

    for (moduleIndex = 0; moduleIndex < moduleInfo->NumberOfModules; ++moduleIndex) { // 遍历每个系统模块条目。
        const KSWORD_ARK_SYSTEM_MODULE_ENTRY* moduleEntry = // 声明当前模块条目只读指针。
            (const KSWORD_ARK_SYSTEM_MODULE_ENTRY*)&moduleInfo->Modules[moduleIndex]; // 取出当前索引模块条目地址。
        const ULONG64 moduleBase = (ULONG64)(ULONG_PTR)moduleEntry->ImageBase; // 计算当前模块基址。
        const ULONG64 moduleEnd = moduleBase + (ULONG64)moduleEntry->ImageSize; // 计算当前模块结束地址。
        if ((ULONG64)(ULONG_PTR)callbackPointer < moduleBase || (ULONG64)(ULONG_PTR)callbackPointer >= moduleEnd) { // 判断回调地址是否落在当前模块范围外。
            continue; // 不在当前模块范围则继续下一项。
        } // 结束模块地址范围判断分支。

        if (moduleBaseOut != NULL) { // 判断是否需要输出模块基址。
            *moduleBaseOut = moduleBase; // 输出匹配模块的基址。
        } // 结束模块基址输出分支。
        if (moduleSizeOut != NULL) { // 判断是否需要输出模块大小。
            *moduleSizeOut = moduleEntry->ImageSize; // 输出匹配模块的大小。
        } // 结束模块大小输出分支。
        (VOID)RtlStringCbPrintfW( // 将模块完整路径转换为宽字符输出。
            modulePathBuffer, // 目标输出缓冲区。
            modulePathChars * sizeof(WCHAR), // 目标缓冲区字节大小。
            L"%S", // 使用 ANSI 到 Unicode 的格式化模板。
            moduleEntry->FullPathName); // 源模块完整路径。
        ExFreePool(moduleInfo); // 命中目标后释放模块信息缓冲。
        return STATUS_SUCCESS; // 成功解析模块并返回成功。
    } // 结束模块遍历循环。

    ExFreePool(moduleInfo); // 未命中任何模块时释放缓冲。
    return STATUS_NOT_FOUND; // 返回未找到模块状态。
} // 结束按地址解析模块函数。

NTSTATUS // 声明 IOCTL 处理函数返回类型。
KswordARKCallbackIoctlRemoveExternalCallback( // 实现外部回调移除 IOCTL 入口。
    _In_ WDFREQUEST Request, // 输入 WDF 请求对象。
    _In_ size_t InputBufferLength, // 输入缓冲区长度。
    _In_ size_t OutputBufferLength, // 输出缓冲区长度。
    _Out_ size_t* CompleteBytesOut // 输出完成字节数。
    ) // 结束函数参数列表。
{ // 开始 IOCTL 处理函数体。
    NTSTATUS status = STATUS_SUCCESS; // 初始化通用状态值。
    NTSTATUS operationStatus = STATUS_SUCCESS; // 初始化具体操作状态值。
    PVOID inputBuffer = NULL; // 初始化输入缓冲区指针。
    PVOID outputBuffer = NULL; // 初始化输出缓冲区指针。
    size_t inputBufferLength = 0; // 初始化输入缓冲区实际长度。
    size_t outputBufferLength = 0; // 初始化输出缓冲区实际长度。
    KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_REQUEST* requestPacket = NULL; // 初始化请求结构体指针。
    KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_RESPONSE* responsePacket = NULL; // 初始化响应结构体指针。
    KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_REQUEST requestCopy; // 缓存完整请求以规避 METHOD_BUFFERED 输入输出同缓冲覆盖。
    ULONG requestVersion = 0UL; // 缓存请求协议版本以规避 METHOD_BUFFERED 覆写风险。
    ULONG requestCallbackClass = 0UL; // 缓存请求回调类别以规避 METHOD_BUFFERED 覆写风险。
    ULONG64 requestCallbackAddress = 0ULL; // 缓存请求回调地址以规避 METHOD_BUFFERED 覆写风险。
    ULONG64 moduleBase = 0ULL; // 初始化模块基址输出。
    ULONG moduleSize = 0UL; // 初始化模块大小输出。

    RtlZeroMemory(&requestCopy, sizeof(requestCopy)); // 默认清空请求副本。

    if (CompleteBytesOut == NULL) { // 校验完成字节数输出指针。
        return STATUS_INVALID_PARAMETER; // 输出指针为空时返回参数错误。
    } // 结束完成字节数参数校验分支。
    *CompleteBytesOut = 0U; // 默认完成字节数初始化为 0。

    if (InputBufferLength < sizeof(KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_REQUEST) || // 校验输入缓冲区是否至少容纳请求结构。
        OutputBufferLength < sizeof(KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_RESPONSE)) { // 校验输出缓冲区是否至少容纳响应结构。
        return STATUS_BUFFER_TOO_SMALL; // 缓冲区不足时返回长度错误。
    } // 结束输入输出长度校验分支。

    status = WdfRequestRetrieveInputBuffer( // 从 WDF 请求中获取输入缓冲区。
        Request, // 传入请求对象。
        sizeof(KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_REQUEST), // 指定最小输入长度。
        &inputBuffer, // 输出输入缓冲区地址。
        &inputBufferLength); // 输出输入缓冲区实际长度。
    if (!NT_SUCCESS(status)) { // 判断输入缓冲区提取是否成功。
        return status; // 失败时直接返回错误状态。
    } // 结束输入缓冲提取结果分支。

    status = WdfRequestRetrieveOutputBuffer( // 从 WDF 请求中获取输出缓冲区。
        Request, // 传入请求对象。
        sizeof(KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_RESPONSE), // 指定最小输出长度。
        &outputBuffer, // 输出输出缓冲区地址。
        &outputBufferLength); // 输出输出缓冲区实际长度。
    if (!NT_SUCCESS(status)) { // 判断输出缓冲区提取是否成功。
        return status; // 失败时直接返回错误状态。
    } // 结束输出缓冲提取结果分支。

    requestPacket = (KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_REQUEST*)inputBuffer; // 解释输入缓冲为请求结构。
    responsePacket = (KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_RESPONSE*)outputBuffer; // 解释输出缓冲为响应结构。

    if (requestPacket->size < sizeof(KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_REQUEST) || // 校验请求包声明大小。
        requestPacket->version != KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_PROTOCOL_VERSION || // 校验协议版本是否匹配。
        requestPacket->callbackAddress == 0ULL || // 校验回调地址非零。
        requestPacket->flags != KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_FLAG_NONE) { // 校验请求标志位是否合法。
        return STATUS_INVALID_PARAMETER; // 请求字段非法时返回参数错误。
    } // 结束请求字段合法性校验分支。

    requestVersion = requestPacket->version; // 先缓存协议版本避免输出清零覆盖输入。
    requestCallbackClass = requestPacket->callbackClass; // 先缓存回调类别避免输出清零覆盖输入。
    requestCallbackAddress = requestPacket->callbackAddress; // 先缓存回调地址避免输出清零覆盖输入。
    requestCopy = *requestPacket; // 保存完整请求副本供后续外部移除子模块使用。
    RtlZeroMemory(responsePacket, sizeof(KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_RESPONSE)); // 清零响应结构体。
    responsePacket->size = sizeof(KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_RESPONSE); // 写入响应结构体大小。
    responsePacket->version = requestVersion; // 回填缓存后的协议版本。
    responsePacket->callbackClass = requestCallbackClass; // 回填缓存后的回调类别。
    responsePacket->callbackAddress = requestCallbackAddress; // 回填缓存后的回调地址。
    responsePacket->moduleBase = 0ULL; // 默认模块基址为 0。
    responsePacket->moduleSize = 0UL; // 默认模块大小为 0。
    responsePacket->mappingFlags = 0UL; // 默认映射标志为 0。
    (VOID)KswordArkCallbackResolveModuleByAddress( // 尝试解析回调地址所属模块。
        requestCallbackAddress, // 输入缓存后的回调地址。
        responsePacket->modulePath, // 输出模块路径缓冲。
        RTL_NUMBER_OF(responsePacket->modulePath), // 传入模块路径缓冲容量。
        &moduleBase, // 输出模块基址。
        &moduleSize); // 输出模块大小。
    responsePacket->moduleBase = moduleBase; // 回填解析到的模块基址。
    responsePacket->moduleSize = moduleSize; // 回填解析到的模块大小。
    if (responsePacket->modulePath[0] != L'\0') { // 判断是否解析到有效模块路径。
        responsePacket->mappingFlags = KSWORD_ARK_EXTERNAL_CALLBACK_MAPPING_FLAG_MODULE; // 标记模块映射成功。
    } // 结束模块映射标志设置分支。

    if (requestCallbackClass == KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_PROCESS || // 进程 notify 移除必须先确认函数地址落在内核模块范围。
        requestCallbackClass == KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_THREAD || // 线程 notify 移除必须先确认函数地址落在内核模块范围。
        requestCallbackClass == KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_IMAGE || // 镜像 notify 移除必须先确认函数地址落在内核模块范围。
        requestCallbackClass == KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_OBJECT || // 对象回调扩展移除必须先确认函数地址可验证。
        requestCallbackClass == KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_REGISTRY || // 注册表回调扩展移除必须先确认函数地址可验证。
        requestCallbackClass == KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_ETW_PROVIDER) { // ETW 回调扩展移除必须先确认函数地址可验证。
        if (moduleBase == 0ULL || moduleSize == 0UL) { // 未命中系统模块表时拒绝继续进入任何移除路径。
            operationStatus = STATUS_INVALID_PARAMETER; // 返回参数非法，避免对不可验证地址调用卸载 API。
            goto CompleteRemoveExternalCallback; // 跳转到统一响应和日志路径。
        } // 结束模块范围校验失败分支。
    } // 结束需要内核函数地址类别的统一校验。

    switch (requestCallbackClass) { // 根据缓存后的回调类型分发移除逻辑。
    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_PROCESS: // 处理进程创建回调移除。
        operationStatus = PsSetCreateProcessNotifyRoutineEx( // 优先调用 Ex 版本卸载回调。
            (KSWORD_ARK_PROCESS_NOTIFY_EX)(ULONG_PTR)requestCallbackAddress, // 传入缓存后的回调函数地址。
            TRUE); // 指定执行移除操作。
        if (operationStatus == STATUS_PROCEDURE_NOT_FOUND || operationStatus == STATUS_INVALID_PARAMETER) { // Ex 版本不可用或参数不匹配时执行回退。
            operationStatus = PsSetCreateProcessNotifyRoutine( // 回退到传统 API 卸载进程回调。
                (PCREATE_PROCESS_NOTIFY_ROUTINE)(ULONG_PTR)requestCallbackAddress, // 以传统签名传入缓存后的回调地址。
                TRUE); // 指定执行移除操作。
        } // 结束进程回调回退逻辑分支。
        break; // 结束进程回调类型处理。

    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_THREAD: // 处理线程创建回调移除。
        operationStatus = PsRemoveCreateThreadNotifyRoutine( // 调用线程回调移除 API。
            (KSWORD_ARK_THREAD_NOTIFY)(ULONG_PTR)requestCallbackAddress); // 传入缓存后的线程回调地址。
        break; // 结束线程回调类型处理。

    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_IMAGE: // 处理镜像加载回调移除。
        operationStatus = PsRemoveLoadImageNotifyRoutine( // 调用镜像回调移除 API。
            (KSWORD_ARK_IMAGE_NOTIFY)(ULONG_PTR)requestCallbackAddress); // 传入缓存后的镜像回调地址。
        break; // 结束镜像回调类型处理。

    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_OBJECT: // 对象回调交给外部安全移除扩展处理。
    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_REGISTRY: // 注册表回调交给外部安全移除扩展处理。
    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_MINIFILTER: // 微过滤器回调优先使用 Filter Manager 公开路径。
    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_WFP_CALLOUT: // WFP callout 回调优先使用 WFP 管理 API。
    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_ETW_PROVIDER: // ETW provider 回调仅在安全可验证时处理。
        operationStatus = KswordArkCallbackExternalRemoveByRequest( // 调用外部回调安全移除聚合函数。
            &requestCopy, // 传入请求副本，避免 METHOD_BUFFERED 输出清零覆盖输入。
            responsePacket); // 传入响应包以补充公开 API 验证结果。
        break; // 结束不支持类型处理。

    default: // 处理未知回调类型。
        operationStatus = STATUS_INVALID_PARAMETER; // 返回回调类型参数非法。
        break; // 结束未知类型处理。
    } // 结束回调类型分发分支。

CompleteRemoveExternalCallback: // 统一完成响应和日志路径。
    responsePacket->ntstatus = operationStatus; // 回填具体操作状态码。
    *CompleteBytesOut = sizeof(KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_RESPONSE); // 指定完成输出字节数。

    KswordArkCallbackLogFormat( // 写入回调移除操作日志。
        NT_SUCCESS(operationStatus) ? "Info" : "Warn", // 根据结果选择日志级别。
        "External callback remove request: class=%lu, callback=0x%llX, status=0x%08lX.", // 定义日志格式字符串。
        (unsigned long)requestCallbackClass, // 输出缓存后的回调类别字段。
        requestCallbackAddress, // 输出缓存后的回调地址字段。
        (unsigned long)operationStatus); // 输出操作状态字段。

    return STATUS_SUCCESS; // 返回 IOCTL 分发执行成功状态。
} // 结束外部回调移除 IOCTL 处理函数。
