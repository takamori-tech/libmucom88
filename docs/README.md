# libmucom88 ドキュメント

MUCOM88形式のMMLを解析し、音源バックエンドへ演奏指示を渡すC++17ライブラリの資料です。
初めて使う場合は、リポジトリの [README](../README.md) から始めてください。

| 目的 | 資料 |
| --- | --- |
| アプリケーションにBGM・ボイス・効果音を組み込む | [組み込みガイド](integration_guide.md) |
| 公開型・メソッド・戻り値を調べる | [APIリファレンス](api_reference.md) |
| パート別のPCM出力を合算する | [Logical Stem Mixing](logical_stem_mixing.md) |
| MML再現の仕組みと検証範囲を確認する | [MUCOM88互換性と検証](Z80_vs_MmlEngine.md) |
| 旧APIから移行する | [APIの移行と互換性](api_cleanup.md) |
| 開発・検証の手順を確認する | [開発規約](../CLAUDE.md) / [C++規約](cpp_coding_standards.md) |
| Codexで開発を再開する | [AGENTS.md](../AGENTS.md) / [作業記録](session_log.md) |

実際の宣言と処理は [include/mucom88](../include/mucom88/)、単体検証は [tests](../tests/) が正本です。
本文書群は2026-09-06に現行実装と照合しました。APIや挙動を変更するときは対応する資料も更新します。
過去の設計案・測定結果はGit履歴に残し、現在の対応機能や検証済み範囲として扱いません。
