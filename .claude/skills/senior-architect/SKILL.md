---
name: senior-architect
description: libmucom88 の実時間性、MML再現、並行性・寿命、公開APIの高難度レビュー。文書や自明な変換では不要。
---

# libmucom88 高難度レビュー

[共通規約](../../../CLAUDE.md) の「高難度レビュー手法」と、
[開発規約](../../../AGENTS.md) の「Code Review Rules」を読み、対象に応じて適用する。
このskillは同じ手法への任意の入口であり、他モデルの起動・認証・外部投稿の許可を与えない。

- 音声経路は入口・到達性・確保/ロック/throw・同期と寿命を確認する。
- MMLはZ80/fmgenの一次資料と再現を照合し、公開APIはconsumerの契約まで追う。
- 単体テストはtests/とCMakeLists.txtにある。全曲再現はconsumerのfmgen oracleが必要。
- 検証とレビューの範囲を明示し、自己レビューを独立レビューとしない。
  結果は影響、file:line、根拠、未確認事項で示す。証拠のない断定や固定数の資料集めを要求しない。
