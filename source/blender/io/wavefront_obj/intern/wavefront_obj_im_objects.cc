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

#include "BKE_collection.h"
#include "BKE_context.h"
#include "BKE_customdata.h"
#include "BKE_mesh.h"
#include "BKE_object.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "DNA_scene_types.h"

#include "wavefront_obj_im_objects.hh"

namespace blender::io::obj {

eGeometryType Geometry::get_geom_type() const
{
  return geom_type_;
}

/**
 * Use very rarely. Only when it is guaranteed that the
 * type originally set is wrong.
 */
void Geometry::set_geom_type(const eGeometryType new_type)
{
  geom_type_ = new_type;
}

std::string_view Geometry::get_geometry_name() const
{
  return geometry_name_;
}

void Geometry::set_geometry_name(std::string_view new_name)
{
  geometry_name_ = std::string(new_name);
}

/**
 * Return the vertex index ranging from zero to total vertices in a Geometry instance.
 * Key ranges from zero to total vertices in an OBJ file.
 * To be used for mloop->v only.
 */
int Geometry::vertex_indices_lookup(const int key) const
{
  BLI_assert(vertex_indices_.contains(key));
  return vertex_indices_.lookup(key);
}

int64_t Geometry::tot_verts() const
{
  return vertex_indices_.size();
}

Span<FaceElement> Geometry::face_elements() const
{
  return face_elements_;
}

int64_t Geometry::tot_face_elems() const
{
  return face_elements_.size();
}

bool Geometry::use_vertex_groups() const
{
  return use_vertex_groups_;
}

Span<MEdge> Geometry::edges() const
{
  return edges_;
}

int64_t Geometry::tot_edges() const
{
  return edges_.size();
}

int Geometry::tot_loops() const
{
  return tot_loops_;
}

int Geometry::tot_normals() const
{
  return tot_normals_;
}

const NurbsElement &Geometry::nurbs_elem() const
{
  return nurbs_element_;
}

const std::string &Geometry::group() const
{
  return nurbs_element_.group_;
}

/**
 * Return a reference to the texture map corresponding to the given ID
 * Caller must ensure that the lookup key given exists in the Map.
 */
tex_map_XX &MTLMaterial::tex_map_of_type(StringRef map_string)
{
  return texture_maps.lookup_as(map_string);
}

/**
 * Create a collection to store all imported objects.
 */
OBJImportCollection::OBJImportCollection(Main *bmain, Scene *scene) : bmain_(bmain), scene_(scene)
{
  obj_import_collection_ = BKE_collection_add(
      bmain_, scene_->master_collection, "OBJ import collection");
}

/**
 * Add the given Mesh/Curve object to the OBJ import collection.
 */
void OBJImportCollection::add_object_to_collection(unique_object_ptr b_object)
{
  BKE_collection_object_add(bmain_, obj_import_collection_, b_object.release());
  id_fake_user_set(&obj_import_collection_->id);
  DEG_id_tag_update(&obj_import_collection_->id, ID_RECALC_COPY_ON_WRITE);
  DEG_relations_tag_update(bmain_);
}

}  // namespace blender::io::obj
