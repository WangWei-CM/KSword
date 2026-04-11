#pragma once

// ============================================================
// StartupSplash.h
// 作用：
// - 提供全局可复用的“启动画面控制器”；
// - 对外统一 show/hide/progress 三个控制接口；
// - 通过 PImpl 隐藏 Win32/GDI+ 细节，避免污染公共头依赖。
// ============================================================

#include <memory>
#include <string>

// kStartupSplash 作用：
// - 管理原生启动窗口生命周期；
// - 对外提供 show/hide/progress 控制；
// - 供 main 与其他模块在启动阶段统一复用。
class kStartupSplash final
{
public:
    // 构造函数作用：
    // - 创建实现对象占位；
    // - 不立即创建窗口，直到调用 show。
    // 调用方式：全局对象自动构造，或业务按需实例化。
    kStartupSplash();

    // 析构函数作用：
    // - 释放启动窗口与图像资源；
    // - 保证进程退出前窗口句柄被回收。
    ~kStartupSplash();

    // 禁止拷贝，避免多个实例重复持有同一原生窗口资源。
    kStartupSplash(const kStartupSplash&) = delete;
    kStartupSplash& operator=(const kStartupSplash&) = delete;

    // show 作用：
    // - 初始化并显示启动窗口；
    // - 首次调用失败时返回 false。
    // 调用方式：启动流程中最早阶段调用。
    // 返回：true=显示成功；false=初始化或显示失败。
    bool show();

    // hide 作用：
    // - 隐藏并销毁启动窗口；
    // - 可重复调用，重复调用安全无副作用。
    // 调用方式：主窗口首帧后或兜底超时回调中调用。
    void hide();

    // progress 作用：
    // - 更新启动状态文案和进度百分比；
    // - 若窗口未显示或初始化失败，函数会静默返回。
    // 调用方式：show 成功后可多次调用。
    // 入参 operationName：当前操作名称（UTF-8 文本）。
    // 入参 progressPercent：进度百分比（0~100，超界会自动裁剪）。
    void progress(const std::string& operationName, int progressPercent);

    // ready 作用：
    // - 返回当前启动窗口是否可用；
    // - 用于外层判断是否需要继续更新进度。
    // 返回：true=窗口已初始化可用；false=不可用。
    bool ready() const;

private:
    // Impl 前置声明：
    // - 隐藏 Win32/GDI+ 成员，避免在头文件暴露平台细节。
    class Impl;

    // m_impl 作用：
    // - 启动画面控制实现体；
    // - 持有窗口句柄、GDI+ 资源、布局参数。
    std::unique_ptr<Impl> m_impl;
};

// kSplash 作用：
// - 全局启动画面控制对象；
// - 通过 Framework.h 暴露，供整个程序直接调用。
extern kStartupSplash kSplash;
