# Licensing / ライセンス

[English](#english) | [日本語](#日本語)

## English

This repository does not apply a single license to all files. Each project
directory states its own licenses. There is intentionally no `LICENSE` file at
the repository root.

### `netzip_bf3/`

| Material | License | Details |
|---|---|---|
| Code and documentation written for this project: all files under `netzip_bf3/` except the third-party files listed below and the data files listed in `netzip_bf3/LICENSE-DATA.md` | MIT License | [`netzip_bf3/LICENSE`](netzip_bf3/LICENSE) |
| Data files listed in `netzip_bf3/LICENSE-DATA.md` (experimental data and measurement results created in this study) | Creative Commons Attribution 4.0 International (CC BY 4.0) | [`netzip_bf3/LICENSE-DATA.md`](netzip_bf3/LICENSE-DATA.md). No data files are listed at present. |
| Third-party code bundled in `netzip_bf3/bf3_deploy/src/`: `lz4.c`, `lz4.h`, `lz4frame.c`, `lz4frame.h`, `lz4hc.c`, `lz4hc.h` (LZ4 1.9.4, <https://github.com/lz4/lz4>) and `xxhash.c`, `xxhash.h` (xxHash 0.6.5, <https://github.com/Cyan4973/xxHash>) | Their original BSD 2-Clause License, as stated in the header of each file (Copyright Yann Collet). These files are not covered by the MIT License of this project. | File headers |

Material that is used by the code but is not included in this repository:

- Zstandard 1.5.7 (<https://github.com/facebook/zstd>) and Snappy 1.1.9
  (<https://github.com/google/snappy>). The Makefile expects them under
  `netzip_bf3/bf3_deploy/deps/`, which is not part of this repository. Obtain
  them from their upstream projects under their own licenses.
- Input tensors computed from the Meta Llama 3.2 1B model. They are not
  included. Regenerating them requires the model, which Meta distributes
  under its own license terms (<https://www.llama.com/llama3_2/license/>).
- The preprocessing methods follow NetZIP (J. Huang et al., MICRO 2025). The
  NetZIP authors' public artifact
  (<https://github.com/ece-fast-lab/MICRO-2025-NetZIP>) is released under the
  MIT License. This repository does not contain files from
  that artifact.

## 日本語

このリポジトリでは、全ファイルに一律のライセンスを適用していません。
プロジェクトのディレクトリごとにライセンスを定めています。
リポジトリ直下には意図的に `LICENSE` ファイルを置いていません。

### `netzip_bf3/`

| 対象 | ライセンス | 詳細 |
|---|---|---|
| 本プロジェクトのために作成したコードと文書：`netzip_bf3/` 以下の全ファイル（下記の第三者ファイルと `netzip_bf3/LICENSE-DATA.md` に記載したデータファイルを除く） | MIT License | [`netzip_bf3/LICENSE`](netzip_bf3/LICENSE) |
| `netzip_bf3/LICENSE-DATA.md` に記載したデータファイル（本研究で作成した実験データ・測定結果） | Creative Commons Attribution 4.0 International（CC BY 4.0） | [`netzip_bf3/LICENSE-DATA.md`](netzip_bf3/LICENSE-DATA.md)。現時点では記載されたデータファイルはありません。 |
| `netzip_bf3/bf3_deploy/src/` に同梱した第三者コード：`lz4.c`、`lz4.h`、`lz4frame.c`、`lz4frame.h`、`lz4hc.c`、`lz4hc.h`（LZ4 1.9.4、<https://github.com/lz4/lz4>）と `xxhash.c`、`xxhash.h`（xxHash 0.6.5、<https://github.com/Cyan4973/xxHash>） | 各ファイル冒頭に記載された元の BSD 2-Clause License（Copyright Yann Collet）。本プロジェクトの MIT License の対象外です。 | 各ファイルの冒頭 |

コードで使用するものの、このリポジトリには含まれていないもの：

- Zstandard 1.5.7（<https://github.com/facebook/zstd>）と Snappy 1.1.9（<https://github.com/google/snappy>）。Makefile は `netzip_bf3/bf3_deploy/deps/` に置かれていることを
  前提にしていますが、このディレクトリはリポジトリに含まれません。各プロジェクトの配布元から、
  それぞれのライセンスに従って入手してください。
- Meta Llama 3.2 1B モデルから計算した入力テンソル。含まれていません。再生成にはモデルが必要で、
  モデルは Meta が独自のライセンス条件で配布しています（<https://www.llama.com/llama3_2/license/>）。
- 前処理の手法は NetZIP（J. Huang ほか、MICRO 2025）に従っています。NetZIP 著者による公開
  アーティファクト（<https://github.com/ece-fast-lab/MICRO-2025-NetZIP>）は MIT License で公開されています。
  このリポジトリには同アーティファクトのファイルは含まれていません。
