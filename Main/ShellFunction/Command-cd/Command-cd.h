#ifdef KSWORD_WITH_COMMAND
#pragma once
#include "../../KswordTotalHead.h"

inline void KInlineForPathDeal1() {

}

inline void KInlineForCommandcd1() {
	size_t firstBackslashPos = path.find('\\'); // ���ҵ�һ����б�ܵ�λ��
	if (firstBackslashPos != std::string::npos) {
		// ����ҵ��˷�б�ܣ���ȡ�ӿ�ʼ����б��֮ǰ������
		path = path.substr(0, firstBackslashPos);
		path += "\\";//��֤·���淶����\��β 
	}
}
inline void KInlineForCommandcd2() {
	size_t secondSlashPos = path.find_last_of('\\', path.length() - 2);
	// ����ҵ��˵ڶ���"\"���������������һ���ַ�
	if (secondSlashPos != std::string::npos && secondSlashPos < path.length() - 1) {
		// ɾ���ӵڶ���"\"֮�����������
		path = path.substr(0, secondSlashPos + 1);
	}
}
inline void KInlineForCommandcd3() {
	size_t secondSlashPos = path.find_last_of('\\', path.length() - 2);
	// ����ҵ��˵ڶ���"\"���������������һ���ַ�
	if (secondSlashPos != std::string::npos && secondSlashPos < path.length() - 1) {
		// ɾ���ӵڶ���"\"֮�����������
		path = path.substr(0, secondSlashPos + 1);
	}
}
#endif