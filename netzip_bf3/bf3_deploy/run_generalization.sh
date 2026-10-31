#!/bin/bash
# run_generalization.sh — データセット一般化測定 (Llama-3.2-1B 全 423 テンソル)
#   forward/ (活性) + grad/ (勾配) の全 .pkl.bin を 4 前処理 × 3 コーデックで全数測定。
#   16T・256KB サブチャンク・DRAM write 込みの標準計時。間引かない。
#   ※ 単体圧縮比較なので RDMA 不要・1 ノード。MPI 不要。
#   ※ diff_byte は非可逆 (誤差は ±1 ULP に収まらない) で、論文の評価には入っていない (論文は bit-exact の 3 前処理のみ)。
# 使い方: sbatch run_generalization.sh
#SBATCH -p rome
#SBATCH --nodelist=romebf3a005
#SBATCH -N 1
#SBATCH --ntasks-per-node=1
#SBATCH --exclusive
#SBATCH -t 04:00:00
#SBATCH -J generalization
#SBATCH -o generalization_%j.out

export MODULEPATH=/global/software/rocky-9.aarch64/modfiles/langs:/global/software/rocky-9.aarch64/modfiles/tools:/global/software/rocky-9.aarch64/modfiles/apps
source /etc/profile.d/modules.sh
module purge 2>/dev/null
module load gcc/11

cd "$SLURM_SUBMIT_DIR/src" 2>/dev/null || cd "$HOME/MICRO-2025-NetZIP/netzip_c/bf3_deploy/src"
echo "=== node: $(hostname) arch=$(uname -m) ==="

export OMP_NUM_THREADS=16
export OMP_PROC_BIND=close
export OMP_PLACES=cores

OUT=results_generalization.csv
D=../../bf3_data
# forward/ と grad/ 直下の .pkl.bin を全件列挙 (再帰しない)。パスに /forward//grad/ を含む。
FILES=$( ls $D/forward/*.pkl.bin $D/grad/*.pkl.bin )
NF=$( echo "$FILES" | wc -l )
echo "対象テンソル数: $NF"

# 標準計時: warmup 3 + 計測 7 の median
./benchmark_generalization --out $OUT --iters 7 --warmup 3 $FILES

echo ""
echo "############### サマリ ###############"
# 列名→index の共通ヘッダ処理 (BEGINでヘッダ行を読む方式ではなく awk 内で解決)
HDR='NR==1{for(i=1;i<=NF;i++)c[$i]=i;next}'

echo ""
echo "=== (1) category × preproc 平均圧縮率 (LZ4, status=ok) ==="
awk -F, "$HDR"'
  $(c["codec"])=="lz4" && $(c["status"])=="ok" {
    k=$(c["category"]) SUBSEP $(c["preproc"]); sum[k]+=$(c["comp_ratio"]); n[k]++;
    cats[$(c["category"])]=1; pres[$(c["preproc"])]=1 }
  END{
    np=split("byte_group bit_group diff_byte idiff_byte",P," ");
    printf "%-20s","category"; for(p=1;p<=np;p++) printf "%12s",P[p]; print "";
    # カテゴリは固定順
    nc=split("embed lm_head norm rotary attn_q attn_k attn_v attn_o attn_combined mlp_gate mlp_up mlp_down mlp_actfn mlp_combined layernorm_input layernorm_postattn layer_block",CL," ");
    for(ci=1;ci<=nc;ci++){ ca=CL[ci]; if(!(ca in cats))continue;
      printf "%-20s",ca;
      for(p=1;p<=np;p++){ k=ca SUBSEP P[p];
        if(n[k]) printf "%12.3f",sum[k]/n[k]; else printf "%12s","-"; }
      print "" } }' $OUT

echo ""
echo "=== (2) 活性 vs 勾配 の平均圧縮率 (LZ4 diff_byte, category別) ==="
awk -F, "$HDR"'
  $(c["codec"])=="lz4" && $(c["preproc"])=="diff_byte" && $(c["status"])=="ok" {
    k=$(c["category"]) SUBSEP $(c["act_or_grad"]); sum[k]+=$(c["comp_ratio"]); n[k]++;
    cats[$(c["category"])]=1 }
  END{ printf "%-20s %12s %12s %10s\n","category","activation","gradient","grad/act";
    nc=split("embed lm_head norm rotary attn_q attn_k attn_v attn_o attn_combined mlp_gate mlp_up mlp_down mlp_actfn mlp_combined layernorm_input layernorm_postattn layer_block",CL," ");
    for(ci=1;ci<=nc;ci++){ ca=CL[ci]; if(!(ca in cats))continue;
      ka=ca SUBSEP "activation"; kg=ca SUBSEP "gradient";
      a=n[ka]?sum[ka]/n[ka]:0; g=n[kg]?sum[kg]/n[kg]:0;
      printf "%-20s %12s %12s %10s\n",ca,
        n[ka]?sprintf("%.3f",a):"-", n[kg]?sprintf("%.3f",g):"-",
        (a>0&&g>0)?sprintf("%.2f",g/a):"-" } }' $OUT

echo ""
echo "=== (3) 層の深さ × 平均圧縮率 (LZ4 diff_byte, layer 0..15, act/grad別) ==="
awk -F, "$HDR"'
  $(c["codec"])=="lz4" && $(c["preproc"])=="diff_byte" && $(c["status"])=="ok" && $(c["layer_idx"])>=0 {
    L=$(c["layer_idx"]); k=L SUBSEP $(c["act_or_grad"]); sum[k]+=$(c["comp_ratio"]); n[k]++ }
  END{ printf "%-6s %12s %12s\n","layer","activation","gradient";
    for(L=0;L<=15;L++){ ka=L SUBSEP "activation"; kg=L SUBSEP "gradient";
      printf "%-6d %12s %12s\n",L,
        n[ka]?sprintf("%.3f",sum[ka]/n[ka]):"-", n[kg]?sprintf("%.3f",sum[kg]/n[kg]):"-" } }' $OUT

echo ""
echo "=== (4) diff_byte(lossy) vs byte_group(bit-exact) 差のカテゴリ一貫性 (LZ4) ==="
awk -F, "$HDR"'
  $(c["codec"])=="lz4" && $(c["status"])=="ok" {
    p=$(c["preproc"]);
    if(p=="diff_byte"||p=="byte_group"||p=="idiff_byte"){
      k=$(c["category"]) SUBSEP p; sum[k]+=$(c["comp_ratio"]); n[k]++;
      cats[$(c["category"])]=1 } }
  END{ printf "%-20s %10s %10s %10s %12s\n","category","diff_byte","byte_grp","idiff","byte/diff";
    nc=split("embed lm_head norm rotary attn_q attn_k attn_v attn_o attn_combined mlp_gate mlp_up mlp_down mlp_actfn mlp_combined layernorm_input layernorm_postattn layer_block",CL," ");
    for(ci=1;ci<=nc;ci++){ ca=CL[ci]; if(!(ca in cats))continue;
      kd=ca SUBSEP "diff_byte"; kb=ca SUBSEP "byte_group"; ki=ca SUBSEP "idiff_byte";
      d=n[kd]?sum[kd]/n[kd]:0; b=n[kb]?sum[kb]/n[kb]:0; ii=n[ki]?sum[ki]/n[ki]:0;
      printf "%-20s %10s %10s %10s %12s\n",ca,
        d?sprintf("%.3f",d):"-", b?sprintf("%.3f",b):"-", ii?sprintf("%.3f",ii):"-",
        (d>0&&b>0)?sprintf("%.2f%%",100*(b-d)/d):"-" } }' $OUT

echo ""
echo "=== (5) 計測できなかった/問題のあったテンソル (status != ok) ==="
awk -F, "$HDR"'$(c["status"])!="ok"{print $(c["tensor"]),$(c["preproc"]),$(c["codec"]),$(c["status"])}' $OUT | sort -u
echo "(上が空なら全テンソル正常測定)"

echo ""
echo "=== bit_exact サニティ (preproc別: diff_byte=no, 他=yes のはず) ==="
awk -F, "$HDR"'$(c["status"])=="ok"{k=$(c["preproc"])" bit_exact="$(c["bit_exact"]); n[k]++}
  END{ for(p in n) printf "%-30s %d 行\n",p,n[p] }' $OUT | sort
