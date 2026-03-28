// =============================
// detail.js
// 作用：渲染各 Tab 详细介绍并支持中英文切换。
// =============================

// detailCopyMap 对象用途：
// - 保存详情页固定文本的中英文版本。
const detailCopyMap = {
    zh: {
        homeLink: "↩ 首页",
        overviewLink: "返回总览",
        subtitle: "模块详细能力说明",
        sectionOverview: "模块定位",
        sectionFeatures: "核心功能",
        sectionWorkflow: "推荐使用流程",
        sectionShots: "截图预留位",
        shotTip: "请将对应截图放入 Website/Images，并使用下方指定文件名。",
        footer: "截图规范文档：Website/Images/截图命名与拍摄说明.txt",
        detailTitleSuffix: "Tab 详细介绍",
        missingTitle: "未找到对应模块",
        missingText: "请返回总览页重新选择模块。"
    },
    en: {
        homeLink: "↩ Home",
        overviewLink: "Back to Overview",
        subtitle: "Detailed module capabilities",
        sectionOverview: "Module Positioning",
        sectionFeatures: "Core Capabilities",
        sectionWorkflow: "Recommended Workflow",
        sectionShots: "Screenshot Placeholders",
        shotTip: "Put screenshots into Website/Images using the exact filenames below.",
        footer: "Guide: Website/Images/截图命名与拍摄说明.txt",
        detailTitleSuffix: "Tab Details",
        missingTitle: "Module Not Found",
        missingText: "Please return to Overview and pick a module again."
    }
};

// toListHtml 函数用途：
// - 将文本数组渲染为 <li> HTML。
// - 传入：itemList(字符串数组)。
// - 返回：列表 HTML 字符串。
function toListHtml(itemList) {
    return itemList.map(function buildListItem(itemText) {
        return "<li>" + itemText + "</li>";
    }).join("");
}

// toShotHtml 函数用途：
// - 渲染截图区域：优先显示真实图片，缺失时回退占位说明。
// - 传入：shotList(截图计划数组), languageCode(当前语言)。
// - 返回：截图区域 HTML 字符串。
function toShotHtml(shotList, languageCode) {
    const pickText = window.kswordLanguage ? window.kswordLanguage.pickText : function fallbackText(value) { return value || ""; };

    return shotList.map(function buildShotItem(shotItem) {
        const shotGoalText = pickText(shotItem.goal, languageCode);
        const imagePath = "../Images/" + shotItem.file;

        return [
            '<div class="shot-placeholder">',
            '  <img class="shot-image" src="' + imagePath + '" alt="' + shotGoalText + '" onerror="this.style.display=\'none\';this.nextElementSibling.style.display=\'flex\';">',
            '  <div class="shot-fallback">',
            '    <div class="shot-name">' + shotItem.file + '</div>',
            '    <div class="shot-desc">' + shotGoalText + '</div>',
            '  </div>',
            '</div>'
        ].join("");
    }).join("");
}

// renderTabDetailPage 函数用途：
// - 根据当前语言和 slug 渲染详情页全部内容。
// - 传入：languageCode("zh" 或 "en")。
function renderTabDetailPage(languageCode) {
    const pickText = window.kswordLanguage ? window.kswordLanguage.pickText : function fallbackText(value) { return value || ""; };
    const pickList = window.kswordLanguage ? window.kswordLanguage.pickList : function fallbackList(value) { return value || []; };

    const copyText = detailCopyMap[languageCode] || detailCopyMap.zh;
    const tabSlug = document.body.getAttribute("data-tab-slug");
    const tabItem = window.findTabBySlug ? window.findTabBySlug(tabSlug) : null;

    const homeLinkElement = document.querySelector("#detail-home-link");
    const overviewLinkElement = document.querySelector("#detail-overview-link");
    const subtitleElement = document.querySelector("#detail-subtitle");
    const titleElement = document.querySelector("#detail-title");
    const overviewHeadingElement = document.querySelector("#detail-overview-heading");
    const featuresHeadingElement = document.querySelector("#detail-features-heading");
    const workflowHeadingElement = document.querySelector("#detail-workflow-heading");
    const shotsHeadingElement = document.querySelector("#detail-shots-heading");
    const shotTipElement = document.querySelector("#detail-shot-tip");
    const footerElement = document.querySelector("#detail-footer");

    const tagsElement = document.querySelector("#detail-tags");
    const overviewTextElement = document.querySelector("#detail-overview");
    const featuresListElement = document.querySelector("#detail-features");
    const workflowListElement = document.querySelector("#detail-workflow");
    const shotsElement = document.querySelector("#detail-shots");

    if (homeLinkElement) {
        homeLinkElement.textContent = copyText.homeLink;
    }

    if (overviewLinkElement) {
        overviewLinkElement.textContent = copyText.overviewLink;
    }

    if (subtitleElement) {
        subtitleElement.textContent = copyText.subtitle;
    }

    if (overviewHeadingElement) {
        overviewHeadingElement.textContent = copyText.sectionOverview;
    }

    if (featuresHeadingElement) {
        featuresHeadingElement.textContent = copyText.sectionFeatures;
    }

    if (workflowHeadingElement) {
        workflowHeadingElement.textContent = copyText.sectionWorkflow;
    }

    if (shotsHeadingElement) {
        shotsHeadingElement.textContent = copyText.sectionShots;
    }

    if (shotTipElement) {
        shotTipElement.textContent = copyText.shotTip;
    }

    if (footerElement) {
        footerElement.textContent = copyText.footer;
    }

    if (!tabItem) {
        if (titleElement) {
            titleElement.textContent = copyText.missingTitle;
        }

        if (overviewTextElement) {
            overviewTextElement.textContent = copyText.missingText;
        }

        if (tagsElement) {
            tagsElement.innerHTML = "";
        }

        if (featuresListElement) {
            featuresListElement.innerHTML = "";
        }

        if (workflowListElement) {
            workflowListElement.innerHTML = "";
        }

        if (shotsElement) {
            shotsElement.innerHTML = "";
        }

        return;
    }

    const localizedTitle = pickText(tabItem.title, languageCode);
    const localizedTags = pickList(tabItem.tags, languageCode);
    const localizedOverview = pickText(tabItem.overview, languageCode);
    const localizedHighlights = pickList(tabItem.highlights, languageCode);
    const localizedWorkflow = pickList(tabItem.workflow, languageCode);

    if (titleElement) {
        titleElement.textContent = localizedTitle + " " + copyText.detailTitleSuffix;
    }

    if (tagsElement) {
        tagsElement.innerHTML = localizedTags.map(function buildTag(tagText) {
            return '<span class="tag-item">' + tagText + '</span>';
        }).join("");
    }

    if (overviewTextElement) {
        overviewTextElement.textContent = localizedOverview;
    }

    if (featuresListElement) {
        featuresListElement.innerHTML = toListHtml(localizedHighlights);
    }

    if (workflowListElement) {
        workflowListElement.innerHTML = toListHtml(localizedWorkflow);
    }

    if (shotsElement) {
        shotsElement.innerHTML = toShotHtml(tabItem.screenshotPlan || [], languageCode);
    }
}

// registerDetailLanguageObserver 函数用途：
// - 注册语言切换监听并执行首次渲染。
function registerDetailLanguageObserver() {
    if (!window.kswordLanguage) {
        renderTabDetailPage("zh");
        return;
    }

    window.kswordLanguage.onLanguageChange(function onDetailLanguageChanged(languageCode) {
        renderTabDetailPage(languageCode);
    });
}

// DOMContentLoaded 监听用途：
// - 页面加载完成后挂载语言监听。
document.addEventListener("DOMContentLoaded", registerDetailLanguageObserver);
