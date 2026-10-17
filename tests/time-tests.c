#include "time_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int64_t clock_ms = INT64_C(1767366245427);
static int hour=15;
static int now(int64_t *ms) { *ms=clock_ms;return 1; }
static int local(int64_t ms,int f[7]) {
    (void)ms;f[0]=2026;f[1]=1;f[2]=2;f[3]=hour;f[4]=4;f[5]=5;f[6]=5;return 1;
}
static int convert(const int f[7],int64_t *ms) {
    if(f[0]!=2026 || f[1]!=1 || f[2]!=2)return 0;
    *ms=INT64_C(1767366245000);return 1;
}
static const ZSharpTimeHost host={now,local,convert};
static int checks;
static void check(int op,size_t n,const char *a,const char *b,const char *c,
                  const char *expected,int type) {
    const char *args[]={a,b,c};char out[512],error[512];int is_text=-1;
    int ok=zsharp_time_evaluate(op,args,n,&host,out,sizeof(out),&is_text,error,sizeof(error));
    checks++;
    if(!expected) { if(!ok && !strncmp(error,"Time:",5))return; }
    else if(ok && !strcmp(out,expected) && is_text==type)return;
    fprintf(stderr,"Time test %d failed (op %d): %s %s\n",checks,op,ok?out:error,expected?expected:"expected failure");exit(1);
}
int main(void) {
    check(ZTIME_CONVERT,3,"1/1/26","00:00","UTC","1767225600",0);
    check(ZTIME_CONVERT,3,"01/01/2026","00:00:00","UTC","1767225600",0);
    check(ZTIME_CONVERT,3,"1970-01-01","00:00","UTC","0",0);
    check(ZTIME_CONVERT,3,"1969-12-31","23:59:59","UTC","-1",0);
    check(ZTIME_CONVERT,3,"0001-01-01","00:00","UTC","-62135596800",0);
    check(ZTIME_CONVERT,3,"9999-12-31","23:59:59","UTC","253402300799",0);
    check(ZTIME_CONVERT,3,"2026-01-01","00:00","UTC+05:30","1767205800",0);
    check(ZTIME_CONVERT,3,"2026-01-01","00:00","UTC-04:00","1767240000",0);
    check(ZTIME_CONVERT,3,"2000-02-29","00:00","UTC","951782400",0);
    check(ZTIME_CONVERT,3,"2026-01-02","15:04:05","LOCAL","1767366245",0);
    check(ZTIME_CONVERT,3,"2026-01-01","00:00","LOCAL",NULL,0);
    check(ZTIME_CONVERT,3,"2026-02-29","00:00","UTC",NULL,0);
    check(ZTIME_CONVERT,3,"1900-02-29","00:00","UTC",NULL,0);
    check(ZTIME_CONVERT,3,"2026-04-31","00:00","UTC",NULL,0);
    check(ZTIME_CONVERT,3,"2026-01-01","24:00","UTC",NULL,0);
    check(ZTIME_CONVERT,3,"2026-01-01","00:60","UTC",NULL,0);
    check(ZTIME_CONVERT,3,"2026-01-01","00:00:60","UTC",NULL,0);
    check(ZTIME_CONVERT,3,"2026-01-01","00:00","UTC+14:01",NULL,0);
    check(ZTIME_CONVERT,3,"2026-01-01","00:00","EST",NULL,0);
    check(ZTIME_CONVERT,3,"1/1/126","00:00","UTC",NULL,0);
    check(ZTIME_CONVERT,3,"2026-01-01junk","00:00","UTC",NULL,0);
    check(ZTIME_CONVERT,3,"2026-01-01","00:00junk","UTC",NULL,0);
    check(ZTIME_CONVERT,2,"2026-01-01","00:00",NULL,NULL,0);
    check(ZTIME_YEAR,0,NULL,NULL,NULL,"2026",0);
    check(ZTIME_MONTH,0,NULL,NULL,NULL,"1",0);
    check(ZTIME_DAY,0,NULL,NULL,NULL,"2",0);
    check(ZTIME_WEEK,0,NULL,NULL,NULL,"Friday",1);
    check(ZTIME_HOUR,0,NULL,NULL,NULL,"15",0);
    check(ZTIME_MINUTE,0,NULL,NULL,NULL,"4",0);
    check(ZTIME_SECOND,0,NULL,NULL,NULL,"5",0);
    check(ZTIME_HOUR_FORMATTED,0,NULL,NULL,NULL,"3 PM",1);
    hour=0;check(ZTIME_HOUR_FORMATTED,0,NULL,NULL,NULL,"12 AM",1);
    hour=12;check(ZTIME_HOUR_FORMATTED,0,NULL,NULL,NULL,"12 PM",1);hour=15;
    check(ZTIME_MS,0,NULL,NULL,NULL,"4",0);
    check(ZTIME_MS,1,"1",NULL,NULL,"4",0);
    check(ZTIME_MS,1,"2",NULL,NULL,"42",0);
    check(ZTIME_MS,1,"3",NULL,NULL,"427",0);
    check(ZTIME_MS,1,"0",NULL,NULL,NULL,0);
    check(ZTIME_MS,1,"4",NULL,NULL,NULL,0);
    check(ZTIME_MS,1,"2.5",NULL,NULL,NULL,0);
    check(ZTIME_YEAR,1,"1",NULL,NULL,NULL,0);
    clock_ms=INT64_C(1767225600000);
    check(ZTIME_SINCE,1,"1767131760",NULL,NULL,"1 day, 2 hours, 4 minutes ago",1);
    check(ZTIME_UNTIL,1,"1767319440",NULL,NULL,"in 1 day, 2 hours, 4 minutes",1);
    check(ZTIME_SINCE,1,"1767319440",NULL,NULL,"in 1 day, 2 hours, 4 minutes",1);
    check(ZTIME_UNTIL,1,"1767131760",NULL,NULL,"1 day, 2 hours, 4 minutes ago",1);
    check(ZTIME_SINCE,2,"1767225599000","ms",NULL,"1 second ago",1);
    check(ZTIME_SINCE,2,"1767225599","s",NULL,"1 second ago",1);
    check(ZTIME_SINCE,1,"1767225600",NULL,NULL,"0 seconds ago",1);
    check(ZTIME_SINCE,1,"1767225600.500",NULL,NULL,"in 0 seconds",1);
    check(ZTIME_SINCE,2,"1767225600000.5","ms",NULL,NULL,0);
    check(ZTIME_SINCE,1,"1e9",NULL,NULL,NULL,0);
    check(ZTIME_SINCE,1,"NaN",NULL,NULL,NULL,0);
    check(ZTIME_SINCE,1,"999999999999999999999999",NULL,NULL,NULL,0);
    check(ZTIME_SINCE,2,"1767225600","minutes",NULL,NULL,0);
    check(ZTIME_SINCE,1,"253402300800",NULL,NULL,NULL,0);
    check(ZTIME_SINCE,1,"-62135596801",NULL,NULL,NULL,0);
    printf("PASS: %d deterministic Time checks\n",checks);return 0;
}
