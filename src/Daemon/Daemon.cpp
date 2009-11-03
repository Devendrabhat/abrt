/*
    Copyright (C) 2009  Jiri Moskovcak (jmoskovc@redhat.com)
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

#include <syslog.h>
#include <sys/inotify.h>
#include <glib.h>
#include <pthread.h>
#include <string>
#include <limits.h>
#if HAVE_CONFIG_H
    #include <config.h>
#endif
#if HAVE_LOCALE_H
    #include <locale.h>
#endif
#if ENABLE_NLS
    #include <libintl.h>
    #define _(S) gettext(S)
#else
    #define _(S) (S)
#endif
#include "abrtlib.h"
#include "ABRTException.h"
#include "CrashWatcher.h"
#include "Daemon.h"


/* Daemon initializes, then sits in glib main loop, waiting for events.
 * Events can be:
 * - inotify: something new appeared under /var/cache/abrt
 * - DBus: dbus message arrived
 * - signal: we got SIGTERM or SIGINT
 *
 * DBus methods we have:
 * - GetCrashInfos(): returns a vector_crash_infos_t (vector_map_vector_string_t)
 *      of crashes for given uid
 *      v[N]["executable"/"uid"/"kernel"/"backtrace"][N] = "contents"
 * - CreateReport(UUID,force): starts creating a report for /var/cache/abrt/DIR with this UUID.
 *      Returns job id (uint64).
 *      After thread returns, when report creation thread has finished,
 *      JobDone(client_dbus_ID,UUID) dbus signal is emitted.
 * - GetJobResult(UUID): returns map_crash_report_t (map_vector_string_t)
 * - Report(map_crash_report_t (map_vector_string_t[, map_map_string_t])):
 *      "Please report this crash": calls Report() of all registered reporter plugins.
 *      Returns report_status_t (map_vector_string_t) - the status of each call.
 *      2nd parameter is the contents of user's abrt.conf.
 * - DeleteDebugDump(UUID): delete corresponding /var/cache/abrt/DIR. Returns bool
 * - GetPluginsInfo(): returns vector_map_string_t
 * - GetPluginSettings(PluginName): returns map_plugin_settings_t (map_string_t)
 * - SetPluginSettings(PluginName, map_plugin_settings_t): returns void
 * - RegisterPlugin(PluginName): returns void
 * - UnRegisterPlugin(PluginName): returns void
 * - GetSettings(): returns map_abrt_settings_t (map_map_string_t)
 * - SetSettings(map_abrt_settings_t): returns void
 *
 * DBus signals we emit:
 * - Crash(progname,uid) - a new crash occurred (new /var/cache/abrt/DIR is found)
 * - JobDone(client_dbus_ID,UUID) - see CreateReport above.
 *      Sent as unicast to the client which did CreateReport.
 * - Warning(msg,job_id)
 * - Update(msg,job_id)
 *      Both are sent as unicast to last client set by set_client_name(name).
 *      If set_client_name(NULL) was done, they are not sent.
 *
 * TODO:
 * - JobDone signal does not need to pass any parameters
 *   - our clients never send multiple CreateReport's.
 */


#define VAR_RUN_LOCK_FILE   VAR_RUN"/abrt.lock"
#define VAR_RUN_PIDFILE     VAR_RUN"/abrt.pid"


//FIXME: add some struct to be able to join all threads!
typedef struct cron_callback_data_t
{
    std::string m_sPluginName;
    std::string m_sPluginArgs;
    unsigned int m_nTimeout;

    cron_callback_data_t(
                      const std::string& pPluginName,
                      const std::string& pPluginArgs,
                      const unsigned int& pTimeout) :
        m_sPluginName(pPluginName),
        m_sPluginArgs(pPluginArgs),
        m_nTimeout(pTimeout)
    {}
} cron_callback_data_t;


static uint8_t s_sig_caught; /* must be one byte */
static int s_signal_pipe[2];
static int s_signal_pipe_write = -1;
static unsigned s_timeout;
static bool s_exiting;

CCommLayerServer* g_pCommLayer;


static double GetDirSize(const std::string &pPath, std::string *worst_dir = NULL, const char *excluded = NULL)
{
    DIR *dp = opendir(pPath.c_str());
    if (dp == NULL)
        return 0;

    struct dirent *ep;
    struct stat stats;
    double size = 0;
    double maxsz = 0;
    while ((ep = readdir(dp)) != NULL)
    {
        if (dot_or_dotdot(ep->d_name))
            continue;
        std::string dname = pPath + "/" + ep->d_name;
        if (lstat(dname.c_str(), &stats) != 0)
            continue;
        if (S_ISDIR(stats.st_mode))
        {
            double sz = GetDirSize(dname);
            size += sz;

            if (worst_dir && strcmp(excluded, ep->d_name) != 0)
            {
                /* Calculate "weighted" size and age
                 * w = sz_kbytes * age_mins */
                sz /= 1024;
                long age = (time(NULL) - stats.st_mtime) / 60;
                if (age > 0)
                    sz *= age;

                if (sz > maxsz)
                {
                    maxsz = sz;
                    *worst_dir = ep->d_name;
                }
            }
        }
        else if (S_ISREG(stats.st_mode))
        {
            size += stats.st_size;
        }
    }
    closedir(dp);
    return size;
}

static void cron_delete_callback_data_cb(gpointer data)
{
    cron_callback_data_t* cronDeleteCallbackData = static_cast<cron_callback_data_t*>(data);
    delete cronDeleteCallbackData;
}

static gboolean cron_activation_periodic_cb(gpointer data)
{
    cron_callback_data_t* cronPeriodicCallbackData = static_cast<cron_callback_data_t*>(data);
    VERB1 log("Activating plugin: %s", cronPeriodicCallbackData->m_sPluginName.c_str());
    RunAction(DEBUG_DUMPS_DIR,
            cronPeriodicCallbackData->m_sPluginName,
            cronPeriodicCallbackData->m_sPluginArgs);
    return TRUE;
}
static gboolean cron_activation_one_cb(gpointer data)
{
    cron_callback_data_t* cronOneCallbackData = static_cast<cron_callback_data_t*>(data);
    VERB1 log("Activating plugin: %s", cronOneCallbackData->m_sPluginName.c_str());
    RunAction(DEBUG_DUMPS_DIR,
            cronOneCallbackData->m_sPluginName,
            cronOneCallbackData->m_sPluginArgs);
    return FALSE;
}
static gboolean cron_activation_reshedule_cb(gpointer data)
{
    cron_callback_data_t* cronResheduleCallbackData = static_cast<cron_callback_data_t*>(data);
    VERB1 log("Rescheduling plugin: %s", cronResheduleCallbackData->m_sPluginName.c_str());
    cron_callback_data_t* cronPeriodicCallbackData = new cron_callback_data_t(cronResheduleCallbackData->m_sPluginName,
                                                                              cronResheduleCallbackData->m_sPluginArgs,
                                                                              cronResheduleCallbackData->m_nTimeout);
    g_timeout_add_seconds_full(G_PRIORITY_DEFAULT,
                               cronPeriodicCallbackData->m_nTimeout,
                               cron_activation_periodic_cb,
                               static_cast<gpointer>(cronPeriodicCallbackData),
                               cron_delete_callback_data_cb);
    return FALSE;
}

static void SetUpMW()
{
    set_string_t::iterator it_k = g_settings_setOpenGPGPublicKeys.begin();
    for (; it_k != g_settings_setOpenGPGPublicKeys.end(); it_k++)
    {
        LoadOpenGPGPublicKey(it_k->c_str());
    }
    set_string_t::iterator it_b = g_settings_mapBlackList.begin();
    for (; it_b != g_settings_mapBlackList.end(); it_b++)
    {
        g_setBlackList.insert(*it_b);
    }
    VERB1 log("Registering plugins");
    set_string_t::iterator it_p = g_settings_setEnabledPlugins.begin();
    for (; it_p != g_settings_setEnabledPlugins.end(); it_p++)
    {
        g_pPluginManager->RegisterPlugin(*it_p);
    }
    VERB1 log("Adding actions or reporters");
    vector_pair_string_string_t::iterator it_ar = g_settings_vectorActionsAndReporters.begin();
    for (; it_ar != g_settings_vectorActionsAndReporters.end(); it_ar++)
    {
        AddActionOrReporter(it_ar->first, it_ar->second);
    }
    VERB1 log("Adding analyzers, actions or reporters");
    map_analyzer_actions_and_reporters_t::iterator it_aar = g_settings_mapAnalyzerActionsAndReporters.begin();
    for (; it_aar != g_settings_mapAnalyzerActionsAndReporters.end(); it_aar++)
    {
        vector_pair_string_string_t::iterator it_ar = it_aar->second.begin();
        for (; it_ar != it_aar->second.end(); it_ar++)
        {
            AddAnalyzerActionOrReporter(it_aar->first, it_ar->first, it_ar->second);
        }
    }
}

static int SetUpCron()
{
    map_cron_t::iterator it_c = g_settings_mapCron.begin();
    for (; it_c != g_settings_mapCron.end(); it_c++)
    {
        std::string::size_type pos = it_c->first.find(":");
        int timeout = 0;
        int nH = -1;
        int nM = -1;
        int nS = -1;

        if (pos != std::string::npos)
        {
            std::string sH = "";
            std::string sM = "";

            sH = it_c->first.substr(0, pos);
            nH = atoi(sH.c_str());
            nH = nH > 23 ? 23 : nH;
            nH = nH < 0 ? 0 : nH;
            nM = nM > 59 ? 59 : nM;
            nM = nM < 0 ? 0 : nM;
            timeout += nH * 60 * 60;
            sM = it_c->first.substr(pos + 1);
            nM = atoi(sM.c_str());
            timeout += nM * 60;
        }
        else
        {
            std::string sS = "";

            sS = it_c->first;
            nS = atoi(sS.c_str());
            nS = nS <= 0 ? 1 : nS;
            timeout = nS;
        }

        if (nS != -1)
        {
            vector_pair_string_string_t::iterator it_ar = it_c->second.begin();
            for (; it_ar != it_c->second.end(); it_ar++)
            {
                cron_callback_data_t* cronPeriodicCallbackData = new cron_callback_data_t(it_ar->first, it_ar->second, timeout);
                g_timeout_add_seconds_full(G_PRIORITY_DEFAULT,
                                           timeout,
                                           cron_activation_periodic_cb,
                                           static_cast<gpointer>(cronPeriodicCallbackData),
                                           cron_delete_callback_data_cb);
            }
        }
        else
        {
            time_t actTime = time(NULL);
            struct tm locTime;
            localtime_r(&actTime, &locTime);
            locTime.tm_hour = nH;
            locTime.tm_min = nM;
            locTime.tm_sec = 0;
            time_t nextTime = mktime(&locTime);
            if (nextTime == ((time_t)-1))
            {
                /* paranoia */
                perror_msg("Can't set up cron time");
                return -1;
            }
            if (actTime > nextTime)
            {
                timeout = 24*60*60 + (nextTime - actTime);
            }
            else
            {
                timeout = nextTime - actTime;
            }
            vector_pair_string_string_t::iterator it_ar = it_c->second.begin();
            for (; it_ar != it_c->second.end(); it_ar++)
            {
                cron_callback_data_t* cronOneCallbackData = new cron_callback_data_t(it_ar->first, it_ar->second, timeout);
                g_timeout_add_seconds_full(G_PRIORITY_DEFAULT,
                                           timeout,
                                           cron_activation_one_cb,
                                           static_cast<gpointer>(cronOneCallbackData),
                                           cron_delete_callback_data_cb);
                cron_callback_data_t* cronResheduleCallbackData = new cron_callback_data_t(it_ar->first, it_ar->second, 24 * 60 * 60);
                g_timeout_add_seconds_full(G_PRIORITY_DEFAULT,
                                           timeout,
                                           cron_activation_reshedule_cb,
                                           static_cast<gpointer>(cronResheduleCallbackData),
                                           cron_delete_callback_data_cb);
            }
        }
    }
    return 0;
}

static void FindNewDumps(const char* pPath)
{
    VERB1 log("Scanning for unsaved entries");
    struct stat stats;
    DIR *dp;
    vector_string_t dirs;
    // get potential unsaved debugdumps
    dp = opendir(pPath);
    if (dp == NULL)
    {
        perror_msg("Can't open directory '%s'", pPath);
        return;
    }
    struct dirent *ep;
    while ((ep = readdir(dp)))
    {
        if (dot_or_dotdot(ep->d_name))
            continue;
        std::string dname = ssprintf("%s/%s", pPath, ep->d_name);
        if (lstat(dname.c_str(), &stats) == 0)
        {
            if (S_ISDIR(stats.st_mode))
            {
                dirs.push_back(dname);
            }
        }
    }
    closedir(dp);

    vector_string_t::iterator itt = dirs.begin();
    for (; itt != dirs.end(); ++itt)
    {
        map_crash_info_t crashinfo;
        try
        {
            mw_result_t res;
            res = SaveDebugDump(*itt, crashinfo);
            switch (res)
            {
                case MW_OK:
                    VERB1 log("Saving into database (%s)", itt->c_str());
                    RunActionsAndReporters(crashinfo[CD_MWDDD][CD_CONTENT]);
                    break;
                case MW_IN_DB:
                    VERB1 log("Already saved in database (%s)", itt->c_str());
                    break;
                case MW_REPORTED:
                case MW_OCCURED:
                case MW_BLACKLISTED:
                case MW_CORRUPTED:
                case MW_PACKAGE_ERROR:
                case MW_GPG_ERROR:
                case MW_FILE_ERROR:
                default:
//Perhaps corrupted & bad needs to be logged unconditionally,
//already saved one - only on VERB1
                    VERB1 log("Corrupted, bad or already saved crash, deleting");
                    DeleteDebugDumpDir(*itt);
                    break;
            }
        }
        catch (CABRTException& e)
        {
            if (e.type() == EXCEP_FATAL)
            {
                throw e;
            }
            error_msg("%s", e.what().c_str());
        }
    }
}

static int CreatePidFile()
{
    int fd;

    /* JIC */
    unlink(VAR_RUN_PIDFILE);

    /* open the pidfile */
    fd = open(VAR_RUN_PIDFILE, O_WRONLY|O_CREAT|O_EXCL, 0644);
    if (fd >= 0)
    {
        /* write our pid to it */
        char buf[sizeof(int)*3 + 2];
        int len = sprintf(buf, "%u\n", (unsigned)getpid());
        write(fd, buf, len);
        close(fd);
        return 0;
    }

    /* something went wrong */
    perror_msg("Can't open '%s'", VAR_RUN_PIDFILE);
    return -1;
}

static int Lock()
{
    int lfd = open(VAR_RUN_LOCK_FILE, O_RDWR|O_CREAT, 0640);
    if (lfd < 0)
    {
        perror_msg("Can't open '%s'", VAR_RUN_LOCK_FILE);
        return -1;
    }
    if (lockf(lfd, F_TLOCK, 0) < 0)
    {
        perror_msg("Can't lock file '%s'", VAR_RUN_LOCK_FILE);
        return -1;
    }
    return 0;
    /* we leak opened lfd intentionally */
}

static void handle_fatal_signal(int signo)
{
    s_sig_caught = signo;
    VERB3 log("Got signal %d", signo);
    if (s_signal_pipe_write >= 0)
        write(s_signal_pipe_write, &s_sig_caught, 1);
}

/* Signal pipe handler */
static gboolean handle_signal_cb(GIOChannel *gio, GIOCondition condition, gpointer ptr_unused)
{
    char signo;
    gsize len = 0;
    g_io_channel_read(gio, &signo, 1, &len);
    if (len == 1)
    {
        /* we did receive a signal */
        VERB3 log("Got signal %d through signal pipe", signo);
        s_exiting = 1;
        return TRUE;
    }
    return FALSE;
}

/* Inotify handler */
static gboolean handle_inotify_cb(GIOChannel *gio, GIOCondition condition, gpointer ptr_unused)
{
    /* 128 simultaneous actions */
#define INOTIFY_BUFF_SIZE ((sizeof(struct inotify_event) + FILENAME_MAX)*128)
    char *buf = (char*)xmalloc(INOTIFY_BUFF_SIZE);
    gsize len;
    gsize i = 0;
    errno = 0;
    GIOError err = g_io_channel_read(gio, buf, INOTIFY_BUFF_SIZE, &len);
    if (err != G_IO_ERROR_NONE)
    {
        perror_msg("Error reading inotify fd");
        free(buf);
        return FALSE;
    }
    /* reconstruct each event and send message to the dbus */
    while (i < len)
    {
        const char *name = NULL;
        struct inotify_event *event;

        event = (struct inotify_event *) &buf[i];
        if (event->len)
            name = &buf[i] + sizeof (*event);
        i += sizeof (*event) + event->len;

        /* ignore lock files and such */
        if (!(event->mask & IN_ISDIR))
        {
            // Happens all the time during normal run
            //VERB3 log("File '%s' creation detected, ignoring", name);
            continue;
        }

        log("Directory '%s' creation detected", name);

        std::string worst_dir;
        while (g_settings_nMaxCrashReportsSize > 0
         && GetDirSize(DEBUG_DUMPS_DIR, &worst_dir, name) / (1024*1024) >= g_settings_nMaxCrashReportsSize
         && worst_dir != ""
        ) {
            log("Size of '%s' >= %u MB, deleting '%s'", DEBUG_DUMPS_DIR, g_settings_nMaxCrashReportsSize, worst_dir.c_str());
            g_pCommLayer->QuotaExceed(_("Report size exceeded the quota. Please check system's MaxCrashReportsSize value in abrt.conf."));
            DeleteDebugDumpDir(std::string(DEBUG_DUMPS_DIR) + "/" + worst_dir);
            worst_dir = "";
        }

        map_crash_info_t crashinfo;
        try
        {
            mw_result_t res;
            res = SaveDebugDump(std::string(DEBUG_DUMPS_DIR) + "/" + name, crashinfo);
            switch (res)
            {
                case MW_OK:
                    log("New crash, saving...");
                    RunActionsAndReporters(crashinfo[CD_MWDDD][CD_CONTENT]);
                    /* Send dbus signal */
                    if(crashinfo[CD_MWANALYZER][CD_CONTENT] == "Kerneloops")
                    {
                        // When Kerneloops comes it will be sent uid with -1
                        // Applet will detected and show normal user
                        g_pCommLayer->Crash(crashinfo[CD_PACKAGE][CD_CONTENT], to_string("-1"));
                    }
                    else
                    {
                        g_pCommLayer->Crash(crashinfo[CD_PACKAGE][CD_CONTENT], crashinfo[CD_UID][CD_CONTENT]);
                    }
                    break;
                case MW_REPORTED:
                case MW_OCCURED:
                    log("Already saved crash, deleting...");
                    /* Send dbus signal */
                    if(crashinfo[CD_MWANALYZER][CD_CONTENT] == "Kerneloops")
                    {
                        // When Kerneloops comes it will be sent uid with -1
                        // Applet will detected and show normal user
                        g_pCommLayer->Crash(crashinfo[CD_PACKAGE][CD_CONTENT], to_string("-1"));
                    }
                    else
                    {
                        g_pCommLayer->Crash(crashinfo[CD_PACKAGE][CD_CONTENT], crashinfo[CD_UID][CD_CONTENT]);
                    }
                    //DeleteDebugDumpDir(std::string(DEBUG_DUMPS_DIR) + "/" + name);
                    break;
                case MW_BLACKLISTED:
                case MW_CORRUPTED:
                case MW_PACKAGE_ERROR:
                case MW_GPG_ERROR:
                case MW_IN_DB:
                case MW_FILE_ERROR:
                default:
                    log("Corrupted or bad crash, deleting...");
                    DeleteDebugDumpDir(std::string(DEBUG_DUMPS_DIR) + "/" + name);
                    break;
            }
        }
        catch (CABRTException& e)
        {
            warn_client(e.what());
            if (e.type() == EXCEP_FATAL)
            {
                free(buf);
                return -1;
            }
        }
        catch (...)
        {
            free(buf);
            throw;
        }
    } /* while */

    free(buf);
    return TRUE;
}

/* Run main loop with idle timeout.
 * Basically, almost like glib's g_main_run(loop)
 */
static void run_main_loop(GMainLoop* loop)
{
    GMainContext *context = g_main_loop_get_context(loop);

    while (!s_exiting)
    {
        /* we have just a handful of sources, 32 should be ample */
        const unsigned NUM_POLLFDS = 32;
        GPollFD fds[NUM_POLLFDS];
        gboolean some_ready;
        gint max_priority;
        gint timeout;

        some_ready = g_main_context_prepare(context, &max_priority);
        if (some_ready)
            g_main_context_dispatch(context);

        gint nfds = g_main_context_query(context, max_priority, &timeout, fds, NUM_POLLFDS);
        if (nfds > NUM_POLLFDS)
            error_msg_and_die("Internal error");

        if (s_timeout)
            alarm(s_timeout);
        g_poll(fds, nfds, timeout);
        if (s_timeout)
            alarm(0);

        some_ready = g_main_context_check(context, max_priority, fds, nfds);
        if (some_ready)
            g_main_context_dispatch(context);
    }
}

static void start_syslog_logging()
{
    /* Open stdin to /dev/null */
    close(STDIN_FILENO);
    xopen("/dev/null", O_RDWR);
    /* We must not leave fds 0,1,2 closed.
     * Otherwise fprintf(stderr) dumps messages into random fds, etc. */
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    xdup(0);
    xdup(0);
    openlog("abrtd", 0, LOG_DAEMON);
    logmode = LOGMODE_SYSLOG;
}

static void ensure_root_writable_dir(const char *dir)
{
    struct stat sb;

    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
        perror_msg_and_die("Can't create '%s'", dir);
    if (stat(dir, &sb) != 0 || !S_ISDIR(sb.st_mode))
        error_msg_and_die("'%s' is not a directory", dir);
    if ((sb.st_uid != 0 || sb.st_gid != 0) && chown(dir, 0, 0) != 0)
        perror_msg_and_die("Can't set owner 0:0 on '%s'", dir);
    /* We can't allow anyone to create dumps: otherwise users can flood
     * us with thousands of bogus or malicious dumps */
    /* 07000 bits are setuid, setgit, and sticky, and they must be unset */
    /* 00777 bits are usual "rwxrwxrwx" access rights */
    if ((sb.st_mode & 07777) != 0755 && chmod(dir, 0755) != 0)
        perror_msg_and_die("Can't set mode rwxr-xr-x on '%s'", dir);
}

static void sanitize_dump_dir_rights()
{
    ensure_root_writable_dir(DEBUG_DUMPS_DIR);
    ensure_root_writable_dir(DEBUG_DUMPS_DIR"-di"); /* debuginfo cache */
    ensure_root_writable_dir(VAR_RUN"/abrt"); /* temp dir */
}

int main(int argc, char** argv)
{
    bool daemonize = true;
    int opt;
    int parent_pid = getpid();

    setlocale(LC_ALL,"");

#if ENABLE_NLS
    bindtextdomain(PACKAGE, LOCALEDIR);
    textdomain(PACKAGE);
#endif

    while ((opt = getopt(argc, argv, "dsvt:")) != -1)
    {
        unsigned long ul;

        switch (opt)
        {
        case 'd':
            daemonize = false;
            break;
        case 's':
            start_syslog_logging();
            break;
        case 'v':
            g_verbose++;
            break;
        case 't':
            char *end;
            errno = 0;
            s_timeout = ul = strtoul(optarg, &end, 0);
            if (errno == 0 && *end == '\0' && ul <= INT_MAX)
                break;
            /* fall through to error */
        default:
            error_msg_and_die(
                "Usage: abrtd [-dv]\n"
                "\nOptions:"
                "\n\t-d\tDo not daemonize"
                "\n\t-s\tLog to syslog even with -d"
                "\n\t-t SEC\tExit after SEC seconds of inactivity"
                "\n\t-v\tVerbose"
            );
        }
    }

    msg_prefix = "abrtd: "; /* for log(), error_msg() and such */

    xpipe(s_signal_pipe);
    signal(SIGTERM, handle_fatal_signal);
    signal(SIGINT,  handle_fatal_signal);
    if (s_timeout)
        signal(SIGALRM, handle_fatal_signal);

    /* Daemonize unless -d */
    if (daemonize)
    {
        /* forking to background */
        pid_t pid = fork();
        if (pid < 0)
        {
            perror_msg_and_die("Can't fork");
        }
        if (pid > 0)
        {
            /* Parent */
            /* Wait for child to notify us via SIGTERM that it feels ok */
            int i = 20; /* 2 sec */
            while (s_sig_caught == 0 && --i)
            {
                usleep(100 * 1000);
            }
            if (s_sig_caught == SIGTERM)
            {
                exit(0);
            }
            if (s_sig_caught)
            {
                error_msg_and_die("Failed to start: got sig %d", s_sig_caught);
            }
            error_msg_and_die("Failed to start: timeout waiting for child");
        }
        /* Child (daemon) continues */
        setsid(); /* never fails */
        if (g_verbose == 0 && logmode != LOGMODE_SYSLOG)
            start_syslog_logging();
    }

    GMainLoop* pMainloop = NULL;
    GIOChannel* pGiochannel_inotify = NULL;
    GIOChannel* pGiochannel_signal = NULL;
    bool lockfile_created = false;
    bool pidfile_created = false;
    CCrashWatcher watcher;

    /* Initialization */
    try
    {
        init_daemon_logging(&watcher);
        VERB1 log("Creating glib main loop");
        pMainloop = g_main_loop_new(NULL, FALSE);
        /* Watching DEBUG_DUMPS_DIR for new files... */
        VERB1 log("Initializing inotify");
// Enabled again since we have new abrt-pyhook-helper, remove comment when verified to work
        /* FIXME: python hook runs with ordinary user privileges,
         * so it fails if everyone doesn't have write acces
         * to DEBUG_DUMPS_DIR
         */
        sanitize_dump_dir_rights();
        errno = 0;
        int inotify_fd = inotify_init();
        if (inotify_fd == -1)
            perror_msg_and_die("inotify_init failed");
        if (inotify_add_watch(inotify_fd, DEBUG_DUMPS_DIR, IN_CREATE) == -1)
            perror_msg_and_die("inotify_add_watch failed on '%s'", DEBUG_DUMPS_DIR);
        VERB1 log("Loading settings");
        LoadSettings();
        VERB1 log("Loading plugins");
        g_pPluginManager = new CPluginManager();
        g_pPluginManager->LoadPlugins();
        SetUpMW(); /* logging is inside */
        if (SetUpCron() != 0)
            throw 1;
#ifdef ENABLE_DBUS
        VERB1 log("Initializing dbus");
        g_pCommLayer = new CCommLayerServerDBus();
#elif ENABLE_SOCKET
        g_pCommLayer = new CCommLayerServerSocket();
#endif
        if (g_pCommLayer->m_init_error)
            throw 1;
        VERB1 log("Adding inotify watch to glib main loop");
        pGiochannel_inotify = g_io_channel_unix_new(inotify_fd);
        g_io_add_watch(pGiochannel_inotify, G_IO_IN, handle_inotify_cb, NULL);
        /* Add an event source which waits for INT/TERM signal */
        VERB1 log("Adding signal pipe watch to glib main loop");
        pGiochannel_signal = g_io_channel_unix_new(s_signal_pipe[0]);
        g_io_add_watch(pGiochannel_signal, G_IO_IN, handle_signal_cb, NULL);
        /* Mark the territory */
        VERB1 log("Creating lock file");
        if (Lock() != 0)
            throw 1;
        lockfile_created = true;
        VERB1 log("Creating pid file");
        if (CreatePidFile() != 0)
            throw 1;
        pidfile_created = true;
    }
    catch (...)
    {
        /* Initialization error */
        error_msg("Error while initializing daemon");
        /* Inform parent that initialization failed */
        if (daemonize)
            kill(parent_pid, SIGINT);
        goto cleanup;
    }

    /* Inform parent that we initialized ok */
    if (daemonize)
    {
        VERB1 log("Signalling parent");
        kill(parent_pid, SIGTERM);
        if (logmode != LOGMODE_SYSLOG)
            start_syslog_logging();
    }

    /* Only now we want signal pipe to work */
    s_signal_pipe_write = s_signal_pipe[1];

    /* Enter the event loop */
    try
    {
        /* This may take a while, therefore we don't do it in init section */
        FindNewDumps(DEBUG_DUMPS_DIR);
        log("Running...");
        run_main_loop(pMainloop);
    }
    catch (CABRTException& e)
    {
        error_msg("Error: %s", e.what().c_str());
    }
    catch (std::exception& e)
    {
        error_msg("Error: %s", e.what());
    }

 cleanup:
    /* Error or INT/TERM. Clean up, in reverse order.
     * Take care to not undo things we did not do.
     */
    if (pidfile_created)
        unlink(VAR_RUN_PIDFILE);
    if (lockfile_created)
        unlink(VAR_RUN_LOCK_FILE);

    if (pGiochannel_signal)
        g_io_channel_unref(pGiochannel_signal);
    if (pGiochannel_inotify)
        g_io_channel_unref(pGiochannel_inotify);

    delete g_pCommLayer;
    if (g_pPluginManager)
    {
        /* This restores /proc/sys/kernel/core_pattern, among other things: */
        g_pPluginManager->UnLoadPlugins();
        delete g_pPluginManager;
    }
    if (pMainloop)
        g_main_loop_unref(pMainloop);

    /* Exiting */
    if (s_sig_caught && s_sig_caught != SIGALRM)
    {
        error_msg_and_die("Got signal %d, exiting", s_sig_caught);
        signal(s_sig_caught, SIG_DFL);
        raise(s_sig_caught);
    }
    error_msg_and_die("Exiting");
}
