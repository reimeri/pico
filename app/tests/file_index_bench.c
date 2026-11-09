#define _GNU_SOURCE
#include "file_index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

static double Now(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9; }
static double cpu(void)
{ struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);return t.tv_sec+t.tv_nsec/1e9; }
static void Pause(void) { struct timespec t={.tv_nsec=100000};nanosleep(&t,NULL); }
static double max_call_ms;
static PicoFileIndexResult Query(PicoFileIndex *i,uint64_t token,const char *q,double *maincpu)
{
    PicoFileIndexResult r;
    do
    {
        double start=cpu(),wall=Now(); PicoFileIndex_Query(i,token,q,&r);
        double call=(Now()-wall)*1000; if(call>max_call_ms)max_call_ms=call;
        *maincpu+=cpu()-start;if(r.pending)Pause();
    }while(r.pending);
    return r;
}
static int Compare(const void *a,const void *b)
{ double x=*(const double *)a,y=*(const double *)b;return (x>y)-(x<y); }
int main(int argc,char **argv)
{
    if(argc<3){fprintf(stderr,"usage: %s ROOT cold|cached [query]\n",argv[0]);return 2;}
    double begin=Now(), maincpu=0,openstart=cpu();
    PicoFileIndex *i=PicoFileIndex_Open(argv[1]);
    maincpu+=cpu()-openstart;if(!i)return 2;
    PicoFileIndexResult first=Query(i,1,argc>3?argv[3]:"target_notes.md",&maincpu);
    double firstms=(Now()-begin)*1000;
    PicoFileIndexInfo info;PicoFileIndex_Info(i,&info);
    printf("{\"mode\":\"%s\",\"first_ms\":%.3f,\"first_query_main_cpu_ms\":%.3f,\"first_cached\":%s,",argv[2],firstms,maincpu*1000,(info.cached || first.generation < info.generation)?"true":"false");
    do{PicoFileIndex_Info(i,&info);if(!info.generation||info.cached||info.refreshing)Pause();}while(!info.generation||info.cached||info.refreshing);
    printf("\"files\":%zu,\"index_bytes\":%zu,\"discovery_ms\":%.3f,\"queries\":[",info.files,info.bytes,info.discovery_ms);
    const char *queries[]={"target_notes.md","component_009","src/group_00009/","00999.gd","no-such-file-anywhere"};
    uint64_t token=2;
    for(size_t q=0;q<sizeof(queries)/sizeof(queries[0]);q++)
    {
        double times[31],calls[31],search[31],wall[31];
        int count=0;
        for(int n=0;n<31;n++)
        {
            double m=0,start=Now();max_call_ms=0;PicoFileIndexResult r=Query(i,token++,queries[q],&m);
            times[n]=(Now()-start)*1000;calls[n]=m*1000;wall[n]=max_call_ms;count=r.count;
            PicoFileIndex_Info(i,&info);search[n]=info.search_ms;
        }
        qsort(times,31,sizeof(double),Compare);qsort(calls,31,sizeof(double),Compare);qsort(search,31,sizeof(double),Compare);qsort(wall,31,sizeof(double),Compare);
        printf("%s{\"query\":\"%s\",\"results\":%d,\"p50_ms\":%.3f,\"p95_ms\":%.3f,\"worker_p50_ms\":%.3f,\"main_cpu_p95_ms\":%.3f,\"main_call_p95_ms\":%.3f}",q?",":"",queries[q],count,times[15],times[29],search[15],calls[29],wall[29]);
    }
    double editstart=Now(); max_call_ms=0;
    for(int n=0;n<1000;n++) { PicoFileIndexResult r; PicoFileIndex_Query(i,token,n%2?"no-such-file-anywhere":"component_009",&r); }
    maincpu=0; PicoFileIndexResult edited=Query(i,token,"target_notes.md",&maincpu);
    double editms=(Now()-editstart)*1000;
    double start=Now();uint64_t previous=info.generation;PicoFileIndex_Refresh(i);
    do{PicoFileIndex_Info(i,&info);if(info.generation<=previous||info.refreshing)Pause();}while(info.generation<=previous||info.refreshing);
    struct rusage usage;getrusage(RUSAGE_SELF,&usage);
    printf("],\"coalesced_1000_edits_ms\":%.3f,\"latest_edit_results\":%d,\"refresh_ms\":%.3f,\"peak_rss_kib\":%ld,\"status\":\"%s\"}\n",editms,edited.count,(Now()-start)*1000,usage.ru_maxrss,info.status);
    PicoFileIndex_Close(i);return first.count?0:1;
}
