#pragma once

#include <QImage>
#include <QRectF>
#include <QWidget>
#include <functional>

namespace Trans {

// One frozen screen per overlay; logical pointer coordinates are mapped to image pixels.
class RegionOverlay final : public QWidget {
public:
    RegionOverlay(QImage image, const QRect &geometry);
    std::function<void(const QImage &)> selected;
    std::function<void()> cancelled;
    static QRect pixelRect(const QRectF &selection, const QSize &logicalSize, const QSize &imageSize);
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
private:
    QImage m_image;
    QPointF m_origin;
    QPointF m_current;
    bool m_dragging = false;
};

} // namespace Trans
