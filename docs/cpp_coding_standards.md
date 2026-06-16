# C++ コーディング規約 (libmucom88)

CLAUDIUS / MUCOM88V プロジェクトと共通の C++ ベストプラクティス。
C++ Core Guidelines / Google C++ Style Guide / CERT C++ Secure Coding Standard 準拠。
本リポジトリは**ヘッダオンリー C++17・外部依存なし・例外不使用**という制約を持つため、
mucom88v 版からその点を翻案している。CLAUDE.md の「C++コーディングベストプラクティス」節の詳細版。

## ヘッダオンリー設計の維持

- `.cpp` を追加しない。全実装を `include/mucom88/*.hpp` に置く（`#pragma once`、include guard 不可）
- `fm_common.hpp` は mucom88v・CLAUDIUS と共有するため、シグネチャ・enum 値・構造体レイアウト・
  デフォルト引数の破壊的変更は後方互換性を壊す。変更時は両利用側のビルドへの影響を確認する
- 単体検証: `g++ -std=c++17 -Wall -Wextra -I include test.cpp`（警告ゼロ）

## 静的解析

- `.clang-tidy` でチェック: `bugprone-*`, `cppcoreguidelines-*`, `performance-*`
- エラー昇格: `bugprone-use-after-move`, `bugprone-narrowing-conversions`
- 対象は `include/mucom88/` のヘッダ群
- `.clang-format`: IndentWidth=4, K&R(Attach), ColumnLimit=120, SortIncludes=Never

## リアルタイムオーディオ安全性

- `advance()`, `renderMixed()`, `generateInterleaved()` 等は `noexcept` 必須（F.6）
- オーディオパス内でメモリ確保（`new` / `vector::push_back`）・mutex lock 禁止（Per.15, CP.43）
- 例外は使用しない方針（エラーは `bool` 戻り値または `std::optional` で返す）。
  リアルタイムスレッドでの例外伝播は未定義動作として扱う
- MmlEngine はスレッドセーフでない。`advance()` と `playVoice()`/`playSe()` は同一オーディオ
  スレッドから呼ぶ契約。UI スレッドからの状態取得（`chNoteOn()` 等）は非アトミックだが表示用途では許容

## キャスト規約

- 新規コードでは `static_cast<>` を使用、C-style cast `(type)` 禁止（ES.48）
- `reinterpret_cast` は PCM バッファ / レジスタ操作等で必要な場合のみ許容

## 乱数

- `rand()` / `srand()` 禁止 → `std::mt19937` + `<random>`（CERT MSC50-CPP）

## モダン C++ スタイル

- 構造化束縛の積極使用（`for (auto& [key, val] : map)`）
- 読み取り専用文字列パラメータは新規 API で `std::string_view` 推奨（LLVM Coding Standards）
- `<algorithm>` / `std::clamp` を活用
- `auto` は型が明白な場合のみ使用

## 検証フロー（テストは利用側）

本リポジトリ単体にはテストが無い。回帰検証は mucom88v の tools/ で行う:

```bash
# 1. ヘッダ単体検証（このリポジトリ）
g++ -std=c++17 -Wall -Wextra -I include test.cpp

# 2. 回帰テスト（mucom88v 側、全曲 avgRMS >= 0.8 必須）
cd ~/git-projects/mucom88v
cmake --build build -- -j8
build/muc_regtest -sec 30
build/muc_compare input.muc
```

## 情報源

- C++ Core Guidelines（Stroustrup & Sutter）
- Google C++ Style Guide
- CERT C++ Secure Coding Standard
- LLVM Coding Standards

*翻案元: mucom88v `docs/dev/cpp_coding_standards.md`（2026-05-27）*
