#pragma once

// ============================================================
// SandboxUploadActions.h
// 作用：
// - 为各 Dock 右键菜单提供统一的“上传到沙箱 -> VT”入口；
// - 调用方只负责把当前行解析成本地文件路径或 PID；
// - 本文件不保存 API Key，VirusTotalOnlineScan 会继续读取设置中的 virustotal_api_key。
// ============================================================

#include <cstdint>
#include <functional>

#include <QString>

class QAction;
class QMenu;
class QWidget;

namespace ks::online_scan
{
    // SandboxUploadTarget 作用：
    // - 承载一次右键菜单上传动作的解析结果；
    // - filePath 是最终要上传的本地文件路径，sourceText 是结果窗口顶部展示的来源说明。
    struct SandboxUploadTarget
    {
        QString filePath;   // filePath：调用方解析出的本地样本路径。
        QString sourceText; // sourceText：上传来源说明，例如“进程列表 PID=1234”。
        QString errorText;  // errorText：路径解析失败时的明确原因；非空时菜单触发处直接提示。
    };

    // QFilePathResolver 作用：
    // - 菜单触发时延迟解析当前行路径，避免菜单构造阶段读取易变状态；
    // - 返回 SandboxUploadTarget，路径为空时由统一 helper 弹窗提示。
    using QFilePathResolver = std::function<SandboxUploadTarget()>;

    // addVirusTotalSandboxMenu 作用：
    // - 在指定菜单中添加“上传到沙箱”子菜单和“VT”子项；
    // - 子项触发后调用 resolver 解析路径并启动 VirusTotal 上传；
    // - ThreatBook 本轮不添加、不显示灰色项。
    // 入参 menu：目标右键菜单。
    // 入参 parentWidget：错误提示和结果窗口父控件。
    // 入参 resolver：点击 VT 时解析文件路径和来源文本的回调。
    // 返回：VT QAction 指针；menu 为空时返回 nullptr。
    QAction* addVirusTotalSandboxMenu(QMenu* menu, QWidget* parentWidget, QFilePathResolver resolver);

    // uploadFileToVirusTotal 作用：
    // - 对文件路径做本地规范化、存在性/可读性/大小校验；
    // - 校验通过后创建 VirusTotalOnlineScan 并异步上传；
    // - API Key 仍由 VirusTotalOnlineScan 从设置读取。
    // 入参 filePath：本地文件路径，可带引号或简单命令行参数。
    // 入参 sourceText：上传来源说明。
    // 入参 parentWidget：弹窗父控件。
    // 返回：无；错误通过统一弹窗提示，结果通过实时结果窗口展示。
    void uploadFileToVirusTotal(const QString& filePath, const QString& sourceText, QWidget* parentWidget);

    // uploadProcessImageByPid 作用：
    // - 根据 PID 调用 ks::process::QueryProcessPathByPid 解析 EXE；
    // - 路径为空或文件不可读时弹窗提示；
    // - 成功后转交 uploadFileToVirusTotal。
    // 入参 pid：目标进程 PID。
    // 入参 sourceText：上传来源说明。
    // 入参 parentWidget：弹窗父控件。
    // 返回：无。
    void uploadProcessImageByPid(std::uint32_t pid, const QString& sourceText, QWidget* parentWidget);

    // normalizeKernelImagePathForUpload 作用：
    // - 把内核/R0 常见路径转换为 Win32 可读路径；
    // - 支持 \SystemRoot、SystemRoot、%SystemRoot%、\??\C:\ 和 \Device\HarddiskVolume...；
    // - 不保证文件存在，调用方仍需 validateReadableFile。
    // 入参 rawPathText：原始路径文本。
    // 返回：规范化后的路径，无法转换时返回清理后的原文本。
    QString normalizeKernelImagePathForUpload(const QString& rawPathText);

    // extractExistingFilePathForUpload 作用：
    // - 从文件路径或常见命令行片段中提取可存在的文件路径；
    // - 自启动项、服务路径和部分 UI 表格可复用该函数减少误解析。
    // 入参 rawPathText：路径或命令行文本。
    // 返回：存在的文件路径；无法提取时返回规范化后的候选文本。
    QString extractExistingFilePathForUpload(const QString& rawPathText);

    // tryParsePidFromText 作用：
    // - 从表格单元格文本中解析十进制 PID；
    // - 用于 PID、PID/TID、RootPid 等事件列的轻量解析。
    // 入参 pidText：待解析文本。
    // 入参 pidOut：解析成功时写入 PID。
    // 返回：true=解析到非零 PID；false=未解析到有效 PID。
    bool tryParsePidFromText(const QString& pidText, std::uint32_t* pidOut);
}
