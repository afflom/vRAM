#define _POSIX_C_SOURCE 200809L
#include "pager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef uint8_t u8;
typedef uint32_t u32;
enum { PAGE=4096, COUNT=128, SCANS=8 };
#include "capacity_pattern.h"
static uint8_t input[COUNT][PAGE], output[COUNT][PAGE];
static uint64_t now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&t)) abort();
    return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;
}
int main(int argc,char **argv) {
    if (argc!=4) return 1;
    int limit=atoi(argv[1]);
    char profile=*argv[2];
    int adaptive=atoi(argv[3]);
    struct store *s=store_new_config(COUNT,limit,adaptive);
    if (!s) return 2;
    for (int p=0;p<COUNT;p++) expected(p,profile,input[p]);
    uint64_t start=now();
    for (int p=0;p<COUNT;p++) if (store_put(s,p,input[p])) return 3;
    uint64_t captured=now()-start,restore=0;
    for (int pass=0;pass<SCANS;pass++) {
        start=now();
        for (int p=0;p<COUNT;p++) if (store_get(s,p,output[p])) return 4;
        restore+=now()-start;
        if (memcmp(input,output,sizeof input)) return 5;
    }
    uint64_t pages,program_bytes,searches,nodes,stats[5];
    store_search_stats(s,&pages,&program_bytes,&searches,&nodes);
    store_stats(s,stats);
    uint64_t refits,skips;
    store_feedback_stats(s,&refits,&skips);
    if (profile=='F' && limit==49 && pages!=COUNT) return 6;
    if (!limit && (pages || searches || nodes)) return 7;
    printf("{\"adaptive\":%d,\"refits\":%lu,\"skips\":%lu,\"profile\":\"%c\",\"budget\":%d,\"input_pages\":%d,\"restores\":%d,"
           "\"program_pages\":%lu,\"program_bytes\":%lu,\"searches\":%lu,\"nodes\":%lu,"
           "\"store_bytes\":%lu,\"raw_pages\":%lu,\"capture_cpu_ns\":%lu,\"restore_cpu_ns\":%lu}\n",
           adaptive,refits,skips,profile,limit,COUNT,COUNT*SCANS,pages,program_bytes,searches,nodes,stats[4],stats[2],captured,restore);
    store_free(s);
}
