/* See LICENSE file for copyright and license details. */
#ifndef WINDOW_H
#define WINDOW_H

#include <xcb/xcb.h>

void applyrules(Client *c);
void cycleclients(const Arg *arg);
void fitclient(Client *c);
void focus(Client *c);
void killselected(const Arg *arg);
void manage(xcb_window_t w);
void maximize(const Arg *arg);
void maximizeaxis(const Arg *arg);
void maximizeaxis_client(Client *c, uint16_t direction);
void maximizeclient(Client *c, bool doit);
void move(const Arg *arg);
void movewin(xcb_window_t win, int16_t x, int16_t y);
void moveresize_win(xcb_window_t win, int16_t x, int16_t y, uint16_t w, uint16_t h);
void raisewindow(xcb_drawable_t win);
void reparent(Client *c);
void resize(const Arg *arg);
void resizewin(xcb_window_t win, uint16_t w, uint16_t h);
void savegeometry(Client *c);
void send_client_message(Client *c, xcb_atom_t proto);
void setborder(Client *c, bool focus);
void setborderwidth(xcb_window_t win, uint16_t bw);
void showhide(Client *c);
void spawn(const Arg *arg);
void teleport(const Arg *arg);
void teleport_client(Client *c, uint16_t location);
void maximize_half(const Arg *arg);
void maximize_half_client(Client *c, uint16_t location);
void unmanage(Client *c);
Client *wintoclient(xcb_window_t w);

#endif

