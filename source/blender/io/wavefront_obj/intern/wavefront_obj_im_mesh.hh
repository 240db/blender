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
 * The Original Code is Copyright (C) 2020 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup obj
 */

#pragma once

#include "BKE_lib_id.h"

#include "BLI_utility_mixins.hh"

#include "wavefront_obj_im_mtl.hh"
#include "wavefront_obj_im_objects.hh"

namespace blender::io::obj {
/**
 * An custom unique_ptr deleter for a Mesh object.
 */
struct UniqueMeshDeleter {
  void operator()(Mesh *mesh)
  {
    BKE_id_free(nullptr, mesh);
  }
};

/**
 * An unique_ptr to a Mesh with a custom deleter.
 */
using unique_mesh_ptr = std::unique_ptr<Mesh, UniqueMeshDeleter>;

/**
 * Make a Blender Mesh Object from a Geometry of GEOM_MESH type.
 * Use the mover function to own the mesh after creation.
 */
class MeshFromGeometry : NonMovable, NonCopyable {
 private:
  /**
   * Mesh datablock made from OBJ data.
   */
  unique_mesh_ptr blender_mesh_{nullptr};
  /**
   * An Object of type OB_MESH. Use the mover function to own it.
   */
  unique_object_ptr mesh_object_{nullptr};
  const Geometry &mesh_geometry_;
  const GlobalVertices &global_vertices_;

 public:
  MeshFromGeometry(const Geometry &mesh_geometry, const GlobalVertices &global_vertices)
      : mesh_geometry_(mesh_geometry), global_vertices_(global_vertices)
  {
  }

  ~MeshFromGeometry();
  void create_mesh(Main *bmain,
                   const Map<std::string, std::unique_ptr<MTLMaterial>> &materials,
                   const OBJImportParams &import_params);
  unique_object_ptr mover()
  {
    return std::move(mesh_object_);
  }

 private:
  std::pair<int64_t, int64_t> tessellate_polygons(Vector<FaceElement> &new_faces,
                                                  Set<std::pair<int, int>> &fgon_edges);
  void create_vertices();
  void create_polys_loops(Span<FaceElement> all_faces);
  void create_edges();
  void create_uv_verts();
  void create_materials(Main *bmain,
                        const Map<std::string, std::unique_ptr<MTLMaterial>> &materials);
  void add_custom_normals();
  void dissolve_edges(const Set<std::pair<int, int>> &fgon_edges);
};

}  // namespace blender::io::obj
