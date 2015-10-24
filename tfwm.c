/* See LICENSE file for copyright and license details. */
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <err.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_ewmh.h>
#include <X11/keysym.h>

#if 1
#   define DEBUG(...) \
		do { fprintf(stderr, "tfwm: ");fprintf(stderr, __VA_ARGS__); } while(0)
#else
#   define DEBUG(...)
#endif

#define Mod1Mask                XCB_MOD_MASK_1
#define Mod4Mask                XCB_MOD_MASK_4
#define ShiftMask               XCB_MOD_MASK_SHIFT
#define ControlMask             XCB_MOD_MASK_CONTROL
#define CLEANMASK(mask)         ((mask & ~0x80))
#define LENGTH(X)               (sizeof(X)/sizeof(*X))
#define MAX(X, Y)               ((X) > (Y) ? (X) : (Y))
#define SUBTRACTLIM(X, Y)       ((X - Y > 0) ? (X - Y) : (X))
#define WIDTH(C)                ((C)->w + 2 * BORDER_WIDTH)
#define HEIGHT(C)               ((C)->h + 2 * BORDER_WIDTH)
#define ISVISIBLE(C)            ((C)->ws == selws)

typedef union {
	const char **com;
	const int i;
} Arg;

typedef struct Key {
	unsigned int mod;
	xcb_keysym_t keysym;
	void (*function)(const Arg *);
	const Arg arg;
} Key;

typedef struct Client Client;
struct Client{
	int x, y, w, h;
	int oldx, oldy, oldw, oldh;
	int basew, baseh, minw, minh, maxw, maxh;
	int borderwidth;
	bool isfullscreen;
	Client *next;
	Client *snext;
	xcb_window_t win;
	unsigned int ws;
};

/* function declarations */
static void attach(Client *c);
static void attachstack(Client *c);
static void changebordercolor(xcb_window_t win, long color);
static void changeborderwidth(xcb_window_t win, int width);
static void cleanup();
static void clientmessage(xcb_generic_event_t *ev);
static void configurerequest(xcb_generic_event_t *ev);
static void destroynotify(xcb_generic_event_t *ev);
static void detach(Client *c);
static void detachstack(Client *c);
static void focus(struct Client *c);
static void focusstack(const Arg *arg);
static void getatoms(xcb_atom_t *atoms, char **names, int count);
static xcb_keycode_t* getkeycodes(xcb_keysym_t keysym);
static xcb_keysym_t getkeysym(xcb_keycode_t keycode);
static void keypress(xcb_generic_event_t *ev);
static void killclient(const Arg *arg);
static void manage(xcb_window_t w);
static void mappingnotify(xcb_generic_event_t *ev);
static void maprequest(xcb_generic_event_t *ev);
static void move(const Arg *arg);
static void movewin(xcb_window_t win, int x, int y);
static void quit(const Arg *arg);
static void raisewindow(xcb_drawable_t win);
static void resize(const Arg *arg);
static void restack();
static void run(void);
static void selectws(const Arg* arg);
static void selectprevws(const Arg* arg);
static bool sendevent(Client *c, xcb_atom_t proto);
static void sendtows(const Arg *arg);
static void setcursor(xcb_window_t w, int cursorid);
static void sethints(Client *c);
static void setup();
static void setupkeys();
static void showhide(Client *c);
static void sigchld();
static void testcookie(xcb_void_cookie_t cookie, char *errormsg);
static void togglefullscreen(const Arg *arg);
static void unfocus(struct Client *c);
static void unmanage(Client *c);
static void unmapnotify(xcb_generic_event_t *ev);
static Client* wintoclient(xcb_window_t w);

/* enums */
enum { MoveDown, MoveRight, MoveUp, MoveLeft };
enum { ResizeDown, ResizeRight, ResizeUp, ResizeLeft };
enum { WMProtocols, WMDeleteWindow, WMState, WMLast }; /* default atoms */
enum { NetSupported, NetWMFullscreen, NetWMState, NetActiveWindow,
	NetWMDesktop, NetCurrentDesktop, NetNumberOfDesktops, NetLast }; /* EWMH atoms */

/* variables */
static xcb_connection_t *conn;
static xcb_screen_t *screen;
static unsigned int sw, sh;
static unsigned int selws = 0;
static unsigned int prevws = 0;
static int scrno;
static Client *clients;
static Client *sel;
static Client *stack;
static bool running = true;
static void (*handler[XCB_NO_OPERATION])(xcb_generic_event_t *ev);
xcb_ewmh_connection_t *ewmh;
static xcb_atom_t wmatom[WMLast];
static xcb_atom_t netatom[NetLast];
static char *wmatomnames[] = { "WM_PROTOCOLS", "WM_DELETE_WINDOW", "WM_STATE" };
static char *netatomnames[] = { "_NET_SUPPORTED", "_NET_WM_STATE_FULLSCREEN",
	"_NET_WM_STATE", "_NET_ACTIVE_WINDOW", "_NET_WM_DESKTOP",
	"_NET_CURRENT_DESKTOP", "_NET_NUMBER_OF_DESKTOPS" };

#include "config.h"

void
attach(Client *c) {
	c->next = clients;
	clients = c;
}

void
attachstack(Client *c) {
	c->snext = stack;
	stack = c;
}

void
changeborderwidth(xcb_window_t win, int width) {
	uint32_t value[1] = { width };
	xcb_configure_window(conn, win, XCB_CONFIG_WINDOW_BORDER_WIDTH, value);
}

void
changebordercolor(xcb_window_t win, long color) {
	uint32_t value[1] = { color };
	xcb_change_window_attributes(conn, win, XCB_CW_BORDER_PIXEL, value);
}

void
cleanup() {
	while (stack)
		unmanage(stack);

    xcb_ewmh_connection_wipe(ewmh);
    if (ewmh)
		free(ewmh);
	xcb_set_input_focus(conn, XCB_NONE,
			XCB_INPUT_FOCUS_POINTER_ROOT, XCB_CURRENT_TIME);
	xcb_flush(conn);
	xcb_disconnect(conn);
}

void
clientmessage(xcb_generic_event_t *ev) {
	xcb_client_message_event_t *e = (xcb_client_message_event_t*)ev;
	Client *c;
	if ((c = wintoclient(e->window))) {
		if (e->type == netatom[NetWMState] &&
				(unsigned)e->data.data32[1] == netatom[NetWMFullscreen]) {
			if (e->data.data32[0] == 0 || e->data.data32[0] == 1)
				togglefullscreen(NULL);
		}
		else if (e->type == netatom[NetActiveWindow])
			focus(c);
	}
}

void
configurerequest(xcb_generic_event_t *ev) {
	xcb_configure_request_event_t *e = (xcb_configure_request_event_t *)ev;
	Client *c;
	if (!(c = wintoclient(e->window)))
		return;
	unsigned int v[7];
	int i = 0;
	if (e->value_mask & XCB_CONFIG_WINDOW_X)            v[i++] = c->x = e->x;
	if (e->value_mask & XCB_CONFIG_WINDOW_Y)            v[i++] = c->y = e->y;
	if (e->value_mask & XCB_CONFIG_WINDOW_WIDTH)        v[i++] = c->w = e->width;
	if (e->value_mask & XCB_CONFIG_WINDOW_HEIGHT)       v[i++] = c->h = e->height;
	if (e->value_mask & XCB_CONFIG_WINDOW_SIBLING)      v[i++] = e->sibling;
	if (e->value_mask & XCB_CONFIG_WINDOW_STACK_MODE)   v[i++] = e->stack_mode;
	if (e->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) v[i++] = c->borderwidth = e->border_width;
	xcb_configure_window(conn, e->window, e->value_mask, v);
}

void
destroynotify(xcb_generic_event_t *ev) {
	xcb_destroy_notify_event_t *e = (xcb_destroy_notify_event_t *)ev;
	Client *c;

	if ((c = wintoclient(e->window)))
		unmanage(c);
}

void
detach(Client *c) {
	Client **tc;

	for (tc = &clients; *tc && *tc != c; tc = &(*tc)->next);
	*tc = c->next;
}

void
detachstack(Client *c) {
	Client **tc, *t;

	for (tc = &stack; *tc && *tc != c; tc = &(*tc)->snext);
	*tc = c->snext;

	if (c == sel) {
		for (t = stack; t && !ISVISIBLE(t); t = t->snext);
		sel = t;
	}
}

void
focus(struct Client *c) {
	if (!c || !ISVISIBLE(c))
		for (c = stack; c && !ISVISIBLE(c); c = c->snext)
			if (sel && sel != c)
				unfocus(sel);
	if (c) {
		detachstack(c);
		attachstack(c);
		changeborderwidth(c->win, c->borderwidth);
		changebordercolor(c->win, FOCUS);
		long data[] = { XCB_ICCCM_WM_STATE_NORMAL, XCB_NONE };
		xcb_change_property(conn, XCB_PROP_MODE_REPLACE, c->win,
				wmatom[WMState], wmatom[WMState], 32, 2, data);
		xcb_set_input_focus(conn, XCB_INPUT_FOCUS_POINTER_ROOT,
				c->win, XCB_CURRENT_TIME);
		xcb_change_property(conn, XCB_PROP_MODE_REPLACE, screen->root,
				netatom[NetActiveWindow] , XCB_ATOM_WINDOW, 32, 1,&c->win);
	}
	else {
        xcb_delete_property(conn, screen->root, netatom[NetActiveWindow]);
		xcb_set_input_focus(conn, XCB_NONE,
				XCB_INPUT_FOCUS_POINTER_ROOT, XCB_CURRENT_TIME);
	}
	sel = c;
}

void
focusstack(const Arg *arg) {
	Client *c = NULL, *i;

	if (!sel)
		return;
	if (arg->i > 0) {
		for (c = sel->next; c && !ISVISIBLE(c); c = c->next);
		if (!c)
			for (c = clients; c && !ISVISIBLE(c); c = c->next);
	}
	else {
		for (i = clients; i != sel; i = i->next)
			if (ISVISIBLE(i))
				c = i;
		if (!c)
			for (; i; i = i->next)
				if (ISVISIBLE(i))
					c = i;
	}
	if (c) {
		focus(c);
		restack();
	}
}

void
getatoms(xcb_atom_t *atoms, char **names, int count) {
	xcb_intern_atom_cookie_t cookies[count];
	for (unsigned int i = 0; i < count; ++i)
		cookies[i] = xcb_intern_atom(conn, 0, strlen(names[i]), names[i]);
	for (unsigned int i = 0; i < count; ++i) {
		xcb_intern_atom_reply_t *reply =
			xcb_intern_atom_reply(conn, cookies[i], NULL);
		if (reply) {
			atoms[i] = reply->atom;
			free(reply);
		} else
			fprintf(stderr, "WARN: failed to register %s atom.\n", wmatomnames[i]);
	}
}

xcb_keycode_t *
getkeycodes(xcb_keysym_t keysym) {
	xcb_key_symbols_t *keysyms;
	if (!(keysyms = xcb_key_symbols_alloc(conn)))
		err(EXIT_FAILURE, "ERROR: can't get keycode.");
	xcb_keycode_t *keycode = xcb_key_symbols_get_keycode(keysyms, keysym);
	xcb_key_symbols_free(keysyms);
	return keycode;
}

xcb_keysym_t
getkeysym(xcb_keycode_t keycode) {
	xcb_key_symbols_t *keysyms;
	if (!(keysyms = xcb_key_symbols_alloc(conn)))
		err(EXIT_FAILURE, "ERROR: can't get keysym.");
	xcb_keysym_t keysym = xcb_key_symbols_get_keysym(keysyms, keycode, 0);
	xcb_key_symbols_free(keysyms);
	return keysym;
}

void
keypress(xcb_generic_event_t *ev) {
	xcb_key_press_event_t *e = (xcb_key_press_event_t *)ev;
	xcb_keysym_t keysym = getkeysym(e->detail);
	for (int i=0; i<LENGTH(keys); i++) {
		if (keysym == keys[i].keysym &&
				CLEANMASK(keys[i].mod) == CLEANMASK(e->state) &&
				keys[i].function)
		{
			keys[i].function(&keys[i].arg);
			break;
		}
	}
}

void
killclient(const Arg *arg) {
	if (!sel)
		return;
	if (!sendevent(sel, wmatom[WMDeleteWindow])) {
		xcb_kill_client(conn, sel->win);
	}
}

void
manage(xcb_window_t w) {
	Client *c = NULL;
	if (!(c = malloc(sizeof(Client))))
		err(EXIT_FAILURE, "ERROR: can't allocate memory");
	c->win = w;
	/* geometry */
	xcb_get_geometry_reply_t *geom;
	geom = xcb_get_geometry_reply(conn, xcb_get_geometry(conn, w), NULL);
	if (!geom)
		err(EXIT_FAILURE, "ERROR: geometry reply failed");
	c->x = c->oldx = geom->x;
	c->y = c->oldy = geom->y;
	c->w = c->oldw = geom->width;
	c->h = c->oldh = geom->height;
	free(geom);
	sethints(c);
	c->ws = selws;
	c->isfullscreen = false;
	attach(c);
	attachstack(c);
	sel = c;
	c->borderwidth = BORDER_WIDTH;
	changeborderwidth(w, c->borderwidth);
	xcb_map_window(conn, w);
	/* set its workspace hint */
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, c->win, netatom[NetWMDesktop],
			XCB_ATOM_CARDINAL, 32, 1, &selws);
	/* set normal state */
	long data[] = { XCB_ICCCM_WM_STATE_NORMAL, XCB_NONE };
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, w, netatom[NetWMState],
			netatom[NetWMState], 32, 2, data);

	// TODO: check reply
	xcb_get_property_reply_t *prop_reply;
	prop_reply = xcb_get_property_reply(conn, xcb_get_property_unchecked(conn, 0, w, netatom[NetWMState], XCB_ATOM_ATOM, 0, 1), NULL);
	if (prop_reply) {
		if (prop_reply->format == 32) {
			xcb_atom_t *v = xcb_get_property_value(prop_reply);
			if (v[0] == netatom[NetWMFullscreen])
				togglefullscreen(NULL);
		}
		free(prop_reply);
	}
	focus(NULL);
}

void
mappingnotify(xcb_generic_event_t *ev) {
	xcb_mapping_notify_event_t *e = (xcb_mapping_notify_event_t *)ev;
	if (e->request != XCB_MAPPING_MODIFIER && e->request != XCB_MAPPING_KEYBOARD)
		return;
	xcb_ungrab_key(conn, XCB_GRAB_ANY, screen->root, XCB_MOD_MASK_ANY);
	setupkeys();
}

void
maprequest(xcb_generic_event_t *ev) {
	xcb_map_request_event_t *e = (xcb_map_request_event_t *)ev;
	if (!wintoclient(e->window))
		manage(e->window);
}

void
move(const Arg *arg) {
	if (!sel || sel->win == screen->root)
		return;
	switch (arg->i) {
		case MoveDown:  sel->y += steps[0]; break;
		case MoveRight: sel->x += steps[0]; break;
		case MoveUp:    sel->y -= steps[0]; break;
		case MoveLeft:  sel->x -= steps[0]; break;
		default:        err(EXIT_FAILURE, "ERROR: bad move argument");
	}
	movewin(sel->win, sel->x, sel->y);
}

void
movewin(xcb_window_t win, int x, int y) {
	unsigned int pos[2] = { x, y };
	xcb_configure_window(conn, win, XCB_CONFIG_WINDOW_X|XCB_CONFIG_WINDOW_Y, pos);
}

void
quit(const Arg *arg) {
	running = false;
}

void
raisewindow(xcb_drawable_t win) {
    uint32_t values[] = { XCB_STACK_MODE_ABOVE };
    if (screen->root == win || 0 == win)
        return;
    xcb_configure_window(conn, win, XCB_CONFIG_WINDOW_STACK_MODE, values);
}

void
resize(const Arg *arg) {
	if (!sel || sel->win == screen->root)
		return;
	int step = steps[1];
	switch (arg->i) {
		case ResizeDown:  sel->h = sel->h + step; break;
		case ResizeRight: sel->w = sel->w + step; break;
		case ResizeUp:    sel->h = SUBTRACTLIM(sel->h, step); break;
		case ResizeLeft:  sel->w = SUBTRACTLIM(sel->w, step); break;
		default:          err(EXIT_FAILURE, "ERROR: bad resize argument");
	}
	uint32_t values[2];
	values[0] = sel->w;
	values[1] = sel->h;
	xcb_configure_window(conn, sel->win, XCB_CONFIG_WINDOW_WIDTH
			|XCB_CONFIG_WINDOW_HEIGHT, values);
}

void
restack() {
	if (!sel)
		return;
	raisewindow(sel->win);
}

void
run(void) {
	xcb_generic_event_t *ev;
	while (running) {
		xcb_flush(conn);
		ev = xcb_wait_for_event(conn);
		if (handler[CLEANMASK(ev->response_type)])
			handler[CLEANMASK(ev->response_type)](ev);
		free(ev);
		if (xcb_connection_has_error(conn))
			running = false;
	}
}

void
selectws(const Arg* arg) {
	if (selws == arg->i)
		return;
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, screen->root,
			netatom[NetCurrentDesktop], XCB_ATOM_CARDINAL, 32, 1, &arg->i);
	xcb_ewmh_set_current_desktop(ewmh, scrno, arg->i);
	prevws = selws;
	selws = arg->i;
	focus(NULL);
	showhide(stack);
}

void
selectprevws(const Arg* arg) {
	const Arg a = { .i = prevws };
	selectws(&a);
}

bool
sendevent(Client *c, xcb_atom_t proto) {
	xcb_client_message_event_t ev;
	xcb_get_property_cookie_t cookie;
	xcb_icccm_get_wm_protocols_reply_t reply;
	bool exists = false;
	cookie = xcb_icccm_get_wm_protocols(conn, c->win, wmatom[WMProtocols]);
	if (xcb_icccm_get_wm_protocols_reply(conn, cookie, &reply, NULL) == 1) {
		for (int i = 0; i < reply.atoms_len && !exists; i++)
			if (reply.atoms[i] == proto)
				exists = true;
		xcb_icccm_get_wm_protocols_reply_wipe(&reply);
	}
	if (exists) {
		ev.response_type = XCB_CLIENT_MESSAGE;
		ev.format = 32;
		ev.sequence = 0;
		ev.window = c->win;
		ev.type = wmatom[WMProtocols];
		ev.data.data32[0] = proto,
		ev.data.data32[1] = XCB_CURRENT_TIME;
		xcb_send_event(conn, true, c->win, XCB_EVENT_MASK_NO_EVENT, (char*)&ev);
	}
	return exists;
}

void
sendtows(const Arg *arg) {
	if (arg->i == selws)
		return;
	// TODO
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, sel->win,
			netatom[NetWMDesktop], XCB_ATOM_CARDINAL, 32, 1, &arg->i);
	focus(NULL);
}

void
setcursor(xcb_window_t w, int cursorid) {
	xcb_font_t font = xcb_generate_id(conn);
	xcb_void_cookie_t fontcookie =
		xcb_open_font_checked(conn, font, strlen("cursor"), "cursor");
	testcookie(fontcookie, "can't open font");
	xcb_cursor_t cursor = xcb_generate_id(conn);
	xcb_create_glyph_cursor(conn, cursor, font, font,
			cursorid, cursorid + 1, 65535, 65535, 65535, 0, 0, 0);
	uint32_t mask = XCB_CW_CURSOR;
	uint32_t values = cursor;
	xcb_change_window_attributes(conn, w, mask, &values);
	xcb_free_cursor(conn, cursor);
	fontcookie = xcb_close_font_checked(conn, font);
	testcookie(fontcookie, "can't close font");
}

void
sethints(Client *c) {
	xcb_size_hints_t h;
	// TODO: check reply
	xcb_icccm_get_wm_normal_hints_reply(conn,
			xcb_icccm_get_wm_normal_hints_unchecked(conn, c->win), &h, NULL);

	if (h.flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) {
		c->minw = h.min_width;
		c->minh = h.min_height;
	}
	if (h.flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) {
		c->maxw = h.max_width;
		c->maxh = h.max_height;
	}
	if (h.flags & XCB_ICCCM_SIZE_HINT_BASE_SIZE) {
		c->basew = h.base_width;
		c->baseh = h.base_height;
	}
	if (c->w < MAX(c->minw, c->basew) || c->h < MAX(c->minh, c->baseh)) {
		c->w = MAX(c->w, c->minw);
		c->h = MAX(c->h, c->minh);
		uint32_t values[2] = { c->w, c->h };
		xcb_configure_window(conn, c->win, XCB_CONFIG_WINDOW_WIDTH
				| XCB_CONFIG_WINDOW_HEIGHT, values);
	}
}

void
setup() {
	/* clean up any zombies */
	sigchld();
	/* init screen */
	screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
	if (!screen)
		err(EXIT_FAILURE, "ERROR: can't find screen.");
	sw = screen->width_in_pixels;
	sh = screen->height_in_pixels;
	/* subscribe to handler */
	unsigned int value[1] = {XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
		XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT};
	xcb_void_cookie_t cookie;
	cookie = xcb_change_window_attributes_checked(conn, screen->root,
			XCB_CW_EVENT_MASK, value);
	testcookie(cookie, "another window manager is running.");
	/* init atoms */
	getatoms(wmatom, wmatomnames, WMLast);
	getatoms(netatom, netatomnames, NetLast);
	/* init ewmh */
    ewmh = malloc(sizeof(xcb_ewmh_connection_t));
    if (xcb_ewmh_init_atoms_replies(ewmh, xcb_ewmh_init_atoms(conn, ewmh), NULL) == 0)
            err(EXIT_FAILURE, "ERROR: can't initialize ewmh.");
    xcb_drawable_t root = screen->root;
	uint32_t mask = XCB_CW_EVENT_MASK;
	uint32_t values[] = { XCB_EVENT_MASK_POINTER_MOTION };
	xcb_window_t recorder = xcb_generate_id(conn);
	xcb_create_window(conn, XCB_COPY_FROM_PARENT, recorder, root, 0, 0,
			screen->width_in_pixels, screen->height_in_pixels, 0,
			XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, mask, values);
	xcb_atom_t net_atoms[] = {
		ewmh->_NET_SUPPORTED,
		ewmh->_NET_NUMBER_OF_DESKTOPS,
		ewmh->_NET_CURRENT_DESKTOP,
		ewmh->_NET_ACTIVE_WINDOW,
		ewmh->_NET_WM_DESKTOP,
		ewmh->_NET_WM_STATE,
		ewmh->_NET_WM_STATE_FULLSCREEN
	};
	xcb_ewmh_set_supported(ewmh, scrno, LENGTH(net_atoms), net_atoms);
	xcb_ewmh_set_supporting_wm_check(ewmh, root, recorder);
	xcb_ewmh_set_supporting_wm_check(ewmh, recorder, recorder);
    static const uint8_t numworkspaces = 5;
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, screen->root,
                        netatom[NetCurrentDesktop], XCB_ATOM_CARDINAL,
                        32, 1, &selws);
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, screen->root,
                        netatom[NetNumberOfDesktops], XCB_ATOM_CARDINAL,
                        32, 1, &numworkspaces);
    xcb_ewmh_set_number_of_desktops(ewmh, scrno, numworkspaces);
    xcb_ewmh_set_current_desktop(ewmh, scrno, 0);
	/* set handlers */
	handler[XCB_CONFIGURE_REQUEST] = configurerequest;
	handler[XCB_DESTROY_NOTIFY] = destroynotify;
	handler[XCB_UNMAP_NOTIFY] = unmapnotify;
	handler[XCB_MAP_REQUEST] = maprequest;
	handler[XCB_MAPPING_NOTIFY] = mappingnotify;
	handler[XCB_CLIENT_MESSAGE] = clientmessage;
	handler[XCB_KEY_PRESS] = keypress;
	setcursor(screen->root, 68); /* left_ptr */
	setupkeys();
	focus(NULL);
}

void
setupkeys() {
	xcb_keycode_t *keycode;
	for (unsigned int i = 0; i < LENGTH(keys); ++i) {
		keycode = getkeycodes(keys[i].keysym);
		for (unsigned int k = 0; keycode[k] != XCB_NO_SYMBOL; k++)
			xcb_grab_key(conn, 1, screen->root, keys[i].mod, keycode[k],
				XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
	}
}

void
showhide(Client *c) {
	if (!c)
		return;
	if (ISVISIBLE(c)) {
		movewin(c->win, c->x, c->y);
		showhide(c->snext);
	}
	else {
		showhide(c->snext);
		movewin(c->win, WIDTH(c) * -2, c->y);
	}
}

void
sigchld() {
	if (signal(SIGCHLD, sigchld) == SIG_ERR)
		err(EXIT_FAILURE, "ERROR: can't install SIGCHLD handler");
	while (0 < waitpid(-1, NULL, WNOHANG));
}

void
testcookie(xcb_void_cookie_t cookie, char *errormsg) {
	xcb_generic_error_t *error = xcb_request_check(conn, cookie);
	if (error) {
		fprintf(stderr, "ERROR: %s : %d\n", errormsg, error->error_code);
		xcb_disconnect(conn);
		exit(EXIT_FAILURE);
	}
}
void
togglefullscreen(const Arg *arg) {
	// TODO: split this function up
	if (!sel)
		return;
	uint32_t val[5];
	val[4] = XCB_STACK_MODE_ABOVE;
	uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y
		| XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT
		| XCB_CONFIG_WINDOW_STACK_MODE;

	if (!sel->isfullscreen) {
		/* changeborderwidth(sel->win, 0); */
		sel->oldx = sel->x;
		sel->oldy = sel->y;
		sel->oldw = sel->w;
		sel->oldh = sel->h;
		val[0] = 0;
		val[1] = 0;
		val[2] = sw - 2*sel->borderwidth;
		val[3] = sh - 2*sel->borderwidth;
		xcb_configure_window(conn, sel->win, mask, val);
		sel->x = val[0];
		sel->y = val[1];
		sel->w = val[2];
		sel->h = val[3];
		sel->isfullscreen = true;
	}
	else {
		val[0] = sel->oldx;
		val[1] = sel->oldy;
		val[2] = sel->oldw;
		val[3] = sel->oldh;
		/* changeborderwidth(sel->win, BORDER_WIDTH); */
		xcb_configure_window(conn, sel->win, mask, val);
		sel->x = sel->oldx;
		sel->y = sel->oldy;
		sel->w = sel->oldw;
		sel->h = sel->oldh;
		sel->isfullscreen = false;
		focus(NULL);
	}
	long data[] = { sel->isfullscreen ? netatom[NetWMFullscreen]
		: XCB_ICCCM_WM_STATE_NORMAL };
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, sel->win,
			netatom[NetWMState], XCB_ATOM_ATOM, 32, 1, data);
}

void
unfocus(struct Client *c) {
	if (!c)
		return;
	changebordercolor(c->win, UNFOCUS);
	xcb_set_input_focus(conn, XCB_INPUT_FOCUS_POINTER_ROOT,
			c->win, XCB_CURRENT_TIME);
}

void
unmanage(Client *c) {
	xcb_kill_client(conn, c->win);
	detach(c);
	detachstack(c);
	free(c);
	focus(NULL);
}

void
unmapnotify(xcb_generic_event_t *ev) {
	xcb_unmap_notify_event_t *e = (xcb_unmap_notify_event_t *)ev;
	Client *c;
	if ((c = wintoclient(e->window))) {
		unmanage(c);
	}
}

Client *
wintoclient(xcb_window_t w) {
	Client *c;
	for (c = clients; c; c = c->next)
		if (w == c->win)
			return c;
	return NULL;
}

int
main() {
	conn = xcb_connect(NULL, &scrno);
	if (xcb_connection_has_error(conn))
		err(EXIT_FAILURE, "ERROR: xcb_connect error");
	setup();
	run();
	cleanup();
	return EXIT_SUCCESS;
}

/* vi: set noexpandtab noautoindent : */
