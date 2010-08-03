/*
    Copyright (C) 2009  RedHat inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
#include "dbus.h"
#include "dbus_common.h"

DBusConnection* s_dbus_conn;

/*
 * DBus member calls
 */

/* helpers */
static DBusMessage* new_call_msg(const char* method)
{
    DBusMessage* msg = dbus_message_new_method_call(ABRTD_DBUS_NAME, ABRTD_DBUS_PATH, ABRTD_DBUS_IFACE, method);
    if (!msg)
        die_out_of_memory();
    return msg;
}

static DBusMessage* send_get_reply_and_unref(DBusMessage* msg)
{
    dbus_uint32_t serial;
    if (TRUE != dbus_connection_send(s_dbus_conn, msg, &serial))
        error_msg_and_die("Error sending DBus message");
    dbus_message_unref(msg);

    int cnt = 99; // needed only for old dbus-libs compat (see below)
    while (true)
    {
        DBusMessage *received = dbus_connection_pop_message(s_dbus_conn);
        if (!received)
        {
            DBusDispatchStatus s = dbus_connection_get_dispatch_status(s_dbus_conn);
            // This is what we want to do:
            //if (FALSE == dbus_connection_read_write(s_dbus_conn, -1))
            //    error_msg_and_die("dbus connection closed");
            // And this is what we forced to do to work around old
            // dbus-libs-1.1.2, which may return FALSE even when
            // there _is_ more data:
            dbus_connection_read_write(s_dbus_conn, -1);
            if (--cnt == 0)
                error_msg_and_die("dbus connection closed");

            continue;
        }

        int tp = dbus_message_get_type(received);
        const char *error_str = dbus_message_get_error_name(received);
#if 0
        /* Debugging */
        printf("type:%u (CALL:%u, RETURN:%u, ERROR:%u, SIGNAL:%u)\n", tp,
                                DBUS_MESSAGE_TYPE_METHOD_CALL,
                                DBUS_MESSAGE_TYPE_METHOD_RETURN,
                                DBUS_MESSAGE_TYPE_ERROR,
                                DBUS_MESSAGE_TYPE_SIGNAL
        );
        const char *sender = dbus_message_get_sender(received);
        if (sender)
            printf("sender: %s\n", sender);
        const char *path = dbus_message_get_path(received);
        if (path)
            printf("path: %s\n", path);
        const char *member = dbus_message_get_member(received);
        if (member)
            printf("member: %s\n", member);
        const char *interface = dbus_message_get_interface(received);
        if (interface)
            printf("interface: %s\n", interface);
        const char *destination = dbus_message_get_destination(received);
        if (destination)
            printf("destination: %s\n", destination);
        if (error_str)
            printf("error: '%s'\n", error_str);
#endif

        DBusError err;
        dbus_error_init(&err);

        if (dbus_message_is_signal(received, ABRTD_DBUS_IFACE, "Update"))
        {
            const char *update_msg;
            if (!dbus_message_get_args(received, &err,
                                   DBUS_TYPE_STRING, &update_msg,
                                   DBUS_TYPE_INVALID))
            {
                error_msg_and_die("dbus Update message: arguments mismatch");
            }
            printf(">> %s\n", update_msg);
        }
        else if (dbus_message_is_signal(received, ABRTD_DBUS_IFACE, "Warning"))
        {
            const char *warning_msg;
            if (!dbus_message_get_args(received, &err,
                                   DBUS_TYPE_STRING, &warning_msg,
                                   DBUS_TYPE_INVALID))
            {
                error_msg_and_die("dbus Warning message: arguments mismatch");
            }
            log(">! %s\n", warning_msg);
        }
        else
        if (tp == DBUS_MESSAGE_TYPE_METHOD_RETURN
         && dbus_message_get_reply_serial(received) == serial
        ) {
            return received;
        }
        else
        if (tp == DBUS_MESSAGE_TYPE_ERROR
         && dbus_message_get_reply_serial(received) == serial
        ) {
            error_msg_and_die("dbus call returned error: '%s'", error_str);
        }

        dbus_message_unref(received);
    }
}

vector_map_crash_data_t call_GetCrashInfos()
{
    DBusMessage* msg = new_call_msg(__func__ + 5);
    DBusMessage *reply = send_get_reply_and_unref(msg);

    DBusMessageIter in_iter;
    dbus_message_iter_init(reply, &in_iter);

    vector_map_crash_data_t argout;
    int r = load_val(&in_iter, argout);
    if (r != ABRT_DBUS_LAST_FIELD) /* more values present, or bad type */
        error_msg_and_die("dbus call %s: return type mismatch", __func__ + 5);

    dbus_message_unref(reply);
    return argout;
}

map_crash_data_t call_CreateReport(const char* crash_id)
{
    DBusMessage* msg = new_call_msg(__func__ + 5);
    dbus_message_append_args(msg,
            DBUS_TYPE_STRING, &crash_id,
            DBUS_TYPE_INVALID);

    DBusMessage *reply = send_get_reply_and_unref(msg);

    DBusMessageIter in_iter;
    dbus_message_iter_init(reply, &in_iter);

    map_crash_data_t argout;
    int r = load_val(&in_iter, argout);
    if (r != ABRT_DBUS_LAST_FIELD) /* more values present, or bad type */
        error_msg_and_die("dbus call %s: return type mismatch", __func__ + 5);

    dbus_message_unref(reply);
    return argout;
}

report_status_t call_Report(const map_crash_data_t& report,
			    const vector_string_t& reporters,
			    const map_map_string_t &plugins)
{
    DBusMessage* msg = new_call_msg(__func__ + 5);
    DBusMessageIter out_iter;
    dbus_message_iter_init_append(msg, &out_iter);

    /* parameter #1: report data */
    store_val(&out_iter, report);
    /* parameter #2: reporters to use */
    store_val(&out_iter, reporters);
    /* parameter #3 (opt): plugin config */
    if (!plugins.empty())
        store_val(&out_iter, plugins);

    DBusMessage *reply = send_get_reply_and_unref(msg);

    DBusMessageIter in_iter;
    dbus_message_iter_init(reply, &in_iter);

    report_status_t result;
    int r = load_val(&in_iter, result);
    if (r != ABRT_DBUS_LAST_FIELD) /* more values present, or bad type */
        error_msg_and_die("dbus call %s: return type mismatch", __func__ + 5);

    dbus_message_unref(reply);
    return result;
}

int32_t call_DeleteDebugDump(const char* crash_id)
{
    DBusMessage* msg = new_call_msg(__func__ + 5);
    dbus_message_append_args(msg,
            DBUS_TYPE_STRING, &crash_id,
            DBUS_TYPE_INVALID);

    DBusMessage *reply = send_get_reply_and_unref(msg);

    DBusMessageIter in_iter;
    dbus_message_iter_init(reply, &in_iter);

    int32_t result;
    int r = load_val(&in_iter, result);
    if (r != ABRT_DBUS_LAST_FIELD) /* more values present, or bad type */
        error_msg_and_die("dbus call %s: return type mismatch", __func__ + 5);

    dbus_message_unref(reply);
    return result;
}

map_map_string_t call_GetPluginsInfo()
{
    DBusMessage *msg = new_call_msg(__func__ + 5);
    DBusMessage *reply = send_get_reply_and_unref(msg);

    DBusMessageIter in_iter;
    dbus_message_iter_init(reply, &in_iter);

    map_map_string_t argout;
    int r = load_val(&in_iter, argout);
    if (r != ABRT_DBUS_LAST_FIELD) /* more values present, or bad type */
        error_msg_and_die("dbus call %s: return type mismatch", __func__ + 5);

    dbus_message_unref(reply);
    return argout;
}

map_plugin_settings_t call_GetPluginSettings(const char *name)
{
    DBusMessage *msg = new_call_msg(__func__ + 5);
    dbus_message_append_args(msg,
            DBUS_TYPE_STRING, &name,
            DBUS_TYPE_INVALID);

    DBusMessage *reply = send_get_reply_and_unref(msg);

    DBusMessageIter in_iter;
    dbus_message_iter_init(reply, &in_iter);

    map_string_t argout;
    int r = load_val(&in_iter, argout);
    if (r != ABRT_DBUS_LAST_FIELD) /* more values present, or bad type */
        error_msg_and_die("dbus call %s: return type mismatch", __func__ + 5);

    dbus_message_unref(reply);
    return argout;
}

map_map_string_t call_GetSettings()
{
    DBusMessage *msg = new_call_msg(__func__ + 5);
    DBusMessage *reply = send_get_reply_and_unref(msg);

    DBusMessageIter in_iter;
    dbus_message_iter_init(reply, &in_iter);
    map_map_string_t argout;
    int r = load_val(&in_iter, argout);
    if (r != ABRT_DBUS_LAST_FIELD) /* more values present, or bad type */
        error_msg_and_die("dbus call %s: return type mismatch", __func__ + 5);

    dbus_message_unref(reply);
    return argout;
}

void handle_dbus_err(bool error_flag, DBusError *err)
{
    if (dbus_error_is_set(err))
    {
        error_msg("dbus error: %s", err->message);
        /* dbus_error_free(&err); */
        error_flag = true;
    }
    if (!error_flag)
        return;
    error_msg_and_die(
            "error requesting DBus name %s, possible reasons: "
            "abrt run by non-root; dbus config is incorrect; "
            "or dbus daemon needs to be restarted to reload dbus config",
            ABRTD_DBUS_NAME);
}
