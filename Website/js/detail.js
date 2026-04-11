// =============================
// detail.js
// 作用：渲染模块详情页，并将截图区改为整屏翻页分镜。
// =============================

// detailCopyMap 对象用途：
// - 保存详情页固定文案中英文版本；
// - 统一管理标题、区块名、缺失态和页脚文案。
const detailCopyMap = {
    zh: {
        homeLink: "↩ 首页",
        overviewLink: "返回总览",
        subtitle: "模块详细能力说明",
        sectionOverview: "模块定位",
        sectionFeatures: "核心功能",
        sectionWorkflow: "推荐使用流程",
        sectionShots: "功能分镜",
        shotTip: "每屏只展示一个功能点，向下滚动即可翻到下一张分镜。",
        footer: "截图规范文档：Website/Images/截图命名与拍摄说明.txt",
        detailTitleSuffix: "模块发布页",
        heroFallback: "主展示图占位",
        galleryFallback: "展示图占位",
        missingTitle: "未找到对应模块",
        missingSummary: "请返回总览页重新选择模块。",
        missingText: "当前 slug 未在 tabs-data.js 中找到匹配项。",
        sectionPrefix: "功能"
    },
    en: {
        homeLink: "↩ Home",
        overviewLink: "Back to Overview",
        subtitle: "Detailed module capabilities",
        sectionOverview: "Module Positioning",
        sectionFeatures: "Core Capabilities",
        sectionWorkflow: "Recommended Workflow",
        sectionShots: "Feature Slides",
        shotTip: "One function per screen. Scroll down to flip to the next slide.",
        footer: "Guide: Website/Images/截图命名与拍摄说明.txt",
        detailTitleSuffix: "Launch Page",
        heroFallback: "Hero visual placeholder",
        galleryFallback: "Visual placeholder",
        missingTitle: "Module Not Found",
        missingSummary: "Please return to Overview and select a module again.",
        missingText: "The current slug has no matching record in tabs-data.js.",
        sectionPrefix: "Feature"
    }
};

// detailSnapObserver 变量用途：
// - 监听详情页当前可见分镜；
// - 同步右侧分页点状态。
let detailSnapObserver = null;

// toListHtml 函数用途：
// - 将字符串数组渲染为 HTML 列表项。
// - 传入：itemList(字符串数组)。
// - 返回：<li> 片段字符串。
function toListHtml(itemList) {
    if (!Array.isArray(itemList) || itemList.length === 0) {
        return "";
    }
    return itemList.map(function buildListItem(itemText) {
        return "<li>" + itemText + "</li>";
    }).join("");
}

// buildHeroShotHtml 函数用途：
// - 渲染详情页首屏主图区域（右侧大图）。
// - 传入：shotItem(截图计划项), languageCode(语言), fallbackText(占位文案)。
// - 返回：主图 HTML。
function buildHeroShotHtml(shotItem, languageCode, fallbackText) {
    const pickText = window.kswordLanguage ? window.kswordLanguage.pickText : function fallbackPicker(value) { return value || ""; };

    if (!shotItem) {
        return [
            '<div class="hero-shot-wrap">',
            '  <div class="hero-shot-fallback" style="display:flex;">',
            '    <div class="hero-shot-file">No Hero Image</div>',
            '    <div class="hero-shot-goal">' + fallbackText + '</div>',
            '  </div>',
            '</div>'
        ].join("");
    }

    const shotGoalText = pickText(shotItem.goal, languageCode);
    const imagePath = "../Images/" + shotItem.file;

    return [
        '<div class="hero-shot-wrap">',
        '  <img class="hero-shot-image" src="' + imagePath + '" alt="' + shotGoalText + '" onerror="this.style.display=\'none\';this.nextElementSibling.style.display=\'flex\';">',
        '  <div class="hero-shot-fallback">',
        '    <div class="hero-shot-file">' + shotItem.file + '</div>',
        '    <div class="hero-shot-goal">' + fallbackText + '</div>',
        '  </div>',
        '  <div class="hero-shot-caption">' + shotGoalText + '</div>',
        '</div>'
    ].join("");
}

// buildGalleryShotHtml 函数用途：
// - 渲染详情页单个“功能分镜页”。
// - 传入：shotItem(截图计划项), languageCode(语言), fallbackText(占位文案), orderText(序号), sectionTitle(标题文案)。
// - 返回：分镜 HTML。
function buildGalleryShotHtml(shotItem, languageCode, fallbackText, orderText, sectionTitle) {
    const pickText = window.kswordLanguage ? window.kswordLanguage.pickText : function fallbackPicker(value) { return value || ""; };
    const shotGoalText = pickText(shotItem.goal, languageCode);
    const imagePath = "../Images/" + shotItem.file;

    return [
        '<article class="detail-shot-card detail-snap">',
        '  <section class="detail-shot-copy">',
        '    <div class="detail-shot-order">' + orderText + '</div>',
        '    <h3 class="detail-shot-title">' + sectionTitle + '</h3>',
        '    <p class="detail-shot-summary">' + shotGoalText + '</p>',
        '    <div class="detail-shot-file-name">' + shotItem.file + '</div>',
        '  </section>',
        '  <div class="detail-shot-media">',
        '    <img class="detail-shot-image" src="' + imagePath + '" alt="' + shotGoalText + '" onerror="this.style.display=\'none\';this.nextElementSibling.style.display=\'flex\';">',
        '    <div class="detail-shot-fallback">',
        '      <div class="detail-shot-file">' + shotItem.file + '</div>',
        '      <div>' + fallbackText + '</div>',
        '    </div>',
        '  </div>',
        '</article>'
    ].join("");
}

// ensureDetailPagerElement 函数用途：
// - 确保详情页右侧分页点容器存在。
function ensureDetailPagerElement() {
    let pagerElement = document.querySelector("#detail-pager");
    if (!pagerElement) {
        pagerElement = document.createElement("nav");
        pagerElement.id = "detail-pager";
        pagerElement.className = "detail-pager";
        pagerElement.setAttribute("aria-label", "Detail slide navigation");
        document.body.appendChild(pagerElement);
    }
    return pagerElement;
}

// updateDetailPagerActive 函数用途：
// - 更新分页点高亮状态。
// - 传入：activeIndex(0 基序号)。
function updateDetailPagerActive(activeIndex) {
    const dotButtonList = document.querySelectorAll("#detail-pager .detail-pager-dot");
    dotButtonList.forEach(function updateDotState(dotButton, dotIndex) {
        const isActive = dotIndex === activeIndex;
        dotButton.classList.toggle("is-active", isActive);
        dotButton.setAttribute("aria-current", isActive ? "true" : "false");
    });
}

// markDetailSnapSections 函数用途：
// - 收集详情页中的整屏分镜节点。
function markDetailSnapSections() {
    const sectionList = [];

    const heroElement = document.querySelector(".detail-page .detail-hero");
    if (heroElement) {
        heroElement.classList.add("detail-snap");
        sectionList.push(heroElement);
    }

    const mainGridElement = document.querySelector(".detail-page .detail-main-grid");
    if (mainGridElement) {
        mainGridElement.classList.add("detail-snap");
        sectionList.push(mainGridElement);
    }

    const shotCardList = Array.from(document.querySelectorAll(".detail-page .detail-shot-card"));
    shotCardList.forEach(function appendShotCard(shotCardElement) {
        shotCardElement.classList.add("detail-snap");
        sectionList.push(shotCardElement);
    });

    sectionList.forEach(function assignSectionIndex(sectionElement, sectionIndex) {
        sectionElement.setAttribute("data-detail-slide", String(sectionIndex));
    });

    return sectionList;
}

// bindDetailSnapPager 函数用途：
// - 生成分页点；
// - 绑定点击跳转与可见分镜高亮。
function bindDetailSnapPager() {
    if (detailSnapObserver) {
        detailSnapObserver.disconnect();
        detailSnapObserver = null;
    }

    const snapSectionList = markDetailSnapSections();
    const pagerElement = ensureDetailPagerElement();

    if (snapSectionList.length <= 1) {
        pagerElement.innerHTML = "";
        pagerElement.style.display = "none";
        return;
    }

    pagerElement.style.display = "flex";
    pagerElement.innerHTML = snapSectionList.map(function buildDot(_, sectionIndex) {
        const sectionText = String(sectionIndex + 1);
        return '<button class="detail-pager-dot" type="button" data-detail-target="' + String(sectionIndex) + '" aria-label="Jump to slide ' + sectionText + '"></button>';
    }).join("");

    const dotButtonList = pagerElement.querySelectorAll(".detail-pager-dot");
    dotButtonList.forEach(function bindJumpEvent(dotButton) {
        dotButton.addEventListener("click", function onPagerDotClicked() {
            const targetIndex = Number(dotButton.getAttribute("data-detail-target"));
            const targetSection = snapSectionList[targetIndex];
            if (targetSection) {
                targetSection.scrollIntoView({ behavior: "smooth", block: "start" });
            }
        });
    });

    updateDetailPagerActive(0);

    if (typeof window.IntersectionObserver !== "function") {
        return;
    }

    detailSnapObserver = new window.IntersectionObserver(function onVisibilityChanged(entryList) {
        entryList.forEach(function updateCurrentSlide(entryItem) {
            if (!entryItem.isIntersecting) {
                return;
            }
            const targetIndex = Number(entryItem.target.getAttribute("data-detail-slide"));
            if (!Number.isNaN(targetIndex)) {
                updateDetailPagerActive(targetIndex);
            }
        });
    }, {
        threshold: 0.55
    });

    snapSectionList.forEach(function observeSection(sectionElement) {
        detailSnapObserver.observe(sectionElement);
    });
}

// renderMissingState 函数用途：
// - 在找不到模块数据时回填页面占位内容。
// - 传入：copyText(当前语言文案)。
function renderMissingState(copyText) {
    const titleElement = document.querySelector("#detail-title");
    const summaryElement = document.querySelector("#detail-summary");
    const overviewElement = document.querySelector("#detail-overview");
    const featuresElement = document.querySelector("#detail-features");
    const workflowElement = document.querySelector("#detail-workflow");
    const heroShotElement = document.querySelector("#detail-hero-shot");
    const shotsElement = document.querySelector("#detail-shots");
    const tagsElement = document.querySelector("#detail-tags");

    if (titleElement) {
        titleElement.textContent = copyText.missingTitle;
    }
    if (summaryElement) {
        summaryElement.textContent = copyText.missingSummary;
    }
    if (overviewElement) {
        overviewElement.textContent = copyText.missingText;
    }
    if (featuresElement) {
        featuresElement.innerHTML = "";
    }
    if (workflowElement) {
        workflowElement.innerHTML = "";
    }
    if (tagsElement) {
        tagsElement.innerHTML = "";
    }
    if (heroShotElement) {
        heroShotElement.innerHTML = buildHeroShotHtml(null, "zh", copyText.heroFallback);
    }
    if (shotsElement) {
        shotsElement.innerHTML = "";
    }

    bindDetailSnapPager();
}

// renderTabDetailPage 函数用途：
// - 按当前语言和 slug 渲染详情页全部内容。
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
    const summaryElement = document.querySelector("#detail-summary");
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
    const heroShotElement = document.querySelector("#detail-hero-shot");
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
        renderMissingState(copyText);
        return;
    }

    const localizedTitle = pickText(tabItem.title, languageCode);
    const localizedSummary = pickText(tabItem.summary, languageCode);
    const localizedTags = pickList(tabItem.tags, languageCode);
    const localizedOverview = pickText(tabItem.overview, languageCode);
    const localizedHighlights = pickList(tabItem.highlights, languageCode);
    const localizedWorkflow = pickList(tabItem.workflow, languageCode);
    const shotList = Array.isArray(tabItem.screenshotPlan) ? tabItem.screenshotPlan : [];

    if (titleElement) {
        titleElement.textContent = localizedTitle + " " + copyText.detailTitleSuffix;
    }
    if (summaryElement) {
        summaryElement.textContent = localizedSummary;
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

    // 首屏主图：优先使用截图计划第一张图。
    if (heroShotElement) {
        const heroShot = shotList.length > 0 ? shotList[0] : null;
        heroShotElement.innerHTML = buildHeroShotHtml(heroShot, languageCode, copyText.heroFallback);
    }

    // 分镜区：每张截图一屏。
    if (shotsElement) {
        if (shotList.length === 0) {
            shotsElement.innerHTML = "";
        } else {
            shotsElement.innerHTML = shotList.map(function buildShotCard(shotItem, shotIndex) {
                const orderText = copyText.sectionPrefix + " " + String(shotIndex + 1).padStart(2, "0");
                const sectionTitle = pickText(shotItem.goal, languageCode);
                return buildGalleryShotHtml(shotItem, languageCode, copyText.galleryFallback, orderText, sectionTitle);
            }).join("");
        }
    }

    bindDetailSnapPager();
}

// registerDetailLanguageObserver 函数用途：
// - 注册语言变化监听并触发首次渲染。
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
// - 页面节点加载完毕后启动详情渲染。
document.addEventListener("DOMContentLoaded", registerDetailLanguageObserver);

