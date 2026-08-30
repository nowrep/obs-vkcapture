// Copyright (C) 2026 Anthony Mendez <anthonymendez9@gmail.com>
//
// Use of this source code is governed by a GPL-2.0-or-later license that can
// be found in the LICENSE file.

#ifndef OBS_VKCAPTURE_SRC_VKCAPTURE_GUI_H_
#define OBS_VKCAPTURE_SRC_VKCAPTURE_GUI_H_

#ifdef __cplusplus
extern "C" {
#endif

// Displays a dialog in OBS to apply or remove obs-gamecapture launch options
// for all games in the user's Steam library.
void show_steam_launch_options_dialog(void *parent);

#ifdef __cplusplus
}
#endif

#endif // OBS_VKCAPTURE_SRC_VKCAPTURE_GUI_H_
