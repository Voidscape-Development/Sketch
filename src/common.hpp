#pragma once
#include <obs-module.h>
#include <obs.hpp>
#include <QColor>
#include <QImage>
#include <QString>
#include <functional>

inline const char *Str(const char *key)
{
	return obs_module_text(key);
}

enum Tool {
	Pen,
	Brush,
	Highlighter,
	Eraser,
	Line,
	Arrow,
	Rect,
	Ellipse,
	Text,
	Stamp,
	Fill,
	Blur,
	Pixelate,
	Laser,
	Image,
	ToolCount,
	Clear = ToolCount
};
enum Style { Solid, Dashed, Glow };
constexpr int PrimaryTools = Stamp; // tools before this are always visible

struct ToolInfo {
	const char *id, *icon;
};
extern const ToolInfo tools[ToolCount];
extern const char *const stamps[];
extern const int stampCount;

struct BrushState {
	int tool = Pen, style = Solid, stamp = 0;
	QColor color{0x8e, 0xa6, 0xff};
	float size = 6, opacity = 1, smoothing = .5f, fade = 0;
	bool pressure = true, fill = false;
	QImage image; // Image tool
};
BrushState &brush();

class Canvas;
Canvas *canvasOf(obs_source_t *owner);             // owner = Sketch source or Sketch filter
obs_source_t *renderSourceOf(obs_source_t *owner); // what to show (filter -> parent), no ref
void forEachCanvas(const std::function<void(obs_source_t *)> &cb);

void setTarget(obs_source_t *owner);
OBSSource getTarget();

obs_data_t *config();
void saveConfig();
void registerSources();
