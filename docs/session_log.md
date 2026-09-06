# libmucom88 session log

## 2026-09-06 開発規約の見直し

利用者からmucom88vとリンク先libmucom88の運用見直しを依頼された。
Codex単独の入口AGENTS.mdを追加。CLAUDE.mdのモデル依存を除去し、CMakeの単体テスト、
optional ymfmとconsumer回帰の境界、直接submodule参照、現行OPSynthのビルド手順を明記。
既存レビューskillを共通手法への入口にし、Claude hookの古いビルド案内も更新。
元のハンドオーバーを以下に移し、過去のcommitを現在の取り込み指示として扱わない。
開始時: branch fix/warning-cleanup、HEAD 9f2ca3e、作業ツリーclean。
この変更は運用文書と案内文のみ。ソース・CMake・ライセンス・consumer gitlinkは非変更。
検証: 両repo git diff --check PASS、レビューskill quick_validate PASS、ローカルリンク PASS。
CMakeの8件のテスト登録を対応ソースと照合（実行はしていない）。hookはbash構文と2入力を確認し、
C++で現行手順の案内、文書では無出力。既存permissionsの値が不変なことを確認。
C++詳細規約の古い「単体テストなし」も除去し、表示用途の競合許容と例外に関する誤記を一次資料で訂正。
ソース変更なしのためビルド/全曲回帰はskip。新規Codex起動検証も未実施。
本変更は6ファイル。前の利用者指示に沿って作業ブランチへcommit/pushし、成否はgit/remoteで確認する。

## 過去の記録（旧CLAUDE.mdから移動・現在の指示ではない）

## 直近ハンドオーバー（2026-06-29 / #99 OPNB Neo Geo clock profile）

- **コミット**: `decb182b1978ebc234b1c7e4108d828aee8d9f1b Fix OPNB chip profile clock`（`origin/main` にpush済み）。
- **Issue**: `takamori-tech/libmucom88#99` は close 済み。
- **目的**: YM2610(OPNB) を Neo Geo 再現用 profile として扱い、consumer が `ChipMode::OPNB` を選んだ時に未確定/不正な clock・FM ch 数へ落ちないようにする。
- **仕様根拠**: Neo Geo は 24MHz 系 master から LSPC2-A2 の 8M 出力（24MHz/3）を YM2610 `PHI M` に入れる。OPNB/YM2610 の FM は 4ch。
- **実装**: `include/mucom88/chip_backend_interface.hpp` の `chipModeProfile(ChipMode::OPNB)` を `{ 8000000u, 4, 3, true, true }` に変更。OPNB defaultClock と numFmChannels の static_assert を追加。
- **検証済み**: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`、`cmake --build build`、`ctest --test-dir build --output-on-failure` が PASS（3/3）。
- **consumer 指示（MUCOM88V）**: MUCOM88V は `vendor/libmucom88` をこの commit へ submodule 追従するだけにする。MUCOM88V 側で `include/mucom88/chip_backend_interface.hpp` を直接編集しない。
- **consumer 側追加注意**: MUCOM88V では fmgen OPNB crash が別途 consumer 側 `patches/fmgen/0001-opn-stem-output-api.patch` で修正されている。これは libmucom88 の責務ではなく、fmgen vendor patch 正本で管理する。

## 直近ハンドオーバー（2026-06-29 / #97 FmEngineYmfm ADPCM L1 calibration）

- **コミット**: `9aa1f42428f940d7c1295da6b6d6779764367342 Apply ymfm ADPCM calibration in compatibility mode`（`origin/main` にpush済み）。
- **Issue**: `takamori-tech/libmucom88#97`。実装コメント: `https://github.com/takamori-tech/libmucom88/issues/97#issuecomment-4826823901`。
- **目的**: CLAUDIUS などの consumer が optional `FmEngineYmfm` を使うだけで、`MmlEngine` default `Tuned` 経由なら MUCOM88V の ymfm Tuned と同じ ADPCM-A/B L1 音量補正を受けられるようにする。
- **実装**: `chip_calibration.hpp` に `calibrateOpnaAdpcmRegister()` を追加。OPNA ADPCM-A `port0/0x11` は `kYmfmCalibration.adpcmATlOffset=12` を total level から減算し 0..63 clamp。ADPCM-B `port1/0x0B` は `kYmfmCalibration.adpcmBGain=1.044` を乗算し 0..255 clamp。
- **既存経路の整理**: `NormalizingChipBackend::writeReg()` の既存 ADPCM-A/B 補正を同 helper 経由へ移し、重複を排除。fmgen calibration は identity のため既存 fmgen 経路は維持。
- **FmEngineYmfm 適用点**: `FmEngineYmfm::writeReg()` と `playVoice()` の ADPCM-B volume write で、`compatibilityOutput` 有効時のみ同 helper を適用。Native profile / compatibility off では raw register write のまま。
- **検証済み**: `cmake --build build` PASS、`ctest --test-dir build --output-on-failure` PASS（3/3）、MUCOM88V `vendor/ymfm` headers を使った `ymfm_engine.hpp` compile smoke PASS。
- **consumer 指示（CLAUDIUS）**: `vendor/libmucom88` を `9aa1f42428f940d7c1295da6b6d6779764367342` へ更新する。`FmEngineYmfm + MmlEngine` default `Tuned` なら追加設定なしで ADPCM-A/B 補正が有効になる。通常ビルドと音声 smoke/regression で確認する。
- **consumer 指示（MUCOM88V）**: `vendor/libmucom88` を同 commit へ更新する。`muc_regtest` / MIDI golden は引き続き fmgen oracle 明示固定を維持し、`./scripts/verify.sh --clean` で submodule bump を検証する。MUCOM88V production `YmfmBackend` の移植拡大は今回しない。
- **非対象として残す範囲**: PolyDecimator、ADPCM-A/B calibration の追加再調整、ADPCM no-data guard、MUCOM88V production `YmfmBackend` への移植拡大。

## 直近ハンドオーバー（2026-06-29 / #95 ymfm key-on retrigger deferral）

- **コミット**: `c0d9f748c3c6434049f4093f3cce281cbf6bc311 Add ymfm key-on retrigger deferral`（`origin/main` にpush済み）。
- **直前コミット**: `e8b6bc3 Use soft limiter for MML mixed output`（同じくpush済み）。
- **Issue**: `takamori-tech/libmucom88#95`。進捗コメント: `https://github.com/takamori-tech/libmucom88/issues/95#issuecomment-4826711317`。
- **目的**: optional `FmEngineYmfm` で同一 FM channel の key-on retrigger が欠落する問題を、ymfm write path 側で deferral して補正する。既存 API は変更しない。
- **libmucom88 検証済み**: `cmake --build build` 成功、`ctest --test-dir build --output-on-failure` 成功、MUCOM88V `vendor/ymfm` を使った optional ymfm smoke compile/run 成功。
- **MUCOM88V 取り込み済み**: `1a7a677f88347bc56baefac1b85e6f6e6a2be658 Bump libmucom88 for ymfm retrigger deferral`（MUCOM88V `origin/main` にpush済み）。`vendor/libmucom88` は `bdf069b -> c0d9f74`。
- **MUCOM88V 親側の重要な切り分け**: `muc_regtest` の avgRMS gate 失敗は libmucom88 退行ではなく、MUCOM88V `OpnaEngine` default が `Ymfm` になったことによる backend drift。過去ドキュメント上、`muc_regtest` と MIDI golden は fmgen oracle。
- **MUCOM88V 親側修正**: `muc_regtest` と `muc_miditest` は `FmEngineType::Fmgen` を明示指定。`muc_regtest` は `vendor/mucom88` が fmgen OPNA symbols を持つため direct `fmgen` link を外し、regtest 限定で `MUCOM88V_FMGENBACKEND_NO_STEMS` により `FmgenBackend::mixStemChunk()` を no-op 化。
- **MUCOM88V 検証済み**: `./scripts/verify.sh --clean` PASS。`muc_regtest -sec 20`: `Files: 127 OK, 5 COMPILE FAILED`, `Mean 1.002`, `Median 1.002`, `>=0.8: 127 (100%)`。`muc_miditest --compare`: `PASS=18 FAIL=0`。
- **consumer 指示**: MUCOM88V / CLAUDIUS とも libmucom88 `c0d9f74` 以降を取り込む。consumer 側 regression/golden は backend default に依存させず、fmgen oracle のテストは明示 fmgen、ymfm 確認用テストは明示 ymfm を選ぶ。CLAUDIUS には MUCOM88V のテスト修正を機械的にコピーせず、CLAUDIUS 側 oracle を確認してから固定する。
- **別 issue 化済み未対応範囲**: no-data guard `#96`、PolyDecimator `#98`。ADPCM calibration `#97` は `9aa1f42` で libmucom88 `FmEngineYmfm` consumer 向け L1 補正を実装済み（追加再調整は非対象）。

## 直近ハンドオーバー（2026-06-28 / engine別Tuned出力プリセットの正本化）

- **コミット**: `93e1cac Add shared output tuning defaults`（`origin/main` にpush済み）。
- **目的**: MUCOM88V OUTPUTタブで作ったengine別 `Tuned` プリセットをゲーム側CLAUDIUSでも既定適用できるよう、出力プリセット定義をlibmucom88の正本へ移した。
- **追加API/正本**: `include/mucom88/chip_output_tuning.hpp`。`ChipOutputProfile::{Native,Tuned}`、`ChipOutputTuning`、`chipOutputTuningFor()`、`defaultChipOutputTuningFor()`、`effectiveSsgMixScaleFor()` を追加。Tuned値は fmgen=`SSG -3.0dB / output 1.0x / compatibility off`、ymfm=`SSG -4.0dB / output +2.5dB / compatibility on(1.9x + soft limiter)`。
- **MmlEngine既定**: `MmlEngine::init()` は `defaultChipOutputProfile()`（現状 `Tuned`）を適用する。`setOutputProfile()` / `outputProfile()` を追加し、Nativeへ戻すことも可能。Rich SEチップにもBGM側と同じSSG mix / compatibilityOutputを同期する。
- **IFmEngine / ymfm**: `IFmEngine::chipEngine()`（既定 `ChipEngine::Fmgen`）、`setCompatibilityOutput()`、`compatibilityOutputEnabled()` を追加。`FmEngineYmfm` は `ChipEngine::Ymfm` を返し、compatibilityOutput有効時に `chip_output_tuning.hpp` のTuned互換段を適用する。
- **ドキュメント**: `README.md`、`docs/api_reference.md`、`docs/integration_guide.md` に `chip_output_tuning.hpp`、Tuned既定、`setOutputProfile()`、互換出力段を反映。古い `setOutputGain(2.0f)` 推奨例は、プリセット値の必要時上書き例へ変更済み。
- **検証済み**: standalone `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -- -j8 && ctest --test-dir build --output-on-failure` 成功。CLAUDIUS側では `6bbca29 Use libmucom88 tuned output defaults` で `vendor/libmucom88` を `93e1cac` へ更新し、手動 `setOutputGain(2.0f)` を削除、`./scripts/build_game.sh --release -j8` 成功・push済み。
- **次回注意**: MUCOM88V側の `vendor/libmucom88` も正本 `93e1cac` へ合わせる。MUCOM88VのOUTPUT UI側に残る同方向のローカル変更は、libmucom88正本の `chip_output_tuning.hpp` 参照へ整理してからcommit/pushする。

## 直近ハンドオーバー（2026-06-27 / ymfm OPNA互換アダプタ）

- **コミット**: `bc3796c Add optional ymfm OPNA engine adapter`（`origin/main` にpush済み）。
- **Issue**: `takamori-tech/libmucom88#90` は completed close 済み。
- **目的**: CLAUDIUS がfmgen実装に依存せず、チップ忠実なymfm OPNAへ切り替えられるようにする。ただし既存 `MmlEngine` / `IFmEngine` API形状は維持し、既存fmgen利用者を壊さない。
- **追加API**: `include/mucom88/ymfm_engine.hpp` の `FmEngineYmfm : IFmEngine`。OPNA固定、`CHIP_CLOCK=7987200`、DACモデル既定true、`FIDELITY_HIGH=1`（ymfm `OPN_FIDELITY_MAX`）既定、`FIDELITY_MED=0` も選択可。
- **依存方針**: libmucom88コアは外部依存なしのまま。`ymfm_engine.hpp` をincludeする利用者だけがymfm include pathと `ymfm_adpcm.cpp` / `ymfm_misc.cpp` / `ymfm_opn.cpp` / `ymfm_ssg.cpp` linkを追加する。
- **実装範囲**: ADPCM-A ROM読出、ADPCM-B RAM読書き、`loadPcmDataToAdpcmB`、voice table、`playVoice(level)`、`stopAdpcmB`、`generateInterleaved` を `IFmEngine` 互換で提供。
- **検証済み**: core headers はymfmなしでコンパイル成功、standalone build + `adpcm_a_roundtrip` 成功、`ymfm_engine.hpp` smoke compile 成功（警告はupstream ymfm unused parameterのみ）。CLAUDIUS側では `7855863` で `vendor/libmucom88` を `bc3796c` へ更新し、`vendor/ymfm` submoduleを追加済み。
