# DesktopWorld 設計整合レビュー

- 対象: 現行 feature/initial-implementation
- 基準文書: docs/requirements.md v1.1 / docs/detailed_design.md v1.1
- 結果: **設計文書の矛盾を解消。P0実装をCIで検証。P1は実装プロトタイプまで。**
- 注意: Debug / Release のビルド成功は、要件充足・実機描画・物理精度の保証ではない。

## 1. リポジトリ確認

現行ブランチには README と実装コードが存在していたが、独立した要件定義書と詳細設計書は存在していなかった。今回、会話で決定した基本方針と現行コードから文書を再構成した。

## 2. 重大な不一致

| ID | 項目 | 現状 | 要件・設計上の判断 | 優先度 |
|---|---|---|---|---|
| R-01 | 要件・詳細設計 | 独立文書なし | 2 文書を追加し、実装とのトレーサビリティを持たせる | 完了 |
| R-02 | 観測地点 | Tokyo 座標をコードへ直書き | settings.ini から取得し Release でも設定可能にする | P0 |
| R-03 | 時刻 | 内部 UTC / Debug UI も UTC | 内部 UTC を正式仕様とする。ローカル時計の瞬間を UTC として取得する | 解消 |
| R-04 | 時間サンプル | 60 / 30 / 15 分の自動切替 | 通常 60 分、薄明で 30 / 15 分に短縮と明文化 | 解消 |
| R-05 | キャッシュ | Sky View 2 枚のみ保持 | 2-slot rolling cache と Static LUT cache を別物として定義 | 解消 |
| R-06 | 大気モデル | RGB近似の複数散乱を追加。参照精度は未検証 | Bruneton系の計算順序・Static LUT分離を実装。高精度な参照検証は残る | P1 |
| R-07 | 描画拡張 | desktop_app と atmosphere_renderer に処理が集中 | RenderSystem / RenderPass を追加し責務分離 | P0 |
| R-08 | WorkerW | 取得失敗時に Progman へ fallback | アイコン背面を保証できない可能性があるため、安全に失敗する仕様へ変更 | P0 |
| R-09 | Explorer 再起動 | TaskbarCreated と3秒のHWND検査、WorkerW再構築を追加。実機検証は未実施 | shell再起動・WorkerW再生成を実機検証 | P2 |
| R-10 | モニター | monitor_managerで列挙し、仮想画面全体を1つのSwapChainで覆う。WorkerW client areaが全範囲を包含しない場合は安全に失敗 | 複数GPU/独立モニターSwapChain・実機構成の検証 | P2 |
| R-11 | 品質検証 | 太陽位置・サンプル・設定・Pass順の単体テストとHLSL compileを実施。B vs D誤差は未実装 | GPU画像比較とB/D誤差を追加 | P1 |
| R-12 | GPU 計測 | Timestamp QueryでStatic LUT全体と各大気計算段階を計測。CPU update / submitも分離 | 計測値を実GPU実行で確認 | P1 |
| R-13 | 過去日時 | epoch / interval の整数除算が負数で 0 方向丸め | floor_div を採用する | P0 |

## 3. 現行実装の方式と意味を明確化

### 3.1 A / B / C / D

- A は各出力ピクセルで3D scattering LUTからradianceを直接評価する。
- B は現在時刻の太陽方向から Sky View LUT を生成する。
- C はサンプル時刻 A/B の Sky View を Pixel Shader で補間する。
- D はサンプル時刻 A/B の Sky View を Compute Shader で補間し、その結果を描画する。

A/B/C/D は同じRGB大気モデルとStatic LUTを共有する。現在の大気モデルはRayleigh / Mieの単一散乱に加えて、複数散乱の次数反復、三角形オゾン密度、拡散地面反射を含む低解像度プロトタイプである。Aは3D scattering LUTを直接参照し、Bは現在時刻Sky View LUTを生成、C/Dは時間サンプルを補間する。AとDの差はStatic LUT・Sky View LUT・時間補間の近似誤差を含み、現実の大気に対する物理精度の差を示すものではない。

B は比較基準として毎回現在時刻 LUT を生成するため、通常運用の既定方式にはしない。D を既定方式とする。

### 3.2 Static LUT と Sky View LUT

現行コードには Transmittance / Direct Irradiance / Single Rayleigh / Single Mie / Scattering Density / Multiple Scattering / Indirect Irradiance / Sky View LUTがある。Bruneton系の段階的な計算順序を参考にしたRGB近似であり、参照実装のスペクトル表現・積分解像度・全精度をそのまま再現したものではない。

Static LUT は大気パラメータに依存する。太陽時刻が変わるだけで再生成してはいけない。

Sky View LUT は太陽方向に依存する。現在時刻を挟む 2 枚を保持する方式は、24 時間分を事前に保持する方式とは異なる。これを明示した。

## 4. 実装で確認された具体的な修正項目

### P0: CI検証済み

1. settings_storeを追加し、設定保存・再読み込みと範囲検証を実装した。
2. EnvironmentController / TimeProvider / WorkerWHostを分離した。
3. 直列RenderSystem / RenderPass / AtmosphereSkyPassを追加した。
4. WorkerW取得失敗時にProgmanへ描画するfallbackを削除した。
5. 負数UTCに対応するfloor_divとサンプルスケジュールのUnit Testを追加した。
6. 太陽位置、サンプルキャッシュ、設定往復、FrameScheduler、RenderPass順をUnit Test化した。
7. CPU update / render submit とGPU Timestampの計測を追加した。

### P1: 大気散乱の品質確立

1. Transmittance / Direct Irradiance / Single Scatteringを生成し、order 2..NについてScattering Density / Indirect Irradiance / Multiple Scatteringを反復するGPUパイプラインを追加した。
2. Mie extinction、三角形オゾン密度、ground albedoを設定可能にした。
3. parameter hash・version・format・dimensionsを含むStatic LUTディスクキャッシュを追加した。
4. Precompute total / Transmittance / Direct Irradiance / Single Scattering / Multiple ScatteringのGPU Timestampを分離した。
5. 参照画像との物理誤差、B/Dのlinear HDR誤差、低角度積分の妥当性は未検証であり、今後の品質確立タスクとする。

### P2: デスクトップホストの実機検証・拡張

1. Explorer 再起動時の WorkerW 再取得処理は追加済み。Explorer 再起動・テーマ変更を含む実機検証が残る。
2. モニター列挙と仮想画面全体を覆う1つのSwapChainを追加済み。異なるGPU・混在DPI・モニター配置変更・WorkerWの実クライアント範囲は実機検証が残る。
3. 各モニター独立SwapChain・独立環境・per-monitor LUTは後続拡張。
4. ウィンドウ入力透過・アイコン背面描画を実機で確認する。

## 5. レビュー結果

### 要件と詳細設計

- 時刻は内部 UTC と明確化した。
- 1 時間基準と薄明での短縮間隔を両立させた。
- 2-slot Sky Sample cache と Static LUT cache を分離した。
- Bruneton 系事前計算の固定 LUT と時刻依存 Sky View LUT を分離した。
- A～D の役割と比較できる誤差の意味を明確化した。
- RenderPass を導入するが、過剰な汎用レンダーグラフは導入しない。
- Platform / Environment / Cache / Render / Diagnostics の依存関係を定義した。

**設計文書間の矛盾は解消した。**

### 実装との整合性

P0の設計項目は実装され、Windows CIでDebug/ReleaseのビルドとコアUnit Test、15本のHLSLエントリーを検証した。P1のRGB複数散乱パイプラインとStatic LUT cacheも実装された。

ただし、GPU実機での画像評価、Bruneton参照画像との誤差計測、B/D時間補間誤差、実際の起動時間・GPUコストは未検証である。現段階のP1は「動く低解像度プロトタイプ」であり、物理精度を確立した完成版ではない。

## 6. 参照資料

- Eric Bruneton, Precomputed Atmospheric Scattering: a New Implementation (2017): https://ebruneton.github.io/precomputed_atmospheric_scattering/
- Reference model precomputation order and texture caching: https://ebruneton.github.io/precomputed_atmospheric_scattering/atmosphere/reference/model.cc.html
- Scattering and transmittance equations / lookup functions: https://ebruneton.github.io/precomputed_atmospheric_scattering/atmosphere/functions.glsl.html
