/*
 * widget.c - widget managing
 *
 * Copyright © 2007-2009 Julien Danjou <julien@danjou.info>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include <math.h>

#include <xcb/xcb.h>
#include <xcb/xcb_atom.h>

#include "screen.h"
#include "mouse.h"
#include "widget.h"
#include "wibox.h"
#include "client.h"
#include "common/atoms.h"
#include "common/xutil.h"

/** Collect a widget structure.
 * \param L The Lua VM state.
 * \return 0
 */
static int
luaA_widget_gc(lua_State *L)
{
    widget_t *widget = luaL_checkudata(L, 1, "widget");
    if(widget->destructor)
        widget->destructor(widget);
    button_array_wipe(&widget->buttons);
    return luaA_object_gc(L);
}

/** Delete a widget node structure.
 * \param node The node to destroy.
 */
void
widget_node_delete(widget_node_t *node)
{
    luaA_object_unref(globalconf.L, node->widget);
}

/** Get a widget node from a wibox by coords.
 * \param orientation Wibox orientation.
 * \param widgets The widget list.
 * \param width The container width.
 * \param height The container height.
 * \param x X coordinate of the widget.
 * \param y Y coordinate of the widget.
 * \return A widget.
 */
widget_t *
widget_getbycoords(orientation_t orientation, widget_node_array_t *widgets,
                   int width, int height, int16_t *x, int16_t *y)
{
    int tmp;

    /* Need to transform coordinates like it was top/bottom */
    switch(orientation)
    {
      case South:
        tmp = *y;
        *y = width - *x;
        *x = tmp;
        break;
      case North:
        tmp = *y;
        *y = *x;
        *x = height - tmp;
        break;
      default:
        break;
    }
    foreach(w, *widgets)
        if(w->widget->isvisible
           && *x >= w->geometry.x && *x < w->geometry.x + w->geometry.width
           && *y >= w->geometry.y && *y < w->geometry.y + w->geometry.height)
            return w->widget;

    return NULL;
}

/** Convert a Lua table to a list of widget nodet.
 * \param L The Lua VM state.
 * \param widgets The linked list of widget node.
 */
static void
luaA_table2widgets(lua_State *L, widget_node_array_t *widgets)
{
    if(lua_istable(L, -1))
    {
        lua_pushnil(L);
        while(luaA_next(L, -2))
            luaA_table2widgets(L, widgets);
        /* remove the table */
        lua_pop(L, 1);
    }
    else
    {
        widget_t *widget = luaA_toudata(L, -1, "widget");
        if(widget)
        {
            widget_node_t w;
            p_clear(&w, 1);
            w.widget = luaA_object_ref(L, -1);
            widget_node_array_append(widgets, w);
        }
        else
            lua_pop(L, 1); /* remove value */
    }
}

/** Retrieve a list of widget geometries using a Lua layout function.
 *  a table which contains the geometries is then pushed onto the stack
 * \param wibox The wibox.
 * \return True is everything is ok, false otherwise.
 * \todo What do we do if there's no layout defined?
 */
bool
widget_geometries(wibox_t *wibox)
{
    /* get the layout field of the widget table */
    if(wibox->widgets_table)
    {
        /* push wibox */
        luaA_object_push(globalconf.L, wibox);
        /* push widgets table */
        luaA_object_push_item(globalconf.L, -1, wibox->widgets_table);
        /* remove wibox */
        lua_remove(globalconf.L, -2);
        lua_getfield(globalconf.L, -1, "layout");
    }
    else
        lua_pushnil(globalconf.L);

    /* if the layout field is a function */
    if(lua_isfunction(globalconf.L, -1))
    {
        /* Push 1st argument: wibox geometry */
        area_t geometry = wibox->geometry;
        geometry.x = 0;
        geometry.y = 0;
        /* we need to exchange the width and height of the wibox window if it
         * it is rotated, so the layout function doesn't need to care about that
         */
        if(wibox->orientation != East)
        {
            int i = geometry.height;
            geometry.height = geometry.width;
            geometry.width = i;
        }
        luaA_pusharea(globalconf.L, geometry);
        /* Re-push 2nd argument: widget table */
        lua_pushvalue(globalconf.L, -3);
        /* Push 3rd argument: wibox screen */
        lua_pushnumber(globalconf.L, screen_array_indexof(&globalconf.screens, wibox->screen));
        /* Re-push the layout function */
        lua_pushvalue(globalconf.L, -4);
        /* call the layout function with 3 arguments (wibox geometry, widget
         * table, screen) and wait for one result */
        if(!luaA_dofunction(globalconf.L, 3, 1))
            return false;

        lua_insert(globalconf.L, -3);
        lua_pop(globalconf.L, 2);
    }
    else
    {
        /* Remove the "nil function" */
        lua_pop(globalconf.L, 1);

        /* If no layout function has been specified, we just push a table with
         * geometries onto the stack. These geometries are nothing fancy, they
         * have x = y = 0 and their height and width set to the widgets demands
         * or the wibox size, depending on which is less.
         */

        widget_node_array_t *widgets = &wibox->widgets;
        widget_node_array_wipe(widgets);
        widget_node_array_init(widgets);

        /* push wibox */
        luaA_object_push(globalconf.L, wibox);
        /* push widgets table */
        luaA_object_push_item(globalconf.L, -1, wibox->widgets_table);
        /* remove wibox */
        lua_remove(globalconf.L, -2);
        luaA_table2widgets(globalconf.L, widgets);

        lua_newtable(globalconf.L);
        for(int i = 0; i < widgets->len; i++)
        {
            lua_pushnumber(globalconf.L, i + 1);
            widget_t *widget = widgets->tab[i].widget;
            lua_pushnumber(globalconf.L, screen_array_indexof(&globalconf.screens, wibox->screen));
            area_t geometry = widget->extents(globalconf.L, widget);
            lua_pop(globalconf.L, 1);
            geometry.x = geometry.y = 0;
            geometry.width = MIN(wibox->geometry.width, geometry.width);
            geometry.height = MIN(wibox->geometry.height, geometry.height);

            luaA_pusharea(globalconf.L, geometry);

            lua_settable(globalconf.L, -3);
        }
    }
    return true;
}

/** Render a list of widgets.
 * \param wibox The wibox.
 * \todo Remove GC.
 */
void
widget_render(wibox_t *wibox)
{
    lua_State *L = globalconf.L;
    draw_context_t *ctx = &wibox->ctx;
    area_t rectangle = { 0, 0, 0, 0 };
    color_t col;

    rectangle.width = ctx->width;
    rectangle.height = ctx->height;

    if (!widget_geometries(wibox))
        return;

    if(ctx->bg.alpha != 0xffff)
    {
        int x = wibox->geometry.x, y = wibox->geometry.y;
        xcb_get_property_reply_t *prop_r;
        char *data;
        xcb_pixmap_t rootpix;
        xcb_get_property_cookie_t prop_c;
        xcb_screen_t *s = xutil_screen_get(globalconf.connection, ctx->phys_screen);
        prop_c = xcb_get_property_unchecked(globalconf.connection, false, s->root, _XROOTPMAP_ID,
                                            PIXMAP, 0, 1);
        if((prop_r = xcb_get_property_reply(globalconf.connection, prop_c, NULL)))
        {
            if(prop_r->value_len
               && (data = xcb_get_property_value(prop_r))
               && (rootpix = *(xcb_pixmap_t *) data))
               switch(wibox->orientation)
               {
                 case North:
                   draw_rotate(ctx,
                               rootpix, ctx->pixmap,
                               s->width_in_pixels, s->height_in_pixels,
                               ctx->width, ctx->height,
                               M_PI_2,
                               y + ctx->width,
                               - x);
                   break;
                 case South:
                   draw_rotate(ctx,
                               rootpix, ctx->pixmap,
                               s->width_in_pixels, s->height_in_pixels,
                               ctx->width, ctx->height,
                               - M_PI_2,
                               - y,
                               x + ctx->height);
                   break;
                 case East:
                   xcb_copy_area(globalconf.connection, rootpix,
                                 wibox->pixmap, wibox->gc,
                                 x, y,
                                 0, 0,
                                 ctx->width, ctx->height);
                   break;
               }
            p_delete(&prop_r);
        }
    }

    widget_node_array_t *widgets = &wibox->widgets;

    widget_node_array_wipe(widgets);
    widget_node_array_init(widgets);
    /* push wibox */
    luaA_object_push(globalconf.L, wibox);
    /* push widgets table */
    luaA_object_push_item(globalconf.L, -1, wibox->widgets_table);
    /* remove wibox */
    lua_remove(globalconf.L, -2);
    luaA_table2widgets(L, widgets);

    /* get computed geometries */
    for(unsigned int i = 0; i < lua_objlen(L, -1); i++)
    {
        lua_pushnumber(L, i + 1);
        lua_gettable(L, -2);

        widgets->tab[i].geometry.x = luaA_getopt_number(L, -1, "x", wibox->geometry.x);
        widgets->tab[i].geometry.y = luaA_getopt_number(L, -1, "y", wibox->geometry.y);
        widgets->tab[i].geometry.width = luaA_getopt_number(L, -1, "width", 1);
        widgets->tab[i].geometry.height = luaA_getopt_number(L, -1, "height", 1);

        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    /* draw background image, only if the background color is not opaque */
    if(wibox->bg_image && ctx->bg.alpha != 0xffff)
        draw_image(ctx, 0, 0, 1.0, wibox->bg_image);

    /* draw background color */
    xcolor_to_color(&ctx->bg, &col);
    draw_rectangle(ctx, rectangle, 1.0, true, &col);

    /* draw everything! */
    for(int i = 0; i < widgets->len; i++)
        if(widgets->tab[i].widget->isvisible)
            widgets->tab[i].widget->draw(widgets->tab[i].widget,
                                         ctx, widgets->tab[i].geometry, wibox);

    switch(wibox->orientation)
    {
        case South:
          draw_rotate(ctx, ctx->pixmap, wibox->pixmap,
                      ctx->width, ctx->height,
                      ctx->height, ctx->width,
                      M_PI_2, ctx->height, 0);
          break;
        case North:
          draw_rotate(ctx, ctx->pixmap, wibox->pixmap,
                      ctx->width, ctx->height,
                      ctx->height, ctx->width,
                      - M_PI_2, 0, ctx->width);
          break;
        case East:
          break;
    }
}

/** Invalidate widgets which should be refresh depending on their types.
 * \param type Widget type to invalidate.
 */
void
widget_invalidate_bytype(widget_constructor_t *type)
{
    foreach(wibox, globalconf.wiboxes)
        foreach(wnode, (*wibox)->widgets)
            if(wnode->widget->type == type)
            {
                (*wibox)->need_update = true;
                break;
            }
}

/** Set a wibox needs update because it has widget, or redraw a titlebar.
 * \param widget The widget to look for.
 */
void
widget_invalidate_bywidget(widget_t *widget)
{
    foreach(wibox, globalconf.wiboxes)
        if(!(*wibox)->need_update)
            foreach(wnode, (*wibox)->widgets)
                if(wnode->widget == widget)
                {
                    (*wibox)->need_update = true;
                    break;
                }

    foreach(_c, globalconf.clients)
    {
        client_t *c = *_c;
        if(c->titlebar && !c->titlebar->need_update)
            for(int j = 0; j < c->titlebar->widgets.len; j++)
                if(c->titlebar->widgets.tab[j].widget == widget)
                {
                    c->titlebar->need_update = true;
                    break;
                }
    }
}

/** Create a new widget.
 * \param L The Lua VM state.
 *
 * \luastack
 * \lparam A table with at least a type value.
 * \lreturn A brand new widget.
 */
static int
luaA_widget_new(lua_State *L)
{
    const char *type;
    widget_t *w;
    widget_constructor_t *wc = NULL;
    size_t len;

    luaA_checktable(L, 2);

    type = luaA_getopt_lstring(L, 2, "type", NULL, &len);

    switch(a_tokenize(type, len))
    {
      case A_TK_TEXTBOX:
        wc = widget_textbox;
        break;
      case A_TK_PROGRESSBAR:
        wc = widget_progressbar;
        break;
      case A_TK_GRAPH:
        wc = widget_graph;
        break;
      case A_TK_SYSTRAY:
        wc = widget_systray;
        break;
      case A_TK_IMAGEBOX:
        wc = widget_imagebox;
        break;
      default:
        break;
    }

    if(wc)
    {
        w = widget_new(L);
        wc(w);
    }
    else
    {
        luaA_warn(L, "unkown widget type: %s", type);
        return 0;
    }

    w->type = wc;

    /* Set visible by default. */
    w->isvisible = true;

    return 1;
}

/** Get or set mouse buttons bindings to a widget.
 * \param L The Lua VM state.
 *
 * \luastack
 * \lvalue A widget.
 * \lparam An array of mouse button bindings objects, or nothing.
 * \return The array of mouse button bindings objects of this widget.
 */
static int
luaA_widget_buttons(lua_State *L)
{
    widget_t *widget = luaL_checkudata(L, 1, "widget");

    if(lua_gettop(L) == 2)
    {
        luaA_button_array_set(L, 1, 2, &widget->buttons);
        return 1;
    }

    return luaA_button_array_get(L, 1, &widget->buttons);
}

/** Generic widget.
 * \param L The Lua VM state.
 * \return The number of elements pushed on stack.
 * \luastack
 * \lfield visible The widget visibility.
 * \lfield mouse_enter A function to execute when the mouse enter the widget.
 * \lfield mouse_leave A function to execute when the mouse leave the widget.
 */
static int
luaA_widget_index(lua_State *L)
{
    size_t len;
    widget_t *widget = luaL_checkudata(L, 1, "widget");
    const char *buf = luaL_checklstring(L, 2, &len);
    awesome_token_t token;

    if(luaA_usemetatable(L, 1, 2))
        return 1;

    switch((token = a_tokenize(buf, len)))
    {
      case A_TK_VISIBLE:
        lua_pushboolean(L, widget->isvisible);
        return 1;
      case A_TK_MOUSE_ENTER:
        return luaA_object_push_item(L, 1, widget->mouse_enter);
      case A_TK_MOUSE_LEAVE:
        return luaA_object_push_item(L, 1, widget->mouse_leave);
      default:
        break;
    }

    return widget->index ? widget->index(L, token) : 0;
}

/** Generic widget newindex.
 * \param L The Lua VM state.
 * \return The number of elements pushed on stack.
 */
static int
luaA_widget_newindex(lua_State *L)
{
    size_t len;
    widget_t *widget = luaL_checkudata(L, 1, "widget");
    const char *buf = luaL_checklstring(L, 2, &len);
    awesome_token_t token;

    switch((token = a_tokenize(buf, len)))
    {
      case A_TK_VISIBLE:
        widget->isvisible = luaA_checkboolean(L, 3);
        break;
      case A_TK_MOUSE_ENTER:
        luaA_checkfunction(L, 3);
        luaA_object_unref_item(L, 1, widget->mouse_enter);
        widget->mouse_enter = luaA_object_ref_item(L, 1, 3);
        return 0;
      case A_TK_MOUSE_LEAVE:
        luaA_checkfunction(L, 3);
        luaA_object_unref_item(L, 1, widget->mouse_leave);
        widget->mouse_leave = luaA_object_ref_item(L, 1, 3);
        return 0;
      default:
        return widget->newindex ? widget->newindex(L, token) : 0;
    }

    widget_invalidate_bywidget(widget);

    return 0;
}

static int
luaA_widget_extents(lua_State *L)
{
    widget_t *widget = luaL_checkudata(L, 1, "widget");
    area_t g = {
        .x = 0,
        .y = 0,
        .width = 0,
        .height = 0
    };

    if(widget->extents)
        g = widget->extents(L, widget);

    lua_newtable(L);
    lua_pushnumber(L, g.width);
    lua_setfield(L, -2, "width");
    lua_pushnumber(L, g.height);
    lua_setfield(L, -2, "height");

    return 1;
}

const struct luaL_reg awesome_widget_methods[] =
{
    LUA_CLASS_METHODS(widget)
    { "__call", luaA_widget_new },
    { NULL, NULL }
};
const struct luaL_reg awesome_widget_meta[] =
{
    LUA_OBJECT_META(widget)
    { "buttons", luaA_widget_buttons },
    { "extents", luaA_widget_extents },
    { "__index", luaA_widget_index },
    { "__newindex", luaA_widget_newindex },
    { "__gc", luaA_widget_gc },
    { NULL, NULL }
};

// vim: filetype=c:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:encoding=utf-8:textwidth=80
