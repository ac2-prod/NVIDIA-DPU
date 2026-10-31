// benchmark_generalization.c — データセット一般化測定 (Llama-3.2-1B 全 423 テンソル)
// ─────────────────────────────────────────────────────────────────────────────
// forward/ (活性) と grad/ (勾配) の全 .pkl.bin について、4 前処理 ×
// 3 コーデックの LZ4 圧縮率・各フェーズスループット・bit-exact を測る。
//
//   前処理: byte_group / bit_group (純並べ替え, lossless) /
//           diff_byte (float, ±1ULP lossy) / idiff_byte (整数 diff, lossless)
//   コーデック: lz4 (block, 主役) / snappy / zstd-3 (Fig3 Pareto 用)
//
// 計時: 各フェーズ独立 OMP 領域 + clock_gettime(MONOTONIC), 16T, 256KB サブチャンク
//   連続割当 (run_direct 準拠), 出力はフルサイズ配列へ (DRAM write 込み), warmup 後 median。
//   スループット基準 = 論理元サイズ (decimal GB/s)。
//
// 堅牢性: テンソル毎に分類 (category/layer_idx/act_or_grad) し、小さすぎる場合は
//   サブチャンク数 1 にフォールバック。1 個の失敗で全体を止めず、status 列に
//   ok / skip:理由 / error:理由 を記録して続行。末尾でエラー一覧を表示。
//
// 使い方:
//   ./benchmark_generalization [--out CSV] [--iters N] [--warmup N] [--sub-kb K]
//                              file1 [file2 ...]
//   ファイルパスに /forward/ か /grad/ を含むことで活性/勾配を判定する。
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
#include "zstd.h"
#include "snappy-c.h"

#define MAX_ITERS 64
#define ZSTD_LEVEL 3                 // Pareto の高速端 (zstd 既定)

enum { PRE_BYTE_GROUP = 0, PRE_BIT_GROUP, PRE_DIFF_BYTE, PRE_IDIFF_BYTE, PRE_N };
static const char *pre_name[] = { "byte_group", "bit_group", "diff_byte", "idiff_byte" };
enum { COD_LZ4 = 0, COD_SNAPPY, COD_ZSTD, COD_N };
static const char *cod_name[] = { "lz4", "snappy", "zstd" };

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

// ─── 分類 (ファイル名 + パス) ───────────────────────────────────────────────
static const char *basename_of(const char*path){
    const char*s=strrchr(path,'/'); return s?s+1:path; }

// "NNNN_" 接頭と ".pkl.bin" を除いたテンソル名を out へ
static void tensor_name(const char*path,char*out,size_t n){
    const char*b=basename_of(path);
    while(*b>='0'&&*b<='9') b++;        // 数値接頭
    if(*b=='_') b++;
    snprintf(out,n,"%s",b);
    char*dot=strstr(out,".pkl.bin"); if(dot)*dot=0;
}
static const char *classify(const char*name){
    if(strstr(name,"self_attn.q_proj")) return "attn_q";
    if(strstr(name,"self_attn.k_proj")) return "attn_k";
    if(strstr(name,"self_attn.v_proj")) return "attn_v";
    if(strstr(name,"self_attn.o_proj")) return "attn_o";
    if(strstr(name,"mlp.gate_proj"))    return "mlp_gate";
    if(strstr(name,"mlp.up_proj"))      return "mlp_up";
    if(strstr(name,"mlp.down_proj"))    return "mlp_down";
    if(strstr(name,"mlp.act_fn"))       return "mlp_actfn";
    if(strstr(name,"input_layernorm"))         return "layernorm_input";
    if(strstr(name,"post_attention_layernorm"))return "layernorm_postattn";
    // 接尾一致 (より一般のものは後で)
    size_t L=strlen(name);
    #define ENDS(suf) (L>=strlen(suf) && strcmp(name+L-strlen(suf),suf)==0)
    if(ENDS(".self_attn")) return "attn_combined";
    if(ENDS(".mlp"))       return "mlp_combined";
    if(strstr(name,"lm_head"))      return "lm_head";
    if(strstr(name,"embed_tokens")) return "embed";
    if(strstr(name,"rotary_emb"))   return "rotary";
    if(strstr(name,"model.norm"))   return "norm";
    // "model.layers.<N>" でこれ以上の接尾が無い = レイヤブロック全体
    if(strstr(name,"model.layers.")){
        // 末尾が数字なら layer_block
        if(name[L-1]>='0'&&name[L-1]<='9') return "layer_block";
    }
    #undef ENDS
    return "other";
}
static int layer_index(const char*name){
    const char*p=strstr(name,"model.layers."); if(!p) return -1;
    p+=strlen("model.layers."); int v=-1;
    if(*p>='0'&&*p<='9'){ v=0; while(*p>='0'&&*p<='9'){ v=v*10+(*p-'0'); p++; } }
    return v;
}
static const char *act_or_grad(const char*path){
    if(strstr(path,"/grad/"))    return "gradient";
    if(strstr(path,"/forward/")) return "activation";
    return "unknown";
}

// ─── グローバル作業バッファ (テンソル毎に再確保) ─────────────────────────────
static size_t  g_sub_bytes, g_sub_elem, g_bound, g_nsub, g_proc_bytes;
static int     g_nth;
static uint16_t *g_src;
static uint8_t  *g_preproc, *g_comp, *g_decomp;
static uint16_t *g_restored;
static size_t   *g_csize;
static float    g_shift;

static inline void block_range(int t,size_t*s0,size_t*s1){
    size_t per=(g_nsub+(size_t)g_nth-1)/(size_t)g_nth;
    *s0=(size_t)t*per; *s1=*s0+per;
    if(*s0>g_nsub) *s0=g_nsub;
    if(*s1>g_nsub) *s1=g_nsub;
}

static void phase_preproc(int pre){
    #pragma omp parallel num_threads(g_nth)
    { size_t s0,s1; block_range(omp_get_thread_num(),&s0,&s1);
      for(size_t s=s0;s<s1;s++){
        const uint16_t*in=g_src+s*g_sub_elem; uint8_t*out=g_preproc+s*g_sub_bytes;
        switch(pre){
        case PRE_BYTE_GROUP: netzip_byte_group_serial(in,g_sub_elem,out); break;
        case PRE_BIT_GROUP:  netzip_bit_group_serial(in,g_sub_elem,out); break;
        case PRE_DIFF_BYTE:  netzip_diff_byte_group_serial(in,g_sub_elem,g_shift,out); break;
        case PRE_IDIFF_BYTE: netzip_idiff_byte_group_serial(in,g_sub_elem,out); break;
        } } }
}
static void phase_restore(int pre){
    #pragma omp parallel num_threads(g_nth)
    { size_t s0,s1; block_range(omp_get_thread_num(),&s0,&s1);
      for(size_t s=s0;s<s1;s++){
        const uint8_t*in=g_decomp+s*g_sub_bytes; uint16_t*out=g_restored+s*g_sub_elem;
        switch(pre){
        case PRE_BYTE_GROUP: netzip_byte_ungroup_serial(in,g_sub_elem,out); break;
        case PRE_BIT_GROUP:  netzip_bit_ungroup_serial(in,g_sub_elem,out); break;
        case PRE_DIFF_BYTE:  netzip_diff_byte_ungroup_serial(in,g_sub_elem,g_shift,out); break;
        case PRE_IDIFF_BYTE: netzip_idiff_byte_ungroup_serial(in,g_sub_elem,out); break;
        } } }
}
// codec 圧縮: 各サブチャンクを g_comp[s*g_bound..] へ。失敗(<=0)は g_csize=0。
static void phase_compress(int cod){
    #pragma omp parallel num_threads(g_nth)
    { size_t s0,s1; block_range(omp_get_thread_num(),&s0,&s1);
      for(size_t s=s0;s<s1;s++){
        const char*src=(const char*)(g_preproc+s*g_sub_bytes);
        char*dst=(char*)(g_comp+s*g_bound); size_t csz=0;
        if(cod==COD_LZ4){
            int r=LZ4_compress_default(src,dst,(int)g_sub_bytes,(int)g_bound);
            csz=(r>0)?(size_t)r:0;
        } else if(cod==COD_SNAPPY){
            size_t out=g_bound;
            if(snappy_compress(src,g_sub_bytes,dst,&out)==SNAPPY_OK) csz=out;
        } else { // ZSTD
            size_t r=ZSTD_compress(dst,g_bound,src,g_sub_bytes,ZSTD_LEVEL);
            csz=ZSTD_isError(r)?0:r;
        }
        g_csize[s]=csz;
      } }
}
static void phase_decompress(int cod){
    #pragma omp parallel num_threads(g_nth)
    { size_t s0,s1; block_range(omp_get_thread_num(),&s0,&s1);
      for(size_t s=s0;s<s1;s++){
        const char*src=(const char*)(g_comp+s*g_bound);
        char*dst=(char*)(g_decomp+s*g_sub_bytes);
        if(g_csize[s]==0){ memset(dst,0,g_sub_bytes); continue; }
        if(cod==COD_LZ4){
            LZ4_decompress_safe(src,dst,(int)g_csize[s],(int)g_sub_bytes);
        } else if(cod==COD_SNAPPY){
            size_t out=g_sub_bytes; snappy_uncompress(src,g_csize[s],dst,&out);
        } else {
            ZSTD_decompress(dst,g_sub_bytes,src,g_csize[s]);
        }
      } }
}

static FILE *g_csv;
static char  g_commit[64], g_ts[32];

// 1 行書き出し
static void emit(const char*tensor,const char*cat,int layer,const char*ag,
                 long size_bytes,const char*pre,const char*cod,
                 double ratio,const char*bitx,long mism,
                 double gp,double gc,double gd,double gr,size_t nsub,
                 const char*status){
    fprintf(g_csv,"%s,%s,%d,%s,%ld,%s,%s,%.4f,%s,%ld,"
                  "%.3f,%.3f,%.3f,%.3f,%d,%zu,%zu,%s,%s,%s\n",
        tensor,cat,layer,ag,size_bytes,pre,cod,ratio,bitx,mism,
        gp,gc,gd,gr,g_nth,g_sub_bytes,nsub,g_commit,g_ts,status);
}

// エラー集計
static char g_errlist[512][256]; static int g_nerr=0;
static void note_err(const char*tensor,const char*reason){
    if(g_nerr<512) snprintf(g_errlist[g_nerr++],256,"%-44s %s",tensor,reason);
}

int main(int argc,char**argv){
    const char*out_csv="results_generalization.csv";
    int iters=5, warmup=2;
    size_t sub_kb=256;
    const char*files[1024]; int nf=0;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--out")&&i+1<argc) out_csv=argv[++i];
        else if(!strcmp(argv[i],"--iters")&&i+1<argc) iters=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--warmup")&&i+1<argc) warmup=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--sub-kb")&&i+1<argc) sub_kb=(size_t)atoi(argv[++i]);
        else if(argv[i][0]=='-'){ fprintf(stderr,"unknown arg %s\n",argv[i]); return 1; }
        else if(nf<1024) files[nf++]=argv[i];
    }
    if(nf==0){ fprintf(stderr,"no input files\n"); return 1; }
    if(iters>MAX_ITERS) iters=MAX_ITERS;
    g_nth=omp_get_max_threads();
    get_commit(g_commit,sizeof g_commit);
    time_t tt=time(NULL); strftime(g_ts,sizeof g_ts,"%Y-%m-%dT%H:%M:%S",localtime(&tt));

    int hdr=(file_size(out_csv)<0);
    g_csv=fopen(out_csv,"a"); if(!g_csv){ perror(out_csv); return 1; }
    if(hdr) fprintf(g_csv,
        "tensor,category,layer_idx,act_or_grad,size_bytes,preproc,codec,"
        "comp_ratio,bit_exact,mismatch_bytes,"
        "gbps_preproc,gbps_compress,gbps_decompress,gbps_restore,"
        "omp_threads,sub_bytes,nsub,commit_hash,timestamp,status\n");

    printf("=== generalization: %d tensors, %d preproc × %d codec, %dT, iters=%d ===\n",
           nf,PRE_N,COD_N,g_nth,iters);
    int n_ok=0,n_skip=0;

    for(int fi=0;fi<nf;fi++){
        const char*path=files[fi];
        char tname[256]; tensor_name(path,tname,sizeof tname);
        const char*cat=classify(tname); int layer=layer_index(tname);
        const char*ag=act_or_grad(path);
        long fsz=file_size(path);
        if(fsz<2 || (fsz&1)){
            emit(tname,cat,layer,ag,fsz,"na","na",0,"na",-1,0,0,0,0,0,"skip:too_small_or_odd");
            note_err(tname,"skip:too_small_or_odd"); n_skip++; continue;
        }
        // 有効サブチャンクサイズ: ファイルが既定 sub 未満なら全体を 1 サブチャンクに
        g_sub_bytes = sub_kb*1024;
        if((size_t)fsz < g_sub_bytes) g_sub_bytes=(size_t)fsz;  // フォールバック
        g_sub_elem  = g_sub_bytes/2;
        g_nsub      = (size_t)fsz/g_sub_bytes;
        g_proc_bytes= g_nsub*g_sub_bytes;                       // 端数は切捨て
        if(g_nsub==0){
            emit(tname,cat,layer,ag,fsz,"na","na",0,"na",-1,0,0,0,0,0,"skip:no_subchunk");
            note_err(tname,"skip:no_subchunk"); n_skip++; continue;
        }
        g_bound=ZSTD_compressBound(g_sub_bytes);
        { size_t b2=(size_t)LZ4_compressBound((int)g_sub_bytes); if(b2>g_bound)g_bound=b2;
          size_t b3=snappy_max_compressed_length(g_sub_bytes); if(b3>g_bound)g_bound=b3; }

        g_src     =(uint16_t*)malloc(g_proc_bytes);
        g_preproc =(uint8_t*) malloc(g_proc_bytes);
        g_comp    =(uint8_t*) malloc(g_nsub*g_bound);
        g_decomp  =(uint8_t*) malloc(g_proc_bytes);
        g_restored=(uint16_t*)malloc(g_proc_bytes);
        g_csize   =(size_t*)  malloc(g_nsub*sizeof(size_t));
        if(!g_src||!g_preproc||!g_comp||!g_decomp||!g_restored||!g_csize){
            emit(tname,cat,layer,ag,fsz,"na","na",0,"na",-1,0,0,0,0,g_nsub,"error:alloc");
            note_err(tname,"error:alloc"); n_skip++;
            free(g_src);free(g_preproc);free(g_comp);free(g_decomp);free(g_restored);free(g_csize);
            continue;
        }
        FILE*f=fopen(path,"rb");
        if(!f || fread(g_src,1,g_proc_bytes,f)!=g_proc_bytes){
            if(f)fclose(f);
            emit(tname,cat,layer,ag,fsz,"na","na",0,"na",-1,0,0,0,0,g_nsub,"error:read");
            note_err(tname,"error:read"); n_skip++;
            free(g_src);free(g_preproc);free(g_comp);free(g_decomp);free(g_restored);free(g_csize);
            continue;
        }
        fclose(f);
        g_shift=netzip_find_min(g_src,g_proc_bytes/2);

        printf("[%3d/%3d] %-44s %-18s L%-3d %-10s %5.1fMB nsub=%zu\n",
               fi+1,nf,tname,cat,layer,ag,g_proc_bytes/1e6,g_nsub);

        for(int pre=0;pre<PRE_N;pre++){
            // bit_group は sub_bytes が 16 の倍数 (=sub_elem%8==0) でないと壊れる
            if(pre==PRE_BIT_GROUP && (g_sub_bytes%16)){
                for(int cod=0;cod<COD_N;cod++)
                    emit(tname,cat,layer,ag,fsz,pre_name[pre],cod_name[cod],
                         0,"na",-1,0,0,0,0,g_nsub,"skip:bit_group_needs_16x");
                note_err(tname,"skip:bit_group_needs_16x"); n_skip++;
                continue;
            }
            for(int cod=0;cod<COD_N;cod++){
                double tp[MAX_ITERS],tc[MAX_ITERS],td[MAX_ITERS],tr[MAX_ITERS];
                size_t comp_bytes=0; int compress_failed=0;
                for(int it=0;it<warmup+iters;it++){
                    double a;
                    a=now_s(); phase_preproc(pre);    double t_pre=now_s()-a;
                    a=now_s(); phase_compress(cod);    double t_cmp=now_s()-a;
                    a=now_s(); phase_decompress(cod);  double t_dec=now_s()-a;
                    a=now_s(); phase_restore(pre);     double t_res=now_s()-a;
                    if(it>=warmup){ int j=it-warmup;
                        tp[j]=t_pre*1e3; tc[j]=t_cmp*1e3; td[j]=t_dec*1e3; tr[j]=t_res*1e3; }
                }
                for(size_t s=0;s<g_nsub;s++){
                    if(g_csize[s]==0) compress_failed=1;
                    comp_bytes+=g_csize[s];
                }
                long mism=0;
                const uint8_t*o=(const uint8_t*)g_src,*r=(const uint8_t*)g_restored;
                for(size_t b=0;b<g_proc_bytes;b++) if(o[b]!=r[b]) mism++;
                int bx=(mism==0);
                double mp=median(tp,iters),mc=median(tc,iters),
                       md=median(td,iters),mr=median(tr,iters);
                double bg=(double)g_proc_bytes/1e9;
                double ratio=comp_bytes?(double)g_proc_bytes/(double)comp_bytes:0;
                const char*status = compress_failed?"error:codec_compress":"ok";
                emit(tname,cat,layer,ag,fsz,pre_name[pre],cod_name[cod],
                     ratio,bx?"yes":"no",mism,
                     bg/(mp/1e3),bg/(mc/1e3),bg/(md/1e3),bg/(mr/1e3),
                     g_nsub,status);
                if(compress_failed) note_err(tname,"error:codec_compress");
                else n_ok++;
            }
        }
        fflush(g_csv);
        free(g_src);free(g_preproc);free(g_comp);free(g_decomp);free(g_restored);free(g_csize);
    }
    fclose(g_csv);

    printf("\n=== 完了: ok 行=%d, skip/error 行=%d ===\n",n_ok,n_skip);
    if(g_nerr){
        printf("--- 計測できなかった/問題のあったテンソル (%d) ---\n",g_nerr);
        for(int i=0;i<g_nerr;i++) printf("  %s\n",g_errlist[i]);
    } else printf("(全テンソル正常)\n");
    printf("CSV -> %s\n",out_csv);
    return 0;
}
