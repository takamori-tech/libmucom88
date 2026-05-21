# libmucom88

MUCOM88互換 MMLパーサー＋シーケンサー＋ADPCM-Bボイス再生ライブラリ（YM2608 / OPNA）。
ヘッダーオンリーC++17。外部依存なし。

## プロジェクト構成

```
include/mucom88/
  fm_common.hpp              FM音色定義（FmPatch/Mucom88Patch）、周波数変換、voice.datパーサー
  fm_engine_interface.hpp    IFmEngine 抽象インターフェース（YM2608エミュレータの共通API）
  mml_parser.hpp             MMLパーサー（MUCOM88形式、132曲検証済み）
  mml_engine.hpp             MMLシーケンサー（Timer-B駆動、11ch、ボイス再生、ダッキング）
docs/
  api_reference.md           全クラス・メソッドの詳細
  integration_guide.md       ゲームプログラム組み込みガイド
```

## アーキテクチャ

```
MUCテキスト (.muc)
    │
    ▼
MmlParser ── パース、マクロ展開、イベント列生成
    │
    ▼
MmlEngine ── シーケンス再生、Timer-B駆動、レジスタ書き込み
    │         ボイス再生時のKトラック優先制御、自動ダッキング
    ▼
IFmEngine ── 抽象インターフェース（writeReg, generateInterleaved, ...）
    │
    ▼
[YM2608エミュレータ]  （fmgen 等、利用側で実装）
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

MUCOM88Vリポジトリがサウンド関連コードの正本（single source of truth）。
libmucom88のヘッダーを変更する場合の手順:

1. mucom88v側 (`vendor/libmucom88/`) で編集・ビルド確認
2. libmucom88リポジトリでコミット＆push
3. mucom88v の submodule 参照を更新＆push
4. CLAUDIUS 側の mucom88v submodule を同期＆ビルド確認

## コーディング規約

- コメントは日本語
- C++17（`<algorithm>`, `std::clamp`, 構造化束縛 等を使用）
- ヘッダーオンリー設計を維持（.cpp を追加しない）
- `fm_common.hpp` は他プロジェクトと共有するため、変更時は後方互換性に注意

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
