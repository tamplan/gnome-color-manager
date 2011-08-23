/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2009-2011 Richard Hughes <richard@hughsie.com>
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

#include "config.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <glib/gstdio.h>
#include <locale.h>
#include <canberra-gtk.h>
#include <colord.h>
#include <lcms2.h>
#include <stdlib.h>

#include "gcm-utils.h"
#include "gcm-debug.h"
#include "gcm-brightness.h"
#include "gcm-calibrate.h"
#include "gcm-calibrate-argyll.h"

typedef enum {
	GCM_CALIBRATE_PAGE_INTRO,
	GCM_CALIBRATE_PAGE_DISPLAY_KIND,
	GCM_CALIBRATE_PAGE_DISPLAY_TEMPERATURE,
	GCM_CALIBRATE_PAGE_DISPLAY_CONFIG,
	GCM_CALIBRATE_PAGE_INSTALL_ARGYLLCMS,
	GCM_CALIBRATE_PAGE_INSTALL_TARGETS,
	GCM_CALIBRATE_PAGE_PRECISION,
	GCM_CALIBRATE_PAGE_PRINT_KIND,
	GCM_CALIBRATE_PAGE_TARGET_KIND,
	GCM_CALIBRATE_PAGE_SENSOR,
	GCM_CALIBRATE_PAGE_ACTION,
	GCM_CALIBRATE_PAGE_FAILURE,
	GCM_CALIBRATE_PAGE_TITLE,
	GCM_CALIBRATE_PAGE_LAST
} GcmCalibratePage;

typedef struct {
	GtkApplication	*application;
	CdClient	*client;
	GcmCalibrate	*calibrate;
	CdDevice	*device;
	CdDeviceKind	 device_kind;
	GCancellable	*cancellable;
	CdSensor	*sensor;
	gchar		*device_id;
	guint		 xid;
	GtkWindow	*main_window;
	GPtrArray	*pages;
	GcmBrightness	*brightness;
	guint		 old_brightness;
	gboolean	 internal_lcd;
	GtkWidget	*reference_preview;
	GtkWidget	*action_title;
	GtkWidget	*action_message;
	GtkWidget	*action_image;
	gboolean	 has_pending_interaction;
	gboolean	 started_calibration;
	GcmCalibratePage current_page;
} GcmCalibratePriv;

/**
 * gcm_window_set_parent_xid:
 **/
static void
gcm_window_set_parent_xid (GtkWindow *window, guint32 xid)
{
	GdkDisplay *display;
	GdkWindow *parent_window;
	GdkWindow *our_window;

	display = gdk_display_get_default ();
	parent_window = gdk_x11_window_foreign_new_for_display (display, xid);
	if (parent_window == NULL) {
		g_warning ("failed to get parent window");
		return;
	}
	our_window = gtk_widget_get_window (GTK_WIDGET (window));
	if (our_window == NULL) {
		g_warning ("failed to get our window");
		return;
	}

	/* set this above our parent */
	gtk_window_set_modal (window, TRUE);
	gdk_window_set_transient_for (our_window, parent_window);
	gtk_window_set_title (window, "");
}

/**
 * gcm_calib_activate_cb:
 **/
static void
gcm_calib_activate_cb (GApplication *application, GcmCalibratePriv *calib)
{
	gtk_window_present (calib->main_window);
}

static void
gcm_calib_confirm_quit_cb (GtkDialog *dialog,
			   gint response_id,
			   GcmCalibratePriv *calib)
{
	if (response_id != GTK_RESPONSE_CLOSE) {
		gtk_widget_destroy (GTK_WIDGET (dialog));
		return;
	}
	gcm_calibrate_interaction (calib->calibrate, GTK_RESPONSE_CANCEL);
	g_application_release (G_APPLICATION (calib->application));
}

/**
 * gcm_calib_confirm_quit:
 **/
static void
gcm_calib_confirm_quit (GcmCalibratePriv *calib)
{
	GtkWidget *dialog;

	/* do not ask for confirmation on the initial page */
	if (calib->current_page == GCM_CALIBRATE_PAGE_INTRO)
		g_application_release (G_APPLICATION (calib->application));

	dialog = gtk_message_dialog_new (GTK_WINDOW (calib->main_window),
					 GTK_DIALOG_MODAL,
					 GTK_MESSAGE_QUESTION,
					 GTK_BUTTONS_NONE,
					 "%s", _("Calibration is not complete"));
	gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
						  "%s",
						  _("Are you sure you want to cancel the calibration?"));
	/* TRANSLATORS: button text */
	gtk_dialog_add_button (GTK_DIALOG (dialog),
			       _("Continue calibration"),
			       GTK_RESPONSE_CANCEL);
	/* TRANSLATORS: button text */
	gtk_dialog_add_button (GTK_DIALOG (dialog),
			       _("Cancel and close"),
			       GTK_RESPONSE_CLOSE);
	g_signal_connect (dialog, "response",
			  G_CALLBACK (gcm_calib_confirm_quit_cb),
			  calib);
	gtk_widget_show (dialog);
}

/**
 * gcm_calib_delete_event_cb:
 **/
static gboolean
gcm_calib_delete_event_cb (GtkWidget *widget, GdkEvent *event, GcmCalibratePriv *calib)
{
	gcm_calib_confirm_quit (calib);
	return FALSE;
}

/**
 * gcm_calib_assistant_cancel_cb:
 **/
static void
gcm_calib_assistant_cancel_cb (GtkAssistant *assistant, GcmCalibratePriv *calib)
{
	gcm_calib_confirm_quit (calib);
}

/**
 * gcm_calib_assistant_close_cb:
 **/
static void
gcm_calib_assistant_close_cb (GtkAssistant *assistant, GcmCalibratePriv *calib)
{
	g_application_release (G_APPLICATION (calib->application));
}

/**
 * gcm_calib_device_inihibit:
 **/
static void
gcm_calib_device_inihibit (GcmCalibratePriv *calib)
{
	gboolean ret;
	GError *error = NULL;

	/* invalid */
	if (calib->device == NULL)
		return;

	/* mark device to be profiled in colord */
	ret = cd_device_profiling_inhibit_sync (calib->device,
						calib->cancellable,
						&error);
	if (!ret) {
		g_warning ("failed to inhibit device: %s",
			   error->message);
		g_clear_error (&error);
	}

	/* set the brightness to max */
	if (calib->device_kind == CD_DEVICE_KIND_DISPLAY &&
	    calib->internal_lcd) {
		ret = gcm_brightness_get_percentage (calib->brightness,
						     &calib->old_brightness,
						     &error);
		if (!ret) {
			g_warning ("failed to get brightness: %s",
				   error->message);
			g_clear_error (&error);
		}
		ret = gcm_brightness_set_percentage (calib->brightness,
						     100,
						     &error);
		if (!ret) {
			g_warning ("failed to set brightness: %s",
				   error->message);
			g_error_free (error);
		}
	}
}

/**
 * gcm_calib_device_uninihibit:
 **/
static void
gcm_calib_device_uninihibit (GcmCalibratePriv *calib)
{
	gboolean ret;
	GError *error = NULL;

	/* invalid */
	if (calib->device == NULL)
		return;

	/* mark device to be profiled in colord */
	ret = cd_device_profiling_uninhibit_sync (calib->device,
						  calib->cancellable,
						  &error);
	if (!ret) {
		g_warning ("failed to uninhibit device: %s",
			   error->message);
		g_clear_error (&error);
	}

	/* reset the brightness to what it was before */
	if (calib->device_kind == CD_DEVICE_KIND_DISPLAY &&
	    calib->old_brightness != G_MAXUINT &&
	    calib->internal_lcd) {
		ret = gcm_brightness_set_percentage (calib->brightness,
						     calib->old_brightness,
						     &error);
		if (!ret) {
			g_warning ("failed to set brightness: %s",
				   error->message);
			g_error_free (error);
		}
	}
}

/**
 * gcm_calib_play_sound:
 **/
static void
gcm_calib_play_sound (GcmCalibratePriv *calib)
{
	/* play sound from the naming spec */
	ca_context_play (ca_gtk_context_get (), 0,
			 CA_PROP_EVENT_ID, "complete",
			 /* TRANSLATORS: this is the application name for libcanberra */
			 CA_PROP_APPLICATION_NAME, _("GNOME Color Manager"),
			 /* TRANSLATORS: this is the sound description */
			 CA_PROP_EVENT_DESCRIPTION, _("Profiling completed"),
			 NULL);
}


/**
 * gcm_calib_get_vbox_for_page:
 **/
static GtkWidget *
gcm_calib_get_vbox_for_page (GcmCalibratePriv *calib,
			     GcmCalibratePage page)
{
	guint i;
	GtkWidget *tmp;
	GcmCalibratePage page_tmp;

	for (i=0; i<calib->pages->len; i++) {
		tmp = g_ptr_array_index (calib->pages, i);
		page_tmp = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (tmp),
								"GcmCalibrateMain::Index"));
		if (page_tmp == page)
			return tmp;
	}
	return NULL;
}

static wchar_t *
utf8_to_wchar_t (const char *src)
{
	gsize len;
	gsize converted;
	wchar_t *buf = NULL;

	len = mbstowcs (NULL, src, 0);
	if (len == (gsize) -1) {
		g_warning ("Invalid UTF-8 in string %s", src);
		goto out;
	}
	len += 1;
	buf = g_malloc (sizeof (wchar_t) * len);
	converted = mbstowcs (buf, src, len - 1);
	g_assert (converted != (gsize) -1);
	buf[converted] = '\0';
out:
	return buf;
}

static cmsBool
_cmsDictAddEntryAscii (cmsHANDLE dict,
		       const gchar *key,
		       const gchar *value)
{
	cmsBool ret = FALSE;
	wchar_t *mb_key = NULL;
	wchar_t *mb_value = NULL;

	mb_key = utf8_to_wchar_t (key);
	if (mb_key == NULL)
		goto out;
	mb_value = utf8_to_wchar_t (value);
	if (mb_value == NULL)
		goto out;
	ret = cmsDictAddEntry (dict, mb_key, mb_value, NULL, NULL);
out:
	g_free (mb_key);
	g_free (mb_value);
	return ret;
}

static gboolean
gcm_calib_set_extra_metadata (const gchar *filename, GError **error)
{
	cmsHANDLE dict = NULL;
	cmsHPROFILE lcms_profile;
	gboolean ret = TRUE;
	gchar *data = NULL;
	gsize len;

	/* parse */
	ret = g_file_get_contents (filename, &data, &len, error);
	if (!ret)
		goto out;
	lcms_profile = cmsOpenProfileFromMem (data, len);
	if (lcms_profile == NULL) {
		g_set_error_literal (error, 1, 0,
				     "failed to open profile");
		ret = FALSE;
		goto out;
	}

	/* just create a new dict */
	dict = cmsDictAlloc (NULL);
	_cmsDictAddEntryAscii (dict,
			       CD_PROFILE_METADATA_CMF_PRODUCT,
			       PACKAGE_NAME);
	_cmsDictAddEntryAscii (dict,
			       CD_PROFILE_METADATA_CMF_BINARY,
			       "gcm-calibrate");
	_cmsDictAddEntryAscii (dict,
			       CD_PROFILE_METADATA_CMF_VERSION,
			       PACKAGE_VERSION);
	_cmsDictAddEntryAscii (dict,
			       CD_PROFILE_METADATA_DATA_SOURCE,
			       CD_PROFILE_METADATA_DATA_SOURCE_CALIB);

	/* just write dict */
	ret = cmsWriteTag (lcms_profile, cmsSigMetaTag, dict);
	if (!ret) {
		g_set_error_literal (error, 1, 0,
				     "cannot write metadata");
		goto out;
	}

	/* write profile id */
	ret = cmsMD5computeID (lcms_profile);
	if (!ret) {
		g_set_error_literal (error, 1, 0,
				     "failed to write profile id");
		goto out;
	}

	/* save, TODO: get error */
	cmsSaveProfileToFile (lcms_profile, filename);
	ret = TRUE;
out:
	g_free (data);
	if (dict != NULL)
		cmsDictFree (dict);
	return ret;
}

static gboolean
gcm_calib_start_idle_cb (gpointer user_data)
{
	CdProfile *profile = NULL;
	const gchar *filename;
	gboolean ret;
	GError *error = NULL;
	GFile *file = NULL;
	GtkWidget *vbox;
	GcmCalibratePriv *calib = (GcmCalibratePriv *) user_data;
	GtkAssistant *assistant = GTK_ASSISTANT (calib->main_window);

	/* actuall do the action */
	calib->started_calibration = TRUE;
	ret = gcm_calibrate_display (calib->calibrate,
				     calib->device,
				     calib->main_window,
				     &error);
	if (!ret) {
		gcm_calibrate_set_title (calib->calibrate, _("Failed to calibrate"));
		gcm_calibrate_set_message (calib->calibrate, error->message);
		gcm_calibrate_set_image (calib->calibrate, NULL);

		g_warning ("failed to calibrate: %s",
			   error->message);
		g_error_free (error);

		/* mark this box as the end */
		vbox = gcm_calib_get_vbox_for_page (calib, GCM_CALIBRATE_PAGE_ACTION);
		gtk_assistant_set_page_type (assistant, vbox, GTK_ASSISTANT_PAGE_SUMMARY);
		goto out;
	}

	/* get profile */
	filename = gcm_calibrate_get_filename_result (calib->calibrate);
	if (filename == NULL) {
		g_warning ("failed to get filename from calibration");
		goto out;
	}

	/* set some private properties */
	ret = gcm_calib_set_extra_metadata (filename, &error);
	if (!ret) {
		g_warning ("failed to set extra metadata: %s",
			   error->message);
		g_error_free (error);
		goto out;
	}

	/* copy the ICC file to the proper location */
	file = g_file_new_for_path (filename);
	profile = cd_client_import_profile_sync (calib->client,
						 file,
						 calib->cancellable,
						 &error);
	if (profile == NULL) {
		g_warning ("failed to find calibration profile: %s",
			   error->message);
		g_error_free (error);
		goto out;
	}
	ret = cd_device_add_profile_sync (calib->device,
					  CD_DEVICE_RELATION_HARD,
					  profile,
					  calib->cancellable,
					  &error);
	if (!ret) {
		g_warning ("failed to add %s to %s: %s",
			   cd_profile_get_object_path (profile),
			   cd_device_get_object_path (calib->device),
			   error->message);
		g_error_free (error);
		goto out;
	}

	/* remove temporary file */
	g_unlink (filename);

out:
	if (profile != NULL)
		g_object_unref (profile);
	if (file != NULL)
		g_object_unref (file);
	return FALSE;
}

static gint
gcm_calib_assistant_page_forward_cb (gint current_page, gpointer user_data)
{
	GtkWidget *vbox;
	GcmCalibratePriv *calib = (GcmCalibratePriv *) user_data;

	/* shouldn't happen... */
	if (calib->current_page != GCM_CALIBRATE_PAGE_ACTION)
		return current_page + 1;

	if (!calib->has_pending_interaction)
		return current_page;

	/* continue calibration */
	gcm_calibrate_interaction (calib->calibrate, GTK_RESPONSE_OK);

	/* no longer allow forward */
	vbox = gcm_calib_get_vbox_for_page (calib,
					    GCM_CALIBRATE_PAGE_ACTION);

	gtk_assistant_set_page_complete (GTK_ASSISTANT (calib->main_window),
					 vbox, FALSE);
	return current_page;
}

/**
 * gcm_calib_assistant_prepare_cb:
 **/
static gboolean
gcm_calib_assistant_prepare_cb (GtkAssistant *assistant,
				GtkWidget *page_widget,
				GcmCalibratePriv *calib)
{
	calib->current_page = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (page_widget),
								   "GcmCalibrateMain::Index"));
	switch (calib->current_page) {
	case GCM_CALIBRATE_PAGE_LAST:
		gcm_calib_device_uninihibit (calib);
		gcm_calib_play_sound (calib);
		break;
	case GCM_CALIBRATE_PAGE_ACTION:
		g_debug ("lights! camera! action!");
		if (!calib->started_calibration) {
			gcm_calib_device_inihibit (calib);
			g_idle_add (gcm_calib_start_idle_cb, calib);
		}
		break;
	default:
		break;
	}

	/* ensure we cancel argyllcms if the user clicks back */
	if (calib->current_page != GCM_CALIBRATE_PAGE_ACTION &&
	    calib->started_calibration) {
		gcm_calibrate_interaction (calib->calibrate,
					   GTK_RESPONSE_CANCEL);
		gcm_calib_device_uninihibit (calib);
		calib->started_calibration = FALSE;
	}

	/* forward on the action page just unsticks the calibration */
	if (calib->current_page == GCM_CALIBRATE_PAGE_ACTION) {
		gtk_assistant_set_forward_page_func (assistant,
						     gcm_calib_assistant_page_forward_cb,
						     calib,
						     NULL);
	} else {
		gtk_assistant_set_forward_page_func (assistant,
						     NULL, NULL, NULL);
	}

	/* use the default on each page */
	switch (calib->current_page) {
	case GCM_CALIBRATE_PAGE_INSTALL_ARGYLLCMS:
	case GCM_CALIBRATE_PAGE_SENSOR:
	case GCM_CALIBRATE_PAGE_ACTION:
		break;
	default:
		gtk_assistant_set_page_complete (assistant, page_widget, TRUE);
		break;
	}
	return FALSE;
}

/**
 * gcm_calib_add_page_title:
 **/
static GtkWidget *
gcm_calib_add_page_title (GcmCalibratePriv *calib, const gchar *text)
{
	GtkWidget *label;
	GtkWidget *hbox;
	GtkWidget *vbox;
	gchar *markup;

	markup = g_strdup_printf ("<span size=\"large\" font_weight=\"bold\">%s</span>", text);
	label = gtk_label_new (NULL);
	gtk_label_set_markup (GTK_LABEL (label), markup);

	/* make left aligned */
	hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_box_pack_start (GTK_BOX (hbox), label, FALSE, TRUE, 0);

	/* header */
	vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 20);
	gtk_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);

	g_free (markup);
	return vbox;
}

static gboolean
gcm_calib_label_activate_link_cb (GtkLabel *label,
				  gchar *uri,
				  GcmCalibratePriv *calib)
{
	gboolean ret;
	GError *error = NULL;
	ret = g_spawn_command_line_async (BINDIR "/gnome-control-center color", &error);
	if (!ret) {
		g_warning ("failed to launch the control center: %s",
			   error->message);
		g_error_free (error);
	}
	return ret;
}

/**
 * gcm_calib_add_page_para:
 **/
static GtkWidget *
gcm_calib_add_page_para (GtkWidget *vbox, const gchar *text)
{
	GtkWidget *label;
	GtkWidget *hbox;

	label = gtk_label_new (NULL);
	g_signal_connect (label, "activate-link",
			  G_CALLBACK (gcm_calib_label_activate_link_cb),
			  NULL);
	gtk_label_set_markup (GTK_LABEL (label), text);
	gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
	gtk_label_set_width_chars (GTK_LABEL (label), 40);
	gtk_misc_set_alignment (GTK_MISC (label), 0.0f, 0.0f);

	/* make left aligned */
	hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_box_pack_start (GTK_BOX (hbox), label, FALSE, TRUE, 0);
	gtk_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);
	return label;
}

/**
 * gcm_calib_add_page_bullet:
 **/
static void
gcm_calib_add_page_bullet (GtkWidget *vbox, const gchar *text)
{
	gchar *markup;
	markup = g_strdup_printf ("• %s", text);
	gcm_calib_add_page_para (vbox, markup);
	g_free (markup);
}

/**
 * gcm_calib_setup_page_intro:
 **/
static void
gcm_calib_setup_page_intro (GcmCalibratePriv *calib)
{
	GtkWidget *vbox;
	GtkWidget *content;
	GtkAssistant *assistant = GTK_ASSISTANT (calib->main_window);

	/* TRANSLATORS: this is intro page text */
	switch (calib->device_kind) {
	case CD_DEVICE_KIND_CAMERA:
	case CD_DEVICE_KIND_WEBCAM:
		/* TRANSLATORS: this is the page title */
		vbox = gcm_calib_add_page_title (calib, _("Calibrate your camera"));
		break;
	case CD_DEVICE_KIND_DISPLAY:
		/* TRANSLATORS: this is the page title */
		vbox = gcm_calib_add_page_title (calib, _("Calibrate your display"));
		break;
	case CD_DEVICE_KIND_PRINTER:
		/* TRANSLATORS: this is the page title */
		vbox = gcm_calib_add_page_title (calib, _("Calibrate your printer"));
		break;
	default:
		/* TRANSLATORS: this is the page title */
		vbox = gcm_calib_add_page_title (calib, _("Calibrate your device"));
		break;
	}

	/* main contents */
	content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_box_pack_start (GTK_BOX (vbox), content, FALSE, FALSE, 0);

	/* TRANSLATORS: this is intro page text */
	switch (calib->device_kind) {
	case CD_DEVICE_KIND_DISPLAY:
		/* TRANSLATORS: this is the final intro page text */
		gcm_calib_add_page_para (content, _("Any existing screen correction will be temporarily turned off and the brightness set to maximum."));
		break;
	default:
		break;
	}

	/* TRANSLATORS: this is the final intro page text */
	gcm_calib_add_page_para (content, _("You can cancel this process at any stage by pressing the cancel button."));

	/* add to assistant */
	gtk_assistant_append_page (assistant, vbox);
	gtk_assistant_set_page_type (assistant, vbox, GTK_ASSISTANT_PAGE_INTRO);
	/* TRANSLATORS: this is the calibration wizard page title */
	gtk_assistant_set_page_title (assistant, vbox, _("Introduction"));
	gtk_assistant_set_page_complete (assistant, vbox, FALSE);
	g_ptr_array_add (calib->pages, vbox);
	g_object_set_data (G_OBJECT (vbox),
			   "GcmCalibrateMain::Index",
			   GUINT_TO_POINTER (GCM_CALIBRATE_PAGE_INTRO));

	/* show page */
	gtk_widget_show_all (vbox);
}

/**
 * gcm_calib_setup_page_summary:
 **/
static void
gcm_calib_setup_page_summary (GcmCalibratePriv *calib)
{
	GtkWidget *vbox;
	GtkWidget *content;
	GtkWidget *image;
	GtkAssistant *assistant = GTK_ASSISTANT (calib->main_window);

	/* TRANSLATORS: this is the page title */
	vbox = gcm_calib_add_page_title (calib, _("All done!"));

	/* main contents */
	content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_box_pack_start (GTK_BOX (vbox), content, FALSE, FALSE, 0);

	switch (calib->device_kind) {
	case CD_DEVICE_KIND_CAMERA:
	case CD_DEVICE_KIND_WEBCAM:
		/* TRANSLATORS: this is the final summary */
		gcm_calib_add_page_para (content, _("The camera has been calibrated successfully."));
		break;
	case CD_DEVICE_KIND_DISPLAY:
		/* TRANSLATORS: this is the final summary */
		gcm_calib_add_page_para (content, _("The display has been calibrated successfully."));
		gcm_calib_add_page_para (content, _("You may notice the screen is a slightly different color."));
		break;
	case CD_DEVICE_KIND_PRINTER:
		/* TRANSLATORS: this is the final summary */
		gcm_calib_add_page_para (content, _("The printer has been calibrated successfully."));
		gcm_calib_add_page_para (content, _("Docments printed from this device will be color managed."));
		break;
	default:
		/* TRANSLATORS: this is the final summary */
		gcm_calib_add_page_para (content, _("The device has been calibrated successfully."));
		break;
	}

	/* only display the backlink if not launched from the control center itself */
	if (calib->xid == 0) {
		/* TRANSLATORS: this is the final summary */
		gcm_calib_add_page_para (content, _("To view details about the new profile or to undo the calibration visit the <a href=\"control-center://color\">control center</a>."));
	}
	/* add image for success */
	image = gtk_image_new ();
	gtk_image_set_from_icon_name (GTK_IMAGE (image), "face-smile", GTK_ICON_SIZE_DIALOG);
	gtk_box_pack_start (GTK_BOX (vbox), image, FALSE, FALSE, 0);

	/* add to assistant */
	gtk_assistant_append_page (assistant, vbox);
	gtk_assistant_set_page_type (assistant, vbox, GTK_ASSISTANT_PAGE_SUMMARY);
	/* TRANSLATORS: this is the calibration wizard page title */
	gtk_assistant_set_page_title (assistant, vbox, _("Summary"));
	gtk_assistant_set_page_complete (assistant, vbox, FALSE);
	g_ptr_array_add (calib->pages, vbox);
	g_object_set_data (G_OBJECT (vbox),
			   "GcmCalibrateMain::Index",
			   GUINT_TO_POINTER (GCM_CALIBRATE_PAGE_LAST));

	/* show page */
	gtk_widget_show_all (vbox);
}

/**
 * gcm_calib_setup_page_action:
 **/
static void
gcm_calib_setup_page_action (GcmCalibratePriv *calib)
{
	GtkWidget *vbox;
	GtkWidget *content;
	GList *list;
	GList *list2;
	GtkAssistant *assistant = GTK_ASSISTANT (calib->main_window);

	/* TRANSLATORS: this is the page title */
	vbox = gcm_calib_add_page_title (calib, _("Performing calibration"));

	/* grab title */
	list = gtk_container_get_children (GTK_CONTAINER (vbox));
	list2 = gtk_container_get_children (GTK_CONTAINER (list->data));
	calib->action_title = list2->data;
	g_list_free (list);
	g_list_free (list2);

	/* main contents */
	content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_box_pack_start (GTK_BOX (vbox), content, FALSE, FALSE, 0);

	calib->action_message = gcm_calib_add_page_para (content, _("Calibration is about to start"));

	/* add image for success */
	calib->action_image = gtk_image_new ();
	gtk_image_set_from_icon_name (GTK_IMAGE (calib->action_image), "face-frown", GTK_ICON_SIZE_DIALOG);
	gtk_box_pack_start (GTK_BOX (content), calib->action_image, FALSE, FALSE, 0);

	/* add content widget */
	gcm_calibrate_set_content_widget (calib->calibrate, vbox);

	/* add to assistant */
	gtk_assistant_append_page (assistant, vbox);
	gtk_assistant_set_page_type (assistant, vbox, GTK_ASSISTANT_PAGE_CONTENT);
	/* TRANSLATORS: this is the calibration wizard page title */
	gtk_assistant_set_page_title (assistant, vbox, _("Action"));
	gtk_assistant_set_page_complete (assistant, vbox, FALSE);
	g_ptr_array_add (calib->pages, vbox);
	g_object_set_data (G_OBJECT (vbox),
			   "GcmCalibrateMain::Index",
			   GUINT_TO_POINTER (GCM_CALIBRATE_PAGE_ACTION));

	/* show page */
	gtk_widget_show_all (vbox);
	gtk_widget_hide (calib->action_image);
}

/**
 * gcm_calib_setup_page_display_configure_wait:
 **/
static void
gcm_calib_setup_page_display_configure_wait (GcmCalibratePriv *calib)
{
	GtkWidget *vbox;
	GtkWidget *content;
	GtkAssistant *assistant = GTK_ASSISTANT (calib->main_window);

	/* TRANSLATORS: dialog message, preface */
	vbox = gcm_calib_add_page_title (calib, _("Calibration checklist"));

	/* main contents */
	content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_box_pack_start (GTK_BOX (vbox), content, FALSE, FALSE, 0);

	/* TRANSLATORS: this is intro page text */
	gcm_calib_add_page_para (content, _("Before calibrating the display, it is recommended to configure your display with the following settings to get optimal results."));

	/* TRANSLATORS: dialog message, preface */
if(0)	gcm_calib_add_page_para (content, _("You may want to consult the owner's manual for your display on how to achieve these settings."));

	/* TRANSLATORS: dialog message, bullet item */
if(0)	gcm_calib_add_page_bullet (content, _("Reset your display to the factory defaults."));

	/* TRANSLATORS: dialog message, bullet item */
if(0)	gcm_calib_add_page_bullet (content, _("Disable dynamic contrast if your display has this feature."));

	/* TRANSLATORS: dialog message, bullet item */
if(0)	gcm_calib_add_page_bullet (content, _("Configure your display with custom color settings and ensure the RGB channels are set to the same values."));

	/* TRANSLATORS: dialog message, addition to bullet item */
if(0)	gcm_calib_add_page_para (content, _("If custom color is not available then use a 6500K color temperature."));

	/* TRANSLATORS: dialog message, bullet item */
if(0)	gcm_calib_add_page_bullet (content, _("Adjust the display brightness to a comfortable level for prolonged viewing."));

	gcm_calib_add_page_para (content, "");

	/* TRANSLATORS: dialog message, suffix */
	gcm_calib_add_page_para (content, _("For best results, the display should have been powered for at least 15 minutes before starting the calibration."));

	/* add to assistant */
	gtk_assistant_append_page (assistant, vbox);
	gtk_assistant_set_page_type (assistant, vbox, GTK_ASSISTANT_PAGE_CONTENT);
	/* TRANSLATORS: this is the calibration wizard page title */
	gtk_assistant_set_page_title (assistant, vbox, _("Check Settings"));
	gtk_assistant_set_page_complete (assistant, vbox, FALSE);
	g_ptr_array_add (calib->pages, vbox);
	g_object_set_data (G_OBJECT (vbox),
			   "GcmCalibrateMain::Index",
			   GUINT_TO_POINTER (GCM_CALIBRATE_PAGE_DISPLAY_CONFIG));

	/* show page */
	gtk_widget_show_all (vbox);
}

/**
 * gcm_calib_button_clicked_install_argyllcms_cb:
 **/
static void
gcm_calib_button_clicked_install_argyllcms_cb (GtkButton *button, GcmCalibratePriv *calib)
{
	gboolean ret;
	GtkWidget *vbox;
	GtkAssistant *assistant = GTK_ASSISTANT (calib->main_window);

	ret = gcm_utils_install_package (GCM_PREFS_PACKAGE_NAME_ARGYLLCMS,
					 calib->main_window);
	/* we can continue now */
	if (TRUE || ret) {
		vbox = gcm_calib_get_vbox_for_page (calib,
						    GCM_CALIBRATE_PAGE_INSTALL_ARGYLLCMS);
		gtk_assistant_set_page_complete (assistant, vbox, TRUE);
		gtk_assistant_next_page (assistant);

		/* we ddn't need to re-install now */
		gtk_widget_hide (vbox);
	}
}

/**
 * gcm_calib_setup_page_install_argyllcms:
 **/
static void
gcm_calib_setup_page_install_argyllcms (GcmCalibratePriv *calib)
{
	GtkWidget *vbox;
	GtkWidget *content;
	GtkWidget *button;
	GString *string;
	GtkAssistant *assistant = GTK_ASSISTANT (calib->main_window);

	string = g_string_new ("");

	/* TRANSLATORS: dialog message saying the argyllcms is not installed */
	g_string_append_printf (string, "%s\n",
				_("Calibration and profiling software is not installed."));
	/* TRANSLATORS: dialog message saying the color targets are not installed */
	g_string_append_printf (string, "%s",
				_("These tools are required to build color profiles for devices."));

	/* TRANSLATORS: this is the page title */
	vbox = gcm_calib_add_page_title (calib, _("More software is required!"));

	/* main contents */
	content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_box_pack_start (GTK_BOX (vbox), content, FALSE, FALSE, 0);

	/* TRANSLATORS: this is intro page text */
	gcm_calib_add_page_para (content, string->str);

	button = gtk_button_new_with_label (_("Install required software"));
	g_signal_connect (button, "clicked",
			  G_CALLBACK (gcm_calib_button_clicked_install_argyllcms_cb),
			  calib);
	gtk_box_pack_start (GTK_BOX (vbox), button, FALSE, FALSE, 0);

	/* add to assistant */
	gtk_assistant_append_page (assistant, vbox);
	gtk_assistant_set_page_type (assistant, vbox, GTK_ASSISTANT_PAGE_CONTENT);
	/* TRANSLATORS: this is the calibration wizard page title */
	gtk_assistant_set_page_title (assistant, vbox, _("Install Tools"));
	gtk_assistant_set_page_complete (assistant, vbox, FALSE);
	g_ptr_array_add (calib->pages, vbox);
	g_object_set_data (G_OBJECT (vbox),
			   "GcmCalibrateMain::Index",
			   GUINT_TO_POINTER (GCM_CALIBRATE_PAGE_INSTALL_ARGYLLCMS));
	g_string_free (string, TRUE);

	/* show page */
	gtk_widget_show_all (vbox);
}

/**
 * gcm_calib_button_clicked_install_targets_cb:
 **/
static void
gcm_calib_button_clicked_install_targets_cb (GtkButton *button, GcmCalibratePriv *calib)
{
	gboolean ret;
	GtkWidget *vbox;
	GtkAssistant *assistant = GTK_ASSISTANT (calib->main_window);

	ret = gcm_utils_install_package (GCM_PREFS_PACKAGE_NAME_SHARED_COLOR_TARGETS,
					 calib->main_window);
	/* we can continue now */
	if (ret) {
		vbox = gcm_calib_get_vbox_for_page (calib,
						    GCM_CALIBRATE_PAGE_INSTALL_TARGETS);
		gtk_assistant_next_page (assistant);

		/* we ddn't need to re-install now */
		gtk_widget_hide (vbox);
	}
}

/**
 * gcm_calib_setup_page_install_targets:
 **/
static void
gcm_calib_setup_page_install_targets (GcmCalibratePriv *calib)
{
	GtkWidget *vbox;
	GtkWidget *content;
	GtkWidget *button;
	GString *string;
	GtkAssistant *assistant = GTK_ASSISTANT (calib->main_window);

	string = g_string_new ("");

	/* TRANSLATORS: dialog message saying the color targets are not installed */
	g_string_append_printf (string, "%s\n", _("Common color target files are not installed on this computer."));
	/* TRANSLATORS: dialog message saying the color targets are not installed */
	g_string_append_printf (string, "%s\n\n", _("Color target files are needed to convert the image to a color profile."));
	/* TRANSLATORS: dialog message, asking if it's okay to install them */
	g_string_append_printf (string, "%s\n\n", _("Do you want them to be installed?"));
	/* TRANSLATORS: dialog message, if the user has the target file on a CDROM then there's no need for this package */
	g_string_append_printf (string, "%s", _("If you already have the correct file, you can skip this step."));

	/* TRANSLATORS: this is the page title */
	vbox = gcm_calib_add_page_title (calib, _("Optional data files available"));

	/* main contents */
	content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_box_pack_start (GTK_BOX (vbox), content, FALSE, FALSE, 0);

	/* TRANSLATORS: this is intro page text */
	gcm_calib_add_page_para (content, string->str);

	button = gtk_button_new_with_label (_("Install Now"));
	g_signal_connect (button, "clicked",
			  G_CALLBACK (gcm_calib_button_clicked_install_targets_cb),
			  calib);
	gtk_box_pack_start (GTK_BOX (vbox), button, FALSE, FALSE, 0);

	/* add to assistant */
	gtk_assistant_append_page (assistant, vbox);
	gtk_assistant_set_page_type (assistant, vbox, GTK_ASSISTANT_PAGE_CONTENT);
	gtk_assistant_set_page_complete (assistant, vbox, FALSE);
	/* TRANSLATORS: this is the calibration wizard page title */
	gtk_assistant_set_page_title (assistant, vbox, _("Install Targets"));
	g_ptr_array_add (calib->pages, vbox);
	g_object_set_data (G_OBJECT (vbox),
			   "GcmCalibrateMain::Index",
			   GUINT_TO_POINTER (GCM_CALIBRATE_PAGE_INSTALL_TARGETS));
	g_string_free (string, TRUE);

	/* show page */
	gtk_widget_show_all (vbox);
}


/**
 * gcm_calib_reference_kind_to_localised_string:
 **/
static const gchar *
gcm_calib_reference_kind_to_localised_string (GcmCalibrateReferenceKind kind)
{
	if (kind == GCM_CALIBRATE_REFERENCE_KIND_CMP_DIGITAL_TARGET_3) {
		/* TRANSLATORS: this is probably a brand name */
		return _("CMP Digital Target 3");
	}
	if (kind == GCM_CALIBRATE_REFERENCE_KIND_CMP_DT_003) {
		/* TRANSLATORS: this is probably a brand name */
		return _("CMP DT 003");
	}
	if (kind == GCM_CALIBRATE_REFERENCE_KIND_COLOR_CHECKER) {
		/* TRANSLATORS: this is probably a brand name */
		return _("Color Checker");
	}
	if (kind == GCM_CALIBRATE_REFERENCE_KIND_COLOR_CHECKER_DC) {
		/* TRANSLATORS: this is probably a brand name */
		return _("Color Checker DC");
	}
	if (kind == GCM_CALIBRATE_REFERENCE_KIND_COLOR_CHECKER_SG) {
		/* TRANSLATORS: this is probably a brand name */
		return _("Color Checker SG");
	}
	if (kind == GCM_CALIBRATE_REFERENCE_KIND_HUTCHCOLOR) {
		/* TRANSLATORS: this is probably a brand name */
		return _("Hutchcolor");
	}
	if (kind == GCM_CALIBRATE_REFERENCE_KIND_I1_RGB_SCAN_1_4) {
		/* TRANSLATORS: this is probably a brand name */
		return _("i1 RGB Scan 1.4");
	}
	if (kind == GCM_CALIBRATE_REFERENCE_KIND_IT8) {
		/* TRANSLATORS: this is probably a brand name */
		return _("IT8.7/2");
	}
	if (kind == GCM_CALIBRATE_REFERENCE_KIND_LASER_SOFT_DC_PRO) {
		/* TRANSLATORS: this is probably a brand name */
		return _("Laser Soft DC Pro");
	}
	if (kind == GCM_CALIBRATE_REFERENCE_KIND_QPCARD_201) {
		/* TRANSLATORS: this is probably a brand name */
		return _("QPcard 201");
	}
	return NULL;
}

/**
 * gcm_calib_reference_kind_to_image_filename:
 **/
static const gchar *
gcm_calib_reference_kind_to_image_filename (GcmCalibrateReferenceKind kind)
{
	if (kind == GCM_CALIBRATE_REFERENCE_KIND_CMP_DIGITAL_TARGET_3)
		return "CMP-DigitalTarget3.png";
	if (kind == GCM_CALIBRATE_REFERENCE_KIND_CMP_DT_003)
		return "CMP_DT_003.png";
	if (kind == GCM_CALIBRATE_REFERENCE_KIND_COLOR_CHECKER)
		return "ColorChecker24.png";
	if (kind == GCM_CALIBRATE_REFERENCE_KIND_COLOR_CHECKER_DC)
		return "ColorCheckerDC.png";
	if (kind == GCM_CALIBRATE_REFERENCE_KIND_COLOR_CHECKER_SG)
		return "ColorCheckerSG.png";
	if (kind == GCM_CALIBRATE_REFERENCE_KIND_HUTCHCOLOR)
		return NULL;
	if (kind == GCM_CALIBRATE_REFERENCE_KIND_I1_RGB_SCAN_1_4)
		return "i1_RGB_Scan_14.png";
	if (kind == GCM_CALIBRATE_REFERENCE_KIND_IT8)
		return "IT872.png";
	if (kind == GCM_CALIBRATE_REFERENCE_KIND_LASER_SOFT_DC_PRO)
		return "LaserSoftDCPro.png";
	if (kind == GCM_CALIBRATE_REFERENCE_KIND_QPCARD_201)
		return "QPcard_201.png";
	return NULL;
}

/**
 * gcm_calib_reference_kind_combobox_cb:
 **/
static void
gcm_calib_reference_kind_combobox_cb (GtkComboBox *combo_box,
				      GcmCalibratePriv *calib)
{
	const gchar *filename;
	gchar *path;
	GcmCalibrateReferenceKind reference_kind;

	/* not sorted so we can just use the index */
	reference_kind = gtk_combo_box_get_active (GTK_COMBO_BOX (combo_box));
	filename = gcm_calib_reference_kind_to_image_filename (reference_kind);

	/* fallback */
	if (filename == NULL)
		filename = "unknown.png";

	path = g_build_filename (GCM_DATA, "targets", filename, NULL);
	gtk_image_set_from_file (GTK_IMAGE (calib->reference_preview), path);
	g_free (path);
}

/**
 * gcm_calib_setup_page_target_kind:
 **/
static void
gcm_calib_setup_page_target_kind (GcmCalibratePriv *calib)
{
	GtkWidget *vbox;
	GtkWidget *content;
	GtkWidget *combo;
	GString *string;
	guint i;
	GtkAssistant *assistant = GTK_ASSISTANT (calib->main_window);

	string = g_string_new ("");

	/* TRANSLATORS: dialog message, preface. A calibration target looks like
	 * this: http://www.colorreference.de/targets/target.jpg */
	g_string_append_printf (string, "%s\n", _("Before profiling the device, you have to manually capture an image of a calibration target and save it as a TIFF image file."));

	/* scanner specific options */
	if (calib->device_kind == CD_DEVICE_KIND_SCANNER) {
		/* TRANSLATORS: dialog message, preface */
		g_string_append_printf (string, "%s\n", _("Ensure that the contrast and brightness are not changed and color correction profiles have not been applied."));

		/* TRANSLATORS: dialog message, suffix */
		g_string_append_printf (string, "%s\n", _("The device sensor should have been cleaned prior to scanning and the output file resolution should be at least 200dpi."));
	}

	/* camera specific options */
	if (calib->device_kind == CD_DEVICE_KIND_CAMERA) {
		/* TRANSLATORS: dialog message, preface */
		g_string_append_printf (string, "%s\n", _("Ensure that the white-balance has not been modified by the camera and that the lens is clean."));
	}

	/* TRANSLATORS: this is the message body for the chart selection */
	g_string_append_printf (string, "\n%s", _("Please select the calibration target type."));

	/* TRANSLATORS: this is the page title */
	vbox = gcm_calib_add_page_title (calib, _("What target type do you have?"));

	/* main contents */
	content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_box_pack_start (GTK_BOX (vbox), content, FALSE, FALSE, 0);

	/* TRANSLATORS: this is intro page text */
	gcm_calib_add_page_para (content, string->str);

	/* pack in a preview image */
	calib->reference_preview = gtk_image_new ();
	gtk_box_pack_start (GTK_BOX (vbox), calib->reference_preview, FALSE, FALSE, 0);

	combo = gtk_combo_box_text_new ();
	for (i=0; i<GCM_CALIBRATE_REFERENCE_KIND_UNKNOWN; i++) {
		gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (combo),
						gcm_calib_reference_kind_to_localised_string (i));
	}
	g_signal_connect (combo, "changed",
			  G_CALLBACK (gcm_calib_reference_kind_combobox_cb),
			  calib);

	/* use IT8 by default */
	gtk_combo_box_set_active (GTK_COMBO_BOX (combo), GCM_CALIBRATE_REFERENCE_KIND_IT8);

	gtk_box_pack_start (GTK_BOX (vbox), combo, FALSE, FALSE, 0);

	/* add to assistant */
	gtk_assistant_append_page (assistant, vbox);
	gtk_assistant_set_page_type (assistant, vbox, GTK_ASSISTANT_PAGE_CONTENT);
	gtk_assistant_set_page_complete (assistant, vbox, FALSE);
	/* TRANSLATORS: this is the calibration wizard page title */
	gtk_assistant_set_page_title (assistant, vbox, _("Select Target"));
	g_ptr_array_add (calib->pages, vbox);
	g_object_set_data (G_OBJECT (vbox),
			   "GcmCalibrateMain::Index",
			   GUINT_TO_POINTER (GCM_CALIBRATE_PAGE_TARGET_KIND));
	g_string_free (string, TRUE);

	/* show page */
	gtk_widget_show_all (vbox);
}

static void
gcm_calib_display_kind_toggled_cb (GtkToggleButton *togglebutton,
				   GcmCalibratePriv *calib)
{
	GcmCalibrateDisplayKind	 display_kind;

	if (!gtk_toggle_button_get_active (togglebutton))
		return;
	display_kind = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (togglebutton),
							    "GcmCalib::display-kind"));
	g_object_set (calib->calibrate,
		      "display-kind", display_kind,
		      NULL);
}

/**
 * gcm_calib_setup_page_display_kind:
 **/
static void
gcm_calib_setup_page_display_kind (GcmCalibratePriv *calib)
{
	GtkWidget *vbox;
	GtkWidget *content;
	GtkWidget *widget;
	GSList *list;
	GtkAssistant *assistant = GTK_ASSISTANT (calib->main_window);

	/* TRANSLATORS: this is the page title */
	vbox = gcm_calib_add_page_title (calib, _("Choose your display type"));

	/* main contents */
	content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_box_pack_start (GTK_BOX (vbox), content, FALSE, FALSE, 0);

	/* TRANSLATORS: this is intro page text */
	gcm_calib_add_page_para (content, _("Select the monitor type that is attached to your computer."));

	widget = gtk_radio_button_new_with_label (NULL, _("LCD"));
	g_object_set_data (G_OBJECT (widget),
			   "GcmCalib::display-kind",
			   GUINT_TO_POINTER (GCM_CALIBRATE_DEVICE_KIND_LCD));
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gcm_calib_display_kind_toggled_cb), calib);
	gtk_box_pack_start (GTK_BOX (content), widget, FALSE, FALSE, 0);

	list = gtk_radio_button_get_group (GTK_RADIO_BUTTON (widget));
	widget = gtk_radio_button_new_with_label (list, _("CRT"));
	g_object_set_data (G_OBJECT (widget),
			   "GcmCalib::display-kind",
			   GUINT_TO_POINTER (GCM_CALIBRATE_DEVICE_KIND_CRT));
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gcm_calib_display_kind_toggled_cb), calib);
	gtk_box_pack_start (GTK_BOX (content), widget, FALSE, FALSE, 0);

	list = gtk_radio_button_get_group (GTK_RADIO_BUTTON (widget));
	widget = gtk_radio_button_new_with_label (list, _("Plasma"));
	g_object_set_data (G_OBJECT (widget),
			   "GcmCalib::display-kind",
			   GUINT_TO_POINTER (GCM_CALIBRATE_DEVICE_KIND_CRT));
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gcm_calib_display_kind_toggled_cb), calib);
	gtk_box_pack_start (GTK_BOX (content), widget, FALSE, FALSE, 0);

	list = gtk_radio_button_get_group (GTK_RADIO_BUTTON (widget));
	widget = gtk_radio_button_new_with_label (list, _("Projector"));
	g_object_set_data (G_OBJECT (widget),
			   "GcmCalib::display-kind",
			   GUINT_TO_POINTER (GCM_CALIBRATE_DEVICE_KIND_PROJECTOR));
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gcm_calib_display_kind_toggled_cb), calib);
	gtk_box_pack_start (GTK_BOX (content), widget, FALSE, FALSE, 0);

	/* add to assistant */
	gtk_assistant_append_page (assistant, vbox);
	gtk_assistant_set_page_type (assistant, vbox, GTK_ASSISTANT_PAGE_CONTENT);
	/* TRANSLATORS: this is the calibration wizard page title */
	gtk_assistant_set_page_title (assistant, vbox, _("Choose Display Type"));
	gtk_assistant_set_page_complete (assistant, vbox, FALSE);
	g_ptr_array_add (calib->pages, vbox);
	g_object_set_data (G_OBJECT (vbox),
			   "GcmCalibrateMain::Index",
			   GUINT_TO_POINTER (GCM_CALIBRATE_PAGE_DISPLAY_KIND));

	/* show page */
	gtk_widget_show_all (vbox);
}

static void
gcm_calib_display_temp_toggled_cb (GtkToggleButton *togglebutton,
				   GcmCalibratePriv *calib)
{
	guint display_temp;
	if (!gtk_toggle_button_get_active (togglebutton))
		return;
	display_temp = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (togglebutton),
							    "GcmCalib::display-temp"));
	g_object_set (calib->calibrate,
		      "target-whitepoint", display_temp,
		      NULL);
}

/**
 * gcm_calib_setup_page_display_temp:
 **/
static void
gcm_calib_setup_page_display_temp (GcmCalibratePriv *calib)
{
	GtkWidget *vbox;
	GtkWidget *content;
	GtkWidget *widget;
	GSList *list;
	GtkAssistant *assistant = GTK_ASSISTANT (calib->main_window);

	/* TRANSLATORS: this is the page title */
	vbox = gcm_calib_add_page_title (calib, _("Choose your display target white point"));

	/* main contents */
	content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_box_pack_start (GTK_BOX (vbox), content, FALSE, FALSE, 0);

	/* TRANSLATORS: this is intro page text */
	gcm_calib_add_page_para (content, _("Most displays should be calibrated to a CIE D65 illuminant for general usage."));

	widget = gtk_radio_button_new_with_label (NULL, _("CIE D50 (Printing and publishing)"));
	g_object_set_data (G_OBJECT (widget),
			   "GcmCalib::display-temp",
			   GUINT_TO_POINTER (5000));
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gcm_calib_display_temp_toggled_cb), calib);
	gtk_box_pack_start (GTK_BOX (content), widget, FALSE, FALSE, 0);

	list = gtk_radio_button_get_group (GTK_RADIO_BUTTON (widget));
	widget = gtk_radio_button_new_with_label (list, _("CIE D55"));
	g_object_set_data (G_OBJECT (widget),
			   "GcmCalib::display-temp",
			   GUINT_TO_POINTER (5500));
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gcm_calib_display_temp_toggled_cb), calib);
	gtk_box_pack_start (GTK_BOX (content), widget, FALSE, FALSE, 0);

	list = gtk_radio_button_get_group (GTK_RADIO_BUTTON (widget));
	widget = gtk_radio_button_new_with_label (list, _("CIE D65 (Photography and graphics)"));
	g_object_set_data (G_OBJECT (widget),
			   "GcmCalib::display-temp",
			   GUINT_TO_POINTER (6500));
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gcm_calib_display_temp_toggled_cb), calib);
	gtk_box_pack_start (GTK_BOX (content), widget, FALSE, FALSE, 0);
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (widget), TRUE);

	list = gtk_radio_button_get_group (GTK_RADIO_BUTTON (widget));
	widget = gtk_radio_button_new_with_label (list, _("CIE D75"));
	g_object_set_data (G_OBJECT (widget),
			   "GcmCalib::display-temp",
			   GUINT_TO_POINTER (7500));
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gcm_calib_display_temp_toggled_cb), calib);
	gtk_box_pack_start (GTK_BOX (content), widget, FALSE, FALSE, 0);

	list = gtk_radio_button_get_group (GTK_RADIO_BUTTON (widget));
	widget = gtk_radio_button_new_with_label (list, _("Native (Already set manually)"));
	g_object_set_data (G_OBJECT (widget),
			   "GcmCalib::display-temp",
			   GUINT_TO_POINTER (0));
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gcm_calib_display_temp_toggled_cb), calib);
	gtk_box_pack_start (GTK_BOX (content), widget, FALSE, FALSE, 0);

	/* add to assistant */
	gtk_assistant_append_page (assistant, vbox);
	gtk_assistant_set_page_type (assistant, vbox, GTK_ASSISTANT_PAGE_CONTENT);
	/* TRANSLATORS: this is the calibration wizard page title */
	gtk_assistant_set_page_title (assistant, vbox, _("Choose Display Whitepoint"));
	gtk_assistant_set_page_complete (assistant, vbox, FALSE);
	g_ptr_array_add (calib->pages, vbox);
	g_object_set_data (G_OBJECT (vbox),
			   "GcmCalibrateMain::Index",
			   GUINT_TO_POINTER (GCM_CALIBRATE_PAGE_DISPLAY_TEMPERATURE));

	/* show page */
	gtk_widget_show_all (vbox);
}

static void
gcm_calib_print_kind_toggled_cb (GtkToggleButton *togglebutton,
				   GcmCalibratePriv *calib)
{
	GcmCalibratePrintKind print_kind;
	if (!gtk_toggle_button_get_active (togglebutton))
		return;
	print_kind = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (togglebutton),
							  "GcmCalib::print-kind"));
	g_object_set (calib->calibrate,
		      "print-kind", print_kind,
		      NULL);
}

/**
 * gcm_calib_setup_page_print_kind:
 **/
static void
gcm_calib_setup_page_print_kind (GcmCalibratePriv *calib)
{
	GtkWidget *vbox;
	GtkWidget *content;
	GtkWidget *widget;
	GSList *list;
	GtkAssistant *assistant = GTK_ASSISTANT (calib->main_window);

	/* TRANSLATORS: this is the page title */
	vbox = gcm_calib_add_page_title (calib, _("Choose profiling mode"));

	/* main contents */
	content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_box_pack_start (GTK_BOX (vbox), content, FALSE, FALSE, 0);

	/* TRANSLATORS: this is intro page text */
	gcm_calib_add_page_para (content, _("Please indicate if you want to profile a local printer, generate some test patches, or profile using existing test patches."));

	widget = gtk_radio_button_new_with_label (NULL, "Local printer");
	g_object_set_data (G_OBJECT (widget),
			   "GcmCalib::print-kind",
			   GUINT_TO_POINTER (GCM_CALIBRATE_PRINT_KIND_LOCAL));
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gcm_calib_print_kind_toggled_cb), calib);
	gtk_box_pack_start (GTK_BOX (content), widget, FALSE, FALSE, 0);

	list = gtk_radio_button_get_group (GTK_RADIO_BUTTON (widget));
	widget = gtk_radio_button_new_with_label (list, "Generate patches for remote printer");
	g_object_set_data (G_OBJECT (widget),
			   "GcmCalib::print-kind",
			   GUINT_TO_POINTER (GCM_CALIBRATE_PRINT_KIND_GENERATE));
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gcm_calib_print_kind_toggled_cb), calib);
	gtk_box_pack_start (GTK_BOX (content), widget, FALSE, FALSE, 0);

	list = gtk_radio_button_get_group (GTK_RADIO_BUTTON (widget));
	widget = gtk_radio_button_new_with_label (list, "Generate profile from existing test patches");
	g_object_set_data (G_OBJECT (widget),
			   "GcmCalib::print-kind",
			   GUINT_TO_POINTER (GCM_CALIBRATE_PRINT_KIND_ANALYZE));
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gcm_calib_print_kind_toggled_cb), calib);
	gtk_box_pack_start (GTK_BOX (content), widget, FALSE, FALSE, 0);

	/* add to assistant */
	gtk_assistant_append_page (assistant, vbox);
	gtk_assistant_set_page_type (assistant, vbox, GTK_ASSISTANT_PAGE_CONTENT);
	/* TRANSLATORS: this is the calibration wizard page title */
	gtk_assistant_set_page_title (assistant, vbox, _("Calibration Mode"));
	gtk_assistant_set_page_complete (assistant, vbox, FALSE);
	g_ptr_array_add (calib->pages, vbox);
	g_object_set_data (G_OBJECT (vbox),
			   "GcmCalibrateMain::Index",
			   GUINT_TO_POINTER (GCM_CALIBRATE_PAGE_PRINT_KIND));

	/* show page */
	gtk_widget_show_all (vbox);
}

static void
gcm_calib_precision_toggled_cb (GtkToggleButton *togglebutton,
				GcmCalibratePriv *calib)
{
	GcmCalibratePrecision precision;
	if (!gtk_toggle_button_get_active (togglebutton))
		return;
	precision = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (togglebutton),
							 "GcmCalib::precision"));
	g_object_set (calib->calibrate,
		      "precision", precision,
		      NULL);
}

/**
 * gcm_calib_setup_page_precision:
 **/
static void
gcm_calib_setup_page_precision (GcmCalibratePriv *calib)
{
	GtkWidget *vbox;
	GtkWidget *content;
	GtkWidget *widget;
	GSList *list;
	GString *labels[3];
	guint i;
	guint values_printer[] = { 20, 10, 4}; /* sheets */
	guint values_display[] = { 20, 10, 4}; /* minutes */
	GtkAssistant *assistant = GTK_ASSISTANT (calib->main_window);

	/* TRANSLATORS: this is the page title */
	vbox = gcm_calib_add_page_title (calib, _("Choose calibration quality"));

	/* main contents */
	content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_box_pack_start (GTK_BOX (vbox), content, FALSE, FALSE, 0);

	/* TRANSLATORS: this is intro page text */
	gcm_calib_add_page_para (content, _("Higher quality calibration requires many color samples and more time."));

#if 0
	/* TRANSLATORS: this is the message body for the chart selection */
	g_string_append (string, _("A higher precision profile provides higher accuracy in color matching but requires more time for reading the color patches."));

	/* TRANSLATORS: this is the message body for the chart selection */
	g_string_append_printf (string, "\n%s", _("For a typical workflow, a normal precision profile is sufficient."));

	/* printer specific options */
	if (calib->device_kind == CD_DEVICE_KIND_PRINTER) {
		/* TRANSLATORS: dialog message, preface */
		g_string_append_printf (string, "\n%s", _("The high precision profile also requires more paper and printer ink."));
	}
#endif

	/* TRANSLATORS: radio options for calibration precision */
	labels[0] = g_string_new (_("Accurate"));
	labels[1] = g_string_new (_("Normal"));
	labels[2] = g_string_new (_("Quick"));
	switch (calib->device_kind) {
	case CD_DEVICE_KIND_PRINTER:
		for (i=0; i<3; i++) {
			g_string_append (labels[i], " ");
			/* TRANSLATORS: radio options for calibration precision */
			g_string_append_printf (labels[i], ngettext (
						"(about %i sheet of paper)",
						"(about %i sheets of paper)",
						values_printer[i]),
						values_printer[i]);
		}
		break;
	case CD_DEVICE_KIND_DISPLAY:
		for (i=0; i<3; i++) {
			g_string_append (labels[i], " ");
			/* TRANSLATORS: radio options for calibration precision */
			g_string_append_printf (labels[i], ngettext (
						"(about %i minute)",
						"(about %i minutes)",
						values_display[i]),
						values_display[i]);
		}
		break;
	default:
		break;
	}

	widget = gtk_radio_button_new_with_label (NULL, labels[0]->str);
	g_object_set_data (G_OBJECT (widget),
			   "GcmCalib::precision",
			   GUINT_TO_POINTER (GCM_CALIBRATE_PRECISION_LONG));
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gcm_calib_precision_toggled_cb), calib);
	gtk_box_pack_start (GTK_BOX (content), widget, FALSE, FALSE, 0);

	list = gtk_radio_button_get_group (GTK_RADIO_BUTTON (widget));
	widget = gtk_radio_button_new_with_label (list, labels[1]->str);
	g_object_set_data (G_OBJECT (widget),
			   "GcmCalib::precision",
			   GUINT_TO_POINTER (GCM_CALIBRATE_PRECISION_NORMAL));
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gcm_calib_precision_toggled_cb), calib);
	gtk_box_pack_start (GTK_BOX (content), widget, FALSE, FALSE, 0);

	list = gtk_radio_button_get_group (GTK_RADIO_BUTTON (widget));
	widget = gtk_radio_button_new_with_label (list, labels[2]->str);
	g_object_set_data (G_OBJECT (widget),
			   "GcmCalib::precision",
			   GUINT_TO_POINTER (GCM_CALIBRATE_PRECISION_SHORT));
	g_signal_connect (widget, "toggled",
			  G_CALLBACK (gcm_calib_precision_toggled_cb), calib);
	gtk_box_pack_start (GTK_BOX (content), widget, FALSE, FALSE, 0);

	/* add to assistant */
	gtk_assistant_append_page (assistant, vbox);
	gtk_assistant_set_page_type (assistant, vbox, GTK_ASSISTANT_PAGE_CONTENT);
	/* TRANSLATORS: this is the calibration wizard page title */
	gtk_assistant_set_page_title (assistant, vbox, _("Calibration Quality"));
	gtk_assistant_set_page_complete (assistant, vbox, FALSE);
	g_ptr_array_add (calib->pages, vbox);
	g_object_set_data (G_OBJECT (vbox),
			   "GcmCalibrateMain::Index",
			   GUINT_TO_POINTER (GCM_CALIBRATE_PAGE_PRECISION));

	for (i=0; i<3; i++)
		g_string_free (labels[i], TRUE);

	/* show page */
	gtk_widget_show_all (vbox);
}

static void
gcm_calib_text_changed_cb (GtkEntry *entry,
			   GcmCalibratePriv *calib)
{
	g_object_set (calib->calibrate,
		      "description", gtk_entry_get_text (entry),
		      NULL);
}

/**
 * gcm_calib_setup_page_profile_title:
 **/
static void
gcm_calib_setup_page_profile_title (GcmCalibratePriv *calib)
{
	GtkWidget *vbox;
	GtkWidget *content;
	GtkWidget *widget;
	gchar *tmp = NULL;
	GtkAssistant *assistant = GTK_ASSISTANT (calib->main_window);

	/* TRANSLATORS: this is the page title */
	vbox = gcm_calib_add_page_title (calib, _("Profile title"));

	/* main contents */
	content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_box_pack_start (GTK_BOX (vbox), content, FALSE, FALSE, 0);

	/* TRANSLATORS: this is intro page text */
	gcm_calib_add_page_para (content, _("Choose a title to identify the profile on your system."));

	widget = gtk_entry_new ();
	gtk_box_pack_start (GTK_BOX (content), widget, FALSE, FALSE, 0);
	gtk_entry_set_max_length (GTK_ENTRY (widget), 128);

	/* set the current title */
	g_object_get (calib->calibrate,
		      "description", &tmp,
		      NULL);
	gtk_entry_set_text (GTK_ENTRY (widget), tmp);
	g_free (tmp);

	/* watch for changes */
	g_signal_connect (GTK_EDITABLE (widget), "changed",
			  G_CALLBACK (gcm_calib_text_changed_cb), calib);

	/* add to assistant */
	gtk_assistant_append_page (assistant, vbox);
	gtk_assistant_set_page_type (assistant, vbox, GTK_ASSISTANT_PAGE_CONTENT);
	/* TRANSLATORS: this is the calibration wizard page title */
	gtk_assistant_set_page_title (assistant, vbox, _("Profile Title"));
	gtk_assistant_set_page_complete (assistant, vbox, TRUE);
	g_ptr_array_add (calib->pages, vbox);
	g_object_set_data (G_OBJECT (vbox),
			   "GcmCalibrateMain::Index",
			   GUINT_TO_POINTER (GCM_CALIBRATE_PAGE_TITLE));

	/* show page */
	gtk_widget_show_all (vbox);
}

/**
 * gcm_calib_setup_page_sensor:
 **/
static void
gcm_calib_setup_page_sensor (GcmCalibratePriv *calib)
{
	GtkWidget *vbox;
	GtkWidget *content;
	GtkAssistant *assistant = GTK_ASSISTANT (calib->main_window);

	/* TRANSLATORS: this is the page title */
	vbox = gcm_calib_add_page_title (calib, _("Insert sensor hardware"));

	/* main contents */
	content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_box_pack_start (GTK_BOX (vbox), content, FALSE, FALSE, 0);

	/* TRANSLATORS: this is intro page text */
	gcm_calib_add_page_para (content, _("You need to insert sensor hardware to continue."));

	/* add to assistant */
	gtk_assistant_append_page (assistant, vbox);
	gtk_assistant_set_page_type (assistant, vbox, GTK_ASSISTANT_PAGE_CONTENT);
	/* TRANSLATORS: this is the calibration wizard page title */
	gtk_assistant_set_page_title (assistant, vbox, _("Sensor Check"));
	gtk_assistant_set_page_complete (assistant, vbox, FALSE);
	g_ptr_array_add (calib->pages, vbox);
	g_object_set_data (G_OBJECT (vbox),
			   "GcmCalibrateMain::Index",
			   GUINT_TO_POINTER (GCM_CALIBRATE_PAGE_SENSOR));

	/* show page */
	gtk_widget_show_all (vbox);
}

/**
 * gcm_calib_setup_page_failure:
 **/
static void
gcm_calib_setup_page_failure (GcmCalibratePriv *calib)
{
	GtkWidget *vbox;
	GtkWidget *content;
	GtkAssistant *assistant = GTK_ASSISTANT (calib->main_window);

	/* TRANSLATORS: this is the page title */
	vbox = gcm_calib_add_page_title (calib, _("Failed to calibrate"));

	/* main contents */
	content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_box_pack_start (GTK_BOX (vbox), content, FALSE, FALSE, 0);

	/* TRANSLATORS: this is intro page text */
	gcm_calib_add_page_para (content, _("The device could not be found. Ensure it is plugged in and turned on."));

	/* add to assistant */
	gtk_assistant_append_page (assistant, vbox);
	gtk_assistant_set_page_type (assistant, vbox, GTK_ASSISTANT_PAGE_SUMMARY);
	/* TRANSLATORS: this is the calibration wizard page title */
	gtk_assistant_set_page_title (assistant, vbox, _("Summary"));
	gtk_assistant_set_page_complete (assistant, vbox, TRUE);
	g_ptr_array_add (calib->pages, vbox);
	g_object_set_data (G_OBJECT (vbox),
			   "GcmCalibrateMain::Index",
			   GUINT_TO_POINTER (GCM_CALIBRATE_PAGE_FAILURE));

	/* show page */
	gtk_widget_show_all (vbox);
}

/**
 * gcm_calib_get_sensors_cb:
 **/
static void
gcm_calib_get_sensors_cb (GObject *object,
			  GAsyncResult *res,
			  gpointer user_data)
{
	CdClient *client = CD_CLIENT (object);
	GcmCalibratePriv *calib = (GcmCalibratePriv *) user_data;
	GError *error = NULL;
	GPtrArray *sensors;
	GtkWidget *vbox;
	gboolean ret;

	/* get the result */
	sensors = cd_client_get_sensors_finish (client, res, &error);
	if (sensors == NULL) {
		g_warning ("failed to get sensors: %s",
			   error->message);
		g_error_free (error);
		goto out;
	}

	/* hide the sensors screen */
	if (sensors->len > 0) {
		calib->sensor = g_object_ref (g_ptr_array_index (sensors, 0));

		ret = cd_sensor_connect_sync (calib->sensor, NULL, &error);
		if (!ret) {
			g_warning ("failed to connect to sensor: %s",
				   error->message);
			g_error_free (error);
			goto out;
		}

		gcm_calibrate_set_sensor (calib->calibrate, calib->sensor);
		vbox = gcm_calib_get_vbox_for_page (calib,
						    GCM_CALIBRATE_PAGE_SENSOR);
		gtk_widget_hide (vbox);
	}
out:
	if (sensors != NULL)
		g_ptr_array_unref (sensors);
}

/**
 * gcm_calib_add_pages:
 **/
static void
gcm_calib_add_pages (GcmCalibratePriv *calib)
{
	gboolean ret;
	const gchar *xrandr_name;

	/* device not found */
	if (calib->device_kind == CD_DEVICE_KIND_UNKNOWN) {
		gcm_calib_setup_page_failure (calib);
		gtk_widget_show_all (GTK_WIDGET (calib->main_window));
		return;
	}

	gcm_calib_setup_page_intro (calib);

	if (calib->device_kind == CD_DEVICE_KIND_DISPLAY ||
	    calib->device_kind == CD_DEVICE_KIND_PRINTER)
		gcm_calib_setup_page_sensor (calib);

	/* find whether argyllcms is installed using a tool which should exist */
	ret = g_file_test ("/usr/bin/dispcal", G_FILE_TEST_EXISTS);
	if (!ret)
		gcm_calib_setup_page_install_argyllcms (calib);

	xrandr_name = cd_device_get_metadata_item (calib->device,
						   CD_DEVICE_METADATA_XRANDR_NAME);
	if (xrandr_name != NULL)
		calib->internal_lcd = gcm_utils_output_is_lcd_internal (xrandr_name);
	if (!calib->internal_lcd && calib->device_kind == CD_DEVICE_KIND_DISPLAY)
		gcm_calib_setup_page_display_configure_wait (calib);

	gcm_calib_setup_page_precision (calib);

	if (calib->device_kind == CD_DEVICE_KIND_DISPLAY) {
		if (!calib->internal_lcd) {
			gcm_calib_setup_page_display_kind (calib);
		} else {
			g_object_set (calib->calibrate,
				      "display-kind", GCM_CALIBRATE_DEVICE_KIND_LCD,
				      NULL);
		}
		gcm_calib_setup_page_display_temp (calib);
	} else if (calib->device_kind == CD_DEVICE_KIND_PRINTER) {
		gcm_calib_setup_page_print_kind (calib);
	} else {
		gcm_calib_setup_page_target_kind (calib);
		ret = g_file_test ("/usr/share/shared-color-targets", G_FILE_TEST_IS_DIR);
		if (!ret) 
			gcm_calib_setup_page_install_targets (calib);
	}

	gcm_calib_setup_page_profile_title (calib);
	gcm_calib_setup_page_action (calib);

	gcm_calib_setup_page_summary (calib);

	/* see if we can hide the sensor check */
	cd_client_get_sensors (calib->client,
			       NULL,
			       gcm_calib_get_sensors_cb,
			       calib);
}

/**
 * gcm_calib_sensor_added_cb:
 **/
static void
gcm_calib_sensor_added_cb (CdClient *client, CdSensor *sensor, GcmCalibratePriv *calib)
{
	GtkWidget *vbox;

	g_debug ("sensor inserted");
	calib->sensor = g_object_ref (sensor);
	vbox = gcm_calib_get_vbox_for_page (calib,
					    GCM_CALIBRATE_PAGE_SENSOR);
	gtk_widget_hide (vbox);
}

/**
 * gcm_calib_startup_cb:
 **/
static void
gcm_calib_startup_cb (GApplication *application, GcmCalibratePriv *calib)
{
	const gchar *description;
	const gchar *manufacturer;
	const gchar *model;
	const gchar *native_device;
	const gchar *serial;
	gchar *copyright = NULL;
	gboolean ret;
	GDateTime *dt = NULL;
	GError *error = NULL;

	/* add application specific icons to search path */
	gtk_icon_theme_append_search_path (gtk_icon_theme_get_default (),
					   GCM_DATA G_DIR_SEPARATOR_S "icons");

	/* connect to colord */
	calib->client = cd_client_new ();
	g_signal_connect (calib->client, "sensor-added",
			  G_CALLBACK (gcm_calib_sensor_added_cb), calib);
	ret = cd_client_connect_sync (calib->client,
				      NULL,
				      &error);
	if (!ret) {
		g_warning ("failed to connect to colord: %s",
			   error->message);
		g_error_free (error);
		goto out;
	}

	/* show main UI */
	calib->main_window = GTK_WINDOW (gtk_assistant_new ());
	gtk_window_set_resizable (calib->main_window, TRUE);
	gtk_window_set_title (calib->main_window, "");
	gtk_container_set_border_width (GTK_CONTAINER (calib->main_window), 12);
	g_signal_connect (calib->main_window, "delete_event",
			  G_CALLBACK (gcm_calib_delete_event_cb), calib);
	g_signal_connect (calib->main_window, "close",
			  G_CALLBACK (gcm_calib_assistant_close_cb), calib);
	g_signal_connect (calib->main_window, "cancel",
			  G_CALLBACK (gcm_calib_assistant_cancel_cb), calib);
	g_signal_connect (calib->main_window, "prepare",
			  G_CALLBACK (gcm_calib_assistant_prepare_cb), calib);
	gtk_application_add_window (calib->application,
				    calib->main_window);

	/* set the parent window if it is specified */
	if (calib->xid != 0) {
		g_debug ("Setting xid %i", calib->xid);
		gcm_window_set_parent_xid (GTK_WINDOW (calib->main_window), calib->xid);
	}

	/* select a specific profile only */
	calib->device = cd_client_find_device_sync (calib->client,
						    calib->device_id,
						    NULL,
						    &error);
	if (calib->device == NULL) {
		g_warning ("failed to get device %s: %s",
			   calib->device_id,
			   error->message);
		g_error_free (error);
		goto out;
	}

	/* connect to the device */
	ret = cd_device_connect_sync (calib->device,
				      NULL,
				      &error);
	if (!ret) {
		g_warning ("failed to connect to device: %s",
			   error->message);
		g_error_free (error);
		goto out;
	}

	/* get the device properties */
	calib->device_kind = cd_device_get_kind (calib->device);

	/* set, with fallbacks */
	serial = cd_device_get_serial (calib->device);
	if (serial == NULL) {
		/* TRANSLATORS: this is saved in the profile */
		serial = _("Unknown serial");
	}
	model = cd_device_get_model (calib->device);
	if (model == NULL) {
		/* TRANSLATORS: this is saved in the profile */
		model = _("Unknown model");
	}
	description = cd_device_get_model (calib->device);
	if (description == NULL) {
		/* TRANSLATORS: this is saved in the profile */
		description = _("Unknown description");
	}
	manufacturer = cd_device_get_vendor (calib->device);
	if (manufacturer == NULL) {
		/* TRANSLATORS: this is saved in the profile */
		manufacturer = _("Unknown manufacturer");
	}

	dt = g_date_time_new_now_local ();
	/* TRANSLATORS: this is the copyright string, where it might be
	 * "Copyright (c) 2009 Edward Scissorhands"
	 * BIG RED FLASHING NOTE: YOU NEED TO USE ASCII ONLY */
	copyright = g_strdup_printf ("%s %04i %s", _("Copyright (c)"),
				     g_date_time_get_year (dt),
				     g_get_real_name ());

	/* set the proper values */
	g_object_set (calib->calibrate,
		      "device-kind", calib->device_kind,
		      "model", model,
		      "description", description,
		      "manufacturer", manufacturer,
		      "serial", serial,
		      "copyright", copyright,
		      NULL);

	/* display specific properties */
	if (calib->device_kind == CD_DEVICE_KIND_DISPLAY) {
		native_device = cd_device_get_metadata_item (calib->device,
							     CD_DEVICE_METADATA_XRANDR_NAME);
		if (native_device == NULL) {
			g_warning ("failed to get output");
			goto out;
		}
		g_object_set (calib->calibrate,
			      "output-name", native_device,
			      NULL);
	}
out:
	/* add different pages depending on the device kind */
	gcm_calib_add_pages (calib);
	gtk_assistant_set_current_page (GTK_ASSISTANT (calib->main_window), 0);
	if (dt != NULL)
		g_date_time_unref (dt);
	g_free (copyright);
}

static void
gcm_calib_title_changed_cb (GcmCalibrate *calibrate,
			    const gchar *title,
			    GcmCalibratePriv *calib)
{
	gchar *markup;

	markup = g_strdup_printf ("<span size=\"large\" font_weight=\"bold\">%s</span>", title);
	gtk_label_set_markup (GTK_LABEL (calib->action_title), markup);
	g_free (markup);
}

static void
gcm_calib_message_changed_cb (GcmCalibrate *calibrate,
			      const gchar *title,
			      GcmCalibratePriv *calib)
{
	gtk_label_set_label (GTK_LABEL (calib->action_message), title);
}

static void
gcm_calib_image_changed_cb (GcmCalibrate *calibrate,
			    const gchar *filename,
			    GcmCalibratePriv *calib)
{
	gchar *path;
	GdkPixbuf *pixbuf;
	GError *error;

	if (filename != NULL) {
		path = g_build_filename (GCM_DATA, "icons", filename, NULL);
		pixbuf = gdk_pixbuf_new_from_file_at_size (path, 200, 400, &error);
		if (pixbuf == NULL) {
			g_warning ("failed to load image: %s", error->message);
			g_error_free (error);
			gtk_widget_hide (calib->action_image);
		} else {
			gtk_image_set_from_pixbuf (GTK_IMAGE (calib->action_image), pixbuf);
			gtk_widget_show (calib->action_image);
		}
		g_free (path);
	} else {
		gtk_widget_hide (calib->action_image);
	}
}

static void
gcm_calib_interaction_required_cb (GcmCalibrate *calibrate,
				   const gchar *button_text,
				   GcmCalibratePriv *calib)
{
	GtkWidget *vbox;
	vbox = gcm_calib_get_vbox_for_page (calib,
					    GCM_CALIBRATE_PAGE_ACTION);
	gtk_assistant_set_page_complete (GTK_ASSISTANT (calib->main_window),
					 vbox, TRUE);
	calib->has_pending_interaction = TRUE;
}

/**
 * main:
 **/
int
main (int argc, char **argv)
{
	gchar *device_id = NULL;
	GcmCalibratePriv *calib;
	GOptionContext *context;
	guint xid = 0;
	int status = 0;

	const GOptionEntry options[] = {
		{ "parent-window", 'p', 0, G_OPTION_ARG_INT, &xid,
		  /* TRANSLATORS: we can make this modal (stay on top of) another window */
		  _("Set the parent window to make this modal"), NULL },
		{ "device", 'd', 0, G_OPTION_ARG_STRING, &device_id,
		  /* TRANSLATORS: show just the one profile, rather than all */
		  _("Set the specific device to calibrate"), NULL },
		{ NULL}
	};

	setlocale (LC_ALL, "");

	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	gtk_init (&argc, &argv);

	context = g_option_context_new ("gnome-color-manager calibration tool");
	g_option_context_add_main_entries (context, options, NULL);
	g_option_context_add_group (context, gcm_debug_get_option_group ());
	g_option_context_add_group (context, gtk_get_option_group (TRUE));
	g_option_context_parse (context, &argc, &argv, NULL);
	g_option_context_free (context);

	calib = g_new0 (GcmCalibratePriv, 1);
	calib->pages = g_ptr_array_new ();
	calib->xid = xid;
	calib->device_id = device_id;
	calib->old_brightness = G_MAXUINT;
	calib->brightness = gcm_brightness_new ();
	calib->calibrate = gcm_calibrate_argyll_new ();
	calib->device_kind = CD_DEVICE_KIND_UNKNOWN;
	g_signal_connect (calib->calibrate, "title-changed",
			  G_CALLBACK (gcm_calib_title_changed_cb), calib);
	g_signal_connect (calib->calibrate, "message-changed",
			  G_CALLBACK (gcm_calib_message_changed_cb), calib);
	g_signal_connect (calib->calibrate, "image-changed",
			  G_CALLBACK (gcm_calib_image_changed_cb), calib);
	g_signal_connect (calib->calibrate, "interaction-required",
			  G_CALLBACK (gcm_calib_interaction_required_cb), calib);

	/* nothing specified */
	if (calib->device_id == NULL) {
		g_print ("did not specify a device ID\n");
//		calib->device_id = g_strdup ("sysfs-Chicony_Electronics_Co.__Ltd.-Integrated_Camera");
		calib->device_id = g_strdup ("xrandr-Lenovo Group Limited");
if(0)		goto out;
	}

	/* ensure single instance */
	calib->application = gtk_application_new ("org.gnome.ColorManager.Calibration", 0);
	g_signal_connect (calib->application, "startup",
			  G_CALLBACK (gcm_calib_startup_cb), calib);
	g_signal_connect (calib->application, "activate",
			  G_CALLBACK (gcm_calib_activate_cb), calib);

	/* wait */
	status = g_application_run (G_APPLICATION (calib->application), argc, argv);

	g_ptr_array_unref (calib->pages);
	g_object_unref (calib->application);
	g_object_unref (calib->brightness);
	g_object_unref (calib->calibrate);
	if (calib->client != NULL)
		g_object_unref (calib->client);
	if (calib->device_id != NULL)
		g_free (calib->device_id);
	if (calib->device != NULL)
		g_object_unref (calib->device);
	g_free (calib);
out:
	return status;
}