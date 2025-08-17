
#include "../../KswordTotalHead.h"
#include "nt.h"
#include <ntstatus.h>
#include "NT.h"
#include <ntstatus.h>
#include <iostream>

#pragma comment(lib, "ntdll.lib")

// ȫ�ֱ���
static std::map<UCHAR, std::wstring> g_typeMap;
static bool g_isQueryCompleted = false;
static bool g_isQuerySuccess = false;

static bool KswordNTInited = false;

// ����ԭ�����е�QueryObjectTypeIndexMap����
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
        kLog.err(C("NtQueryObject��ȡ������Ϣʧ��"), C("������Ͳ�ѯ"));
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

    kLog.info(C("�ɹ���ȡ�����������ӳ��"), C("������Ͳ�ѯ"));
    return typeMap;
}

// ImGui��Ⱦ����
void KswordNTMain()
{
    // ��ѯ��ť
    if (ImGui::CollapsingHeader(C("�ڴ��������Ͳ�ѯ"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if(!KswordNTInited){

            g_typeMap = QueryObjectTypeIndexMap();
            g_isQueryCompleted = true;
            g_isQuerySuccess = !g_typeMap.empty();
			KswordNTInited = true;
        }

        // ��ʾ��ѯ���
        if (g_isQueryCompleted) {
            if (g_isQuerySuccess) {
                ImGui::Text(C("���ҵ� %d �ֶ�������"), g_typeMap.size());

                if (ImGui::BeginTable(C("����������"), 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                    ImGui::TableSetupColumn(C("��������"));
                    ImGui::TableSetupColumn(C("��������"));
                    ImGui::TableHeadersRow();

                    for (const auto& [idx, name] : g_typeMap) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("%d", idx);
                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text(C(WCharToString(name.c_str())));  // ֱ����ʾ���ַ���
                    }
                    ImGui::EndTable();
                }
            }
            else {
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), C("��ѯʧ�ܣ�����Ȩ�޻�ϵͳ����"));
            }
        }
    }
    // ������
    ImGui::EndTabItem();
    return;
}