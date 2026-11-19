#pragma once
#include "common.hpp"
#include <QPointF>
#include <QRect>
#include <QVector>
#include <map>
#include <mutex>
#include <vector>

struct Op {
	BrushState b;
	QVector<QPointF> pts;
	QVector<float> pr;
	QString text;
	double end = 0; // fade-out time for fading strokes
};

struct Region {
	int tool;
	QRect r;
	float amount;
};

class Canvas {
public:
	Canvas(obs_source_t *self, bool filter) : self(self), filter(filter) {}
	~Canvas();

	obs_source_t *self;
	const bool filter;
	int bg = 0, w = 0, h = 0;
	bool keep = true, clearOnSwitch = false;

	// Input; id separates simultaneous pointers (0 = panel, others = remote clients)
	void down(int id, QPointF p, float pr, const BrushState &b, const QString &text = {});
	void move(int id, QPointF p, float pr);
	void up(int id);
	void undo();
	void redo();
	void clear();
	void importImage(const QImage &img);
	QImage exportImage();
	void resize(int w, int h);
	void load(obs_data_t *s);
	void save(obs_data_t *s);

	// Graphics thread
	void draw();
	bool hasRegions();
	void drawRegions(gs_texture_t *src);
	gs_texrender_t *texrender = nullptr;

private:
	void apply(QImage &dst, const Op &o, QVector<Region> *regions) const;
	void commit(const Op &o, bool keepRedo = false);
	void rebuild();
	void resetBase();
	void compose(double now);

	struct LaserPt {
		QPointF p;
		double t;
		int stroke;
		QColor c;
	};
	std::mutex m;
	QImage base, img, comp, pending;
	QVector<Region> baseRegions, regions, pendingRegions;
	std::vector<Op> ops, redos, fading;
	std::map<int, Op> live;
	std::vector<LaserPt> laser;
	int laserStroke = 0;
	bool dirty = true;
	gs_texture_t *tex = nullptr;
	gs_effect_t *fx = nullptr;
};
