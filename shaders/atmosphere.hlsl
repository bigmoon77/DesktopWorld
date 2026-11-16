#define PI 3.14159265358979323846
#define TAU 6.28318530717958647692

#define TRANSMITTANCE_WIDTH 256
#define TRANSMITTANCE_HEIGHT 64

#define IRRADIANCE_WIDTH 64
#define IRRADIANCE_HEIGHT 16

#define SCATTERING_R_SIZE 32
#define SCATTERING_MU_SIZE 128
#define SCATTERING_MU_S_SIZE 32
#define SCATTERING_NU_SIZE 8
#define SCATTERING_WIDTH (SCATTERING_MU_S_SIZE * SCATTERING_NU_SIZE)
#define SCATTERING_HEIGHT SCATTERING_MU_SIZE
#define SCATTERING_DEPTH SCATTERING_R_SIZE

cbuffer FrameCB : register(b0)
{
    float2 g_resolution;
    float2 g_inverse_resolution;
    float3 g_sun_direction;
    float g_exposure;
    float g_blend_factor;
    uint g_scattering_order;
    float2 g_frame_padding;
};

cbuffer AtmosphereCB : register(b1)
{
    float3 g_beta_rayleigh;
    float g_planet_radius_km;

    float3 g_beta_mie;
    float g_atmosphere_height_km;

    float3 g_beta_mie_extinction;
    float g_rayleigh_scale_height_km;

    float3 g_beta_ozone;
    float g_mie_scale_height_km;

    float3 g_ground_albedo;
    float g_mie_g;

    float g_solar_intensity;
    float g_ozone_center_height_km;
    float g_ozone_half_width_km;
    float g_sun_angular_radius;

    float g_mu_s_min;
    uint g_max_scattering_order;
    uint g_angular_theta_samples;
    uint g_angular_phi_samples;
};

Texture2D<float4> g_transmittance : register(t0);
Texture3D<float4> g_single_rayleigh : register(t1);
Texture3D<float4> g_single_mie : register(t2);
Texture3D<float4> g_scattering_source : register(t3);
Texture2D<float4> g_irradiance_source : register(t4);
Texture3D<float4> g_scattering_density : register(t5);
Texture2D<float4> g_sky_a : register(t6);
Texture2D<float4> g_sky_b : register(t7);
Texture2D<float4> g_sky_current : register(t8);
Texture3D<float4> g_scattering_sum : register(t9);
Texture3D<float4> g_mie_single_render : register(t10);

SamplerState g_linear_sampler : register(s0);
SamplerState g_clamp_sampler : register(s1);

RWTexture2D<float4> g_output_2d : register(u0);
RWTexture3D<float4> g_output_3d : register(u1);
RWTexture3D<float4> g_output_3d_b : register(u2);

struct VertexOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float safe_sqrt(float value)
{
    return sqrt(max(value, 0.0));
}

float clamp_cosine(float value)
{
    return clamp(value, -1.0, 1.0);
}

float clamp_radius(float value)
{
    return clamp(
        value,
        g_planet_radius_km,
        g_planet_radius_km + g_atmosphere_height_km);
}

float get_texture_coord_from_unit_range(float value, float size)
{
    return 0.5 / size + saturate(value) * (1.0 - 1.0 / size);
}

float get_unit_range_from_texture_coord(float value, float size)
{
    return saturate((value - 0.5 / size) / (1.0 - 1.0 / size));
}

float distance_to_top_boundary(float radius, float mu)
{
    const float discriminant =
        radius * radius * (mu * mu - 1.0) +
        (g_planet_radius_km + g_atmosphere_height_km) *
        (g_planet_radius_km + g_atmosphere_height_km);

    return max(
        -radius * mu + safe_sqrt(discriminant),
        0.0);
}

float distance_to_bottom_boundary(float radius, float mu)
{
    const float discriminant =
        radius * radius * (mu * mu - 1.0) +
        g_planet_radius_km * g_planet_radius_km;

    return max(-radius * mu - safe_sqrt(discriminant), 0.0);
}

bool ray_intersects_ground(float radius, float mu)
{
    return mu < 0.0 &&
        radius * radius * (mu * mu - 1.0) +
        g_planet_radius_km * g_planet_radius_km >= 0.0;
}

float distance_to_nearest_boundary(
    float radius,
    float mu,
    bool intersects_ground)
{
    return intersects_ground
        ? distance_to_bottom_boundary(radius, mu)
        : distance_to_top_boundary(radius, mu);
}

float get_rayleigh_density(float altitude_km)
{
    return exp(-max(altitude_km, 0.0) /
        max(g_rayleigh_scale_height_km, 0.001));
}

float get_mie_density(float altitude_km)
{
    return exp(-max(altitude_km, 0.0) /
        max(g_mie_scale_height_km, 0.001));
}

float get_ozone_density(float altitude_km)
{
    const float normalized =
        abs(altitude_km - g_ozone_center_height_km) /
        max(g_ozone_half_width_km, 0.001);
    return saturate(1.0 - normalized);
}

float2 transmittance_uv_from_r_mu(float radius, float mu)
{
    const float bottom_radius = g_planet_radius_km;
    const float top_radius = bottom_radius + g_atmosphere_height_km;
    const float H = safe_sqrt(top_radius * top_radius - bottom_radius * bottom_radius);
    const float rho = safe_sqrt(radius * radius - bottom_radius * bottom_radius);

    const float distance = distance_to_top_boundary(radius, mu);
    const float distance_min = top_radius - radius;
    const float distance_max = rho + H;

    const float x_mu = (distance - distance_min) /
        max(distance_max - distance_min, 1.0e-6);
    const float x_r = rho / max(H, 1.0e-6);

    return float2(
        get_texture_coord_from_unit_range(x_mu, TRANSMITTANCE_WIDTH),
        get_texture_coord_from_unit_range(x_r, TRANSMITTANCE_HEIGHT));
}

float3 integrate_transmittance(
    float radius,
    float mu,
    out float optical_rayleigh,
    out float optical_mie,
    out float optical_ozone)
{
    const int sample_count = 48;
    const float distance = distance_to_top_boundary(radius, mu);
    const float step_length = distance / sample_count;

    optical_rayleigh = 0.0;
    optical_mie = 0.0;
    optical_ozone = 0.0;

    [loop]
    for (int i = 0; i <= sample_count; ++i)
    {
        const float current_distance = i * step_length;
        const float current_radius = clamp_radius(sqrt(
            current_distance * current_distance +
            2.0 * radius * mu * current_distance +
            radius * radius));
        const float altitude = current_radius - g_planet_radius_km;
        const float weight = (i == 0 || i == sample_count) ? 0.5 : 1.0;

        optical_rayleigh += get_rayleigh_density(altitude) * weight;
        optical_mie += get_mie_density(altitude) * weight;
        optical_ozone += get_ozone_density(altitude) * weight;
    }

    optical_rayleigh *= step_length;
    optical_mie *= step_length;
    optical_ozone *= step_length;

    return exp(
        -g_beta_rayleigh * optical_rayleigh -
        g_beta_mie_extinction * optical_mie -
        g_beta_ozone * optical_ozone);
}

float3 sample_transmittance_to_top(float radius, float mu)
{
    const float2 uv = transmittance_uv_from_r_mu(radius, clamp_cosine(mu));
    return g_transmittance.SampleLevel(g_clamp_sampler, uv, 0).rgb;
}

float3 get_transmittance(
    float radius,
    float mu,
    float distance,
    bool intersects_ground)
{
    const float radius_d = clamp_radius(sqrt(
        distance * distance +
        2.0 * radius * mu * distance +
        radius * radius));
    const float mu_d = clamp_cosine((radius * mu + distance) / radius_d);

    if (intersects_ground)
    {
        return min(
            sample_transmittance_to_top(radius, -mu) /
                max(sample_transmittance_to_top(radius_d, -mu_d), float3(1.0e-6, 1.0e-6, 1.0e-6)),
            1.0.xxx);
    }

    return min(
        sample_transmittance_to_top(radius, mu) /
            max(sample_transmittance_to_top(radius_d, mu_d), float3(1.0e-6, 1.0e-6, 1.0e-6)),
        1.0.xxx);
}

float3 get_transmittance_to_sun(float radius, float mu_s)
{
    const float sin_theta_h = g_planet_radius_km / radius;
    const float cos_theta_h = -safe_sqrt(1.0 - sin_theta_h * sin_theta_h);
    const float visible_sun = smoothstep(
        -sin_theta_h * g_sun_angular_radius,
        sin_theta_h * g_sun_angular_radius,
        mu_s - cos_theta_h);

    return sample_transmittance_to_top(radius, mu_s) * visible_sun;
}

float phase_rayleigh(float nu)
{
    return (3.0 / (16.0 * PI)) * (1.0 + nu * nu);
}

float phase_mie(float nu)
{
    const float g = clamp(g_mie_g, -0.98, 0.98);
    const float denominator = max(1.0 + g * g - 2.0 * g * nu, 1.0e-4);
    const float normalization =
        (3.0 / (8.0 * PI)) * (1.0 - g * g) / (2.0 + g * g);

    return normalization * (1.0 + nu * nu) /
        pow(denominator, 1.5);
}

float2 irradiance_uv_from_r_mu_s(float radius, float mu_s)
{
    const float x_mu_s = mu_s * 0.5 + 0.5;
    const float x_r =
        (radius - g_planet_radius_km) /
        max(g_atmosphere_height_km, 1.0e-6);

    return float2(
        get_texture_coord_from_unit_range(x_mu_s, IRRADIANCE_WIDTH),
        get_texture_coord_from_unit_range(x_r, IRRADIANCE_HEIGHT));
}

float3 sample_irradiance(float radius, float mu_s)
{
    return g_irradiance_source.SampleLevel(
        g_clamp_sampler,
        irradiance_uv_from_r_mu_s(radius, mu_s),
        0).rgb;
}

void decode_scattering_parameters(
    float3 frag_coord,
    out float radius,
    out float mu,
    out float mu_s,
    out float nu,
    out bool intersects_ground)
{
    const float H = safe_sqrt(
        (g_planet_radius_km + g_atmosphere_height_km) *
        (g_planet_radius_km + g_atmosphere_height_km) -
        g_planet_radius_km * g_planet_radius_km);

    const float frag_nu = floor(frag_coord.x / SCATTERING_MU_S_SIZE);
    const float frag_mu_s = fmod(frag_coord.x, SCATTERING_MU_S_SIZE);

    const float u_nu = frag_nu / (SCATTERING_NU_SIZE - 1.0);
    const float u_mu_s = frag_mu_s / SCATTERING_MU_S_SIZE;
    const float u_mu = frag_coord.y / SCATTERING_MU_SIZE;
    const float u_r = frag_coord.z / SCATTERING_R_SIZE;

    const float rho = H * get_unit_range_from_texture_coord(u_r, SCATTERING_R_SIZE);
    radius = sqrt(rho * rho + g_planet_radius_km * g_planet_radius_km);

    if (u_mu < 0.5)
    {
        const float distance_min = radius - g_planet_radius_km;
        const float distance_max = rho;
        const float distance = distance_min +
            (distance_max - distance_min) *
            get_unit_range_from_texture_coord(
                1.0 - 2.0 * u_mu,
                SCATTERING_MU_SIZE / 2.0);

        mu = distance <= 1.0e-6
            ? -1.0
            : clamp_cosine(
                -(rho * rho + distance * distance) /
                (2.0 * radius * distance));
        intersects_ground = true;
    }
    else
    {
        const float top_radius = g_planet_radius_km + g_atmosphere_height_km;
        const float distance_min = top_radius - radius;
        const float distance_max = rho + H;
        const float distance = distance_min +
            (distance_max - distance_min) *
            get_unit_range_from_texture_coord(
                2.0 * u_mu - 1.0,
                SCATTERING_MU_SIZE / 2.0);

        mu = distance <= 1.0e-6
            ? 1.0
            : clamp_cosine(
                (H * H - rho * rho - distance * distance) /
                (2.0 * radius * distance));
        intersects_ground = false;
    }

    const float x_mu_s =
        get_unit_range_from_texture_coord(u_mu_s, SCATTERING_MU_S_SIZE);
    const float distance_min = g_atmosphere_height_km;
    const float distance_max = H;
    const float distance_to_min_sun = distance_to_top_boundary(
        g_planet_radius_km,
        g_mu_s_min);
    const float A = (distance_to_min_sun - distance_min) /
        max(distance_max - distance_min, 1.0e-6);
    const float a = (A - x_mu_s * A) / (1.0 + x_mu_s * A);
    const float distance = distance_min +
        min(a, A) * (distance_max - distance_min);

    mu_s = distance <= 1.0e-6
        ? 1.0
        : clamp_cosine(
            (H * H - distance * distance) /
            (2.0 * g_planet_radius_km * distance));

    nu = clamp_cosine(u_nu * 2.0 - 1.0);
    const float nu_min =
        mu * mu_s - sqrt(max((1.0 - mu * mu) * (1.0 - mu_s * mu_s), 0.0));
    const float nu_max =
        mu * mu_s + sqrt(max((1.0 - mu * mu) * (1.0 - mu_s * mu_s), 0.0));
    nu = clamp(nu, nu_min, nu_max);
}

float4 scattering_uvwz_from_r_mu_mu_s_nu(
    float radius,
    float mu,
    float mu_s,
    float nu,
    bool intersects_ground)
{
    const float H = safe_sqrt(
        (g_planet_radius_km + g_atmosphere_height_km) *
        (g_planet_radius_km + g_atmosphere_height_km) -
        g_planet_radius_km * g_planet_radius_km);
    const float rho = safe_sqrt(
        radius * radius -
        g_planet_radius_km * g_planet_radius_km);
    const float u_r = get_texture_coord_from_unit_range(
        rho / max(H, 1.0e-6),
        SCATTERING_R_SIZE);

    const float r_mu = radius * mu;
    const float discriminant =
        r_mu * r_mu -
        radius * radius +
        g_planet_radius_km * g_planet_radius_km;
    float u_mu = 0.0;

    if (intersects_ground)
    {
        const float distance = -r_mu - safe_sqrt(discriminant);
        const float distance_min = radius - g_planet_radius_km;
        const float distance_max = rho;
        const float normalized = distance_max == distance_min
            ? 0.0
            : (distance - distance_min) /
                (distance_max - distance_min);
        u_mu = 0.5 - 0.5 * get_texture_coord_from_unit_range(
            normalized,
            SCATTERING_MU_SIZE / 2.0);
    }
    else
    {
        const float distance = -r_mu + safe_sqrt(discriminant + H * H);
        const float distance_min =
            g_planet_radius_km + g_atmosphere_height_km - radius;
        const float distance_max = rho + H;
        u_mu = 0.5 + 0.5 * get_texture_coord_from_unit_range(
            (distance - distance_min) /
                max(distance_max - distance_min, 1.0e-6),
            SCATTERING_MU_SIZE / 2.0);
    }

    const float distance_s = distance_to_top_boundary(
        g_planet_radius_km,
        mu_s);
    const float distance_min_s = g_atmosphere_height_km;
    const float distance_max_s = H;
    const float a = (distance_s - distance_min_s) /
        max(distance_max_s - distance_min_s, 1.0e-6);

    const float distance_to_min_sun = distance_to_top_boundary(
        g_planet_radius_km,
        g_mu_s_min);
    const float A = (distance_to_min_sun - distance_min_s) /
        max(distance_max_s - distance_min_s, 1.0e-6);

    const float u_mu_s = get_texture_coord_from_unit_range(
        max(1.0 - a / max(A, 1.0e-6), 0.0) /
            (1.0 + a),
        SCATTERING_MU_S_SIZE);

    const float u_nu = nu * 0.5 + 0.5;
    return float4(u_nu, u_mu_s, u_mu, u_r);
}

float3 sample_scattering_3d(
    Texture3D<float4> source_texture,
    float radius,
    float mu,
    float mu_s,
    float nu,
    bool intersects_ground)
{
    const float4 uvwz = scattering_uvwz_from_r_mu_mu_s_nu(
        radius, mu, mu_s, nu, intersects_ground);

    const float tex_coord_nu = uvwz.x * (SCATTERING_NU_SIZE - 1.0);
    const float slice = floor(tex_coord_nu);
    const float slice_blend = tex_coord_nu - slice;

    const float3 uvw0 = float3(
        (slice + uvwz.y) / SCATTERING_NU_SIZE,
        uvwz.z,
        uvwz.w);
    const float3 uvw1 = float3(
        (min(slice + 1.0, SCATTERING_NU_SIZE - 1.0) + uvwz.y) /
            SCATTERING_NU_SIZE,
        uvwz.z,
        uvwz.w);

    const float3 value0 = source_texture.SampleLevel(
        g_clamp_sampler,
        uvw0,
        0).rgb;
    const float3 value1 = source_texture.SampleLevel(
        g_clamp_sampler,
        uvw1,
        0).rgb;

    return lerp(value0, value1, slice_blend);
}

float3 get_single_scattering(
    float radius,
    float mu,
    float mu_s,
    float nu,
    bool intersects_ground)
{
    const float3 rayleigh = sample_scattering_3d(
        g_single_rayleigh,
        radius,
        mu,
        mu_s,
        nu,
        intersects_ground);
    const float3 mie = sample_scattering_3d(
        g_single_mie,
        radius,
        mu,
        mu_s,
        nu,
        intersects_ground);

    return rayleigh * phase_rayleigh(nu) +
        mie * phase_mie(nu);
}

float3 get_full_scattering(
    float radius,
    float mu,
    float mu_s,
    float nu,
    bool intersects_ground)
{
    const float3 multiple = sample_scattering_3d(
        g_scattering_sum,
        radius,
        mu,
        mu_s,
        nu,
        intersects_ground);
    const float3 mie = sample_scattering_3d(
        g_mie_single_render,
        radius,
        mu,
        mu_s,
        nu,
        intersects_ground);

    return multiple * phase_rayleigh(nu) +
        mie * phase_mie(nu);
}

float3 get_scattering_source(
    float radius,
    float mu,
    float mu_s,
    float nu,
    bool intersects_ground,
    uint scattering_order)
{
    if (scattering_order <= 1)
    {
        return get_single_scattering(
            radius,
            mu,
            mu_s,
            nu,
            intersects_ground);
    }

    return sample_scattering_3d(
        g_scattering_source,
        radius,
        mu,
        mu_s,
        nu,
        intersects_ground);
}

float3 compute_direct_irradiance(float radius, float mu_s)
{
    const float alpha_s = g_sun_angular_radius;
    const float average_cosine =
        mu_s < -alpha_s ? 0.0 :
        (mu_s > alpha_s ? mu_s :
            (mu_s + alpha_s) * (mu_s + alpha_s) /
            (4.0 * alpha_s));

    return g_solar_intensity *
        get_transmittance_to_sun(radius, mu_s) *
        average_cosine;
}

float3 compute_single_scattering(
    float radius,
    float mu,
    float mu_s,
    float nu,
    bool intersects_ground,
    out float3 mie_scattering)
{
    const int sample_count = 24;
    const float distance = distance_to_nearest_boundary(
        radius,
        mu,
        intersects_ground);
    const float dx = distance / sample_count;

    float3 rayleigh_sum = 0.0.xxx;
    float3 mie_sum = 0.0.xxx;

    [loop]
    for (int i = 0; i <= sample_count; ++i)
    {
        const float d = i * dx;
        const float r_i = clamp_radius(sqrt(
            d * d + 2.0 * radius * mu * d + radius * radius));
        const float mu_s_i = clamp_cosine((radius * mu_s + d * nu) / r_i);

        const float3 transmittance =
            get_transmittance(radius, mu, d, intersects_ground) *
            get_transmittance_to_sun(r_i, mu_s_i);

        const float altitude = r_i - g_planet_radius_km;
        const float weight = (i == 0 || i == sample_count) ? 0.5 : 1.0;

        rayleigh_sum += transmittance *
            get_rayleigh_density(altitude) * weight;
        mie_sum += transmittance *
            get_mie_density(altitude) * weight;
    }

    rayleigh_sum *= dx * g_solar_intensity * g_beta_rayleigh;
    mie_sum *= dx * g_solar_intensity * g_beta_mie;
    mie_scattering = mie_sum;

    return rayleigh_sum;
}

float3 compute_scattering_density(
    float radius,
    float mu,
    float mu_s,
    float nu,
    bool intersects_ground,
    uint scattering_order)
{
    const uint theta_samples = max(g_angular_theta_samples, 2);
    const uint phi_samples = max(g_angular_phi_samples, 4);
    const float dtheta = PI / theta_samples;
    const float dphi = TAU / phi_samples;

    const float3 omega = float3(safe_sqrt(1.0 - mu * mu), 0.0, mu);
    const float sun_dir_x = omega.x <= 1.0e-6
        ? 0.0
        : (nu - mu * mu_s) / omega.x;
    const float sun_dir_y = safe_sqrt(
        1.0 - sun_dir_x * sun_dir_x - mu_s * mu_s);
    const float3 omega_s = float3(sun_dir_x, sun_dir_y, mu_s);

    float3 sum = 0.0.xxx;
    const float3 zenith = float3(0.0, 0.0, 1.0);

    [loop]
    for (uint j = 0; j < theta_samples; ++j)
    {
        const float theta = (j + 0.5) * dtheta;
        const float sin_theta = sin(theta);
        const float cos_theta = cos(theta);
        const bool ground_hit = ray_intersects_ground(radius, cos_theta);

        float ground_distance = 0.0;
        float3 transmittance_to_ground = 0.0.xxx;
        if (ground_hit)
        {
            ground_distance = distance_to_bottom_boundary(radius, cos_theta);
            transmittance_to_ground = get_transmittance(
                radius,
                cos_theta,
                ground_distance,
                true);
        }

        [loop]
        for (uint i = 0; i < phi_samples; ++i)
        {
            const float phi = (i + 0.5) * dphi;
            const float3 omega_i = float3(
                cos(phi) * sin_theta,
                sin(phi) * sin_theta,
                cos_theta);

            const float domega =
                dtheta * dphi * sin_theta;

            const float nu1 = dot(omega_s, omega_i);
            float3 incident_radiance = get_scattering_source(
                radius,
                omega_i.z,
                mu_s,
                nu1,
                ground_hit,
                scattering_order - 1);

            if (ground_hit)
            {
                const float3 ground_normal = normalize(
                    zenith * radius + omega_i * ground_distance);
                const float ground_mu_s = dot(ground_normal, omega_s);
                const float3 ground_irradiance = sample_irradiance(
                    g_planet_radius_km,
                    ground_mu_s);

                incident_radiance +=
                    transmittance_to_ground *
                    g_ground_albedo *
                    (1.0 / PI) *
                    ground_irradiance;
            }

            const float nu2 = dot(omega, omega_i);
            const float altitude = radius - g_planet_radius_km;
            const float rayleigh_density = get_rayleigh_density(altitude);
            const float mie_density = get_mie_density(altitude);

            sum += incident_radiance *
                (
                    g_beta_rayleigh * rayleigh_density *
                        phase_rayleigh(nu2) +
                    g_beta_mie * mie_density *
                        phase_mie(nu2)
                ) *
                domega;
        }
    }

    return sum;
}

float3 compute_multiple_scattering(
    float radius,
    float mu,
    float mu_s,
    float nu,
    bool intersects_ground)
{
    const int sample_count = 24;
    const float distance = distance_to_nearest_boundary(
        radius,
        mu,
        intersects_ground);
    const float dx = distance / sample_count;

    float3 sum = 0.0.xxx;
    [loop]
    for (int i = 0; i <= sample_count; ++i)
    {
        const float d = i * dx;
        const float r_i = clamp_radius(sqrt(
            d * d + 2.0 * radius * mu * d + radius * radius));
        const float mu_i = clamp_cosine((radius * mu + d) / r_i);
        const float mu_s_i = clamp_cosine((radius * mu_s + d * nu) / r_i);

        const float3 density = sample_scattering_3d(
            g_scattering_density,
            r_i,
            mu_i,
            mu_s_i,
            nu,
            intersects_ground);
        const float3 transmittance =
            get_transmittance(radius, mu, d, intersects_ground);
        const float weight = (i == 0 || i == sample_count) ? 0.5 : 1.0;

        sum += density * transmittance * (dx * weight);
    }

    return sum;
}

float2 sky_view_direction_uv(float2 uv)
{
    const float azimuth = (uv.x - 0.5) * TAU;
    const float elevation = (1.0 - uv.y) * (PI * 0.5);
    return float2(azimuth, elevation);
}

float3 sky_view_direction(float2 uv)
{
    const float2 angles = sky_view_direction_uv(uv);
    const float horizontal = cos(angles.y);
    return normalize(float3(
        sin(angles.x) * horizontal,
        sin(angles.y),
        cos(angles.x) * horizontal));
}

float3 evaluate_sky_radiance(float3 view_direction)
{
    const float3 sun_direction = normalize(g_sun_direction);
    const float radius = g_planet_radius_km + 0.01;
    const float mu = clamp_cosine(view_direction.y);
    const float mu_s = clamp_cosine(sun_direction.y);
    const float nu = clamp_cosine(dot(view_direction, sun_direction));

    return get_full_scattering(
        radius,
        mu,
        mu_s,
        nu,
        false);
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
    if (dispatch_id.x >= TRANSMITTANCE_WIDTH ||
        dispatch_id.y >= TRANSMITTANCE_HEIGHT)
    {
        return;
    }

    const float2 uv =
        (float2(dispatch_id.xy) + 0.5) /
        float2(TRANSMITTANCE_WIDTH, TRANSMITTANCE_HEIGHT);

    const float H = safe_sqrt(
        (g_planet_radius_km + g_atmosphere_height_km) *
        (g_planet_radius_km + g_atmosphere_height_km) -
        g_planet_radius_km * g_planet_radius_km);
    const float rho = H * get_unit_range_from_texture_coord(
        uv.y,
        TRANSMITTANCE_HEIGHT);
    const float radius = sqrt(
        rho * rho +
        g_planet_radius_km * g_planet_radius_km);

    const float x_mu = get_unit_range_from_texture_coord(
        uv.x,
        TRANSMITTANCE_WIDTH);
    const float d_min =
        g_planet_radius_km + g_atmosphere_height_km - radius;
    const float d_max = rho + H;
    const float distance = d_min + x_mu * (d_max - d_min);
    const float mu = distance <= 1.0e-6
        ? 1.0
        : clamp_cosine(
            (H * H - rho * rho - distance * distance) /
            (2.0 * radius * distance));

    float optical_rayleigh = 0.0;
    float optical_mie = 0.0;
    float optical_ozone = 0.0;

    const float3 transmittance = integrate_transmittance(
        radius,
        mu,
        optical_rayleigh,
        optical_mie,
        optical_ozone);

    g_output_2d[dispatch_id.xy] = float4(transmittance, 1.0);
}

[numthreads(8, 8, 1)]
void CS_DirectIrradiance(uint3 dispatch_id : SV_DispatchThreadID)
{
    if (dispatch_id.x >= IRRADIANCE_WIDTH ||
        dispatch_id.y >= IRRADIANCE_HEIGHT)
    {
        return;
    }

    const float2 uv =
        (float2(dispatch_id.xy) + 0.5) /
        float2(IRRADIANCE_WIDTH, IRRADIANCE_HEIGHT);
    const float mu_s =
        get_unit_range_from_texture_coord(uv.x, IRRADIANCE_WIDTH) * 2.0 - 1.0;
    const float radius =
        g_planet_radius_km +
        get_unit_range_from_texture_coord(uv.y, IRRADIANCE_HEIGHT) *
        g_atmosphere_height_km;

    g_output_2d[dispatch_id.xy] =
        float4(compute_direct_irradiance(radius, mu_s), 1.0);
}

[numthreads(4, 4, 4)]
void CS_SingleScattering(uint3 dispatch_id : SV_DispatchThreadID)
{
    if (dispatch_id.x >= SCATTERING_WIDTH ||
        dispatch_id.y >= SCATTERING_HEIGHT ||
        dispatch_id.z >= SCATTERING_DEPTH)
    {
        return;
    }

    float radius = 0.0;
    float mu = 0.0;
    float mu_s = 0.0;
    float nu = 0.0;
    bool intersects_ground = false;

    decode_scattering_parameters(
        float3(dispatch_id) + 0.5,
        radius,
        mu,
        mu_s,
        nu,
        intersects_ground);

    float3 mie = 0.0.xxx;
    const float3 rayleigh = compute_single_scattering(
        radius,
        mu,
        mu_s,
        nu,
        intersects_ground,
        mie);

    g_output_3d[dispatch_id] = float4(rayleigh, 1.0);
    g_output_3d_b[dispatch_id] = float4(mie, 1.0);
}

[numthreads(4, 4, 4)]
void CS_ScatteringDensity(uint3 dispatch_id : SV_DispatchThreadID)
{
    if (dispatch_id.x >= SCATTERING_WIDTH ||
        dispatch_id.y >= SCATTERING_HEIGHT ||
        dispatch_id.z >= SCATTERING_DEPTH)
    {
        return;
    }

    float radius = 0.0;
    float mu = 0.0;
    float mu_s = 0.0;
    float nu = 0.0;
    bool intersects_ground = false;

    decode_scattering_parameters(
        float3(dispatch_id) + 0.5,
        radius,
        mu,
        mu_s,
        nu,
        intersects_ground);

    const float3 density = compute_scattering_density(
        radius,
        mu,
        mu_s,
        nu,
        intersects_ground,
        g_scattering_order);

    g_output_3d[dispatch_id] = float4(density, 1.0);
}

[numthreads(4, 4, 4)]
void CS_MultipleScattering(uint3 dispatch_id : SV_DispatchThreadID)
{
    if (dispatch_id.x >= SCATTERING_WIDTH ||
        dispatch_id.y >= SCATTERING_HEIGHT ||
        dispatch_id.z >= SCATTERING_DEPTH)
    {
        return;
    }

    float radius = 0.0;
    float mu = 0.0;
    float mu_s = 0.0;
    float nu = 0.0;
    bool intersects_ground = false;

    decode_scattering_parameters(
        float3(dispatch_id) + 0.5,
        radius,
        mu,
        mu_s,
        nu,
        intersects_ground);

    g_output_3d[dispatch_id] = float4(
        compute_multiple_scattering(
            radius,
            mu,
            mu_s,
            nu,
            intersects_ground),
        1.0);
}

[numthreads(8, 8, 1)]
void CS_IndirectIrradiance(uint3 dispatch_id : SV_DispatchThreadID)
{
    if (dispatch_id.x >= IRRADIANCE_WIDTH ||
        dispatch_id.y >= IRRADIANCE_HEIGHT)
    {
        return;
    }

    const float2 uv =
        (float2(dispatch_id.xy) + 0.5) /
        float2(IRRADIANCE_WIDTH, IRRADIANCE_HEIGHT);
    const float mu_s =
        get_unit_range_from_texture_coord(uv.x, IRRADIANCE_WIDTH) * 2.0 - 1.0;
    const float radius =
        g_planet_radius_km +
        get_unit_range_from_texture_coord(uv.y, IRRADIANCE_HEIGHT) *
        g_atmosphere_height_km;

    const uint theta_samples = max(g_angular_theta_samples, 2);
    const uint phi_samples = max(g_angular_phi_samples, 4);
    const float dtheta = (PI * 0.5) / theta_samples;
    const float dphi = TAU / phi_samples;
    const float3 sun_direction = float3(
        safe_sqrt(1.0 - mu_s * mu_s),
        0.0,
        mu_s);

    float3 irradiance = 0.0.xxx;

    [loop]
    for (uint j = 0; j < theta_samples; ++j)
    {
        const float theta = (j + 0.5) * dtheta;
        const float sin_theta = sin(theta);
        const float cos_theta = cos(theta);

        [loop]
        for (uint i = 0; i < phi_samples; ++i)
        {
            const float phi = (i + 0.5) * dphi;
            const float3 omega = float3(
                cos(phi) * sin_theta,
                sin(phi) * sin_theta,
                cos_theta);
            const float nu = dot(omega, sun_direction);
            const float3 incident = get_scattering_source(
                radius,
                omega.z,
                mu_s,
                nu,
                false,
                g_scattering_order);

            irradiance += incident *
                cos_theta *
                sin_theta *
                dtheta *
                dphi;
        }
    }

    g_output_2d[dispatch_id.xy] = float4(irradiance, 1.0);
}

[numthreads(4, 4, 4)]
void CS_AccumulateScattering(uint3 dispatch_id : SV_DispatchThreadID)
{
    if (dispatch_id.x >= SCATTERING_WIDTH ||
        dispatch_id.y >= SCATTERING_HEIGHT ||
        dispatch_id.z >= SCATTERING_DEPTH)
    {
        return;
    }

    float radius = 0.0;
    float mu = 0.0;
    float mu_s = 0.0;
    float nu = 0.0;
    bool intersects_ground = false;

    decode_scattering_parameters(
        float3(dispatch_id) + 0.5,
        radius,
        mu,
        mu_s,
        nu,
        intersects_ground);

    const float3 previous = g_scattering_sum.Load(int4(dispatch_id, 0)).rgb;
    const float3 multiple = g_scattering_source.Load(int4(dispatch_id, 0)).rgb;
    const float3 result = previous +
        multiple / max(phase_rayleigh(nu), 1.0e-6);

    g_output_3d[dispatch_id] = float4(result, 1.0);
}

[numthreads(8, 8, 1)]
void CS_AccumulateIrradiance(uint3 dispatch_id : SV_DispatchThreadID)
{
    if (dispatch_id.x >= IRRADIANCE_WIDTH ||
        dispatch_id.y >= IRRADIANCE_HEIGHT)
    {
        return;
    }

    const float3 previous =
        g_irradiance_source.Load(int3(dispatch_id.xy, 0)).rgb;
    const float3 delta =
        g_sky_current.Load(int3(dispatch_id.xy, 0)).rgb;

    g_output_2d[dispatch_id.xy] = float4(previous + delta, 1.0);
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

    g_output_2d[dispatch_id.xy] = float4(radiance, 1.0);
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

    g_output_2d[coordinate] = float4(
        lerp(a, b, saturate(g_blend_factor)),
        1.0);
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
