// =============================
// overview.js
// 作用：渲染第二页产品总览信息和模块矩阵入口。
// =============================

// overviewCopyMap 对象用途：
// - 保存总览页静态文案中英文版本。
const overviewCopyMap = {
    zh: {
        homeLink: "↩ 首页",
        title: "Ksword5.1 产品总览",
        desc: "从进程、内存、网络到内核对象，构建 Windows 深度调试的一体化能力矩阵。",
        modulesTitle: "模块矩阵",
        modulesDesc: "点击任意模块，进入独立子页面查看详细能力与截图位清单。",
        footer: "截图文件目录：Website/Images/ ｜ 命名规范：Website/Images/截图命名与拍摄说明.txt",
        metricLabelModule: "模块数量",
        metricLabelCoverage: "覆盖范围",
        metricLabelMode: "界面模式",
        metricLabelPosition: "产品定位",
        metricCoverageValue: "进程 / 网络 / 内存 / 内核",
        metricModeValue: "浅色 + 深色 / 中文 + English",
        metricPositionValue: "Windows 内核调试平台",
        cardAction: "查看模块详情"
    },
    en: {
        homeLink: "↩ Home",
        title: "Ksword5.1 Product Overview",
        desc: "From process, memory, and network to kernel internals, it builds an integrated Windows deep-debugging capability matrix.",
        modulesTitle: "Module Matrix",
        modulesDesc: "Click any module to open its dedicated subpage with capabilities and screenshot checklist.",
        footer: "Screenshots: Website/Images/ ｜ Guide: Website/Images/截图命名与拍摄说明.txt",
        metricLabelModule: "Modules",
        metricLabelCoverage: "Coverage",
        metricLabelMode: "Modes",
        metricLabelPosition: "Positioning",
        metricCoverageValue: "Process / Network / Memory / Kernel",
        metricModeValue: "Light + Dark / Chinese + English",
        metricPositionValue: "Windows Kernel Debugging Platform",
        cardAction: "View Details"
    }
};

// createMetricHtml 函数用途：
// - 创建单个指标卡片 HTML 文本。
// - 传入：labelText(标签), valueText(值)。
// - 返回：HTML 字符串。
function createMetricHtml(labelText, valueText) {
    return [
        '<div class="metric-item">',
        '  <div class="metric-label">' + labelText + '</div>',
        '  <div class="metric-value">' + valueText + '</div>',
        '</div>'
    ].join("");
}

// createModuleCardHtml 函数用途：
// - 渲染模块卡片中英文内容与截图占位。
// - 传入：tabItem(模块数据), languageCode(当前语言), actionText(按钮文本)。
// - 返回：模块卡片 HTML 字符串。
function createModuleCardHtml(tabItem, languageCode, actionText) {
    const pickText = window.kswordLanguage ? window.kswordLanguage.pickText : function fallbackText(value) { return value || ""; };
    const pickList = window.kswordLanguage ? window.kswordLanguage.pickList : function fallbackList(value) { return value || []; };

    const localizedTitle = pickText(tabItem.title, languageCode);
    const localizedSummary = pickText(tabItem.summary, languageCode);
    const localizedTags = pickList(tabItem.tags, languageCode);
    const previewShot = Array.isArray(tabItem.screenshotPlan) ? tabItem.screenshotPlan[0] : null;

    let screenshotHtml = "";
    if (previewShot) {
        const shotGoal = pickText(previewShot.goal, languageCode);
        const shotPath = "../Images/" + previewShot.file;
        screenshotHtml = [
            '<div class="shot-placeholder">',
            '  <img class="shot-image" src="' + shotPath + '" alt="' + shotGoal + '" onerror="this.style.display=\'none\';this.nextElementSibling.style.display=\'flex\';">',
            '  <div class="shot-fallback">',
            '    <div class="shot-name">' + previewShot.file + '</div>',
            '    <div class="shot-desc">' + shotGoal + '</div>',
            '  </div>',
            '</div>'
        ].join("");
    }

    const tagHtml = localizedTags.map(function buildTag(tagText) {
        return '<span class="tag-item">' + tagText + '</span>';
    }).join("");

    return [
        '<article class="module-card">',
        '  <div class="module-title">' + localizedTitle + '</div>',
        '  <div class="module-summary">' + localizedSummary + '</div>',
        '  <div class="tag-row">' + tagHtml + '</div>',
        screenshotHtml,
        '  <a class="action-button" href="./' + tabItem.page + '" title="' + actionText + '">',
        '    <span>➜</span><span>' + actionText + '</span>',
        '  </a>',
        '</article>'
    ].join("");
}

// renderOverviewPage 函数用途：
// - 渲染总览页静态文本、指标卡片和模块列表。
// - 传入：languageCode(当前语言代码)。
function renderOverviewPage(languageCode) {
    const copyText = overviewCopyMap[languageCode] || overviewCopyMap.zh;

    const homeLinkElement = document.querySelector("#overview-home-link");
    const titleElement = document.querySelector("#overview-title");
    const descElement = document.querySelector("#overview-desc");
    const modulesTitleElement = document.querySelector("#modules-title");
    const modulesDescElement = document.querySelector("#modules-desc");
    const footerElement = document.querySelector("#overview-footer");
    const metricsElement = document.querySelector("#overview-metrics");
    const modulesGridElement = document.querySelector("#modules-grid");

    if (homeLinkElement) {
        homeLinkElement.textContent = copyText.homeLink;
    }

    if (titleElement) {
        titleElement.textContent = copyText.title;
    }

    if (descElement) {
        descElement.textContent = copyText.desc;
    }

    if (modulesTitleElement) {
        modulesTitleElement.textContent = copyText.modulesTitle;
    }

    if (modulesDescElement) {
        modulesDescElement.textContent = copyText.modulesDesc;
    }

    if (footerElement) {
        footerElement.textContent = copyText.footer;
    }

    if (metricsElement) {
        const moduleCountValue = String(Array.isArray(window.kswordTabs) ? window.kswordTabs.length : 0);
        metricsElement.innerHTML = [
            createMetricHtml(copyText.metricLabelModule, moduleCountValue),
            createMetricHtml(copyText.metricLabelCoverage, copyText.metricCoverageValue),
            createMetricHtml(copyText.metricLabelMode, copyText.metricModeValue),
            createMetricHtml(copyText.metricLabelPosition, copyText.metricPositionValue)
        ].join("");
    }

    if (modulesGridElement && Array.isArray(window.kswordTabs)) {
        modulesGridElement.innerHTML = window.kswordTabs.map(function buildCard(tabItem) {
            return createModuleCardHtml(tabItem, languageCode, copyText.cardAction);
        }).join("");
    }
}

// registerOverviewLanguageObserver 函数用途：
// - 订阅语言变化并触发总览页重渲染。
function registerOverviewLanguageObserver() {
    if (!window.kswordLanguage) {
        renderOverviewPage("zh");
        return;
    }

    window.kswordLanguage.onLanguageChange(function onOverviewLanguageChanged(languageCode) {
        renderOverviewPage(languageCode);
    });
}

// DOMContentLoaded 监听用途：
// - 页面节点就绪后再初始化渲染逻辑。
document.addEventListener("DOMContentLoaded", registerOverviewLanguageObserver);
