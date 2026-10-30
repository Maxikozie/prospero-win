/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The PS4 title's log channel: see orbislog.c. The ps5log functions are
 * declared by native/ps5log/ps5log.h; this adds the kernel log. */
#ifndef ORBISLOG_H
#define ORBISLOG_H

/* One line to the kernel log (klog, GoldHEN port 3232). Slow and blocking
 * when nothing drains it: for the milestone lines and fatal records only. */
void orbislog_klog(const char *line);

#endif /* ORBISLOG_H */
