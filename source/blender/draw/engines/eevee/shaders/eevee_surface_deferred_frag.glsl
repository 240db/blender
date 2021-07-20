
#pragma BLENDER_REQUIRE(common_view_lib.glsl)
#pragma BLENDER_REQUIRE(common_math_lib.glsl)
#pragma BLENDER_REQUIRE(eevee_surface_lib.glsl)
#pragma BLENDER_REQUIRE(eevee_bsdf_lib.glsl)
#pragma BLENDER_REQUIRE(eevee_gbuffer_lib.glsl)
#pragma BLENDER_REQUIRE(eevee_nodetree_eval_lib.glsl)
#pragma BLENDER_REQUIRE(eevee_sampling_lib.glsl)
#pragma BLENDER_REQUIRE(eevee_shader_shared.hh)

layout(std140) uniform sampling_block
{
  SamplingData sampling;
};

uniform sampler2DArray utility_tx;

utility_tx_fetch_define(utility_tx)
utility_tx_sample_define(utility_tx)

/* Diffuse or Transmission Color. */
layout(location = 0) out vec3 out_transmit_color;
/* RG: Normal (negative if Tranmission), B: SSS ID, A: Min-Thickness */
layout(location = 1) out vec4 out_transmit_normal;
/* RGB: SSS RGB Radius.
 * or
 * R: Transmission IOR, G: Transmission Roughness, B: Unused. */
layout(location = 2) out vec3 out_transmit_data;
/* Reflection Color. */
layout(location = 3) out vec3 out_reflection_color;
/* RG: Normal, B: Roughness X, A: Roughness Y. */
layout(location = 4) out vec4 out_reflection_normal;
/* Volume Emission, Absorption, Scatter, Phase. */
layout(location = 5) out uvec4 out_volume_data;
/* Emission. */
layout(location = 6) out vec3 out_emission_data;
/* Transparent BSDF, Holdout. */
layout(location = 7) out vec4 out_transparency_data;

void main(void)
{
  g_data = init_globals();

  float noise_offset = sampling_rng_1D_get(sampling, SAMPLING_CLOSURE);
  float noise = utility_tx_fetch(gl_FragCoord.xy, UTIL_BLUE_NOISE_LAYER).r;
  g_data.closure_rand = fract(noise + noise_offset);
  /* TODO(fclem) other RNG. */
  g_data.transmit_rand = fract(g_data.closure_rand * 6.1803398875);

  nodetree_surface();

  /* TODO(fclem) Textured minimal thickness. */
  // nodetree_thickness();

  float alpha = saturate(1.0 - avg(g_transparency_data.transmittance));

  if (alpha > 0.0) {
    g_diffuse_data.color /= alpha;
    g_reflection_data.color /= alpha;
    g_refraction_data.color /= alpha;
    g_emission_data.emission /= alpha;
  }

  if (gl_FrontFacing) {
    g_refraction_data.ior = safe_rcp(g_refraction_data.ior);
  }

  if (g_data.transmit_rand == 0.0) {
    out_transmit_color = g_refraction_data.color;
    out_transmit_normal.xy = -gbuffer_encode_normal(g_refraction_data.N);
    out_transmit_normal.z = 1.0;
    // out_transmit_normal.w = g_data.thickness;
    out_transmit_data.x = g_refraction_data.ior;
    out_transmit_data.y = g_refraction_data.roughness;
  }
  else {
    /* Output diffuse / SSS in transmit data. */
    out_transmit_color = g_diffuse_data.color;
    out_transmit_normal.xy = gbuffer_encode_normal(g_diffuse_data.N);
    out_transmit_normal.z = fract(float(g_diffuse_data.sss_id) / 1024.0);
    // out_transmit_normal.w = g_data.thickness;
    out_transmit_data = g_diffuse_data.sss_radius;
  }

  {
    out_reflection_color = g_reflection_data.color;
    out_reflection_normal.xy = gbuffer_encode_normal(g_reflection_data.N);
    out_reflection_normal.z = g_reflection_data.roughness;
  }

  out_volume_data = gbuffer_store_volume_data(g_volume_data);
  out_emission_data = gbuffer_store_emission_data(g_emission_data);
  out_transparency_data = gbuffer_store_transparency_data(g_transparency_data);
}
