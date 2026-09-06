# libmucom88 作業記録

再開時はGitの作業状態・branch・直近commitと、この記録を確認します。
実行規約は [AGENTS.md](../AGENTS.md)、技術規約は [CLAUDE.md](../CLAUDE.md) が正本です。

## 2026-09-06 文書全体を現行実装に合わせて再構成

利用者からREADMEに続いて、その他の文書も実態に合わせた改稿を依頼された。
開始時はmain、HEAD `646b8a7`、作業ツリーclean。
照合対象のC++・CMakeはこのrevisionから変更していない。

### 変更

- [文書一覧](README.md) を新設し、利用・API・互換性・移行・開発の入口を整理。
- APIリファレンスを公開宣言と照合。存在しないAPI・enum、privateメソッド、テンポやpanの誤記を修正。
- 組み込みガイドを、CMake、バックエンド初期化、BGM、PCM、ボイス、SE、音量、同期の順に再構成。
  `renderMixed()` に時間更新・PCM生成をまとめ、フェード順序と失敗判定を修正。
- SSGの音出しに内蔵プリセット `@0` が必要なことを実行で確認し、READMEを含む該当例を修正。
- Z80比較文書を現在の処理・互換性の境界・検証方法へ改稿。
  旧API整理案を移行案内に変更。利用者数・完全一致・未実装なしといった未確認の断定を除去。
- logical stemのmain再構成、二重加算、無効時の出力保持を明記。
  構成・Rich合算・ボイス優先制御の図を作り直し、SE・音量図の記載も修正。
- 開発規約から特定の利用側リポジトリを前提にする手順を除去。
  音声経路の方針と既存ymfmの制約を区別し、30秒／曲・avgRMS基準、承認、レビュー条件を維持。
- 作業記録を現状中心に整理。旧記録はGit履歴で保持。

### 実行証拠

文書からコードブロックを抽出して検証した。以下のスクリプト・ログは実行時のローカル一時成果物で、
リポジトリ付属ツールではない。

| 確認 | 実出力・結果 |
| --- | --- |
| C++例11件 | `c++ -std=c++17 -Wall -Wextra -Werror ... -fsyntax-only`：全件exit 0 |
| README解析例 | `First melody` / `D: 8 notes`、exit 0 |
| 文書のCMake構成 | 外部の最小プロジェクトでconfigure・build、各exit 0 |
| ymfmアダプタ | 文書例のcompile/link成功。`frames=4096, nonzero=1`、exit 0 |
| Markdown | ローカルファイル・見出しリンクの存在を照合 |
| SVG | 全7図のXML解析成功。大きく変更した4図をQuick Lookで描画し、文字・配置を目視確認 |
| 差分 | `git diff --check`。変更範囲は文書・図のみ |

ymfmは `17decfae857b92ab55fbb30ade2287ace095a381`、AppleClang 21.0.0.21000101で確認。
初回の音出し例はSSGプリセット未指定で `nonzero=0` / exit 2だった。
`@0` を加えた後、同じ4096フレームで非ゼロPCMを確認した。ライブラリの実装変更は行っていない。

実行した検証スクリプト:

```text
/private/tmp/validate_libmu_docs.py
/private/tmp/check_libmu_consumer.py
```

ログ・描画画像: `/private/tmp/libmucom88-docs-20260906/`。
`consumer.log` が修正後、`consumer-initial-silence.log` が初回の無音結果。
レビューはCodexの自己レビューと読み取り調査の分担で実施。正式な独立承認ゲートではない。

### 検証範囲と再開状態

文書のみのためライブラリ全体のCMake/CTest・全曲回帰は `skip`。
上記の例の実行は、全曲互換性・実時間安全性・ボイス/SEの包括的な回帰結果ではない。
実音・DAW・操作感は `NOT_VERIFIABLE`。

現行の構文診断不足、PCM統合ロードの戻り値、ymfmのmutex/RAM処理、固定Timer-B周期等は文書化した制約であり、
今回コードを修正した事項ではない。これらの実装修正を自動的に開始しない。
反映先は利用者の既存承認に従いmain。commit/pushの結果はGitとremoteの一致が正本。
次は利用者の次の依頼を確認する。ローカル限定の未追跡・無視対象ファイルは保持した。

## 過去の記録

旧作業記録には当時の利用側への取り込み指示や特定環境の測定値が含まれる。
現在の更新指示・対応機能・検証結果として読み込まない。
改稿前の全文は履歴から参照できる。

```bash
git show 646b8a7:docs/session_log.md
git log -- docs/session_log.md
```
