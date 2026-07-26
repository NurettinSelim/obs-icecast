#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Create the widget, register the dock and the frontend event callback. */
void radioco_dock_init(void);

/* Stop the output, remove the dock, delete the widget. */
void radioco_dock_free(void);

#ifdef __cplusplus
}
#endif
