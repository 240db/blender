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
 * The Original Code is Copyright (C) 2008 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup obj
 */
#ifndef __WAVEFRONT_OBJ_FILE_HANDLER_HH__
#define __WAVEFRONT_OBJ_FILE_HANDLER_HH__

#include "wavefront_obj.hh"

namespace io {
namespace obj {

/**
 * Low level writer to the OBJ file at filepath.
 */
void write_object_fprintf(const char *filepath,
                          std::vector<OBJ_object_to_export> &object_to_export);

}  // namespace obj
}  // namespace io
#endif
