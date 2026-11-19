#include "canvas.hpp"
#include <util/platform.h>
#include <cstring>
#include <mutex>
#include <string>

const ToolInfo tools[ToolCount] = {
	{"Pen", "M4 20l4-1 11-11-3-3L5 16z"},
	{"Brush", "M9 15c-3 0-4 2-4 5 3 0 5-1 5-4z M10 14l9-9 1 1-9 9"},
	{"Highlighter", "M6 18l2-6 8-8 4 4-8 8-6 2z"},
	{"Eraser", "M8 20l-4-4 10-10 6 6-8 8z M4 20h16"},
	{"Line", "M5 19L19 5"},
	{"Arrow", "M5 19L19 5 M10 5h9v9"},
	{"Rectangle", "M4 6h16v12H4z"},
	{"Ellipse", "M12 5c4.4 0 8 3.1 8 7s-3.6 7-8 7-8-3.1-8-7 3.6-7 8-7z"},
	{"Text", "M5 6V4h14v2 M12 4v16"},
	{"Stamp", "M12 3l2.6 5.6 6 .7-4.5 4.1 1.2 6L12 16.4 6.7 19.4l1.2-6L3.4 9.3l6-.7z"},
	{"Fill", "M5 12l7-7 7 7-7 7z M19 15c0 2 2 3 2 5a2 2 0 01-4 0c0-2 2-3 2-5z"},
	{"Blur", "M12 3c4 5 6 8 6 11a6 6 0 01-12 0c0-3 2-6 6-11z"},
	{"Pixelate", "M4 4h6v6H4z M14 4h6v6h-6z M4 14h6v6H4z M14 14h6v6h-6z"},
	{"Laser", "M9 12a3 3 0 106 0 3 3 0 10-6 0 M12 2v3 M12 19v3 M2 12h3 M19 12h3"},
	{"Image", "M4 5h16v14H4z M4 16l5-5 4 4 3-3 4 4"},
};
const char *const stamps[] = {"★", "✔", "✖", "♥", "?", "!", "➜", "●"};
const int stampCount = int(std::size(stamps));

BrushState &brush()
{
	static BrushState b;
	return b;
}

void readBrush(obs_data_t *d, BrushState &b)
{
	b.color = QColor::fromRgb(QRgb(obs_data_get_int(d, "color")));
	b.tool = int(obs_data_get_int(d, "tool")) % ToolCount;
	b.style = int(obs_data_get_int(d, "style"));
	b.stamp = int(obs_data_get_int(d, "stamp"));
	b.size = float(obs_data_get_double(d, "size"));
	b.opacity = float(obs_data_get_double(d, "opacity"));
	b.smoothing = float(obs_data_get_double(d, "smoothing"));
	b.fade = float(obs_data_get_double(d, "fade"));
	b.pressure = obs_data_get_bool(d, "pressure");
	b.fill = obs_data_get_bool(d, "fill");
	if (b.tool == Image)
		b.tool = Pen;
}

void writeBrush(obs_data_t *d, const BrushState &b)
{
	obs_data_set_int(d, "color", b.color.rgb());
	obs_data_set_int(d, "tool", b.tool);
	obs_data_set_int(d, "style", b.style);
	obs_data_set_int(d, "stamp", b.stamp);
	obs_data_set_double(d, "size", b.size);
	obs_data_set_double(d, "opacity", b.opacity);
	obs_data_set_double(d, "smoothing", b.smoothing);
	obs_data_set_double(d, "fade", b.fade);
	obs_data_set_bool(d, "pressure", b.pressure);
	obs_data_set_bool(d, "fill", b.fill);
}

static const char *SourceId = "sketch_source", *FilterId = "sketch_filter";

Canvas *canvasOf(obs_source_t *s)
{
	const char *id = s ? obs_source_get_unversioned_id(s) : nullptr;
	return id && (!strcmp(id, SourceId) || !strcmp(id, FilterId)) ? static_cast<Canvas *>(obs_obj_get_data(s))
								      : nullptr;
}

obs_source_t *renderSourceOf(obs_source_t *owner)
{
	return obs_source_get_type(owner) == OBS_SOURCE_TYPE_FILTER ? obs_filter_get_parent(owner) : owner;
}

void forEachCanvas(const std::function<void(obs_source_t *)> &cb)
{
	auto visit = [](void *p, obs_source_t *s) {
		auto &f = *static_cast<const std::function<void(obs_source_t *)> *>(p);
		if (canvasOf(s))
			f(s);
		obs_source_enum_filters(
			s,
			[](obs_source_t *, obs_source_t *fl, void *p2) {
				if (canvasOf(fl))
					(*static_cast<const std::function<void(obs_source_t *)> *>(p2))(fl);
			},
			p);
		return true;
	};
	obs_enum_sources(visit, (void *)&cb);
	obs_enum_scenes(visit, (void *)&cb);
}

static std::mutex targetMutex;
static obs_weak_source_t *target;

void setTarget(obs_source_t *owner)
{
	std::lock_guard l(targetMutex);
	obs_weak_source_release(target);
	target = owner ? obs_source_get_weak_source(owner) : nullptr;
}

OBSSource getTarget()
{
	std::lock_guard l(targetMutex);
	OBSSourceAutoRelease s = obs_weak_source_get_source(target);
	return s.Get();
}

obs_data_t *config()
{
	static obs_data_t *cfg = [] {
		char *path = obs_module_config_path("config.json");
		obs_data_t *d = obs_data_create_from_json_file_safe(path, "bak");
		bfree(path);
		return d ? d : obs_data_create();
	}();
	return cfg;
}

void saveConfig()
{
	char *dir = obs_module_config_path(""), *path = obs_module_config_path("config.json");
	os_mkdirs(dir);
	obs_data_save_json_safe(config(), path, "tmp", "bak");
	bfree(dir);
	bfree(path);
}

// --- Sketch source ---------------------------------------------------------

static void update(void *d, obs_data_t *s)
{
	auto c = static_cast<Canvas *>(d);
	c->keep = obs_data_get_bool(s, "keep");
	c->clearOnSwitch = obs_data_get_bool(s, "clear_on_switch");
	if (!c->filter) {
		c->bg = int(obs_data_get_int(s, "background"));
		c->resize(int(obs_data_get_int(s, "width")), int(obs_data_get_int(s, "height")));
	}
}

static void *create(obs_data_t *s, obs_source_t *src, bool filter)
{
	auto c = new Canvas(src, filter);
	update(c, s);
	c->load(s);
	return c;
}

static void defaults(obs_data_t *s)
{
	obs_video_info ovi{};
	obs_get_video_info(&ovi);
	obs_data_set_default_int(s, "width", ovi.base_width ? ovi.base_width : 1920);
	obs_data_set_default_int(s, "height", ovi.base_height ? ovi.base_height : 1080);
	obs_data_set_default_bool(s, "keep", true);
}

static obs_properties_t *properties(void *d)
{
	auto c = static_cast<Canvas *>(d);
	obs_properties_t *p = obs_properties_create();
	if (!c || !c->filter) {
		obs_properties_add_int(p, "width", Str("Width"), 16, 8192, 1);
		obs_properties_add_int(p, "height", Str("Height"), 16, 8192, 1);
		obs_property_t *bg = obs_properties_add_list(p, "background", Str("Background"), OBS_COMBO_TYPE_LIST,
							     OBS_COMBO_FORMAT_INT);
		for (int i = 0; i < 4; i++)
			obs_property_list_add_int(bg, Str(("Background." + std::to_string(i)).c_str()), i);
	}
	obs_properties_add_bool(p, "keep", Str("KeepDrawings"));
	obs_properties_add_bool(p, "clear_on_switch", Str("ClearOnSceneSwitch"));
	obs_properties_add_button2(
		p, "clear", Str("Clear"),
		[](obs_properties_t *, obs_property_t *, void *d2) {
			static_cast<Canvas *>(d2)->clear();
			return false;
		},
		c);
	return p;
}

static void filterRender(void *d, gs_effect_t *)
{
	auto c = static_cast<Canvas *>(d);
	obs_source_t *tgt = obs_filter_get_target(c->self);
	const uint32_t w = obs_source_get_base_width(tgt), h = obs_source_get_base_height(tgt);
	if (!w || !h) {
		obs_source_skip_video_filter(c->self);
		return;
	}
	c->resize(int(w), int(h));
	if (c->hasRegions()) {
		if (!c->texrender)
			c->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		gs_texrender_reset(c->texrender);
		if (gs_texrender_begin(c->texrender, w, h)) {
			vec4 zero;
			vec4_zero(&zero);
			gs_clear(GS_CLEAR_COLOR, &zero, 0, 0);
			gs_ortho(0, float(w), 0, float(h), -100, 100);
			obs_source_skip_video_filter(c->self);
			gs_texrender_end(c->texrender);
		}
		gs_texture_t *t = gs_texrender_get_texture(c->texrender);
		gs_effect_t *e = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_effect_set_texture(gs_effect_get_param_by_name(e, "image"), t);
		while (gs_effect_loop(e, "Draw"))
			gs_draw_sprite(t, 0, w, h);
		c->drawRegions(t);
	} else
		obs_source_skip_video_filter(c->self);
	c->draw();
}

void registerSources()
{
	obs_source_info s{};
	s.id = SourceId;
	s.type = OBS_SOURCE_TYPE_INPUT;
	s.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	s.icon_type = OBS_ICON_TYPE_IMAGE;
	s.get_name = [](void *) {
		return Str("SketchSource");
	};
	s.create = [](obs_data_t *d, obs_source_t *src) {
		return create(d, src, false);
	};
	s.destroy = [](void *d) {
		delete static_cast<Canvas *>(d);
	};
	s.update = update;
	s.get_defaults = defaults;
	s.get_properties = properties;
	s.save = [](void *d, obs_data_t *st) {
		static_cast<Canvas *>(d)->save(st);
	};
	s.get_width = [](void *d) {
		return uint32_t(static_cast<Canvas *>(d)->w);
	};
	s.get_height = [](void *d) {
		return uint32_t(static_cast<Canvas *>(d)->h);
	};
	s.video_render = [](void *d, gs_effect_t *) {
		static_cast<Canvas *>(d)->draw();
	};
	obs_register_source(&s);

	obs_source_info f = s;
	f.id = FilterId;
	f.type = OBS_SOURCE_TYPE_FILTER;
	f.output_flags = OBS_SOURCE_VIDEO;
	f.get_name = [](void *) {
		return Str("SketchFilter");
	};
	f.create = [](obs_data_t *d, obs_source_t *src) {
		return create(d, src, true);
	};
	f.get_width = nullptr;
	f.get_height = nullptr;
	f.video_render = filterRender;
	obs_register_source(&f);
}
