/* SPDX-License-Identifier: MIT */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* The contract between the web process extension and the UI process. Both
 * sides include this so the message name and its payload cannot drift apart. */

#define WIG_CONSOLE_MESSAGE_NAME "wig.console-message"

/* source, level, text, source id, line */
#define WIG_CONSOLE_MESSAGE_FORMAT "(ssssu)"

G_END_DECLS
