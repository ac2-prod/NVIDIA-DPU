#!/bin/bash
# run_diff_error.sh — ドラフト §3.6: diff_byte (float diff) 復元誤差の大きさ測定
#   全 423 テンソルで diff_byte forward→inverse を回し、uint16(bf16) 要素単位の
#   ULP 誤差 / 相対誤差 / 絶対誤差 と、サブチャンク内位置×誤差 を算出する。
#   ※ 誤差測定は決定的 (反復不要)。単体・1 ノード・MPI 不要。
#   ※ BF3 (aarch64) では diff_byte は NEON truncation 経路 = 実パイプラインと同一。
# 使い方: sbatch run_diff_error.sh
#SBATCH -p rome
#SBATCH --nodelist=romebf3a005
#SBATCH -N 1
#SBATCH --ntasks-per-node=1
#SBATCH --exclusive
#SBATCH -t 00:30:00
#SBATCH -J diff_error
#SBATCH -o diff_error_%j.out

export MODULEPATH=/global/software/rocky-9.aarch64/modfiles/langs:/global/software/rocky-9.aarch64/modfiles/tools:/global/software/rocky-9.aarch64/modfiles/apps
source /etc/profile.d/modules.sh
module purge 2>/dev/null
module load gcc/11

cd "$SLURM_SUBMIT_DIR/src" 2>/dev/null || cd "$HOME/MICRO-2025-NetZIP/netzip_c/bf3_deploy/src"
echo "=== node: $(hostname) arch=$(uname -m) ==="
export OMP_NUM_THREADS=16 OMP_PLACES=cores OMP_PROC_BIND=close

OUT=results_diff_error.csv
D=../../bf3_data
FILES=$( ls $D/forward/*.pkl.bin $D/grad/*.pkl.bin )
echo "対象テンソル数: $( echo "$FILES" | wc -l )"

./benchmark_diff_error --out $OUT $FILES

echo ""
echo "############### サマリ (§3.6) ###############"
HDR='NR==1{for(i=1;i<=NF;i++)c[$i]=i;next}'

echo ""
echo "=== (1) 活性 vs 勾配: ULP/誤差 (status=ok の平均) ==="
awk -F, "$HDR"'$(c["status"])=="ok"{ag=$(c["act_or_grad"]);
  n[ag]++; smm[ag]+=$(c["elem_mismatch_pct"]);
  sum_um[ag]+=$(c["ulp_mean"]); if($(c["ulp_max"])>mx[ag])mx[ag]=$(c["ulp_max"]);
  s0[ag]+=$(c["ulp_pct_0"]); s1[ag]+=$(c["ulp_pct_1"]); s2[ag]+=$(c["ulp_pct_2"]);
  s3[ag]+=$(c["ulp_pct_3"]); s4[ag]+=$(c["ulp_pct_4plus"]);
  sre[ag]+=$(c["relerr_mean"]); if($(c["relerr_max"])>rx[ag])rx[ag]=$(c["relerr_max"]);
  sae[ag]+=$(c["abserr_mean"]); if($(c["abserr_max"])>ax[ag])ax[ag]=$(c["abserr_max"]) }
  END{ printf "%-11s %6s %9s %9s | %7s %7s %7s %7s %7s | %9s %9s\n",
        "act/grad","n","mism%","ulp_mean","0ulp%","1ulp%","2ulp%","3ulp%","4+ulp%","relerr_mx","abserr_mx";
    split("activation gradient",A," ");
    for(i=1;i<=2;i++){ag=A[i]; if(!n[ag])continue;
      printf "%-11s %6d %8.2f%% %9.1f | %6.1f%% %6.1f%% %6.1f%% %6.1f%% %6.1f%% | %9.2e %9.2e\n",
        ag,n[ag],smm[ag]/n[ag],sum_um[ag]/n[ag],
        s0[ag]/n[ag],s1[ag]/n[ag],s2[ag]/n[ag],s3[ag]/n[ag],s4[ag]/n[ag],rx[ag],ax[ag]} }' $OUT
echo "  (ulp_max は act/grad 内の最大、relerr_mx/abserr_mx も最大。±1ULP に収まらないことを確認)"

echo ""
echo "=== (2) サブチャンク内 位置×平均ULP (b0=先頭 .. b9=末尾; 平坦なら非累積) ==="
awk -F, "$HDR"'$(c["status"])=="ok"{ag=$(c["act_or_grad"]); n[ag]++;
  for(b=0;b<10;b++) pb[ag,b]+=$(c["pos_ulp_b" b]) }
  END{ printf "%-11s %8s %8s %8s %8s %8s %8s %8s %8s %8s %8s\n",
        "act/grad","b0","b1","b2","b3","b4","b5","b6","b7","b8","b9";
    split("activation gradient",A," ");
    for(i=1;i<=2;i++){ag=A[i]; if(!n[ag])continue; printf "%-11s",ag;
      for(b=0;b<10;b++) printf " %8.1f",pb[ag,b]/n[ag]; print ""} }' $OUT
echo "  → b0..b9 が単調増加なら累積誤差、ほぼ一定なら非累積 (定数シフトの per-element 誤差)。"

echo ""
echo "=== (3) カテゴリ別 ulp_mean (上位/下位の住み分け) ==="
awk -F, "$HDR"'$(c["status"])=="ok"{k=$(c["category"]) SUBSEP $(c["act_or_grad"]);
  s[k]+=$(c["ulp_mean"]); n[k]++; cats[$(c["category"])]=1}
  END{ printf "%-20s %12s %12s\n","category","act ulp_mean","grad ulp_mean";
    nc=split("embed lm_head norm rotary attn_q attn_k attn_v attn_o attn_combined mlp_gate mlp_up mlp_down mlp_actfn mlp_combined layernorm_input layernorm_postattn layer_block",CL," ");
    for(ci=1;ci<=nc;ci++){ca=CL[ci]; if(!(ca in cats))continue;
      ka=ca SUBSEP "activation"; kg=ca SUBSEP "gradient";
      printf "%-20s %12s %12s\n",ca,
        n[ka]?sprintf("%.1f",s[ka]/n[ka]):"-", n[kg]?sprintf("%.1f",s[kg]/n[kg]):"-"} }' $OUT

echo ""
echo "=== (4) 計測できなかったテンソル (status != ok) ==="
awk -F, "$HDR"'$(c["status"])!="ok"{print "  "$(c["tensor"]),$(c["status"])}' $OUT
echo "  (上が空なら全テンソル正常)"
echo ""
echo "結論の読み方: 0ulp%+1ulp% が圧倒的でなく、4+ulp% や ulp_max が大きい →"
echo "  diff_byte は ±1 ULP に収まらない。位置ビンが平坦 → 累積ではなく定数シフトの"
echo "  per-element 誤差 (小さい値ほど ULP/相対誤差が壊滅的、特に勾配)。"
