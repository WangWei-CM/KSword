// =============================
// landing.js
// 作用：渲染发布会首页文案并响应中英文切换。
// =============================

// landingCopyMap 对象用途：
// - 保存首页全部可切换文案；
// - 结构按“主舞台 + 预告板块”组织，避免散乱硬编码。
const landingCopyMap = {
    zh: {
        overviewLink: "产品总览",
        kicker: "KSWORD 5.1 发布主页",
        slogan: "Windows 深度调试，像发布会一样直观",
        subtitle: "面向进程、网络、内存、内核与系统治理的一体化平台。",
        metrics: ["16 个主功能 Tab", "4 个辅助面板", "浅色 / 深色 + 中英双语"],
        ctaPrimary: "进入模块展厅",
        ctaSecondary: "查看截图规范",
        scrollTip: "每个能力板块都提供独立页面与大图展示。",
        stageCaption: "产品主视觉（可替换为宣传大图）",
        stripTitle: "核心模块抢先看",
        stripDesc: "每个板块均有独立页面，采用左文右图的大型展示布局。",
        stripItems: [
            {
                title: "进程控制与诊断",
                desc: "实时进程枚举、控制动作与多层详情联动，适合快速定位系统异常。",
                action: "打开页面",
                fallback: "进程模块主展示图占位"
            },
            {
                title: "网络观测与管控",
                desc: "流量监控、连接管理、限速规则与 HTTPS 解析整合在同一工作区。",
                action: "打开页面",
                fallback: "网络模块主展示图占位"
            },
            {
                title: "内存分析与定位",
                desc: "从附加进程到地址定位，覆盖区域浏览、搜索扫描与十六进制查看。",
                action: "打开页面",
                fallback: "内存模块主展示图占位"
            }
        ]
    },
    en: {
        overviewLink: "Overview",
        kicker: "KSWORD 5.1 Launch Stage",
        slogan: "Windows Deep Debugging, Presented Like a Product Keynote",
        subtitle: "An integrated platform for process, network, memory, kernel, and system governance workflows.",
        metrics: ["16 Primary Tabs", "4 Auxiliary Panels", "Light / Dark + CN / EN"],
        ctaPrimary: "Enter Module Showcase",
        ctaSecondary: "Screenshot Guide",
        scrollTip: "Every capability block has its own dedicated page with large visual sections.",
        stageCaption: "Main product visual (replace with your hero image)",
        stripTitle: "Featured Modules",
        stripDesc: "Each module is split into an independent page using a keynote-style left-copy right-image layout.",
        stripItems: [
            {
                title: "Process Control & Diagnostics",
                desc: "Real-time process enumeration, control actions, and deep linked details for fast troubleshooting.",
                action: "Open Page",
                fallback: "Process module hero image placeholder"
            },
            {
                title: "Network Observation & Governance",
                desc: "Traffic monitor, connection control, rate limits, and HTTPS analysis in one workspace.",
                action: "Open Page",
                fallback: "Network module hero image placeholder"
            },
            {
                title: "Memory Analysis & Localization",
                desc: "From process attach to address localization with region browsing, scans, and hex viewer.",
                action: "Open Page",
                fallback: "Memory module hero image placeholder"
            }
        ]
    }
};

// setTextIfExists 函数用途：
// - 安全设置节点文本，避免空节点导致脚本报错。
// - 传入：selectorText(选择器), textValue(目标文本)。
function setTextIfExists(selectorText, textValue) {
    const targetElement = document.querySelector(selectorText);
    if (targetElement) {
        targetElement.textContent = textValue;
    }
}

// renderLandingCopy 函数用途：
// - 根据当前语言刷新首页主舞台与预告板块文案。
// - 传入：languageCode("zh" 或 "en")。
function renderLandingCopy(languageCode) {
    const copyText = landingCopyMap[languageCode] || landingCopyMap.zh;

    setTextIfExists("#landing-overview-link", copyText.overviewLink);
    setTextIfExists("#landing-kicker", copyText.kicker);
    setTextIfExists("#landing-slogan", copyText.slogan);
    setTextIfExists("#landing-subtitle", copyText.subtitle);
    setTextIfExists("#landing-cta-primary", copyText.ctaPrimary);
    setTextIfExists("#landing-cta-secondary", copyText.ctaSecondary);
    setTextIfExists("#landing-scroll-tip", copyText.scrollTip);
    setTextIfExists("#landing-stage-caption", copyText.stageCaption);
    setTextIfExists("#landing-strip-title", copyText.stripTitle);
    setTextIfExists("#landing-strip-desc", copyText.stripDesc);

    // 指标文本：固定 3 项，保持首页首屏简洁。
    if (Array.isArray(copyText.metrics) && copyText.metrics.length >= 3) {
        setTextIfExists("#landing-metric-1", copyText.metrics[0]);
        setTextIfExists("#landing-metric-2", copyText.metrics[1]);
        setTextIfExists("#landing-metric-3", copyText.metrics[2]);
    }

    // 三个抢先看板块文案同步。
    if (Array.isArray(copyText.stripItems) && copyText.stripItems.length >= 3) {
        for (let stripIndex = 0; stripIndex < 3; stripIndex += 1) {
            const stripItem = copyText.stripItems[stripIndex];
            const selectorPrefix = "#landing-strip-" + String(stripIndex + 1);
            setTextIfExists(selectorPrefix + "-title", stripItem.title || "");
            setTextIfExists(selectorPrefix + "-desc", stripItem.desc || "");
            setTextIfExists(selectorPrefix + "-action", stripItem.action || "");
            setTextIfExists(selectorPrefix + "-fallback", stripItem.fallback || "");
        }
    }
}

// registerLandingLanguageObserver 函数用途：
// - 订阅语言变化并触发首页重渲染。
function registerLandingLanguageObserver() {
    if (!window.kswordLanguage) {
        renderLandingCopy("zh");
        return;
    }

    window.kswordLanguage.onLanguageChange(function onLandingLanguageChanged(languageCode) {
        renderLandingCopy(languageCode);
    });
}

// DOMContentLoaded 监听用途：
// - 页面节点就绪后再执行文案渲染。
document.addEventListener("DOMContentLoaded", registerLandingLanguageObserver);
