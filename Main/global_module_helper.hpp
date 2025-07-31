#pragma once
#include <vector>
#include <string>
#include <functional>
#include <cassert>

class ImGuiDependentModule {
public:
    explicit ImGuiDependentModule(const std::string& name)
        : m_name(name), m_isInitialized(false) {
        registerModule(this);
    }

    virtual ~ImGuiDependentModule() {
        // ȷ��ģ����ImGui����������ǰ������
        assert(ImGui::GetCurrentContext() &&
            "Module destroyed after ImGui context! Use shutdownAllModules() first.");

        unregisterModule(this);
    }

    // ��ʼ��ģ�飨������ImGui�����Ĵ���ʱ���ã�
    void initialize() {
        assert(ImGui::GetCurrentContext() &&
            "ImGui context must be created before module initialization");

        if (!m_isInitialized) {
            onInitialize();
            m_isInitialized = true;
        }
    }

    // �ر�ģ�飨������ImGui����������ǰ���ã�
    void shutdown() {
        if (m_isInitialized) {
            onShutdown();
            m_isInitialized = false;
        }
    }

    // ���ģ���Ƿ��ʼ��
    bool isInitialized() const { return m_isInitialized; }

    // ��ȡģ������
    const std::string& getName() const { return m_name; }

    // ȫ��ģ��ע���
    static std::vector<ImGuiDependentModule*>& getRegistry() {
        static std::vector<ImGuiDependentModule*> registry;
        return registry;
    }

    static void registerModule(ImGuiDependentModule* module) {
        getRegistry().push_back(module);
    }

    static void unregisterModule(ImGuiDependentModule* module) {
        auto& registry = getRegistry();
        registry.erase(std::remove(registry.begin(), registry.end(), module), registry.end());
    }

protected:
    // ������Ҫʵ�ֳ�ʼ���߼�
    virtual void onInitialize() = 0;

    // ������Ҫʵ�ֹر��߼�
    virtual void onShutdown() = 0;

private:
    std::string m_name;
    bool m_isInitialized;

    // ��ֹ����
    ImGuiDependentModule(const ImGuiDependentModule&) = delete;
    ImGuiDependentModule& operator=(const ImGuiDependentModule&) = delete;
};

class ModuleManager {
public:
    // ��ʼ������ģ��
    static void initializeAll() {
        for (auto module : ImGuiDependentModule::getRegistry()) {
            if (!module->isInitialized()) {
                module->initialize();
            }
        }
    }

    // �ر�����ģ��
    static void shutdownAll() {
        auto& registry = ImGuiDependentModule::getRegistry();

        // ����ر�
        for (auto it = registry.rbegin(); it != registry.rend(); ++it) {
            if ((*it)->isInitialized()) {
                (*it)->shutdown();
            }
        }
    }

    // ����Ƿ���δ�رյ�ģ��
    static bool hasActiveModules() {
        for (auto module : ImGuiDependentModule::getRegistry()) {
            if (module->isInitialized()) {
                return true;
            }
        }
        return false;
    }

    // ��ImGui����ǰ���ã�ȷ������ģ���ѹر�
    static void preImGuiShutdown() {
        if (hasActiveModules()) {
            std::vector<std::string> activeModules;
            for (auto module : ImGuiDependentModule::getRegistry()) {
                if (module->isInitialized()) {
                    activeModules.push_back(module->getName());
                }
            }

            std::string errorMsg = "The following modules are still active:\n";
            for (const auto& name : activeModules) {
                errorMsg += "  - " + name + "\n";
            }
            errorMsg += "Call ModuleManager::shutdownAll() before destroying ImGui context.";

            assert(false && errorMsg.c_str());

            // ǿ�ƹر�����ģ�飨���ڵ���ģʽ�£�
#ifdef _DEBUG
            shutdownAll();
#endif
        }
    }
};