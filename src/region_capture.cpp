#include "region_capture.h"
#include <QCloseEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QtMath>

namespace Trans {
RegionOverlay::RegionOverlay(QImage image, const QRect &geometry)
    : QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::BypassWindowManagerHint),
      m_image(std::move(image))
{
    setObjectName(QStringLiteral("screenshotRegionOverlay"));
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_QuitOnClose, false);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
    setGeometry(geometry);
    // Drawing and cropping use physical pixels, independently of the source pixmap DPR.
    m_image.setDevicePixelRatio(1);
}

QRect RegionOverlay::pixelRect(const QRectF &selection, const QSize &logicalSize, const QSize &imageSize)
{
    if (logicalSize.isEmpty() || imageSize.isEmpty()) return {};
    const auto rect = selection.normalized().intersected(QRectF(QPointF(0, 0), QSizeF(logicalSize)));
    if (rect.isEmpty()) return {};
    const qreal scaleX = qreal(imageSize.width()) / logicalSize.width();
    const qreal scaleY = qreal(imageSize.height()) / logicalSize.height();
    const int left = qFloor(rect.left() * scaleX);
    const int top = qFloor(rect.top() * scaleY);
    const int right = qCeil(rect.right() * scaleX);
    const int bottom = qCeil(rect.bottom() * scaleY);
    return QRect(left, top, right - left, bottom - top).intersected(QRect(QPoint(0, 0), imageSize));
}

void RegionOverlay::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.drawImage(rect(), m_image);
    const QRectF area = QRectF(m_origin, m_current).normalized().intersected(QRectF(rect()));
    QRegion shade(rect());
    if (m_dragging) shade -= area.toAlignedRect();
    painter.setClipRegion(shade);
    painter.fillRect(rect(), QColor(0, 0, 0, 110));
    painter.setClipping(false);
    if (m_dragging && !area.isEmpty()) {
        painter.setPen(QPen(QColor(80, 170, 255), 2));
        painter.drawRect(area.adjusted(1, 1, -1, -1));
    }
    QString hint = QStringLiteral("拖动鼠标框选文字，松开识别 · Esc / 右键取消");
    if (m_dragging) {
        const auto pixels = pixelRect(area, size(), m_image.size());
        hint = QStringLiteral("%1 × %2 像素 · 松开识别 · Esc / 右键取消").arg(pixels.width()).arg(pixels.height());
    }
    auto font = painter.font();
    font.setPixelSize(15);
    painter.setFont(font);
    const auto textSize = painter.fontMetrics().size(Qt::TextSingleLine, hint);
    const QRect label(16, 16, textSize.width() + 24, textSize.height() + 16);
    painter.fillRect(label, QColor(20, 24, 30, 230));
    painter.setPen(Qt::white);
    painter.drawText(label, Qt::AlignCenter, hint);
}

void RegionOverlay::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::RightButton) {
        if (const auto callback = cancelled) callback();
    } else if (event->button() == Qt::LeftButton) {
        m_origin = m_current = event->position();
        m_dragging = true;
        setFocus();
        update();
    }
}
void RegionOverlay::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging) return;
    m_current = event->position();
    update();
}
void RegionOverlay::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_dragging || event->button() != Qt::LeftButton) return;
    m_current = event->position();
    m_dragging = false;
    const auto pixels = pixelRect(QRectF(m_origin, m_current), size(), m_image.size());
    // Ignore clicks/tiny drags; retain the overlay so the user can select again.
    if (pixels.width() < 15 || pixels.height() < 15) { update(); return; }
    if (const auto callback = selected) callback(m_image.copy(pixels));
}
void RegionOverlay::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        event->accept();
        if (const auto callback = cancelled) callback();
    } else QWidget::keyPressEvent(event);
}
void RegionOverlay::closeEvent(QCloseEvent *event)
{
    event->ignore();
    if (const auto callback = cancelled) callback();
}

} // namespace Trans
