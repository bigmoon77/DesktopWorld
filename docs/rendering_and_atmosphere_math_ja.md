# DesktopWorld 描画・大気散乱計算の全体解説

- 対象: Win32 / Direct3D 11 / C++20 / HLSL
- 実装対応: src/solar_model.cpp、src/environment_controller.cpp、src/time_sample_scheduler.cpp、src/atmosphere_renderer.cpp、shaders/atmosphere.hlsl
- 目的: 太陽位置の算出から大気散乱、HDR 合成、デスクトップ表示までを数式と実行順序で説明する。

> 本書は現行コードの説明である。現行モデルは Bruneton 系の段階的 LUT 事前計算を参考にした RGB 近似・低解像度プロトタイプであり、47 波長の高精度参照モデルと同等の物理精度を保証しない。

## 1. CPU レンダリングか GPU レンダリングか

DesktopWorld は GPU レンダリングである。CPU は環境条件を決め、GPU に計算・描画コマンドを発行する。画面の各ピクセルに対する大気散乱評価は HLSL の Compute Shader / Pixel Shader が担当する。

| 処理 | 実行場所 | 主な実装 |
|---|---|---|
| UTC 時刻の取得・変換 | CPU | time_provider、environment_controller |
| 太陽高度・方位・方向 | CPU | solar_model.cpp |
| サンプル時刻・補間係数 | CPU | time_sample_scheduler.cpp |
| 大気パラメータ検証・キャッシュキー | CPU | atmosphere_renderer.cpp |
| LUT キャッシュのファイル I/O | CPU | atmosphere_renderer.cpp |
| LUT の散乱積分 | GPU | HLSL Compute Shader |
| Sky View LUT の生成 | GPU | CS_SkyView |
| 2 時刻の Sky View 補間 | GPU | CS_Blend または Pixel Shader |
| 全画面の最終色計算 | GPU | Fullscreen VS + Pixel Shader |
| ウィンドウ・SwapChain・Present | CPU が API 操作、GPU が描画 | Platform / Render |

CPU の UpdateSubresource、Dispatch、Draw、Present は GPU に仕事を依頼する API 呼び出しである。CPU が GPU の全計算を逐次実行する構造ではない。例外として、任意ビルドの bruneton_reference_compare は比較用の CPU 参照実装を実行する。これは通常アプリの描画経路ではない。

## 2. 全体パイプライン

    UTC 時刻 + 緯度経度 + 大気設定
                    |
                    v
       CPU: 太陽位置・方向を計算
                    |
                    v
       CPU: サンプル区間 t0, t1 と係数 α
                    |
                    v
       GPU: Static Atmosphere LUT
             初回 / パラメータ変更時
                    |
                    v
       GPU: Sky View LUT(t0), LUT(t1)
             サンプル区間変更時
                    |
                    v
       GPU: 2 枚の HDR LUT を補間
                    |
                    v
       GPU: Fullscreen Triangle を描画
                    |
                    v
       Linear HDR -> Tone Mapping -> SwapChain
                    |
                    v
                  Present

大気の物理パラメータに依存する LUT と、太陽方向・時刻に依存する Sky View LUT を分離する。太陽が少し動くたびに大気全体の事前計算をやり直さない。

## 3. CPU: 太陽位置計算

### 3.1 入力・座標系

入力は UTC 日時、緯度 φ、経度 λ。経度は東経を正とする。ワールド座標は +Y が天頂、+X が東、+Z が北。方位角 0° は北、90° は東。

UTC 日時から年通算日 N と小数時刻 h を求める。うるう年は Gregorian calendar の規則で判定する。

### 3.2 年周角・均時差・赤緯

NOAA Solar Calculator 系の近似式を使う。

    γ = (2π / 365) * (N - 1 + (h - 12) / 24)

均時差 E は分単位。

    E = 229.18 * (
        0.000075
      + 0.001868 cos γ
      - 0.032077 sin γ
      - 0.014615 cos 2γ
      - 0.040849 sin 2γ )

太陽赤緯 δ はラジアン。

    δ = 0.006918
      - 0.399912 cos γ + 0.070257 sin γ
      - 0.006758 cos 2γ + 0.000907 sin 2γ
      - 0.002697 cos 3γ + 0.001480 sin 3γ

真太陽時 Tsolar と時角 H は次の通り。

    Tsolar = 60h + E + 4λ
    H = (π / 180) * (Tsolar / 4 - 180)

経度 1 度あたり 4 分として UTC から観測地点の真太陽時へ変換する。

### 3.3 太陽高度角・方位角

緯度 φ に対して天頂角 θz は、

    cos θz = sin φ sin δ + cos φ cos δ cos H
    θz = acos(clamp(cos θz, -1, 1))
    e = π/2 - θz

太陽高度角 e は地平線を 0、天頂を π/2 とする。方位角 A は実装上、

    A = normalize_0_2π(
          atan2(sin H, cos H sin φ - tan δ cos φ) + π)

最後に方向ベクトルへ変換する。

    s = ( cos(e) sin(A),
          sin(e),
          cos(e) cos(A) )

この単位ベクトルが HLSL の太陽方向となる。極点・天頂付近では方位角が不安定になり得るため、方向ベクトルを主要な比較値とする。

## 4. CPU: 時間サンプルの決定

日の出・日の入り前後は空の見た目が急変するため、太陽高度に応じてサンプル間隔を変更する。

| 太陽高度の絶対値 | 間隔 |
|---:|---:|
| > 12° | 3600 秒 |
| > 6° ～ <= 12° | 1800 秒 |
| <= 6° | 900 秒 |

手動設定は 3600 / 1800 / 900 秒。

現在 UTC epoch 秒を t、間隔を Δt とすると、

    t0 = floor(t / Δt) * Δt
    t1 = t0 + Δt
    α  = clamp((t - t0) / Δt, 0, 1)

t0 は前側サンプル、t1 は後側サンプル、α は補間係数である。負の epoch 秒でも数学的な floor になるよう C++ 側で専用の floor division を行う。CPU は t0 と t1 における太陽方向も計算し、サンプル区間が変わらない限りキャッシュする。

## 5. 大気モデルのパラメータ

現行モデルは RGB 3 チャンネルで光の減衰・散乱を近似する。

| パラメータ | 初期値 | 意味 |
|---|---:|---|
| 惑星半径 Rg | 6360 km | 地表半径 |
| 大気高度 Ha | 100 km | 大気上端までの高さ |
| Rayleigh scale height HR | 8 km | 分子密度の減衰距離 |
| Mie scale height HM | 1.2 km | エアロゾル密度の減衰距離 |
| Mie anisotropy g | 0.8 | 前方散乱の強さ |
| Ozone center | 25 km | オゾン密度ピーク |
| Ozone half width | 15 km | 三角形プロファイルの半幅 |
| Ground albedo | (0.1, 0.1, 0.1) | 地面の拡散反射率 |
| Solar intensity | 20 | 太陽光スカラー強度 |
| 最大散乱次数 | 4 | 多重散乱の打ち切り次数 |

βR は Rayleigh 散乱係数、βM は Mie 散乱係数、βMe は Mie 消散係数、βO はオゾン吸収係数。それぞれ RGB ベクトルである。距離を km で積分するため、係数は逆 km に対応する。

## 6. 高度による大気密度

地表からの高度を h km とする。

Rayleigh 密度:

    ρR(h) = exp(-max(h, 0) / HR)

Mie 密度:

    ρM(h) = exp(-max(h, 0) / HM)

オゾン密度は三角形で近似する。

    ρO(h) = saturate(1 - abs(h - hO) / wO)

hO は中心高度、wO は半幅。中心高度で 1、中心から半幅以上離れると 0 になる。

## 7. Transmittance: レイに沿った透過率

### 7.1 球面との交点

観測点の惑星中心からの距離を r、レイ方向と天頂方向の内積を μ = cos θ とする。大気上端半径は Rt = Rg + Ha。

上端との交点距離:

    dt = -r μ + sqrt(r²(μ² - 1) + Rt²)

地面との交点距離:

    dg = -r μ - sqrt(r²(μ² - 1) + Rg²)

レイが地面と交差するか判定し、積分終端を地面または大気上端にする。

### 7.2 光学的厚さ

レイ上の距離 d における半径と高度:

    r(d) = sqrt(r² + 2 r μ d + d²)
    h(d) = r(d) - Rg

光学的厚さ:

    τR = integral[0,D] ρR(h(d)) dd
    τM = integral[0,D] ρM(h(d)) dd
    τO = integral[0,D] ρO(h(d)) dd

現行実装は 48 分割の台形則で積分する。

    integral[0,D] f(x) dx
      ≈ Δx * ((f0 + fn)/2 + sum(i=1..n-1, fi))
    Δx = D / n

端点の重みは 0.5、中間点は 1.0。

### 7.3 Beer-Lambert 則

RGB 透過率は、

    T = exp(-βR τR - βMe τM - βO τO)

exp は RGB 各成分に適用する。光学的厚さが大きいほど透過率は 0 に近づく。

### 7.4 Transmittance LUT

毎回 48 ステップ積分すると高価なので、Compute Shader で 256 x 64 の 2D LUT を事前生成する。状態 (r, μ) をテクスチャ UV へ写像し、実行時は線形サンプルする。高度と大気上端までの距離を正規化して LUT に格納する。テクセル中心を使うため、座標変換では half-texel 分を考慮する。

任意の有限区間の透過率は、大気上端までの透過率の比から求める。地面と交差しない場合の概念式:

    T(A -> B) = T(A -> top) / max(T(B -> top), ε)

地面方向はレイを反転した対応する比を使い、結果を 1 以下に制限する。

## 8. Phase Function: 散乱の角度依存性

散乱は入射光と観測方向の角度に依存する。両方向の内積を ν = cos Θ = ω・ωs とする。

Rayleigh phase function:

    PR(ν) = 3 / (16π) * (1 + ν²)

前方・後方で対称な分布になる。短波長側ほど Rayleigh 散乱係数が大きいことが青空の主因。

現行 Mie phase function:

    PM(ν) = (3 / (8π)) * ((1 - g²) / (2 + g²))
            * (1 + ν²) / (1 + g² - 2gν)^(3/2)

g が正で大きいほど前方散乱が強くなる。既定値 g=0.8 は太陽方向付近の強い散乱を作る。

## 9. Single Scattering: 単一散乱

単一散乱は太陽光が大気中の一点で一度散乱し、観測方向へ届く光である。

観測点から視線方向へ距離 d 進んだ位置:

    ri = sqrt(r² + 2 r μ d + d²)
    μs_i = (r μs + d ν) / ri

散乱点までの視線透過率と、太陽から散乱点までの太陽方向透過率を掛ける。

    Tpath(d) = T(view path) * T(sun path)

Rayleigh と Mie の積分:

    SR = Isun βR integral[0,D] Tpath(d) ρR(h(d)) dd
    SM = Isun βM integral[0,D] Tpath(d) ρM(h(d)) dd

現行コードは各積分を 24 分割の台形則で近似する。Rayleigh と Mie は別々の 3D LUT に保存し、描画時に位相関数を掛ける。

太陽方向の透過率には太陽円盤の可視係数を掛ける。太陽角半径を使い、地平線近傍で可視・不可視を滑らかに切り替える。

## 10. Multiple Scattering: 多重散乱

### 10.1 散乱次数

- 1 次: 太陽直射光が一度散乱。
- 2 次: 1 次散乱光がもう一度散乱。
- 3 次以降: さらに散乱を繰り返す。

現行実装は最大次数 2～4 を選べる。

### 10.2 Scattering Density

位置・視線条件ごとに全方向から入る前次数の放射輝度を角度積分する。球面方向を θ, φ で表すと、

    dω = sin(θ) dθ dφ

散乱密度の概念式:

    Jn(x, ω) = integral[4π] L(n-1)(x, ωi)
      * [βR ρR PR(ωi・ω) + βM ρM PM(ωi・ω)] dωi

RGB 成分ごとの積を使う。現行実装は θ=4、φ=8 の合計 32 方向でサンプリングし、各サンプルに sin θ Δθ Δφ の重みを掛ける。

レイが地面に当たる方向では、地面からの Lambert 拡散反射を入射光へ加える。

    Lground = TtoGround * albedo * (1/π) * Eground

1/π は Lambert 拡散反射の BRDF。

### 10.3 Multiple Scattering 積分

散乱密度を視線方向へ積分し、観測点までの透過率を掛ける。

    Sn = integral[0,D] Jn(ri, μi, μs_i, ν)
         * T(view path, d) dd

現行実装は 24 分割の台形則で近似する。

### 10.4 Irradiance と累積

地面反射に使う Irradiance は半球方向の入射放射輝度を余弦重み付きで積分する。

    E(x, n) = integral[hemisphere] L(x, ω)
              * max(0, n・ω) dω

現行コードは θ=4、φ=8 の角度サンプルで近似する。

散乱次数ごとに、(1) Scattering Density、(2) Indirect Irradiance、(3) Multiple Scattering、(4) Scattering と Irradiance の累積を行う。ping-pong テクスチャを入れ替え、次の次数へ進む。CPU は順序と Dispatch 数を制御し、各 texel の積分は GPU が実行する。

## 11. LUT 構成

すべて RGBA16F (DXGI_FORMAT_R16G16B16A16_FLOAT)。RGB に透過率・照度・放射輝度などを格納する。

| LUT | 次元 | 用途 |
|---|---:|---|
| Transmittance | 256 x 64 | レイの透過率 |
| Direct Irradiance | 64 x 16 | 太陽直射照度 |
| Scattering | 256 x 128 x 32 | 高度・視線・太陽角度・相対角度に依存する散乱 |
| Sky View | 256 x 128 | 特定の太陽方向で見える空の放射輝度 |

3D Scattering LUT は 4 変数を 3 軸へパックする。

- 高度 r
- 視線天頂余弦 μ
- 太陽天頂余弦 μs
- 視線と太陽方向の相対角余弦 ν

ν は 8 スライスとして幅方向へ格納し、各スライス内に μs の 32 サンプルを配置する。幅は 8 x 32 = 256、高さは μ の 128、深さは高度の 32。地面と交差する方向・しない方向で μ の領域を分ける。

サンプリング時、ν 方向は隣接スライスを 2 回サンプルして線形補間し、スライス内の軸は D3D 線形サンプラーで補間する。

## 12. Static LUT 事前計算

大気パラメータ変更時、または有効キャッシュがない時に実行する。

1. Transmittance: 256 x 64、48 ステップで光学的厚さを積分。
2. Direct Irradiance: 64 x 16、太陽光の透過と地平線可視性を評価。
3. Single Scattering: 256 x 128 x 32、24 ステップで Rayleigh / Mie を分離積分。
4. Scattering Density: 4 x 8 方向を積分し、前次数の散乱光と地面反射を評価。
5. Indirect Irradiance: 半球方向を積分して照度を求め、累積。
6. Multiple Scattering: 散乱密度を 24 ステップで視線積分。
7. 5 と 6 を必要次数まで反復。
8. Static LUT 群をディスクキャッシュへ保存。

キャッシュキーには大気パラメータ、LUT 寸法、形式、アルゴリズムバージョンを含む。時刻・太陽方向は含めない。有効キャッシュがあれば GPU テクスチャへロードして事前計算を省略する。

## 13. Dynamic Sky View LUT

### 13.1 UV から視線方向へ

Sky View LUT の UV から方位角 a と高度角 e を作る。

    a = (u - 0.5) * 2π
    e = (1 - v) * π/2

方向ベクトル:

    ω = (sin(a) cos(e), sin(e), cos(a) cos(e))

観測位置は地表から 0.01 km 上、r = Rg + 0.01。

    μ  = ω.y
    μs = sun_direction.y
    ν  = dot(ω, sun_direction)

これらで 3D Scattering LUT を参照し、Sky View の RGB radiance を得る。

### 13.2 時刻補間

サンプル時刻 t0,t1 の Sky View を L0,L1 とする。Compute Shader 方式 D は各 texel について、

    L(t) = (1 - α) L0 + α L1

を計算して sky_current に書く。Pixel Shader 方式 C は描画時に同じ式を適用する。補間はトーンマッピング前の Linear HDR radiance で行う。ガンマ変換後の色を補間すると光量として正しい中間値にならない。

## 14. 描画方式 A～D

| 方式 | 処理 | 特徴 |
|---|---|---|
| A Direct | Pixel Shader が各画面ピクセルで scattering LUT を直接評価 | Sky View LUT と時間補間を省く比較基準 |
| B Sky Current | 現在時刻の Sky View LUT を Compute Shader で生成し Pixel Shader で表示 | 現在時刻 LUT の品質・生成コストの基準 |
| C Pixel Blend | 2 枚の Sky View LUT を Pixel Shader で lerp | 各画面ピクセルで 2 枚を読み補間 |
| D Compute Blend | 2 枚を Compute Shader で補間して 1 枚にし Pixel Shader で表示 | 低解像度 LUT 上で先に補間。現行既定方式 |

A～D は同じ Static LUT と大気パラメータを使う。A と D の差には Sky View の空間解像度・時間サンプル補間による近似誤差が含まれる。

## 15. Tone Mapping と最終描画

散乱結果は Linear HDR radiance。露出 q を掛けて簡易指数トーンマッピングを行う。

    exposed = max(q * L, 0)
    mapped = 1 - exp(-exposed)
    display = saturate(mapped)^(1/2.2)

最後の 1/2.2 は簡易ガンマ変換。物理計算・HDR 誤差計測・PFM キャプチャではトーンマッピング前の radiance を使う。

頂点バッファを作らず SV_VertexID から 3 頂点の Fullscreen Triangle を生成する。Pixel Shader は UV から Sky View をサンプルする。方式 A は scattering LUT を直接評価する。GPU は SwapChain に紐づく RenderTarget へ描画し、CPU が Present を呼ぶ。

## 16. CPU / GPU の計測と同期

- CPU 時間: 環境更新、描画コマンド投入、Present 周辺。
- GPU 時間: D3D11 Timestamp Query で LUT 生成、補間、描画を計測。
- Timestamp Query はリングバッファで非同期取得。未完了なら前回値を保持し、通常計測で GPU 完了待ちは行わない。
- HDR 品質比較・PFM キャプチャは staging texture へコピーして CPU へ readback する。診断操作は同期的で、一時的な待ち時間が発生し得る。

CPU の Dispatch 発行時間と GPU が実際に計算した時間は異なる。

## 17. 現行実装の近似・制限

1. RGB 近似であり、波長ごとのスペクトル積分を行わない。
2. 積分は有限ステップの数値積分。
3. 角度積分は 4 x 8 サンプルで、精度より負荷を優先。
4. LUT 解像度が低く、テクスチャ補間誤差を含む。
5. オゾン密度は三角形プロファイル。
6. 太陽強度の波長依存変換はスカラー強度による近似。
7. 時間方向は 2 サンプルの線形補間。
8. 地面反射は Lambert 拡散反射で、地形・材質・局所遮蔽は扱わない。
9. 物理精度の評価には同じ条件で Bruneton 参照実装との比較が必要。

## 18. コードを読む順番

1. src/solar_model.cpp: UTC・緯度経度から太陽方向を作る。
2. src/time_sample_scheduler.cpp: サンプル区間と補間係数を決める。
3. src/environment_controller.cpp: 現在の太陽と隣接サンプルをまとめる。
4. src/atmosphere_renderer.cpp: D3D11 リソース、キャッシュ、Compute Dispatch、描画を管理。
5. shaders/atmosphere.hlsl: 密度・透過率・位相関数・散乱積分・Sky View・最終色を計算。
6. src/atmosphere_sky_pass.cpp と src/render_system.cpp: Render Pass として実行。

### 主要 HLSL エントリーポイント

| Entry Point | 役割 |
|---|---|
| CS_Transmittance | Transmittance LUT |
| CS_DirectIrradiance | 直達照度 |
| CS_SingleScattering | Rayleigh / Mie 単一散乱 |
| CS_ScatteringDensity | 多重散乱の散乱密度 |
| CS_IndirectIrradiance | 間接照度 |
| CS_MultipleScattering | 次数ごとの散乱光 |
| CS_AccumulateScattering | 散乱次数の累積 |
| CS_AccumulateIrradiance | 照度の累積 |
| CS_SkyView | 時刻サンプル用 Sky View LUT |
| CS_Blend | 2 サンプルの Compute 補間 |
| VS_Fullscreen | Fullscreen Triangle |
| PS_Direct | 方式 A |
| PS_SkyCurrent | 方式 B |
| PS_SkyPair | 方式 C |
| PS_SkyFinal | 方式 D |

## 19. 学習・検証の着眼点

- Rayleigh と Mie の係数を個別に変え、散乱色と太陽近傍の明るさを確認する。
- 各 scale height を変え、散乱が発生する高度分布を確認する。
- Mie g を 0 に近づけると前方指向性が弱まり、正に大きくすると太陽方向の光輪が強くなる。
- オゾン中心高度・幅・吸収係数を変え、RGB の吸収差を確認する。
- 散乱次数を 2 / 3 / 4 と変え、GPU 時間と HDR 誤差を比較する。
- 方式 A～D を同じ時刻・地点で切り替え、GPU 時間と MAE / RMSE / 最大誤差を記録する。
- 太陽高度 -12°～+12° を重点的に観察する。
