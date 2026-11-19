#include "canvas.hpp"
#include <util/platform.h>
#include <QBuffer>
#include <QLineF>
#include <QPainter>
#include <QPainterPath>

static double now()
{
	return os_gettime_ns() / 1e9;
}
static bool isStroke(int t)
{
	return t <= Ellipse;
}
static bool isFreehand(int t)
{
	return t <= Eraser;
}
static const double laserLife = 1.0;

static void floodFill(QImage &img, QPoint s, QColor c)
{
	if (!img.rect().contains(s))
		return;
	const int W = img.width(), H = img.height();
	auto *px = reinterpret_cast<QRgb *>(img.bits());
	const QRgb t = px[s.y() * W + s.x()], f = qPremultiply(c.rgba());
	auto near = [&](QRgb q) {
		return qAbs(qAlpha(q) - qAlpha(t)) < 40 && qAbs(qRed(q) - qRed(t)) < 40 &&
		       qAbs(qGreen(q) - qGreen(t)) < 40 && qAbs(qBlue(q) - qBlue(t)) < 40;
	};
	std::vector<uchar> seen(size_t(W) * H);
	std::vector<QPoint> st{s};
	while (!st.empty()) {
		const QPoint q = st.back();
		st.pop_back();
		const int y = q.y(), row = y * W;
		if (seen[row + q.x()])
			continue;
		int l = q.x(), r = q.x();
		while (l > 0 && !seen[row + l - 1] && near(px[row + l - 1]))
			l--;
		while (r < W - 1 && !seen[row + r + 1] && near(px[row + r + 1]))
			r++;
		for (int x = l; x <= r; x++) {
			seen[row + x] = 1;
			px[row + x] = f;
			for (int ny : {y - 1, y + 1})
				if (ny >= 0 && ny < H && !seen[ny * W + x] && near(px[ny * W + x]))
					st.push_back({x, ny});
		}
	}
}

static void strokeOp(QPainter &p, const Op &o)
{
	const BrushState &b = o.b;
	QColor c = b.color;
	c.setAlpha(255);
	const float w = b.tool == Highlighter ? b.size * 2.5f : b.size;
	QPen pen(c, w, b.style == Dashed ? Qt::DashLine : Qt::SolidLine,
		 b.tool == Highlighter ? Qt::FlatCap : Qt::RoundCap, Qt::RoundJoin);
	const QPointF a = o.pts.first(), z = o.pts.last();
	QPainterPath path;
	switch (b.tool) {
	case Line:
	case Arrow:
		path.moveTo(a);
		path.lineTo(z);
		if (b.tool == Arrow && a != z) {
			const QLineF back(z, a);
			for (int s : {-28, 28}) {
				QLineF head = QLineF::fromPolar(qMax(12.f, w * 4), back.angle() + s).translated(z);
				path.moveTo(z);
				path.lineTo(head.p2());
			}
		}
		break;
	case Rect:
		path.addRect(QRectF(a, z).normalized());
		break;
	case Ellipse:
		path.addEllipse(QRectF(a, z).normalized());
		break;
	default:
		if (o.pts.size() == 1) {
			p.setPen(pen);
			p.drawPoint(a);
			return;
		}
		if ((b.tool == Brush || (b.pressure && b.tool != Highlighter)) && b.style == Solid) {
			for (int i = 1; i < o.pts.size(); i++) {
				pen.setWidthF(qMax(.5f, w * (.15f + .85f * o.pr[i])));
				p.setPen(pen);
				p.drawLine(o.pts[i - 1], o.pts[i]);
			}
			return;
		}
		path.moveTo(a);
		for (int i = 1; i < o.pts.size(); i++)
			path.quadTo(o.pts[i - 1], (o.pts[i - 1] + o.pts[i]) / 2);
		path.lineTo(z);
	}
	if (b.fill && (b.tool == Rect || b.tool == Ellipse))
		p.fillPath(path, c);
	if (b.style == Glow) {
		QPen g = pen;
		g.setColor(QColor(c.red(), c.green(), c.blue(), 90));
		g.setWidthF(w * 3);
		p.setPen(g);
		p.drawPath(path);
		pen.setColor(c.lighter(170));
	}
	p.setPen(pen);
	p.drawPath(path);
}

void Canvas::apply(QImage &dst, const Op &o, QVector<Region> *rg) const
{
	const BrushState &b = o.b;
	QPainter p(&dst);
	p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform | QPainter::TextAntialiasing);
	if (b.tool == Clear) {
		p.setCompositionMode(QPainter::CompositionMode_Clear);
		p.fillRect(dst.rect(), Qt::transparent);
		if (rg)
			rg->clear();
		return;
	}
	if (o.pts.isEmpty())
		return;
	const QPointF a = o.pts.first(), z = o.pts.last();
	const QRectF box = QRectF(a, z).normalized();
	p.setOpacity(b.opacity);
	switch (b.tool) {
	case Text:
	case Stamp: {
		QFont f;
		f.setBold(true);
		f.setPixelSize(int(b.size * (b.tool == Text ? 4 : 8)));
		p.setFont(f);
		p.setPen(b.color);
		if (b.tool == Text)
			p.drawText(a, o.text);
		else
			p.drawText(QRectF(a.x() - b.size * 8, a.y() - b.size * 8, b.size * 16, b.size * 16),
				   Qt::AlignCenter, QString::fromUtf8(stamps[b.stamp % stampCount]));
		return;
	}
	case Image:
		if (!b.image.isNull())
			p.drawImage(box.width() < 4 ? QRectF(a, QSizeF(b.image.size())) : box, b.image);
		return;
	case Fill: {
		p.end();
		QColor c = b.color;
		c.setAlphaF(b.opacity);
		floodFill(dst, a.toPoint(), c);
		return;
	}
	case Blur:
	case Pixelate: {
		const QRect r = box.toRect() & dst.rect();
		if (r.width() < 2 || r.height() < 2)
			return;
		if (filter) { // the filter blurs the source underneath on the GPU
			if (rg)
				rg->push_back({b.tool, r, b.size});
			else {
				p.setOpacity(1);
				p.setPen(QPen(Qt::white, 2, Qt::DashLine));
				p.drawRect(r);
			}
			return;
		}
		const auto mode = b.tool == Blur ? Qt::SmoothTransformation : Qt::FastTransformation;
		const int k = qMax(2, int(b.size));
		const QImage s =
			dst.copy(r)
				.scaled(qMax(1, r.width() / k), qMax(1, r.height() / k), Qt::IgnoreAspectRatio, mode)
				.scaled(r.size(), Qt::IgnoreAspectRatio, mode);
		p.setOpacity(1);
		p.setCompositionMode(QPainter::CompositionMode_Source);
		p.drawImage(r.topLeft(), s);
		return;
	}
	default:
		break;
	}
	// Strokes render opaque on a layer first so overlapping segments don't darken
	QImage layer(dst.size(), QImage::Format_ARGB32_Premultiplied);
	layer.fill(Qt::transparent);
	{
		QPainter lp(&layer);
		lp.setRenderHint(QPainter::Antialiasing);
		strokeOp(lp, o);
	}
	if (b.tool == Highlighter)
		p.setOpacity(b.opacity * .45);
	if (b.tool == Eraser)
		p.setCompositionMode(QPainter::CompositionMode_DestinationOut);
	p.drawImage(0, 0, layer);
}

Canvas::~Canvas()
{
	obs_enter_graphics();
	gs_texture_destroy(tex);
	gs_effect_destroy(fx);
	gs_texrender_destroy(texrender);
	obs_leave_graphics();
}

void Canvas::commit(const Op &o, bool keepRedo)
{
	if (!keepRedo)
		redos.clear();
	ops.push_back(o);
	apply(img, o, &regions);
	if (ops.size() > 64) { // bake the oldest step so undo history stays bounded
		apply(base, ops.front(), &baseRegions);
		ops.erase(ops.begin());
	}
	dirty = true;
}

void Canvas::rebuild()
{
	img = base.copy();
	regions = baseRegions;
	for (auto &o : ops)
		apply(img, o, &regions);
	dirty = true;
}

void Canvas::down(int id, QPointF p, float pr, const BrushState &b, const QString &text)
{
	std::lock_guard l(m);
	if (img.isNull())
		return;
	Op o{b, {p}, {pr}, text};
	if (b.tool == Text || b.tool == Stamp || b.tool == Fill) {
		if (b.tool != Text || !text.isEmpty())
			commit(o);
		return;
	}
	if (b.tool == Laser)
		laser.push_back({p, now(), ++laserStroke, b.color});
	live[id] = o;
	dirty = true;
}

void Canvas::move(int id, QPointF p, float pr)
{
	std::lock_guard l(m);
	auto it = live.find(id);
	if (it == live.end())
		return;
	Op &o = it->second;
	if (o.b.tool == Laser)
		laser.push_back({p, now(), laserStroke, o.b.color});
	else if (isFreehand(o.b.tool)) {
		const QPointF last = o.pts.last();
		o.pts.push_back(last + (p - last) * (1 - o.b.smoothing * .85));
		o.pr.push_back(pr);
	} else {
		o.pts.resize(1);
		o.pts.push_back(p);
	}
	dirty = true;
}

void Canvas::up(int id)
{
	std::lock_guard l(m);
	auto it = live.find(id);
	if (it == live.end())
		return;
	Op o = it->second;
	live.erase(it);
	dirty = true;
	if (o.b.tool == Laser)
		return;
	if (o.b.fade > 0 && isStroke(o.b.tool)) {
		o.end = now() + o.b.fade;
		fading.push_back(o);
	} else
		commit(o);
}

void Canvas::undo()
{
	std::lock_guard l(m);
	if (ops.empty())
		return;
	redos.push_back(ops.back());
	ops.pop_back();
	rebuild();
}

void Canvas::redo()
{
	std::lock_guard l(m);
	if (redos.empty())
		return;
	Op o = redos.back();
	redos.pop_back();
	commit(o, true);
}

void Canvas::clear()
{
	std::lock_guard l(m);
	Op o;
	o.b.tool = Clear;
	fading.clear();
	laser.clear();
	commit(o);
}

void Canvas::importImage(const QImage &im)
{
	std::lock_guard l(m);
	if (img.isNull() || im.isNull())
		return;
	Op o;
	o.b.tool = Image;
	o.b.image = im.size().boundedTo(img.size()) == im.size()
			    ? im
			    : im.scaled(img.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
	const QPointF tl((w - o.b.image.width()) / 2.0, (h - o.b.image.height()) / 2.0);
	o.pts = {tl, tl + QPointF(o.b.image.width(), o.b.image.height())};
	commit(o);
}

QImage Canvas::exportImage()
{
	std::lock_guard l(m);
	return img.copy();
}

void Canvas::resize(int nw, int nh)
{
	std::lock_guard l(m);
	if (nw == w && nh == h)
		return;
	w = nw;
	h = nh;
	resetBase();
}

void Canvas::resetBase()
{
	QImage n(w, h, QImage::Format_ARGB32_Premultiplied);
	n.fill(Qt::transparent);
	QPainter p(&n);
	if (!img.isNull())
		p.drawImage(0, 0, img);
	if (!pending.isNull())
		p.drawImage(0, 0, pending);
	p.end();
	base = n;
	baseRegions = regions + pendingRegions;
	pending = {};
	pendingRegions.clear();
	ops.clear();
	redos.clear();
	rebuild();
}

void Canvas::save(obs_data_t *s)
{
	std::lock_guard l(m);
	if (!keep || img.isNull()) {
		obs_data_erase(s, "sketch_png");
		obs_data_erase(s, "sketch_regions");
		return;
	}
	QByteArray png;
	QBuffer buf(&png);
	img.save(&buf, "PNG");
	obs_data_set_string(s, "sketch_png", png.toBase64().constData());
	OBSDataArrayAutoRelease arr = obs_data_array_create();
	for (auto &r : regions) {
		OBSDataAutoRelease d = obs_data_create();
		obs_data_set_int(d, "t", r.tool);
		obs_data_set_int(d, "x", r.r.x());
		obs_data_set_int(d, "y", r.r.y());
		obs_data_set_int(d, "w", r.r.width());
		obs_data_set_int(d, "h", r.r.height());
		obs_data_set_double(d, "a", r.amount);
		obs_data_array_push_back(arr, d);
	}
	obs_data_set_array(s, "sketch_regions", arr);
}

void Canvas::load(obs_data_t *s)
{
	std::lock_guard l(m);
	pending = QImage::fromData(QByteArray::fromBase64(obs_data_get_string(s, "sketch_png")), "PNG")
			  .convertToFormat(QImage::Format_ARGB32_Premultiplied);
	OBSDataArrayAutoRelease arr = obs_data_get_array(s, "sketch_regions");
	for (size_t i = 0; i < obs_data_array_count(arr); i++) {
		OBSDataAutoRelease d = obs_data_array_item(arr, i);
		pendingRegions.push_back({int(obs_data_get_int(d, "t")),
					  QRect(int(obs_data_get_int(d, "x")), int(obs_data_get_int(d, "y")),
						int(obs_data_get_int(d, "w")), int(obs_data_get_int(d, "h"))),
					  float(obs_data_get_double(d, "a"))});
	}
	if (w && h)
		resetBase();
}

void Canvas::compose(double t)
{
	static const QColor bgs[] = {Qt::transparent, QColor(0xf4, 0xf4, 0xf0), QColor(0x1f, 0x3a, 0x2e),
				     QColor(0x16, 0x18, 0x1d)};
	if (comp.size() != img.size())
		comp = QImage(img.size(), QImage::Format_ARGB32_Premultiplied);
	comp.fill(filter ? QColor(Qt::transparent) : bgs[qBound(0, bg, 3)]);
	QPainter p(&comp);
	p.setRenderHint(QPainter::Antialiasing);
	if (!filter && bg == 3) {
		p.setPen(QColor(0x2a, 0x2c, 0x31));
		for (int x = 40; x < w; x += 40)
			p.drawLine(x, 0, x, h);
		for (int y = 40; y < h; y += 40)
			p.drawLine(0, y, w, y);
	}
	QImage work = img;
	for (auto &[id, o] : live)
		if (o.b.tool != Laser)
			apply(work, o, nullptr);
	p.drawImage(0, 0, work);
	for (auto &o : fading) {
		QImage layer(img.size(), QImage::Format_ARGB32_Premultiplied);
		layer.fill(Qt::transparent);
		apply(layer, o, nullptr);
		p.setOpacity(qBound(0.0, (o.end - t) / .5, 1.0));
		p.drawImage(0, 0, layer);
	}
	for (size_t i = 1; i < laser.size(); i++) {
		if (laser[i].stroke != laser[i - 1].stroke)
			continue;
		const double a = qBound(0.0, 1 - (t - laser[i].t) / laserLife, 1.0);
		QColor c = laser[i].c;
		for (auto [width, alpha] : {std::pair{14.f, .3}, std::pair{4.f, 1.0}}) {
			c.setAlphaF(a * alpha);
			p.setPen(QPen(alpha < 1 ? c : QColor(255, 255, 255, int(a * 255)), width, Qt::SolidLine,
				      Qt::RoundCap));
			p.drawLine(laser[i - 1].p, laser[i].p);
		}
	}
}

void Canvas::draw()
{
	std::lock_guard l(m);
	if (img.isNull())
		return;
	const double t = now();
	const size_t nf = fading.size(), nl = laser.size();
	std::erase_if(fading, [t](const Op &o) { return o.end <= t; });
	std::erase_if(laser, [t](const LaserPt &p) { return t - p.t > laserLife; });
	const bool anim = !fading.empty() || !laser.empty();
	if (dirty || anim || nf != fading.size() || nl != laser.size()) {
		compose(t);
		if (tex && (gs_texture_get_width(tex) != uint32_t(w) || gs_texture_get_height(tex) != uint32_t(h))) {
			gs_texture_destroy(tex);
			tex = nullptr;
		}
		if (!tex)
			tex = gs_texture_create(w, h, GS_BGRA, 1, nullptr, GS_DYNAMIC);
		gs_texture_set_image(tex, comp.constBits(), comp.bytesPerLine(), false);
		dirty = false;
	}
	gs_effect_t *e = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	gs_effect_set_texture(gs_effect_get_param_by_name(e, "image"), tex);
	while (gs_effect_loop(e, "Draw"))
		gs_draw_sprite(tex, 0, w, h);
	gs_blend_state_pop();
}

bool Canvas::hasRegions()
{
	std::lock_guard l(m);
	return !regions.isEmpty();
}

void Canvas::drawRegions(gs_texture_t *src)
{
	std::lock_guard l(m);
	if (!fx) {
		char *f = obs_module_file("regions.effect");
		fx = gs_effect_create_from_file(f, nullptr);
		bfree(f);
	}
	if (!fx)
		return;
	vec2 texel;
	vec2_set(&texel, 1.f / w, 1.f / h);
	for (auto &r : regions) {
		gs_effect_set_texture(gs_effect_get_param_by_name(fx, "image"), src);
		gs_effect_set_vec2(gs_effect_get_param_by_name(fx, "texel"), &texel);
		gs_effect_set_float(gs_effect_get_param_by_name(fx, "amount"), qMax(2.f, r.amount));
		gs_matrix_push();
		gs_matrix_translate3f(float(r.r.x()), float(r.r.y()), 0);
		while (gs_effect_loop(fx, r.tool == Blur ? "Blur" : "Pixelate"))
			gs_draw_sprite_subregion(src, 0, r.r.x(), r.r.y(), r.r.width(), r.r.height());
		gs_matrix_pop();
	}
}
