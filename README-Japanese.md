# VSTCompare

VSTCompare は、2つの VST3 プラグインを比較するための Windows 専用 C++17 コマンドラインツールです。両方のプラグインをホストし、決定論的なテスト信号を内部生成して音響挙動を測定し、JSON データを埋め込んだ自己完結型の HTML レポートを出力します。

目的は、プラグイン同士のキャラクターを客観的に A/B 比較することです。周波数特性、位相、過渡応答、歪み、ステレオ幅、ピッチ追従、ダイナミクス、時間応答を測定します。外部の音声ファイル入力は不要で、レポートは人間にも、埋め込み構造化データを読むツールにも扱いやすい形式です。

英語版は [README.md](README.md) を参照してください。

## ステータス

現在は Windows のみをサポートします。`CMakeLists.txt` は Windows 以外のビルドを拒否します。

## 前提条件

- Windows
- CMake 3.25 以降
- MSVC / Visual Studio
- `external/vst3sdk` に配置された VST3 SDK

VST3 SDK を別の場所に置く場合は、CMake 設定時に `VST3_SDK_ROOT` を指定してください。

## ビルド

```powershell
cmake -S . -B build
cmake --build build --config Release
```

リポジトリ外の VST3 SDK を使う場合:

```powershell
cmake -S . -B build -DVST3_SDK_ROOT="C:\path\to\vst3sdk"
cmake --build build --config Release
```

Release ビルドの実行ファイルは次の場所に生成されます。

```text
build\bin\Release\vstcompare_cli.exe
```

## 実行

```powershell
build\bin\Release\vstcompare_cli.exe --plugin-a "C:\Path\To\PluginA.vst3" --plugin-b "C:\Path\To\PluginB.vst3" --out reports --non-interactive
```

`--out` がディレクトリを指す場合、VSTCompare は必要に応じてディレクトリを作成し、次の形式でレポートを出力します。

```text
test_report_<PluginA>_vs_<PluginB>.html
```

`--out` が `.html` ファイルを指す場合は、そのファイルパスがそのまま使われます。

`--non-interactive` を付けない場合、標準入力が対話型であれば、公開されている各プラグインパラメータの入力を順番に求めます。空欄にしたパラメータはスキップされます。

## CLI オプション

| オプション | 必須 | 説明 |
| --- | --- | --- |
| `--plugin-a <path>` | はい | 1つ目の `.vst3` プラグインへのパス。 |
| `--plugin-b <path>` | はい | 2つ目の `.vst3` プラグインへのパス。 |
| `--out <dir-or-file>` | はい | 出力先ディレクトリ、または `.html` レポートファイル。 |
| `--plugin-a-class-id <32hex>` | いいえ | プラグイン A の特定の VST3 class ID を選択します。 |
| `--plugin-b-class-id <32hex>` | いいえ | プラグイン B の特定の VST3 class ID を選択します。 |
| `--sample-rate <value>` | いいえ | 処理サンプルレート。既定値は `48000`。 |
| `--block-size <value>` | いいえ | 処理ブロックサイズ。既定値は `512`。 |
| `--non-interactive` | いいえ | パラメータ入力を省略し、既定または現在のプラグインパラメータで実行します。 |
| `--help`, `-h` | いいえ | 使い方を表示します。 |

## テスト仕様

VSTCompare は、内部生成した固定のテスト信号で一連の測定を実行します。既定の処理設定はサンプルレート `48000` Hz、ブロックサイズ `512` で、どちらも CLI から変更できます。多くのテストでは、ステレオ出力を L/R 平均のモノラル信号に変換して解析します。ただし mono-to-stereo width テストでは L/R チャンネルを保持します。各テストは、プラグインが報告したレイテンシ、解析時に適用した整列ディレイ、クランプされたレイテンシ、解析品質に関する警告を記録します。

### インパルス応答

インパルス応答テストは、過渡応答、リンギング、テール成分、残留レイテンシ差を確認します。

- 入力信号: モノラルのデジタルインパルス。長さ `500 ms`、先頭サンプルのみ振幅 `1.0`、残りは無音。
- 解析内容: 両プラグインに同じインパルスを通し、出力を解析用モノラルへ変換します。プラグインが報告したレイテンシに基づいて整列し、A-minus-B の差分波形を作成します。絶対差分が大きい上位5サンプルを抽出し、整列後の出力ピーク位置から残留レイテンシを推定します。
- レポート内容: 入力/出力/差分波形、表示用に整列したチャンネル波形、`peakAbsDelta`、`energyDelta`、`estimatedLatencyMs`、プラグインレイテンシサンプル数、整列ディレイ、レイテンシクランプ警告。

### 周波数特性

周波数特性テストは、音色バランスや EQ 的な差分を測定します。

- 入力信号: 決定論的なモノラルホワイトノイズ。長さ `2000 ms`、`-12 dBFS` RMS、固定 seed。
- 解析内容: FFT サイズ `65536`、Hann 窓、`50%` オーバーラップで平均パワースペクトルを計算します。各出力スペクトルを入力スペクトルで正規化し、A-minus-B の dB 差分を算出します。差分スペクトルは `20 Hz` から `20 kHz` までの30個の1/3オクターブ相当帯域に要約します。
- レポート内容: 入力スペクトル、A/B の正規化スペクトル、差分スペクトル、1/3オクターブ帯域テーブル、`peakAbsDeltaDb`、`meanAbsDeltaDb`、推定残留レイテンシ、整列警告。

### 位相特性

位相特性テストは、位相回転や周波数依存の時間差を測定します。

- 入力信号: 決定論的なモノラルホワイトノイズ。長さ `2000 ms`、`-12 dBFS` RMS、固定 seed。
- 解析内容: 入力と出力のクロススペクトルから伝達位相を計算します。FFT サイズは `65536`、Hann 窓、`50%` オーバーラップです。位相は `-pi` から `+pi` にラップし、A-minus-B の位相差を計算します。さらに `20 Hz` から `20 kHz` までの24個の対数帯域に要約します。
- レポート内容: A/B の位相カーブ、位相差カーブ、24帯域サマリ、`peakAbsDeltaRad`、`meanAbsDeltaRad`、推定残留レイテンシ、整列警告。

### 倍音歪み

倍音歪みテストは、固定サイン波に対する倍音キャラクターを測定します。

- 入力信号: `100 Hz`、`1 kHz`、`5 kHz`、`10 kHz` のモノラルサイン波。各 `1000 ms`、`-6 dBFS`。
- 解析内容: 先頭 `200 ms` をスキップして解析します。Blackman-Harris 窓の FFT サイズ `65536` で dBFS スペクトルを測定し、各基音に対して1次から10次までの倍音ピークを抽出します。各次数の振幅を A-minus-B で比較し、倍音近傍を除外したノイズフロアも推定します。
- レポート内容: 周波数ごとのスペクトル、倍音次数テーブル、倍音差分、A/B のノイズフロア、`noiseFloorDeltaDb`、`peakAbsDeltaDb`、`meanAbsDeltaDb`、周波数ごとの整列警告。

### THD / THD+N

THD / THD+N テストは、入力レベルに対して歪みがどう変化し、どこから飽和やクリップに近づくかを測定します。

- 入力信号: `1 kHz` のモノラルサイン波。`-60 dBFS` から `+6 dBFS` まで `2 dB` 刻みでスイープし、各ポイントは `1000 ms`。
- 解析内容: 各ポイントの先頭 `200 ms` をスキップします。Blackman-Harris 窓の FFT サイズ `65536` を要求し、解析可能なサンプル数が足りない場合だけ短縮します。THD は2次から10次までの倍音成分を基音比で算出します。THD+N は基音帯域を除外した残留パワーを基音帯域と比較します。スイープは `low` (`-60` から `-30 dBFS`)、`mid` (`-28` から `-10 dBFS`)、`high` (`-8` から `0 dBFS`)、`overdrive` (`2` から `6 dBFS`) に要約します。
- レポート内容: A/B の THD と THD+N カーブ、パーセント差分、セグメントサマリ、`peakAbsThdDeltaPercent`、`meanAbsThdDeltaPercent`、`peakAbsThdnDeltaPercent`、`meanAbsThdnDeltaPercent`、基音検出失敗、フロア張り付き、FFT 短縮、チャート上限クリップの警告。

### Mono-to-Stereo Width

Mono-to-stereo width テストは、モノラル入力がどの程度広がるか、狭まるか、デコリレートされるかを測定します。

- 入力信号: L/R が同一のステレオピンクノイズ。長さ `500 ms`、`-12 dBFS` RMS、固定 seed。
- 解析内容: ステレオ出力を保持します。モノラル出力はステレオに複製し、3チャンネル以上の出力は L/R のみを使用します。整列後、`mid = (L + R) / 2` と `side = (L - R) / 2` を計算します。時間方向では `20 ms` 窓、`10 ms` hop で mid/side 比と width percentage を測定します。周波数方向では FFT サイズ `65536`、`50%` オーバーラップで mid/side 比を計算し、`20 Hz` から `20 kHz` までの24対数帯域に要約します。
- レポート内容: 時間方向の width、周波数方向の width、24帯域サマリ、`peakAbsTimeDeltaDb`、`meanAbsTimeDeltaDb`、`peakAbsBandDeltaDb`、`meanAbsBandDeltaDb`、時間/帯域ごとの width percentage 差分、チャンネル形状警告、整列警告。

### ピッチ追従

ピッチ追従テストは、ピッチ系プロセッサが変化するピッチにどう反応するかを測定します。

- 入力信号: `110 Hz` から `880 Hz` までのモノラル対数サインスイープ。長さ `5000 ms`、`-6 dBFS`、開始/終了に `5 ms` フェード。
- 解析内容: レイテンシ整列後、`100 ms` フレームを `10 ms` 間隔で取り、立ち上がりゼロクロス間隔からピッチを推定します。各プラグインの出力ピッチを入力リファレンスと比較し、A/B 間のピッチ差も計算します。弱いトーンやノイズ状の出力を判断できるよう、有効フレーム率も記録します。
- レポート内容: 入力/出力ピッチカーブ、`meanAbsErrorHzA`、`meanAbsErrorHzB`、`meanAbsDeltaHz`、`peakAbsDeltaHz`、`validFrameRateA`、`validFrameRateB`、推定残留レイテンシ、有効率低下警告。

### ダイナミクス

ダイナミクステストは、静的なレベル伝達、ゲインリダクション、コンプレッサー的な挙動を測定します。

- 入力信号: `1 kHz` のモノラルサイン波。レベルを `-90 dBFS` から `0 dBFS` まで `1 dB` 刻みで変化させ、各ステップは `50 ms`。
- 解析内容: 整列後、各ステップ中央付近の `20 ms` RMS を測定します。I/O カーブを作成し、ゲインリダクションを入力レベル minus 出力レベルとして算出します。スムージングした I/O 傾きが `0.9` を下回る位置をスレッショルド候補とし、スレッショルドより `6 dB` 以上上の領域からレシオを推定します。ニー幅も遷移領域から推定します。
- レポート内容: I/O カーブ、ゲインリダクションカーブ、出力レベル差、ゲインリダクション差、A/B それぞれの推定 threshold/ratio/knee、`thresholdDeltaDb`、`ratioDelta`、`kneeWidthDeltaDb`、出力差分とゲインリダクション差分の peak/mean、フィッティング警告。

### 時間応答

時間応答テストは、バーストレイテンシ、エンベロープ速度、リリース挙動、残留リンギングを測定します。

- 入力信号: `1 kHz`、`-6 dBFS` のモノラルトーンバースト。`200 ms` の前無音、`100 ms` のトーンオン、`200 ms` の後無音。
- 解析内容: レイテンシ整列後、`20 ms` RMS エンベロープを計算します。アタックはピークエンベロープの `10%` から `90%` までの時間、リリースは `90%` から `10%` までの時間として測定します。A/B 間の残留レイテンシは `+/-50 ms` 範囲の相互相関で推定し、エンベロープカーブは `1 ms` ごとにサンプリングします。
- レポート内容: エンベロープカーブ、`residualLatencyMs`、A/B の attack/release time、`attackDeltaMs`、`releaseDeltaMs`、post-release residual level と差分、アタック/リリース推定や残留窓が不安定な場合の警告。

## レポート

レポートは、CSS、チャート、測定値、警告、JSON を埋め込んだ単一 HTML ファイルです。人間向けの要約と LLM 向けの構造化データを決定論的に生成します。レポート生成に外部 LLM やネットワークサービスは必要ありません。

## テスト

ビルド後、次を実行します。

```powershell
ctest --test-dir build -C Release
```

## リポジトリ構成

- `src/`: CLI、パイプライン、VST3 ホスト、各テストマネージャ、レポート生成。
- `include/vstcompare/`: プロジェクトの公開ヘッダ。
- `tests/`: CTest ベースのユニットテストと挙動テスト。
- `external/vst3sdk/`: Steinberg VST3 SDK 依存関係。
- `reports/`: サンプルまたは生成済みレポート。
- `vsts-for-dev-only/`: 開発専用のプラグイン/バイナリ。プロジェクトのライセンス許諾対象ではありません。

## ライセンス

VSTCompare のオリジナルコードとドキュメントは Creative Commons Zero v1.0 Universal (CC0 1.0) として公開します。詳しくは [LICENSE.md](LICENSE.md) を参照してください。

この CC0 の許諾は、プロジェクト独自のコードとドキュメントにのみ適用されます。`external/vst3sdk` 以下の VST3 SDK は VST3 SDK の MIT ライセンスのままです。詳細は `external/vst3sdk/LICENSE.txt` を参照してください。第三者 SDK ファイル、生成されたレポート、開発専用 VST プラグイン/バイナリは、このプロジェクトによって再ライセンスされません。
