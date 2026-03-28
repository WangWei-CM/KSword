// =============================
// theme.js
// 作用：提供全站深浅模式切换并兼容中英文提示文案。
// =============================

// THEME_STORAGE_KEY 常量用途：
// - 持久化用户最后一次主题选择。
const THEME_STORAGE_KEY = "ksword-site-theme";

// currentThemeName 变量用途：
// - 缓存当前主题状态，便于语言切换时复用。
let currentThemeName = "light";

// getInitialTheme 函数用途：
// - 读取本地缓存主题，若不存在则根据系统偏好回退。
function getInitialTheme() {
    const savedTheme = window.localStorage.getItem(THEME_STORAGE_KEY);
    if (savedTheme === "light" || savedTheme === "dark") {
        return savedTheme;
    }

    const prefersDark = window.matchMedia && window.matchMedia("(prefers-color-scheme: dark)").matches;
    return prefersDark ? "dark" : "light";
}

// getCurrentLanguageForThemeHint 函数用途：
// - 获取当前语言，用于生成主题按钮 tooltip 文案。
function getCurrentLanguageForThemeHint() {
    if (window.kswordLanguage && typeof window.kswordLanguage.getLanguage === "function") {
        return window.kswordLanguage.getLanguage();
    }
    return "zh";
}

// buildThemeButtonHint 函数用途：
// - 按语言和目标主题返回按钮提示文本。
// - 传入：isDarkMode(当前是否深色)。
// - 返回：提示文案字符串。
function buildThemeButtonHint(isDarkMode) {
    const languageCode = getCurrentLanguageForThemeHint();
    if (languageCode === "en") {
        return isDarkMode ? "Switch to Light Mode" : "Switch to Dark Mode";
    }
    return isDarkMode ? "切换到浅色模式" : "切换到深色模式";
}

// applyThemeToDocument 函数用途：
// - 写入根节点 data-theme 属性触发 CSS 变量切换。
function applyThemeToDocument(themeName) {
    currentThemeName = themeName;
    document.documentElement.setAttribute("data-theme", themeName);
}

// updateThemeToggleButton 函数用途：
// - 刷新图标按钮字符与提示文案。
function updateThemeToggleButton(themeName) {
    const toggleButtonList = document.querySelectorAll("[data-theme-toggle]");
    const isDarkMode = themeName === "dark";
    const hintText = buildThemeButtonHint(isDarkMode);

    toggleButtonList.forEach(function setButtonState(toggleButton) {
        toggleButton.textContent = isDarkMode ? "☀" : "☾";
        toggleButton.setAttribute("title", hintText);
        toggleButton.setAttribute("aria-label", hintText);
    });
}

// switchTheme 函数用途：
// - 在浅色与深色之间切换并持久化。
function switchTheme() {
    const nextTheme = currentThemeName === "dark" ? "light" : "dark";
    applyThemeToDocument(nextTheme);
    updateThemeToggleButton(nextTheme);
    window.localStorage.setItem(THEME_STORAGE_KEY, nextTheme);
}

// bindLanguageAwareThemeHint 函数用途：
// - 监听语言变化并刷新主题按钮提示文案。
function bindLanguageAwareThemeHint() {
    if (!window.kswordLanguage || typeof window.kswordLanguage.onLanguageChange !== "function") {
        return;
    }

    window.kswordLanguage.onLanguageChange(function onLanguageChanged() {
        updateThemeToggleButton(currentThemeName);
    });
}

// initializeTheme 函数用途：
// - 初始化主题、绑定切换按钮并挂载语言监听。
function initializeTheme() {
    const initialTheme = getInitialTheme();
    applyThemeToDocument(initialTheme);
    updateThemeToggleButton(initialTheme);

    const toggleButtonList = document.querySelectorAll("[data-theme-toggle]");
    toggleButtonList.forEach(function bindToggleEvent(toggleButton) {
        toggleButton.addEventListener("click", switchTheme);
    });

    bindLanguageAwareThemeHint();
}

// DOMContentLoaded 监听用途：
// - DOM 就绪后执行主题初始化。
document.addEventListener("DOMContentLoaded", initializeTheme);
