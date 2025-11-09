#pragma once
#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QDesktopServices>
#include <QUrl>

class WelcomeDock : public QWidget
{
    Q_OBJECT
public:

    explicit WelcomeDock(QWidget* parent = nullptr);

    void loadUserAvatar(QLabel* avatarLabel);
    QString getWindowsUserAvatarPath();
    QString getWindowsUserName(); 

    QLabel* m_leftImage;    // 左侧图片
    QLabel* m_copyright;    // 版权信息
    QPushButton* m_githubBtn; // Github按钮
    QPushButton* m_qqBtn;    // QQ群按钮
    QPushButton* m_webBtn;   // 网站按钮

    // 右侧组件
    QLabel* m_userAvatar;   // 用户头像
    QLabel* m_greeting;     // 问候语

    // 布局管理器
    QHBoxLayout* m_mainLayout;  // 主布局（左右分栏）
    QVBoxLayout* m_leftLayout;  // 左侧垂直布局
    QHBoxLayout* m_btnLayout;   // 按钮水平布局
    QVBoxLayout* m_rightLayout; // 右侧垂直布局
};
