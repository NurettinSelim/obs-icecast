/*
 * obs-shoutcast - audio-only MP3 streaming output for OBS Studio
 * Copyright (C) 2026 Nurettin Selim
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <obs-module.h>

#include "mp3-encoder.h"
#include "shoutcast-output.h"
#include "radioco-dock.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-shoutcast", "en-US")

bool obs_module_load(void)
{
	obs_register_encoder(&shoutcast_mp3_encoder);
	obs_register_output(&shoutcast_output_info);
	blog(LOG_INFO, "[obs-shoutcast] plugin loaded (version %s)",
	     PLUGIN_VERSION);
	return true;
}

/*
 * Runs after every module has loaded and the main window exists.
 * Registering the dock from obs_module_load would race its construction;
 * OBS's own decklink-output-ui plugin uses obs_module_post_load for the
 * same reason.
 */
void obs_module_post_load(void)
{
	radioco_dock_init();
}

void obs_module_unload(void)
{
	radioco_dock_free();
	blog(LOG_INFO, "[obs-shoutcast] plugin unloaded");
}
