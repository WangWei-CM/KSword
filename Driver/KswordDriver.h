#pragma once
int ReleaseDriverToFile();
int  ReleasePFXToFile();
int KswordDriverInit();
BOOL InstallDriver(const std::wstring& drvAbsPath);   // 加载（安装）驱动
BOOL StartDriver();                                   // 启动驱动
BOOL StopDriver();                                    // 停止驱动
BOOL UnloadDriver();                                  // 卸载驱动