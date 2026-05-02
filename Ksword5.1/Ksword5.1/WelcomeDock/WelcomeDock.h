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

    QLabel* m_leftImage;       // 左侧图片：展示欢迎页主 Logo。
    QLabel* m_copyright;       // 版权信息：展示版权、版本号和编译时间。
    QLabel* m_contributors;    // 贡献者信息：展示当前参与名单。
    QLabel* m_referenceTitle;  // 参考项目标题：说明下方按钮均为外部参考仓库入口。
    QPushButton* m_githubBtn;  // Github按钮：打开项目仓库入口。
    QPushButton* m_qqBtn;      // QQ群按钮：保留项目交流群入口。
    QPushButton* m_pplControlBtn;      // PPLcontrol按钮：打开 PPLcontrol 参考仓库。
    QPushButton* m_systemInformerBtn;  // System Informer按钮：打开 System Informer 参考仓库。
    QPushButton* m_skt64Btn;           // SKT64按钮：打开 SKT64 参考仓库。

    // 布局管理器：欢迎页只保留主内容区，并在主入口下方展示贡献者和参考项目。
    QHBoxLayout* m_mainLayout;      // 主布局：承载欢迎页主体内容。
    QVBoxLayout* m_leftLayout;      // 左侧垂直布局：按 Logo、发布信息、按钮区和扩展信息排列。
    QHBoxLayout* m_btnLayout;       // 按钮水平布局：放置 Github 与 QQ 群按钮。
    QHBoxLayout* m_referenceLayout; // 参考项目布局：横向放置外部参考仓库按钮。
};
