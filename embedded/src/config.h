/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include <Limelight.h>

#include <stdbool.h>

#define MAX_INPUTS 6

typedef struct _CONFIGURATION {
  STREAM_CONFIGURATION stream;
  int debug_level;
  char* app;
  char* action;
  char* address;
  char* mapping;
  char* platform;
  char* audio_device;
  char* config_file;
  char key_dir[4096];
  bool sops;
  bool localaudio;
  bool fullscreen;
  int rotate;
  bool unsupported;
  bool quitappafter;
  bool viewonly;
  bool mouse_emulation;
  char* inputs[MAX_INPUTS];
  int inputsCount;
  enum codecs codec;
  bool hdr;
  int pin;
  unsigned short port;
  int refresh_ms;
  int full_refresh_ms;
  char* wfm;
  int fbink_rotate;
  /* Rendering preset from kindle-modes.h; KINDLE_MODE_CUSTOM (-1) means
   * the manual refresh/fullrefresh/wfm values apply. */
  int render_mode;
  /* Pixel path: KINDLE_BACKEND_X11 or KINDLE_BACKEND_FBINK. */
  int render_backend;
  /* Menu zoom factor (uizoom in moonlight.conf / the Settings page).
   * Below 1 zooms out, above 1 zooms in; see kindle-style.h. */
  double uizoom;
} CONFIGURATION, *PCONFIGURATION;

extern bool inputAdded;

bool config_file_parse(char* filename, PCONFIGURATION config);
void config_parse(int argc, char* argv[], PCONFIGURATION config);
