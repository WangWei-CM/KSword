// =============================
// overview.js
// 作用：渲染模块总览页，并把模块入口改为整屏翻页分镜。
// =============================

// overviewCopyMap 对象用途：
// - 保存总览页固定文案中英文版本；
// - 统一管理标题、按钮和指标名称。
const overviewCopyMap = {
    zh: {
        homeLink: "↩ 首页",
        title: "Ksword5.1 模块展厅",
        desc: "整页翻屏展示：一屏一个模块，滚动即可像 PPT 一样切换分镜。",
        modulesTitle: "模块展示板块",
        modulesDesc: "向下滚动可逐屏浏览模块，点击按钮进入对应独立页面。",
        footer: "截图目录：Website/Images/ ｜ 命名规范：Website/Images/截图命名与拍摄说明.txt",
        metricLabelModule: "模块数量",
        metricLabelCoverage: "能力覆盖",
        metricLabelMode: "展示模式",
        metricLabelPosition: "产品定位",
        metricCoverageValue: "进程 / 网络 / 内存 / 内核 / 系统治理",
        metricModeValue: "整屏翻页 / 一屏一模块 / 深浅主题",
        metricPositionValue: "Windows 深度调试与分析平台",
        cardAction: "进入模块页",
        noPreviewTitle: "暂无预览图",
        noPreviewDesc: "该模块当前未配置截图计划。"
    },
    en: {
        homeLink: "↩ Home",
        title: "Ksword5.1 Module Showcase",
        desc: "Full-screen paging: one module per screen, scrolling behaves like keynote slide switching.",
        modulesTitle: "Showcase Blocks",
        modulesDesc: "Scroll downward to browse module slides one by one and open the dedicated detail page.",
        footer: "Screenshots: Website/Images/ ｜ Guide: Website/Images/截图命名与拍摄说明.txt",
        metricLabelModule: "Modules",
        metricLabelCoverage: "Coverage",
        metricLabelMode: "Presentation",
        metricLabelPosition: "Positioning",
        metricCoverageValue: "Process / Network / Memory / Kernel / System Governance",
        metricModeValue: "Full-screen Paging / One Module per Screen / Light + Dark",
        metricPositionValue: "Windows Deep Debugging & Analysis Platform",
        cardAction: "Open Module",
        noPreviewTitle: "No Preview",
        noPreviewDesc: "This module currently has no screenshot plan."
    }
};

// overviewSnapObserver 变量用途：
// - 监听当前可见分镜；
// - 同步右侧分页点高亮。
let overviewSnapObserver = null;

// createMetricHtml 函数用途：
// - 生成单个指标卡 HTML。
// - 传入：labelText(指标名), valueText(指标值)。
function createMetricHtml(labelText, valueText) {
    return [
        '<div class="metric-item">',
        '  <div class="metric-label">' + labelText + '</div>',
        '  <div class="metric-value">' + valueText + '</div>',
        '</div>'
    ].join("");
}

// createShowcaseCardHtml 函数用途：
// - 生成单个模块分镜（左文右图）；
// - 传入：tabItem(模块数据), tabIndex(序号), languageCode(语言), actionText(按钮文案), noPreviewTitle(占位标题), noPreviewDesc(占位说明)。
function createShowcaseCardHtml(tabItem, tabIndex, languageCode, actionText, noPreviewTitle, noPreviewDesc) {
    const pickText = window.kswordLanguage ? window.kswordLanguage.pickText : function fallbackText(value) { return value || ""; };
    const pickList = window.kswordLanguage ? window.kswordLanguage.pickList : function fallbackList(value) { return value || []; };

    const localizedTitle = pickText(tabItem.title, languageCode);
    const localizedSummary = pickText(tabItem.summary, languageCode);
    const localizedTags = pickList(tabItem.tags, languageCode);
    const previewShot = Array.isArray(tabItem.screenshotPlan) ? tabItem.screenshotPlan[0] : null;

    const sequenceText = String(tabIndex + 1).padStart(2, "0");
    const revealDelayMs = Math.min((tabIndex + 1) * 60, 880);

    let mediaHtml = "";
    if (previewShot) {
        const shotGoalText = pickText(previewShot.goal, languageCode);
        const imagePath = "../Images/" + previewShot.file;
        mediaHtml = [
            '<div class="showcase-media">',
            '  <img class="showcase-image" src="' + imagePath + '" alt="' + shotGoalText + '" onerror="this.style.display=\'none\';this.nextElementSibling.style.display=\'flex\';">',
            '  <div class="showcase-fallback">',
            '    <div class="showcase-fallback-name">' + previewShot.file + '</div>',
            '    <div>' + shotGoalText + '</div>',
            '  </div>',
            '</div>'
        ].join("");
    } else {
        mediaHtml = [
            '<div class="showcase-media">',
            '  <div class="showcase-fallback" style="display:flex;">',
            '    <div class="showcase-fallback-name">' + noPreviewTitle + '</div>',
            '    <div>' + noPreviewDesc + '</div>',
            '  </div>',
            '</div>'
        ].join("");
    }

    const tagHtml = localizedTags.map(function buildTag(tagText) {
        return '<span class="tag-item">' + tagText + '</span>';
    }).join("");

    return [
        '<article class="showcase-card reveal-on-load overview-snap" style="--reveal-delay:' + String(revealDelayMs) + 'ms;">',
        '  <div class="showcase-copy">',
        '    <div class="showcase-index">#' + sequenceText + '</div>',
        '    <h3 class="showcase-title">' + localizedTitle + '</h3>',
        '    <p class="showcase-summary">' + localizedSummary + '</p>',
        '    <div class="tag-row">' + tagHtml + '</div>',
        '    <a class="action-button primary" href="./' + tabItem.page + '" title="' + actionText + '">' + actionText + '</a>',
        '  </div>',
        mediaHtml,
        '</article>'
    ].join("");
}

// ensureOverviewPagerElement 函数用途：
// - 确保右侧分页点容器存在。
function ensureOverviewPagerElement() {
    let pagerElement = document.querySelector("#overview-pager");
    if (!pagerElement) {
        pagerElement = document.createElement("nav");
        pagerElement.id = "overview-pager";
        pagerElement.className = "overview-pager";
        pagerElement.setAttribute("aria-label", "Overview slide navigation");
        document.body.appendChild(pagerElement);
    }
    return pagerElement;
}

// updateOverviewPagerActive 函数用途：
// - 根据可见分镜序号更新分页点高亮。
// - 传入：activeIndex(0 基序号)。
function updateOverviewPagerActive(activeIndex) {
    const buttonList = document.querySelectorAll("#overview-pager .overview-pager-dot");
    buttonList.forEach(function updateDotState(dotButton, dotIndex) {
        const isActive = dotIndex === activeIndex;
        dotButton.classList.toggle("is-active", isActive);
        dotButton.setAttribute("aria-current", isActive ? "true" : "false");
    });
}

// markOverviewSnapSections 函数用途：
// - 标记总览页所有分镜节点，供翻页定位和分页点使用。
function markOverviewSnapSections() {
    const sectionList = [];
    const heroElement = document.querySelector(".overview-page .overview-hero");

    if (heroElement) {
        heroElement.classList.add("overview-snap");
        sectionList.push(heroElement);
    }

    const moduleSlideList = Array.from(document.querySelectorAll(".overview-page .showcase-card"));
    moduleSlideList.forEach(function appendModuleSlide(moduleSlideElement) {
        moduleSlideElement.classList.add("overview-snap");
        sectionList.push(moduleSlideElement);
    });

    sectionList.forEach(function assignSectionIndex(sectionElement, sectionIndex) {
        sectionElement.setAttribute("data-overview-slide", String(sectionIndex));
    });

    return sectionList;
}

// bindOverviewSnapPager 函数用途：
// - 生成分页点；
// - 绑定点击跳转与可见分镜高亮同步。
function bindOverviewSnapPager() {
    if (overviewSnapObserver) {
        overviewSnapObserver.disconnect();
        overviewSnapObserver = null;
    }

    const snapSectionList = markOverviewSnapSections();
    const pagerElement = ensureOverviewPagerElement();

    if (snapSectionList.length <= 1) {
        pagerElement.innerHTML = "";
        pagerElement.style.display = "none";
        return;
    }

    pagerElement.style.display = "flex";
    pagerElement.innerHTML = snapSectionList.map(function buildDot(_, sectionIndex) {
        const sectionText = String(sectionIndex + 1);
        return '<button class="overview-pager-dot" type="button" data-overview-target="' + String(sectionIndex) + '" aria-label="Jump to slide ' + sectionText + '"></button>';
    }).join("");

    const dotButtonList = pagerElement.querySelectorAll(".overview-pager-dot");
    dotButtonList.forEach(function bindJumpEvent(dotButton) {
        dotButton.addEventListener("click", function onPagerDotClicked() {
            const targetIndex = Number(dotButton.getAttribute("data-overview-target"));
            const targetSection = snapSectionList[targetIndex];
            if (targetSection) {
                targetSection.scrollIntoView({ behavior: "smooth", block: "start" });
            }
        });
    });

    updateOverviewPagerActive(0);

    if (typeof window.IntersectionObserver !== "function") {
        return;
    }

    overviewSnapObserver = new window.IntersectionObserver(function onVisibilityChanged(entryList) {
        entryList.forEach(function updateCurrentSlide(entryItem) {
            if (!entryItem.isIntersecting) {
                return;
            }
            const targetIndex = Number(entryItem.target.getAttribute("data-overview-slide"));
            if (!Number.isNaN(targetIndex)) {
                updateOverviewPagerActive(targetIndex);
            }
        });
    }, {
        threshold: 0.55
    });

    snapSectionList.forEach(function observeSection(sectionElement) {
        overviewSnapObserver.observe(sectionElement);
    });
}

// renderOverviewPage 函数用途：
// - 按当前语言渲染总览页全部内容。
// - 传入：languageCode("zh" 或 "en")。
function renderOverviewPage(languageCode) {
    const copyText = overviewCopyMap[languageCode] || overviewCopyMap.zh;

    const homeLinkElement = document.querySelector("#overview-home-link");
    const titleElement = document.querySelector("#overview-title");
    const descElement = document.querySelector("#overview-desc");
    const modulesTitleElement = document.querySelector("#modules-title");
    const modulesDescElement = document.querySelector("#modules-desc");
    const footerElement = document.querySelector("#overview-footer");
    const metricsElement = document.querySelector("#overview-metrics");
    const modulesShowcaseElement = document.querySelector("#modules-showcase");

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

    if (modulesShowcaseElement && Array.isArray(window.kswordTabs)) {
        modulesShowcaseElement.innerHTML = window.kswordTabs.map(function buildShowcase(tabItem, tabIndex) {
            return createShowcaseCardHtml(
                tabItem,
                tabIndex,
                languageCode,
                copyText.cardAction,
                copyText.noPreviewTitle,
                copyText.noPreviewDesc
            );
        }).join("");
    }

    bindOverviewSnapPager();
}

// registerOverviewLanguageObserver 函数用途：
// - 监听语言切换并重新渲染总览页。
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
// - 确保节点就绪后再初始化渲染。
document.addEventListener("DOMContentLoaded", registerOverviewLanguageObserver);

