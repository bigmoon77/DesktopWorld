# DesktopWorld 詳細設計

- 文書バージョン: 1.8
- 対応要件: docs/requirements.md v1.4
- 状態: 単一仮想デスクトップ描画方式、旧 WorkerW / raised desktop ホスト構成、HDR品質計測、Bruneton CPU比較仕様の反映版
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
4. workerw_host が Explorer のデスクトップホスト構成を判別する。旧構成ではアイコンを含む SHELLDLL_DefView の後ろにあるトップレベル WorkerW を描画親として取得する。layered Progman 構成（Windows 11 24H2+）では Progman を描画親として取得し、独自の layered child window を SHELLDLL_DefView と壁紙 WorkerW の間へ配置する。
5. monitor_manager が算出した仮想デスクトップ矩形を選択した描画親のクライアント座標へ変換し、矩形全体が包含されることを確認する。安全なホストと全体を包含する描画領域を確保できない場合は起動失敗とする。旧構成・layered Progman 構成以外への無条件フォールバックや一部モニターだけへの縮退は禁止する。
6. D3D11 device / immediate context を 1 組生成する。
7. 選択した描画親の子として仮想デスクトップ矩形全体を覆う単一の描画ウィンドウ、SwapChain、RenderTarget を生成する。layered Progman 構成では、描画ウィンドウを SHELLDLL_DefView の直下、壁紙 WorkerW の直上へ Z オーダー設定する。SwapChain の寸法は仮想デスクトップ矩形の幅・高さと一致させ、負のデスクトップ座標は親ウィンドウ座標へ変換して配置する。
8. atmosphere_lut_cache がパラメータ hash に対応する Static LUT をロードする。存在しなければ生成する。
9. solar_model が現在 UTC と観測地点から太陽方向を計算する。
10. sky_sample_cache が必要な Sample Pair を生成する。
11. RenderSystem を開始する。RenderContext に渡す RenderTarget / viewport は単一の仮想デスクトップ描画面に対応させる。

### 2.2 終了

1. 新規フレーム要求を停止する。
2. RenderSystem の GPU リソース参照を解放する。
3. 単一の SwapChain / RenderTarget / 仮想デスクトップ描画ウィンドウを解放する。
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
- 仮想デスクトップ全体を覆う単一 RenderTarget / viewport
- LUT / SRV / UAV への参照
- frame constants
- profiling scope

Pass は WorkerW の HWND や settings_store を直接参照しない。

## 7. Platform / モニター

### 7.1 デスクトップホスト（WorkerW / layered Progman）

- Explorer のウィンドウ階層の検出とホスト HWND の取得は workerw_host に閉じ込める。
- 旧構成では、SHELLDLL_DefView を持つトップレベルウィンドウの後ろにあるトップレベル WorkerW を描画親とする。
- layered Progman 構成（Windows 11 24H2+）では、Progman を描画親とし、その子である SHELLDLL_DefView と壁紙 WorkerW を検出する。DesktopWorld の layered child window は両者の間に Z オーダー設定する。壁紙 WorkerW 自体を DesktopWorld の親にはしない。
- Progman の WS_EX_NOREDIRECTIONBITMAP を確認して raised desktop を識別する。
- raised desktop で壁紙 WorkerW が未生成の場合は、Progman へ 0x052C(wParam=0x0D, lParam=0)、続けて 0x052C(wParam=0x0D, lParam=1) を SMTO_ABORTIFHUNG 付きで送り、各メッセージ後に Progman 直下の WorkerW を再探索する。従来構成では 0x052C(0, 0) の生成要求を使用する。
- SendMessageTimeoutW の戻り値と Win32 エラー、Progman の拡張スタイルを startup.log に記録する。
- どちらの既知構成も取得できなければエラーを返す。親子関係や Z オーダーが不明なウィンドウへの無条件フォールバックは禁止する。
- 選択した描画親の client area に仮想デスクトップ矩形全体が包含されることを確認する。
- TaskbarCreated、ホスト HWND の無効化、定期検査を契機にレイアウト全体を再取得する。
- Explorer 再起動後、古い HWND を使い続けない。

### 7.2 マルチモニター

- monitor_manager は EnumDisplayMonitors / GetMonitorInfo を用いてモニターの物理座標と device name を取得する。
- 各モニターの bounds union を仮想デスクトップ範囲とする。負の左上座標を許容する。
- 選択した描画親（旧構成の WorkerW または layered Progman）の client area に仮想デスクトップ矩形を MapWindowPoints で変換し、矩形全体が client area 内に収まることを確認する。
- 初期実装は仮想デスクトップ全体を覆う1つの child window / SwapChain / RenderTarget を使う。全モニターに同じ空を表示する。
- モニター追加・削除・配置・解像度変更時は monitor_manager を再列挙し、仮想デスクトップ矩形を再計算する。その後、既存の描画ウィンドウ・SwapChain・RenderTarget を一旦破棄し、新しい矩形に対して単一描画面を一括再構築する。
- 再構築中は旧 SwapChain の表示を継続しない。新しい仮想デスクトップ全体を包含するデスクトップホスト構成と描画面の生成に成功した場合のみ描画を再開する。
- 選択したデスクトップホストが仮想デスクトップ全体を包含しない、または再構築に失敗した場合は描画面全体を停止する。残存モニターだけを対象とした部分表示へ縮退せず、定期検査または Shell / Display 通知を契機に全体の再構築を再試行する。
- モニターごとに異なる環境を設定する機能、複数の独立した描画親（WorkerW / Progman）、複数 SwapChain は後続設計で追加する。

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
MieExtinctionR=0.004440
MieExtinctionG=0.004440
MieExtinctionB=0.004440
OzoneR=0.000650
OzoneG=0.001881
OzoneB=0.000085
OzoneCenterHeightKm=25
OzoneHalfWidthKm=15
GroundAlbedoR=0.1
GroundAlbedoG=0.1
GroundAlbedoB=0.1
MaxScatteringOrder=4
RayleighScaleHeightKm=8
MieScaleHeightKm=1.2
MieAnisotropyG=0.8
SolarIntensity=20
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

### 9.2 HDR品質計測・画像キャプチャ

計測は Debug UI からの明示要求でのみ実行する。常駐時の描画負荷に影響させないため、通常フレームでは readback を行わない。

#### B vs D: 時間補間誤差

- B 基準画像は、現在時刻の太陽方向を用いて Sky View LUT を再生成する。
- D 比較画像は、Sample A / B を Compute Shader で補間した Sky View LUT とする。
- 比較領域は 256 x 128。両画像の各テクセルを同じ座標で比較する。
- 比較値は tone mapping / exposure 適用前の RGBA16F Linear HDR radiance。
- GPU から staging texture へコピーし、CPU 側で half float を float32 に変換する。

#### A vs D: 描画経路・空間近似誤差

- A は現在時刻の太陽方向を用いる Fullscreen Pixel Shader の直接評価。
- D は補間済み Sky View LUT を Fullscreen Pixel Shader でサンプルする。
- 両方式を同じ Linear HDR RenderTarget に描画し、同じ正規化画面座標で比較する。
- 解析解像度は元の仮想デスクトップのアスペクト比を維持し、幅 1920 / 高さ 1080 の範囲に収める。
- A / D の双方に同一の太陽状態、Static LUT、露出未適用条件を使用する。

#### 共通指標

MAE / RMSE / Maximum Absolute Error は RGB の各スカラー成分を対象として算出する。pixel_count は RGB 3成分をまとめたピクセル数を表す。B vs D は LUT 解像度、A vs D は上記解析解像度を結果に記録する。

#### PFM キャプチャ

- Debug UI の操作で現在の Sky View radiance を保存する。
- 保存形式は PFM (`PF`)、RGB 32-bit float、little-endian。
- 画像サイズは 256 x 128。行順は PFM 仕様に合わせて下から上へ書き出す。
- tone mapping / exposure を適用しない。結果は `%LOCALAPPDATA%/DesktopWorld/Captures/sky_<timestamp>.pfm` に保存する。
- 同名の `.meta` サイドカーに View Mapping、Render Method、Sun Direction、Atmosphere parameters を保存する。サイドカー書き込みに失敗した場合は PFM も削除し、片方だけの不完全な capture を残さない。
- Direct / B は現在時刻の Sky View、C / D は Sample Pair の補間値を保存する。

readback は同期的であり、計測・キャプチャ中は一時的に CPU / GPU が待機する。この処理は明示的な Debug 操作に限定する。

この測定で得られるのは同一モデル内の描画経路・時間補間誤差である。大気モデル自体の物理精度は別途 Bruneton 参照実装との同条件比較で評価する。

### 9.3 GPUメモリ監視

Debug UI では DXGI 1.4 の `IDXGIAdapter3::QueryVideoMemoryInfo` を使用し、選択中の D3D11 アダプターについてプロセス単位のメモリ情報を約 1 秒間隔で取得する。

- Local segment: 現在のプロセス使用量と OS のメモリ予算。
- Non-local segment: 現在のプロセス使用量と OS のメモリ予算。
- 表示単位は MiB (1024 x 1024 bytes)。
- API / ドライバーが取得に対応しない segment は unavailable と表示する。
- 情報取得は Debug UI のみで行い、描画処理やリソース生成を待たせない。

この値はプロセス内の全 D3D / DXGI リソースを含むプロセス全体の使用量であり、DesktopWorld の大気 LUT だけのサイズではない。OS の Budget は動的に変化する上限目安であり、物理 VRAM 容量と同一視しない。

### 9.4 Bruneton CPU reference comparison

物理モデルの比較はデスクトップ常駐アプリから分離した CLI で実行する。既定ビルドでは参照実装を取得・ビルドせず、明示的な CMake option で有効化する。

```powershell
cmake -S . -B out/reference -A x64 -DDESKTOPWORLD_BUILD_BRUNETON_COMPARATOR=ON
cmake --build out/reference --config Release --target bruneton_reference_compare
.\out\reference\Release\bruneton_reference_compare.exe --self-test
.\out\reference\Release\bruneton_reference_compare.exe "C:\path\sky_capture.pfm"
```

#### 入力と比較条件

- 入力は DesktopWorld が生成した 256 x 128 PFM と同名 `.meta` サイドカー。
- Render Method A (Direct) または B (Sky Current) で取得した capture のみを受け付ける。C / D は時間補間誤差を含むため、物理モデル比較から除外する。
- View direction は Sky View Shader と同じ `azimuth=(u-0.5)*2π`, `elevation=(1-v)*π/2`。各ピクセル中心の座標を用いる。
- カメラ位置は `(0, planet_radius + 0.01 km, 0)`。太陽方向、大気半径、密度プロファイル、散乱係数、地面反射率、散乱次数を sidecar から再構成する。
- 参照側は公式 Bruneton CPU reference model の 47 波長スペクトル実装を使用し、同一の太陽方向・幾何条件で Sky Radiance を評価する。

#### RGB / spectral parameter mapping

- DesktopWorld の RGB チャネルは B=440 nm / G=550 nm / R=680 nm に対応させる。
- Rayleigh / Mie / Ozone extinction / Ground albedo は、各 RGB 値を対応波長で一致させ、47 波長の中間値は区分線形補間する。
- DesktopWorld の散乱係数は km⁻¹ のため、参照側の m⁻¹ に 1/1000 で変換する。
- 現行 HLSL の scalar `solar_intensity` は 47 波長で一定の spectral irradiance として参照側に渡す。これは現在の RGB renderer のスケールを揃えるための仮定であり、実太陽スペクトルの再現ではない。
- Rayleigh / Mie の指数密度プロファイル、Ozone の三角プロファイル、`mie_g`、`mu_s_min=-0.207912`、太陽角半径 0.004675 rad を対応させる。
- 参照画像の RGB は 47 波長結果の 680 / 550 / 440 nm samples から取り出す。
- `--self-test` で PFM の bottom-up / top-down 変換・RGB spectral anchor・MAE / RMSE / 最大誤差を検証する。

#### 出力と誤差

- 入力 PFM と同じ top-down image coordinates で CPU reference を評価する。
- `<stem>_bruneton_reference.pfm` に RGB32F / Linear HDR 参照画像を保存する。
- `<stem>_bruneton_metrics.txt` に RGB 全成分および R/G/B 各チャネルの MAE / RMSE / Maximum Absolute Error を保存する。
- 参照 LUT は大気パラメータ hash ごとに OS temporary directory へキャッシュする。Sun Direction は静的 LUT に影響しないため cache key から除外する。
- 参照モデル初回初期化は CPU 負荷が大きく数分を要する場合がある。通常アプリの起動・描画処理には含めない。

この比較は DesktopWorld の 3 波長係数を合わせた参照計算であり、現行 scalar solar intensity の近似を含む。実太陽の連続スペクトル、偏光、地表の空間変動などをモデル化した完全な分光検証とは扱わない。比較ツールのビルド成功と実測値の収集も別々に評価する。

## 10. エラー処理

| 失敗 | 動作 |
|---|---|
| 対応するデスクトップホストを取得できない | 初期化失敗として通知 |
| D3D11 device 作成失敗 | 起動失敗として HRESULT を記録 |
| Shader compile / Create* 失敗 | Shader 名・HRESULT・Compiler message を記録 |
| Static LUT キャッシュ破損 | キャッシュ削除後に再計算 |
| GPU query が未完了 | 取得を次フレーム以降に延期 |
| 設定ファイル不正 | 不正キーを既定値で補完し警告 |
| モニター構成変更・消失 | 旧描画面全体を破棄し、仮想デスクトップ全体を再列挙・再構築する。成功するまで部分表示せず停止状態を維持して再試行 |

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
- B vs D の MAE / RMSE / Maximum Absolute Error。
- A vs D の Linear HDR 比較。解析解像度はアスペクト比を維持した最大 1920 x 1080。
- PFM capture の dimensions / channel order / row order / Linear HDR 値、および `.meta` サイドカーとの整合。
- Bruneton CLI が A/B capture を受け付け、C/D capture を拒否すること。
- Reference PFM の RGB channel mapping と per-channel / combined MAE・RMSE・Maximum Absolute Error。
- 比較時に tone mapping / exposure が混入しないこと。

### 11.3 Integration Test

- 旧 WorkerW、layered Progman、および raised desktop の WorkerW 生成要求を含むホスト取得失敗時の安全な終了。
- Explorer 再起動後の再接続。
- モニター追加・削除・配置変更・解像度変更・DPI 変更時に、単一描画面が仮想デスクトップ全体へ再構築されること。再構築失敗時に部分表示へ縮退しないこと。
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
