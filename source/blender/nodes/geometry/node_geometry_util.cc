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
 */

#include "node_geometry_util.hh"
#include "node_util.h"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"

#include "BKE_mesh.h"
#include "BKE_mesh_runtime.h"
#include "BKE_pointcloud.h"

#include "NOD_type_conversions.hh"

namespace blender::nodes {

using bke::GeometryInstanceGroup;

/**
 * Update the availability of a group of input sockets with the same name,
 * used for switching between attribute inputs or single values.
 *
 * \param mode: Controls which socket of the group to make available.
 * \param name_is_available: If false, make all sockets with this name unavailable.
 */
void update_attribute_input_socket_availabilities(bNode &node,
                                                  const StringRef name,
                                                  const GeometryNodeAttributeInputMode mode,
                                                  const bool name_is_available)
{
  const GeometryNodeAttributeInputMode mode_ = (GeometryNodeAttributeInputMode)mode;
  LISTBASE_FOREACH (bNodeSocket *, socket, &node.inputs) {
    if (name == socket->name) {
      const bool socket_is_available =
          name_is_available &&
          ((socket->type == SOCK_STRING && mode_ == GEO_NODE_ATTRIBUTE_INPUT_ATTRIBUTE) ||
           (socket->type == SOCK_FLOAT && mode_ == GEO_NODE_ATTRIBUTE_INPUT_FLOAT) ||
           (socket->type == SOCK_INT && mode_ == GEO_NODE_ATTRIBUTE_INPUT_INTEGER) ||
           (socket->type == SOCK_VECTOR && mode_ == GEO_NODE_ATTRIBUTE_INPUT_VECTOR) ||
           (socket->type == SOCK_RGBA && mode_ == GEO_NODE_ATTRIBUTE_INPUT_COLOR));
      nodeSetSocketAvailability(socket, socket_is_available);
    }
  }
}

void prepare_field_inputs(bke::FieldInputs &field_inputs,
                          const GeometryComponent &component,
                          const AttributeDomain domain,
                          Vector<std::unique_ptr<bke::FieldInputValue>> &r_values)
{
  const int domain_size = component.attribute_domain_size(domain);
  for (const bke::FieldInputKey &key : field_inputs) {
    std::unique_ptr<bke::FieldInputValue> input_value;
    if (const bke::PersistentAttributeFieldInputKey *persistent_attribute_key =
            dynamic_cast<const bke::PersistentAttributeFieldInputKey *>(&key)) {
      const StringRef name = persistent_attribute_key->name();
      const CPPType &cpp_type = persistent_attribute_key->type();
      const CustomDataType type = bke::cpp_type_to_custom_data_type(cpp_type);
      GVArrayPtr attribute = component.attribute_get_for_read(name, domain, type);
      input_value = std::make_unique<bke::GVArrayFieldInputValue>(std::move(attribute));
    }
    else if (dynamic_cast<const bke::IndexFieldInputKey *>(&key) != nullptr) {
      auto index_func = [](int i) { return i; };
      VArrayPtr<int> index_varray = std::make_unique<VArray_For_Func<int, decltype(index_func)>>(
          domain_size, index_func);
      GVArrayPtr index_gvarray = std::make_unique<fn::GVArray_For_VArray<int>>(
          std::move(index_varray));
      input_value = std::make_unique<bke::GVArrayFieldInputValue>(std::move(index_gvarray));
    }
    else if (const bke::AnonymousAttributeFieldInputKey *anonymous_attribute_key =
                 dynamic_cast<const bke::AnonymousAttributeFieldInputKey *>(&key)) {
      const AnonymousCustomDataLayerID &layer_id = anonymous_attribute_key->layer_id();
      ReadAttributeLookup attribute = component.attribute_try_get_anonymous_for_read(layer_id);
      if (!attribute) {
        continue;
      }
      GVArrayPtr varray = std::move(attribute.varray);
      if (attribute.domain != domain) {
        varray = component.attribute_try_adapt_domain(std::move(varray), attribute.domain, domain);
      }
      const CPPType &type = anonymous_attribute_key->type();
      if (varray->type() != type) {
        const blender::nodes::DataTypeConversions &conversions = get_implicit_type_conversions();
        varray = conversions.try_convert(std::move(varray), type);
      }
      input_value = std::make_unique<bke::GVArrayFieldInputValue>(std::move(varray));
    }

    field_inputs.set_input(key, *input_value);
    r_values.append(std::move(input_value));
  }
}

}  // namespace blender::nodes

bool geo_node_poll_default(bNodeType *UNUSED(ntype),
                           bNodeTree *ntree,
                           const char **r_disabled_hint)
{
  if (!STREQ(ntree->idname, "GeometryNodeTree")) {
    *r_disabled_hint = "Not a geometry node tree";
    return false;
  }
  return true;
}

void geo_node_type_base(bNodeType *ntype, int type, const char *name, short nclass, short flag)
{
  node_type_base(ntype, type, name, nclass, flag);
  ntype->poll = geo_node_poll_default;
  ntype->update_internal_links = node_update_internal_links_default;
  ntype->insert_link = node_insert_link_default;
}
