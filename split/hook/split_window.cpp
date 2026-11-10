#include "split_window.h"

#include <algorithm>
#include <utility>
#include <vector>

#include <QColor>
#include <QFontMetrics>
#include <QPainter>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include "split_hook.h"

namespace SplitWindow {
namespace {

constexpr int kPadding = 8;
constexpr int kMargin = 16;
constexpr int kRefreshMs = 500;

// A frameless, see-through tool window, like the emulator's own performance
// overlay: a child widget would sit under the native render surface.
class Panel : public QWidget {
public:
  Panel(QWidget* main, QWidget* render) : QWidget(main), render_(render) {
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowTransparentForInput |
                   Qt::WindowDoesNotAcceptFocus);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    QFont f = font();
    f.setPixelSize(13);
    f.setWeight(QFont::DemiBold);
    setFont(f);

    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] { Refresh(); });
    timer->start(kRefreshMs);
  }

protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 150));
    p.drawRoundedRect(rect(), 6, 6);

    const QFontMetrics fm(font());
    int y = kPadding + fm.ascent();
    for (const auto& line : lines_) {
      p.setPen(line.dim ? QColor(170, 170, 170) : QColor(255, 255, 255));
      p.drawText(kPadding, y, QString::fromStdString(line.text));
      y += fm.lineSpacing();
    }
  }

private:
  void Refresh() {
    lines_ = SplitHook::OverlayLines();
    QWidget* main = parentWidget();
    const bool game_shown = render_ && render_->isVisible();
    if (lines_.empty() || !main || !main->isVisible() || main->isMinimized()) {
      hide();
      return;
    }

    const QFontMetrics fm(font());
    int width = 0;
    for (const auto& line : lines_) {
      width = std::max(width, fm.horizontalAdvance(QString::fromStdString(line.text)));
    }
    const int height = static_cast<int>(lines_.size()) * fm.lineSpacing();
    resize(width + 2 * kPadding, height + 2 * kPadding);

    QWidget* anchor = game_shown ? render_.data() : main;
    move(anchor->mapToGlobal(QPoint(kMargin, kMargin)));
    if (!isVisible()) {
      show();
    }
    update();
  }

  QPointer<QWidget> render_;
  std::vector<SplitHook::OverlayLine> lines_;
};

}  // namespace

void Attach(QWidget* main, QWidget* render) {
  SplitHook::Start();
  if (!SplitHook::Enabled()) {
    return;
  }
  new Panel(main, render);
}

}  // namespace SplitWindow
