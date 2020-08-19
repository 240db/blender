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

/** \file
 * \ingroup pythonintern
 *
 * This file wraps around pythons stdin stdout and exposes it in buffer
 */

#include <Python.h>
#include <stdbool.h>

#include <BLI_assert.h>

#include "bpy_interface_inoutwrapper.h"

PyObject *string_io_mod = NULL;
PyObject *string_io = NULL;
PyObject *stdout_backup = NULL;
PyObject *stderr_backup = NULL;
PyObject *string_io_buf = NULL;
PyObject *string_io_getvalue = NULL;

bool BPY_intern_init_io_wrapper()
{
  BLI_assert(string_io_mod == NULL);
  BLI_assert(string_io == NULL);
  BLI_assert(stderr_backup == NULL);
  BLI_assert(stdout_backup == NULL);
  BLI_assert(string_io_buf == NULL);
  BLI_assert(string_io_getvalue == NULL);
  PyImport_ImportModule("sys");
  stdout_backup = PySys_GetObject("stdout"); /* borrowed */
  stderr_backup = PySys_GetObject("stderr"); /* borrowed */

  if (!(string_io_mod = PyImport_ImportModule("io"))) {
    return false;
  }
  if (!(string_io = PyObject_CallMethod(string_io_mod, "StringIO", NULL))) {
    return false;
  }
  if (!(string_io_getvalue = PyObject_GetAttrString(string_io, "getvalue"))) {
    return false;
  }
  Py_INCREF(stdout_backup);  // since these were borrowed we don't want them freed when replaced.
  Py_INCREF(stderr_backup);

  if (PySys_SetObject("stdout", string_io) == -1) {
    return false;
  }
  if (PySys_SetObject("stderr", string_io) == -1) {
    return false;
  }

  return true;
}

PyObject *BPY_intern_get_io_buffer()
{
  BLI_assert(string_io_getvalue != NULL);
  string_io_buf = PyObject_CallObject(string_io_getvalue, NULL);
  return string_io_buf;
}

void BPY_intern_free_io_twrapper()
{
  Py_DECREF(stdout_backup);
  Py_DECREF(stderr_backup);
  Py_DECREF(string_io_mod);
  Py_DECREF(string_io_getvalue);
  Py_DECREF(string_io);
  stdout_backup = NULL;
  stderr_backup = NULL;
  string_io_buf = NULL;
  string_io_getvalue = NULL;
  string_io_mod = NULL;
  string_io = NULL;
}
