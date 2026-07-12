// =============================
// lang.js
// 作用：实现全站中英文切换、状态持久化与页面订阅通知。
// =============================

// LANGUAGE_STORAGE_KEY 常量用途：
// - 记录用户最后一次选择的语言。
const LANGUAGE_STORAGE_KEY = "ksword-site-language";

// SUPPORTED_LANGUAGE_LIST 常量用途：
// - 限制可选语言范围，避免异常字符串写入。
const SUPPORTED_LANGUAGE_LIST = ["zh", "en"];

// languageListeners 数组用途：
// - 保存语言变更后的回调函数队列。
const languageListeners = [];

// currentLanguage 变量用途：
// - 缓存当前语言，供所有页面统一读取。
let currentLanguage = "zh";

// hasLanguageInitialized 标记用途：
// - 防止重复初始化导致重复绑定事件。
let hasLanguageInitialized = false;

// getSystemDefaultLanguage 函数用途：
// - 根据浏览器语言推断默认语言。
// - 返回值："zh" 或 "en"。
function getSystemDefaultLanguage() {
    const browserLanguage = (navigator.language || "zh").toLowerCase();
    return browserLanguage.startsWith("zh") ? "zh" : "en";
}

// loadInitialLanguage 函数用途：
// - 优先读取本地缓存语言；
// - 若缓存无效则回退到系统默认语言。
function loadInitialLanguage() {
    const storedLanguage = window.localStorage.getItem(LANGUAGE_STORAGE_KEY);
    if (SUPPORTED_LANGUAGE_LIST.includes(storedLanguage)) {
        return storedLanguage;
    }
    return getSystemDefaultLanguage();
}

// pickLocalizedText 函数用途：
// - 从多语言对象中按指定语言取值；
// - 支持字符串直出与对象字段回退。
// - 传入：localizedValue(字符串或 {zh,en})、languageCode。
// - 返回：可显示的字符串。
function pickLocalizedText(localizedValue, languageCode) {
    if (typeof localizedValue === "string") {
        return localizedValue;
    }

    if (!localizedValue || typeof localizedValue !== "object") {
        return "";
    }

    if (localizedValue[languageCode]) {
        return localizedValue[languageCode];
    }

    return localizedValue.zh || localizedValue.en || "";
}

// pickLocalizedList 函数用途：
// - 从多语言数组对象中按语言取数组。
// - 传入：localizedList(数组或 {zh:[],en:[]})、languageCode。
// - 返回：字符串数组。
function pickLocalizedList(localizedList, languageCode) {
    if (Array.isArray(localizedList)) {
        return localizedList;
    }

    if (!localizedList || typeof localizedList !== "object") {
        return [];
    }

    const matchedList = localizedList[languageCode];
    if (Array.isArray(matchedList)) {
        return matchedList;
    }

    if (Array.isArray(localizedList.zh)) {
        return localizedList.zh;
    }

    if (Array.isArray(localizedList.en)) {
        return localizedList.en;
    }

    return [];
}

// refreshLanguageToggleButton 函数用途：
// - 更新页面上语言切换按钮文本和提示。
function refreshLanguageToggleButton() {
    const toggleButtonList = document.querySelectorAll("[data-lang-toggle]");
    const nextLanguage = currentLanguage === "zh" ? "en" : "zh";
    const buttonTitle = currentLanguage === "zh" ? "Switch to English" : "切换为中文";

    toggleButtonList.forEach(function updateButton(toggleButton) {
        toggleButton.textContent = nextLanguage.toUpperCase() === "EN" ? "EN" : "中";
        toggleButton.setAttribute("title", buttonTitle);
        toggleButton.setAttribute("aria-label", buttonTitle);
    });
}

// notifyLanguageChanged 函数用途：
// - 触发所有订阅回调，让页面重新渲染文本。
function notifyLanguageChanged() {
    languageListeners.forEach(function callListener(listenerFn) {
        listenerFn(currentLanguage);
    });
}

// applyLanguage 函数用途：
// - 统一修改当前语言并执行持久化与广播。
// - 传入：targetLanguage("zh" 或 "en")。
function applyLanguage(targetLanguage) {
    if (!SUPPORTED_LANGUAGE_LIST.includes(targetLanguage)) {
        return;
    }

    currentLanguage = targetLanguage;
    document.documentElement.setAttribute("lang", targetLanguage === "zh" ? "zh-CN" : "en");
    window.localStorage.setItem(LANGUAGE_STORAGE_KEY, targetLanguage);
    refreshLanguageToggleButton();
    notifyLanguageChanged();
}

// toggleLanguage 函数用途：
// - 切换语言按钮点击后的执行入口。
function toggleLanguage() {
    const nextLanguage = currentLanguage === "zh" ? "en" : "zh";
    applyLanguage(nextLanguage);
}

// initializeLanguageSystem 函数用途：
// - 初始化语言状态并绑定按钮事件。
function initializeLanguageSystem() {
    if (hasLanguageInitialized) {
        return;
    }

    hasLanguageInitialized = true;
    currentLanguage = loadInitialLanguage();

    const toggleButtonList = document.querySelectorAll("[data-lang-toggle]");
    toggleButtonList.forEach(function bindToggleEvent(toggleButton) {
        toggleButton.addEventListener("click", toggleLanguage);
    });

    refreshLanguageToggleButton();
    notifyLanguageChanged();
}

// 暴露给其它脚本的全局 API：
// - getLanguage：读取当前语言；
// - setLanguage：主动设置语言；
// - onLanguageChange：注册语言变更监听。
window.kswordLanguage = {
    getLanguage: function getLanguage() {
        return currentLanguage;
    },
    setLanguage: function setLanguage(languageCode) {
        applyLanguage(languageCode);
    },
    onLanguageChange: function onLanguageChange(listenerFn) {
        if (typeof listenerFn !== "function") {
            return;
        }

        languageListeners.push(listenerFn);
        if (hasLanguageInitialized) {
            listenerFn(currentLanguage);
        }
    },
    pickText: pickLocalizedText,
    pickList: pickLocalizedList
};

// DOMContentLoaded 监听用途：
// - 确保节点加载完成后初始化语言系统。
document.addEventListener("DOMContentLoaded", initializeLanguageSystem);
