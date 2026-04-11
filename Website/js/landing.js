// =============================
// landing.js
// 作用：渲染发布会首页文案并响应中英文切换。
// =============================

// landingCopyMap 对象用途：
// - 保存首页全部可切换文案；
// - 结构按“主舞台 + Changelog”组织，避免散乱硬编码。
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
        changelogTitle: "更新日志",
        changelogDesc: "读取网站根目录 change.log 文件内容。",
        changelogLoading: "正在读取 change.log ...",
        changelogEmpty: "change.log 目前为空。",
        changelogError: "读取 change.log 失败，请稍后重试。"
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
        changelogTitle: "Changelog",
        changelogDesc: "Read content from the website root change.log file.",
        changelogLoading: "Loading change.log ...",
        changelogEmpty: "change.log is currently empty.",
        changelogError: "Failed to load change.log. Please try again later."
    }
};

// landingChangeLogState 变量用途：
// - 维护 changelog 文件读取状态；
// - 避免重复请求。
const landingChangeLogState = {
    started: false,
    loaded: false,
    failed: false,
    text: ""
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

// getCurrentLandingLanguage 函数用途：
// - 读取当前语言代码；
// - 在语言模块未就绪时回退中文。
function getCurrentLandingLanguage() {
    if (window.kswordLanguage && typeof window.kswordLanguage.getLanguage === "function") {
        return window.kswordLanguage.getLanguage();
    }
    return "zh";
}

// renderLandingChangelog 函数用途：
// - 按当前状态渲染 changelog 面板文本。
// - 传入：copyText(当前语言文案)。
function renderLandingChangelog(copyText) {
    setTextIfExists("#landing-changelog-title", copyText.changelogTitle);
    setTextIfExists("#landing-changelog-desc", copyText.changelogDesc);

    const changelogContentElement = document.querySelector("#landing-changelog-content");
    if (!changelogContentElement) {
        return;
    }

    if (!landingChangeLogState.started || (!landingChangeLogState.loaded && !landingChangeLogState.failed)) {
        changelogContentElement.textContent = copyText.changelogLoading;
        return;
    }

    if (landingChangeLogState.failed) {
        changelogContentElement.textContent = copyText.changelogError;
        return;
    }

    const normalizedLogText = landingChangeLogState.text.replace(/\r\n/g, "\n").trim();
    changelogContentElement.textContent = normalizedLogText || copyText.changelogEmpty;
}

// ensureLandingChangelogLoaded 函数用途：
// - 只请求一次 change.log；
// - 请求结束后按当前语言刷新显示。
function ensureLandingChangelogLoaded() {
    if (landingChangeLogState.started) {
        return;
    }

    landingChangeLogState.started = true;

    fetch("./change.log?ts=" + String(Date.now()), { cache: "no-store" })
        .then(function onResponse(response) {
            if (!response.ok) {
                throw new Error("HTTP " + String(response.status));
            }
            return response.text();
        })
        .then(function onTextLoaded(fileText) {
            landingChangeLogState.loaded = true;
            landingChangeLogState.failed = false;
            landingChangeLogState.text = typeof fileText === "string" ? fileText : "";
        })
        .catch(function onLoadFailed() {
            landingChangeLogState.loaded = false;
            landingChangeLogState.failed = true;
            landingChangeLogState.text = "";
        })
        .finally(function onLoadSettled() {
            renderLandingCopy(getCurrentLandingLanguage());
        });
}

// renderLandingCopy 函数用途：
// - 根据当前语言刷新首页主舞台与 changelog 文案。
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

    // 指标文本：固定 3 项，保持首页首屏简洁。
    if (Array.isArray(copyText.metrics) && copyText.metrics.length >= 3) {
        setTextIfExists("#landing-metric-1", copyText.metrics[0]);
        setTextIfExists("#landing-metric-2", copyText.metrics[1]);
        setTextIfExists("#landing-metric-3", copyText.metrics[2]);
    }

    renderLandingChangelog(copyText);
}

// registerLandingLanguageObserver 函数用途：
// - 订阅语言变化并触发首页重渲染。
function registerLandingLanguageObserver() {
    if (!window.kswordLanguage) {
        renderLandingCopy("zh");
        ensureLandingChangelogLoaded();
        return;
    }

    window.kswordLanguage.onLanguageChange(function onLandingLanguageChanged(languageCode) {
        renderLandingCopy(languageCode);
    });

    ensureLandingChangelogLoaded();
}

// DOMContentLoaded 监听用途：
// - 页面节点就绪后再执行文案渲染。
document.addEventListener("DOMContentLoaded", registerLandingLanguageObserver);
