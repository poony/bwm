/* See LICENSE file for copyright and license details. */
#include <string.h>
#include <xcb/xcb_icccm.h>
#include "tfwm.h"
#include "list.h"
#include "client.h"
#include "keys.h"
#include "ewmh.h"

unsigned int selws = 0;
unsigned int prevws = 0;
Client *sel;

void
applyrules(Client *c) {
	unsigned int i;
	xcb_atom_t a = XCB_NONE;

	// WM_WINDOW_TYPE_*
	xcb_ewmh_get_atoms_reply_t win_type;
	if (xcb_ewmh_get_wm_window_type_reply(ewmh, xcb_ewmh_get_wm_window_type(ewmh, c->win), &win_type, NULL) == 1) {
		for (i = 0; i < win_type.atoms_len; i++) {
			a = win_type.atoms[i];
#ifdef DEBUG
			char *name;
			name = get_atom_name(a);
			PRINTF("applyrules: win %#x, atom %s\n", c->win, name);
			free(name);
#endif
		}
		xcb_ewmh_get_atoms_reply_wipe(&win_type);
	}

	// WM_STATE_*
	xcb_ewmh_get_atoms_reply_t win_state;
	if (xcb_ewmh_get_wm_state_reply(ewmh, xcb_ewmh_get_wm_state(ewmh, c->win), &win_state, NULL) == 1) {
		for (i = 0; i < win_state.atoms_len; i++) {
			a = win_state.atoms[i];
#ifdef DEBUG
			char *name;
			name = get_atom_name(a);
			PRINTF("applyrules: win %#x, atom %s\n", c->win, name);
			free(name);
#endif
			if (a == ewmh->_NET_WM_STATE_FULLSCREEN) {
				maximizeclient(c, true);
			}
		}
		xcb_ewmh_get_atoms_reply_wipe(&win_state);
	}

	/* custom rules */
	const Rule *r;
	xcb_icccm_get_wm_class_reply_t class_reply;

	if (!xcb_icccm_get_wm_class_reply(conn, xcb_icccm_get_wm_class(conn, c->win), &class_reply, NULL))
		return;

	for (i = 0; i < LENGTH(rules); i++) {
		r = &rules[i];
		if ((r->class && strstr(class_reply.class_name, r->class))) {
			if (!r->border)
				c->noborder = true;
		}
	}
	xcb_icccm_get_wm_class_reply_wipe(&class_reply);
}

void
cycleclients(const Arg *arg) {
	if (arg->i == NextWindow)
		focusstack(true);
	else
		focusstack(false);
}

void
fitclient(Client *c) {
	bool update = false;

	if (c->noborder)
		return;
	if (c->geom.width >= screen->width_in_pixels-2*border_width) {
		c->geom.width = screen->width_in_pixels - border_width * 2;
		update = true;
	}
	if (c->geom.height >= screen->height_in_pixels-2*border_width) {
		c->geom.height = screen->height_in_pixels - border_width * 2;
		update = true;
	}
	if (update) {
		c->geom.x = c->geom.y = 0;
		moveresize(c, c->geom.x, c->geom.y, c->geom.width, c->geom.height);
	}
}

void
killselected(const Arg *arg) {
	(void)arg;
	if (!sel)
		return;

	if (sel->frame) {
		xcb_reparent_window(conn, sel->win, screen->root, sel->geom.x, sel->geom.y);
		xcb_destroy_window(conn, sel->frame);
	}

	if (sel->can_delete)
		send_client_message(sel, WM_DELETE_WINDOW);
	else
		xcb_kill_client(conn, sel->win);
}

void
manage(xcb_window_t w) {
	Client *c = NULL;
	xcb_get_geometry_reply_t *gr;
	xcb_icccm_get_wm_protocols_reply_t pr;
	unsigned int i;

	if (!(c = malloc(sizeof(Client))))
		err("can't allocate memory.");

	c->win = w;

	/* geometry */
	gr = xcb_get_geometry_reply(conn, xcb_get_geometry(conn, w), NULL);
	if (!gr) {
		warn("xcb_get_geometry failed.");
		return;
	}
	c->geom.x = c->old_geom.x = gr->x;
	c->geom.y = c->old_geom.y = gr->y;
	c->geom.width = c->old_geom.width = gr->width;
	c->geom.height = c->old_geom.height = gr->height;
	free(gr);

	/* init members */
	c->size_hints.flags = 0;
	c->size_hints.x = c->size_hints.y = 0;
	c->size_hints.width = c->size_hints.height = 0;
	c->size_hints.min_width = c->size_hints.min_height = 0;
	c->size_hints.max_width = c->size_hints.max_height = 0;
	c->size_hints.width_inc = c->size_hints.height_inc = 0;
	c->size_hints.min_aspect_num = c->size_hints.min_aspect_den = 0;
	c->size_hints.max_aspect_num = c->size_hints.max_aspect_den = 0;
	c->size_hints.base_width = c->size_hints.base_height = 0;
	c->size_hints.win_gravity = 0;
	c->ismax = c->isvertmax = c->ishormax = c->can_focus = c->can_delete =  c->noborder = false;
	c->frame = 0;
	c->ws = selws;

	/* get size hints */
	xcb_icccm_get_wm_normal_hints_reply(conn, xcb_icccm_get_wm_normal_hints(conn, c->win), &c->size_hints, NULL);

	/* get wm hints */
	xcb_icccm_get_wm_hints_reply(conn, xcb_icccm_get_wm_hints(conn, c->win), &c->wm_hints, NULL);

	/* get protocols */
	if (xcb_icccm_get_wm_protocols_reply(conn, xcb_icccm_get_wm_protocols(conn, c->win, WM_PROTOCOLS), &pr, NULL) == 1) {
		for (i = 0; i < pr.atoms_len; ++i) {
			if (pr.atoms[i] == WM_DELETE_WINDOW)
				c->can_delete = true;
			if (pr.atoms[i] == WM_TAKE_FOCUS)
				c->can_focus = true;
		}
		xcb_icccm_get_wm_protocols_reply_wipe(&pr);
	}

	reparent(c);
	applyrules(c);
	attach(c);
	attachstack(c);
	sel = c;
	fitclient(c);

	if (sloppy_focus)
		xcb_change_window_attributes(conn, w, XCB_CW_EVENT_MASK, (uint32_t[]){CLIENT_EVENT_MASK});

	if (c->ws == selws)
		xcb_map_window(conn, w);

	ewmh_update_client_list(clients);
	focus(NULL);
}

void
reparent(Client *c) {
	xcb_rectangle_t f;

	f.x = c->geom.x;
	f.y = c->geom.y;
	f.width = c->geom.width + 2 * border_width;
	f.height = c->geom.height + 2 * border_width;

	c->frame = xcb_generate_id(conn);
	xcb_create_window(conn, XCB_COPY_FROM_PARENT, c->frame, screen->root, f.x, f.y, f.width, f.height,
			  border_width, XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT,
			  XCB_CW_BORDER_PIXEL | XCB_CW_OVERRIDE_REDIRECT,
			  (uint32_t[]){ focuscol, true });

	xcb_configure_window(conn, c->win, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
			     (uint32_t[]){ f.width, f.height });

	xcb_map_window(conn, c->frame);

#ifdef SHAPE
	/* if (has_shape) */
	/* 	roundcorners(c); */
#endif
	PRINTF("reparenting win %#x to %#x\n", c->win, c->frame);
	xcb_reparent_window(conn, c->win, c->frame, 0, 0);
}

void
maximize(const Arg *arg) {
	(void)arg;
	if (!sel)
		return;
	maximizeclient(sel, !sel->ismax);
}

void
maximizeaxis(const Arg *arg) {
	if (!sel || sel->ismax)
		return;
	if (sel->isvertmax || sel->ishormax) {
		maximizeclient(sel, false);
		return;
	}
	savegeometry(sel);
	uint32_t values[3];
	uint16_t tw = screen->width_in_pixels;
	uint16_t th = screen->height_in_pixels;
	if (!sel->noborder) {
		tw -= border_width * 2;
		th -= border_width * 2;
	}
	if (arg->i == MaxVertical) {
		sel->geom.y = 0;
		sel->geom.height = th;
		values[0] = sel->geom.y;
		values[1] = sel->geom.height;
		xcb_configure_window(conn, sel->win, XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_HEIGHT, values);
		sel->isvertmax = true;
	}
	else if (arg->i == MaxHorizontal) {
		sel->geom.x = 0;
		sel->geom.width = tw;
		values[0] = sel->geom.x;
		values[1] = sel->geom.width;
		xcb_configure_window(conn, sel->win, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_WIDTH, values);
		sel->ishormax = true;
	}
	setborder(sel, true);
}

void
maximizeclient(Client *c, bool doit) {
	if (!c)
		return;
	if (c->ismax && doit)
		return;

	PRINTF("maximizeclient: %s, ", doit ? "maximizing" : "unmaximizing");
	if (doit) {
		savegeometry(c);
		c->ismax = true;
		c->isvertmax = c->ishormax = false;
		c->geom.x = 0;
		c->geom.y = 0;
		c->geom.width = screen->width_in_pixels;
		c->geom.height = screen->height_in_pixels;
		PRINTF("x %d y %d w %d h %d\n", c->geom.x, c->geom.y, c->geom.width, c->geom.height);
		moveresize(c, c->geom.x, c->geom.y, c->geom.width, c->geom.height);
		focus(NULL);
		setborderwidth(c, 0);
	}
	else {
		c->geom.x = c->isvertmax ? c->geom.x : MAX(0, c->old_geom.x);
		c->geom.y = c->ishormax ? c->geom.y : MAX(0, c->old_geom.y);
		c->geom.width = c->old_geom.width;
		c->geom.height = c->old_geom.height;
		c->ismax = c->ishormax = c->isvertmax = false;
		PRINTF("x %d y %d w %d h %d\n", c->geom.x, c->geom.y, c->geom.width, c->geom.height);
		moveresize(c, c->geom.x, c->geom.y, c->geom.width, c->geom.height);
		setborderwidth(sel, border_width);
		setborder(sel, true);
	}
}

void
move(const Arg *arg) {
	if (!sel || sel->win == screen->root)
		return;

	PRINTF("move win %#x\n", sel->frame);

	switch (arg->i) {
		case MoveDown:  sel->geom.y += move_step; break;
		case MoveRight: sel->geom.x += move_step; break;
		case MoveUp:    sel->geom.y -= move_step; break;
		case MoveLeft:  sel->geom.x -= move_step; break;
	}

	movewin(sel->frame, sel->geom.x, sel->geom.y);
}

void
movewin(xcb_window_t win, int x, int y) {
	const uint32_t values[] = { x, y };
	xcb_configure_window(conn, win, XCB_CONFIG_WINDOW_X|XCB_CONFIG_WINDOW_Y, values);
}

void
moveresize(Client *c, int x, int y, int w, int h) {
	uint32_t mask = 0;

	if (!c || !c->frame)
		return;

	PRINTF("moveresize win %#x\n", c->frame);

	const uint32_t val[] = { x, y, w, h };
	mask |= XCB_CONFIG_WINDOW_X
		| XCB_CONFIG_WINDOW_Y
		| XCB_CONFIG_WINDOW_WIDTH
		| XCB_CONFIG_WINDOW_HEIGHT;

	xcb_configure_window(conn, c->frame, mask, val);
}


void
raisewindow(xcb_drawable_t win) {
	if (screen->root == win || !win)
		return;
	PRINTF("raise win %#x", win);
	const uint32_t values[] = { XCB_STACK_MODE_ABOVE };
	xcb_configure_window(conn, win, XCB_CONFIG_WINDOW_STACK_MODE, values);
}

void
resize(const Arg *arg) {
	int iw = resize_step, ih = resize_step;
	if (!sel)
		return;

	PRINTF("resize win %#x\n", sel->frame);

	if (sel->size_hints.width_inc > 7 && sel->size_hints.width_inc < screen->width_in_pixels)
		iw = sel->size_hints.width_inc;
	if (sel->size_hints.height_inc > 7 && sel->size_hints.height_inc < screen->height_in_pixels)
		ih = sel->size_hints.height_inc;

	if (arg->i == GrowHeight || arg->i == GrowBoth) {
		sel->geom.height = sel->geom.height + ih;
	}
	if (arg->i == GrowWidth || arg->i == GrowBoth) {
		sel->geom.width = sel->geom.width + iw;
	}
	if (arg->i == ShrinkHeight || arg->i == ShrinkBoth) {
		if (sel->geom.height - ih > sel->size_hints.min_height)
			sel->geom.height = sel->geom.height - ih;
	}
	if (arg->i == ShrinkWidth || arg->i == ShrinkBoth) {
		if (sel->geom.width - iw > sel->size_hints.min_width)
			sel->geom.width = sel->geom.width - iw;
	}
	resizewin(sel->frame, sel->geom.width, sel->geom.height);

	if (sel->ismax) {
		sel->ismax = false;
		setborderwidth(sel, border_width);
	}
	sel->ishormax = sel->isvertmax = false;
	setborder(sel, true);
}

void
resizewin(xcb_window_t win, int w, int h) {
	unsigned int values[2] = { w, h };
	xcb_configure_window(conn, win, XCB_CONFIG_WINDOW_WIDTH |
			     XCB_CONFIG_WINDOW_HEIGHT, values);
}

void
savegeometry(Client *c) {
	c->old_geom.x = c->geom.x;
	c->old_geom.y = c->geom.y;
	c->old_geom.height = c->geom.height;
	c->old_geom.width = c->geom.width;
}

void
send_client_message(Client *c, xcb_atom_t proto) {
	xcb_client_message_event_t ev;
#if DEBUG
	char *name;

	name = get_atom_name(proto);
	PRINTF("send_client_message: %s to win %#x\n", name, c->win);
	free(name);
#endif
	memset(&ev, '\0', sizeof ev);
	ev.response_type = XCB_CLIENT_MESSAGE;
	ev.format = 32;
	ev.sequence = 0;
	ev.window = c->win;
	ev.type = ewmh->WM_PROTOCOLS;
	ev.data.data32[0] = proto;
	ev.data.data32[1] = XCB_CURRENT_TIME;
	xcb_send_event(conn, false, c->win, XCB_EVENT_MASK_NO_EVENT, (char*)&ev);
}

void
setborder(Client *c, bool focus) {
	(void)c;
	(void)focus;
	/* if (c->ismax || c->noborder) */
	/* 	return; */
	/* uint32_t values[1]; */
	/* int half = outer_border_width; */
	/* values[0] = border_width; */
	/* xcb_configure_window(conn, c->win, XCB_CONFIG_WINDOW_BORDER_WIDTH, values); */
	/* if (!double_border) { */
	/* 	values[0] = focus ? focuscol : unfocuscol; */
	/* 	xcb_change_window_attributes(conn, c->win, XCB_CW_BORDER_PIXEL, values); */
	/* 	return; */
	/* } */
	/* xcb_pixmap_t pmap = xcb_generate_id(conn); */
	/* xcb_gcontext_t gc = xcb_generate_id(conn); */
	/* xcb_create_pixmap(conn, screen->root_depth, pmap, screen->root, */
	/* 		  c->geom.width+border_width*2, c->geom.height+border_width*2); */
	/* xcb_create_gc(conn, gc, pmap, 0, NULL); */

	/* values[0] = outercol; */
	/* xcb_change_gc(conn, gc, XCB_GC_FOREGROUND, &values[0]); */
	/* xcb_rectangle_t rect_outer[] = { */
	/* 	{ c->geom.width+border_width-half, 0, half, c->geom.height+border_width*2 }, */
	/* 	{ c->geom.width+border_width, 0, half, c->geom.height+border_width*2 }, */
	/* 	{ 0, c->geom.height+border_width-half, c->geom.width+border_width*2, half }, */
	/* 	{ 0, c->geom.height+border_width, c->geom.width+border_width*2, half }, */
	/* 	{ 1, 1, 1, 1 } */
	/* }; */
	/* xcb_poly_fill_rectangle(conn, pmap, gc, 5, rect_outer); */

	/* values[0] = focus ? focuscol : unfocuscol; */
	/* xcb_change_gc(conn, gc, XCB_GC_FOREGROUND, &values[0]); */
	/* xcb_rectangle_t rect_inner[] = { */
	/* 	{ c->geom.width, 0, border_width-half, c->geom.height+border_width-half }, */
	/* 	{ c->geom.width+border_width+half, 0, border_width-half, c->geom.height+border_width-half }, */
	/* 	{ 0, c->geom.height, c->geom.width+border_width-half, border_width-half }, */
	/* 	{ 0, c->geom.height+border_width+half, c->geom.width+border_width-half, border_width-half }, */
	/* 	{ c->geom.width+border_width+half, border_width+c->geom.height+half, border_width, border_width } */
	/* }; */
	/* xcb_poly_fill_rectangle(conn, pmap, gc, 5, rect_inner); */

	/* values[0] = pmap; */
	/* xcb_change_window_attributes(conn,c->win, XCB_CW_BORDER_PIXMAP, &values[0]); */
	/* xcb_free_pixmap(conn, pmap); */
	/* xcb_free_gc(conn, gc); */
}

void
setborderwidth(Client *c, int width) {
	if (c->noborder)
		return;
	uint32_t value[1] = { width };
	xcb_configure_window(conn, c->win, XCB_CONFIG_WINDOW_BORDER_WIDTH, value);
}

void
showhide(Client *c) {
	if (!c)
		return;
	if (ISVISIBLE(c)) {
		movewin(c->frame, c->geom.x, c->geom.y);
		showhide(c->snext);
	}
	else {
		showhide(c->snext);
		// TODO: set iconic state
		movewin(c->frame, WIDTH(c) * -2, c->geom.y);
	}
}

void
teleport(const Arg *arg) {
	uint16_t tw;
	uint16_t th;

	if (!sel || sel->frame == screen->root)
		return;

	PRINTF("teleport win %#x\n", sel->frame);

	tw = sel->geom.width;
	th = sel->geom.height;
	if (!sel->noborder) {
		tw +=  border_width * 2;
		th +=  border_width * 2;
	}
	switch (arg->i) {
		case Center:
			sel->geom.x = (screen->width_in_pixels - tw) / 2;
			sel->geom.y = (screen->height_in_pixels - th) / 2;
			break;
		case TopLeft:
			sel->geom.x = 0;
			sel->geom.y = 0;
			break;
		case TopRight:
			sel->geom.x = screen->width_in_pixels - tw;
			sel->geom.y = 0;
			break;
		case BottomLeft:
			sel->geom.x = 0;
			sel->geom.y = screen->height_in_pixels - th;
			break;
		case BottomRight:
			sel->geom.x = screen->width_in_pixels - tw;
			sel->geom.y = screen->height_in_pixels - th;
			break;
	}
	movewin(sel->frame, sel->geom.x, sel->geom.y);
}

void
unmanage(Client *c) {
	detach(c);
	detachstack(c);
	free(c);
	ewmh_update_client_list(clients);
	focus(NULL);
}


Client *
wintoclient(xcb_window_t w) {
	Client *c;
	for (c = clients; c; c = c->next)
		if (w == c->win)
			return c;
	return NULL;
}

