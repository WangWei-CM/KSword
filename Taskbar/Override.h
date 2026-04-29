#include <QPushButton>
#include <QGraphicsDropShadowEffect>
#include <QEnterEvent>
#include <QIcon>
#include <QPixmap>
#include <QImage>
#include <QColor>

class GlowIconButton : public QPushButton {
    Q_OBJECT
public:
    // 构造函数：传入图标路径+图标大小（尺寸完全由图标大小决定）
    explicit GlowIconButton(const QString& iconPath, const QSize& iconSize, QWidget* parent = nullptr)
        : QPushButton(parent) {
        // 1. 加载图标并验证
        QIcon originalIcon(iconPath);
        if (originalIcon.isNull()) {
            qWarning() << "图标加载失败！路径：" << iconPath;
            return;
        }

        // 2. 精准变色（只处理图标本体，保留透明背景）
        QIcon coloredIcon = colorizeIcon(originalIcon, iconSize, QColor("#43A0FF"));
        setIcon(coloredIcon);
        setIconSize(iconSize); // 强制图标尺寸

        // 3. 发光效果
        glowEffect = new QGraphicsDropShadowEffect(this);
        glowEffect->setColor(QColor("#43A0FF"));
        glowEffect->setBlurRadius(8); // 发光强度（适中）
        glowEffect->setOffset(0, 0);
        glowEffect->setEnabled(false);
        setGraphicsEffect(glowEffect);

        // 4. 样式表：边框自适应，无固定尺寸
        setStyleSheet(R"(
            QPushButton {
                border: 0px solid #ccc;
                border-radius: 3px;
                padding: 3px; /* 图标与边框的间距（小一点更紧凑） */
                background-color: transparent;
                /* 移除固定宽高，完全由图标+内边距决定 */
            }
            QPushButton:hover {
                border-color: #43A0FF;
            }
            QPushButton:pressed {
                background-color: rgba(67, 160, 255, 0.14);
            }
        )");
    }

protected:
    // 鼠标进入：启用发光
    void enterEvent(QEnterEvent* event) override {
        glowEffect->setEnabled(true);
        QPushButton::enterEvent(event);
    }

    // 鼠标离开：关闭发光
    void leaveEvent(QEvent* event) override {
        glowEffect->setEnabled(false);
        QPushButton::leaveEvent(event);
    }

private:
    QGraphicsDropShadowEffect* glowEffect;

    // 精准变色算法：只处理非透明的深色像素
    QIcon colorizeIcon(const QIcon& original, const QSize& iconSize, const QColor& targetColor) {
        QIcon coloredIcon;
        // 直接使用指定的图标尺寸（避免自动缩放导致的问题）
        QPixmap pix = original.pixmap(iconSize);
        QImage img = pix.toImage(); // 转为QImage处理像素

        // 遍历每个像素
        for (int y = 0; y < img.height(); ++y) {
            for (int x = 0; x < img.width(); ++x) {
                QRgb pixel = img.pixel(x, y);
                int alpha = qAlpha(pixel); // 获取透明度（0=完全透明，255=完全不透明）

                // 只处理「非透明」且「深色」的像素（图标本体）
                if (alpha > 50) { // 排除几乎透明的像素（背景）
                    int red = qRed(pixel);
                    int green = qGreen(pixel);
                    int blue = qBlue(pixel);
                    // 判断是否为深色（RGB值都较低）
                    if (red < 100 && green < 100 && blue < 100) {
                        // 替换为目标颜色，保留原透明度
                        QRgb newPixel = qRgba(
                            targetColor.red(),
                            targetColor.green(),
                            targetColor.blue(),
                            alpha // 关键：保留原图标的透明度
                        );
                        img.setPixel(x, y, newPixel);
                    }
                }
            }
        }

        // 转换回QPixmap并添加到图标
        coloredIcon.addPixmap(QPixmap::fromImage(img));
        return coloredIcon;
    }
};
