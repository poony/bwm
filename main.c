/* See LICENSE file for copyright and license details. */
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <xcb/xcb.h>
#include <xcb/xcb_aux.h>
#include <xcb/xcb_ewmh.h>
#include "main.h"
#include "util.h"
#include "list.h"
#include "client.h"
#include "workspace.h"
#include "events.h"
#include "keys.h"
#include "cursor.h"
#include "ewmh.h"
#include "config.h"
#include "xcb.h"

static void cleanup(void);
static void remanage_windows(void);
static void run(void);
static void setup(void);
static void sigcatch(int sig);

xcb_connection_t *conn;
xcb_screen_t *screen;
unsigned int numlockmask;
int scrno;
Client *stack;
xcb_ewmh_connection_t *ewmh;
uint32_t focuscol;
uint32_t unfocuscol;
static volatile sig_atomic_t sigcode;
static volatile bool restart_wm;
xcb_atom_t WM_DELETE_WINDOW;
xcb_atom_t WM_TAKE_FOCUS;
xcb_atom_t WM_PROTOCOLS;
unsigned int selws = 0;
unsigned int prevws = 0;
Client *sel;
Client *clients;

void
quit(const Arg *arg) {
	(void)arg;
	cleanup();
	exit(sigcode);
}

void
restart(const Arg *arg) {
	(void) arg;
	restart_wm = true;
	sigcode = 1;
}

void
sigcatch(int sig) {
	PRINTF("caught signal %d\n", sig);
	switch (sig) {
	case SIGINT:
	case SIGTERM:
		sigcode = sig;
		break;
	case SIGHUP:
		restart_wm = true;
		sigcode = sig;
		break;
	}
}

void
run(void) {
	xcb_generic_event_t *ev;

	while (sigcode == 0) {
		xcb_flush(conn);
		if ((ev = xcb_wait_for_event(conn)) != NULL) {
			handleevent(ev);
			FREE(ev);
		}
		if (connection_has_error()) {
			cleanup();
			exit(1);
		}
	}
}

void
cleanup(void) {
	Client *c;

	for (c = clients; c; c = c->next) {
		xcb_reparent_window(conn, c->win, screen->root,
				    c->geom.x, c->geom.y);
	}
	xcb_aux_sync(conn);

	// FIXME
	while (stack) {
		PRINTF("unmap and free win %#x\n", stack->win);
		xcb_unmap_window(conn, stack->win);
		detach(stack);
		detachstack(stack);
		FREE(stack);
		ewmh_update_client_list(clients);
		focus(NULL);
	}
	ewmh_teardown();
	cursor_free_context();
	FREE(focus_color);
	FREE(unfocus_color);
	xcb_set_input_focus(conn, XCB_NONE, XCB_INPUT_FOCUS_POINTER_ROOT,
			    XCB_CURRENT_TIME);
	xcb_flush(conn);
	xcb_disconnect(conn);
	PRINTF("bye\n");
}

void
remanage_windows(void) {
	xcb_window_t                        sup;
	xcb_query_tree_cookie_t             qtc;
	xcb_query_tree_reply_t             *qtr;
	xcb_get_window_attributes_cookie_t  gac;
	xcb_get_window_attributes_reply_t  *gar;
	xcb_window_t *children;

	if (!ewmh_get_supporting_wm_check(&sup))
		warn("ewmh_get_supporting_wm_check fail\n");

	qtc = xcb_query_tree(conn, screen->root);
	if ((qtr = xcb_query_tree_reply(conn, qtc, NULL)) == NULL)
		return;

	children = xcb_query_tree_children(qtr);

	for (int i = 0; i < xcb_query_tree_children_length(qtr); i++) {
		if (children[i] == sup)
			continue;

		PRINTF("remanage_windows: %#x\n", children[i]);
		manage(children[i]);

		gac = xcb_get_window_attributes(conn, children[i]);
		gar = xcb_get_window_attributes_reply(conn, gac, NULL);
		if (!gar)
			continue;
		if (gar->override_redirect) {
			PRINTF("remanage_windows: skip %#x: "
			       "override_redirect set\n", children[i]);
			FREE(gar);
			continue;
		}
	}
	FREE(qtr);
}

void
setup(void) {
	xcb_generic_error_t *e;
	xcb_void_cookie_t    wac;
	uint32_t             vals[1];

	screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
	if (!screen)
		err("can't find screen.");

	/* subscribe to handler */
	vals[0] = ROOT_EVENT_MASK;
	wac = xcb_change_window_attributes_checked(conn, screen->root,
						   XCB_CW_EVENT_MASK, vals);
	e = xcb_request_check(conn, wac);
	if (e) {
		xcb_disconnect(conn);
		err("another window manager is running.");
	}

	getatom(&WM_DELETE_WINDOW, "WM_DELETE_WINDOW");
	getatom(&WM_TAKE_FOCUS, "WM_TAKE_FOCUS");
	getatom(&WM_PROTOCOLS, "WM_PROTOCOLS");

	updatenumlockmask();
	grabkeys();

	if (focus_color)
		focuscol = getcolor(focus_color);
	else
		focuscol = getcolor(DEFAULT_FOCUS_COLOR);

	if (unfocus_color)
		unfocuscol = getcolor(unfocus_color);
	else
		unfocuscol = getcolor(DEFAULT_UNFOCUS_COLOR);

	cursor_load_cursors();
	cursor_set_window_cursor(screen->root, XC_POINTER);

	ewmh_setup();
	remanage_windows();
	focus(NULL);
}

int
main(int argc, char **argv) {
	(void)argc;

	warn("welcome to acidWM %s\n", VERSION);

	conn = xcb_connect(NULL, &scrno);
	if (connection_has_error())
		return EXIT_FAILURE;

	/* while handling a signal, reset disposition to SIG_DFL */
	struct sigaction sa = {
		.sa_handler = sigcatch,
		.sa_flags = SA_RESETHAND };
	sigemptyset(&sa.sa_mask);

	if (sigaction(SIGINT, &sa, NULL) == -1 ||
	    sigaction(SIGTERM, &sa, NULL) == -1 ||
	    sigaction(SIGHUP, &sa, NULL) == -1) {
		warn("failed to add signal handlers.\n");
	}

	/* load config */
	/* char *rc_path = NULL; */
	/* if ((rc_path = find_config("acidwmrc"))) { */
	/* 	int err_line = parse_config(rc_path); */
	/* 	if (err_line > 0) */
	/* 		warn("parse_config: error on line %d.\n", err_line); */
	/* 	else if (err_line < 0) */
	/* 		warn("parse_config: fopen error\n"); */
	/* 	FREE(rc_path); */
	/* } else { */
	/* 	warn("no config file found. using default settings\n"); */
	/* } */

	xcb_aux_sync(conn);
	setup();
	run();
	cleanup();

	if (restart_wm)
		execvp(argv[0], argv);

	return EXIT_SUCCESS;
}

