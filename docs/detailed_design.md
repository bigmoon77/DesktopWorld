# DesktopWorld 詳細設計

- 文書バージョン: 1.1
- 対応要件: docs/requirements.md v1.1
- 状態: 実装着手前の整合設計
- 対象 API: Win32 / Direct3D 11

## 1. 全体構成

~~~text
DesktopWorld
├─ Application
│  ├─ desktop_app
│  ├─ settings_store
│  └─ frame_scheduler
├─ Platform
│  ├─ desktop_host
│  ├─ workerw_host
│  ├─ monitor_manager
│  └─ time_provider
├─ Environment
│  ├─ solar_model
│  ├─ atmosphere_model
│  └─ atmosphere_parameters
├─ Cache
│  ├─ atmosphere_lut_cache
│  └─ sky_sample_cache
├─ Render
│  ├─ render_system
│  ├─ render_context
│  ├─ render_target
│  ├─ render_pass
│  └─ atmosphere_sky_pass
├─ Diagnostics
│  ├─ gpu_profiler
│  ├─ quality_analyzer
│  └─ image_capture
└─ Shaders
   └─ Atmosphere
~~~

### 1.1 依存関係

- Application は Platform / Environment / Cache / Render の制御を行う。
- Platform は Win32 / DXGI の詳細を隠蔽する。
- Environment は GPU リソースを直接所有しない。物理パラメータと太陽状態を提供する。
- Cache は LUT の有効性、キー、サンプル時刻を管理する。
- RenderSystem は RenderPass を順番に実行する。
- AtmosphereSkyPass は Atmosphere LUT と Sample Pair を参照する。
- Diagnostics は RenderSystem / Cache の実行結果を観測し、描画ロジックを変更しない。

依存方向は Application → 各サブシステムとする。Platform から Environment や Render の具体クラスを参照しない。

## 2. 起動・終了シーケンス

### 2.1 起動

1. settings_store から設定を読み込む。存在しなければ既定値を作成する。
2. 設定値を範囲検証する。
3. monitor_manager が対象モニターを列挙する。
4. workerw_host が各モニターの WorkerW 描画親を取得する。
5. 安全な WorkerW を取得できない場合は起動失敗とする。Progman への無条件フォールバックは禁止する。
6. D3D11 device / immediate context を 1 組生成する。
7. モニターごとに子描画ウィンドウ、SwapChain、RenderTarget を生成する。
8. atmosphere_lut_cache がパラメータ hash に対応する Static LUT をロードする。存在しなければ生成する。
9. solar_model が現在 UTC と観測地点から太陽方向を計算する。
10. sky_sample_cache が必要な Sample Pair を生成する。
11. RenderSystem を開始する。

### 2.2 終了

1. 新規フレーム要求を停止する。
2. RenderSystem の GPU リソース参照を解放する。
3. 各 SwapChain / RenderTarget / 描画ウィンドウを解放する。
4. D3D11 context / device を解放する。
5. 設定を settings_store へ書き出す。

## 3. 時刻・太陽位置

### 3.1 時刻表現

内部は UTC epoch seconds を正規表現とする。Win32 から UTC を取得し、太陽位置計算へ渡す。固定時刻モードも UTC を保持する。

SYSTEMTIME は OS 境界でのみ使用し、サンプルキー・比較・補間計算は epoch seconds で行う。

### 3.2 太陽位置

入力:
- UTC 日時
- latitude: [-90, 90] degrees
- longitude: [-180, 180] degrees。東経を正とする。

出力:
- direction: ワールド座標系の単位ベクトル
- elevation_degrees
- azimuth_degrees

座標系:
- +Y: 天頂
- +X: 東
- +Z: 北
- 方位角 0°: 北、90°: 東、180°: 南、270°: 西

太陽方向:
- x = cos(elevation) * sin(azimuth)
- y = sin(elevation)
- z = cos(elevation) * cos(azimuth)

計算式の妥当性は、基準日時・地点の太陽高度/方位角を含む自動テストで検証する。極点・天頂付近では方位角が不定となるため、direction を主たる比較値とする。

## 4. サンプルスケジューリング

### 4.1 サンプル間隔

Auto モードの間隔:

| 太陽高度の絶対値 | 間隔 |
|---:|---:|
| > 12° | 3600 秒 |
| > 6° ～ <= 12° | 1800 秒 |
| <= 6° | 900 秒 |

Manual モードでは 3600 / 1800 / 900 秒のいずれかを使用する。

### 4.2 時刻境界

interval 秒に対する UTC サンプル開始時刻を数学的な floor で計算する。

~~~text
t0 = floor_div(now_utc_seconds, interval) * interval
t1 = t0 + interval
blend = clamp((now - t0) / interval, 0, 1)
~~~

C++ の整数除算は負数で 0 方向へ丸めるため、floor_div を別途実装する。過去日時のデバッグ入力でもサンプル区間が未来側へずれないこと。

### 4.3 キャッシュキー

Sky View Sample のキー:

- atmosphere_parameter_hash
- latitude / longitude
- sample UTC epoch
- sky LUT resolution
- shader / model version

キーが一致する Sample Pair は再利用する。最初の実装は 2 スロットのリングキャッシュとし、24 時間分を常時確保しない。

### 4.4 補間

C:
- Sky LUT A/B を Pixel Shader で読み込む。
- radiance A/B を lerp(A, B, blend) する。
- 補間後に tone mapping を行う。

D:
- Compute Shader が A/B を読み込み、HDR の sky_current へ補間結果を書き込む。
- Pixel Shader は sky_current をサンプルして tone mapping を行う。

A:
- 出力ピクセルごとに現在時刻の太陽方向を用いて3D scattering LUTからradianceを直接評価する。
- Sky View LUTと時間補間を使わないため、B/Dに対する空間LUT・時間サンプル近似の比較基準となる。
- 3D scattering LUTそのものはStatic LUTの離散化誤差を含む。

B:
- 現在時刻の太陽方向で Sky View LUT を生成して描画する。基準方式として毎回の再生成を許す。通常運用の既定値にはしない。

A/B/C/D は比較のための独立したモードとする。物理モデル変更時は全方式が同じ AtmosphereModel を参照する。

## 5. 大気散乱 LUT

### 5.1 LUT の依存性分離

Static LUT:
- Transmittance
- 単一散乱の事前計算成分
- Scattering Density / Multiple Scattering
- Ground Irradiance

これらは次の値に依存する。
- 惑星半径・大気上端高度
- Rayleigh / Mie scattering・extinction
- 密度プロファイル
- オゾン吸収プロファイル
- 地面アルベド
- 最大散乱次数
- LUT 解像度・フォーマット
- Shader / algorithm version

太陽時刻と longitude によって毎時再生成しない。キーはこれらのパラメータから作る。

Dynamic LUT:
- Sky View LUT
- 太陽方向、観測位置、Static LUT のキー、解像度に依存する。
- 時間サンプルの更新時に 2 枚を生成する。

### 5.2 Bruneton 系事前計算順序

1. Transmittance LUT を計算する。
2. Direct Irradiance と Single Scattering を計算する。
3. Scattering Density を各散乱次数について計算する。
4. Indirect Irradiance を計算して累積する。
5. Multiple Scattering を計算して累積する。
6. 必要な scattering order まで反復する。
7. Static LUT を保存する。

散乱次数ごとの中間結果は次の次数の入力になる。順序・入出力リソースを固定し、Compute Shader の各段階を個別に計測できるようにする。

初期値は最大 4 次とし、設定で 2～4 次を選択できる。現行実装はRGB近似であり、角度積分は4×8サンプル、レイ方向の積分は24ステップ（Transmittanceは48ステップ）に抑えた低負荷プロトタイプである。参照実装の高精度設定とは異なる。GPU時間と参照画像誤差を測定して、最終サンプル数を決定する。

### 5.3 Static LUT キャッシュ

- 保存先: %LOCALAPPDATA%/DesktopWorld/Cache/<16桁parameter-hash>.bin
- パラメータ・解像度・アルゴリズムバージョンを含む hash を使用する。
- 全ファイルが存在し、ヘッダ・サイズ・version が一致する場合のみロードする。
- 不完全・破損データは破棄し再生成する。
- キャッシュ生成中は同じディレクトリの一時ファイルへ出力し、完了後に置換する。
- 現行ファイルはヘッダにmagic / version / format / 各LUT寸法 / parameter hashを記録し、Transmittance、Direct Irradiance、Irradiance累積、Single Rayleigh、Single Mie、Scattering累積を格納する。

## 6. RenderSystem

### 6.1 RenderPass 契約

各 RenderPass は次を持つ。

- initialize(render_context&)
- execute(render_context&, const frame_context&)
- resize(render_context&, width, height)
- shutdown()

初期 Pass:
- AtmosphereSkyPass

将来追加:
- CloudPass
- CelestialPass
- WeatherPass
- TerrainPass

Pass は必要な入力テクスチャと出力先を宣言し、RenderSystem が実行順序を管理する。初期段階では明示的な直列実行とし、汎用 dependency graph は導入しない。

### 6.2 RenderContext

RenderContext は次を提供する。

- D3D11 device / context
- 現在のモニター向け RenderTarget / viewport
- LUT / SRV / UAV への参照
- frame constants
- profiling scope

Pass は WorkerW の HWND や settings_store を直接参照しない。

## 7. Platform / モニター

### 7.1 WorkerW

- WorkerW の発見処理は workerw_host に閉じ込める。
- Progman メッセージは探索トリガーとして扱う。
- アイコンを含む SHELLDLL_DefView の sibling となる WorkerW を取得する。
- 取得できなければエラーを返す。別ホストへの無条件フォールバックは禁止する。
- TaskbarCreated、ホスト HWND の無効化、定期検査を契機に再取得する。
- Explorer 再起動後、古い HWND を使い続けない。

### 7.2 マルチモニター

- EnumDisplayMonitors と GetMonitorInfo でモニターを列挙する。
- 各モニターに desktop child window / SwapChain / RenderTarget を 1 組持つ。
- D3D11 device と大気 LUT は原則共有する。
- モニター解像度・配置・DPI が変化したら対象 SwapChain を再構築する。
- 描画ホストの作成・破棄はモニター単位で独立させる。

## 8. 設定ファイル

%APPDATA%/DesktopWorld/settings.ini

初期項目:

~~~ini
[Location]
Latitude=35.6812
Longitude=139.7671

[Rendering]
Method=3
FrameMode=0
BaseFps=2
FixedFps=2
TwilightFps=15
SampleIntervalMode=0
Exposure=1.0

[Atmosphere]
PlanetRadiusKm=6360
AtmosphereHeightKm=100
RayleighR=0.005802
RayleighG=0.013558
RayleighB=0.033100
MieScatteringR=0.003996
MieScatteringG=0.003996
MieScatteringB=0.003996
RayleighScaleHeightKm=8
MieScaleHeightKm=1.2
MieAnisotropyG=0.8
SolarIntensity=20
; P1 additions: GroundAlbedoR/G/B, ozone density profile, MaxScatteringOrder
~~~

全値は読み込み時に検証する。欠落キーは既定値で補完し、範囲外値はクランプまたは既定値へ戻す。保存は一時ファイルへ書き出した後に置換する。

## 9. Diagnostics / 品質評価

### 9.1 GPU Timestamp

次の scope を記録する。
- Static LUT precompute total
- Transmittance
- Direct Irradiance
- Single Scattering
- Multiple Scattering
- Sky View sample generation
- Blend
- Desktop render
- Frame total

Query ring buffer で非同期取得する。Query が未完了の場合は前回値を保持し、GPU wait は行わない。

CPU 計測は次を分ける。
- CPU environment update
- CPU render command submission / Present

### 9.2 補間誤差

- B の現在時刻 Sky View LUT を比較基準とする。
- D の時間補間結果と同一解像度・同一座標で比較する。
- 比較は tone mapping 前の linear HDR radiance で行う。
- MAE / RMSE / Maximum Absolute Error を記録する。
- 特に太陽高度 -12°～+12° と、日付・緯度の異なる条件を検証する。

この測定で得られるのは同一モデル内の時間補間誤差である。大気モデル自体の物理精度は別途、CPU 参照実装または参照画像と比較する。

## 10. エラー処理

| 失敗 | 動作 |
|---|---|
| WorkerW を取得できない | 初期化失敗として通知 |
| D3D11 device 作成失敗 | 起動失敗として HRESULT を記録 |
| Shader compile / Create* 失敗 | Shader 名・HRESULT・Compiler message を記録 |
| Static LUT キャッシュ破損 | キャッシュ削除後に再計算 |
| GPU query が未完了 | 取得を次フレーム以降に延期 |
| 設定ファイル不正 | 不正キーを既定値で補完し警告 |
| モニター消失 | 対象ホストを破棄し残りのモニターを継続 |

## 11. テスト設計

### 11.1 Unit Test

- 太陽位置: 春分・夏至・冬至、昼夜、経度差、極付近。
- 日付: 閏年、年境界、負 epoch の floor division。
- サンプル区間: 60 / 30 / 15 分の境界。
- blend: 0、0.5、1、範囲外入力。
- Settings: 欠落、破損、範囲外、保存・再読み込み。

### 11.2 GPU Test

- 各 LUT の生成と Shader Resource / Unordered Access の切り替え。
- Render Method A/B/C/D の出力。
- Static LUT cache load / invalidation。
- Timestamp query の非同期回収。
- B vs D の MAE / RMSE。
- 現行Aは3D scattering LUTの直接参照、B/DはSky View LUTを経由するため、A vs Dの誤差も併記する。

### 11.3 Integration Test

- WorkerW が取得できない場合の安全な終了。
- Explorer 再起動後の再接続。
- モニター追加・削除・解像度変更・DPI 変更。
- デスクトップアイコン操作とマウス入力の透過。
- Debug UI を閉じた Release 相当の常駐動作。

## 12. 現行実装からの移行対応

| 現行コード | 移行先 |
|---|---|
| desktop_app::implementation 内の Win32 処理 | Platform/workerw_host・monitor_manager |
| desktop_app 内の時刻取得 | Platform/time_provider |
| desktop_app 内の緯度・経度 | settings_store |
| atmosphere_renderer | Render/atmosphere_sky_pass と Environment/atmosphere_model |
| sample_start_time / sample_interval | Cache/sky_sample_cache |
| _sky_a / _sky_b | sky_sample_cache が所有する 2 slot |
| blend_sky_views | AtmosphereSkyPass の D 方式 |
| gpu_profiler | Diagnostics 層。RenderContext から scope を取得 |
| _transmittance_texture | atmosphere_lut_cache の Static LUT セット |

段階移行し、各段階で Debug / Release と HLSL validation を通す。巨大な一括置換は行わない。
