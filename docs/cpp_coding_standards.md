# C++ コーディング規約 (libmucom88)

CLAUDIUS / MUCOM88V プロジェクトと共通の C++ ベストプラクティス。
C++ Core Guidelines / Google C++ Style Guide / CERT C++ Secure Coding Standard 準拠。
本リポジトリは**ヘッダオンリー C++17・外部依存なし・例外不使用**という制約を持つため、
mucom88v 版からその点を翻案している。CLAUDE.md の「C++コーディングベストプラクティス」節の詳細版。

## ヘッダオンリー設計の維持

- コアに `.cpp` を追加しない。コア実装を `include/mucom88/*.hpp` に置く。tests/toolsの `.cpp` は対象外（`#pragma once`、include guard 不可）
- `fm_common.hpp` は mucom88v・CLAUDIUS と共有するため、シグネチャ・enum 値・構造体レイアウト・
  デフォルト引数の破壊的変更は後方互換性を壊す。変更時は両利用側のビルドへの影響を確認する
- 単体検証は [CLAUDE.md](../CLAUDE.md)「テスト」のCMake/CTestを使う。

## 静的解析

- `.clang-tidy` でチェック: `bugprone-*`, `cppcoreguidelines-*`, `performance-*`
- エラー昇格: `bugprone-use-after-move`, `bugprone-narrowing-conversions`
- 対象は `include/mucom88/` のヘッダ群
- `.clang-format`: IndentWidth=4, K&R(Attach), ColumnLimit=120, SortIncludes=Never

## リアルタイムオーディオ安全性

- `advance()`, `renderMixed()`, `generateInterleaved()` 等は `noexcept` 必須（F.6）
- オーディオパス内でメモリ確保（`new` / `vector::push_back`）・mutex lock 禁止（Per.15, CP.43）
- 例外は使用しない方針（エラーは `bool` 戻り値または `std::optional` で返す）。
  `noexcept`境界を例外が越えると `std::terminate` が呼ばれる。
  根拠: [C++ draft except.terminate](https://eel.is/c++draft/except.terminate)。
- MmlEngine はスレッドセーフでない。`advance()` と `playVoice()`/`playSe()` は同一オーディオ
  スレッドから呼ぶ契約。UIの表示用途でも非atomic値の競合する読み書きを無条件に許容しない。
  共有状態の同期・寿命をconsumer側で保証する。根拠: [C++ draft intro.races](https://eel.is/c++draft/intro.races)。

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

## 検証フロー

単体テストは `tests/` と `CMakeLists.txt` にある。単体CMake/CTest、optional ymfmの
compile/link、consumer全曲回帰のコマンドと適用範囲は [CLAUDE.md](../CLAUDE.md)「テスト」を正本とする。
文書のみは差分・参照整合性を確認し、ビルド/回帰をskipと記録する。
API/MML変更では対応docsも更新する。単体PASSからconsumerや実音のPASSを推定しない。

## 情報源

- C++ Core Guidelines（Stroustrup & Sutter）
- Google C++ Style Guide
- CERT C++ Secure Coding Standard
- LLVM Coding Standards

*翻案元: mucom88v `docs/dev/cpp_coding_standards.md`（2026-05-27）*
