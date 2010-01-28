/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2009 Richard Hughes <richard@hughsie.com>
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

#ifndef __GCM_CALIBRATE_H
#define __GCM_CALIBRATE_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GCM_TYPE_CALIBRATE		(gcm_calibrate_get_type ())
#define GCM_CALIBRATE(o)		(G_TYPE_CHECK_INSTANCE_CAST ((o), GCM_TYPE_CALIBRATE, GcmCalibrate))
#define GCM_CALIBRATE_CLASS(k)		(G_TYPE_CHECK_CLASS_CAST((k), GCM_TYPE_CALIBRATE, GcmCalibrateClass))
#define GCM_IS_CALIBRATE(o)		(G_TYPE_CHECK_INSTANCE_TYPE ((o), GCM_TYPE_CALIBRATE))
#define GCM_IS_CALIBRATE_CLASS(k)	(G_TYPE_CHECK_CLASS_TYPE ((k), GCM_TYPE_CALIBRATE))
#define GCM_CALIBRATE_GET_CLASS(o)	(G_TYPE_INSTANCE_GET_CLASS ((o), GCM_TYPE_CALIBRATE, GcmCalibrateClass))

typedef struct _GcmCalibratePrivate	GcmCalibratePrivate;
typedef struct _GcmCalibrate		GcmCalibrate;
typedef struct _GcmCalibrateClass	GcmCalibrateClass;

struct _GcmCalibrate
{
	 GObject			 parent;
	 GcmCalibratePrivate		*priv;
};

struct _GcmCalibrateClass
{
	GObjectClass	parent_class;
	/* vtable */
	gboolean	 (*calibrate_display)		(GcmCalibrate	*calibrate,
							 GtkWindow	*window,
							 GError		**error);
	gboolean	 (*calibrate_device)		(GcmCalibrate	*calibrate,
							 GtkWindow	*window,
							 GError		**error);
	/* padding for future expansion */
	void (*_gcm_reserved1) (void);
	void (*_gcm_reserved2) (void);
	void (*_gcm_reserved3) (void);
	void (*_gcm_reserved4) (void);
	void (*_gcm_reserved5) (void);
};

GType		 gcm_calibrate_get_type			(void);
GcmCalibrate	*gcm_calibrate_new			(void);
gboolean	 gcm_calibrate_display			(GcmCalibrate	*calibrate,
							 GtkWindow	*window,
							 GError		**error);
gboolean	 gcm_calibrate_device			(GcmCalibrate	*calibrate,
							 GtkWindow	*window,
							 GError		**error);

G_END_DECLS

#endif /* __GCM_CALIBRATE_H */

