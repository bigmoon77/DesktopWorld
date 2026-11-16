#define PI 3.14159265358979323846
#define TAU 6.28318530717958647692

cbuffer FrameCB : register(b0)
{
    float2 g_resolution;
    float2 g_inverse_resolution;
    float3 g_sun_direction;
    float g_exposure;
    float g_blend_factor;
    float3 g_frame_padding;
};

cbuffer AtmosphereCB : register(b1)
{
    float3 g_beta_rayleigh;
    float g_planet_radius_km;
    float3 g_beta_mie;
    float g_atmosphere_height_km;
    float g_rayleigh_scale_height_km;
    float g_mie_scale_height_km;
    float g_mie_g;
    float g_solar_intensity;
};

Texture2D<float4> g_transmittance : register(t0);
Texture2D<float4> g_sky_a : register(t1);
Texture2D<float4> g_sky_b : register(t2);
Texture2D<float4> g_sky_current : register(t3);
SamplerState g_linear_sampler : register(s0);

RWTexture2D<float4> g_compute_output : register(u0);

struct VertexOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float ray_sphere_far(float3 origin, float3 direction, float radius)
{
    const float b = dot(origin, direction);
    const float c = dot(origin, origin) - radius * radius;
    const float discriminant = max(b * b - c, 0.0);
    return -b + sqrt(discriminant);
}

float ray_sphere_near(float3 origin, float3 direction, float radius)
{
    const float b = dot(origin, direction);
    const float c = dot(origin, origin) - radius * radius;
    const float discriminant = b * b - c;
    if (discriminant <= 0.0)
    {
        return -1.0;
    }
    return -b - sqrt(discriminant);
}

float2 transmittance_uv(float altitude_km, float zenith_cosine)
{
    const float h = max(g_atmosphere_height_km, 0.001);
    return float2(
        saturate(zenith_cosine * 0.5 + 0.5),
        saturate(altitude_km / h));
}

float3 sample_transmittance(float altitude_km, float zenith_cosine)
{
    return g_transmittance.SampleLevel(
        g_linear_sampler,
        transmittance_uv(altitude_km, zenith_cosine),
        0).rgb;
}

float3 integrate_transmittance(
    float3 origin,
    float3 direction,
    float max_distance,
    out float optical_rayleigh,
    out float optical_mie)
{
    const int step_count = 32;
    const float step_length = max_distance / step_count;
    optical_rayleigh = 0.0;
    optical_mie = 0.0;

    for (int step_index = 0; step_index < step_count; ++step_index)
    {
        const float distance = (step_index + 0.5) * step_length;
        const float3 position = origin + direction * distance;
        const float altitude = max(length(position) - g_planet_radius_km, 0.0);

        optical_rayleigh += exp(-altitude / max(g_rayleigh_scale_height_km, 0.001)) * step_length;
        optical_mie += exp(-altitude / max(g_mie_scale_height_km, 0.001)) * step_length;
    }

    return exp(-g_beta_rayleigh * optical_rayleigh - g_beta_mie * optical_mie);
}

float3 calculate_transmittance(float altitude_km, float mu)
{
    const float3 origin = float3(0.0, g_planet_radius_km + altitude_km, 0.0);
    const float3 direction = normalize(float3(sqrt(max(1.0 - mu * mu, 0.0)), mu, 0.0));
    const float outer_radius = g_planet_radius_km + g_atmosphere_height_km;

    const float outer_distance = ray_sphere_far(origin, direction, outer_radius);
    const float ground_distance = ray_sphere_near(origin, direction, g_planet_radius_km);

    if (ground_distance > 0.0 && ground_distance < outer_distance)
    {
        return 0.0.xxx;
    }

    float optical_rayleigh = 0.0;
    float optical_mie = 0.0;
    return integrate_transmittance(
        origin,
        direction,
        max(outer_distance, 0.0),
        optical_rayleigh,
        optical_mie);
}

float3 sky_view_direction(float2 uv)
{
    const float azimuth = (uv.x - 0.5) * TAU;
    const float elevation = (0.5 - uv.y) * PI;
    const float horizontal = cos(elevation);

    return normalize(float3(
        sin(azimuth) * horizontal,
        sin(elevation),
        cos(azimuth) * horizontal));
}

float phase_rayleigh(float cosine_angle)
{
    return (3.0 / (16.0 * PI)) * (1.0 + cosine_angle * cosine_angle);
}

float phase_mie(float cosine_angle)
{
    const float g = clamp(g_mie_g, -0.98, 0.98);
    const float denominator = max(
        1.0 + g * g - 2.0 * g * cosine_angle,
        0.001);
    return (1.0 - g * g) /
        (4.0 * PI * pow(denominator, 1.5));
}

float3 evaluate_sky_radiance(float3 view_direction)
{
    const float3 sun_direction = normalize(g_sun_direction);
    const float3 origin = float3(0.0, g_planet_radius_km + 0.01, 0.0);
    const float outer_radius = g_planet_radius_km + g_atmosphere_height_km;

    const float outer_distance = ray_sphere_far(origin, view_direction, outer_radius);
    const float ground_distance = ray_sphere_near(origin, view_direction, g_planet_radius_km);

    float max_distance = max(outer_distance, 0.0);
    if (ground_distance > 0.0)
    {
        max_distance = min(max_distance, ground_distance);
    }

    const int step_count = 24;
    const float step_length = max_distance / step_count;
    const float cosine_angle = dot(view_direction, sun_direction);
    const float rayleigh_phase = phase_rayleigh(cosine_angle);
    const float mie_phase = phase_mie(cosine_angle);

    float optical_rayleigh = 0.0;
    float optical_mie = 0.0;
    float3 radiance = 0.0.xxx;

    for (int step_index = 0; step_index < step_count; ++step_index)
    {
        const float distance = (step_index + 0.5) * step_length;
        const float3 position = origin + view_direction * distance;
        const float altitude = max(length(position) - g_planet_radius_km, 0.0);
        const float3 up_direction = normalize(position);

        const float density_rayleigh =
            exp(-altitude / max(g_rayleigh_scale_height_km, 0.001));
        const float density_mie =
            exp(-altitude / max(g_mie_scale_height_km, 0.001));

        const float sun_zenith_cosine = dot(up_direction, sun_direction);
        const float3 sun_transmittance =
            sample_transmittance(altitude, sun_zenith_cosine);

        const float3 view_transmittance =
            exp(-g_beta_rayleigh * optical_rayleigh - g_beta_mie * optical_mie);

        const float3 rayleigh_contribution =
            g_beta_rayleigh * density_rayleigh * rayleigh_phase;
        const float3 mie_contribution =
            g_beta_mie * density_mie * mie_phase;

        radiance +=
            (rayleigh_contribution + mie_contribution) *
            view_transmittance *
            sun_transmittance *
            (g_solar_intensity * step_length);

        optical_rayleigh += density_rayleigh * step_length;
        optical_mie += density_mie * step_length;
    }

    return max(radiance, 0.0.xxx);
}

float3 tone_map(float3 radiance)
{
    const float3 exposed = max(radiance * max(g_exposure, 0.001), 0.0.xxx);
    const float3 mapped = 1.0.xxx - exp(-exposed);
    return pow(saturate(mapped), 1.0 / 2.2);
}

[numthreads(8, 8, 1)]
void CS_Transmittance(uint3 dispatch_id : SV_DispatchThreadID)
{
    if (dispatch_id.x >= 256 || dispatch_id.y >= 64)
    {
        return;
    }

    const float2 uv = (float2(dispatch_id.xy) + 0.5) / float2(256.0, 64.0);
    const float mu = uv.x * 2.0 - 1.0;
    const float altitude = uv.y * g_atmosphere_height_km;

    g_compute_output[dispatch_id.xy] =
        float4(calculate_transmittance(altitude, mu), 1.0);
}

[numthreads(8, 8, 1)]
void CS_SkyView(uint3 dispatch_id : SV_DispatchThreadID)
{
    if (dispatch_id.x >= 256 || dispatch_id.y >= 128)
    {
        return;
    }

    const float2 uv = (float2(dispatch_id.xy) + 0.5) / float2(256.0, 128.0);
    const float3 view_direction = sky_view_direction(uv);
    const float3 radiance = evaluate_sky_radiance(view_direction);

    g_compute_output[dispatch_id.xy] = float4(radiance, 1.0);
}

[numthreads(8, 8, 1)]
void CS_Blend(uint3 dispatch_id : SV_DispatchThreadID)
{
    if (dispatch_id.x >= 256 || dispatch_id.y >= 128)
    {
        return;
    }

    const int2 coordinate = int2(dispatch_id.xy);
    const float3 a = g_sky_a.Load(int3(coordinate, 0)).rgb;
    const float3 b = g_sky_b.Load(int3(coordinate, 0)).rgb;

    g_compute_output[coordinate] = float4(lerp(a, b, saturate(g_blend_factor)), 1.0);
}

VertexOutput VS_Fullscreen(uint vertex_id : SV_VertexID)
{
    VertexOutput output;

    const float2 position = float2(
        (vertex_id << 1) & 2,
        vertex_id & 2);

    output.position = float4(
        position.x * 2.0 - 1.0,
        1.0 - position.y * 2.0,
        0.0,
        1.0);

    output.uv = position;
    return output;
}

float4 PS_Direct(VertexOutput input) : SV_TARGET
{
    const float3 direction = sky_view_direction(input.uv);
    return float4(tone_map(evaluate_sky_radiance(direction)), 1.0);
}

float4 PS_SkyCurrent(VertexOutput input) : SV_TARGET
{
    const float3 radiance = g_sky_current.SampleLevel(
        g_linear_sampler,
        input.uv,
        0).rgb;
    return float4(tone_map(radiance), 1.0);
}

float4 PS_SkyPair(VertexOutput input) : SV_TARGET
{
    const float3 a = g_sky_a.SampleLevel(g_linear_sampler, input.uv, 0).rgb;
    const float3 b = g_sky_b.SampleLevel(g_linear_sampler, input.uv, 0).rgb;
    return float4(tone_map(lerp(a, b, saturate(g_blend_factor))), 1.0);
}

float4 PS_SkyFinal(VertexOutput input) : SV_TARGET
{
    const float3 radiance = g_sky_current.SampleLevel(
        g_linear_sampler,
        input.uv,
        0).rgb;
    return float4(tone_map(radiance), 1.0);
}
