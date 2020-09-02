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

#include "BKE_image.h"
#include "BKE_node.h"

#include "BLI_map.hh"

#include "DNA_node_types.h"

#include "NOD_shader.h"

#include "obj_import_mtl.hh"

namespace blender::io::obj {

/**
 * Set the socket's (of given ID) value to the given number(s).
 * Only float value(s) can be set using this method.
 */
static void set_property_of_socket(eNodeSocketDatatype property_type,
                                   StringRef socket_id,
                                   Span<float> value,
                                   bNode *r_node)
{
  BLI_assert(r_node);
  bNodeSocket *socket{nodeFindSocket(r_node, SOCK_IN, socket_id.data())};
  BLI_assert(socket && socket->type == property_type);
  switch (property_type) {
    case SOCK_FLOAT: {
      BLI_assert(value.size() == 1);
      static_cast<bNodeSocketValueFloat *>(socket->default_value)->value = value[0];
      break;
    }
    case SOCK_RGBA: {
      /* Alpha will be added manually. It is not read from the MTL file either. */
      BLI_assert(value.size() == 3);
      copy_v3_v3(static_cast<bNodeSocketValueRGBA *>(socket->default_value)->value, value.data());
      static_cast<bNodeSocketValueRGBA *>(socket->default_value)->value[3] = 1.0f;
      break;
    }
    case SOCK_VECTOR: {
      BLI_assert(value.size() == 3);
      copy_v4_v4(static_cast<bNodeSocketValueVector *>(socket->default_value)->value,
                 value.data());
      break;
    }
    default: {
      BLI_assert(0);
      break;
    }
  }
}

static std::string replace_all_occurences(StringRef path, StringRef to_remove, StringRef to_add)
{
  std::string clean_path{path};
  while (true) {
    std::string::size_type pos = clean_path.find(to_remove);
    if (pos == std::string::npos) {
      break;
    }
    clean_path.replace(pos, to_add.size(), to_add);
  }
  return clean_path;
}

/**
 * Load image for Image Texture node and set the node properties.
 * Return success if Image can be loaded successfully.
 */
static bool load_texture_image(Main *bmain, const tex_map_XX &tex_map, bNode *r_node)
{
  BLI_assert(r_node && r_node->type == SH_NODE_TEX_IMAGE);

  std::string tex_file_path{tex_map.mtl_dir_path + tex_map.image_path};
  Image *tex_image = BKE_image_load(bmain, tex_file_path.c_str());
  if (!tex_image) {
    /* Could be absolute, so load the image directly. */
    fprintf(stderr, "Cannot load image file:'%s'\n", tex_file_path.c_str());
    tex_image = BKE_image_load(bmain, tex_map.image_path.c_str());
  }
  if (!tex_image) {
    fprintf(stderr, "Cannot load image file:'%s'\n", tex_map.image_path.c_str());
    /* Remove quotes from the filepath. */
    std::string no_quote_path{tex_map.mtl_dir_path +
                              replace_all_occurences(tex_map.image_path, "\"", "")};
    tex_image = BKE_image_load(nullptr, no_quote_path.c_str());
    if (!tex_image) {
      fprintf(stderr, "Cannot load image file:'%s'\n", no_quote_path.data());
      std::string no_underscore_path{replace_all_occurences(no_quote_path, "_", " ")};
      tex_image = BKE_image_load(nullptr, no_underscore_path.c_str());
      if (!tex_image) {
        fprintf(stderr, "Cannot load image file:'%s'\n", no_underscore_path.data());
      }
    }
  }
  BLI_assert(tex_image);
  if (tex_image) {
    fprintf(stderr, "Loaded image from:'%s'\n", tex_image->filepath);
    r_node->id = reinterpret_cast<ID *>(tex_image);
    NodeTexImage *image = static_cast<NodeTexImage *>(r_node->storage);
    image->projection = tex_map.projection_type;
    return true;
  }
  return false;
}

/**
 * Initialises a nodetree with a p-BSDF node's BSDF socket connected to shader output node's
 * surface socket.
 */
ShaderNodetreeWrap::ShaderNodetreeWrap(Main *bmain, const MTLMaterial &mtl_mat)
    : mtl_mat_(&mtl_mat)
{
  nodetree_.reset(ntreeAddTree(nullptr, "Shader Nodetree", ntreeType_Shader->idname));
  bsdf_.reset(add_node_to_tree(SH_NODE_BSDF_PRINCIPLED));
  shader_output_.reset(add_node_to_tree(SH_NODE_OUTPUT_MATERIAL));

  set_bsdf_socket_values();
  add_image_textures(bmain);
  link_sockets(std::move(bsdf_), "BSDF", shader_output_.get(), "Surface", 4);

  nodeSetActive(nodetree_.get(), shader_output_.get());
}

/**
 * Assert if caller hasn't acquired nodetree.
 */
ShaderNodetreeWrap::~ShaderNodetreeWrap()
{
  if (nodetree_) {
    /* nodetree's ownership must be acquired by the caller. */
    nodetree_.reset();
    BLI_assert(0);
  }
}

/**
 * Release nodetree for materials to own it. nodetree has its unique deleter
 * if destructor is not reached for some reason.
 */
bNodeTree *ShaderNodetreeWrap::get_nodetree()
{
  /* If this function has been reached, we know that nodes and the nodetree
   * can be added to the scene safely. */
  static_cast<void>(shader_output_.release());
  return nodetree_.release();
}

/**
 * Add a new static node to the tree.
 * No two nodes are linked here.
 */
bNode *ShaderNodetreeWrap::add_node_to_tree(const int node_type)
{
  return nodeAddStaticNode(nullptr, nodetree_.get(), node_type);
}

/**
 * Return x-y coordinates for a node where y is determined by other nodes present in
 * the same vertical column.
 */
std::tuple<float, float> ShaderNodetreeWrap::set_node_locations(const int pos_x)
{
  int pos_y = 0;
  bool found = false;
  while (true) {
    for (Span<int> location : node_locations) {
      if (location[0] == pos_x && location[1] == pos_y) {
        pos_y += 1;
        found = true;
      }
      else {
        found = false;
      }
    }
    if (!found) {
      node_locations.append({pos_x, pos_y});
      return std::make_tuple(pos_x * node_size, pos_y * node_size * 2 / 3);
    }
  }
}

/**
 * Link two nodes by the sockets of given IDs.
 * Also releases the ownership of the "from" node for nodetree to free it.
 * \param from_node_pos_x 0 to 4 value as per nodetree arrangement.
 */
void ShaderNodetreeWrap::link_sockets(unique_node_ptr from_node,
                                      StringRef from_node_id,
                                      bNode *to_node,
                                      StringRef to_node_id,
                                      const int from_node_pos_x)
{
  std::tie(from_node->locx, from_node->locy) = set_node_locations(from_node_pos_x);
  std::tie(to_node->locx, to_node->locy) = set_node_locations(from_node_pos_x + 1);
  bNodeSocket *from_sock{nodeFindSocket(from_node.get(), SOCK_OUT, from_node_id.data())};
  bNodeSocket *to_sock{nodeFindSocket(to_node, SOCK_IN, to_node_id.data())};
  BLI_assert(from_sock && to_sock);
  nodeAddLink(nodetree_.get(), from_node.get(), from_sock, to_node, to_sock);
  static_cast<void>(from_node.release());
}

/**
 * Set values of sockets in p-BSDF node of the nodetree.
 */
void ShaderNodetreeWrap::set_bsdf_socket_values()
{
  const float specular_exponent{1 - sqrt(mtl_mat_->Ns) / 30};
  set_property_of_socket(SOCK_FLOAT, "Roughness", {specular_exponent}, bsdf_.get());
  /* Only one value is taken for Metallic and Specular. */
  set_property_of_socket(SOCK_FLOAT, "Specular", {mtl_mat_->Ks[0]}, bsdf_.get());
  set_property_of_socket(SOCK_FLOAT, "Metallic", {mtl_mat_->Ka[0]}, bsdf_.get());
  set_property_of_socket(SOCK_FLOAT, "IOR", {mtl_mat_->Ni}, bsdf_.get());
  set_property_of_socket(SOCK_FLOAT, "Alpha", {mtl_mat_->d}, bsdf_.get());
  set_property_of_socket(SOCK_RGBA, "Base Color", {mtl_mat_->Kd, 3}, bsdf_.get());
  set_property_of_socket(SOCK_RGBA, "Emission", {mtl_mat_->Ke, 3}, bsdf_.get());
}

/**
 * Create image texture, vector and normal mapping nodes from MTL materials and link the
 * nodes to p-BSDF node.
 */
void ShaderNodetreeWrap::add_image_textures(Main *bmain)
{
  for (const Map<const std::string, tex_map_XX>::Item texture_map :
       mtl_mat_->texture_maps.items()) {
    if (texture_map.value.image_path.empty()) {
      /* No Image texture node of this map type can be added to this material. */
      continue;
    }

    unique_node_ptr image_texture{add_node_to_tree(SH_NODE_TEX_IMAGE)};
    unique_node_ptr mapping{add_node_to_tree(SH_NODE_MAPPING)};
    unique_node_ptr texture_coordinate(add_node_to_tree(SH_NODE_TEX_COORD));
    unique_node_ptr normal_map = nullptr;

    if (texture_map.key == "map_Bump") {
      normal_map.reset(add_node_to_tree(SH_NODE_NORMAL_MAP));
      set_property_of_socket(
          SOCK_FLOAT, "Strength", {mtl_mat_->map_Bump_strength}, normal_map.get());
    }

    if (!load_texture_image(bmain, texture_map.value, image_texture.get())) {
      /* Image could not be added, so don't link image texture, vector, normal map nodes. */
      continue;
    }
    set_property_of_socket(
        SOCK_VECTOR, "Location", {texture_map.value.translation, 3}, mapping.get());
    set_property_of_socket(SOCK_VECTOR, "Scale", {texture_map.value.scale, 3}, mapping.get());

    link_sockets(std::move(texture_coordinate), "UV", mapping.get(), "Vector", 0);
    link_sockets(std::move(mapping), "Vector", image_texture.get(), "Vector", 1);
    if (normal_map) {
      link_sockets(std::move(image_texture), "Color", normal_map.get(), "Color", 2);
      link_sockets(std::move(normal_map), "Normal", bsdf_.get(), "Normal", 3);
    }
    else {
      link_sockets(
          std::move(image_texture), "Color", bsdf_.get(), texture_map.value.dest_socket_id, 2);
    }
  }
}
}  // namespace blender::io::obj
