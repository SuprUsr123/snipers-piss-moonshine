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

#include "connection.h"

#include <stdio.h>
#include <stdarg.h>
#include <signal.h>
#include <string.h>

#ifdef HAVE_SDL
#include <SDL.h>
#endif

pthread_t main_thread_id = 0;
bool connection_debug;
char connection_message[192] = "";
bool stream_session_started = false;
ConnListenerRumble rumble_handler = NULL;
ConnListenerRumbleTriggers rumble_triggers_handler = NULL;
ConnListenerSetMotionEventState set_motion_event_state_handler = NULL;
ConnListenerSetControllerLED set_controller_led_handler = NULL;

static void connection_terminated(int errorCode) {
  const char* text;
  switch (errorCode) {
  case ML_ERROR_GRACEFUL_TERMINATION:
    text = "The PC ended the stream.";
    break;
  case ML_ERROR_NO_VIDEO_TRAFFIC:
    text = "No video received from the PC. Check its firewall (UDP ports 47998-48000).";
    break;
  case ML_ERROR_NO_VIDEO_FRAME:
    text = "The Wi-Fi connection is too slow. Lower the bitrate or move closer to the router.";
    break;
  case ML_ERROR_UNEXPECTED_EARLY_TERMINATION:
    text = "The PC stopped the stream because of a video capture error.";
    break;
  case ML_ERROR_PROTECTED_CONTENT:
    text = "The PC stopped the stream because DRM-protected content is on screen.";
    break;
  default:
    snprintf(connection_message, sizeof(connection_message),
             "The stream ended unexpectedly (error %d).", errorCode);
    text = NULL;
    break;
  }
  if (text != NULL)
    snprintf(connection_message, sizeof(connection_message), "%s", text);
  printf("%s\n", connection_message);

  #ifdef HAVE_SDL
      SDL_Event event;
      event.type = SDL_QUIT;
      SDL_PushEvent(&event);
  #endif

  if (main_thread_id != 0)
    pthread_kill(main_thread_id, SIGTERM);
}

static void connection_log_message(const char* format, ...) {
  va_list arglist;
  va_start(arglist, format);
  vprintf(format, arglist);
  va_end(arglist);
}

static void rumble(unsigned short controllerNumber, unsigned short lowFreqMotor, unsigned short highFreqMotor) {
  if (rumble_handler)
    rumble_handler(controllerNumber, lowFreqMotor, highFreqMotor);
}

static void rumble_triggers(unsigned short controllerNumber, unsigned short leftTrigger, unsigned short rightTrigger) {
  if (rumble_handler)
    rumble_triggers_handler(controllerNumber, leftTrigger, rightTrigger);
}

static void set_motion_event_state(unsigned short controllerNumber, unsigned char motionType, unsigned short reportRateHz) {
  if (set_motion_event_state_handler)
    set_motion_event_state_handler(controllerNumber, motionType, reportRateHz);
}

static void set_controller_led(unsigned short controllerNumber, unsigned char r, unsigned char g, unsigned char b) {
  if (set_controller_led_handler)
    set_controller_led_handler(controllerNumber, r, g, b);
}

/* Without this the Kindle shows an empty window when a start-up stage
 * fails: stdout/stderr are not visible there. */
static void connection_stage_failed(int stage, int errorCode) {
  snprintf(connection_message, sizeof(connection_message),
           "The stream failed during \"%s\" (error %d).", LiGetStageName(stage),
           errorCode);
  fprintf(stderr, "%s\n", connection_message);
}

static void connection_status_update(int status) {
  switch (status) {
    case CONN_STATUS_OKAY:
      printf("Connection is okay\n");
      break;
    case CONN_STATUS_POOR:
      printf("Connection is poor\n");
      break;
  }
}

CONNECTION_LISTENER_CALLBACKS connection_callbacks = {
  .stageStarting = NULL,
  .stageComplete = NULL,
  .stageFailed = connection_stage_failed,
  .connectionStarted = NULL,
  .connectionTerminated = connection_terminated,
  .logMessage = connection_log_message,
  .rumble = rumble,
  .connectionStatusUpdate = connection_status_update,
  .setHdrMode = NULL,
  .rumbleTriggers = rumble_triggers,
  .setMotionEventState = set_motion_event_state,
  .setControllerLED = set_controller_led,
};
