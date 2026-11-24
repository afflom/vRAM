// Native end-to-end tensor workload. Input rows have 64 pixel bytes and label.
#include "pager.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { TRAIN=1200, ALL=1797, TEST=ALL-TRAIN, SIDE=64, CLASSES=10, BLOCKS=(TEST+63)/64, REPEATS=20 };
static uint8_t data[ALL][65], tiles[BLOCKS][4096];
static int32_t means[CLASSES][SIDE];
static int64_t norm[CLASSES];
static int results[2][TEST];
static uint64_t now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;
}
static int classify(struct store *s,int mode) {
    int correct=0;
    for (int b=0;b<BLOCKS;b++) {
        int64_t best[SIDE];
        for (int c=0;c<CLASSES;c++) {
            int64_t dots[SIDE]={0};
            if (mode) {
                if (store_project(s,b,means[c],dots)<0) exit(3);
            } else {
                for (int f=0;f<SIDE;f++)
                    for (int col=0;col<SIDE;col++)
                        dots[col]+=(int64_t)means[c][f]*tiles[b][f*SIDE+col];
            }
            for (int col=0;col<SIDE && b*SIDE+col<TEST;col++){
                int64_t score=2*dots[col]-norm[c];
                if (c==0 || score>best[col]) {
                    best[col]=score; results[mode][b*SIDE+col]=c;
                }
            }
        }
    }
    for (int i=0;i<TEST;i++) correct+=results[mode][i]==data[TRAIN+i][64];
    return correct;
}
int main(int argc,char **argv) {
    if (argc!=2) return 1;
    FILE *f=fopen(argv[1],"rb");
    if (!f || fread(data,sizeof data,1,f)!=1 || fgetc(f)!=EOF) return 1;
    fclose(f);
    int count[CLASSES]={0};
    for (int i=0;i<TRAIN;i++) {
        int c=data[i][64]; if(c>=CLASSES) return 1; count[c]++;
        for (int j=0;j<SIDE;j++) means[c][j]+=data[i][j];
    }
    for (int c=0;c<CLASSES;c++) {
        if (!count[c]) return 1;
        for (int j=0;j<SIDE;j++) {
            means[c][j]=(means[c][j]+count[c]/2)/count[c];
            norm[c]+=(int64_t)means[c][j]*means[c][j];
        }
    }
    struct store *s=store_new(BLOCKS);
    if (!s) return 2;
    for (int i=0;i<TEST;i++)
        for (int j=0;j<SIDE;j++) tiles[i/SIDE][j*SIDE+i%SIDE]=data[TRAIN+i][j];
    for (int b=0;b<BLOCKS;b++) if (store_put(s,b,tiles[b])) return 2;
    int correct=classify(s,0);
    if (classify(s,1)!=correct || memcmp(results[0],results[1],sizeof results[0])) return 3;
    uint64_t sizes[5]; store_stats(s,sizes);
    printf("held_out=%d correct=%d encoded_and_index_B=%lu dense_padded_B=%zu packed_tiles=%lu\n",TEST,correct,sizes[4],sizeof tiles,sizes[1]);
    volatile int observed=0;
    for (int trial=0;trial<7;trial++) {
        uint64_t ns[2];
        for (int arm=0;arm<2;arm++) {
            int mode=(trial+arm)%2; uint64_t start=now();
            for (int rep=0;rep<REPEATS;rep++) observed+=classify(s,mode);
            ns[mode]=(now()-start)/REPEATS;
        }
        printf("trial=%d dense_ns=%lu device_ns=%lu\n",trial,ns[0],ns[1]);
    }
    store_free(s); return observed!=7*2*REPEATS*correct;
}
