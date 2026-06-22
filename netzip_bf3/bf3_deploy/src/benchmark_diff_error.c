// benchmark_diff_error.c — diff_byte (float diff) 復元誤差の大きさ測定 (ドラフト §3.6)
// ─────────────────────────────────────────────────────────────────────────────
// これまで mismatch_bytes (バイト数) しか測っていなかった diff_byte の復元誤差を、
// uint16(bf16) 要素単位で「何 ULP ずれるか」まで定量化する。
//
// diff_byte は **定数シフト** (累積 diff ではない):
//   forward d[i] = fp32_to_bf16(bf16_to_fp32(x[i]) - shift)   (shift = テンソル min, 定数)
//   inverse x'[i] = fp32_to_bf16(bf16_to_fp32(d[i]) + shift)
// → 各要素が独立。要素間の累積は無い。位置×誤差ビンが平坦なら非累積の実証となる。
// (累積和は整数版 idiff_byte 側で、そちらは bit-exact 誤差ゼロ。)
//
// ── ULP 距離の定義 ───────────────────────────────────────────────────────────
// bf16 (1 符号 / 8 指数 / 7 仮数) のビットパターン u を、実数値に対し単調増加する
// 順序キー ord(u) に写像し、距離を |ord(x') - ord(x)| とする (sign-magnitude →
// 単調整数, Bruce Dawson 方式の 16bit 版):
//   ord(u) = (u & 0x8000) ? (0x8000 - (u & 0x7FFF))     // 負: 大きい負値ほど小さい ord
//                         : (0x8000 + u)                // 正: 大きい正値ほど大きい ord
// 性質:
//   ・隣接する表現可能値の距離 = 1 ULP。
//   ・+0 (0x0000) と -0 (0x8000) は共に ord=0x8000 → 距離 0 (同一視)。
//   ・符号反転をまたぐ場合: ゼロを 1 段として数える。例えば最小正(+ε)→最小負(-ε)は
//     +ε(ord 0x8001) → 0/−0(0x8000) → −ε(0x7FFF) で 2 ULP。
//   ・NaN/Inf (指数 all 1 = (u & 0x7F80)==0x7F80) は ULP/相対/絶対誤差統計から除外し
//     n_special に計上 (実データには通常現れない)。
//
// 絶対誤差 |x'-x| / 相対誤差 |x'-x|/|x| は bf16→fp32 (無損失) 変換後に算出。
// 相対誤差は x=0 を除外 (除外数を n_zero に記録)。
//
// 出力: results_diff_error.csv (1 テンソル 1 行)。位置×誤差は pos_ulp_b0..b9 列。
// 使い方: ./benchmark_diff_error [--out CSV] [--sub-kb K] file1 [file2 ...]
// ─────────────────────────────────────────────────────────────────────────────
#define _GNU_SOURCE
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

#include "netzip.h"

#define NBIN 10                      // サブチャンク内位置のビン数

static void get_commit(char*o,size_t n){ snprintf(o,n,"nogit");
    FILE*p=popen("git rev-parse --short HEAD 2>/dev/null","r");
    if(p){ char b[64]; if(fgets(b,sizeof b,p)){ b[strcspn(b,"\n")]=0; if(b[0]) snprintf(o,n,"%s",b);} pclose(p);} }
static long file_size(const char*p){ FILE*f=fopen(p,"rb"); if(!f)return -1;
    fseek(f,0,SEEK_END); long n=ftell(f); fclose(f); return n; }

// ─── 分類 (generalization と同じ) ────────────────────────────────────────────
static const char *basename_of(const char*path){ const char*s=strrchr(path,'/'); return s?s+1:path; }
static void tensor_name(const char*path,char*out,size_t n){
    const char*b=basename_of(path); while(*b>='0'&&*b<='9')b++; if(*b=='_')b++;
    snprintf(out,n,"%s",b); char*d=strstr(out,".pkl.bin"); if(d)*d=0; }
static const char *classify(const char*name){
    if(strstr(name,"self_attn.q_proj"))return "attn_q";
    if(strstr(name,"self_attn.k_proj"))return "attn_k";
    if(strstr(name,"self_attn.v_proj"))return "attn_v";
    if(strstr(name,"self_attn.o_proj"))return "attn_o";
    if(strstr(name,"mlp.gate_proj"))return "mlp_gate";
    if(strstr(name,"mlp.up_proj"))return "mlp_up";
    if(strstr(name,"mlp.down_proj"))return "mlp_down";
    if(strstr(name,"mlp.act_fn"))return "mlp_actfn";
    if(strstr(name,"input_layernorm"))return "layernorm_input";
    if(strstr(name,"post_attention_layernorm"))return "layernorm_postattn";
    size_t L=strlen(name);
    #define ENDS(suf)(L>=strlen(suf)&&strcmp(name+L-strlen(suf),suf)==0)
    if(ENDS(".self_attn"))return "attn_combined";
    if(ENDS(".mlp"))return "mlp_combined";
    if(strstr(name,"lm_head"))return "lm_head";
    if(strstr(name,"embed_tokens"))return "embed";
    if(strstr(name,"rotary_emb"))return "rotary";
    if(strstr(name,"model.norm"))return "norm";
    if(strstr(name,"model.layers.")){ if(name[L-1]>='0'&&name[L-1]<='9')return "layer_block"; }
    #undef ENDS
    return "other";
}
static int layer_index(const char*name){
    const char*p=strstr(name,"model.layers."); if(!p)return -1;
    p+=strlen("model.layers."); int v=-1;
    if(*p>='0'&&*p<='9'){v=0;while(*p>='0'&&*p<='9'){v=v*10+(*p-'0');p++;}} return v; }
static const char *act_or_grad(const char*path){
    if(strstr(path,"/grad/"))return "gradient";
    if(strstr(path,"/forward/"))return "activation";
    return "unknown"; }

// ─── ULP 順序キー / 特殊値判定 ───────────────────────────────────────────────
static inline uint32_t bf16_ord(uint16_t u){
    return (u & 0x8000u) ? (uint32_t)(0x8000u - (u & 0x7FFFu))
                         : (uint32_t)(0x8000u + (u & 0x7FFFu)); }
static inline int bf16_special(uint16_t u){ return (u & 0x7F80u)==0x7F80u; }  // inf/nan

int main(int argc,char**argv){
    const char*out_csv="results_diff_error.csv";
    size_t sub_kb=256;
    const char*files[1024]; int nf=0;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--out")&&i+1<argc) out_csv=argv[++i];
        else if(!strcmp(argv[i],"--sub-kb")&&i+1<argc) sub_kb=(size_t)atoi(argv[++i]);
        else if(argv[i][0]=='-'){ fprintf(stderr,"unknown arg %s\n",argv[i]); return 1; }
        else if(nf<1024) files[nf++]=argv[i];
    }
    if(nf==0){ fprintf(stderr,"no input files\n"); return 1; }

    char commit[64],ts[32]; get_commit(commit,sizeof commit);
    time_t tt=time(NULL); strftime(ts,sizeof ts,"%Y-%m-%dT%H:%M:%S",localtime(&tt));
    int hdr=(file_size(out_csv)<0);
    FILE*csv=fopen(out_csv,"a"); if(!csv){ perror(out_csv); return 1; }
    if(hdr){
        fprintf(csv,"tensor,category,layer_idx,act_or_grad,size_bytes,n_elem,n_zero,n_special,"
                    "preproc,elem_mismatch_pct,ulp_max,ulp_mean,"
                    "ulp_pct_0,ulp_pct_1,ulp_pct_2,ulp_pct_3,ulp_pct_4plus,"
                    "relerr_max,relerr_mean,abserr_max,abserr_mean,");
        for(int b=0;b<NBIN;b++) fprintf(csv,"pos_ulp_b%d,",b);
        fprintf(csv,"sub_bytes,shift_val,commit_hash,timestamp,status\n");
    }

    printf("=== diff_byte 復元誤差測定 (%d テンソル) ===\n",nf);
    printf("ULP 定義: ord(u)=(u&0x8000)?(0x8000-(u&0x7FFF)):(0x8000+(u&0x7FFF)); "
           "dist=|ord(x')-ord(x)|; ±0 同一視; 符号跨ぎはゼロ1段; NaN/Inf 除外\n\n");
    int n_err=0;

    for(int fi=0;fi<nf;fi++){
        const char*path=files[fi];
        char tname[256]; tensor_name(path,tname,sizeof tname);
        const char*cat=classify(tname); int layer=layer_index(tname);
        const char*ag=act_or_grad(path);
        long fsz=file_size(path);
        if(fsz<2 || (fsz&1)){
            fprintf(csv,"%s,%s,%d,%s,%ld,0,0,0,diff_byte,0,0,0,0,0,0,0,0,0,0,0,0,",
                    tname,cat,layer,ag,fsz);
            for(int b=0;b<NBIN;b++) fprintf(csv,"0,");
            fprintf(csv,"0,0,%s,%s,skip:too_small_or_odd\n",commit,ts);
            n_err++; continue;
        }
        size_t sub_bytes=sub_kb*1024;
        if((size_t)fsz<sub_bytes) sub_bytes=(size_t)fsz;     // フォールバック (1 サブチャンク)
        size_t sub_elem=sub_bytes/2;
        size_t nsub=(size_t)fsz/sub_bytes;
        size_t proc_bytes=nsub*sub_bytes, n_elem=proc_bytes/2;

        uint16_t*src=(uint16_t*)malloc(proc_bytes);
        uint8_t *pp =(uint8_t*) malloc(proc_bytes);
        uint16_t*rst=(uint16_t*)malloc(proc_bytes);
        if(!src||!pp||!rst){ free(src);free(pp);free(rst);
            fprintf(stderr,"alloc %s\n",tname); n_err++; continue; }
        FILE*f=fopen(path,"rb");
        if(!f||fread(src,1,proc_bytes,f)!=proc_bytes){ if(f)fclose(f);
            free(src);free(pp);free(rst);
            fprintf(csv,"%s,%s,%d,%s,%ld,0,0,0,diff_byte,0,0,0,0,0,0,0,0,0,0,0,0,",
                    tname,cat,layer,ag,fsz);
            for(int b=0;b<NBIN;b++) fprintf(csv,"0,");
            fprintf(csv,"%zu,0,%s,%s,error:read\n",sub_bytes,commit,ts);
            n_err++; continue; }
        fclose(f);

        float shift=netzip_find_min(src,n_elem);
        // diff_byte forward → inverse (実パイプラインと同じサブチャンク単位)
        #pragma omp parallel for schedule(static)
        for(size_t s=0;s<nsub;s++){
            netzip_diff_byte_group_serial(src+s*sub_elem,sub_elem,shift,pp+s*sub_bytes);
            netzip_diff_byte_ungroup_serial(pp+s*sub_bytes,sub_elem,shift,rst+s*sub_elem);
        }

        // ── 要素単位の誤差統計 ──
        long mism=0, nzero=0, nspec=0;
        long ulp_hist[5]={0,0,0,0,0};
        long long ulp_sum=0; long ulp_max=0;
        double abserr_sum=0, abserr_max=0, relerr_sum=0, relerr_max=0;
        double pos_sum[NBIN]; long pos_cnt[NBIN];
        for(int b=0;b<NBIN;b++){ pos_sum[b]=0; pos_cnt[b]=0; }

        for(size_t i=0;i<n_elem;i++){
            uint16_t x=src[i], y=rst[i];
            if(x!=y) mism++;
            if(bf16_special(x)||bf16_special(y)){ nspec++; continue; }
            long ulp=labs((long)bf16_ord(y)-(long)bf16_ord(x));
            ulp_sum+=ulp; if(ulp>ulp_max)ulp_max=ulp;
            ulp_hist[ ulp>=4?4:ulp ]++;
            float fx=bf16_to_fp32(x), fy=bf16_to_fp32(y);
            double ae=fabs((double)fy-(double)fx);
            abserr_sum+=ae; if(ae>abserr_max)abserr_max=ae;
            if(fx!=0.0f){ double re=ae/fabs((double)fx);
                relerr_sum+=re; if(re>relerr_max)relerr_max=re; }
            else nzero++;
            size_t j=i%sub_elem; int b=(int)(j*NBIN/sub_elem); if(b>=NBIN)b=NBIN-1;
            pos_sum[b]+=ulp; pos_cnt[b]++;
        }
        long nstat = (long)n_elem - nspec;             // ULP 統計対象数
        long nrel  = nstat - nzero;                    // 相対誤差対象 (非ゼロ)
        double mism_pct = 100.0*(double)mism/(double)n_elem;
        double ulp_mean = nstat? (double)ulp_sum/(double)nstat : 0;
        double abserr_mean = nstat? abserr_sum/(double)nstat : 0;
        double relerr_mean = nrel>0? relerr_sum/(double)nrel : 0;

        fprintf(csv,"%s,%s,%d,%s,%ld,%zu,%ld,%ld,diff_byte,"
                    "%.4f,%ld,%.5f,%.4f,%.4f,%.4f,%.4f,%.4f,"
                    "%.6e,%.6e,%.6e,%.6e,",
            tname,cat,layer,ag,fsz,n_elem,nzero,nspec,
            mism_pct,ulp_max,ulp_mean,
            nstat?100.0*ulp_hist[0]/nstat:0, nstat?100.0*ulp_hist[1]/nstat:0,
            nstat?100.0*ulp_hist[2]/nstat:0, nstat?100.0*ulp_hist[3]/nstat:0,
            nstat?100.0*ulp_hist[4]/nstat:0,
            relerr_max,relerr_mean,abserr_max,abserr_mean);
        for(int b=0;b<NBIN;b++) fprintf(csv,"%.5f,", pos_cnt[b]?pos_sum[b]/pos_cnt[b]:0.0);
        fprintf(csv,"%zu,%.6f,%s,%s,ok\n",sub_bytes,shift,commit,ts);

        printf("[%3d/%3d] %-40s %-16s %-10s ulp_max=%ld ulp_mean=%.4f "
               "mism=%.1f%% (>=1ulp:%.1f%%) abserr_max=%.3e\n",
               fi+1,nf,tname,cat,ag,ulp_max,ulp_mean,mism_pct,
               nstat?100.0*(nstat-ulp_hist[0])/nstat:0,abserr_max);
        free(src);free(pp);free(rst);
        fflush(csv);
    }
    fclose(csv);
    printf("\n=== 完了: %d テンソル (エラー/skip %d) -> %s ===\n",nf-n_err,n_err,out_csv);
    return 0;
}
