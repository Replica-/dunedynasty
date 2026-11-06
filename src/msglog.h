/** @file src/msglog.h A floating message log, drawn over the viewport in place of the status bar. */

#ifndef MSGLOG_H
#define MSGLOG_H

#include <stdbool.h>

extern bool g_floating_log;     /* [graphics] floating_message_log (default on). */

extern void MsgLog_Add(const char *text);   /* A message for the player, stamped with the game clock. */
extern void MsgLog_Mute(bool mute);         /* While muted, messages only go to the status bar (unit status text). */
extern void MsgLog_Draw(void);              /* In game, after the interface: the last few messages, the oldest fading. */

/* The height of the menu bar: only the top bar when the log floats, else the status bar too. */
extern float MsgLog_MenubarHeight(void);

#endif /* MSGLOG_H */
