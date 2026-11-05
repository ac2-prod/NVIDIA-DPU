# Licensing / ライセンス

[English](#english) | [日本語](#日本語)

## English

This repository does not apply a single license to all files. Each project
directory states its own licenses. There is intentionally no `LICENSE` file at
the repository root.

### `netzip_bf3/`

| Material | License | Details |
|---|---|---|
| Original code and documentation under `netzip_bf3/` | MIT License — Copyright (c) 2026 Taiga Kobayashi | [`netzip_bf3/LICENSE`](netzip_bf3/LICENSE). See below for portions derived from NetZIP. |
| Raw experimental measurement data | None included. The data notice does not apply CC BY 4.0 to any file at present. | [`netzip_bf3/LICENSE-DATA.md`](netzip_bf3/LICENSE-DATA.md) |
| Third-party code bundled in `netzip_bf3/bf3_deploy/src/`: `lz4.c`, `lz4.h`, `lz4frame.c`, `lz4frame.h`, `lz4hc.c`, `lz4hc.h` (LZ4 1.9.4, <https://github.com/lz4/lz4>) and `xxhash.c`, `xxhash.h` (xxHash 0.6.5, <https://github.com/Cyan4973/xxHash>) | Their original BSD 2-Clause License, as stated in the header of each file (Copyright Yann Collet). These files are not covered by the MIT License of this project. | File headers |

`netzip_bf3/bf3_deploy/src/netzip.c` and `netzip.h` implement in C the
preprocessing transforms defined in [`compression_ratio_calculation.py`](https://github.com/ece-fast-lab/MICRO-2025-NetZIP/blob/main/compression_ratio_calculation/compression_ratio_calculation.py)
of the public artifact of NetZIP (J. Huang et al., MICRO 2025;
<https://github.com/ece-fast-lab/MICRO-2025-NetZIP>). The artifact contains no C code; its
transforms are written in Python. It is released under the MIT License
(Copyright (c) 2025 FAST Illinois Lab). The portions ported from MICRO-2025-NetZIP retain that MIT License.
Original porting code and modifications made for this project are covered by
[the project MIT License](netzip_bf3/LICENSE). The upstream copyright notice and
permission notice are reproduced in [NetZIP license notice](#netzip-license-notice)
below. The optimized implementations in these files (the NEON and OpenMP code
and the serial, fused and sub-chunked variants), the inverse transforms,
`netzip_find_min` and the `idiff` transforms were written for this project.
Any other third-party material retains its original license or terms of use.

Material that is used by the code but is not included in this repository:

- Zstandard 1.5.7 (<https://github.com/facebook/zstd>) and Snappy 1.1.9
  (<https://github.com/google/snappy>). The Makefile expects them under
  `netzip_bf3/bf3_deploy/deps/`, which is not part of this repository. Obtain
  them from their upstream projects under their own licenses.
- Input tensors computed from the Meta Llama 3.2 1B model. They are not
  included. Regenerating them requires the model, which Meta distributes
  under its own license terms (<https://www.llama.com/llama3_2/license/>).
- The preprocessing methods follow NetZIP (J. Huang et al., MICRO 2025). No
  upstream Python source files are bundled here. For
  `netzip.c` and `netzip.h`, see the note above.

## 日本語

このリポジトリでは、全ファイルに一律のライセンスを適用していません。
プロジェクトのディレクトリごとにライセンスを定めています。
リポジトリ直下には意図的に `LICENSE` ファイルを置いていません。

### `netzip_bf3/`

| 対象 | ライセンス | 詳細 |
|---|---|---|
| `netzip_bf3/` の独自コードと文書 | MIT License — Copyright (c) 2026 Taiga Kobayashi | [`netzip_bf3/LICENSE`](netzip_bf3/LICENSE)。NetZIP 由来部分は下記を参照してください。 |
| 生の実験測定データ | 現在は同梱していません。データに関するライセンス表記で CC BY 4.0 を適用しているファイルはありません。 | [`netzip_bf3/LICENSE-DATA.md`](netzip_bf3/LICENSE-DATA.md) |
| `netzip_bf3/bf3_deploy/src/` に同梱した第三者コード：`lz4.c`、`lz4.h`、`lz4frame.c`、`lz4frame.h`、`lz4hc.c`、`lz4hc.h`（LZ4 1.9.4、<https://github.com/lz4/lz4>）と `xxhash.c`、`xxhash.h`（xxHash 0.6.5、<https://github.com/Cyan4973/xxHash>） | 各ファイル冒頭に記載された元の BSD 2-Clause License（Copyright Yann Collet）。本プロジェクトの MIT License の対象外です。 | 各ファイルの冒頭 |

`netzip_bf3/bf3_deploy/src/netzip.c` と `netzip.h` は、NetZIP（J. Huang ほか、MICRO 2025）の公開アーティファクト
（<https://github.com/ece-fast-lab/MICRO-2025-NetZIP>）の [`compression_ratio_calculation.py`](https://github.com/ece-fast-lab/MICRO-2025-NetZIP/blob/main/compression_ratio_calculation/compression_ratio_calculation.py) で
定義された前処理の変換を C で実装したものです。同アーティファクトに C のコードはなく、変換は Python で書かれています。
同アーティファクトは MIT License（Copyright (c) 2025 FAST Illinois Lab）で公開されています。MICRO-2025-NetZIP から移植した部分には元の MIT License が引き続き適用されます。
本プロジェクト独自の移植コード・変更部分は[本プロジェクトの MIT License](netzip_bf3/LICENSE) の対象です。
元の著作権表示と許諾文は、下の [NetZIP license notice](#netzip-license-notice)
に記載しています。これらのファイルにある高速化した実装（NEON と OpenMP のコード、serial 版・融合版・サブチャンク版）、
逆変換、`netzip_find_min`、`idiff` 変換は、本プロジェクトで作成したものです。その他の第三者素材には、その素材の元のライセンスまたは利用条件が適用されます。

コードで使用するものの、このリポジトリには含まれていないもの：

- Zstandard 1.5.7（<https://github.com/facebook/zstd>）と Snappy 1.1.9（<https://github.com/google/snappy>）。Makefile は `netzip_bf3/bf3_deploy/deps/` に置かれていることを
  前提にしていますが、このディレクトリはリポジトリに含まれません。各プロジェクトの配布元から、
  それぞれのライセンスに従って入手してください。
- Meta Llama 3.2 1B モデルから計算した入力テンソル。含まれていません。再生成にはモデルが必要で、
  モデルは Meta が独自のライセンス条件で配布しています（<https://www.llama.com/llama3_2/license/>）。
- 前処理の手法は NetZIP（J. Huang ほか、MICRO 2025）に従っています。元の Python ソースは
  このリポジトリには同梱していません。`netzip.c` と `netzip.h` については、上の説明を参照してください。

## NetZIP license notice

The following notice applies to the NetZIP-derived portions of `netzip_bf3/bf3_deploy/src/netzip.c` and
`netzip_bf3/bf3_deploy/src/netzip.h` (see above). It is reproduced from the
[upstream LICENSE](https://github.com/ece-fast-lab/MICRO-2025-NetZIP/blob/main/LICENSE).

以下は、上で説明したとおり `netzip_bf3/bf3_deploy/src/netzip.c` と `netzip_bf3/bf3_deploy/src/netzip.h` に
含まれる NetZIP 由来部分の表示です。[元の LICENSE ファイル](https://github.com/ece-fast-lab/MICRO-2025-NetZIP/blob/main/LICENSE)から写したものです。

```text
MIT License

Copyright (c) 2025 FAST Illinois Lab

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
