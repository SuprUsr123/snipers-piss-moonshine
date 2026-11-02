/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2019 Iwan Timmer
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

#include "loop.h"

#include "connection.h"

#include <sys/stat.h>
#include <sys/signalfd.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#ifdef HAVE_KINDLE
#include <gtk/gtk.h>
#endif

static struct pollfd* fds = NULL;
static FdHandler* fdHandlers = NULL;
static int numFds = 0;

static int sigFd = -1;
#ifdef HAVE_KINDLE
static guint loopPumpId;
#endif

static int loop_sig_handler(int fd) {
  struct signalfd_siginfo info;
  if (read(fd, &info, sizeof(info)) != sizeof(info))
    return LOOP_RETURN;
  switch (info.ssi_signo) {
    case SIGINT:
    case SIGTERM:
    case SIGQUIT:
    case SIGHUP:
      return LOOP_RETURN;
  }
  return LOOP_OK;
}

void loop_add_fd(int fd, FdHandler handler, int events) {
  int fdindex = numFds;
  numFds++;

  if (fds == NULL) {
    fds = malloc(sizeof(struct pollfd));
    fdHandlers = malloc(sizeof(FdHandler));
  } else {
    fds = realloc(fds, sizeof(struct pollfd)*numFds);
    fdHandlers = realloc(fdHandlers, sizeof(FdHandler)*numFds);
  }

  if (fds == NULL || fdHandlers == NULL) {
    fprintf(stderr, "Not enough memory\n");
    exit(EXIT_FAILURE);
  }

  fds[fdindex].fd = fd;
  fds[fdindex].events = events;
  fdHandlers[fdindex] = handler;
}

void loop_remove_fd(int fd) {
  int fdindex = -1;

  for (int i=0;i<numFds;i++) {
    if (fds[i].fd == fd) {
      fdindex = i;
      break;
    }
  }

  if (fdindex == -1)
    return;

  numFds--;
  if (fdindex < numFds) {
    memmove(&fds[fdindex], &fds[fdindex+1], sizeof(struct pollfd)*(numFds-fdindex));
    memmove(&fdHandlers[fdindex], &fdHandlers[fdindex+1], sizeof(FdHandler)*(numFds-fdindex));
  }
}

void loop_init() {
  if (sigFd >= 0)
    return;
  main_thread_id = pthread_self();
  sigset_t sigset;
  sigemptyset(&sigset);
  sigaddset(&sigset, SIGHUP);
  sigaddset(&sigset, SIGTERM);
  sigaddset(&sigset, SIGINT);
  sigaddset(&sigset, SIGQUIT);
  sigprocmask(SIG_BLOCK, &sigset, NULL);
  sigFd = signalfd(-1, &sigset, 0);
  if (sigFd < 0) {
    perror("signalfd");
    return;
  }
  loop_add_fd(sigFd, loop_sig_handler, POLLIN | POLLERR | POLLHUP);
}

static int loop_poll_once(int timeout) {
  int result = poll(fds, numFds, timeout);
  if (result > 0) {
    for (int i=0;i<numFds;i++) {
      if (fds[i].revents > 0 && fdHandlers[i](fds[i].fd) == LOOP_RETURN)
        return LOOP_RETURN;
    }
  }
  return LOOP_OK;
}

#ifdef HAVE_KINDLE
static gboolean loop_gtk_pump(gpointer) {
  if (loop_poll_once(0) == LOOP_RETURN) {
    loopPumpId = 0;
    gtk_main_quit();
    return FALSE;
  }
  return TRUE;
}
#endif

void loop_main() {
#ifdef HAVE_KINDLE
  loopPumpId = g_timeout_add(5, loop_gtk_pump, NULL);
  gtk_main();
  if (loopPumpId != 0) {
    g_source_remove(loopPumpId);
    loopPumpId = 0;
  }
#else
  while (poll(fds, numFds, -1)) {
    for (int i=0;i<numFds;i++) {
      if (fds[i].revents > 0) {
        int ret = fdHandlers[i](fds[i].fd);
        if (ret == LOOP_RETURN) {
          return;
        }
      }
    }
  }
#endif
}
