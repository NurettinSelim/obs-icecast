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
