#include "panel.hpp"
#include "canvas.hpp"
#include "remote.hpp"
#include <obs-frontend-api.h>
#ifdef __linux__
#include <obs-nix-platform.h>
#endif
#include <algorithm>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPlatformSurfaceEvent>
#include <QPushButton>
#include <QSlider>
#include <QSvgRenderer>
#include <QTabletEvent>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

namespace {
const char *UndoIcon = "M9 14L4 9l5-5 M4 9h10a6 6 0 010 12h-3", *RedoIcon = "M15 14l5-5-5-5 M20 9H10a6 6 0 000 12h3",
	   *PopIcon = "M14 4h6v6 M20 4l-8 8 M18 14v6H4V6h6", *DockIcon = "M14 10l6-6 M4 14h6v6 M10 14l-6 6 M20 10h-6V4",
	   *SettingsIcon = "M4 7h10 M18 7h2 M4 17h2 M10 17h10 M16 5v4 M8 15v4",
	   *MoreIcon = "M5 12h.01 M12 12h.01 M19 12h.01", *PresetIcon = "M6 4h12v16l-6-4-6 4z",
	   *ClearIcon = "M4 7h16 M9 7V4h6v3 M6 7l1 13h10l1-13", *CopyIcon = "M9 9h11v11H9z M5 15H4V4h11v1",
	   *RegenIcon = "M20 11a8 8 0 00-14.9-3 M4 4v4h4 M4 13a8 8 0 0014.9 3 M20 20v-4h-4";
const QColor swatches[] = {QColor(0x8e, 0xa6, 0xff), QColor(0xed, 0xed, 0xed), QColor(0xff, 0xb8, 0x6b),
			   QColor(0x7c, 0xf2, 0xc8), QColor(0xff, 0x7a, 0xb6), QColor(0xff, 0x5a, 0x5f)};

QIcon svgIcon(const char *d, const QColor &c)
{
	const QString svg =
		QString("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='%1' "
			"stroke-width='1.7' stroke-linecap='round' stroke-linejoin='round'><path d='%2'/></svg>")
			.arg(c.name(), d);
	QSvgRenderer r(svg.toUtf8());
	QPixmap pm(48, 48);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	r.render(&p);
	return QIcon(pm);
}

QIcon dotIcon(const QColor &c, bool ring = false)
{
	QPixmap pm(40, 40);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	if (ring) {
		p.setPen(QPen(qApp->palette().color(QPalette::ButtonText), 2.5));
		p.drawEllipse(QRectF(3, 3, 34, 34));
	}
	p.setPen(Qt::NoPen);
	p.setBrush(c);
	p.drawEllipse(QRectF(9, 9, 22, 22));
	return QIcon(pm);
}
} // namespace

// --- DrawView ----------------------------------------------------------------

DrawView::DrawView(QWidget *parent) : QWidget(parent)
{
	for (auto a : {Qt::WA_PaintOnScreen, Qt::WA_StaticContents, Qt::WA_NoSystemBackground, Qt::WA_OpaquePaintEvent,
		       Qt::WA_DontCreateNativeAncestors, Qt::WA_NativeWindow})
		setAttribute(a);
	setCursor(Qt::CrossCursor);
	setMinimumSize(160, 90);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

DrawView::~DrawView()
{
	destroy();
}

void DrawView::create()
{
	if (display || !isVisible())
		return;
	const QSize s = size() * devicePixelRatioF();
	gs_init_data info{};
	info.cx = uint32_t(s.width());
	info.cy = uint32_t(s.height());
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;
#ifdef _WIN32
	info.window.hwnd = reinterpret_cast<void *>(winId());
#elif defined(__APPLE__)
	info.window.view = reinterpret_cast<id>(winId());
#else
	info.window.id = uint32_t(winId());
	info.window.display = obs_get_nix_platform_display();
#endif
	const QColor c = palette().color(QPalette::Window).darker(130);
	display = obs_display_create(&info, 0xFF000000u | uint32_t(c.blue()) << 16 | uint32_t(c.green()) << 8 |
						    uint32_t(c.red()));
	obs_display_add_draw_callback(display, render, this);
}

void DrawView::destroy()
{
	if (!display)
		return;
	obs_display_remove_draw_callback(display, render, this);
	obs_display_destroy(display);
	display = nullptr;
}

void DrawView::showEvent(QShowEvent *e)
{
	QWidget::showEvent(e);
	QTimer::singleShot(0, this, &DrawView::create);
}

void DrawView::resizeEvent(QResizeEvent *e)
{
	QWidget::resizeEvent(e);
	const QSize s = size() * devicePixelRatioF();
	if (display)
		obs_display_resize(display, uint32_t(s.width()), uint32_t(s.height()));
	else
		create();
}

bool DrawView::event(QEvent *e)
{
	if (e->type() == QEvent::PlatformSurface) {
		if (static_cast<QPlatformSurfaceEvent *>(e)->surfaceEventType() ==
		    QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
			destroy();
		else
			QTimer::singleShot(0, this, &DrawView::create);
	}
	return QWidget::event(e);
}

void DrawView::render(void *, uint32_t cx, uint32_t cy)
{
	OBSSource owner = getTarget();
	obs_source_t *rs = owner ? renderSourceOf(owner) : nullptr;
	const uint32_t sw = rs ? obs_source_get_width(rs) : 0, sh = rs ? obs_source_get_height(rs) : 0;
	if (!sw || !sh)
		return;
	const float s = std::min(float(cx) / sw, float(cy) / sh);
	const int w = int(sw * s), h = int(sh * s);
	gs_viewport_push();
	gs_projection_push();
	gs_set_viewport((int(cx) - w) / 2, (int(cy) - h) / 2, w, h);
	gs_ortho(0, float(sw), 0, float(sh), -100, 100);
	obs_source_video_render(rs);
	gs_projection_pop();
	gs_viewport_pop();
}

void DrawView::input(int phase, QPointF pos, float pr, bool eraser)
{
	OBSSource owner = getTarget();
	Canvas *c = canvasOf(owner);
	obs_source_t *rs = c ? renderSourceOf(owner) : nullptr;
	const double sw = rs ? obs_source_get_width(rs) : 0, sh = rs ? obs_source_get_height(rs) : 0;
	if (!sw || !sh || !c->w) {
		drawing = false;
		return;
	}
	const double dpr = devicePixelRatioF(), W = width() * dpr, H = height() * dpr, s = std::min(W / sw, H / sh);
	const QPointF q((pos.x() * dpr - (W - sw * s) / 2) / s * c->w / sw,
			(pos.y() * dpr - (H - sh * s) / 2) / s * c->h / sh);
	BrushState b = brush();
	if (eraser)
		b.tool = Eraser;
	if (phase == 0) {
		if (b.tool == Text) {
			bool ok = false;
			const QString t = QInputDialog::getText(this, Str("Tool.Text"), Str("EnterText"),
								QLineEdit::Normal, {}, &ok);
			if (ok)
				c->down(0, q, pr, b, t);
			return;
		}
		drawing = true;
		c->down(0, q, pr, b);
	} else if (drawing && phase == 1)
		c->move(0, q, pr);
	else if (drawing) {
		c->up(0);
		drawing = false;
	}
}

void DrawView::mousePressEvent(QMouseEvent *e)
{
	if (e->button() == Qt::LeftButton)
		input(0, e->position(), 1);
}

void DrawView::mouseMoveEvent(QMouseEvent *e)
{
	input(1, e->position(), 1);
}

void DrawView::mouseReleaseEvent(QMouseEvent *e)
{
	if (e->button() == Qt::LeftButton)
		input(2, e->position(), 1);
}

void DrawView::tabletEvent(QTabletEvent *e)
{
	e->accept();
	const int phase = e->type() == QEvent::TabletPress ? 0 : e->type() == QEvent::TabletMove ? 1 : 2;
	input(phase, e->position(), float(e->pressure()), e->pointerType() == QPointingDevice::PointerType::Eraser);
}

// --- Panel -------------------------------------------------------------------

static void queueRefresh(void *p, calldata_t *)
{
	QMetaObject::invokeMethod(static_cast<Panel *>(p), &Panel::refreshTargets, Qt::QueuedConnection);
}
static const char *const sourceSignals[] = {"source_create", "source_destroy", "source_rename"};

Panel::Panel(QWidget *host) : QWidget(host), dockHost(host)
{
	BrushState &b = brush();
	obs_data_t *cfg = config();
	if (obs_data_has_user_value(cfg, "color"))
		readBrush(cfg, b);
	loadPresets();

	root = new QVBoxLayout(this);
	root->setContentsMargins(8, 8, 8, 8);
	root->setSpacing(8);
	top = new QHBoxLayout;
	top->setSpacing(2);
	root->addLayout(top);

	target = new QComboBox;
	target->setSizeAdjustPolicy(QComboBox::AdjustToContents);
	target->installEventFilter(this);
	connect(target, &QComboBox::currentIndexChanged, this, &Panel::targetChanged);
	top->addWidget(target);
	sep = new QFrame;
	sep->setFrameShape(QFrame::VLine);
	sep->hide();
	top->addWidget(sep);
	top->addStretch();
	auto button = [this](const char *tip, auto fn) {
		auto btn = new QToolButton;
		btn->setAutoRaise(true);
		btn->setToolTip(Str(tip));
		connect(btn, &QToolButton::clicked, this, fn);
		top->addWidget(btn);
		return btn;
	};
	undoBtn = button("Undo", [this] { action(Undo); });
	redoBtn = button("Redo", [this] { action(Redo); });
	clearBtn = button("Clear", [this] {
		if (QMessageBox::question(this, Str("Clear"), Str("ClearConfirm")) == QMessageBox::Yes)
			action(ClearCanvas);
	});
	popBtn = button("PopOut", [this] { setWindowMode(!window); });
	setBtn = button("Settings", [this](bool on) { settings->setVisible(on); });
	setBtn->setCheckable(true);

	toolbar = new QToolBar;
	toolbar->setIconSize(QSize(18, 18));
	auto group = new QActionGroup(this);
	for (int i = 0; i < ToolCount; i++) {
		toolActs[i] = toolbar->addAction(Str((std::string("Tool.") + tools[i].id).c_str()),
						 [this, i] { selectTool(i); });
		toolActs[i]->setCheckable(true);
		toolActs[i]->setVisible(i < PrimaryTools);
		group->addAction(toolActs[i]);
	}
	moreAct = toolbar->addAction(Str("MoreTools"), [this] {
		more = !more;
		for (int i = PrimaryTools; i < ToolCount; i++)
			toolActs[i]->setVisible(more);
	});
	root->addWidget(toolbar);

	buildSettings();
	view = new DrawView(this);
	root->addWidget(view, 1);

	auto bottom = new QHBoxLayout;
	bottom->setSpacing(2);
	for (const QColor &c : swatches) {
		auto sw = new QToolButton;
		sw->setAutoRaise(true);
		sw->setIcon(dotIcon(c));
		sw->setToolTip(c.name());
		connect(sw, &QToolButton::clicked, this, [this, c] {
			brush().color = c;
			syncColor();
		});
		bottom->addWidget(sw);
	}
	colorBtn = new QToolButton;
	colorBtn->setAutoRaise(true);
	colorBtn->setToolTip(Str("CustomColor"));
	connect(colorBtn, &QToolButton::clicked, this, [this] {
		const QColor c = QColorDialog::getColor(brush().color, this, Str("CustomColor"));
		if (c.isValid()) {
			brush().color = c;
			syncColor();
		}
	});
	bottom->addWidget(colorBtn);
	presetBtn = new QToolButton;
	presetBtn->setAutoRaise(true);
	presetBtn->setToolTip(Str("Presets"));
	presetBtn->setPopupMode(QToolButton::InstantPopup);
	presetBtn->setMenu(new QMenu(presetBtn));
	connect(presetBtn->menu(), &QMenu::aboutToShow, this, &Panel::buildPresetMenu);
	bottom->addWidget(presetBtn);
	bottom->addSpacing(8);
	size = new QSlider(Qt::Horizontal);
	size->setRange(1, 64);
	size->setValue(int(b.size));
	size->setToolTip(Str("Size"));
	sizeLbl = new QLabel(QString::number(int(b.size)));
	sizeLbl->setMinimumWidth(22);
	connect(size, &QSlider::valueChanged, this, [this](int v) {
		brush().size = float(v);
		sizeLbl->setNum(v);
	});
	syncers.push_back([this] { size->setValue(int(brush().size)); });
	bottom->addWidget(size, 1);
	bottom->addWidget(sizeLbl);
	root->addLayout(bottom);

	updateIcons();
	for (auto &sync : syncers)
		sync();
	selectTool(b.tool);
	refreshTargets();
	for (auto s : sourceSignals)
		signal_handler_connect(obs_get_signal_handler(), s, queueRefresh, this);
	if (obs_data_get_bool(cfg, "window_mode"))
		QTimer::singleShot(0, this, [this] { setWindowMode(true); });
}

Panel::~Panel()
{
	for (auto s : sourceSignals)
		signal_handler_disconnect(obs_get_signal_handler(), s, queueRefresh, this);
	obs_data_t *cfg = config();
	writeBrush(cfg, brush());
	savePresets();
	obs_data_set_bool(cfg, "window_mode", window != nullptr);
	if (window)
		obs_data_set_string(cfg, "window_geom", window->saveGeometry().toBase64().constData());
	saveConfig();
}

void Panel::buildSettings()
{
	settings = new QWidget;
	settings->hide();
	auto form = new QFormLayout(settings);
	form->setContentsMargins(4, 0, 4, 0);
	auto slider = [&](const char *label, int lo, int hi, float (*val)(), void (*fn)(int)) {
		auto s = new QSlider(Qt::Horizontal);
		s->setRange(lo, hi);
		connect(s, &QSlider::valueChanged, this, fn);
		syncers.push_back([s, val] { s->setValue(int(val() * 100)); });
		form->addRow(Str(label), s);
	};
	slider("Opacity", 5, 100, [] { return brush().opacity; }, [](int v) { brush().opacity = v / 100.f; });
	slider("Smoothing", 0, 100, [] { return brush().smoothing; }, [](int v) { brush().smoothing = v / 100.f; });
	auto check = [&](const char *label, bool BrushState::*field) {
		auto c = new QCheckBox;
		connect(c, &QCheckBox::toggled, this, [field](bool v) { brush().*field = v; });
		syncers.push_back([c, field] { c->setChecked(brush().*field); });
		form->addRow(Str(label), c);
	};
	check("PenPressure", &BrushState::pressure);
	check("FillShapes", &BrushState::fill);
	auto style = new QComboBox;
	for (int i = 0; i < 3; i++)
		style->addItem(Str(("Style." + std::to_string(i)).c_str()));
	syncers.push_back([style] { style->setCurrentIndex(brush().style); });
	connect(style, &QComboBox::currentIndexChanged, this, [](int i) { brush().style = i; });
	form->addRow(Str("Style"), style);
	auto fade = new QDoubleSpinBox;
	fade->setRange(0, 30);
	fade->setSingleStep(.5);
	fade->setSuffix(" s");
	fade->setSpecialValueText(Str("Off"));
	syncers.push_back([fade] { fade->setValue(brush().fade); });
	connect(fade, &QDoubleSpinBox::valueChanged, this, [](double v) { brush().fade = float(v); });
	form->addRow(Str("FadeStrokes"), fade);
	auto stamp = new QComboBox;
	for (int i = 0; i < stampCount; i++)
		stamp->addItem(QString::fromUtf8(stamps[i]));
	syncers.push_back([stamp] { stamp->setCurrentIndex(brush().stamp % stampCount); });
	connect(stamp, &QComboBox::currentIndexChanged, this, [](int i) { brush().stamp = i; });
	form->addRow(Str("Tool.Stamp"), stamp);
	bgBox = new QComboBox;
	for (int i = 0; i < 4; i++)
		bgBox->addItem(Str(("Background." + std::to_string(i)).c_str()));
	connect(bgBox, &QComboBox::activated, this, [this](int i) {
		OBSSource o = getTarget();
		if (o && obs_source_get_type(o) == OBS_SOURCE_TYPE_INPUT) {
			OBSDataAutoRelease s = obs_data_create();
			obs_data_set_int(s, "background", i);
			obs_source_update(o, s);
		}
	});
	form->addRow(Str("Background"), bgBox);

	auto remote = new QHBoxLayout;
	remoteOn = new QCheckBox;
	remoteOn->setChecked(Remote::get().enabled());
	remoteUrl = new QLineEdit(Remote::get().url());
	remoteUrl->setReadOnly(true);
	remoteUrl->setEnabled(remoteOn->isChecked());
	connect(remoteOn, &QCheckBox::toggled, this, [this](bool on) {
		Remote::get().setEnabled(on);
		remoteUrl->setText(Remote::get().url());
		remoteUrl->setEnabled(on);
	});
	remote->addWidget(remoteOn);
	remote->addWidget(remoteUrl, 1);
	auto remoteBtn = [&](const char *tip, auto fn) {
		auto btn = new QToolButton;
		btn->setAutoRaise(true);
		btn->setToolTip(Str(tip));
		connect(btn, &QToolButton::clicked, this, fn);
		remote->addWidget(btn);
		return btn;
	};
	copyBtn = remoteBtn("CopyLink", [this] { QApplication::clipboard()->setText(remoteUrl->text()); });
	regenBtn = remoteBtn("NewLink", [this] {
		Remote::get().regenToken();
		remoteUrl->setText(Remote::get().url());
	});
	form->addRow(Str("RemoteDrawing"), remote);

	auto canvasRow = new QHBoxLayout;
	auto push = [&](const char *label, auto fn) {
		auto p = new QPushButton(Str(label));
		connect(p, &QPushButton::clicked, this, fn);
		canvasRow->addWidget(p);
	};
	push("Import", [this] {
		const QString f = QFileDialog::getOpenFileName(this, Str("Import"), {},
							       "Images (*.png *.jpg *.jpeg *.bmp *.gif *.webp)");
		const QImage im(f);
		withCanvas([&](Canvas *c) { c->importImage(im.convertToFormat(QImage::Format_ARGB32_Premultiplied)); });
	});
	push("Export", [this] {
		const QString f = QFileDialog::getSaveFileName(this, Str("Export"), "sketch.png", "PNG (*.png)");
		if (!f.isEmpty())
			withCanvas([&](Canvas *c) { c->exportImage().save(f, "PNG"); });
	});
	form->addRow(Str("Canvas"), canvasRow);
	root->addWidget(settings);
}

void Panel::updateIcons()
{
	const QColor c = palette().color(QPalette::ButtonText);
	for (int i = 0; i < ToolCount; i++)
		toolActs[i]->setIcon(svgIcon(tools[i].icon, c));
	moreAct->setIcon(svgIcon(MoreIcon, c));
	undoBtn->setIcon(svgIcon(UndoIcon, c));
	redoBtn->setIcon(svgIcon(RedoIcon, c));
	clearBtn->setIcon(svgIcon(ClearIcon, c));
	popBtn->setIcon(svgIcon(window ? DockIcon : PopIcon, c));
	popBtn->setToolTip(Str(window ? "Dock" : "PopOut"));
	setBtn->setIcon(svgIcon(SettingsIcon, c));
	presetBtn->setIcon(svgIcon(PresetIcon, c));
	copyBtn->setIcon(svgIcon(CopyIcon, c));
	regenBtn->setIcon(svgIcon(RegenIcon, c));
	syncColor();
}

void Panel::changeEvent(QEvent *e)
{
	QWidget::changeEvent(e);
	if (e->type() == QEvent::PaletteChange)
		updateIcons();
}

void Panel::syncColor()
{
	if (colorBtn)
		colorBtn->setIcon(dotIcon(brush().color, true));
}

void Panel::selectTool(int t)
{
	if (t == Image) {
		const QString f = QFileDialog::getOpenFileName(this, Str("ChooseImage"), {},
							       "Images (*.png *.jpg *.jpeg *.bmp *.gif *.webp)");
		const QImage im(f);
		if (im.isNull()) {
			toolActs[brush().tool]->setChecked(true);
			return;
		}
		brush().image = im.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	}
	brush().tool = t;
	toolActs[t]->setChecked(true);
	if (t >= PrimaryTools && !more)
		moreAct->trigger();
}

void Panel::withCanvas(const std::function<void(Canvas *)> &f)
{
	OBSSource o = getTarget();
	if (Canvas *c = canvasOf(o))
		f(c);
}

void Panel::action(int a)
{
	if (a < ToolCount)
		return selectTool(a);
	if (a >= FirstPreset)
		return applyPreset(a - FirstPreset);
	withCanvas([a](Canvas *c) {
		if (a == Undo)
			c->undo();
		else if (a == Redo)
			c->redo();
		else
			c->clear();
	});
}

void Panel::loadPresets()
{
	OBSDataArrayAutoRelease arr = obs_data_get_array(config(), "presets");
	if (!arr) { // first run: a few useful defaults
		auto add = [this](const char *name, int tool, QRgb color, float size, int style = Solid) {
			BrushState b;
			b.tool = tool;
			b.color = QColor::fromRgb(color);
			b.size = size;
			b.style = style;
			presets.push_back({Str(name), b});
		};
		add("Preset.Fineliner", Pen, 0xededed, 2);
		add("Preset.Marker", Pen, 0xffb86b, 10);
		add("Preset.Highlight", Highlighter, 0xffe55c, 10);
		add("Preset.Neon", Pen, 0x7cf2c8, 6, Glow);
		add("Preset.Dashed", Pen, 0xff7ab6, 4, Dashed);
		add("Preset.Laser", Laser, 0xff5a5f, 6);
		return;
	}
	for (size_t i = 0; i < obs_data_array_count(arr); i++) {
		OBSDataAutoRelease d = obs_data_array_item(arr, i);
		BrushState b;
		readBrush(d, b);
		presets.push_back({QString::fromUtf8(obs_data_get_string(d, "name")), b});
	}
}

void Panel::savePresets()
{
	OBSDataArrayAutoRelease arr = obs_data_array_create();
	for (auto &[name, b] : presets) {
		OBSDataAutoRelease d = obs_data_create();
		obs_data_set_string(d, "name", name.toUtf8().constData());
		writeBrush(d, b);
		obs_data_array_push_back(arr, d);
	}
	obs_data_set_array(config(), "presets", arr);
	saveConfig();
}

void Panel::applyPreset(int i)
{
	if (i < 0 || i >= int(presets.size()))
		return;
	const QImage image = brush().image;
	brush() = presets[size_t(i)].second;
	brush().image = image;
	for (auto &sync : syncers)
		sync();
	syncColor();
	selectTool(brush().tool);
}

void Panel::buildPresetMenu()
{
	QMenu *m = presetBtn->menu();
	m->clear();
	for (int i = 0; i < int(presets.size()); i++) {
		auto &[name, b] = presets[size_t(i)];
		m->addAction(dotIcon(b.color), name, this, [this, i] { applyPreset(i); });
	}
	m->addSeparator();
	m->addAction(Str("SavePreset"), this, [this] {
		bool ok = false;
		const QString name =
			QInputDialog::getText(this, Str("SavePreset"), Str("PresetName"), QLineEdit::Normal,
					      Str(("Tool." + std::string(tools[brush().tool].id)).c_str()), &ok)
				.trimmed();
		if (!ok || name.isEmpty())
			return;
		BrushState b = brush();
		b.image = {};
		auto it = std::find_if(presets.begin(), presets.end(), [&](auto &p) { return p.first == name; });
		if (it != presets.end())
			it->second = b;
		else
			presets.push_back({name, b});
		savePresets();
	});
	QMenu *del = m->addMenu(Str("DeletePreset"));
	del->setEnabled(!presets.empty());
	for (int i = 0; i < int(presets.size()); i++)
		del->addAction(presets[size_t(i)].first, this, [this, i] {
			presets.erase(presets.begin() + i);
			savePresets();
		});
}

void Panel::refreshTargets()
{
	QString cur = target->currentData().toString();
	if (cur.isEmpty())
		cur = obs_data_get_string(config(), "target");
	target->blockSignals(true);
	target->clear();
	forEachCanvas([this](obs_source_t *s) {
		const bool isFilter = obs_source_get_type(s) == OBS_SOURCE_TYPE_FILTER;
		obs_source_t *parent = renderSourceOf(s);
		if (!parent)
			return;
		const QString key =
			QString(obs_source_get_uuid(parent)) + "/" + (isFilter ? obs_source_get_name(s) : "");
		target->addItem(isFilter ? QString("%1 ▸ %2").arg(obs_source_get_name(parent), obs_source_get_name(s))
					 : QString(obs_source_get_name(s)),
				key);
	});
	if (!target->count())
		target->addItem(Str("NoTarget"), QString());
	target->setCurrentIndex(std::max(0, target->findData(cur)));
	target->blockSignals(false);
	targetChanged();
}

void Panel::targetChanged()
{
	const QString key = target->currentData().toString();
	const int slash = key.indexOf('/');
	OBSSourceAutoRelease src = slash > 0 ? obs_get_source_by_uuid(key.left(slash).toUtf8().constData()) : nullptr;
	const QByteArray filter = key.mid(slash + 1).toUtf8();
	OBSSourceAutoRelease owner = src && !filter.isEmpty() ? obs_source_get_filter_by_name(src, filter.constData())
							      : obs_source_get_ref(src);
	setTarget(owner);
	if (!key.isEmpty())
		obs_data_set_string(config(), "target", key.toUtf8().constData());
	const bool isSource = owner && obs_source_get_type(owner) == OBS_SOURCE_TYPE_INPUT;
	bgBox->setEnabled(isSource);
	if (isSource) {
		OBSDataAutoRelease s = obs_source_get_settings(owner);
		bgBox->setCurrentIndex(int(obs_data_get_int(s, "background")));
	}
}

void Panel::setWindowMode(bool on)
{
	if (on == (window != nullptr))
		return;
	if (on) {
		window = new QWidget(static_cast<QWidget *>(obs_frontend_get_main_window()), Qt::Window);
		window->setWindowTitle(Str("Sketch"));
		auto l = new QVBoxLayout(window);
		l->setContentsMargins(0, 0, 0, 0);
		l->addWidget(this);
		root->removeWidget(toolbar);
		top->insertWidget(2, toolbar);
		sep->show();
		if (!window->restoreGeometry(QByteArray::fromBase64(obs_data_get_string(config(), "window_geom"))))
			window->resize(1100, 720);
		window->installEventFilter(this);
		window->show();
	} else {
		obs_data_set_string(config(), "window_geom", window->saveGeometry().toBase64().constData());
		top->removeWidget(toolbar);
		root->insertWidget(1, toolbar);
		sep->hide();
		dockHost->layout()->addWidget(this);
		window->removeEventFilter(this);
		window->deleteLater();
		window = nullptr;
	}
	updateIcons();
}

bool Panel::eventFilter(QObject *o, QEvent *e)
{
	if (o == window && e->type() == QEvent::Close)
		setWindowMode(false);
	else if (o == target && e->type() == QEvent::MouseButtonPress)
		refreshTargets();
	return QWidget::eventFilter(o, e);
}
