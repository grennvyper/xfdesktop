/*
 *  xfdesktop - xfce4's desktop manager
 *
 *  Copyright (c) 2004-2008 Brian Tarricone, <bjt23@cornell.edu>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 *  Random portions taken from or inspired by the original xfdesktop for xfce4:
 *     Copyright (C) 2002-2003 Jasper Huijsmans (huysmans@users.sourceforge.net)
 *     Copyright (C) 2003 Benedikt Meurer <benedikt.meurer@unix-ag.uni-siegen.de>
 *  X event forwarding code:
 *     Copyright (c) 2004 Nils Rennebarth
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#ifdef HAVE_SIGNAL_H
#include <signal.h>
#endif

#include <X11/Xlib.h>

#include <gmodule.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>

#include <xfconf/xfconf.h>
#include <libxfcegui4/libxfcegui4.h>

#ifdef ENABLE_FILE_ICONS
#include <dbus/dbus-glib.h>
#include <thunar-vfs/thunar-vfs.h>
#endif

#include "xfdesktop-common.h"
#include "xfce-backdrop.h"
#include "xfce-desktop.h"
#include "menu.h"
#include "windowlist.h"

static SessionClient *client_session = NULL;
static gboolean is_session_managed = FALSE;

static void
session_logout(void)
{
    g_return_if_fail(is_session_managed);
    logout_session(client_session);
}

static void
session_die(gpointer user_data)
{
    gtk_main_quit();
}

static void
event_forward_to_rootwin(GdkScreen *gscreen, GdkEvent *event)
{
    XButtonEvent xev, xev2;
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_screen_get_display(gscreen));
    
    if(event->type == GDK_BUTTON_PRESS || event->type == GDK_BUTTON_RELEASE) {
        if(event->type == GDK_BUTTON_PRESS) {
            xev.type = ButtonPress;
            /*
             * rox has an option to disable the next
             * instruction. it is called "blackbox_hack". Does
             * anyone know why exactly it is needed?
             */
            XUngrabPointer(dpy, event->button.time);
        } else
            xev.type = ButtonRelease;
        
        xev.button = event->button.button;
        xev.x = event->button.x;    /* Needed for icewm */
        xev.y = event->button.y;
        xev.x_root = event->button.x_root;
        xev.y_root = event->button.y_root;
        xev.state = event->button.state;

        xev2.type = 0;
    } else if(event->type == GDK_SCROLL) {
        xev.type = ButtonPress;
        xev.button = event->scroll.direction + 4;
        xev.x = event->scroll.x;    /* Needed for icewm */
        xev.y = event->scroll.y;
        xev.x_root = event->scroll.x_root;
        xev.y_root = event->scroll.y_root;
        xev.state = event->scroll.state;
        
        xev2.type = ButtonRelease;
        xev2.button = xev.button;
    } else
        return;
    xev.window = GDK_WINDOW_XWINDOW(gdk_screen_get_root_window(gscreen));
    xev.root =  xev.window;
    xev.subwindow = None;
    xev.time = event->button.time;
    xev.same_screen = True;

    XSendEvent(dpy, xev.window, False, ButtonPressMask | ButtonReleaseMask,
            (XEvent *)&xev);
    if(xev2.type == 0)
        return;

    /* send button release for scroll event */
    xev2.window = xev.window;
    xev2.root = xev.root;
    xev2.subwindow = xev.subwindow;
    xev2.time = xev.time;
    xev2.x = xev.x;
    xev2.y = xev.y;
    xev2.x_root = xev.x_root;
    xev2.y_root = xev.y_root;
    xev2.state = xev.state;
    xev2.same_screen = xev.same_screen;

    XSendEvent(dpy, xev2.window, False, ButtonPressMask | ButtonReleaseMask,
            (XEvent *)&xev2);
}

static gboolean
scroll_cb(GtkWidget *w, GdkEventScroll *evt, gpointer user_data)
{
    event_forward_to_rootwin(gtk_widget_get_screen(w), (GdkEvent*)evt);
    return TRUE;
}

static gboolean 
reload_idle_cb(gpointer data)
{
    XfceDesktop **desktops = data;
    gint i, nscreens;

    nscreens = gdk_display_get_n_screens(gdk_display_get_default());
    for(i = 0; i < nscreens; ++i) {
        if(desktops[i])
            xfce_desktop_refresh(desktops[i]);
    }

    menu_reload();

    return FALSE;
}

static gboolean
client_message_received(GtkWidget *w, GdkEventClient *evt, gpointer user_data)
{
    if(evt->data_format == 8) {
        if(!strcmp(RELOAD_MESSAGE, evt->data.b)) {
            g_idle_add((GSourceFunc)reload_idle_cb, user_data);
            return TRUE;
        } else if(!strcmp(MENU_MESSAGE, evt->data.b)) {
            xfce_desktop_popup_root_menu(XFCE_DESKTOP(w), 0,
                                         GDK_CURRENT_TIME);
            return TRUE;
        } else if(!strcmp(WINDOWLIST_MESSAGE, evt->data.b)) {
            xfce_desktop_popup_secondary_root_menu(XFCE_DESKTOP(w), 0,
                                                   GDK_CURRENT_TIME);
            return TRUE;
        } else if(!strcmp(QUIT_MESSAGE, evt->data.b)) {
            if(is_session_managed) {
                client_session_set_restart_style(client_session,
                                                 SESSION_RESTART_IF_RUNNING);
            }
            gtk_main_quit();
            return TRUE;
        }
    }

    return FALSE;
}

static void
xfdesktop_handle_SIGUSR1(gint sig,
                         gpointer user_data)
{
    //settings_reload_all();  /* FIXME */
    menu_reload();
}

static void
xfdesktop_handle_quit_signals(gint sig,
                              gpointer user_data)
{
    gint main_level;
    
    for(main_level = gtk_main_level(); main_level > 0; --main_level)
        gtk_main_quit();
}

int
main(int argc, char **argv)
{
    GdkDisplay *gdpy;
    GtkWidget **desktops;
    gint i, nscreens;
    Window xid;
    XfconfChannel *channel = NULL;
    const gchar *message = NULL;
    gboolean already_running;
    gchar buf[1024];
    GError *error = NULL;
    
    /* bind gettext textdomain */
    xfce_textdomain(GETTEXT_PACKAGE, LOCALEDIR, "UTF-8");
    
#if defined(ENABLE_FILE_ICONS) || defined(USE_DESKTOP_MENU)
    g_thread_init(NULL);
#endif
#ifdef ENABLE_FILE_ICONS
    dbus_g_thread_init();
#endif
    gtk_init(&argc, &argv);
        
    if(argc > 1 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V"))) {
        g_print(_("This is %s version %s, running on Xfce %s.\n"), PACKAGE,
                VERSION, xfce_version_string());
        g_print(_("Built with GTK+ %d.%d.%d, linked with GTK+ %d.%d.%d."),
                GTK_MAJOR_VERSION,GTK_MINOR_VERSION, GTK_MICRO_VERSION,
                gtk_major_version, gtk_minor_version, gtk_micro_version);
        g_print("\n");
        g_print(_("Build options:\n"));
        g_print(_("    Desktop Menu:        %s\n"),
#ifdef USE_DESKTOP_MENU
                _("enabled")
#else
                _("disabled")
#endif
                );
        g_print(_("    Desktop Icons:       %s\n"),
#ifdef ENABLE_DESKTOP_ICONS
                _("enabled")
#else
                _("disabled")
#endif
                );
        g_print(_("    Desktop File Icons:  %s\n"),
#ifdef ENABLE_FILE_ICONS
                _("enabled")
#else
                _("disabled")
#endif
                );
        
        return 0;
    }

    if(argc > 1) {
        const gchar *argument = argv[1];
        
        /* allow both --(option) and -(option) */
        if('-' == argument[0] && '-' == argument[1])
            ++argument;
        
        if(!strcmp("-reload", argument))
            message = RELOAD_MESSAGE;
        else if(!strcmp("-menu", argument))
            message = MENU_MESSAGE;
        else if(!strcmp("-windowlist", argument))
            message = WINDOWLIST_MESSAGE;
        else if(!strcmp("-quit", argument))
            message = QUIT_MESSAGE;
        else if(!strcmp("--sm-client-id", argv[1])) {
            /* do nothing */
        } else {
            g_printerr(_("%s: Unknown option: %s\n"), PACKAGE, argv[1]);
            g_printerr(_("Options are:\n"));
            g_printerr(_("    --reload      Reload all settings, refresh image list\n"));
            g_printerr(_("    --menu        Pop up the menu (at the current mouse position)\n"));
            g_printerr(_("    --windowlist  Pop up the window list (at the current mouse position)\n"));
            g_printerr(_("    --quit        Cause xfdesktop to quit\n"));
            
            return 1;
        }
    }
    
    signal(SIGPIPE, SIG_IGN);

    already_running = xfdesktop_check_is_running(&xid);
    if(already_running) {
        if(!message) {
            g_printerr("%s[%d] is already running; assuming --reload\n",
                       PACKAGE, (int)getpid());
            message = RELOAD_MESSAGE;
        }
    }

    g_print("%s[%d]: starting up\n", PACKAGE, getpid());
    
    if(message) {
        if(!already_running)
            g_printerr(_("%s is not running.\n"), PACKAGE);
        else
            xfdesktop_send_client_message(xid, message);

        return (already_running ? 0 : 1);
    }
    
    client_session = client_session_new(argc, argv, NULL,
                                        SESSION_RESTART_IMMEDIATELY, 40);
    client_session->die = session_die;
    is_session_managed = session_init(client_session);

#ifdef ENABLE_FILE_ICONS
    thunar_vfs_init();
#endif
    
    gdpy = gdk_display_get_default();

    if(!xfconf_init(&error)) {
        g_warning("%s: unable to connect to settings daemon: %s.  Defaults will be used",
                  PACKAGE, error->message);
        g_error_free(error);
        error = NULL;
    } else
        channel = xfconf_channel_new(XFDESKTOP_CHANNEL);

    nscreens = gdk_display_get_n_screens(gdpy);
    desktops = g_new0(GtkWidget *, nscreens);
    for(i = 0; i < nscreens; i++) {
        g_snprintf(buf, sizeof(buf), "/backdrop/screen%d/", i);
        desktops[i] = xfce_desktop_new(gdk_display_get_screen(gdpy, i),
                                       channel, buf);
        gtk_widget_add_events(desktops[i], GDK_BUTTON_PRESS_MASK
                              | GDK_BUTTON_RELEASE_MASK | GDK_SCROLL_MASK);
        g_signal_connect(G_OBJECT(desktops[i]), "scroll-event",
                         G_CALLBACK(scroll_cb), NULL);
        g_signal_connect(G_OBJECT(desktops[i]), "client-event",
                         G_CALLBACK(client_message_received), desktops);
        menu_attach(XFCE_DESKTOP(desktops[i]));
        windowlist_attach(XFCE_DESKTOP(desktops[i]));
        gtk_widget_show(desktops[i]);
        gdk_window_lower(desktops[i]->window);
    }
    
    if(is_session_managed) {
        for(i = 0; i < nscreens; ++i)
            xfce_desktop_set_session_logout_func(XFCE_DESKTOP(desktops[i]),
                                                 session_logout);
    }
    
    menu_init(channel);
    windowlist_init(channel);
    
    if(xfce_posix_signal_handler_init(&error)) {
        xfce_posix_signal_handler_set_handler(SIGHUP,
                                              xfdesktop_handle_quit_signals,
                                              desktops, NULL);
        xfce_posix_signal_handler_set_handler(SIGINT,
                                              xfdesktop_handle_quit_signals,
                                              desktops, NULL);
        xfce_posix_signal_handler_set_handler(SIGTERM,
                                              xfdesktop_handle_quit_signals,
                                              desktops, NULL);
        xfce_posix_signal_handler_set_handler(SIGUSR1,
                                              xfdesktop_handle_SIGUSR1,
                                              desktops, NULL);
    } else {
        g_warning("Unable to set up POSIX signal handlers: %s", error->message);
        g_error_free(error);
    }

    gtk_main();
    
    menu_cleanup();
    windowlist_cleanup();
    
    for(i = 0; i < nscreens; i++)
        gtk_widget_destroy(desktops[i]);
    g_free(desktops);
    
    xfconf_shutdown();
    
#ifdef ENABLE_FILE_ICONS
    thunar_vfs_shutdown();
#endif
    
    return 0;
}
