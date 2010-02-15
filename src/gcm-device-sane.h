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

#ifndef __GCM_DEVICE_SANE_H
#define __GCM_DEVICE_SANE_H

#include <glib-object.h>

#include "gcm-device-udev.h"

G_BEGIN_DECLS

#define GCM_TYPE_DEVICE_SANE		(gcm_device_sane_get_type ())
#define GCM_DEVICE_SANE(o)		(G_TYPE_CHECK_INSTANCE_CAST ((o), GCM_TYPE_DEVICE_SANE, GcmDeviceSane))
#define GCM_IS_DEVICE_SANE(o)		(G_TYPE_CHECK_INSTANCE_TYPE ((o), GCM_TYPE_DEVICE_SANE))

typedef struct _GcmDeviceSanePrivate	GcmDeviceSanePrivate;
typedef struct _GcmDeviceSane		GcmDeviceSane;
typedef struct _GcmDeviceSaneClass	GcmDeviceSaneClass;

struct _GcmDeviceSane
{
	 GcmDeviceUdev			 parent;
	 GcmDeviceSanePrivate		*priv;
};

struct _GcmDeviceSaneClass
{
	GcmDeviceUdevClass		 parent_class;
};

GType		 gcm_device_sane_get_type		  	(void);
GcmDevice	*gcm_device_sane_new				(void);

G_END_DECLS

#endif /* __GCM_DEVICE_SANE_H */

