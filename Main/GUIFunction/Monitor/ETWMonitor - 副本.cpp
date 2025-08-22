#define CURRENT_MODULE C("ETW监控")
void ETWMonitorMain() {
    if (ImGui::CollapsingHeader(C("ETW监控"))) {
        // 用于存储筛选文本的缓冲区
        if (firstRun) {
            firstRun = false;
            std::thread(EnumerateAllEtwProvidersWithEvents).detach(); // 异步加载ETW Providers
        }
        if (ImGui::BeginChild("ETWProviderScrolling", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.5f - ImGui::GetStyle().ItemSpacing.y), true,
            ImGuiWindowFlags_HorizontalScrollbar)) {

            // 获取ETW Providers数据
            // 创建筛选输入框
            ImGui::InputText(C("筛选 Provider 或事件ID..."), filterText, IM_ARRAYSIZE(filterText));

            // 创建表格
            if (ImGui::BeginTable(C("EtwProvidersTable"), 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY))
            {
                // 设置表头
                ImGui::TableSetupColumn(C("Provider GUID"), ImGuiTableColumnFlags_WidthFixed, 300.0f);
                ImGui::TableSetupColumn(C("事件ID列表"), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                // 遍历所有Providers
                for (const auto& provider : etwProviders)
                {

                    // 将GUID转换为字符串以便显示和筛选
                    WCHAR guidStr[64];
                    StringFromGUID2(provider.guid, guidStr, IM_ARRAYSIZE(guidStr));
                    char ansiGuidStr[64];
                    WideCharToMultiByte(CP_ACP, 0, guidStr, -1, ansiGuidStr, sizeof(ansiGuidStr), NULL, NULL);

                    // 检查是否符合筛选条件
                    bool matchesFilter = false;

                    if (strstr(ansiGuidStr, filterText) != nullptr) {
                        matchesFilter = true;
                    }
                    else {
                        // 2. 新增：检查Provider Name是否匹配（不分大小写包含）
                        // 宽字符Provider Name转多字节（与filterText编码一致）
                        char providerNameA[512] = { 0 };
                        WideCharToMultiByte(
                            CP_ACP,
                            0,
                            provider.providerName.c_str(),  // 使用Provider的名称（宽字符）
                            -1,
                            providerNameA,
                            sizeof(providerNameA),
                            NULL,
                            NULL
                        );
                        // 不分大小写模糊匹配（使用_stricmp逐字符比较子串）
                        if (_strnicmp(providerNameA, filterText, strlen(filterText)) != 0) {
                            // 若前缀不匹配，再检查是否包含（自定义不分大小写strstr）
                            auto w2a = [](const wchar_t* wstr) { char b[512] = { 0 }; WideCharToMultiByte(CP_ACP, 0, wstr, -1, b, 512, nullptr, nullptr); return b; };
                            char* findPos = StrStrIA(w2a(provider.providerName.c_str()), filterText);
                            if (findPos != nullptr) {
                                matchesFilter = true;
                            }
                        }
                        else {
                            matchesFilter = true;
                        }

                        // 3. 原有：检查事件ID是否匹配（保留原逻辑，优先级低于Provider Name）
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
                    // 只显示符合筛选条件的项
                    if (matchesFilter || filterText[0] == '\0')
                    {
                        ImGui::PushID(ansiGuidStr);
                        ImGui::TableNextRow();

                        // 显示GUID
                        ImGui::TableSetColumnIndex(0);
                        bool isSelected = (s_selectedGUID.guid == provider.guid);
                        if (ImGui::Selectable(C(ansiGuidStr), isSelected,
                            ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap))
                        {
                            s_selectedGUID = provider;
                        }
                        if (ImGui::BeginPopupContextItem("DLLModuleRightClick")) {  // 使用当前ID的上下文菜单

                            if (ImGui::MenuItem(C("复制GUID##" + GuidToString(provider.guid)))) {
                                ImGui::SetClipboardText(ansiGuidStr);
                            }ImGui::EndPopup();  // 关键修复：添加此行关闭弹出菜单
                        }
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted(C(WCharToString(provider.providerName.c_str())));

                        // 显示事件ID列表
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
        ImGui::Text(C("ETW事件监控"));
        ImGui::Separator();
        ImGui::InputText(C("自定义GUID"), customGuidInput, sizeof(customGuidInput), ImGuiInputTextFlags_CharsNoBlank);
        ImGui::SameLine();
        ImGui::Text(C("格式: {xxxxxxxx - xxxx - xxxx - xxxx - xxxxxxxxxxxx}"));

        ImGui::InputInt(C("事件级别"), (int*)&customLevel, 1, 1);
        ImGui::Text(C("级别范围: 1(详细) - 5(致命)"));
        if (customLevel < 1) customLevel = 1;
        if (customLevel > 5) customLevel = 5;

        // 新增：添加自定义Provider按钮
        if (ImGui::Button(C("添加自定义Provider")) && g_etwMonitor.IsRunning()) {
            GUID customGuid;
            int wideSize = MultiByteToWideChar(CP_ACP, 0, customGuidInput, -1, nullptr, 0);
            std::vector<wchar_t> wideGuid(wideSize);
            MultiByteToWideChar(CP_ACP, 0, customGuidInput, -1, wideGuid.data(), wideSize);
            // 尝试从输入字符串解析GUID
            HRESULT hr = IIDFromString(wideGuid.data(), &customGuid);
            if (SUCCEEDED(hr)) {
                bool result = g_etwMonitor.AddProvider(customGuid, customLevel);
                if (result) {
                    kLog.info(C("成功添加自定义Provider: " + std::string(customGuidInput)), CURRENT_MODULE);
                    // 清空输入框
                    memset(customGuidInput, 0, sizeof(customGuidInput));
                }
            }
            else {
                kLog.err(C("GUID格式无效，请检查输入"), CURRENT_MODULE);
                // 显示错误提示
                ImGui::OpenPopup("InvalidGuidPopup");
            }
        }


        // 控制按钮
        if (ImGui::Button(C("启动监控##ETWMonitor")) && !g_etwMonitor.IsRunning()) {
            // 初始启动TCP/IP和进程监控
            std::vector<GUID> initialProviders = {
                EtwProviders::TcpIp,
                EtwProviders::Process
            };
            if (g_etwMonitor.Start(initialProviders)) {
                kLog.info(C("ETW监控启动成功"), CURRENT_MODULE);
            }
            else {
                kLog.err(C("ETW监控启动失败"), CURRENT_MODULE);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(C("停止监控##ETWMonitor")) && g_etwMonitor.IsRunning()) {
            g_etwMonitor.Stop();
        }
        ImGui::SameLine();
        if (ImGui::Button(C("清空事件##ETWMonitor"))) {
            g_etwMonitor.ClearEvents();
        }

        // 动态添加Provider
        ImGui::Separator();
        ImGui::Text(C("动态Provider管理"));
        if (ImGui::Button(C("添加注册表监控")) && g_etwMonitor.IsRunning()) {
            g_etwMonitor.AddProvider(EtwProviders::Registry, TRACE_LEVEL_VERBOSE);
        }

        // 事件过滤控件
        static uint32_t filterEventId = 0;
        static char filterProvider[256] = { 0 };
        static uint8_t filterMinLevel = 0;
        ImGui::Separator();
        ImGui::Text(C("事件过滤"));
        ImGui::InputInt(C("事件ID"), (int*)&filterEventId);
        ImGui::InputText(C("Provider(GUID/名称)"), filterProvider, sizeof(filterProvider));
        ImGui::InputInt(C("最小级别(1-5)"), (int*)&filterMinLevel, 1, 1);
        if (filterMinLevel > 5) filterMinLevel = 5;

        // 事件列表展示
        ImGui::Separator();
        ImGui::Text(C("事件列表 (总数: %d)"), g_etwMonitor.GetEvents().size());
        if (ImGui::BeginChild("ETWEventTablescrolling", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.5f - ImGui::GetStyle().ItemSpacing.y), true,
            ImGuiWindowFlags_HorizontalScrollbar)) {

            const ImVec2 outerSize(0.0f, ImGui::GetContentRegionAvail().y - 10);
            if (ImGui::BeginTable("ETWEvents", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, outerSize)) {
                ImGui::TableSetupColumn(C("ID"), ImGuiTableColumnFlags_WidthFixed, 60);
                ImGui::TableSetupColumn(C("时间戳"), ImGuiTableColumnFlags_WidthFixed, 180);
                ImGui::TableSetupColumn(C("Provider"), ImGuiTableColumnFlags_WidthFixed, 150);
                ImGui::TableSetupColumn(C("事件ID"), ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn(C("级别"), ImGuiTableColumnFlags_WidthFixed, 60);
                ImGui::TableSetupColumn(C("详情"), ImGuiTableColumnFlags_WidthStretch);
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