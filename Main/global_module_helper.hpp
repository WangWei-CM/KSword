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
        // 确保模块在ImGui上下文销毁前被清理
        assert(ImGui::GetCurrentContext() &&
            "Module destroyed after ImGui context! Use shutdownAllModules() first.");

        unregisterModule(this);
    }

    // 初始化模块（必须在ImGui上下文存在时调用）
    void initialize() {
        assert(ImGui::GetCurrentContext() &&
            "ImGui context must be created before module initialization");

        if (!m_isInitialized) {
            onInitialize();
            m_isInitialized = true;
        }
    }

    // 关闭模块（必须在ImGui上下文销毁前调用）
    void shutdown() {
        if (m_isInitialized) {
            onShutdown();
            m_isInitialized = false;
        }
    }

    // 检查模块是否初始化
    bool isInitialized() const { return m_isInitialized; }

    // 获取模块名称
    const std::string& getName() const { return m_name; }

    // 全局模块注册表
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
    // 子类需要实现初始化逻辑
    virtual void onInitialize() = 0;

    // 子类需要实现关闭逻辑
    virtual void onShutdown() = 0;

private:
    std::string m_name;
    bool m_isInitialized;

    // 禁止拷贝
    ImGuiDependentModule(const ImGuiDependentModule&) = delete;
    ImGuiDependentModule& operator=(const ImGuiDependentModule&) = delete;
};

class ModuleManager {
public:
    // 初始化所有模块
    static void initializeAll() {
        for (auto module : ImGuiDependentModule::getRegistry()) {
            if (!module->isInitialized()) {
                module->initialize();
            }
        }
    }

    // 关闭所有模块
    static void shutdownAll() {
        auto& registry = ImGuiDependentModule::getRegistry();

        // 逆序关闭
        for (auto it = registry.rbegin(); it != registry.rend(); ++it) {
            if ((*it)->isInitialized()) {
                (*it)->shutdown();
            }
        }
    }

    // 检查是否有未关闭的模块
    static bool hasActiveModules() {
        for (auto module : ImGuiDependentModule::getRegistry()) {
            if (module->isInitialized()) {
                return true;
            }
        }
        return false;
    }

    // 在ImGui销毁前调用，确保所有模块已关闭
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

            // 强制关闭所有模块（仅在调试模式下）
#ifdef _DEBUG
            shutdownAll();
#endif
        }
    }
};