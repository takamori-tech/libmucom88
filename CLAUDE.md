# libmucom88

MUCOM88互換 MMLパーサー＋シーケンサー＋ADPCM-Bボイス再生ライブラリ（YM2608 / OPNA）。
コアはヘッダーオンリーC++17・外部依存なし。optional `mucom88/ymfm_engine.hpp` を使う場合のみ利用側でymfmヘッダ/linkを追加する。

詳細API: [API reference](docs/api_reference.md) / [組み込みガイド](docs/integration_guide.md)

エージェントの入口は [AGENTS.md](AGENTS.md)。本書は共通の技術・検証規約。
過去の取り込み指示は [session log](docs/session_log.md) の履歴として保存し、現在の更新先には使わない。

## アーキテクチャ

ヘッダーオンリー設計: submodule経由で複数プロジェクト（VST/AU, ゲーム）に組み込むため、ビルド依存を最小化。
IFmEngineを抽象化している理由: エミュレータ実装（fmgen等）を利用側に委ねることで、ライブラリ自体をエミュレータ非依存にする。
ただし ymfm については `FmEngineYmfm : IFmEngine` の互換アダプタを optional header として提供する。

```
MUCテキスト (.muc)
    ▼
MmlParser ── パース、マクロ展開、イベント列生成
    ▼
MmlEngine ── シーケンス再生、Timer-B駆動、レジスタ書き込み
    ▼
IFmEngine ── 抽象インターフェース（利用側で実装、または FmEngineYmfm を使用）
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

- **OPSynth** (`takamori-tech/OPSynth`) — YM2608 VST/AUプラグイン。このライブラリを git submodule として参照
- **CLAUDIUS** (`takamori-tech/rpi5-native-game`) — レトロSTGゲーム。このライブラリを git submodule として**直接**参照（`vendor/libmucom88`。OPSynth 経由の nested ではない）

## 正本と変更フロー

修正元は独立した `takamori-tech/libmucom88` リポジトリ。consumer 内の submodule を直接編集しない。
OPSynth と CLAUDIUS は、それぞれ `vendor/libmucom88` を直接参照する。

1. 今回の依頼・Issue、git status、branch、現行コードを確認し、関連する修正を本リポジトリで実施する。
2. 本書の単体検証を行う。公開APIならAPI referenceとintegration guide、MML挙動ならZ80比較表を更新する。
3. consumer 更新が承認範囲なら、到達可能な修正commitへ各consumerのgitlinkを更新する。
   自動的にmainへpushしたり、CLAUDIUSへ更新を波及させたりしない。文書だけの変更はgitlink更新を必須にしない。
4. OPSynth では同リポジトリの AGENTS.md を読み、正本ビルド・日時確認・必要な回帰を行う。
   `verify.sh` はsubmodule updateを行うため、候補commitがindexのgitlinkと一致することを確認してから実行する。
   一致しない状態で検証して古いcommitへ戻した結果を、新しいcommitの検証としない。
5. CLAUDIUS も独自の規約とテストに従う。consumerごとに検証済みcommitと未確認事項を記録する。

## コーディング規約

- コメントは日本語
- C++17（`<algorithm>`, `std::clamp`, 構造化束縛 等を使用）
- コアのヘッダーオンリー設計を維持（コアに.cppを追加しない。tests/toolsは別）
- `#pragma once` を使用（include guard は使わない）
- 例外は使用しない（エラーはbool戻り値またはstd::optionalで返す）
- `fm_common.hpp` は他プロジェクトと共有するため、変更時は後方互換性に注意

## C++コーディングベストプラクティス（CLAUDIUS準拠）

CLAUDIUSプロジェクトと統一したC++コーディング基準。Core Guidelines / Google / CERT準拠。
詳細版: [C++規約](docs/cpp_coding_standards.md)

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

## MML再現目標

- OpenMUCOM88（Z80 VM + fmgen）と機能的に完全一致を目指す
- 許容差異: Z80 VM起動遅延、Timer-B 0x27レジスタ書き込み回数差（ハードウェア由来）
- muc_compare / muc_regtest（OPSynthリポジトリの tools/）で回帰テスト
- 目標: avgRMS ~1.0（0.95-1.05 圏内）、全曲 Median ~1.000
- 差分があれば Z80 music.asm を参照して根本原因を特定・修正
- MmlEngine と Z80 正本の差分表: [Z80比較表](docs/Z80_vs_MmlEngine.md)（エンジン挙動変更時は更新）

## テスト

単体テストは `tests/` にあり、正本の登録は `CMakeLists.txt` の `add_test`。
トップレベルでツール・テストを明示的に有効化し、件数ゼロの成功を検証済みとしない。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLIBMUCOM88_BUILD_TOOLS=ON -DLIBMUCOM88_BUILD_TESTS=ON
cmake --build build --parallel 8
ctest --test-dir build --output-on-failure --no-tests=error
```

必要な関連テストを反復し、最終変更で全単体テストを確認する。期待値を単に更新して失敗を消さない。
文書のみなら `git diff --check` と参照・コマンドの整合性確認でよい。ビルド・回帰はskipと記録する。
optional `ymfm_engine.hpp` を変更した場合は、利用側の実際のymfmヘッダ・ソースを使う
compile/linkと関連動作確認も必要。コア単体テストだけではoptional adapterを検証したことにならない。

MML/音声挙動変更の全曲互換性は OPSynth の fmgen oracle で確認する。
単体テストの成功を全曲・MIDI・DAW・実音の成功へ広げない。consumer更新を行う場合のコマンド:

```bash
# OPSynth リポジトリルートで実行
./scripts/build_release_artifacts.sh
ls -la build/OPSynth_artefacts/Release/Standalone/OPSynth.app/Contents/MacOS/OPSynth
./scripts/verify.sh
build/muc_regtest -sec 30
```

全曲 avgRMS >= 0.8 を維持する。regtestは共有一時パスのため同時多重起動しない。
fmgen再現指標とymfm決定論goldenを混同しない。ROM/sampleがなければ不足を記録し、
`--no-tests`を全曲互換性のPASSとしない。コード修正が済んでも必要なconsumer検証は未完了のまま示す。

## 高難度レビュー手法

実時間性・MML再現・並行性/寿命・公開契約の変更では次を適用する。ローカルskillや別モデルは不要。

- 入口から到達経路を追い、危険な値が既に制限されていないか先に確認する。
  RELEASEで消えるassertだけを外部入力の防御としない。
- `advance()` / `renderMixed()` / `generateInterleaved()` の確保・throw・待機ロックと、
  IFmEngineへの到達を確認する。`noexcept`やテスト成功だけで実時間安全性を証明したことにしない。
- 共有データの所有者、書き込み元、寿命、同期順序を示す。表示用途でも非atomicの競合を許容としない。
- Z80 music.asm、fmgen、公開APIの実装・契約と照合し、反証できる再現を優先する。
  根拠のない引用・行番号・テスト数を作らず、資料の数だけで正しさを判定しない。
- 公開型・enum・既定値・IFmEngineの変更がconsumerへ与える影響を確認する。
- 差分レビューは実際の不具合・退行も対象とし、仕様項目の採点だけで終わらせない。
  指摘にfile:line・影響・根拠を付け、自己/独立レビューの別を明示する。
- 合格は対象条件の実証に基づく。未実行はskip、不明はUnknown、聴感等はNOT_VERIFIABLE。
  独立レビューが明示必須なら自己レビューで代用しない。

*規約更新: 2026-09-06。OpenAI公式資料と運用理由は AGENTS.md を参照。*
