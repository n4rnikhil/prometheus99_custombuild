/**
 * @file utc_format.c
 * @brief hal_utc_format(): civil date from a day count (H. Hinnant's algorithm), no time zone database
 */

#include <stdio.h>
#include "hal/sys.h"

void hal_utc_format(uint64_t utc_ms, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    uint64_t secs = utc_ms / 1000u;
    unsigned ms = (unsigned)(utc_ms % 1000u);
    int64_t days = (int64_t)(secs / 86400u);
    unsigned sod = (unsigned)(secs % 86400u);

    days += 719468;                                        /* shift epoch to 0000-03-01 */
    int64_t era = days / 146097;
    unsigned doe = (unsigned)(days - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;

    snprintf(out, cap, "%04ld-%02u-%02uT%02u:%02u:%02u.%03uZ", (long)y, m, d, sod / 3600u, (sod / 60u) % 60u,
             sod % 60u, ms);
}
