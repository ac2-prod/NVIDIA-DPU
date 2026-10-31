// benchmark_omp_scaling.c — 論文 Fig 4 (スレッドスケーリング) / Fig 5 (スレッド別フェーズ内訳)
// ─────────────────────────────────────────────────────────────────────────────
// 16 Arm コアの OpenMP 並列が各処理フェーズ (preproc / compress / decompress /
// restore) にどう効くかを定量化する。スレッド数 1,2,4,8,16 を振り、各フェーズの
// 時間(ms)・スループット(GB/s)・speedup(p)=T(1)/T(p)・efficiency=speedup/p を測る。
//
//   主役構成: diff_byte + LZ4 (開発中に E2E で使っていた構成。diff_byte は非可逆で
//             論文の評価対象外。論文の E2E は byte grouping + LZ4)
//   対照構成: none(恒等) + Snappy (高スループットだが圧縮しない = スケールするが無意味)
//
// 計時: 各フェーズ独立 OMP 領域 (num_threads(p)) + clock_gettime(MONOTONIC),
//   256KB サブチャンク連続割当 (run_direct 準拠), 出力フルサイズ配列 (DRAM write 込み),
//   warmup 後 median。スループット基準 = 論理元サイズ (decimal GB/s)。
//   OMP_DYNAMIC=false を強制し、affinity は環境 (OMP_PLACES=cores, PROC_BIND=close) に従う。
//
// 出力: results_omp_scaling.csv
//   列: threads,preproc,codec,phase,time_ms,gbps,speedup_vs_1t,efficiency,
//       comp_ratio,commit_hash,timestamp
//
// 使い方: ./benchmark_omp_scaling [--data PATH] [--out CSV] [--iters N] [--warmup N] [--sub-kb K]
// ─────────────────────────────────────────────────────────────────────────────
#define _GNU_SOURCE
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "netzip.h"
#include "lz4.h"
#include "snappy-c.h"

#define DEFAULT_DATA "../../bf3_data/forward/0212_lm_head.pkl.bin"
#define MAX_ITERS 64

enum { PRE_DIFF_BYTE = 0, PRE_NONE };
enum { COD_LZ4 = 0, COD_SNAPPY };
enum { PH_PREPROC = 0, PH_COMPRESS, PH_DECOMPRESS, PH_RESTORE, PH_N };
static const char *ph_name[] = { "preproc", "compress", "decompress", "restore" };

static const int THREADS[] = { 1, 2, 4, 8, 16 };
#define N_THR ((int)(sizeof(THREADS)/sizeof(THREADS[0])))

static double now_s(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (double)t.tv_sec + (double)t.tv_nsec*1e-9; }
static int cmp_d(const void*a,const void*b){ double x=*(const double*)a,y=*(const double*)b;
    return (x>y)-(x<y); }
static double median(double*v,int n){ qsort(v,n,sizeof(double),cmp_d);
    return (n&1)?v[n/2]:0.5*(v[n/2-1]+v[n/2]); }
static long file_size(const char*p){ FILE*f=fopen(p,"rb"); if(!f)return -1;
    fseek(f,0,SEEK_END); long n=ftell(f); fclose(f); return n; }
static void get_commit(char*o,size_t n){ snprintf(o,n,"nogit");
    FILE*p=popen("git rev-parse --short HEAD 2>/dev/null","r");
    if(p){ char b[64]; if(fgets(b,sizeof b,p)){ b[strcspn(b,"\n")]=0; if(b[0]) snprintf(o,n,"%s",b);} pclose(p);} }

// ─── グローバル作業バッファ ──────────────────────────────────────────────────
static size_t  g_sub_bytes, g_sub_elem, g_bound, g_nsub, g_proc_bytes;
static uint16_t *g_src, *g_restored;
static uint8_t  *g_preproc, *g_comp, *g_decomp;
static size_t   *g_csize;
static float    g_shift;

static inline void range_p(int t,int p,size_t*s0,size_t*s1){
    size_t per=(g_nsub+(size_t)p-1)/(size_t)p;
    *s0=(size_t)t*per; *s1=*s0+per;
    if(*s0>g_nsub) *s0=g_nsub;
    if(*s1>g_nsub) *s1=g_nsub;
}

static void phase_preproc(int pre,int p){
    #pragma omp parallel num_threads(p)
    { size_t s0,s1; range_p(omp_get_thread_num(),p,&s0,&s1);
      for(size_t s=s0;s<s1;s++){
        const uint16_t*in=g_src+s*g_sub_elem; uint8_t*out=g_preproc+s*g_sub_bytes;
        if(pre==PRE_DIFF_BYTE) netzip_diff_byte_group_serial(in,g_sub_elem,g_shift,out);
        else                   memcpy(out,in,g_sub_bytes);   // 恒等 (DRAM write 込み)
      } }
}
static void phase_restore(int pre,int p){
    #pragma omp parallel num_threads(p)
    { size_t s0,s1; range_p(omp_get_thread_num(),p,&s0,&s1);
      for(size_t s=s0;s<s1;s++){
        const uint8_t*in=g_decomp+s*g_sub_bytes; uint16_t*out=g_restored+s*g_sub_elem;
        if(pre==PRE_DIFF_BYTE) netzip_diff_byte_ungroup_serial(in,g_sub_elem,g_shift,out);
        else                   memcpy(out,in,g_sub_bytes);
      } }
}
static void phase_compress(int cod,int p){
    #pragma omp parallel num_threads(p)
    { size_t s0,s1; range_p(omp_get_thread_num(),p,&s0,&s1);
      for(size_t s=s0;s<s1;s++){
        const char*src=(const char*)(g_preproc+s*g_sub_bytes);
        char*dst=(char*)(g_comp+s*g_bound); size_t csz=0;
        if(cod==COD_LZ4){ int r=LZ4_compress_default(src,dst,(int)g_sub_bytes,(int)g_bound);
            csz=(r>0)?(size_t)r:0; }
        else { size_t out=g_bound; if(snappy_compress(src,g_sub_bytes,dst,&out)==SNAPPY_OK) csz=out; }
        g_csize[s]=csz;
      } }
}
static void phase_decompress(int cod,int p){
    #pragma omp parallel num_threads(p)
    { size_t s0,s1; range_p(omp_get_thread_num(),p,&s0,&s1);
      for(size_t s=s0;s<s1;s++){
        const char*src=(const char*)(g_comp+s*g_bound);
        char*dst=(char*)(g_decomp+s*g_sub_bytes);
        if(g_csize[s]==0){ memset(dst,0,g_sub_bytes); continue; }
        if(cod==COD_LZ4) LZ4_decompress_safe(src,dst,(int)g_csize[s],(int)g_sub_bytes);
        else { size_t out=g_sub_bytes; snappy_uncompress(src,g_csize[s],dst,&out); }
      } }
}

int main(int argc,char**argv){
    const char*data=DEFAULT_DATA, *out_csv="results_omp_scaling.csv";
    int iters=7, warmup=3; size_t sub_kb=256;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--data")&&i+1<argc) data=argv[++i];
        else if(!strcmp(argv[i],"--out")&&i+1<argc) out_csv=argv[++i];
        else if(!strcmp(argv[i],"--iters")&&i+1<argc) iters=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--warmup")&&i+1<argc) warmup=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--sub-kb")&&i+1<argc) sub_kb=(size_t)atoi(argv[++i]);
        else { fprintf(stderr,"unknown arg %s\n",argv[i]); return 1; }
    }
    if(iters>MAX_ITERS) iters=MAX_ITERS;
    omp_set_dynamic(0);          // 動的スレッド数を無効化 (要求した p をそのまま使う)

    long fsz=file_size(data);
    if(fsz<(long)(sub_kb*1024)){ fprintf(stderr,"data too small: %ld\n",fsz); return 1; }
    g_sub_bytes=sub_kb*1024; g_sub_elem=g_sub_bytes/2;
    g_nsub=(size_t)fsz/g_sub_bytes; g_proc_bytes=g_nsub*g_sub_bytes;
    g_bound=(size_t)LZ4_compressBound((int)g_sub_bytes);
    { size_t b=snappy_max_compressed_length(g_sub_bytes); if(b>g_bound)g_bound=b; }

    g_src=(uint16_t*)malloc(g_proc_bytes);
    g_preproc=(uint8_t*)malloc(g_proc_bytes);
    g_comp=(uint8_t*)malloc(g_nsub*g_bound);
    g_decomp=(uint8_t*)malloc(g_proc_bytes);
    g_restored=(uint16_t*)malloc(g_proc_bytes);
    g_csize=(size_t*)malloc(g_nsub*sizeof(size_t));
    if(!g_src||!g_preproc||!g_comp||!g_decomp||!g_restored||!g_csize){ fprintf(stderr,"alloc\n"); return 1; }
    FILE*f=fopen(data,"rb");
    if(!f||fread(g_src,1,g_proc_bytes,f)!=g_proc_bytes){ fprintf(stderr,"read %s\n",data); return 1; }
    fclose(f);
    g_shift=netzip_find_min(g_src,g_proc_bytes/2);

    char commit[64],ts[32]; get_commit(commit,sizeof commit);
    time_t tt=time(NULL); strftime(ts,sizeof ts,"%Y-%m-%dT%H:%M:%S",localtime(&tt));
    int hdr=(file_size(out_csv)<0);
    FILE*csv=fopen(out_csv,"a"); if(!csv){ perror(out_csv); return 1; }
    if(hdr) fprintf(csv,"threads,preproc,codec,phase,time_ms,gbps,speedup_vs_1t,efficiency,"
                        "comp_ratio,commit_hash,timestamp\n");

    printf("=== OMP scaling: %s  %.1fMB  nsub=%zu  sub=%zuKB  iters=%d (warmup %d) ===\n",
           data,g_proc_bytes/1e6,g_nsub,sub_kb,iters,warmup);
    printf("OMP_DYNAMIC=%d  max_threads=%d\n\n",omp_get_dynamic(),omp_get_max_threads());

    struct Conf { int pre,cod; const char*pname,*cname; } confs[] = {
        { PRE_DIFF_BYTE, COD_LZ4,    "diff_byte", "lz4"    },  // 主役 (開発中の E2E 構成, lossy)
        { PRE_NONE,      COD_SNAPPY, "none",      "snappy" },  // 対照 (スケールするが無圧縮)
    };
    int nconf=(int)(sizeof(confs)/sizeof(confs[0]));

    for(int ci=0;ci<nconf;ci++){
        int pre=confs[ci].pre, cod=confs[ci].cod;
        double t_ms[N_THR][PH_N];        // median 時間
        double ratio[N_THR];
        printf("--- %s + %s ---\n", confs[ci].pname, confs[ci].cname);
        for(int ti=0;ti<N_THR;ti++){
            int p=THREADS[ti];
            double tp[MAX_ITERS],tc[MAX_ITERS],td[MAX_ITERS],tr[MAX_ITERS];
            size_t comp_bytes=0;
            for(int it=0;it<warmup+iters;it++){
                double a;
                a=now_s(); phase_preproc(pre,p);    double x0=now_s()-a;
                a=now_s(); phase_compress(cod,p);    double x1=now_s()-a;
                a=now_s(); phase_decompress(cod,p);  double x2=now_s()-a;
                a=now_s(); phase_restore(pre,p);     double x3=now_s()-a;
                if(it>=warmup){ int j=it-warmup;
                    tp[j]=x0*1e3; tc[j]=x1*1e3; td[j]=x2*1e3; tr[j]=x3*1e3; }
            }
            t_ms[ti][PH_PREPROC]=median(tp,iters);
            t_ms[ti][PH_COMPRESS]=median(tc,iters);
            t_ms[ti][PH_DECOMPRESS]=median(td,iters);
            t_ms[ti][PH_RESTORE]=median(tr,iters);
            comp_bytes=0; for(size_t s=0;s<g_nsub;s++) comp_bytes+=g_csize[s];
            ratio[ti]=comp_bytes?(double)g_proc_bytes/(double)comp_bytes:0;
            printf("  %2dT: pre/cmp/dec/res = %6.2f/%6.2f/%6.2f/%6.2f ms  ratio=%.3f\n",
                   p,t_ms[ti][0],t_ms[ti][1],t_ms[ti][2],t_ms[ti][3],ratio[ti]);
        }
        // speedup / efficiency (vs 1 スレッド) を計算して書き出し
        for(int ph=0;ph<PH_N;ph++){
            double t1=t_ms[0][ph];
            for(int ti=0;ti<N_THR;ti++){
                int p=THREADS[ti]; double tm=t_ms[ti][ph];
                double sp=(tm>0)?t1/tm:0, eff=(p>0)?sp/p:0;
                double bg=(tm>0)?(double)g_proc_bytes/(tm/1e3)/1e9:0;
                fprintf(csv,"%d,%s,%s,%s,%.4f,%.3f,%.4f,%.4f,%.4f,%s,%s\n",
                    p,confs[ci].pname,confs[ci].cname,ph_name[ph],
                    tm,bg,sp,eff,ratio[ti],commit,ts);
            }
        }
        printf("\n");
    }
    fclose(csv);
    printf("CSV -> %s\n",out_csv);
    free(g_src);free(g_preproc);free(g_comp);free(g_decomp);free(g_restored);free(g_csize);
    return 0;
}
