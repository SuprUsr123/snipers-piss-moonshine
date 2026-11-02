#pragma once

#include "platform.h"
#include "config.h"
#include <client.h>

void kindle_ui_run(PCONFIGURATION config);
int stream(PSERVER_DATA server, PCONFIGURATION config, enum platform system);
