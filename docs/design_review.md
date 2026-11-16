# DesktopWorld 設計整合レビュー

- 対象: 現行 feature/initial-implementation
- 基準文書: docs/requirements.md v1.0 / docs/detailed_design.md v1.0
- 結果: **設計上の矛盾を解消。実装は P0 / P1 の未完了項目あり**
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
| R-06 | 大気モデル | Rayleigh / Mie 単一散乱 | 最終要件は多重散乱・地面反射・オゾン吸収を含む Bruneton 系事前計算 | P1 |
| R-07 | 描画拡張 | desktop_app と atmosphere_renderer に処理が集中 | RenderSystem / RenderPass を追加し責務分離 | P0 |
| R-08 | WorkerW | 取得失敗時に Progman へ fallback | アイコン背面を保証できない可能性があるため、安全に失敗する仕様へ変更 | P0 |
| R-09 | Explorer 再起動 | 再取得処理なし。再検査 timer も未使用 | TaskbarCreated と HWND 検査で再構築 | P2 |
| R-10 | モニター | プライマリのみ | monitor_manager とモニターごとの SwapChain を追加 | P2 |
| R-11 | 品質検証 | HLSL compile のみ | 太陽位置・サンプル計算・B vs D 補間誤差の自動検証を追加 | P0 |
| R-12 | GPU 計測 | Timestamp Query 実装済み | GPU frame total に加え CPU update / submit を分離。Query は非同期回収 | P0 |
| R-13 | 過去日時 | epoch / interval の整数除算が負数で 0 方向丸め | floor_div を採用する | P0 |

## 3. 現行実装の方式と意味を明確化

### 3.1 A / B / C / D

- A は各出力ピクセルで単一散乱を直接積分する。
- B は現在時刻の太陽方向から Sky View LUT を生成する。
- C はサンプル時刻 A/B の Sky View を Pixel Shader で補間する。
- D はサンプル時刻 A/B の Sky View を Compute Shader で補間し、その結果を描画する。

A/B/C/D は現状同じ単一散乱物理モデルを使う。したがって A と D の差は、現在のモデルにおける LUT 解像度・時間補間の差であり、現実の大気に対する物理精度の差ではない。

B は比較基準として毎回現在時刻 LUT を生成するため、通常運用の既定方式にはしない。D を既定方式とする。

### 3.2 Static LUT と Sky View LUT

現行コードには Transmittance LUT と Sky View LUT があるが、Bruneton 系の複数散乱事前計算は存在しない。

Static LUT は大気パラメータに依存する。太陽時刻が変わるだけで再生成してはいけない。

Sky View LUT は太陽方向に依存する。現在時刻を挟む 2 枚を保持する方式は、24 時間分を事前に保持する方式とは異なる。これを明示した。

## 4. 実装で確認された具体的な修正項目

### P0: 次に必ず修正

1. settings_store を追加し、緯度・経度・描画方式・FPS・間隔・大気値・露出を %APPDATA%/DesktopWorld/settings.ini から読み書きする。
2. desktop_app.cpp の巨大な実装構造体から Win32 ホスト・時刻・設定・環境更新の責務を切り出す。
3. render_system / render_pass / atmosphere_sky_pass を追加する。初期段階では直列 Pass 実行とし、汎用レンダーグラフは作らない。
4. WorkerW 取得失敗時の Progman fallback を削除し、誤った場所への描画を避ける。
5. サンプル時刻計算に負数対応 floor_div を導入する。
6. 太陽位置とサンプルスケジュールの Unit Test を追加する。
7. CPU update と CPU render submit の計測値を実際に収集する。未使用の cpu_update_ms を放置しない。

### P1: 大気散乱の完成

1. Transmittance / Single Scattering / Scattering Density / Indirect Irradiance / Multiple Scattering の順で事前計算する。
2. Ground Albedo と Ozone Absorption をモデルに加える。
3. 大気パラメータと LUT 形式の hash によるディスクキャッシュを追加する。
4. B vs D の linear HDR 誤差を計測する。
5. 4 次までの散乱と GPU 時間・画質の関係を計測する。

### P2: デスクトップホスト完成

1. Explorer 再起動時の WorkerW 再取得。
2. モニター列挙、個別 SwapChain、解像度・DPI 変更処理。
3. ウィンドウ入力透過・アイコン背面描画を実機で確認する。

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

現行実装は初期プロトタイプとしてビルドするが、P0 の設定保存・RenderPass 分離・WorkerW fallback 修正・自動テストが未完了である。

したがって、今回のレビューは「残りの実装へ進むための設計整合レビュー合格」を意味し、製品機能の完成判定ではない。

## 6. 参照資料

- Eric Bruneton, Precomputed Atmospheric Scattering: a New Implementation (2017): https://ebruneton.github.io/precomputed_atmospheric_scattering/
- Reference model precomputation order and texture caching: https://ebruneton.github.io/precomputed_atmospheric_scattering/atmosphere/reference/model.cc.html
- Scattering and transmittance equations / lookup functions: https://ebruneton.github.io/precomputed_atmospheric_scattering/atmosphere/functions.glsl.html
