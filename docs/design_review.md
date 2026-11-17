# DesktopWorld 設計整合レビュー

- 対象ブランチ: `feature/initial-implementation`
- 基準文書: `docs/requirements.md` v1.5 / `docs/detailed_design.md` v1.9
- レビュー範囲: 要件定義・詳細設計・現行実装の構成整合性、HDR品質計測、Bruneton CPU比較CLIの仕様とコードの照合
- 結果: **単一仮想デスクトップ描画方式の矛盾を解消。旧 WorkerW と raised desktop のホスト構成、layered child 用の Windows 10 対応マニフェスト、埋め込み検査を実装。B vs D / A vs D のHDR誤差計測、PFM＋メタデータ、DXGIプロセスGPUメモリ監視、Bruneton CPU比較CLIを実装。**
- 未完了: 実際のGPU captureを用いたBruneton比較値の収集、実機での複数モニター / Explorer 復旧検証。
- 注意: CI のビルド・テスト成功は、実機表示・物理精度・全品質要件の充足を保証しない。

## 1. レビュー対象と前提

今回のレビューでは、要件定義・詳細設計・設計レビュー文書を読み直し、現行実装のデスクトップホスト生成・SwapChain生成・モニター変更通知・ホスト再構築処理と照合した。

初期実装の方式は次のとおりである。

- モニターを列挙し、各モニターの物理座標の union から仮想デスクトップ矩形を求める。
- OS のウィンドウ階層に対応したデスクトップ描画親を1つ取得する。旧構成ではトップレベル WorkerW、layered Progman 構成では Progman を描画親として使用する。
- 仮想デスクトップ全体を覆う子描画ウィンドウを1つ生成する。
- D3D11 device / immediate context、SwapChain、RenderTarget はそれぞれ単一構成とする。
- すべてのモニターに同じ空の環境を連続した1つの描画面として表示する。
- モニター構成変更・デスクトップホストの消失やレイアウト変化時は描画面全体を再構築する。全体を構築できない間は描画を停止し、部分モニターだけの表示を継続しない。

モニターごとの独立 SwapChain / 独立環境は初期方式に含めず、将来拡張として扱う。

## 2. 重大な不整合と修正内容

| ID | 項目 | 修正前の問題 | 統一した仕様 | 状態 |
|---|---|---|---|---|
| R-01 | SwapChain構成 | 詳細設計 2.1 はモニターごと、FR-01 / 詳細設計 7.2 は仮想デスクトップ全体に1つ | 単一の仮想デスクトップ子ウィンドウ / SwapChain / RenderTarget | 修正済み |
| R-02 | 起動時デスクトップホスト取得 | raised desktop で WorkerW の生成要求が旧方式の引数のみだった | Progman の WS_EX_NOREDIRECTIONBITMAP を検出し、0x052C(0x0D, 1) を要求して WorkerW を再探索 | 実装済み・実機検証待ち |
| R-03 | RenderContext | 「現在のモニター向けRenderTarget」と単一描画面の説明が不一致 | 仮想デスクトップ全体の単一 RenderTarget / viewport | 修正済み |
| R-04 | モニター変更時の復旧 | 「対象ホストを破棄し残りのモニターを継続」は単一SwapChain方式と不整合 | 全体を再列挙・再構築し、成功まで描画面全体を停止 | 修正済み |
| R-05 | 終了処理 | 複数SwapChainを前提とする表現 | 単一SwapChain / RenderTarget / 描画ウィンドウの解放 | 修正済み |

### 2.1 採用した理由

単一 SwapChain 方式を採用する理由は、初期要件が「全モニターに同じ空を連続した描画面として表示する」ことだからである。

この要件に対して、仮想デスクトップ矩形を1つの描画面に対応させることで、モニター間の描画座標・時刻サンプル・環境パラメータを一元化できる。モニターごとの独立 SwapChain は、独立環境や per-monitor 設定を導入する段階で改めて設計する。

### 2.2 モニター変更時の安全性

単一 SwapChain 方式では、モニター構成が変化した際に一部だけを残して継続すると、仮想デスクトップ全体を覆うという要件を破る。

したがって、次の順序を正式仕様とする。

1. Display / DPI / Shell の通知または HWND 検査で変更を検出する。
2. 旧描画面の RenderTarget / SwapChain / 子ウィンドウを破棄する。
3. モニターを再列挙して仮想デスクトップ矩形を再計算する。
4. 現在の Explorer 構成に対応するデスクトップホストの親 HWND と Z オーダー情報を再取得する。
5. 単一子ウィンドウ / SwapChain / RenderTarget を再生成する。
6. すべて成功した場合だけ描画を再開する。
7. 失敗した場合は部分表示せず、描画停止状態で再試行する。

## 3. 要件定義と詳細設計の照合

| 領域 | 要件 | 詳細設計 | 判定 |
|---|---|---|---|
| デスクトップホスト | アイコン背面、入力透過、安全な失敗 | 旧 WorkerW / layered Progman の構成別ホスト、単一仮想画面 | 整合 |
| 複数モニター | 仮想デスクトップ全体を連続表示 | bounds union と単一 SwapChain | 整合 |
| モニター構成変更 | 全体を覆う状態を維持 | 全体再構築、失敗時は停止・再試行 | 整合 |
| 時刻 | UTC を内部標準とする | UTC epoch seconds | 整合 |
| 太陽位置 | 日時・緯度・経度から算出 | 座標系・式・境界テストを定義 | 整合 |
| サンプル | 60 / 30 / 15 分、薄明で短縮 | 同一の高度境界と floor division | 整合 |
| LUT | Static と時刻依存 Sky View を分離 | parameter hash と Sample Pair を分離 | 整合 |
| 描画方式 | A / B / C / D を比較 | 各方式の入出力・補間位置を定義 | 整合 |
| 大気モデル | Bruneton 系を最終基準とする | RGB・低解像度プロトタイプと最終精度検証を区別 | 整合。ただし品質未達 |
| Render構造 | Render Pass 単位で拡張 | 直列 RenderSystem / AtmosphereSkyPass | 整合 |
| 設定 | Releaseでも設定を保持 | settings.ini と範囲検証 | 整合 |
| Diagnostics | GPU / CPU 計測と品質評価 | Timestamp / HDR画像比較 / DXGIメモリ情報 | 設計と Debug 計測機能が整合。実機での計測値収集は継続 |
| テスト | 数値・設定・Pass・HLSLを検証 | Unit / GPU / Integration Test | 整合。ただし未実装テストあり |

## 4. 実装との照合

### 4.1 確認できた実装

- `desktop_app` は単一の desktop child window と desktop SwapChain / RenderTarget を保持する。
- `monitor_manager` の仮想デスクトップ bounds を基準に描画面の寸法・位置を決定する。
- 選択したデスクトップホストの client area が仮想デスクトップ全体を包含しない場合、描画面生成を失敗させる。
- `WM_DISPLAYCHANGE` / `WM_DPICHANGED`、TaskbarCreated、定期 HWND 検査を再構築要求につなげる。
- ホスト再構築では旧描画面を破棄し、新しい描画面の生成に成功した場合だけ描画を再開する。
- 旧 WorkerW / layered Progman の取得・再構築に失敗した場合は描画面を無効化し、定期検査で再試行する。
- D3D11 device / immediate context は単一構成である。
- 実行ファイルの埋め込みマニフェストに Windows 10 supportedOS 宣言を含め、layered child window の互換性要件を満たす。

### 4.2 HDR品質計測の実装確認

- B vs D は現在時刻の直接生成 Sky View と、Sample Pair の Compute 補間結果を 256 x 128 の同一テクセル座標で比較する。
- A vs D は Linear HDR の RGBA16F RenderTarget を使い、直接評価と Sky View LUT の画面サンプル結果を同じ解析解像度で比較する。
- 比較値は tone mapping / exposure 適用前で、MAE / RMSE / Maximum Absolute Error を RGB スカラー成分単位で算出する。
- PFM キャプチャは Linear HDR Sky View LUT を RGB 32-bit float で保存する。
- これらの GPU readback は同期的な Debug 操作であり、常時実行しない。

この計測は同一の近似大気モデル内で描画経路の誤差を見るもの。Bruneton 参照実装に対する物理精度を検証した結果とは扱わない。

### 4.3 Bruneton CPU reference comparator

- PFM capture は大気パラメータ、太陽方向、Render Method、View Mapping を同名 `.meta` に保存する。
- Optional CMake target `bruneton_reference_compare` は公式CPU reference modelと、`dimensional_types` / `progress_bar` の必要部分だけを pinned commit で取得してビルドする。
- Render Method A/B の capture を受け付け、C/D の時間補間を含む capture は拒否する。
- RGB係数を 440 / 550 / 680 nm に一致させ、km⁻¹→m⁻¹ の変換と同一の density profile / atmosphere geometry を使用する。
- 参照画像と combined / per-channel MAE・RMSE・max error を出力する。
- `--self-test` で PFM round-trip / 画像上下反転 / RGB spectral anchor / MAE・RMSE・最大誤差を検証する。47波長CPU referenceの初回初期化・実GPU capture比較はCIで実行していない。scalar solar intensityを47波長で一定とする近似を含むため、設計上の比較と実太陽スペクトルの完全な検証は区別する。

### 4.4 GPUメモリ監視の実装確認

- Debug UI は `IDXGIAdapter3::QueryVideoMemoryInfo` から Local / Non-local segment のプロセス使用量と OS Budget を取得する。
- 約 1 秒間隔で取得し、Debug UI 以外の描画経路には追加の同期を導入しない。
- 表示値は DesktopWorld 単体のリソース総量ではなく、対象プロセス全体の DXGI 使用量である。
- ドライバー / segment が未対応の場合は unavailable と表示する。

### 4.5 CIで確認した範囲

確認済みの Windows CI run では以下が成功している。

- Debug Configure / Build / Test
- Release Build / Test
- Optional `DESKTOPWORLD_BUILD_BRUNETON_COMPARATOR=ON` configure, `bruneton_reference_compare` target build, and `--self-test` execution
- HLSL 17 entry-point compile validation
- 太陽位置・サンプル区間・環境サンプルキャッシュ・設定往復・Frame Controller・RenderPass順序のコアテスト

これらはロジックとビルドの検証であり、実際の Explorer の旧 WorkerW / layered Progman 構成や複数モニター環境を用いた実機統合テストの代替にはならない。

## 5. 残存課題と設計上の注意

| ID | 課題 | 現状 | 次の作業 |
|---|---|---|---|
| O-01 | Bruneton参照精度 | 公式CPU referenceとの画像生成・誤差レポートCLIを実装。GPU captureを使った代表条件の実測は未実施 | 薄明・日付・緯度でcaptureし、参照比較の結果を収集。solar spectrumの近似条件を明記 |
| O-02 | B vs D補間誤差 | Debug UI からワンショットで Linear HDR の MAE / RMSE / 最大誤差を計測できる | 薄明・緯度・日付の異なる条件で実測結果を収集 |
| O-03 | A vs D描画経路・空間近似誤差 | 同一 Linear HDR 条件、最大 1920 x 1080 の解析グリッドで計測できる | 複数の出力解像度・太陽高度で実測結果を収集 |
| O-04 | GPUメモリ | DXGI 1.4 によるプロセス単位 Local / Non-local 使用量・Budget を Debug UI に表示 | 複数GPU / iGPU / dGPU環境で取得可否と挙動を確認 |
| O-05 | 実機ホスト検証 | 旧 WorkerW / raised desktop のホスト検出、0x052C(0x0D, 1)、layered child 用マニフェストを実装。実機表示検証は未完了 | Windows 10 / Windows 11 24H2+、raised desktop の描画子ウィンドウ生成、Explorer再起動、混在DPI、負座標、モニター抜き差しを検証 |
| O-06 | OnDemandの時刻更新 | 明示要求時だけ描画し、待機中は時刻を自動追従しない | FR-06 の strict on-demand として定義済み。時間追従が必要な場合は Adaptive / Fixed を使う |

### 5.1 OnDemandの時刻更新

OnDemand は明示的な描画要求がない限り待機し、システム時刻や環境サンプル境界だけを理由に自動描画しない。これは FR-06 の「明示的な描画要求がある場合だけデスクトップを描画する」という定義と一致する。時間経過に合わせた背景の自動変化が必要な場合は Adaptive または Fixed を選択する。

## 6. 最終判定

### 設計文書

**単一仮想デスクトップ描画方式と、旧 WorkerW / layered Progman のホスト構成について、要件定義と詳細設計の整合を取った。実機表示検証は継続課題とする。**

- 起動・終了シーケンス
- RenderContext
- 旧 WorkerW / layered Progman のデスクトップホスト取得
- モニター列挙
- モニター構成変更時の復旧
- エラー処理
- Integration Test

これらを同じ単一描画面のライフサイクルに統一した。

### 実装

単一 SwapChain の構成とホスト再構築の基本処理は現行コードと一致する。ただし、モニター構成変更・Explorer再起動・混在DPIを含む実機検証は未完了である。

### 次工程へ進む条件

1. 単一 SwapChain の実機統合テストを行う。
2. Debug captureからBruneton参照画像・誤差レポートを生成し、複数の太陽高度・日付・緯度で測定値を収集する。

以上より、**設計文書の構成矛盾修正は合格。プロジェクト全体の品質要件は未達項目が残るため、残作業を継続する。**
