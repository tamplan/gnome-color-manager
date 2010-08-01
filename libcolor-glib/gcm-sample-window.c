/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2009-2010 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU General Public License Version 2
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/**
 * SECTION:gcm-sample_window
 * @short_description: Color lookup table object
 *
 * This object represents a color lookup table that is useful to manipulating
 * gamma values in a trivial RGB color space.
 */

#include "config.h"

#include <glib-object.h>
#include "gcm-sample-window.h"

#include "egg-debug.h"

static void     gcm_sample_window_finalize	(GObject     *object);

#define GCM_SAMPLE_WINDOW_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), GCM_TYPE_SAMPLE_WINDOW, GcmSampleWindowPrivate))

/**
 * GcmSampleWindowPrivate:
 *
 * Private #GcmSampleWindow data
 **/
struct _GcmSampleWindowPrivate
{
	GtkWidget			*image;
};

G_DEFINE_TYPE (GcmSampleWindow, gcm_sample_window, GTK_TYPE_WINDOW)

/**
 * gcm_sample_window_set_color:
 * @sample_window: a valid #GcmSampleWindow instance
 * @red: the red color value
 * @green: the green color value
 * @blue: the blue color value
 *
 * Sets the window to a specific color.
 *
 * Since: 0.0.1
 **/
void
gcm_sample_window_set_color (GcmSampleWindow *sample_window, guint8 red, guint8 green, guint8 blue)
{
	GdkPixbuf *pixbuf;
	gint width;
	gint height;
	gint i;
	guchar *data;
	guchar *pixels;
	GtkWindow *window = GTK_WINDOW (sample_window);

	/* get the window size */
	gtk_window_get_size (window, &width, &height);

	/* if no pixbuf, create it */
	pixbuf = gtk_image_get_pixbuf (GTK_IMAGE (sample_window->priv->image));
	if (pixbuf == NULL) {
		data = g_new0 (guchar, width*height*3);
		pixbuf = gdk_pixbuf_new_from_data (data, GDK_COLORSPACE_RGB, FALSE, 8, width, height, width*3, (GdkPixbufDestroyNotify) g_free, NULL);
		gtk_image_set_from_pixbuf (GTK_IMAGE (sample_window->priv->image), pixbuf);
	}

	/* set the pixel array */
	pixels = gdk_pixbuf_get_pixels (pixbuf);
	for (i=0; i<width*height*3; i+=3) {
		pixels[i+0] = red;
		pixels[i+1] = green;
		pixels[i+2] = blue;
	}
}

/**
 * gcm_sample_window_class_init:
 **/
static void
gcm_sample_window_class_init (GcmSampleWindowClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = gcm_sample_window_finalize;
	g_type_class_add_private (klass, sizeof (GcmSampleWindowPrivate));
}

/**
 * gcm_sample_window_init:
 **/
static void
gcm_sample_window_init (GcmSampleWindow *sample_window)
{
	GtkWindow *window = GTK_WINDOW (sample_window);
	sample_window->priv = GCM_SAMPLE_WINDOW_GET_PRIVATE (sample_window);
	sample_window->priv->image = gtk_image_new ();
	gtk_widget_set_size_request (sample_window->priv->image, 200, 200);
	gtk_container_add (GTK_CONTAINER (sample_window), sample_window->priv->image);
	gtk_widget_show (sample_window->priv->image);

	/* show on all virtual desktops */
	gtk_window_stick (window);
	gtk_window_set_default_size (window, 200, 200);

}

/**
 * gcm_sample_window_finalize:
 **/
static void
gcm_sample_window_finalize (GObject *object)
{
	GcmSampleWindow *sample_window = GCM_SAMPLE_WINDOW (object);
	GcmSampleWindowPrivate *priv = sample_window->priv;

	g_object_unref (priv->image);

	G_OBJECT_CLASS (gcm_sample_window_parent_class)->finalize (object);
}

/**
 * gcm_sample_window_new:
 *
 * Return value: a new #GcmSampleWindow object.
 **/
GtkWindow *
gcm_sample_window_new (void)
{
	GcmSampleWindow *sample_window;
	sample_window = g_object_new (GCM_TYPE_SAMPLE_WINDOW,
				      "accept-focus", FALSE,
				      "decorated", FALSE,
				      "default-height", 200,
				      "default-width", 200,
				      "deletable", FALSE,
				      "destroy-with-parent", TRUE,
				      "icon-name", "icc-profile",
				      "modal", TRUE,
				      "resizable", FALSE,
				      "skip-pager-hint", TRUE,
				      "skip-taskbar-hint", TRUE,
				      "title", "calibration square",
				      "type-hint", GDK_WINDOW_TYPE_HINT_SPLASHSCREEN,
				      "urgency-hint", TRUE,
				      "window-position", GTK_WIN_POS_CENTER_ALWAYS,
				      NULL);
	return GTK_WINDOW (sample_window);
}
