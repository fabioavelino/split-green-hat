#pragma once

class QWidget;

namespace SplitWindow {

// Starts split mode and shows the small status panel over the game. Call once
// from the GUI thread, after the render window exists. The panel is owned by
// `main` and deleted with it.
void Attach(QWidget* main, QWidget* render);

}  // namespace SplitWindow
