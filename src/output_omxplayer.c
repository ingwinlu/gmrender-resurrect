/* output_omxplayer.c - Output module for Omxplayer
 *
 * Copyright (C) 2005-2007   Ivo Clarysse
 *
 * Adapted to gstreamer-0.10 2006 David Siorpaes
 *
 * This file is part of GMediaRender.
 *
 * GMediaRender is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * GMediaRender is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GMediaRender; if not, write to the Free Software 
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, 
 * MA 02110-1301, USA.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <gio/gio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "logging.h"
#include "upnp_connmgr.h"
#include "output_module.h"
#include "output_omxplayer.h"

#define OMX_STOPPED 0
#define OMX_RUNNING 1
#define OMX_PAUSED 2


/*
 * TODO check whitespace
 */

static gchar player_state = OMX_STOPPED;

// stuff used for callbacks from transport
static struct SongMetaData song_meta_;
static output_update_meta_cb_t meta_update_callback_ = NULL;
static output_transition_cb_t play_trans_callback_ = NULL;

// spawn stuff, might be able to slim down once dbus is setup
static gchar *omx_argv[] = {"omxplayer.bin", "-b", "-o", "both", "--live", NULL, NULL};
static const gint omx_argv_url_pos = 5;
static gchar *omx_uri_ = NULL;
static gchar *omx_next_uri_ = NULL;
static gint omx_in;
static GIOChannel *omx_in_ch;
static GPid omx_pid;

// dbus
static GDBusConnection *dbus_con = NULL;

static void dbus_callback_signal(
        GDBusConnection *con,
        const gchar *sender_name,
        const gchar *object_path,
        const gchar *interface_name,
        const gchar *signal_name,
        GVariant *parameters,
        gpointer user_data) {
    Log_info("dbus", "\nsender %s\npath %s\ninterface %s\nsignal %s\n",
             sender_name, object_path, interface_name, signal_name);
}

static void dbus_callback_setup(
        GObject *source_object,
        GAsyncResult *res,
        gpointer user_data) {
    Log_info("omxplayer", "init dbus");
    GError *g_error = NULL;
    dbus_con = g_bus_get_finish(res, &g_error);
    if(g_error) {
        Log_error("omxplayer", "Error: %s", g_error->message);
        g_error_free(g_error);
        return -1;
    };

    Log_info("omxplayer", "subscribe to omxplayer");
    g_dbus_connection_signal_subscribe(
            dbus_con,
            NULL,
            NULL,
            NULL,
            NULL,
            NULL,
            G_DBUS_SIGNAL_FLAGS_NONE,
            (GDBusSignalCallback) dbus_callback_signal,
            NULL,
            NULL
    );
}

static int output_omxplayer_add_options(GOptionContext *ctx) {
	return 0;
/*
	GOptionGroup *option_group;
	option_group = g_option_group_new("gstout", "GStreamer Output Options",
	                                  "Show GStreamer Output Options",
	                                  NULL, NULL);
	g_option_group_add_entries(option_group, option_entries);

	g_option_context_add_group (ctx, option_group);
	
	g_option_context_add_group (ctx, gst_init_get_option_group ());
	return 0;
        */
}

static int get_current_player_state() {
	return player_state;
}

static void output_omxplayer_set_next_uri(const char *uri) {
	Log_info("omxplayer", "Set next uri to '%s'", uri);
	g_free(omx_next_uri_);
        omx_next_uri_ = (uri && *uri) ? g_strdup(uri) : NULL;
}

static void output_omxplayer_set_uri(const char *uri,
				     output_update_meta_cb_t meta_cb) {
	Log_info("omxplayer", "Set uri to '%s'", uri);
	g_free(omx_uri_);
        omx_uri_ = (uri && *uri) ? g_strdup(uri) : NULL;
	meta_update_callback_ = meta_cb;
	SongMetaData_clear(&song_meta_);
}

static void output_omxplayer_closer(GPid omx_pid, gint status, gpointer *data) {
	Log_info("omxplayer", "omxplayer.bin closed... cleaning up");

	player_state = OMX_STOPPED;

        if (play_trans_callback_) {
            play_trans_callback_(PLAY_STOPPED);
        }

        g_io_channel_shutdown(omx_in_ch, TRUE, NULL);
        g_io_channel_unref(omx_in_ch);

        g_free(omx_argv[omx_argv_url_pos]);
        omx_argv[omx_argv_url_pos] = NULL;

	g_spawn_close_pid(omx_pid);
}

static int output_omxplayer_stop(void) {
	Log_info("omxplayer", "stop");

        if (get_current_player_state() != OMX_STOPPED) {
            // send q via stdin, cleanup in _watcher
            g_io_channel_write_chars(omx_in_ch, "q", -1, NULL, NULL);
            g_io_channel_flush(omx_in_ch, NULL);
        }
        return 0;
}

static int output_omxplayer_play(output_transition_cb_t callback) {
	Log_info("omxplayer", "play");

        play_trans_callback_ = callback;

        if (get_current_player_state() != OMX_STOPPED) {
            output_omxplayer_stop();
        }

        // prepare command line
        omx_argv[omx_argv_url_pos] = omx_uri_;

        GError *g_error = NULL;
        g_spawn_async_with_pipes(
                NULL, omx_argv, NULL,
                G_SPAWN_DO_NOT_REAP_CHILD |
                G_SPAWN_SEARCH_PATH_FROM_ENVP,
                NULL, NULL, &omx_pid, &omx_in, NULL, NULL, &g_error
        );

        if(g_error) {
            Log_error("omxplayer", "Error: %s", g_error->message);
            g_error_free(g_error);
            return -1;
        }

        Log_info("omxplayer", "omxplayer spawned");

        g_child_watch_add(omx_pid, (GChildWatchFunc)output_omxplayer_closer, NULL);

        Log_info("omxplayer", "omxplayer watcher added");

#ifdef G_OS_WIN32
        omx_in_ch = g_io_channel_win32_new_fd(omx_in);
#else
        omx_in_ch = g_io_channel_unix_new(omx_in);
#endif
        Log_info("omxplayer", "omxplayer channel setup");

        // temporary set metadata
        //meta_update_callback_(&song_meta_);
	Log_info("omxplayer", "play finished");
        return 0;
}

static int output_omxplayer_pause(void) {
	Log_info("omxplayer", "pause");
        if (get_current_player_state() != OMX_STOPPED) {
            g_io_channel_write_chars(omx_in_ch, "p", -1, NULL, NULL);
            g_io_channel_flush(omx_in_ch, NULL);
        }
        player_state = player_state==OMX_RUNNING ? OMX_PAUSED : OMX_RUNNING;
        return 0;
}

static int output_omxplayer_seek(gint64 position_nanos) {
	Log_info("omxplayer", "seek %ld position_nanos", position_nanos);
        return 0;
        /*
	if (gst_element_seek(player_, 1.0, GST_FORMAT_TIME,
			     GST_SEEK_FLAG_FLUSH,
			     GST_SEEK_TYPE_SET, position_nanos,
			     GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE)) {
		return -1;
	} else {
		return 0;
	}
        */
}

static int output_omxplayer_get_position(gint64 *track_duration,
					 gint64 *track_pos) {
	Log_info("omxplayer", "get_position");
        return 0;

	/*
	*track_duration = last_known_time_.duration;
	*track_pos = last_known_time_.position;

	int rc = 0;
	if (get_current_player_state() != GST_STATE_PLAYING) {
		return rc;  // playbin2 only returns valid values then.
	}
#if (GST_VERSION_MAJOR < 1)
	GstFormat fmt = GST_FORMAT_TIME;
	GstFormat* query_type = &fmt;
#else
	GstFormat query_type = GST_FORMAT_TIME;
#endif	
	if (!gst_element_query_duration(player_, query_type, track_duration)) {
		Log_error("gstreamer", "Failed to get track duration.");
		rc = -1;
	}
	if (!gst_element_query_position(player_, query_type, track_pos)) {
		Log_error("gstreamer", "Failed to get track pos");
		rc = -1;
	}
	// playbin2 does not allow to query while paused. Remember in case
	// we're asked then (it actually returns something, but it is bogus).
	last_known_time_.duration = *track_duration;
	last_known_time_.position = *track_pos;
	return rc;
        */
}

static int output_omxplayer_get_volume(float *v) {
	Log_info("omxplayer", "get_volume");
        return 0;
	/*
	double volume;
	g_object_get(player_, "volume", &volume, NULL);
	Log_info("gstreamer", "Query volume fraction: %f", volume);
	*v = volume;
	return 0;
        */
}
static int output_omxplayer_set_volume(float value) {
	Log_info("omxplayer", "Set volume fraction to %f", value);
        return 0;
        /*
	g_object_set(player_, "volume", (double) value, NULL);
	return 0;
        */
}
static int output_omxplayer_get_mute(int *m) {
	Log_info("omxplayer", "get_mute");
        return 0;
	/*
	gboolean val;
	g_object_get(player_, "mute", &val, NULL);
	*m = val;
	return 0;
        */
}
static int output_omxplayer_set_mute(int m) {
	Log_info("omxplayer", "Set mute to %s", m ? "on" : "off");
//	g_object_set(player_, "mute", (gboolean) m, NULL);
	return 0;
}

static int output_omxplayer_init(void){
	Log_info("omxplayer", "init");
        SongMetaData_init(&song_meta_);

        g_bus_get(
                G_BUS_TYPE_SESSION,
                NULL,
                (GAsyncReadyCallback) dbus_callback_setup,
                NULL
        );
        return 0;
/*
	GstBus *bus;

	SongMetaData_init(&song_meta_);
	scan_mime_list();

#if (GST_VERSION_MAJOR < 1)
	const char player_element_name[] = "playbin2";
#else
	const char player_element_name[] = "playbin";
#endif

	player_ = gst_element_factory_make(player_element_name, "play");
	assert(player_ != NULL);

	bus = gst_pipeline_get_bus(GST_PIPELINE(player_));
	gst_bus_add_watch(bus, my_bus_callback, NULL);
	gst_object_unref(bus);

	if (audio_sink != NULL) {
		GstElement *sink = NULL;
		Log_info("gstreamer", "Setting audio sink to %s; device=%s\n",
			 audio_sink, audio_device ? audio_device : "");
		sink = gst_element_factory_make (audio_sink, "sink");
		if (sink == NULL) {
		  Log_error("gstreamer", "Couldn't create sink '%s'",
			    audio_sink);
		} else {
		  if (audio_device != NULL) {
		    g_object_set (G_OBJECT(sink), "device", audio_device, NULL);
		  }
		  g_object_set (G_OBJECT (player_), "audio-sink", sink, NULL);
		}
	}
	if (videosink != NULL) {
		GstElement *sink = NULL;
		Log_info("gstreamer", "Setting video sink to %s", videosink);
		sink = gst_element_factory_make (videosink, "sink");
		g_object_set (G_OBJECT (player_), "video-sink", sink, NULL);
	}

	if (gst_element_set_state(player_, GST_STATE_READY) ==
	    GST_STATE_CHANGE_FAILURE) {
		Log_error("gstreamer", "Error: pipeline doesn't become ready.");
	}

	g_signal_connect(G_OBJECT(player_), "about-to-finish",
			 G_CALLBACK(prepare_next_stream), NULL);
	output_gstreamer_set_mute(0);
	if (initial_db < 0) {
		output_gstreamer_set_volume(exp(initial_db / 20 * log(10)));
	}

	return 0;
        */
}

struct output_module omxplayer_output = {
        .shortname = "omxplayer",
	.description = "OMXPlayer used by Raspberry Pi's.",
	.init        = output_omxplayer_init,
	.add_options = output_omxplayer_add_options,
	.set_uri     = output_omxplayer_set_uri,
	.set_next_uri= output_omxplayer_set_next_uri,
	.play        = output_omxplayer_play,
	.stop        = output_omxplayer_stop,
	.pause       = output_omxplayer_pause,
	.seek        = output_omxplayer_seek,
	.get_position = output_omxplayer_get_position,
	.get_volume  = output_omxplayer_get_volume,
	.set_volume  = output_omxplayer_set_volume,
	.get_mute  = output_omxplayer_get_mute,
	.set_mute  = output_omxplayer_set_mute,
};
