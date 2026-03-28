// =============================
// landing.js
// 作用：渲染品牌首屏文案并响应中英文切换。
// =============================

// landingCopyMap 对象用途：
// - 保存首屏页面全部可切换的中英文文本。
const landingCopyMap = {
    zh: {
        overviewLink: "进入详情",
        slogan: "全国最强的Windows 内核调试工具",
        subtitle: "面向系统排障、内核分析与安全研究的一体化调试平台。",
        ctaPrimary: "查看详细介绍",
        ctaSecondary: "查看截图规范",
        scrollTip: "详细功能内容在第二页（Overview）展示"
    },
    en: {
        overviewLink: "Overview",
        slogan: "China's Most Powerful Windows Kernel Debugging Suite",
        subtitle: "An integrated platform for system troubleshooting, kernel analysis, and security research.",
        ctaPrimary: "Explore Modules",
        ctaSecondary: "Screenshot Guide",
        scrollTip: "Detailed information starts from page 2 (Overview)."
    }
};

// renderLandingCopy 函数用途：
// - 根据当前语言刷新首屏文案文本。
// - 传入：languageCode("zh" 或 "en")。
function renderLandingCopy(languageCode) {
    const copyText = landingCopyMap[languageCode] || landingCopyMap.zh;

    const overviewLinkElement = document.querySelector("#landing-overview-link");
    const sloganElement = document.querySelector("#landing-slogan");
    const subtitleElement = document.querySelector("#landing-subtitle");
    const ctaPrimaryElement = document.querySelector("#landing-cta-primary");
    const ctaSecondaryElement = document.querySelector("#landing-cta-secondary");
    const scrollTipElement = document.querySelector("#landing-scroll-tip");

    if (overviewLinkElement) {
        overviewLinkElement.textContent = copyText.overviewLink;
    }

    if (sloganElement) {
        sloganElement.textContent = copyText.slogan;
    }

    if (subtitleElement) {
        subtitleElement.textContent = copyText.subtitle;
    }

    if (ctaPrimaryElement) {
        ctaPrimaryElement.textContent = copyText.ctaPrimary;
    }

    if (ctaSecondaryElement) {
        ctaSecondaryElement.textContent = copyText.ctaSecondary;
    }

    if (scrollTipElement) {
        scrollTipElement.textContent = copyText.scrollTip;
    }
}

// registerLandingLanguageObserver 函数用途：
// - 监听语言系统变化并在初始化时刷新文案。
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
// - 确保页面节点存在后再绑定语言渲染逻辑。
document.addEventListener("DOMContentLoaded", registerLandingLanguageObserver);
