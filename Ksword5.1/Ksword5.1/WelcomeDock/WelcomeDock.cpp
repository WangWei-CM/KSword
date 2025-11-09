#include "WelcomeDock.h"
#include "../UI/UI.css/UI_css.h"
#include <QStyle>
#include <QDir>
#include <QPixmap>
#include <QStandardPaths>
#include <windows.h>  // Windows API 头文件
#include <shlobj.h>   // 用于获取用户目录（需链接 shell32.lib）
// 获取 Windows 登录头像路径（优先找用户自定义头像，找不到用系统默认）
QString WelcomeDock::getWindowsUserName() {
    WCHAR userNameBuffer[256] = { 0 }; // 存储用户名的缓冲区
    DWORD bufferSize = sizeof(userNameBuffer) / sizeof(WCHAR); // 缓冲区大小

    // 调用Windows API获取用户名（宽字符版本，支持中文）
    if (GetUserNameW(userNameBuffer, &bufferSize)) {
        return QString::fromWCharArray(userNameBuffer); // 转换为QString
    }
    else {
        return "用户"; // 获取失败时的默认文本
    }
}
QString WelcomeDock::getWindowsUserAvatarPath() {
    // 1. 用户自定义头像路径（Win10/Win11 通用）
    QString accountPicsPath = QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
        + "/AppData/Roaming/Microsoft/Windows/Account Pictures/";

    QDir accountDir(accountPicsPath);
    if (accountDir.exists()) {
        // 查找目录下的图片文件（优先 .png，其次 .jpg）
        QStringList filters;
        filters << "*.png" << "*.jpg" << "*.jpeg";
        QFileInfoList fileList = accountDir.entryInfoList(filters, QDir::Files);

        // 低 Qt 版本无 SizeReversed，手动按文件大小排序（取最大文件，通常是高清头像）
        std::sort(fileList.begin(), fileList.end(), [](const QFileInfo& a, const QFileInfo& b) {
            return a.size() > b.size(); // 降序排序（大文件在前）
            });

        if (!fileList.isEmpty()) {
            return fileList.first().absoluteFilePath();
        }
    }

    // 2. 系统默认头像路径（如果用户未自定义）
    QString defaultAvatarPath = "C:/ProgramData/Microsoft/User Account Pictures/default.png";
    if (QFile::exists(defaultAvatarPath)) {
        return defaultAvatarPath;
    }

    // 3. 兜底：系统默认备选头像（不同版本路径可能不同）
    QString defaultAvatarBackup = "C:/ProgramData/Microsoft/User Account Pictures/guest.png";
    if (QFile::exists(defaultAvatarBackup)) {
        return defaultAvatarBackup;
    }

    return "";  // 未找到头像
}// 加载头像并显示到 QLabel（自动缩放，保持比例）
void WelcomeDock::loadUserAvatar(QLabel* avatarLabel) {
    QString avatarPath = WelcomeDock::getWindowsUserAvatarPath();
    if (avatarPath.isEmpty() || !QFile::exists(avatarPath)) {
        // 加载失败：显示占位文本
        avatarLabel->setText("当前Windows<br>登陆账户图片");
        return;
    }

    // 加载图片并缩放（适配 QLabel 尺寸，保持宽高比，抗锯齿）
    QPixmap avatarPixmap(avatarPath);
    if (avatarPixmap.isNull()) {
        avatarLabel->setText("当前Windows<br>登陆账户图片");
        return;
    }

    // 缩放图片（最小尺寸 100x100，最大尺寸不超过标签大小）
    QSize labelSize = avatarLabel->minimumSize();
    QPixmap scaledPixmap = avatarPixmap.scaled(labelSize,
        Qt::KeepAspectRatio,
        Qt::SmoothTransformation);

    // 显示图片
    avatarLabel->setPixmap(scaledPixmap);
    avatarLabel->setText("");  // 清空占位文本
}
WelcomeDock::WelcomeDock(QWidget* parent) : QWidget(parent) {
    // ==== 1. 初始化组件 ====
// 左侧图片（用占位文本模拟，实际可替换为QPixmap）
    m_leftImage = new QLabel(this);
    // 1. 设置 QLabel 固定大小为图片原始尺寸（655x250）
    m_leftImage->setFixedSize(655, 250);
    // 2. 关闭自动扩展（避免被布局拉伸）
    m_leftImage->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_leftImage->setAlignment(Qt::AlignCenter);

    // 加载图片（不缩放，直接使用原始尺寸）
    QPixmap pixmap(":/Image/Resource/Logo/MainLogo.png");
    if (!pixmap.isNull()) {
        // 直接设置原图（不调用 scaled，保持 655x250 像素）
        m_leftImage->setPixmap(pixmap);
    }
    else {
        m_leftImage->setText("左侧图片区域");
    }
    // 版权信息
    m_copyright = new QLabel(this);
    m_copyright->setText("Ksword Dev 卡利剑ARK工具开发团队 保留所有权利。<br>");
    m_copyright->setStyleSheet("line-height: 1.8;");
    m_copyright->setWordWrap(true); // 开启自动换行
    m_copyright->setTextInteractionFlags(Qt::TextSelectableByMouse); // 允许选中
    m_copyright->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    // 可选：限制最大高度（防止内容过多时占满空间）
    m_copyright->setMaximumHeight(100); // 根据实际行数调整（3行约60px，留余量）
    // 按钮
    m_githubBtn = new QPushButton("Github仓库", this);
    m_githubBtn->setStyleSheet(QSS_Buttons_Light);

    m_qqBtn = new QPushButton("QQ群", this);
	m_qqBtn->setStyleSheet(QSS_Buttons_Light);

    m_webBtn = new QPushButton("网站", this);
	m_webBtn->setStyleSheet(QSS_Buttons_Light);

    // 右侧用户头像（用占位文本模拟）
    // 右侧用户头像（初始隐藏文本，加载成功显示图片，失败显示占位）
    m_userAvatar = new QLabel(this);
    //m_userAvatar->setStyleSheet("padding: 15px;");
    m_userAvatar->setAlignment(Qt::AlignCenter);
    m_userAvatar->setMinimumSize(100, 100);
    loadUserAvatar(m_userAvatar);  // 加载 Windows 头像

    // 问候语（显示实际 Windows 用户名）
    QString userName = getWindowsUserName();// 获取当前 Windows 用户名
    m_greeting = new QLabel(QString("早上好！%1").arg(userName), this);
    m_greeting->setStyleSheet("font-size: 16px;");
    m_greeting->setAlignment(Qt::AlignCenter);

    // ==== 2. 布局管理 ====
    // 按钮布局（水平排列，等宽拉伸）
    m_btnLayout = new QHBoxLayout();
    m_btnLayout->addWidget(m_githubBtn, 1); // 占1份宽度
    m_btnLayout->addWidget(m_qqBtn, 1);
    m_btnLayout->addWidget(m_webBtn, 1);
    m_btnLayout->setSpacing(10); // 按钮间距

    // 左侧布局（垂直排列：图片 -> 版权 -> 按钮）
    m_leftLayout = new QVBoxLayout();
    m_leftLayout->addWidget(m_leftImage);
    m_leftLayout->addWidget(m_copyright);
    m_leftLayout->addLayout(m_btnLayout);
    m_leftLayout->setSpacing(20); // 组件间距
    m_leftLayout->setContentsMargins(20, 20, 20, 20); // 左布局内边距
    m_leftLayout->addStretch(1);
    m_leftLayout->addLayout(m_btnLayout);
    m_leftLayout->setSpacing(20);
    m_leftLayout->setContentsMargins(20, 20, 20, 20);
    // 右侧布局（垂直排列：头像 -> 问候语，居中对齐）
    m_rightLayout = new QVBoxLayout();
    m_rightLayout->addWidget(m_userAvatar);
    m_rightLayout->addWidget(m_greeting);
    m_rightLayout->setAlignment(Qt::AlignTop); // 整体居中
    m_rightLayout->setContentsMargins(0,0,0,0); // 右布局内边距

    // 主布局（水平分栏：左侧占3份，右侧占1份）
    m_mainLayout = new QHBoxLayout(this);
    m_mainLayout->addLayout(m_leftLayout, 3); // 左侧占3/4宽度
    m_mainLayout->addLayout(m_rightLayout, 1); // 右侧占1/4宽度
    m_mainLayout->setContentsMargins(0, 0, 0, 0); // 主布局无边距（消除白边）
    setLayout(m_mainLayout);


    // ==== 3. 信号连接（示例：Github按钮打开链接） ====
    connect(m_githubBtn, &QPushButton::clicked, this, [=]() {
        QDesktopServices::openUrl(QUrl("https://github.com/xxx"));
        });
}
