#pragma once
#include "common.hpp"
#include <QWidget>
#include <vector>

class QComboBox;
class QToolBar;
class QHBoxLayout;
class QVBoxLayout;
class QToolButton;
class QLabel;
class QSlider;
class QAction;
class QCheckBox;
class QLineEdit;
class QFrame;

class DrawView : public QWidget {
	Q_OBJECT
public:
	explicit DrawView(QWidget *parent);
	~DrawView() override;
	QPaintEngine *paintEngine() const override { return nullptr; }

protected:
	void paintEvent(QPaintEvent *) override {}
	void showEvent(QShowEvent *) override;
	void resizeEvent(QResizeEvent *) override;
	bool event(QEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void tabletEvent(QTabletEvent *e) override;

private:
	void create();
	void destroy();
	void input(int phase, QPointF pos, float pressure, bool eraser = false);
	static void render(void *data, uint32_t cx, uint32_t cy);
	obs_display_t *display = nullptr;
	bool drawing = false;
};

class Panel : public QWidget {
	Q_OBJECT
public:
	explicit Panel(QWidget *dockHost);
	~Panel() override;
	void action(int a); // a Tool or an Action
	void refreshTargets();

protected:
	bool eventFilter(QObject *o, QEvent *e) override;
	void changeEvent(QEvent *e) override;

private:
	void buildSettings();
	void updateIcons();
	void loadPresets();
	void savePresets();
	void applyPreset(int i);
	void buildPresetMenu();
	void setWindowMode(bool on);
	void selectTool(int t);
	void targetChanged();
	void syncColor();
	void withCanvas(const std::function<void(Canvas *)> &f);

	QWidget *dockHost, *window = nullptr, *settings = nullptr;
	QVBoxLayout *root = nullptr;
	QHBoxLayout *top = nullptr;
	QFrame *sep = nullptr;
	QComboBox *target = nullptr, *bgBox = nullptr;
	QToolBar *toolbar = nullptr;
	QAction *toolActs[ToolCount] = {}, *moreAct = nullptr;
	QToolButton *undoBtn = nullptr, *redoBtn = nullptr, *clearBtn = nullptr, *popBtn = nullptr, *setBtn = nullptr,
		    *colorBtn = nullptr, *presetBtn = nullptr;
	QSlider *size = nullptr;
	QLabel *sizeLbl = nullptr;
	QCheckBox *remoteOn = nullptr;
	QLineEdit *remoteUrl = nullptr;
	DrawView *view = nullptr;
	bool more = false;
	std::vector<std::function<void()>> syncers; // refresh controls from brush()
	std::vector<std::pair<QString, BrushState>> presets;
};
