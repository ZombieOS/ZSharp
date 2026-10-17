#include "time_api.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#endif

/* Gregorian civil dates, restricted to years 1..9999; no libc date rollover. */
static int leap(int y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }
static int valid(const int f[7]) {
    static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return f[0] >= 1 && f[0] <= 9999 && f[1] >= 1 && f[1] <= 12 &&
        f[2] >= 1 && f[2] <= days[f[1]-1] + (f[1] == 2 && leap(f[0])) &&
        f[3] >= 0 && f[3] <= 23 && f[4] >= 0 && f[4] <= 59 &&
        f[5] >= 0 && f[5] <= 59;
}
static int64_t civil_seconds(const int f[7]) {
    int y = f[0] - (f[1] <= 2), era = y / 400;
    unsigned yo = (unsigned)(y - era * 400);
    unsigned mp = (unsigned)(f[1] + (f[1] > 2 ? -3 : 9));
    unsigned doy = (153 * mp + 2) / 5 + (unsigned)f[2] - 1;
    int64_t days = (int64_t)era * 146097 + yo * 365 + yo/4 - yo/100 + doy - 719468;
    return days * 86400 + f[3] * 3600 + f[4] * 60 + f[5];
}
static int digits(const char **p, int min, int max, int *value) {
    int n = 0, v = 0;
    while (isdigit((unsigned char)**p) && n < max) { v = v * 10 + *(*p)++ - '0'; n++; }
    if (n < min || isdigit((unsigned char)**p)) return 0;
    *value = v; return 1;
}
static int date(const char *s, int f[7]) {
    const char *p = s; int a, b, c, year_digits;
    if (!digits(&p, 1, 4, &a)) return 0;
    if (*p == '-') {
        if (p - s != 4) return 0;
        p++; if (!digits(&p, 2, 2, &b) || *p++ != '-' || !digits(&p, 2, 2, &c) || *p) return 0;
        f[0] = a; f[1] = b; f[2] = c;
    } else if (*p == '/') {
        p++; if (!digits(&p, 1, 2, &b) || *p++ != '/') return 0;
        s = p; if (!digits(&p, 2, 4, &c) || *p) return 0;
        year_digits = (int)(p - s); if (year_digits != 2 && year_digits != 4) return 0;
        f[0] = year_digits == 2 ? 2000 + c : c; f[1] = a; f[2] = b;
    } else return 0;
    return 1;
}
static int daytime(const char *p, int f[7]) {
    if (!digits(&p, 2, 2, &f[3]) || *p++ != ':' || !digits(&p, 2, 2, &f[4])) return 0;
    f[5] = 0;
    if (*p == ':') { p++; if (!digits(&p, 2, 2, &f[5])) return 0; }
    return *p == 0;
}
static int stamp(const char *s, int milliseconds, int64_t *out) {
    const char *p = s; int negative = 0, fraction = 0, places = 0;
    uint64_t value = 0;
    if (*p == '-' || *p == '+') negative = *p++ == '-';
    if (!isdigit((unsigned char)*p)) return 0;
    while (isdigit((unsigned char)*p)) {
        if (value > UINT64_C(253402300800000)) return 0;
        value = value * 10 + (unsigned)(*p++ - '0');
    }
    if (*p == '.') {
        p++; if (!isdigit((unsigned char)*p)) return 0;
        while (isdigit((unsigned char)*p)) {
            int d = *p++ - '0';
            if (milliseconds) { if (d) return 0; }
            else if (places < 3) { fraction = fraction * 10 + d; places++; }
            else if (d) return 0;
        }
    }
    if (*p || value > (milliseconds ? UINT64_C(253402300799999) : UINT64_C(253402300799))) return 0;
    while (places < 3) { fraction *= 10; places++; }
    *out = milliseconds ? (int64_t)value : (int64_t)value * 1000 + fraction;
    if (negative) *out = -*out;
    return *out >= INT64_C(-62135596800000) && *out <= INT64_C(253402300799999);
}

#ifndef __wasi__
static int system_now(int64_t *ms) {
#ifdef _WIN32
    FILETIME ft; ULARGE_INTEGER n;
    GetSystemTimeAsFileTime(&ft); n.LowPart = ft.dwLowDateTime; n.HighPart = ft.dwHighDateTime;
    *ms = (int64_t)(n.QuadPart / 10000) - INT64_C(11644473600000); return 1;
#else
    struct timespec t;
    if (clock_gettime(CLOCK_REALTIME, &t) != 0) return 0;
    *ms = (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; return 1;
#endif
}
static int system_local(int64_t ms, int f[7]) {
    time_t seconds = (time_t)(ms / 1000 - (ms < 0 && ms % 1000 != 0)); struct tm t;
#ifdef _WIN32
    if (localtime_s(&t, &seconds) != 0) return 0;
#else
    if (!localtime_r(&seconds, &t)) return 0;
#endif
    f[0] = t.tm_year + 1900; f[1] = t.tm_mon + 1; f[2] = t.tm_mday;
    f[3] = t.tm_hour; f[4] = t.tm_min; f[5] = t.tm_sec; f[6] = t.tm_wday;
    return 1;
}
static int system_convert(const int f[7], int64_t *ms) {
    int dst, found = 0, check[7]; int64_t candidate = 0;
    /* Try both DST states. Reject nonexistent and ambiguous local times. */
    for (dst = 0; dst <= 1; dst++) {
        struct tm t = {0}; time_t seconds;
        t.tm_year=f[0]-1900; t.tm_mon=f[1]-1; t.tm_mday=f[2];
        t.tm_hour=f[3]; t.tm_min=f[4]; t.tm_sec=f[5]; t.tm_isdst=dst;
        seconds=mktime(&t);
        if (!system_local((int64_t)seconds * 1000, check) || memcmp(check,f,6*sizeof(int))) continue;
        if (found && candidate != (int64_t)seconds * 1000) return 0;
        found=1; candidate=(int64_t)seconds*1000;
    }
    *ms=candidate; return found;
}
static const ZSharpTimeHost system_host = {system_now, system_local, system_convert};
#endif

static int error_text(char *error, size_t size, const char *message) {
    snprintf(error, size, "Time: %s", message); return 0;
}
int zsharp_time_evaluate(int op, const char *const *args, size_t count,
                        const ZSharpTimeHost *host, char *out, size_t size,
                        int *is_text, char *error, size_t error_size) {
    int f[7]={0}, value=0; int64_t now=0, target=0;
    if (!out || size < 256 || !is_text) return error_text(error,error_size,"invalid result buffer");
    *is_text=0; out[0]=0;
#ifndef __wasi__
    if (!host) host=&system_host;
#endif
    if (op == ZTIME_CONVERT) {
        int offset=0, sign=1; const char *p;
        if(count!=3) return error_text(error,error_size,"convert requires date, time, and timezone");
        if(!date(args[0],f) || !daytime(args[1],f) || !valid(f))
            return error_text(error,error_size,"invalid date/time; use M/D/YY, M/D/YYYY or YYYY-MM-DD and HH:MM[:SS]");
        if(!strcmp(args[2],"LOCAL") || !strcmp(args[2],"local")) {
            if(!host || !host->convert_local || !host->convert_local(f,&target))
                return error_text(error,error_size,"local date/time is unsupported, nonexistent or ambiguous; use UTC or an explicit UTC offset");
        } else {
            p=args[2];
            if(strncmp(p,"UTC",3)) return error_text(error,error_size,"timezone must be UTC, LOCAL or UTC+HH:MM/UTC-HH:MM");
            p+=3;
            if(*p) {
                int hours, minutes;
                if(*p!='+' && *p!='-') return error_text(error,error_size,"invalid UTC offset");
                sign=*p++=='-'?-1:1;
                if(!digits(&p,2,2,&hours) || *p++!=':' || !digits(&p,2,2,&minutes) || *p ||
                   hours>14 || minutes>59 || (hours==14 && minutes)) return error_text(error,error_size,"invalid UTC offset");
                offset=sign*(hours*60+minutes);
            }
            target=(civil_seconds(f)-(int64_t)offset*60)*1000;
        }
        if(target<INT64_C(-62135596800000) || target>INT64_C(253402300799999)) return error_text(error,error_size,"timestamp outside years 1..9999");
        snprintf(out,size,"%" PRId64,target/1000); return 1;
    }
    if (!host || !host->now || !host->now(&now)) return error_text(error,error_size,"system clock unavailable");
    if (op == ZTIME_SINCE || op == ZTIME_UNTIL) {
        int ms=0, future; int64_t seconds, units[4]; size_t used=0; int i;
        static const char *names[]={"day","hour","minute","second"};
        if(count<1 || count>2) return error_text(error,error_size,"since/until requires a timestamp and optional unit");
        if(count==2) {
            if(!strcmp(args[1],"ms")) ms=1;
            else if(strcmp(args[1],"s")) return error_text(error,error_size,"timestamp unit must be s or ms");
        }
        if(!stamp(args[0],ms,&target)) return error_text(error,error_size,"invalid Unix timestamp (seconds or whole milliseconds; years 1..9999)");
        future=target>now; seconds=(future?target-now:now-target)/1000;
        units[0]=seconds/86400; seconds%=86400; units[1]=seconds/3600;
        seconds%=3600; units[2]=seconds/60; units[3]=seconds%60;
        if(future) { strcpy(out,"in "); used=3; }
        for(i=0;i<4;i++) if(units[i] || (i==3 && used==(size_t)(future?3:0))) {
            int n=snprintf(out+used,size-used,"%s%" PRId64 " %s%s",
                used>(size_t)(future?3:0)?", ":"",units[i],names[i],units[i]==1?"":"s");
            if(n<0 || (size_t)n>=size-used) return error_text(error,error_size,"duration result too long");
            used+=(size_t)n;
        }
        if(!future) snprintf(out+used,size-used," ago");
        *is_text=1; return 1;
    }
    if(op<ZTIME_YEAR || op>ZTIME_HOUR_FORMATTED || (op!=ZTIME_MS && count) || (op==ZTIME_MS && count>1))
        return error_text(error,error_size,"invalid getter or argument count");
    if(op==ZTIME_MS) {
        int precision=1;
        if(count) {
            char *end; double p=strtod(args[0],&end);
            if(end==args[0] || *end || !(p==1 || p==2 || p==3)) return error_text(error,error_size,"getMS precision must be 1, 2 or 3");
            precision=(int)p;
        }
        value=(int)((now%1000+1000)%1000); if(precision==1)value/=100;else if(precision==2)value/=10;
    } else {
        if(!host->local || !host->local(now,f) || !valid(f) || f[6]<0 || f[6]>6)
            return error_text(error,error_size,"local timezone unavailable");
        if(op==ZTIME_WEEK) {
            static const char *names[]={"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
            snprintf(out,size,"%s",names[f[6]]); *is_text=1; return 1;
        }
        if(op==ZTIME_HOUR_FORMATTED) {
            snprintf(out,size,"%d %s",f[3]%12?f[3]%12:12,f[3]>=12?"PM":"AM"); *is_text=1; return 1;
        }
        switch(op) { case ZTIME_YEAR:value=f[0];break;case ZTIME_MONTH:value=f[1];break;
            case ZTIME_DAY:value=f[2];break;case ZTIME_HOUR:value=f[3];break;
            case ZTIME_MINUTE:value=f[4];break;case ZTIME_SECOND:value=f[5];break;
            default:return error_text(error,error_size,"invalid Time getter"); }
    }
    snprintf(out,size,"%d",value); return 1;
}
