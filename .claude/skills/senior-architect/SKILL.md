---
name: senior-architect
description: >-
  高難度レビュー手法（Fable 5 由来）を「間違えると高くつく」判断の前に必ずロードする。
  対象: 設計レビュー / コードレビュー / 監査 / 根本原因の確定 / オーディオパス実時間安全性
  (advance/renderMixed/generateInterleaved の noexcept・alloc/lock 禁止) や MML 再現精度に
  関わる判断。libmucom88（ヘッダオンリー MML パーサ＋シーケンサ）向けに翻案（カーネル/DMA →
  オーディオ実時間 / MML 再現精度）。severity 較正、RELEASE-vs-DEBUG、証拠誠実性（過大主張禁止）、
  反証優先の仮説規律、二段階レビュー、横展開を含む。
---

# Senior-Architect Method（libmucom88 翻案版）

「間違えると高くつく」判断（オーディオパス実時間安全性、MML 再現精度、後方互換性の非退行、
並行性/生存期間）にはこの手法を毎回適用する。グローバル CLAUDE.md のモデル分担ルールでは、
この層は本来 Fable 5 を召喚する。本 skill はその手法を Opus 4.8 が常時安価に実行できる形へ
蒸留したもの。

> 単発の OK 出しより *厳しく* 運用する。蒸留版の弱点は「推測による水増し」
> 「severity 較正ミス」「機構を断定するが実は誤り」。下のチェックリストが解毒剤。

このプロジェクトの一次資料（裏取り先）:
- `docs/Z80_vs_MmlEngine.md` — MmlEngine と Z80 正本の MML 再現精度差分表（エンジン挙動変更時は更新必須）
- OpenMUCOM88 Z80 `music.asm` — MML 挙動の正本（mucom88v `vendor/mucom88/`）
- fmgen — YM2608 エミュレーション（mucom88v `vendor/fmgen/`）
- OPNA / YM2608 データシート — レジスタ仕様・タイミング
- `include/mucom88/*.hpp` — 本ライブラリ本体（ヘッダオンリー。`fm_common.hpp` は他プロジェクト共有＝後方互換性注意）
- `docs/cpp_coding_standards.md` — noexcept / static_cast / RT 安全規約・ヘッダオンリー制約
- `docs/api_reference.md` / `docs/integration_guide.md` — API 契約の権威文書
- mucom88v `build/muc_regtest` / `build/muc_compare` — 証拠生成ツール（本リポジトリにテストは無い）

## 0. スコーピング（着手前）

- [ ] **正しさの境界を明示する。** severity はこの境界に対して判定する。例:
      「regtest 全曲 avgRMS >= 0.8 を割らない（目標 Median ~1.000、0.95-1.05 圏内）」
      「`advance()` / `renderMixed()` / `generateInterleaved()` で alloc / throw / lock / syscall なし」
      「OpenMUCOM88 と機能的完全一致（許容差: Z80 起動遅延・Timer-B 0x27 書き込み回数差 等のみ）」
      「`fm_common.hpp` / 公開 API の後方互換性を壊さない」。
- [ ] **証拠の段階を宣言する。** source-only / 単体ヘッダ検証済み(`g++ -std=c++17 -Wall -Wextra -I include`) /
      mucom88v ビルド済み(警告ゼロ) / regtest 実行済み(数値添付) / 実音確認済み のどれか。
      対象コミットを固定する。本リポジトリ単体にはテストが無いため、再現精度の主張は mucom88v 側の
      regtest 数値が無い限り **Unknown**。

## 1. レビューループ（候補となる指摘/判断ごとに回す）

- [ ] **エントリポイント優先。** オーディオコールバック経路（`advance()` → 内部 16 サンプル分割 →
      `IFmEngine::writeReg()` / `generateInterleaved()`）と、利用側からの入口（`parse()` 出力、
      `setEvents()` / `setPatch()` / `loadPcmBinary()`、PCM/voice テーブルのロード）を先に読み、
      危険な値や不変条件が *in-scope のコードに入る前に* すでに守られているかを証明する。
- [ ] **RELEASE vs DEBUG。** すべての防御を分類する。`assert` は DEBUG 専用。外部入力（MUC テキスト、
      voice.dat、mucompcm.bin、ボイステーブル）で変わる値を `assert` だけで守るのは **release ホール**。
      ヘッダオンリーかつ例外不使用（bool/optional 返し）なので、不正入力は黙って no-op/false に落ちる
      設計か、境界チェックがあるかを確認する。
- [ ] **到達性が severity を決める。** オフライン解析（パーサ）からのみ到達する欠陥より、
      **実時間オーディオパス**（`advance()`/`renderMixed()`）に到達する欠陥を上位に置く。
      実時間パスでの alloc/lock/throw は本プロジェクトのハードルール違反＝高 severity。
- [ ] **実時間安全（RT-safety）レンズ（DMA レンズの置換）。** `advance()` / `renderMixed()` /
      `generateInterleaved()` 内に、ヒープ確保（`new`/`vector::push_back`）・ロック取得・例外 throw・
      syscall・非有界ループが無いことを確認する。`noexcept` が付いているか。これらは API リファレンスで
      明示された契約 — 違反は契約破り。
- [ ] **並行性 / 生存期間。** MmlEngine はスレッドセーフでない設計（`advance()` と `playVoice()`/`playSe()`
      は同一オーディオスレッドから呼ぶ契約）。UI スレッドからの状態取得（`chNoteOn()` 等）は非アトミックだが
      表示用途では許容。この契約を破る共有（外部から writeReg 直叩きするハイジャック経路、ボイス override
      フラグ）の生存期間・順序を確認する。ポインタ（`IFmEngine*`、SE チップ）の寿命と再 init 時の整合も見る。
- [ ] **上流同一性。** 挙動差は OpenMUCOM88 `music.asm` / fmgen / OPNA データシートと diff し、
      「上流由来の潜在差」か「本ライブラリの退行」かをラベルする。
- [ ] **後方互換性レンズ。** `fm_common.hpp` と公開 API（IFmEngine の非 pure default、MmlEngine の
      公開メソッド、MucFile/MmlEvent の構造）の変更は mucom88v・CLAUDIUS 両方をビルド不能/挙動変化に
      しうる。シグネチャ・デフォルト引数・enum 値・構造体レイアウトの破壊的変更を高 severity で扱う。
- [ ] **確証の前に反証する。** 危険な解釈を *まず反証しにいく*。生き残ったら、抽象語でなく
      プリミティブ水準（確保サイズ・書き込む値・境界・外部制御ベクタ、または avgRMS への定量影響）で記述する。

## 2. 較正と誠実性（蒸留版が最も間違える 2 点）

- [ ] **severity は基準スケールで較正する（雰囲気でなく）。** 本物の BLOCKER を LOW へ下げない、
      診断経路の nit を BLOCKER へ盛らない。同一根本原因の複数面は同格で評価。
- [ ] **来歴の捏造禁止 / 過大主張禁止（本プロジェクトの第一リスク）。** 「修正した」を regtest 証拠
      なしに主張しない。実行していないツール結果を捏造しない。Issue 番号や数値を発明しない。
      **一次資料の裏付けが無い主張は Done でなく Unknown。** 緑の end-to-end 結果（regtest 緑）は、
      それが観測できない性質（実時間グリッチ、利用側統合の破壊）については *傍証* に格下げし証明扱いしない。
- [ ] **機構は根拠を示せる時だけ断定する。** 正しいパラメータ・誤ったモデルは偽陽性。「なぜ壊れるか」を
      断定する前に Z80 music.asm / fmgen ソース / OPNA データシートで実際の機構を確認する。

## 3. 仮説規律（根本原因の作業）

- [ ] 確信度付きでランク付けした仮説を立て、独立した参照を 3 つ以上で三角測量し、生の証拠を意味へ
      復号（例: レジスタ書き込み列のデコード、avgRMS/波形差の解釈、Timer-B ティック数の照合）し、
      対立仮説を敵対的に排除し、**一発で識別できるテスト**（と反証述語）を設計する。
      **反証された (REFUTED)** 仮説を記録し、自己訂正をオープンに行う。

## 4. 出力（機械可読・誠実性優先）

- [ ] 段階的な **verdict**（裸の pass/fail 禁止）: `PASS` / `PASS_WITH_NITS` /
      `APPROVE-WITH-FIXES` / `CHANGES_REQUIRED` / `REFUTED` / `accept-and-track`。
- [ ] severity スケール: `BLOCKER` > `HIGH` > `MEDIUM` > `LOW` > `NIT` > `none`
      （`none` = 調査済み・クリーン、証拠付きで記録）。`latent`（実在欠陥だが現境界では非到達）は直交。
- [ ] 番号付き指摘（`F1`/`A1`…）に `file:line`、severity、到達性、引用証拠、影響（可能なら avgRMS 等の
      定量、または利用側への後方互換影響）、修正スケッチ、確信度を付す。
- [ ] 調査してクリーンだった箇所は **clean-with-evidence** として記録。不確実性は捏造でなく明示。
- [ ] **横展開:** あるクラスの欠陥が確定したら、類似箇所（他チャンネル種別 FM/SSG/ADPCM、他パート、
      Classic/Rich SE 経路、ループ巻き戻し経路）を全て掃き、CLEAN / not-CLEAN を記録する。

## 5. オーディオパス & MML 再現クリティカルなコードは二段階で

- [ ] Pass 1 は敵対的に探索し、修正は実装者（Codex / subagent）へ渡す（レビューは
      **read-only — 自分でコミットしない**）。Pass 2 で変更後コードを読み直し、ヘッダ単体検証
      （`g++ -std=c++17 -Wall -Wextra -I include`）→ mucom88v で **regtest を実行**して avgRMS 退行が
      無いこと（必要なら実音）を確認し HIGH を閉じる。最難の並行性/実時間項目は単発 OK でなく
      明示的な二回目読み直しを優先する。

## 6. ルーティング & ゲート

- [ ] ネガティブルーティング: 決定的なテーブル写像、ビルド時文字列、構造的にソースから検証可能な
      作業はこの層を要さない — 理由を記録して労力を節約。
- [ ] 実装は Codex へ委譲し、アーキテクト（main session）がレビューする。Codex は強力な
      breadth/second-opinion 生成器だが **最終裁定者ではない** — 較正と過大主張ゲートはこちらに残す
      （グローバル CLAUDE.md のモデル分担ルール）。
