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
 * Copyright 2020, Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "gpu_context_private.hh"

#include "vk_state.hh"

namespace blender {
namespace gpu {

class VKContext : public Context {
 public:
  /** Capabilities. */
  /** Extensions. */
  /** Workarounds. */

 public:
  VKContext(void *ghost_window){};
  ~VKContext(){};

  void activate(void) override{};
  void deactivate(void) override{};

  void flush(void) override{};
  void finish(void) override{};

  void memory_statistics_get(int *total_mem, int *free_mem) override{};

  static VKContext *get()
  {
    return static_cast<VKContext *>(Context::get());
  }

  MEM_CXX_CLASS_ALLOC_FUNCS("VKContext")
};

}  // namespace gpu
}  // namespace blender
