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

    // ���ȣ�ʹ�� MultiByteToWideChar �� UTF-8 ת��Ϊ���ַ�
    int wide_length = MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), -1, nullptr, 0);
    std::vector<wchar_t> wide_str(wide_length);
    MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), -1, wide_str.data(), wide_length);

    // Ȼ��ʹ�� WideCharToMultiByte �����ַ�ת��Ϊ GBK
    int gbk_length = WideCharToMultiByte(CP_ACP, 0, wide_str.data(), -1, nullptr, 0, nullptr, nullptr);
    std::vector<char> gbk_str(gbk_length);
    WideCharToMultiByte(CP_ACP, 0, wide_str.data(), -1, gbk_str.data(), gbk_length, nullptr, nullptr);

    // ת�����ַ��������һ���ַ��� '\0'
    return std::string(gbk_str.begin(), gbk_str.end() - 1);
}

std::string extractValue(const std::string& jsonString, const std::string& key) {
    
    size_t startPos = 0;
    size_t endPos = 0;
    std::string value;

    // ���Ҽ�����ʼλ��
    std::string searchKey = "\"" + key + "\":";
    startPos = jsonString.find(searchKey);
    if (startPos == std::string::npos) {
        return ""; // ��������
    }

    // �ƶ�����ֵ�Ե�ֵ����ʼλ��
    startPos += searchKey.length();

    // �����ո�����еĻ���
    while (startPos < jsonString.length() && std::isspace(jsonString[startPos])) {
        ++startPos;
    }

    // ȷ��ֵ�����ͣ��ַ��������֣�
    if (jsonString[startPos] == '\"') { // �ַ���ֵ
        // ����ֵ����ʼ����
        startPos++; // ������ʼ����
        endPos = jsonString.find("\"", startPos);
        if (endPos == std::string::npos) {
            return ""; // ֵ�Ľ������Ų�����
        }
        // ��ȡֵ
        value = jsonString.substr(startPos, endPos - startPos);
    } else { // ��ֵ
        // ����ֵ�Ľ���λ�ã�ֱ�����������ֻ�ǵ��ַ���
        endPos = startPos;
        while (endPos < jsonString.length() && (std::isdigit(jsonString[endPos]) || jsonString[endPos] == '.')) {
            ++endPos;
        }
        // ��ȡֵ
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
    // ���Ҽ���
    startPos = jsonString.find(key);
    if (startPos != std::string::npos) {
        // �ƶ������������ð�źͿո�λ��
        startPos = jsonString.find(':', startPos) + 2;
        // ���ҵ�һ�������ַ�
        while (startPos < jsonString.length() && !isdigit(jsonString[startPos])) {
            ++startPos;
        }
        // ���ҽ����Ķ��Ż����������
        size_t endPos = startPos;
        while (endPos < jsonString.length() && (isdigit(jsonString[endPos]) || jsonString[endPos] == ',')) {
            ++endPos;
        }
        // ��ȡ�����ַ���
        numberStr = jsonString.substr(startPos, endPos - startPos);
        // �Ƴ����ܵĶ���
        if (!numberStr.empty() && numberStr.back() == ',') {
            numberStr.pop_back();
        }
    }
    return numberStr;
}

void KimiAPI(string userRequest, string API_KEY= "sk-CVtOiHqFxGgP9NkvKz4CE8jRGmYb5GCVA6uV4fZAYwjLgIHM") {
    std::string api_key = API_KEY; // �滻Ϊ���API��Կ
    //SetConsoleOutputCP(CP_UTF8);
    //RunCmdNow("chcp 65001");
    if (api_key == "")api_key = "sk-CVtOiHqFxGgP9NkvKz4CE8jRGmYb5GCVA6uV4fZAYwjLgIHM";
    string curlCmd="curl https://api.moonshot.cn/v1/chat/completions -H \"Content-Type: application/json\" -H \"Authorization: Bearer ";
    curlCmd += api_key;/*chcp 65001 && */
    curlCmd += "\" -d \"{\\\"model\\\": \\\"moonshot-v1-8k\\\", \\\"messages\\\": [ { \\\"role\\\": \\\"system\\\", \\\"content\\\": \\\"Answer my question in Chinese.\\\" }, { \\\"role\\\": \\\"user\\\", \\\"content\\\": \\\"";
    curlCmd += userRequest;
    curlCmd += "\\\" } ], \\\"temperature\\\": 0.3 }\" && chcp 936";
    //cout << "ƴ�ӵ����" << curlCmd << endl;
    //curlCmd = gbk_to_utf8(curlCmd);
    //system("pause");
    cout << "ƴ�ӵ����" << curlCmd << endl;
    string jsonString=GetCmdResult(curlCmd);

    KMesInfo("�Ự��ɣ����ڽ������ݣ�");
    jsonString = utf8_to_gbk(jsonString);
    size_t pos = 0;
    while ((pos = jsonString.find("\\n", pos)) != std::string::npos) {
        jsonString.replace(pos, 2, "\n");
        pos += 1; // ���� 1 �������²���Ļ��з�
    }
    if (extractContent(jsonString) == "") {
        cprint("ԭʼ����:", 1, 0); cout << jsonString << endl;
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
 * ��libjpeg-turboΪvs2010����ʱ��vs2015�¾�̬����libjpeg-turbo�����ӳ���:�Ҳ���__iob_func,
 * ����__iob_func��__acrt_iob_func��ת���������������,
 * ��libjpeg-turbo��vs2015����ʱ������Ҫ�˲����ļ�
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
//        // �����ڴ����ʧ�ܵ����
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
//        // ���������JSON����
//        std::string jsonData = R"({
//            "model": "moonshot-v1-8k",
//            "messages": [
//                {"role": "system", "content": "���� Kimi���� Moonshot AI �ṩ���˹��������֣�����ó����ĺ�Ӣ�ĵĶԻ������Ϊ�û��ṩ��ȫ���а�����׼ȷ�Ļش�ͬʱ�����ܾ�һ���漰�ֲ����壬�������ӣ���ɫ����������Ļش�Moonshot AI Ϊר�����ʣ����ɷ�����������ԡ�"},
//                {"role": "user", "content": ")" + post + R"("}
//            ],
//            "temperature": 0.3
//        })";
//
//        // ���������URL
//        curl_easy_setopt(curl, CURLOPT_URL, "https://api.moonshot.cn/v1/chat/completions");
//
//        // ����HTTPͷ
//        struct curl_slist* headers = NULL;
//        headers = curl_slist_append(headers, "Content-Type: application/json");
//        std::string authHeader = "Authorization: Bearer " + API_KEY;
//        headers = curl_slist_append(headers, authHeader.c_str());
//        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
//
//        // ����POST����
//        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonData.c_str());
//
//        // ���ûص����������ڽ�����Ӧ
//        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
//        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
//
//        // ִ������
//        res = curl_easy_perform(curl);
//
//        // ��������Ƿ�ɹ�
//        if (res != CURLE_OK) {
//            std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
//        }
//        else {
//            std::cout << "Response from Kimi:\n" << readBuffer << std::endl;
//        }
//
//        // ����
//        curl_slist_free_all(headers);
//        curl_easy_cleanup(curl);
//    }
//
//    curl_global_cleanup();
//}
#endif