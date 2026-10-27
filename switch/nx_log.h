/* pds-nx: buffered log file on the SD card. GPL v2 or later. */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void nxlog_open(const char *path);
void nxlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void nxlog_flush(void);
void nxlog_close(void);
#ifdef __cplusplus
}
#endif
