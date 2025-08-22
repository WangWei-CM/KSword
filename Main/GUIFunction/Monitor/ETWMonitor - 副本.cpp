#define CURRENT_MODULE C("ETW���")
void ETWMonitorMain() {
    if (ImGui::CollapsingHeader(C("ETW���"))) {
        // ���ڴ洢ɸѡ�ı��Ļ�����
        if (firstRun) {
            firstRun = false;
            std::thread(EnumerateAllEtwProvidersWithEvents).detach(); // �첽����ETW Providers
        }
        if (ImGui::BeginChild("ETWProviderScrolling", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.5f - ImGui::GetStyle().ItemSpacing.y), true,
            ImGuiWindowFlags_HorizontalScrollbar)) {

            // ��ȡETW Providers����
            // ����ɸѡ�����
            ImGui::InputText(C("ɸѡ Provider ���¼�ID..."), filterText, IM_ARRAYSIZE(filterText));

            // �������
            if (ImGui::BeginTable(C("EtwProvidersTable"), 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY))
            {
                // ���ñ�ͷ
                ImGui::TableSetupColumn(C("Provider GUID"), ImGuiTableColumnFlags_WidthFixed, 300.0f);
                ImGui::TableSetupColumn(C("�¼�ID�б�"), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                // ��������Providers
                for (const auto& provider : etwProviders)
                {

                    // ��GUIDת��Ϊ�ַ����Ա���ʾ��ɸѡ
                    WCHAR guidStr[64];
                    StringFromGUID2(provider.guid, guidStr, IM_ARRAYSIZE(guidStr));
                    char ansiGuidStr[64];
                    WideCharToMultiByte(CP_ACP, 0, guidStr, -1, ansiGuidStr, sizeof(ansiGuidStr), NULL, NULL);

                    // ����Ƿ����ɸѡ����
                    bool matchesFilter = false;

                    if (strstr(ansiGuidStr, filterText) != nullptr) {
                        matchesFilter = true;
                    }
                    else {
                        // 2. ���������Provider Name�Ƿ�ƥ�䣨���ִ�Сд������
                        // ���ַ�Provider Nameת���ֽڣ���filterText����һ�£�
                        char providerNameA[512] = { 0 };
                        WideCharToMultiByte(
                            CP_ACP,
                            0,
                            provider.providerName.c_str(),  // ʹ��Provider�����ƣ����ַ���
                            -1,
                            providerNameA,
                            sizeof(providerNameA),
                            NULL,
                            NULL
                        );
                        // ���ִ�Сдģ��ƥ�䣨ʹ��_stricmp���ַ��Ƚ��Ӵ���
                        if (_strnicmp(providerNameA, filterText, strlen(filterText)) != 0) {
                            // ��ǰ׺��ƥ�䣬�ټ���Ƿ�������Զ��岻�ִ�Сдstrstr��
                            auto w2a = [](const wchar_t* wstr) { char b[512] = { 0 }; WideCharToMultiByte(CP_ACP, 0, wstr, -1, b, 512, nullptr, nullptr); return b; };
                            char* findPos = StrStrIA(w2a(provider.providerName.c_str()), filterText);
                            if (findPos != nullptr) {
                                matchesFilter = true;
                            }
                        }
                        else {
                            matchesFilter = true;
                        }

                        // 3. ԭ�У�����¼�ID�Ƿ�ƥ�䣨����ԭ�߼������ȼ�����Provider Name��
                        if (!matchesFilter) {
                            for (uint32_t eventId : provider.eventIds) {
                                char eventIdStr[32];
                                sprintf_s(eventIdStr, "%u", eventId);
                                if (strstr(eventIdStr, filterText) != nullptr) {
                                    matchesFilter = true;
                                    break;
                                }
                            }
                        }
                    }
                    // ֻ��ʾ����ɸѡ��������
                    if (matchesFilter || filterText[0] == '\0')
                    {
                        ImGui::PushID(ansiGuidStr);
                        ImGui::TableNextRow();

                        // ��ʾGUID
                        ImGui::TableSetColumnIndex(0);
                        bool isSelected = (s_selectedGUID.guid == provider.guid);
                        if (ImGui::Selectable(C(ansiGuidStr), isSelected,
                            ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap))
                        {
                            s_selectedGUID = provider;
                        }
                        if (ImGui::BeginPopupContextItem("DLLModuleRightClick")) {  // ʹ�õ�ǰID�������Ĳ˵�

                            if (ImGui::MenuItem(C("����GUID##" + GuidToString(provider.guid)))) {
                                ImGui::SetClipboardText(ansiGuidStr);
                            }ImGui::EndPopup();  // �ؼ��޸�����Ӵ��йرյ����˵�
                        }
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted(C(WCharToString(provider.providerName.c_str())));

                        // ��ʾ�¼�ID�б�
                        ImGui::TableSetColumnIndex(2);
                        std::string eventIdsStr;
                        for (size_t i = 0; i < provider.eventIds.size(); ++i)
                        {
                            if (i > 0)
                                eventIdsStr += ", ";
                            eventIdsStr += std::to_string(provider.eventIds[i]);
                        }
                        ImGui::Text(C("%s"), eventIdsStr.c_str());
                    }
                    ImGui::PopID();
                }

                ImGui::EndTable();
            }
            ImGui::EndChild();
        }
        if (ImGui::GetCurrentContext() == nullptr) return;
        ImGui::Text(C("ETW�¼����"));
        ImGui::Separator();
        ImGui::InputText(C("�Զ���GUID"), customGuidInput, sizeof(customGuidInput), ImGuiInputTextFlags_CharsNoBlank);
        ImGui::SameLine();
        ImGui::Text(C("��ʽ: {xxxxxxxx - xxxx - xxxx - xxxx - xxxxxxxxxxxx}"));

        ImGui::InputInt(C("�¼�����"), (int*)&customLevel, 1, 1);
        ImGui::Text(C("����Χ: 1(��ϸ) - 5(����)"));
        if (customLevel < 1) customLevel = 1;
        if (customLevel > 5) customLevel = 5;

        // ����������Զ���Provider��ť
        if (ImGui::Button(C("����Զ���Provider")) && g_etwMonitor.IsRunning()) {
            GUID customGuid;
            int wideSize = MultiByteToWideChar(CP_ACP, 0, customGuidInput, -1, nullptr, 0);
            std::vector<wchar_t> wideGuid(wideSize);
            MultiByteToWideChar(CP_ACP, 0, customGuidInput, -1, wideGuid.data(), wideSize);
            // ���Դ������ַ�������GUID
            HRESULT hr = IIDFromString(wideGuid.data(), &customGuid);
            if (SUCCEEDED(hr)) {
                bool result = g_etwMonitor.AddProvider(customGuid, customLevel);
                if (result) {
                    kLog.info(C("�ɹ�����Զ���Provider: " + std::string(customGuidInput)), CURRENT_MODULE);
                    // ��������
                    memset(customGuidInput, 0, sizeof(customGuidInput));
                }
            }
            else {
                kLog.err(C("GUID��ʽ��Ч����������"), CURRENT_MODULE);
                // ��ʾ������ʾ
                ImGui::OpenPopup("InvalidGuidPopup");
            }
        }


        // ���ư�ť
        if (ImGui::Button(C("�������##ETWMonitor")) && !g_etwMonitor.IsRunning()) {
            // ��ʼ����TCP/IP�ͽ��̼��
            std::vector<GUID> initialProviders = {
                EtwProviders::TcpIp,
                EtwProviders::Process
            };
            if (g_etwMonitor.Start(initialProviders)) {
                kLog.info(C("ETW��������ɹ�"), CURRENT_MODULE);
            }
            else {
                kLog.err(C("ETW�������ʧ��"), CURRENT_MODULE);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(C("ֹͣ���##ETWMonitor")) && g_etwMonitor.IsRunning()) {
            g_etwMonitor.Stop();
        }
        ImGui::SameLine();
        if (ImGui::Button(C("����¼�##ETWMonitor"))) {
            g_etwMonitor.ClearEvents();
        }

        // ��̬���Provider
        ImGui::Separator();
        ImGui::Text(C("��̬Provider����"));
        if (ImGui::Button(C("���ע�����")) && g_etwMonitor.IsRunning()) {
            g_etwMonitor.AddProvider(EtwProviders::Registry, TRACE_LEVEL_VERBOSE);
        }

        // �¼����˿ؼ�
        static uint32_t filterEventId = 0;
        static char filterProvider[256] = { 0 };
        static uint8_t filterMinLevel = 0;
        ImGui::Separator();
        ImGui::Text(C("�¼�����"));
        ImGui::InputInt(C("�¼�ID"), (int*)&filterEventId);
        ImGui::InputText(C("Provider(GUID/����)"), filterProvider, sizeof(filterProvider));
        ImGui::InputInt(C("��С����(1-5)"), (int*)&filterMinLevel, 1, 1);
        if (filterMinLevel > 5) filterMinLevel = 5;

        // �¼��б�չʾ
        ImGui::Separator();
        ImGui::Text(C("�¼��б� (����: %d)"), g_etwMonitor.GetEvents().size());
        if (ImGui::BeginChild("ETWEventTablescrolling", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.5f - ImGui::GetStyle().ItemSpacing.y), true,
            ImGuiWindowFlags_HorizontalScrollbar)) {

            const ImVec2 outerSize(0.0f, ImGui::GetContentRegionAvail().y - 10);
            if (ImGui::BeginTable("ETWEvents", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, outerSize)) {
                ImGui::TableSetupColumn(C("ID"), ImGuiTableColumnFlags_WidthFixed, 60);
                ImGui::TableSetupColumn(C("ʱ���"), ImGuiTableColumnFlags_WidthFixed, 180);
                ImGui::TableSetupColumn(C("Provider"), ImGuiTableColumnFlags_WidthFixed, 150);
                ImGui::TableSetupColumn(C("�¼�ID"), ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn(C("����"), ImGuiTableColumnFlags_WidthFixed, 60);
                ImGui::TableSetupColumn(C("����"), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                auto events = g_etwMonitor.GetEvents(filterEventId, filterProvider, filterMinLevel);
                for (const auto& ev : events) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("%llu", ev.id);
                    ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(ev.timestamp.c_str());
                    ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(ev.providerName.c_str());
                    ImGui::TableSetColumnIndex(3); ImGui::Text("%u", ev.eventId);
                    ImGui::TableSetColumnIndex(4); ImGui::Text("%hhu", ev.level);
                    ImGui::TableSetColumnIndex(5); ImGui::TextWrapped("%s", ev.description.c_str());
                }
                ImGui::EndTable();
            }
            ImGui::EndChild();
        }
    }
}

#undef CURRENT_MODULE