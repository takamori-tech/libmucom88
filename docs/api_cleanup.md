# APIの移行と互換性

過去のAPI整理で削除・非公開化された入口と、現在の利用方法をまとめます。
新しい削除を指示する設計案ではありません。現在の宣言は
[APIリファレンス](api_reference.md) と [公開ヘッダー](../include/mucom88/) を参照してください。

## 旧APIからの移行

| 旧API | 現在の状態 | 移行方法 |
| --- | --- | --- |
| `MmlEngine::loadMml()` | 存在しない | `MmlParser::parse()` → `loadFromParseResult()`、または `setEvents()` |
| `parseSingleChannelMml()` | 存在しない | トラック記号を付けて `parse()` し、`channelEvents[ch]` を使う |
| `MmlEngine::loadPcmFile()` | 存在しない | 用途に応じて `loadPcmData()`、`loadPcmBinary()`、`loadPcmBinaryFile()` |
| `globalLoopRestart()` / `perChannelRestart()` | `MmlEngine` のprivate | 通常は `L` と `setLoop()` による内部制御へ任せる。直接の公開代替はない |
| `setGlobalAttenuation()` | `MmlEngine` のprivate | マスター・BGM音量、ダッキング、フェードの公開APIから目的に合うものを選ぶ |
| `globalAttenuation()` | 存在しない | 内部減衰値への依存を除き、利用側の設定値を管理する |
| フリー関数 `makeDefaultPatch()` | `MmlEngine` のprivate staticへ移動 | 独自の `FmPatch` を作り `setPatch()` で登録する |

`play()` は曲全体の状態をリセットするため、パート別ループ再開の同等な置き換えではありません。
音量APIもPCM振幅や減衰単位が異なり、旧呼び出しとの機械的な置き換えは避けます。

## 単一パートを読み込む例

次の関数は再生停止中、`engine.init()` 後に呼びます。

```cpp
#include <mucom88/mml_engine.hpp>

void loadSinglePart(MmlEngine& engine)
{
    MmlParser parser;
    const auto song = parser.parse("D T120 @0 o4 l4 v12 cdef\n");
    engine.loadFromParseResult(song);
    engine.setLoop(false);
}
```

既存曲の特定パートだけを入れ替える場合は `setEvents(ch, song.channelEvents[ch])` が使えます。
その場合は音色・全音符tick・ループなど、他の設定を自動反映しない点に注意してください。

## PCMロードの違い

- `loadPcmData()` はテーブルの解析だけを行い、音声データをバックエンドへ転送しません。
- `loadPcmBinary()` は両方を試みますが、内部の失敗を伝播しません。
  非nullの0x400バイト超データに対するtrueは、PCM再生準備完了の保証ではありません。
- 転送の成否を確認する方法は [組み込みガイド](integration_guide.md#mucompcmbin) を参照してください。

## 今後のAPI変更

利用者が既知のプロジェクトだけであるとは仮定しません。
公開型、列挙値、構造体、既定値、仮想関数、戻り値の意味を変更するときは、利用側への影響と移行方法を記録します。
既存派生クラスにoverrideを要求しない追加でも、バイナリABIの互換性が保証されるわけではありません。

変更時は、関連するコンパイル例・単体テスト、必要なMML・音声回帰を確認します。
利用側のsubmodule更新は承認済みの範囲で行い、利用側固有のビルド・検証結果を別途記録します。
文書だけの更新では、依存リポジトリの更新を必須にしません。

旧設計と実施時の記録は `git log -- docs/api_cleanup.md` から参照できます。
