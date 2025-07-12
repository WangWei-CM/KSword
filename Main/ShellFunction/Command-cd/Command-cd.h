#ifdef KSWORD_WITH_COMMAND
#pragma once
#include "../../KswordTotalHead.h"

inline void KInlineForPathDeal1() {

}

inline void KInlineForCommandcd1() {
	size_t firstBackslashPos = path.find('\\'); // 查找第一个反斜杠的位置
	if (firstBackslashPos != std::string::npos) {
		// 如果找到了反斜杠，截取从开始到反斜杠之前的内容
		path = path.substr(0, firstBackslashPos);
		path += "\\";//保证路径规范，以\结尾 
	}
}
inline void KInlineForCommandcd2() {
	size_t secondSlashPos = path.find_last_of('\\', path.length() - 2);
	// 如果找到了第二个"\"，并且它不是最后一个字符
	if (secondSlashPos != std::string::npos && secondSlashPos < path.length() - 1) {
		// 删除从第二个"\"之后的所有内容
		path = path.substr(0, secondSlashPos + 1);
	}
}
inline void KInlineForCommandcd3() {
	size_t secondSlashPos = path.find_last_of('\\', path.length() - 2);
	// 如果找到了第二个"\"，并且它不是最后一个字符
	if (secondSlashPos != std::string::npos && secondSlashPos < path.length() - 1) {
		// 删除从第二个"\"之后的所有内容
		path = path.substr(0, secondSlashPos + 1);
	}
}
#endif