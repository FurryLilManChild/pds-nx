/*
 * pds-nx: buffered log file on the SD card.
 *
 * Writes go through a stdio buffer and are flushed on demand (once a second
 * from the main loop, and on exit), because unbuffered SD writes stall the
 * emulator badly.
 *
 * This file is part of pds-nx and is licensed under the GNU GPL v2 or later.
 */
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include "nx_log.h"

static FILE *logf;
static char logbuf[64 * 1024];

void nxlog_open(const char *path)
{
    logf = fopen(path, "w");
    if (logf)
        setvbuf(logf, logbuf, _IOFBF, sizeof(logbuf));
}

void nxlog(const char *fmt, ...)
{
    if (!logf)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(logf, fmt, ap);
    va_end(ap);
    fputc('\n', logf);
}

void nxlog_flush(void)
{
    if (logf)
        fflush(logf);
}

void nxlog_close(void)
{
    if (logf) {
        fclose(logf);
        logf = NULL;
    }
}
