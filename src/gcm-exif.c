/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2010 Richard Hughes <richard@hughsie.com>
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
 * SECTION:gcm-exif
 * @short_description: EXIF metadata object
 *
 * This object represents a a file wih EXIF data.
 */

#include "config.h"

#include <glib-object.h>
//#include <math.h>
#include <tiff.h>
#include <tiffio.h>

#include "gcm-exif.h"

#include "egg-debug.h"

static void     gcm_exif_finalize	(GObject     *object);

#define GCM_EXIF_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), GCM_TYPE_EXIF, GcmExifPrivate))

/**
 * GcmExifPrivate:
 *
 * Private #GcmExif data
 **/
struct _GcmExifPrivate
{
	gchar				*manufacturer;
	gchar				*model;
	gchar				*serial;
};

enum {
	PROP_0,
	PROP_MANUFACTURER,
	PROP_MODEL,
	PROP_SERIAL,
	PROP_LAST
};

G_DEFINE_TYPE (GcmExif, gcm_exif, G_TYPE_OBJECT)

/**
 * gcm_exif_parse:
 **/
gboolean
gcm_exif_parse (GcmExif *exif, const gchar *filename, GError **error)
{
	gboolean ret = TRUE;
	const gchar *manufacturer = NULL;
	const gchar *model = NULL;
	const gchar *serial = NULL;
	TIFF *tiff;
	GcmExifPrivate *priv = exif->priv;

	g_return_val_if_fail (GCM_IS_EXIF (exif), FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* open file */
	tiff = TIFFOpen (filename, "r");
	TIFFGetField (tiff,TIFFTAG_MAKE, &manufacturer);
	TIFFGetField (tiff,TIFFTAG_MODEL, &model);
	TIFFGetField (tiff,TIFFTAG_CAMERASERIALNUMBER, &serial);

	/* we failed to get data */
	if (manufacturer == NULL || model == NULL) {
		g_set_error (error,
			     GCM_EXIF_ERROR,
			     GCM_EXIF_ERROR_NO_DATA,
			     "failed to get EXIF data from TIFF");
		ret = FALSE;
		goto out;
	}

	/* create copies for ourselves */
	priv->manufacturer = g_strdup (manufacturer);
	priv->model = g_strdup (model);
	priv->serial = g_strdup (serial);
out:
	TIFFClose (tiff);
	return ret;
}

/**
 * gcm_exif_get_manufacturer:
 **/
const gchar *
gcm_exif_get_manufacturer (GcmExif *exif)
{
	g_return_val_if_fail (GCM_IS_EXIF (exif), NULL);
	return exif->priv->manufacturer;
}

/**
 * gcm_exif_get_model:
 **/
const gchar *
gcm_exif_get_model (GcmExif *exif)
{
	g_return_val_if_fail (GCM_IS_EXIF (exif), NULL);
	return exif->priv->model;
}

/**
 * gcm_exif_get_serial:
 **/
const gchar *
gcm_exif_get_serial (GcmExif *exif)
{
	g_return_val_if_fail (GCM_IS_EXIF (exif), NULL);
	return exif->priv->serial;
}

/**
 * gcm_exif_get_property:
 **/
static void
gcm_exif_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
	GcmExif *exif = GCM_EXIF (object);
	GcmExifPrivate *priv = exif->priv;

	switch (prop_id) {
	case PROP_MANUFACTURER:
		g_value_set_string (value, priv->manufacturer);
		break;
	case PROP_MODEL:
		g_value_set_string (value, priv->model);
		break;
	case PROP_SERIAL:
		g_value_set_string (value, priv->serial);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

#if 0
/**
 * gcm_exif_set_property:
 **/
static void
gcm_exif_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
	switch (prop_id) {
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}
#endif

/**
 * gcm_exif_class_init:
 **/
static void
gcm_exif_class_init (GcmExifClass *klass)
{
	GParamSpec *pspec;
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = gcm_exif_finalize;
	object_class->get_property = gcm_exif_get_property;
//	object_class->set_property = gcm_exif_set_property;

	/**
	 * GcmExif:manufacturer:
	 */
	pspec = g_param_spec_string ("manufacturer", NULL, NULL,
				     NULL,
				     G_PARAM_READABLE);
	g_object_class_install_property (object_class, PROP_MANUFACTURER, pspec);

	/**
	 * GcmExif:model:
	 */
	pspec = g_param_spec_string ("model", NULL, NULL,
				     NULL,
				     G_PARAM_READABLE);
	g_object_class_install_property (object_class, PROP_MODEL, pspec);

	/**
	 * GcmExif:serial:
	 */
	pspec = g_param_spec_string ("serial", NULL, NULL,
				     NULL,
				     G_PARAM_READABLE);
	g_object_class_install_property (object_class, PROP_SERIAL, pspec);

	g_type_class_add_private (klass, sizeof (GcmExifPrivate));
}

/**
 * gcm_exif_init:
 **/
static void
gcm_exif_init (GcmExif *exif)
{
	exif->priv = GCM_EXIF_GET_PRIVATE (exif);
	exif->priv->manufacturer = NULL;
	exif->priv->model = NULL;
	exif->priv->serial = NULL;
}

/**
 * gcm_exif_finalize:
 **/
static void
gcm_exif_finalize (GObject *object)
{
	GcmExif *exif = GCM_EXIF (object);
	GcmExifPrivate *priv = exif->priv;

	g_free (priv->manufacturer);
	g_free (priv->model);
	g_free (priv->serial);

	G_OBJECT_CLASS (gcm_exif_parent_class)->finalize (object);
}

/**
 * gcm_exif_new:
 *
 * Return value: a new GcmExif object.
 **/
GcmExif *
gcm_exif_new (void)
{
	GcmExif *exif;
	exif = g_object_new (GCM_TYPE_EXIF, NULL);
	return GCM_EXIF (exif);
}

