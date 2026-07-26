/*
 * obs-icecast - audio-only MP3 streaming output for OBS Studio
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
