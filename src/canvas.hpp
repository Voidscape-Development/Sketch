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
	double end = 0;    // fade-out time for fading strokes
	int owner = 0;     // pointer id that drew it (0 = host)
	QPointF off{};     // Track: how far the subject moved
	QByteArray tmpl{}; // Track: grayscale template at tracking resolution
	QRect trect{};     // Track: template area relative to the shape's top-left, at tracking resolution
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
	void cancel(int id, bool leave = false); // drop the live stroke; leave also removes the name tag
	void tag(int id, QPointF p, const QString &name, QColor c);
	void undo(int id = 0); // id > 0: only that guest's strokes
	void redo(int id = 0);
	void clear(int id = 0);
	void importImage(const QImage &img);
	QImage exportImage();
	void resize(int w, int h);
	void load(obs_data_t *s);
	void save(obs_data_t *s);

	// Graphics thread
	void draw();
	bool hasRegions();
	void drawRegions(gs_texture_t *src);
	bool hasTracks();
	void track(gs_texture_t *src); // follow tracked subjects in the source frame
	gs_texrender_t *texrender = nullptr;

private:
	void apply(QImage &dst, const Op &o, QVector<Region> *regions) const;
	void commit(const Op &o, bool keepRedo = false);
	void rebuild();
	void resetBase();
	void compose(double now);
	int nextStep() const;
	std::vector<Op> activeTracks() const;

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
	struct Tag {
		QPointF p;
		QString name;
		QColor c;
		double t;
	};
	std::map<int, Tag> tags;
	std::vector<Op> pendingTracks;
	int laserStroke = 0, stepBase = 0, pendingStep = 0;
	bool dirty = true;
	gs_texture_t *tex = nullptr;
	gs_effect_t *fx = nullptr;
	gs_texrender_t *trackRender = nullptr;
	gs_stagesurf_t *trackStage = nullptr;
	bool trackStaged = false;
	uint64_t trackFrame = 0;
};
