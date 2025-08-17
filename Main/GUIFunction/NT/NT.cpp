
#include "../../KswordTotalHead.h"
#include "nt.h"
#include <ntstatus.h>
#include "NT.h"
#include <ntstatus.h>
#include <iostream>

#pragma comment(lib, "ntdll.lib")

// 全局变量
static std::map<UCHAR, std::wstring> g_typeMap;
static bool g_isQueryCompleted = false;
static bool g_isQuerySuccess = false;

static bool KswordNTInited = false;

// 复用原代码中的QueryObjectTypeIndexMap方法
std::map<UCHAR, std::wstring> QueryObjectTypeIndexMap() {
    std::map<UCHAR, std::wstring> typeMap;
    ULONG len = 0x10000;
    NTSTATUS status;
    std::vector<BYTE> buffer;

    do {
        buffer.resize(len);
        status = NtQueryObject(nullptr,
            static_cast<OBJECT_INFORMATION_CLASS>(OIC_ObjectTypesInformation),
            buffer.data(), len, &len);
        if (status == STATUS_INFO_LENGTH_MISMATCH)
            len *= 2;
    } while (status == STATUS_INFO_LENGTH_MISMATCH);

    if (!NT_SUCCESS(status)) {
        kLog.err(C("NtQueryObject获取类型信息失败"), C("句柄类型查询"));
        return typeMap;
    }

    auto typesInfo = reinterpret_cast<OBJECT_TYPES_INFORMATION*>(buffer.data());
    ULONG_PTR bufferBase = reinterpret_cast<ULONG_PTR>(buffer.data());
    ULONG_PTR entryPtr = bufferBase + sizeof(OBJECT_TYPES_INFORMATION);
    entryPtr = (entryPtr + sizeof(PVOID) - 1) & ~(sizeof(PVOID) - 1);
    ULONG_PTR totalSize = bufferBase + len;

    for (ULONG i = 0; i < typesInfo->NumberOfTypes && entryPtr < totalSize; ++i) {
        auto typeInfo = reinterpret_cast<OBJECT_TYPE_INFORMATION*>(entryPtr);

        std::wstring name;
        if (typeInfo->TypeName.Buffer && typeInfo->TypeName.Length > 0) {
            name.assign(typeInfo->TypeName.Buffer, typeInfo->TypeName.Length / sizeof(WCHAR));
        }

        typeMap[typeInfo->TypeIndex] = name;

        SIZE_T entrySize = sizeof(OBJECT_TYPE_INFORMATION) + typeInfo->TypeName.MaximumLength;
        entrySize = (entrySize + sizeof(PVOID) - 1) & ~(sizeof(PVOID) - 1);
        entryPtr += entrySize;
    }

    kLog.info(C("成功获取句柄类型索引映射"), C("句柄类型查询"));
    return typeMap;
}

// ImGui渲染函数
void KswordNTMain()
{
    // 查询按钮
    if (ImGui::CollapsingHeader(C("内存索引类型查询"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if(!KswordNTInited){

            g_typeMap = QueryObjectTypeIndexMap();
            g_isQueryCompleted = true;
            g_isQuerySuccess = !g_typeMap.empty();
			KswordNTInited = true;
        }

        // 显示查询结果
        if (g_isQueryCompleted) {
            if (g_isQuerySuccess) {
                ImGui::Text(C("共找到 %d 种对象类型"), g_typeMap.size());

                if (ImGui::BeginTable(C("类型索引表"), 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                    ImGui::TableSetupColumn(C("类型索引"));
                    ImGui::TableSetupColumn(C("类型名称"));
                    ImGui::TableHeadersRow();

                    for (const auto& [idx, name] : g_typeMap) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("%d", idx);
                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text(C(WCharToString(name.c_str())));  // 直接显示宽字符串
                    }
                    ImGui::EndTable();
                }
            }
            else {
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), C("查询失败，请检查权限或系统环境"));
            }
        }
    }
    // 主窗口
    ImGui::EndTabItem();
    return;
}