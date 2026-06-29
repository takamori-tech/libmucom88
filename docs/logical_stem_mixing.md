# Logical Stem Mixing

`mucom88/logical_stem_mixer.hpp` は、`IChipBackend::mixStemChunk()` が返す
`ChipStemFrame` を、論理 stem 単位で 64-bit double accumulation する opt-in helper です。
既存の `MmlEngine` や `IFmEngine` の通常出力には自動接続されません。consumer が明示的に
include し、`LogicalStemMixOptions::enableDoubleStemSumming = true` を渡した場合だけ使われます。

![Logical stem mixing](diagrams/logical_stem_mixing.svg)

## Scope

この helper が扱う責務は、backend-output stem の加算と final float frame への変換だけです。
UI、DAW bus 数、plugin state、ホスト固有の parallel out mapping は consumer 側の責務です。

対象 stem は OPNA/OPN で使う `FM1..FM6`, `SSG1..SSG3`, `Rhythm`, `ADPCM-B` です。
OPNB/OPM は stem coverage と bus mapping を consumer 側で確認してから使う必要があります。

## API shape

主な型は次の通りです。

```cpp
#include <mucom88/logical_stem_mixer.hpp>

LogicalStemAccumulator acc;
acc.addStem(stemFromBackend, fadeGain);
acc.addFallbackStereo(left, right, fadeGain);

LogicalStemMixOptions options;
options.enableDoubleStemSumming = true;
options.outputScale = 1.0 / 32768.0;
options.masterGain = master;

LogicalStemFloatFrame out;
if (writeLogicalStemFloatFrame(acc, options, out)) {
    // out.main / out.fm / out.ssg / out.rhythm / out.adpcmB を consumer が配線する
}
```

`enableDoubleStemSumming` の既定値は `false` です。false の場合
`writeLogicalStemFloatFrame()` は `false` を返し、consumer は従来経路へ戻れます。

## Ordering contract

libmucom88 は emulator core の内部演算を float 化しません。fmgen/ymfm などの backend は従来どおり
native な整数系 sample を作ります。`LogicalStemAccumulator` は、その backend boundary の
`ChipStemFrame` を論理 stem として加算します。

Native/Tuned、SSG mix、ADPCM 補正、Rhythm、DAC/Hi-Fi、Soft Limiter 相当の処理は、各 backend が
`ChipStemFrame` を返す前に既存契約どおり適用する必要があります。この helper はそれらの順序を
並べ替えません。

## Headroom

同相の信号を足した場合、peak level は `20 * log10(N)` dB 増えます。

| Summed signals | Peak increase |
| ---: | ---: |
| 2 | +6.0 dB |
| 4 | +12.0 dB |
| 8 | +18.1 dB |
| 11 OPNA logical stems | +20.8 dB |
| 8 polyphonic instances x 11 stems | +38.9 dB |

double accumulator の目的は、内部の stem / polyphonic 合算で早すぎる clip と丸め誤差を避けることです。
final float は `1.0` を超える値を運べますが、fixed-point 書き出し、DAC、後段 plugin では
clip し得るため、最終的な level management は consumer 側で行います。

## Soft limiter relationship

Soft limiter は summing precision とは別の nonlinear safety stage です。ymfm などの backend が
Tuned 互換出力段内で limiter を使う場合、それは `LogicalStemAccumulator` に入る前の
backend-output stem に反映されます。logical stem mixer は post-master limiter ではありません。

consumer が「double summing 後にも limiter が必要」と判断する場合は、final float frame の後段に
consumer 固有の処理として追加します。
