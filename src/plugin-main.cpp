/*
Sketch - drawing plugin for OBS Studio
Copyright (C) 2026 Voidscape Development

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "canvas.hpp"
#include "panel.hpp"
#include "remote.hpp"
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <QApplication>
#include <QPointer>
#include <QVBoxLayout>
#include <string>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static QPointer<Panel> panel;
static std::vector<std::pair<std::string, obs_hotkey_id>> hotkeys;

static void hotkey(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	const int a = int(intptr_t(data));
	if (pressed)
		QMetaObject::invokeMethod(
			qApp,
			[a] {
				if (panel)
					panel->action(a);
			},
			Qt::QueuedConnection);
}

static void registerHotkeys()
{
	auto add = [](const std::string &name, const char *desc, int action) {
		const std::string key = "sketch." + name;
		obs_hotkey_id id = obs_hotkey_register_frontend(key.c_str(), desc, hotkey, (void *)intptr_t(action));
		OBSDataArrayAutoRelease saved = obs_data_get_array(config(), key.c_str());
		obs_hotkey_load(id, saved);
		hotkeys.emplace_back(key, id);
	};
	for (int i = 0; i < ToolCount; i++)
		add(tools[i].id,
		    (std::string(Str("Sketch")) + ": " + Str((std::string("Tool.") + tools[i].id).c_str())).c_str(), i);
	add("undo", (std::string(Str("Sketch")) + ": " + Str("Undo")).c_str(), ToolCount);
	add("redo", (std::string(Str("Sketch")) + ": " + Str("Redo")).c_str(), ToolCount + 1);
	add("clear", (std::string(Str("Sketch")) + ": " + Str("Clear")).c_str(), ToolCount + 2);
}

static void frontendEvent(obs_frontend_event e, void *)
{
	switch (e) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING: {
		auto host = new QWidget;
		auto l = new QVBoxLayout(host);
		l->setContentsMargins(0, 0, 0, 0);
		panel = new Panel(host);
		l->addWidget(panel);
		obs_frontend_add_dock_by_id("sketch", Str("Sketch"), host);
		break;
	}
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		forEachCanvas([](obs_source_t *s) {
			if (Canvas *c = canvasOf(s); c && c->clearOnSwitch)
				c->clear();
		});
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		for (auto &[key, id] : hotkeys) {
			OBSDataArrayAutoRelease a = obs_hotkey_save(id);
			obs_data_set_array(config(), key.c_str(), a);
		}
		Remote::get().shutdown();
		setTarget(nullptr);
		saveConfig();
		break;
	default:
		break;
	}
}

bool obs_module_load(void)
{
	registerSources();
	registerHotkeys();
	obs_frontend_add_event_callback(frontendEvent, nullptr);
	obs_log(LOG_INFO, "plugin loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(frontendEvent, nullptr);
	for (auto &[key, id] : hotkeys)
		obs_hotkey_unregister(id);
	obs_data_release(config());
}
