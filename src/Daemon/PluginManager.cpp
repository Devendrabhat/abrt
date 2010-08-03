/*
    PluginManager.cpp

    Copyright (C) 2009  Zdenek Prikryl (zprikryl@redhat.com)
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
#include <dlfcn.h>
#include "abrtlib.h"
#include "abrt_exception.h"
#include "comm_layer_inner.h"
#include "PluginManager.h"

using namespace std;


/**
 * CLoadedModule class. A class which contains a loaded plugin.
 */
class CLoadedModule
{
    private:
        /* dlopen'ed library */
        void *m_pHandle;
        const plugin_info_t *m_pPluginInfo;
        CPlugin* (*m_pFnPluginNew)();

    public:
        CLoadedModule(void *handle, const char *mod_name);
        ~CLoadedModule()             { dlclose(m_pHandle); }
        int GetMagicNumber()         { return m_pPluginInfo->m_nMagicNumber; }
        const char *GetVersion()     { return m_pPluginInfo->m_sVersion; }
        const char *GetName()        { return m_pPluginInfo->m_sName; }
        const char *GetDescription() { return m_pPluginInfo->m_sDescription; }
        const char *GetEmail()       { return m_pPluginInfo->m_sEmail; }
        const char *GetWWW()         { return m_pPluginInfo->m_sWWW; }
        const char *GetGTKBuilder()  { return m_pPluginInfo->m_sGTKBuilder; }
        plugin_type_t GetType()      { return m_pPluginInfo->m_Type; }
        CPlugin *PluginNew()         { return m_pFnPluginNew(); }
};
CLoadedModule::CLoadedModule(void *handle, const char *mod_name)
{
    m_pHandle = handle;
    /* All errors are fatal */
#define LOADSYM(fp, handle, name) \
    do { \
        fp = (typeof(fp)) (dlsym(handle, name)); \
        if (!fp) \
            error_msg_and_die("'%s' has no %s entry", mod_name, name); \
    } while (0)

    LOADSYM(m_pPluginInfo, handle, "plugin_info");
    LOADSYM(m_pFnPluginNew, handle, "plugin_new");
#undef LOADSYM
}


/**
 * Text representation of plugin types.
 */
static const char *const plugin_type_str[] = {
    "Analyzer",
    "Action",
    "Reporter",
    "Database"
};


/**
 * A function. It saves settings. On success it returns true, otherwise returns false.
 * @param path A path of config file.
 * @param settings Plugin's settings.
 * @return if it success it returns true, otherwise it returns false.
 */
static bool SavePluginSettings(const char *pPath, const map_plugin_settings_t& pSettings)
{
    FILE* fOut = fopen(pPath, "w");
    if (fOut)
    {
        fprintf(fOut, "# Settings were written by abrt\n");
        map_plugin_settings_t::const_iterator it = pSettings.begin();
        for (; it != pSettings.end(); it++)
        {
            fprintf(fOut, "%s = %s\n", it->first.c_str(), it->second.c_str());
        }
        fclose(fOut);
        return true;
    }
    return false;
}


CPluginManager::CPluginManager()
{}

CPluginManager::~CPluginManager()
{}

void CPluginManager::LoadPlugins()
{
    DIR *dir = opendir(PLUGINS_LIB_DIR);
    if (dir != NULL)
    {
        struct dirent *dent;
        while ((dent = readdir(dir)) != NULL)
        {
            if (!is_regular_file(dent, PLUGINS_LIB_DIR))
                continue;
            char *ext = strrchr(dent->d_name, '.');
            if (!ext || strcmp(ext + 1, PLUGINS_LIB_EXTENSION) != 0)
                continue;
            *ext = '\0';
            if (strncmp(dent->d_name, PLUGINS_LIB_PREFIX, sizeof(PLUGINS_LIB_PREFIX)-1) != 0)
                continue;
            LoadPlugin(dent->d_name + sizeof(PLUGINS_LIB_PREFIX)-1, /*enabled_only:*/ true);
        }
        closedir(dir);
    }
}

void CPluginManager::UnLoadPlugins()
{
    map_loaded_module_t::iterator it_module;
    while ((it_module = m_mapLoadedModules.begin()) != m_mapLoadedModules.end())
    {
        UnLoadPlugin(it_module->first.c_str());
    }
}

CPlugin* CPluginManager::LoadPlugin(const char *pName, bool enabled_only)
{
    map_plugin_t::iterator it_plugin = m_mapPlugins.find(pName);
    if (it_plugin != m_mapPlugins.end())
    {
        return it_plugin->second; /* ok */
    }

    map_string_t plugin_info;
    plugin_info["Name"] = pName;

    const char *conf_name = pName;
    if (strncmp(pName, "Kerneloops", sizeof("Kerneloops")-1) == 0)
    {
        /* Kerneloops{,Scanner,Reporter} share the same .conf file */
        conf_name = "Kerneloops";
    }
    map_plugin_settings_t pluginSettings;
    string conf_fullname = ssprintf(PLUGINS_CONF_DIR"/%s."PLUGINS_CONF_EXTENSION, conf_name);
    LoadPluginSettings(conf_fullname.c_str(), pluginSettings);
    m_map_plugin_settings[pName] = pluginSettings;
    /* If settings are empty, most likely .conf file does not exist.
     * Don't mislead the user: */
    VERB3 if (!pluginSettings.empty()) log("Loaded %s.conf", conf_name);

    if (enabled_only)
    {
        map_plugin_settings_t::iterator it = pluginSettings.find("Enabled");
        if (it == pluginSettings.end() || !string_to_bool(it->second.c_str()))
        {
            plugin_info["Enabled"] = "no";
            string empty;
            plugin_info["Type"] = empty;
            plugin_info["Version"] = empty;
            plugin_info["Description"] = empty;
            plugin_info["Email"] = empty;
            plugin_info["WWW"] = empty;
            plugin_info["GTKBuilder"] = empty;
            m_map_plugin_info[pName] = plugin_info;
            VERB3 log("Plugin %s: 'Enabled' is not set, not loading it (yet)", pName);
            return NULL; /* error */
        }
    }

    string libPath = ssprintf(PLUGINS_LIB_DIR"/"PLUGINS_LIB_PREFIX"%s."PLUGINS_LIB_EXTENSION, pName);
    void *handle = dlopen(libPath.c_str(), RTLD_NOW);
    if (!handle)
    {
        error_msg("Can't load '%s': %s", libPath.c_str(), dlerror());
        return NULL; /* error */
    }
    CLoadedModule *module = new CLoadedModule(handle, pName);
    if (module->GetMagicNumber() != PLUGINS_MAGIC_NUMBER
     || module->GetType() < 0
     || module->GetType() > MAX_PLUGIN_TYPE
    ) {
        error_msg("Can't load non-compatible plugin %s: magic %d != %d or type %d is not in [0,%d]",
                pName,
                module->GetMagicNumber(), PLUGINS_MAGIC_NUMBER,
                module->GetType(), MAX_PLUGIN_TYPE);
        delete module;
        return NULL; /* error */
    }
    VERB3 log("Loaded plugin %s v.%s", pName, module->GetVersion());

    CPlugin *plugin = NULL;
    try
    {
        plugin = module->PluginNew();
        plugin->Init();
        plugin->SetSettings(pluginSettings);
    }
    catch (CABRTException& e)
    {
        error_msg("Can't initialize plugin %s: %s",
                pName,
                e.what()
        );
        delete plugin;
        delete module;
        return NULL; /* error */
    }

    plugin_info["Enabled"] = "yes";
    plugin_info["Type"] = plugin_type_str[module->GetType()];
    //plugin_info["Name"] = module->GetName();
    plugin_info["Version"] = module->GetVersion();
    plugin_info["Description"] = module->GetDescription();
    plugin_info["Email"] = module->GetEmail();
    plugin_info["WWW"] = module->GetWWW();
    plugin_info["GTKBuilder"] = module->GetGTKBuilder();

    m_map_plugin_info[pName] = plugin_info;
    m_mapLoadedModules[pName] = module;
    m_mapPlugins[pName] = plugin;
    log("Registered %s plugin '%s'", plugin_type_str[module->GetType()], pName);
    return plugin; /* ok */
}

void CPluginManager::UnLoadPlugin(const char *pName)
{
    map_loaded_module_t::iterator it_module = m_mapLoadedModules.find(pName);
    if (it_module != m_mapLoadedModules.end())
    {
        map_plugin_t::iterator it_plugin = m_mapPlugins.find(pName);
        if (it_plugin != m_mapPlugins.end()) /* always true */
        {
            it_plugin->second->DeInit();
            delete it_plugin->second;
            m_mapPlugins.erase(it_plugin);
        }
        log("UnRegistered %s plugin %s", plugin_type_str[it_module->second->GetType()], pName);
        delete it_module->second;
        m_mapLoadedModules.erase(it_module);
    }
}

#ifdef PLUGIN_DYNAMIC_LOAD_UNLOAD
void CPluginManager::RegisterPluginDBUS(const char *pName, const char *pDBUSSender)
{
    LoadPlugin(pName);
}

void CPluginManager::UnRegisterPluginDBUS(const char *pName, const char *pDBUSSender)
{
    UnLoadPlugin(pName);
}
#endif

CAnalyzer* CPluginManager::GetAnalyzer(const char *pName)
{
    CPlugin *plugin = LoadPlugin(pName);
    if (!plugin)
    {
        error_msg("Plugin '%s' is not registered", pName);
        return NULL;
    }
    if (m_mapLoadedModules[pName]->GetType() != ANALYZER)
    {
        error_msg("Plugin '%s' is not an analyzer plugin", pName);
        return NULL;
    }
    return (CAnalyzer*)plugin;
}

CReporter* CPluginManager::GetReporter(const char *pName)
{
    CPlugin *plugin = LoadPlugin(pName);
    if (!plugin)
    {
        error_msg("Plugin '%s' is not registered", pName);
        return NULL;
    }
    if (m_mapLoadedModules[pName]->GetType() != REPORTER)
    {
        error_msg("Plugin '%s' is not a reporter plugin", pName);
        return NULL;
    }
    return (CReporter*)plugin;
}

CAction* CPluginManager::GetAction(const char *pName, bool silent)
{
    CPlugin *plugin = LoadPlugin(pName);
    if (!plugin)
    {
        error_msg("Plugin '%s' is not registered", pName);
        return NULL;
    }
    if (m_mapLoadedModules[pName]->GetType() != ACTION)
    {
        if (!silent)
            error_msg("Plugin '%s' is not an action plugin", pName);
        return NULL;
    }
    return (CAction*)plugin;
}

CDatabase* CPluginManager::GetDatabase(const char *pName)
{
    CPlugin *plugin = LoadPlugin(pName);
    if (!plugin)
    {
        throw CABRTException(EXCEP_PLUGIN, "Plugin '%s' is not registered", pName);
    }
    if (m_mapLoadedModules[pName]->GetType() != DATABASE)
    {
        throw CABRTException(EXCEP_PLUGIN, "Plugin '%s' is not a database plugin", pName);
    }
    return (CDatabase*)plugin;
}

plugin_type_t CPluginManager::GetPluginType(const char *pName)
{
    CPlugin *plugin = LoadPlugin(pName);
    if (!plugin)
    {
        throw CABRTException(EXCEP_PLUGIN, "Plugin '%s' is not registered", pName);
    }
    map_loaded_module_t::iterator it_module = m_mapLoadedModules.find(pName);
    return it_module->second->GetType();
}

void CPluginManager::SetPluginSettings(const char *pName,
                                       const char *pUID,
                                       const map_plugin_settings_t& pSettings)
{
    map_loaded_module_t::iterator it_module = m_mapLoadedModules.find(pName);
    if (it_module == m_mapLoadedModules.end())
    {
        return;
    }
    map_plugin_t::iterator it_plugin = m_mapPlugins.find(pName);
    if (it_plugin == m_mapPlugins.end())
    {
        return;
    }
    it_plugin->second->SetSettings(pSettings);

#if 0 /* Writing to ~user/.abrt/ is bad wrt security */
    if (it_module->second->GetType() != REPORTER)
    {
        return;
    }

    const char *home = get_home_dir(xatoi_u(pUID.c_str()));
    if (home == NULL || strlen(home) == 0)
        return;

    string confDir = home + "/.abrt";
    string confPath = confDir + "/" + pName + "."PLUGINS_CONF_EXTENSION;
    uid_t uid = xatoi_u(pUID.c_str());
    struct passwd* pw = getpwuid(uid);
    gid_t gid = pw ? pw->pw_gid : uid;

    struct stat buf;
    if (stat(confDir.c_str(), &buf) != 0)
    {
        if (mkdir(confDir.c_str(), 0700) == -1)
        {
            perror_msg("Can't create dir '%s'", confDir.c_str());
            return;
        }
        if (chmod(confDir.c_str(), 0700) == -1)
        {
            perror_msg("Can't change mod of dir '%s'", confDir.c_str());
            return;
        }
        if (chown(confDir.c_str(), uid, gid) == -1)
        {
            perror_msg("Can't change '%s' ownership to %lu:%lu", confPath.c_str(), (long)uid, (long)gid);
            return;
        }
    }
    else if (!S_ISDIR(buf.st_mode))
    {
        perror_msg("'%s' is not a directory", confDir.c_str());
        return;
    }

    /** we don't want to save it from daemon if it's running under root
    but wi might get back to this once we make the daemon to not run
    with root privileges
    */
    /*
    SavePluginSettings(confPath, pSettings);
    if (chown(confPath.c_str(), uid, gid) == -1)
    {
        perror_msg("Can't change '%s' ownership to %lu:%lu", confPath.c_str(), (long)uid, (long)gid);
        return;
    }
    */
#endif
}

map_plugin_settings_t CPluginManager::GetPluginSettings(const char *pName)
{
    map_plugin_settings_t ret;

    map_loaded_module_t::iterator it_module = m_mapLoadedModules.find(pName);
    if (it_module != m_mapLoadedModules.end())
    {
        map_plugin_t::iterator it_plugin = m_mapPlugins.find(pName);
        if (it_plugin != m_mapPlugins.end())
        {
            VERB3 log("Returning settings for loaded plugin %s", pName);
            ret = it_plugin->second->GetSettings();
            return ret;
        }
    }
    /* else: module is not loaded */
    map_map_string_t::iterator it_settings = m_map_plugin_settings.find(pName);
    if (it_settings != m_map_plugin_settings.end())
    {
        /* but it exists, its settings are available nevertheless */
        VERB3 log("Returning settings for non-loaded plugin %s", pName);
        ret = it_settings->second;
        return ret;
    }

    VERB3 log("Request for settings of unknown plugin %s, returning null result", pName);
    return ret;
}
