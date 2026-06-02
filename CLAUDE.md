# libmucom88

MUCOM88互換 MMLパーサー＋シーケンサー＋ADPCM-Bボイス再生ライブラリ（YM2608 / OPNA）。
ヘッダーオンリーC++17。外部依存なし。

詳細API: @docs/api_reference.md / 組み込みガイド: @docs/integration_guide.md

## アーキテクチャ

ヘッダーオンリー設計: submodule経由で複数プロジェクト（VST/AU, ゲーム）に組み込むため、ビルド依存を最小化。
IFmEngineを抽象化している理由: エミュレータ実装（fmgen等）を利用側に委ねることで、ライブラリ自体をエミュレータ非依存にする。

```
MUCテキスト (.muc)
    ▼
MmlParser ── パース、マクロ展開、イベント列生成
    ▼
MmlEngine ── シーケンス再生、Timer-B駆動、レジスタ書き込み
    ▼
IFmEngine ── 抽象インターフェース（利用側で実装）
    ▼
[YM2608エミュレータ]
```

## チャンネル構成（11ch）

- A-C (0-2): FM ch1-3（port 0）
- D-F (3-5): SSG ch1-3（PSG互換矩形波）
- G (6): リズム音源（ADPCM-A: BD/SD/CY/HH/TM/RS）
- H-J (7-9): FM ch4-6（port 1）
- K (10): ADPCM-B

## 利用プロジェクト

- **MUCOM88V** (`takamori-tech/mucom88v`) — YM2608 VST/AUプラグイン。このライブラリを git submodule として参照
- **CLAUDIUS** (`takamori-tech/rpi5-native-game`) — レトロSTGゲーム。mucom88v経由のnested submoduleとして参照

## 正本と変更フロー

**この標準リポジトリ（`/Users/moriyata/git-projects/libmucom88`）が libmucom88 の正本（single source of truth）。**
ゲーム開発用（CLAUDIUS等）とVSTプラグイン用（mucom88v）を兼ねる独立した共用ライブラリのため、
**不具合・改善はこのリポジトリでGitHub Issueを立てて作業する**（mucom88v の `vendor/libmucom88` submodule を直接編集しない）。

libmucom88のヘッダーを変更する場合の手順:

1. この標準リポジトリでIssue起票・`include/mucom88/*.hpp` を編集
   - ヘッダオンリーの単体検証: `g++ -std=c++17 -Wall -Wextra -I include test.cpp`
   - コミット＆push（`... (Fix #N)`）
2. mucom88v の `vendor/libmucom88` submodule を新コミットへ追従
   - `cmake --build build -- -j8`（IFmEngine等のAPI変更時は OpnaChipAdapter / tools/fm_engine_fmgen を追従）
   - `build/muc_regtest -sec 20`（全曲 avgRMS >= 0.8 必須）
   - submodule参照を更新＆push
3. CLAUDIUS 側の mucom88v submodule を同期＆ビルド確認

## コーディング規約

- コメントは日本語
- C++17（`<algorithm>`, `std::clamp`, 構造化束縛 等を使用）
- ヘッダーオンリー設計を維持（.cpp を追加しない）
- `#pragma once` を使用（include guard は使わない）
- 例外は使用しない（エラーはbool戻り値またはstd::optionalで返す）
- `fm_common.hpp` は他プロジェクトと共有するため、変更時は後方互換性に注意

## C++コーディングベストプラクティス（CLAUDIUS準拠）

CLAUDIUSプロジェクトと統一したC++コーディング基準。Core Guidelines / Google / CERT準拠。

### 静的解析ガードレール

- `.clang-tidy`: bugprone-*, cppcoreguidelines-*, performance-* を有効化
  - `bugprone-use-after-move` と `bugprone-narrowing-conversions` はエラー昇格
- `.clang-format`: IndentWidth=4, K&R(Attach), ColumnLimit=120, SortIncludes=Never

### 必須ルール（新規・変更コード）

- **`static_cast` 使用**: 新規コードではC-style cast `(int)x` 禁止 → `static_cast<int>(x)` を使用（ES.48）
- **`noexcept` 必須**: オーディオコールバック/render系メソッド（`advance()`, `renderMixed()`, `generateInterleaved()`）は `noexcept`（F.6）
- **オーディオパス内禁止操作**: `advance()` / `renderMixed()` 内でのメモリ確保（`new`, `vector::push_back`）、mutex lock 禁止（Per.15, CP.43）
- **`rand()`/`srand()` 禁止**: `std::mt19937` + `<random>` を使用（CERT MSC50-CPP）
- **読み取り専用文字列パラメータ**: 新規APIでは `std::string_view` を推奨（LLVM Coding Standards）
- **構造化束縛の積極使用**: `for (auto& [key, val] : map)` 形式を推奨

### ハーネスエンジニアリング

- `.claude/settings.json`: deny rules（.env/secrets保護）、PostToolUse Hook（C++変更時ビルド確認促進）
- ヘッダー変更後は mucom88v 側でビルド確認を推奨

## MML再現目標

- OpenMUCOM88（Z80 VM + fmgen）と機能的に完全一致を目指す
- 許容差異: Z80 VM起動遅延、Timer-B 0x27レジスタ書き込み回数差（ハードウェア由来）
- muc_compare / muc_regtest（mucom88vリポジトリの tools/）で回帰テスト
- 目標: avgRMS ~1.0（0.95-1.05 圏内）、全曲 Median ~1.000
- 差分があれば Z80 music.asm を参照して根本原因を特定・修正

## テスト

このリポジトリ単体にはテストはない。mucom88v側の tools/ にある比較・回帰テストで検証:

```bash
cd ~/git-projects/mucom88v
cmake --build build -- -j8
build/muc_regtest -sec 30    # 全曲回帰テスト
build/muc_compare input.muc  # 個別MUC比較
```
