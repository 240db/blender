/*
 * Copyright 2011-2013 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __FILM_H__
#define __FILM_H__

#include "util/util_string.h"
#include "util/util_vector.h"

#include "kernel/kernel_types.h"

#include "graph/node.h"

CCL_NAMESPACE_BEGIN

class Device;
class DeviceScene;
class Scene;

typedef enum FilterType {
  FILTER_BOX,
  FILTER_GAUSSIAN,
  FILTER_BLACKMAN_HARRIS,

  FILTER_NUM_TYPES,
} FilterType;

class Pass : public Node {
 public:
  NODE_DECLARE

  Pass();

  PassType type;
  int components;
  bool filter;
  bool exposure;
  PassType divide_type;
  ustring name;

  /* The has been created automatically as a requirement to various rendering functionality (such
   * as adaptive sampling). */
  bool is_auto;

  /* Is true when the actual storage of the pass is not aligned to any of boundary.
   * For example, if the pass with 3 components is stored (and written by the kernel) as individual
   * float components. */
  bool is_unaligned;

  static void add(PassType type,
                  vector<Pass> &passes,
                  const char *name = nullptr,
                  bool is_auto = false);

  /* Check whether two sets of passes are matching exactly. */
  static bool equals_exact(const vector<Pass> &A, const vector<Pass> &B);

  /* Check whether two sets of passes define same set of non-auto passes. */
  static bool equals_no_auto(const vector<Pass> &A, const vector<Pass> &B);

  static bool contains(const vector<Pass> &passes, PassType type);

  /* Remove given pass type if it was automatically created. */
  static void remove_auto(vector<Pass> &passes, PassType type);

  /* Remove all passes which were automatically created. */
  static void remove_all_auto(vector<Pass> &passes);
};

class Film : public Node {
 public:
  NODE_DECLARE

  NODE_SOCKET_API(float, exposure)
  NODE_SOCKET_API(float, pass_alpha_threshold)

  NODE_SOCKET_API(PassType, display_pass)

  NODE_SOCKET_API(FilterType, filter_type)
  NODE_SOCKET_API(float, filter_width)

  NODE_SOCKET_API(float, mist_start)
  NODE_SOCKET_API(float, mist_depth)
  NODE_SOCKET_API(float, mist_falloff)

  NODE_SOCKET_API(bool, use_light_visibility)
  NODE_SOCKET_API(CryptomatteType, cryptomatte_passes)
  NODE_SOCKET_API(int, cryptomatte_depth)

 private:
  int pass_stride;
  size_t filter_table_offset;

 public:
  Film();
  ~Film();

  /* add default passes to scene */
  static void add_default(Scene *scene);

  void device_update(Device *device, DeviceScene *dscene, Scene *scene);
  void device_free(Device *device, DeviceScene *dscene, Scene *scene);

  void assign_and_tag_passes_update(Scene *scene, const vector<Pass> &passes);

  int get_aov_offset(Scene *scene, string name, bool &is_color);

  int get_pass_stride() const;
  size_t get_filter_table_offset() const;
};

CCL_NAMESPACE_END

#endif /* __FILM_H__ */
