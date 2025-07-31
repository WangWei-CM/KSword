#ifdef KSWORD_WITH_COMMAND
#include "..\Main\KswordTotalHead.h"

using namespace std;

std::string gbk_to_utf8(const std::string& gbk_str) {
    int utf8_len = MultiByteToWideChar(CP_ACP, 0, gbk_str.c_str(), -1, nullptr, 0);
    std::wstring utf8_wstr(utf8_len, 0);
    MultiByteToWideChar(CP_ACP, 0, gbk_str.c_str(), -1, &utf8_wstr[0], utf8_len);

    int utf8_bytes_len = WideCharToMultiByte(CP_UTF8, 0, &utf8_wstr[0], -1, nullptr, 0, nullptr, nullptr);
    std::string utf8_str(utf8_bytes_len, 0);
    WideCharToMultiByte(CP_UTF8, 0, &utf8_wstr[0], -1, &utf8_str[0], utf8_bytes_len, nullptr, nullptr);

    return utf8_str;
}
std::string utf8_to_gbk(const std::string& utf8_str) {
    if (utf8_str.empty()) {
        return std::string();
    }

    // 首先，使用 MultiByteToWideChar 将 UTF-8 转换为宽字符
    int wide_length = MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), -1, nullptr, 0);
    std::vector<wchar_t> wide_str(wide_length);
    MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), -1, wide_str.data(), wide_length);

    // 然后，使用 WideCharToMultiByte 将宽字符转换为 GBK
    int gbk_length = WideCharToMultiByte(CP_ACP, 0, wide_str.data(), -1, nullptr, 0, nullptr, nullptr);
    std::vector<char> gbk_str(gbk_length);
    WideCharToMultiByte(CP_ACP, 0, wide_str.data(), -1, gbk_str.data(), gbk_length, nullptr, nullptr);

    // 转换后字符串的最后一个字符是 '\0'
    return std::string(gbk_str.begin(), gbk_str.end() - 1);
}

std::string extractValue(const std::string& jsonString, const std::string& key) {
    
    size_t startPos = 0;
    size_t endPos = 0;
    std::string value;

    // 查找键的起始位置
    std::string searchKey = "\"" + key + "\":";
    startPos = jsonString.find(searchKey);
    if (startPos == std::string::npos) {
        return ""; // 键不存在
    }

    // 移动到键值对的值的起始位置
    startPos += searchKey.length();

    // 跳过空格（如果有的话）
    while (startPos < jsonString.length() && std::isspace(jsonString[startPos])) {
        ++startPos;
    }

    // 确定值的类型（字符串或数字）
    if (jsonString[startPos] == '\"') { // 字符串值
        // 查找值的起始引号
        startPos++; // 跳过起始引号
        endPos = jsonString.find("\"", startPos);
        if (endPos == std::string::npos) {
            return ""; // 值的结束引号不存在
        }
        // 提取值
        value = jsonString.substr(startPos, endPos - startPos);
    } else { // 数值
        // 查找值的结束位置（直到遇到非数字或非点字符）
        endPos = startPos;
        while (endPos < jsonString.length() && (std::isdigit(jsonString[endPos]) || jsonString[endPos] == '.')) {
            ++endPos;
        }
        // 提取值
        value = jsonString.substr(startPos, endPos - startPos);
    }

    return value;
}

std::string extractContent(const std::string& jsonString) {
    std::regex pattern(R"re(\"content\":\s*\"([^\"]+)\")re");
    std::smatch match;
    if (std::regex_search(jsonString, match, pattern) && match.size() > 1) {
        return match[1];
    }
    return "";
}
std::string extractTokens(const std::string& jsonString, const std::string& key) {
    size_t startPos = 0;
    std::string numberStr;
    // 查找键名
    startPos = jsonString.find(key);
    if (startPos != std::string::npos) {
        // 移动到键名后面的冒号和空格位置
        startPos = jsonString.find(':', startPos) + 2;
        // 查找第一个数字字符
        while (startPos < jsonString.length() && !isdigit(jsonString[startPos])) {
            ++startPos;
        }
        // 查找结束的逗号或结束大括号
        size_t endPos = startPos;
        while (endPos < jsonString.length() && (isdigit(jsonString[endPos]) || jsonString[endPos] == ',')) {
            ++endPos;
        }
        // 提取数字字符串
        numberStr = jsonString.substr(startPos, endPos - startPos);
        // 移除可能的逗号
        if (!numberStr.empty() && numberStr.back() == ',') {
            numberStr.pop_back();
        }
    }
    return numberStr;
}

void KimiAPI(string userRequest, string API_KEY= "sk-CVtOiHqFxGgP9NkvKz4CE8jRGmYb5GCVA6uV4fZAYwjLgIHM") {
    std::string api_key = API_KEY; // 替换为你的API密钥
    //SetConsoleOutputCP(CP_UTF8);
    //RunCmdNow("chcp 65001");
    if (api_key == "")api_key = "sk-CVtOiHqFxGgP9NkvKz4CE8jRGmYb5GCVA6uV4fZAYwjLgIHM";
    string curlCmd="curl https://api.moonshot.cn/v1/chat/completions -H \"Content-Type: application/json\" -H \"Authorization: Bearer ";
    curlCmd += api_key;/*chcp 65001 && */
    curlCmd += "\" -d \"{\\\"model\\\": \\\"moonshot-v1-8k\\\", \\\"messages\\\": [ { \\\"role\\\": \\\"system\\\", \\\"content\\\": \\\"Answer my question in Chinese.\\\" }, { \\\"role\\\": \\\"user\\\", \\\"content\\\": \\\"";
    curlCmd += userRequest;
    curlCmd += "\\\" } ], \\\"temperature\\\": 0.3 }\" && chcp 936";
    //cout << "拼接的命令：" << curlCmd << endl;
    //curlCmd = gbk_to_utf8(curlCmd);
    //system("pause");
    cout << "拼接的命令：" << curlCmd << endl;
    string jsonString=GetCmdResult(curlCmd);

    KMesInfo("会话完成，正在解析数据：");
    jsonString = utf8_to_gbk(jsonString);
    size_t pos = 0;
    while ((pos = jsonString.find("\\n", pos)) != std::string::npos) {
        jsonString.replace(pos, 2, "\n");
        pos += 1; // 增加 1 以跳过新插入的换行符
    }
    if (extractContent(jsonString) == "") {
        cprint("原始数据:", 1, 0); cout << jsonString << endl;
    }
    else {
        cprint("ID: \t", 2, 0); cout << extractValue(jsonString, "id") << std::endl;
        cprint("Object: \t", 2, 0); cout << extractValue(jsonString, "object") << std::endl;
        cprint("Created: \t", 2, 0); cout << extractValue(jsonString, "created") << std::endl;
        cprint("Model: \t", 2, 0); cout << extractValue(jsonString, "model") << std::endl;
        cprint("Assistant Role: \t", 2, 0); cout << extractValue(jsonString, "role") << std::endl;
        cprint("Content: \t", 2, 0); cout << extractContent(jsonString) << std::endl;
        cprint("Prompt Tokens: \t", 2, 0); cout << extractTokens(jsonString, "prompt_tokens") << std::endl;
        cprint("Completion Tokens: \t", 2, 0); cout << extractTokens(jsonString, "completion_tokens") << std::endl;
        cprint("Total Tokens: \t", 2, 0); cout << extractTokens(jsonString, "total_tokens") << std::endl;
    }
    

}
























//#ifndef _ACRTIMP
//#if defined _CRTIMP && !defined _VCRT_DEFINED_CRTIMP
//#define _ACRTIMP _CRTIMP
//#elif !defined _CORECRT_BUILD && defined _DLL
//#define _ACRTIMP __declspec(dllimport)
//#else
//#define _ACRTIMP
//#endif
//#endif
/*
 * 当libjpeg-turbo为vs2010编译时，vs2015下静态链接libjpeg-turbo会链接出错:找不到__iob_func,
 * 增加__iob_func到__acrt_iob_func的转换函数解决此问题,
 * 当libjpeg-turbo用vs2015编译时，不需要此补丁文件
 */
//#if _MSC_VER>=1900
//#include "stdio.h" 
//_ACRTIMP_ALT FILE* __cdecl __acrt_iob_func(unsigned);
//#ifdef __cplusplus 
//extern "C"
//#endif 
//FILE* __cdecl __iob_func(unsigned i) {
//    return __acrt_iob_func(i);
//}
//#endif /* _MSC_VER>=1900 */
//
//#pragma comment(lib, "legacy_stdio_definitions.lib")
////#include <curl\curl.h>
//
//size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* s) {
//    size_t newLength = size * nmemb;
//    try {
//        s->append((char*)contents, newLength);
//    }
//    catch (std::bad_alloc& e) {
//        // 处理内存分配失败的情况
//        return 0;
//    }
//    return newLength;
//}
//void KimiAPI(string post,string API_KEY="sk-HGygN7CQTgZKnQYG9LVoidMM5c017CQoKSXiL2Y8wGV89gt6") {
//    CURL* curl;
//    CURLcode res;
//    std::string readBuffer;
//
//    curl_global_init(CURL_GLOBAL_DEFAULT);
//    curl = curl_easy_init();
//
//    if (curl) {
//        // 构造请求的JSON数据
//        std::string jsonData = R"({
//            "model": "moonshot-v1-8k",
//            "messages": [
//                {"role": "system", "content": "你是 Kimi，由 Moonshot AI 提供的人工智能助手，你更擅长中文和英文的对话。你会为用户提供安全，有帮助，准确的回答。同时，你会拒绝一切涉及恐怖主义，种族歧视，黄色暴力等问题的回答。Moonshot AI 为专有名词，不可翻译成其他语言。"},
//                {"role": "user", "content": ")" + post + R"("}
//            ],
//            "temperature": 0.3
//        })";
//
//        // 设置请求的URL
//        curl_easy_setopt(curl, CURLOPT_URL, "https://api.moonshot.cn/v1/chat/completions");
//
//        // 设置HTTP头
//        struct curl_slist* headers = NULL;
//        headers = curl_slist_append(headers, "Content-Type: application/json");
//        std::string authHeader = "Authorization: Bearer " + API_KEY;
//        headers = curl_slist_append(headers, authHeader.c_str());
//        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
//
//        // 设置POST数据
//        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonData.c_str());
//
//        // 设置回调函数，用于接收响应
//        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
//        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
//
//        // 执行请求
//        res = curl_easy_perform(curl);
//
//        // 检查请求是否成功
//        if (res != CURLE_OK) {
//            std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
//        }
//        else {
//            std::cout << "Response from Kimi:\n" << readBuffer << std::endl;
//        }
//
//        // 清理
//        curl_slist_free_all(headers);
//        curl_easy_cleanup(curl);
//    }
//
//    curl_global_cleanup();
//}
#endif