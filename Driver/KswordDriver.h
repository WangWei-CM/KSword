#pragma once
int ReleaseDriverToFile();
int  ReleasePFXToFile();
int KswordDriverInit();
BOOL InstallDriver(const std::wstring& drvAbsPath);   // ���أ���װ������
BOOL StartDriver();                                   // ��������
BOOL StopDriver();                                    // ֹͣ����
BOOL UnloadDriver();                                  // ж������