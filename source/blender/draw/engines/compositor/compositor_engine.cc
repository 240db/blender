/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Copyright 2021, Blender Foundation.
 */

/** \file
 * \ingroup draw_engine
 *
 * Engine processing the render buffer using GLSL to apply the scene compositing node tree.
 */

#include "DRW_render.h"

#include "compositor_shader.hh"

namespace blender::compositor {

static ShaderModule *g_shader_module = nullptr;

class Instance {
 public:
  ShaderModule &shaders;

 private:
  /** TODO(fclem) multipass. */
  DRWPass *pass_;
  GPUMaterial *gpumat_;
  /** Temp buffers to hold intermediate results or the input color. */
  GPUTexture *tmp_buffer_;

  bool enabled_;

 public:
  Instance(ShaderModule &shader_module) : shaders(shader_module){};

  void init()
  {
    const DRWContextState *ctx_state = DRW_context_state_get();
    Scene *scene = ctx_state->scene;
    enabled_ = scene->use_nodes && scene->nodetree;

    if (!enabled_) {
      return;
    }

    gpumat_ = shaders.material_get(scene);
    enabled_ = GPU_material_status(gpumat_) == GPU_MAT_SUCCESS;

    if (!enabled_) {
      return;
    }

    /* Create temp double buffer to render to or copy source to. */
    /* TODO(fclem) with multipass compositing we might need more than one temp buffer. */
    DrawEngineType *owner = (DrawEngineType *)this;
    eGPUTextureFormat format = GPU_texture_format(DRW_viewport_texture_list_get()->color);
    tmp_buffer_ = DRW_texture_pool_query_fullscreen(format, owner);
  }

  void sync()
  {
    if (!enabled_) {
      return;
    }

    pass_ = DRW_pass_create("Compositing", DRW_STATE_WRITE_COLOR);
    DRWShadingGroup *grp = DRW_shgroup_material_create(gpumat_, pass_);
    /* TODO(fclem) Find a way to identify the renderlayers. */
    DRW_shgroup_uniform_texture_ex(grp, "samp0", tmp_buffer_, GPU_SAMPLER_FILTER);

    DRW_shgroup_call_procedural_triangles(grp, nullptr, 1);
  }

  void draw()
  {
    if (!enabled_) {
      return;
    }

    DefaultFramebufferList *dfbl = DRW_viewport_framebuffer_list_get();
    DefaultTextureList *dtxl = DRW_viewport_texture_list_get();
    /* TODO(fclem) only copy if we need to. Only possible in multipass. */
    GPU_texture_copy(tmp_buffer_, dtxl->color);

    GPU_framebuffer_bind(dfbl->color_only_fb);
    DRW_draw_pass(pass_);
  }
};

}  // namespace blender::compositor

/* -------------------------------------------------------------------- */
/** \name C interface
 * \{ */

using namespace blender::compositor;

typedef struct COMPOSITOR_Data {
  DrawEngineType *engine_type;
  DRWViewportEmptyList *fbl;
  DRWViewportEmptyList *txl;
  DRWViewportEmptyList *psl;
  DRWViewportEmptyList *stl;
  Instance *instance_data;
} COMPOSITOR_Data;

static void compositor_engine_init(void *vedata)
{
  COMPOSITOR_Data *ved = (COMPOSITOR_Data *)vedata;

  if (g_shader_module == nullptr) {
    /* TODO(fclem) threadsafety. */
    g_shader_module = new ShaderModule();
  }

  if (ved->instance_data == nullptr) {
    ved->instance_data = new Instance(*g_shader_module);
  }

  ved->instance_data->init();
}

static void compositor_engine_free(void)
{
  delete g_shader_module;
  g_shader_module = nullptr;
}

static void compositor_instance_free(void *instance_data_)
{
  Instance *instance_data = reinterpret_cast<Instance *>(instance_data_);
  delete instance_data;
}

static void compositor_cache_init(void *vedata)
{
  COMPOSITOR_Data *ved = (COMPOSITOR_Data *)vedata;
  ved->instance_data->sync();
}

static void compositor_draw(void *vedata)
{
  COMPOSITOR_Data *ved = (COMPOSITOR_Data *)vedata;
  ved->instance_data->draw();
}

/** \} */

extern "C" {

static const DrawEngineDataSize compositor_data_size = DRW_VIEWPORT_DATA_SIZE(COMPOSITOR_Data);

DrawEngineType draw_engine_compositor_type = {
    nullptr,
    nullptr,
    N_("Compositor"),
    &compositor_data_size,
    &compositor_engine_init,
    &compositor_engine_free,
    &compositor_instance_free,
    &compositor_cache_init,
    nullptr,
    nullptr,
    &compositor_draw,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};
}
