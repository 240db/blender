# ##### BEGIN GPL LICENSE BLOCK #####
#
#  This program is free software; you can redistribute it and/or
#  modify it under the terms of the GNU General Public License
#  as published by the Free Software Foundation; either version 2
#  of the License, or (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program; if not, write to the Free Software Foundation,
#  Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
#
# ##### END GPL LICENSE BLOCK #####

# <pep8 compliant>

__all__ = (
    "draw_user_menus",
)

import bpy
from bpy.app.translations import pgettext_iface as iface_
from bpy.app.translations import contexts as i18n_contexts

def _indented_layout(layout, level):
    indentpx = 16
    if level == 0:
        level = 0.0001   # Tweak so that a percentage of 0 won't split by half
    indent = level * indentpx / bpy.context.region.width

    split = layout.split(factor=indent)
    col = split.column()
    col = split.column()
    return col

def get_keymap(context, idname, ensure):
    prefs = context.preferences
    um = prefs.user_menus

    if not idname:
        idname = um.active_group.idname

    for km in context.window_manager.keyconfigs.user.keymaps:
        for kmi in km.keymap_items:
            if kmi.idname == "wm.call_user_menu":
                if kmi.properties.name == idname:
                    return kmi
    if ensure:
        km = context.window_manager.keyconfigs.user.keymaps['Window']
        kmi = km.keymap_items.new("wm.call_user_menu",'NONE', 'ANY', shift=False, ctrl=False, alt=False)
        kmi.properties.idname = idname
        kmi.active = True
        return kmi

def draw_button(context, box, item, index):
    prefs = context.preferences
    um = prefs.user_menus

    name = item.name
    if name == "":
        name = " "
    item.is_selected = item == um.active_item
    col = box.column(align=True)
    row = col.row(align=True)
    if item.type == "SEPARATOR":
        name = "___________"
    #icons = bpy.types.UILayout.bl_rna.functions["prop"].parameters["icon"].enum_items.keys()
    #selected_icon = icons[item.icon]
    row.prop(item, "is_selected", icon=item.icon, text=name, toggle=1)
    if item.type == "SUBMENU":
        sm = item.get_submenu()
        if um.active_group.type == "PIE" and index >= 0:
            row.operator("preferences.pie_menuitem_add", text="", icon='ADD').index = index
            row.operator("preferences.menuitem_remove", text="", icon='REMOVE')
            row.operator("preferences.menuitem_up", text="", icon='TRIA_UP')
            row.operator("preferences.menuitem_down", text="", icon='TRIA_DOWN')
        sub_box = col.box()
        sub_box = sub_box.column(align=True)
        draw_item(context, sub_box, sm.items_list)

def draw_item(context, box, items):
    for umi in items:
        draw_button(context, box, umi, -1)

def draw_item_box(context, row):
    prefs = context.preferences
    um = prefs.user_menus

    box_line = row.box()
    box_col = box_line.column(align=True)

    has_item = um.has_item()
    if not has_item:
        box_col.label(text="none")
    else:
        draw_item(context, box_col, um.get_current_menu().menu_items)

    row = row.split(factor=0.9, align=True)
    col = row.column(align=True)

    col.operator("preferences.menuitem_add", text="", icon='ADD').index = -1
    col.operator("preferences.menuitem_remove", text="", icon='REMOVE')
    col.operator("preferences.menuitem_up", text="", icon='TRIA_UP')
    col.operator("preferences.menuitem_down", text="", icon='TRIA_DOWN')
    row.separator()

def draw_pie_item(context, col, items, label, index):
    row = col.row()
    row.label(text=label)
    draw_button(context, row, items, index)

def draw_pie(context, row):
    prefs = context.preferences
    um = prefs.user_menus

    cm = um.get_current_menu()
    if not cm:
        return
    col = row.column()
    draw_pie_item(context, col, cm.menu_items[0], "Left : ", 0)
    draw_pie_item(context, col, cm.menu_items[1], "Right : ", 1)
    draw_pie_item(context, col, cm.menu_items[2], "Down : ", 2)
    draw_pie_item(context, col, cm.menu_items[3], "Up : ", 3)
    draw_pie_item(context, col, cm.menu_items[4], "Upper left : ", 4)
    draw_pie_item(context, col, cm.menu_items[5], "Upper right : ", 5)
    draw_pie_item(context, col, cm.menu_items[6], "Lower left : ", 6)
    draw_pie_item(context, col, cm.menu_items[7], "Lower right : ", 7)
    row.separator()
        

def draw_item_editor(context, row):
    prefs = context.preferences
    um = prefs.user_menus

    col = row.column()

    has_item = um.has_item()
    current = um.active_item
    if not has_item:
        col.label(text="No item in this list.")
        col.label(text="Add one or choose another list to get started")
    elif current:
        col.prop(current, "type")
        if (current.type != "SEPARATOR"):
            rowsub = col.row(align=True)
            rowsub.prop(current, "icon", icon=current.icon, text="")
            col.prop(current, "name")
        if (current.type == "OPERATOR"):
            umi_op = current.get_operator()
            col.prop(umi_op, "operator")
            box = col.box()
            box.template_user_menu_item_properties(umi_op)
        if (current.type == "MENU"):
            umi_pm = current.get_menu()
            col.prop(umi_pm, "id_name", text="ID name")
        if (current.type == "PROPERTY"):
            umi_prop = current.get_property()
            col.prop(umi_prop, "id_name")
            col.prop(umi_prop, "context")
    else:
        col.label(text="No item selected.")

def draw_user_menu_preference_expanded(context, layout, kmi, map_type):
    prefs = context.preferences
    um = prefs.user_menus

    box = layout.box()
    sub = box.row()

    if kmi:
        if map_type not in {'TEXTINPUT', 'TIMER'}:
            sub = box.column()
            subrow = sub.row(align=True)

            if map_type == 'KEYBOARD':
                subrow.prop(kmi, "type", text="", event=True)
                subrow.prop(kmi, "value", text="")
                subrow_repeat = subrow.row(align=True)
                subrow_repeat.active = kmi.value in {'ANY', 'PRESS'}
                subrow_repeat.prop(kmi, "repeat", text="Repeat")
            elif map_type in {'MOUSE', 'NDOF'}:
                subrow.prop(kmi, "type", text="")
                subrow.prop(kmi, "value", text="")

            subrow = sub.row()
            subrow.scale_x = 0.75
            subrow.prop(kmi, "any", toggle=True)
            subrow.prop(kmi, "shift", toggle=True)
            subrow.prop(kmi, "ctrl", toggle=True)
            subrow.prop(kmi, "alt", toggle=True)
            subrow.prop(kmi, "oskey", text="Cmd", toggle=True)
            subrow.prop(kmi, "key_modifier", text="", event=True)
    else:
        sub.label(text="No key set")

def draw_user_menu_preference(context, layout):
    prefs = context.preferences
    um = prefs.user_menus
    umg = um.active_group
    kmi = get_keymap(context, None, False)
    map_type = None

    box = layout.box()
    row = box.row()

    row.prop(um, "expanded", text="", emboss=False)

    qf = um.menus[0]
    if umg != qf:
        row.prop(umg, "name")
        pie_text = "List"
        if umg.type == "PIE":
            pie_text = "Pie"
        row.prop(umg, "type", text=pie_text, expand=True)
    if kmi:
        row.prop(kmi, "map_type", text="")
        map_type = kmi.map_type
        if map_type == 'KEYBOARD':
            row.prop(kmi, "type", text="", full_event=True)
        elif map_type == 'MOUSE':
            row.prop(kmi, "type", text="", full_event=True)
        elif map_type == 'NDOF':
            row.prop(kmi, "type", text="", full_event=True)
        elif map_type == 'TWEAK':
            subrow = row.row()
            subrow.prop(kmi, "type", text="")
            subrow.prop(kmi, "value", text="")
        elif map_type == 'TIMER':
            row.prop(kmi, "type", text="")
        else:
            row.label()

    if um.expanded:
        draw_user_menu_preference_expanded(context=context, layout=box, kmi=kmi, map_type=map_type)


def menu_id(context, umg):
    prefs = context.preferences
    um = prefs.user_menus

    i = 0
    for item in um.menus:
        if item == umg:
            return i
        i = i + 1
    return -1


def draw_user_menus(context, layout):
    prefs = context.preferences
    um = prefs.user_menus

    if not um.active_group:
        um.active_group = um.menus[0]

    split = layout.split(factor=0.4)

    row = split.row()

    rowsub = row.row(align=True)
    rowsub.menu("USERPREF_MT_menu_select", text=um.active_group.name)
    rowsub.operator("preferences.usermenus_add", text="", icon='ADD')
    rowsub.operator("preferences.usermenus_remove", text="", icon='REMOVE')

    rowsub = split.row(align=True)
    rowsub.prop(um, "space_selected", text="")

    rowsub = split.row(align=True)
    rowsub.prop(um, "context_selected", text="")

    #rowsub = split.row(align=True)
    #rowsub.operator("preferences.keyconfig_import", text="", icon='IMPORT')
    #rowsub.operator("preferences.keyconfig_export", text="", icon='EXPORT')

    row = layout.row()
    row.separator()

    draw_user_menu_preference(context=context, layout=row)

    row = layout.row()
    row.separator()

    if um.active_group.type == "PIE":
        draw_pie(context=context, row=row)
    else:
        draw_item_box(context=context, row=row)
    draw_item_editor(context=context, row=row)

    row.separator()

