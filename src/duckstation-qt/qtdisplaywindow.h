#pragma once
#include <QtWidgets/QWidget>
#include "common/types.h"

class QKeyEvent;
class QResizeEvent;

class HostDisplay;

class QtHostInterface;

class QtDisplayWindow : public QWidget
{
  Q_OBJECT

public:
  QtDisplayWindow(QtHostInterface* host_interface, QWidget* parent);
  virtual ~QtDisplayWindow();

  virtual HostDisplay* getHostDisplayInterface();

  virtual bool hasDeviceContext() const;
  virtual bool createDeviceContext(QThread* worker_thread, bool debug_device);
  virtual bool initializeDeviceContext(bool debug_device);
  virtual void destroyDeviceContext();

  virtual void Render() = 0;

  // this comes back on the emu thread
  virtual void WindowResized(s32 new_window_width, s32 new_window_height);

Q_SIGNALS:
  void windowResizedEvent(int width, int height);

protected:
  int getScaledWindowWidth() const { return static_cast<int>(static_cast<qreal>(width()) * devicePixelRatio()); }
  int getScaledWindowHeight() const { return static_cast<int>(static_cast<qreal>(height()) * devicePixelRatio()); }

  virtual bool createImGuiContext();
  virtual void destroyImGuiContext();
  virtual bool createDeviceResources();
  virtual void destroyDeviceResources();

  virtual void keyPressEvent(QKeyEvent* event) override;
  virtual void keyReleaseEvent(QKeyEvent* event) override;
  virtual void resizeEvent(QResizeEvent* event) override;

  QtHostInterface* m_host_interface;
};
