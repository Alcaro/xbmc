/*
 *      Copyright (C) 2005-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "AddonManager.h"

#include <memory>
#include <utility>

#include "Addon.h"
#include "addons/AddonBuilder.h"
#include "addons/ImageResource.h"
#include "addons/LanguageResource.h"
#include "addons/UISoundsResource.h"
#include "addons/Webinterface.h"
#include "AudioDecoder.h"
#include "AudioEncoder.h"
#include "ContextMenuAddon.h"
#include "ContextMenuManager.h"
#include "cores/AudioEngine/DSPAddons/ActiveAEDSP.h"
#include "DllAudioDSP.h"
#include "DllLibCPluff.h"
#include "events/AddonManagementEvent.h"
#include "events/EventLog.h"
#include "LangInfo.h"
#include "PluginSource.h"
#include "Repository.h"
#include "Scraper.h"
#include "Service.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include "Skin.h"
#include "system.h"
#include "threads/SingleLock.h"
#include "Util.h"
#include "utils/JobManager.h"
#include "utils/log.h"
#include "utils/StringUtils.h"
#include "utils/XBMCTinyXML.h"

#ifdef HAS_VISUALISATION
#include "Visualisation.h"
#endif
#ifdef HAS_SCREENSAVER
#include "ScreenSaver.h"
#endif
#ifdef HAS_PVRCLIENTS
#include "pvr/addons/PVRClient.h"
#endif

using namespace XFILE;

namespace ADDON
{

cp_log_severity_t clog_to_cp(int lvl);
void cp_fatalErrorHandler(const char *msg);
void cp_logger(cp_log_severity_t level, const char *msg, const char *apid, void *user_data);

/**********************************************************
 * CAddonMgr
 *
 */

std::map<TYPE, IAddonMgrCallback*> CAddonMgr::m_managers;

static cp_extension_t* GetFirstExtPoint(const cp_plugin_info_t* addon, TYPE type)
{
  for (unsigned int i = 0; i < addon->num_extensions; ++i)
  {
    cp_extension_t* ext = &addon->extensions[i];
    if (strcmp(ext->ext_point_id, "kodi.addon.metadata") == 0 || strcmp(ext->ext_point_id, "xbmc.addon.metadata") == 0)
      continue;

    if (type == ADDON_UNKNOWN)
      return ext;

    if (type == TranslateType(ext->ext_point_id))
      return ext;
  }
  return nullptr;
}

AddonPtr CAddonMgr::Factory(const cp_plugin_info_t* plugin, TYPE type)
{
  CAddonBuilder builder;
  return Factory(plugin, type, builder);
}

AddonPtr CAddonMgr::Factory(const cp_plugin_info_t* plugin, TYPE type, CAddonBuilder& builder)
{
  if (!plugin || !plugin->identifier)
    return nullptr;

  if (!PlatformSupportsAddon(plugin))
    return nullptr;

  cp_extension_t* ext = GetFirstExtPoint(plugin, type);

  if (ext == nullptr && type != ADDON_UNKNOWN)
    return nullptr; // no extension point satisfies the type requirement

  if (ext)
  {
    builder.SetType(TranslateType(ext->ext_point_id));
    builder.SetExtPoint(ext);

    auto libname = CAddonMgr::GetInstance().GetExtValue(ext->configuration, "@library");
    if (libname.empty())
      libname = CAddonMgr::GetInstance().GetPlatformLibraryName(ext->configuration);
    builder.SetLibName(libname);

    {
      //TODO: figure out wtf this is and remove it from here
      /* Check if user directories need to be created */
      const cp_cfg_element_t* settings = CAddonMgr::GetInstance().GetExtElement(ext->configuration, "settings");
      if (settings)
        CheckUserDirs(settings);
    }
  }

  FillCpluffMetadata(plugin, builder);
  return builder.Build();
}

void CAddonMgr::FillCpluffMetadata(const cp_plugin_info_t* plugin, CAddonBuilder& builder)
{
  builder.SetId(plugin->identifier);

  if (plugin->version)
    builder.SetVersion(AddonVersion(plugin->version));

  if (plugin->abi_bw_compatibility)
    builder.SetMinVersion(AddonVersion(plugin->abi_bw_compatibility));

  if (plugin->name)
    builder.SetName(plugin->name);

  if (plugin->provider_name)
    builder.SetAuthor(plugin->provider_name);

  if (plugin->plugin_path && strcmp(plugin->plugin_path, "") != 0)
  {
    builder.SetPath(plugin->plugin_path);
    builder.SetIcon(URIUtils::AddFileToFolder(plugin->plugin_path, "icon.png"));
    builder.SetFanart(URIUtils::AddFileToFolder(plugin->plugin_path, "fanart.jpg"));
    builder.SetChangelog(URIUtils::AddFileToFolder(plugin->plugin_path, "changelog.txt"));
  }

  {
    ADDONDEPS dependencies;
    for (unsigned int i = 0; i < plugin->num_imports; ++i)
    {
      if (plugin->imports[i].plugin_id)
      {
        std::string id(plugin->imports[i].plugin_id);
        AddonVersion version(plugin->imports[i].version ? plugin->imports[i].version : "0.0.0");
        dependencies.emplace(std::move(id), std::make_pair(version, plugin->imports[i].optional != 0));
      }
    }
    builder.SetDependencies(std::move(dependencies));
  }

  auto metadata = CAddonMgr::GetInstance().GetExtension(plugin, "xbmc.addon.metadata");
  if (!metadata)
    metadata = CAddonMgr::GetInstance().GetExtension(plugin, "kodi.addon.metadata");
  if (metadata)
  {
    builder.SetSummary(CAddonMgr::GetInstance().GetTranslatedString(metadata->configuration, "summary"));
    builder.SetDescription(CAddonMgr::GetInstance().GetTranslatedString(metadata->configuration, "description"));
    builder.SetDisclaimer(CAddonMgr::GetInstance().GetTranslatedString(metadata->configuration, "disclaimer"));
    builder.SetLicense(CAddonMgr::GetInstance().GetExtValue(metadata->configuration, "license"));

    std::string language = CAddonMgr::GetInstance().GetExtValue(metadata->configuration, "language");
    if (!language.empty())
    {
      InfoMap extrainfo;
      extrainfo.insert(std::make_pair("language",language));
      builder.SetExtrainfo(std::move(extrainfo));
    }

    builder.SetBroken(CAddonMgr::GetInstance().GetExtValue(metadata->configuration, "broken"));

    if (CAddonMgr::GetInstance().GetExtValue(metadata->configuration, "nofanart") == "true")
      builder.SetFanart("");
    if (CAddonMgr::GetInstance().GetExtValue(metadata->configuration, "noicon") == "true")
      builder.SetIcon("");
    if (CAddonMgr::GetInstance().GetExtValue(metadata->configuration, "nochangelog") == "true")
      builder.SetChangelog("");
  }
}

bool CAddonMgr::CheckUserDirs(const cp_cfg_element_t *settings)
{
  if (!settings)
    return false;

  const cp_cfg_element_t *userdirs = CAddonMgr::GetInstance().GetExtElement((cp_cfg_element_t *)settings, "userdirs");
  if (!userdirs)
    return false;

  ELEMENTS elements;
  if (!CAddonMgr::GetInstance().GetExtElements((cp_cfg_element_t *)userdirs, "userdir", elements))
    return false;

  ELEMENTS::iterator itr = elements.begin();
  while (itr != elements.end())
  {
    std::string path = CAddonMgr::GetInstance().GetExtValue(*itr++, "@path");
    if (!CDirectory::Exists(path))
    {
      if (!CUtil::CreateDirectoryEx(path))
      {
        CLog::Log(LOGERROR, "CAddonMgr::CheckUserDirs: Unable to create directory %s.", path.c_str());
        return false;
      }
    }
  }

  return true;
}

CAddonMgr::CAddonMgr()
  : m_cp_context(nullptr),
  m_cpluff(nullptr)
{ }

CAddonMgr::~CAddonMgr()
{
  DeInit();
}

CAddonMgr &CAddonMgr::GetInstance()
{
  static CAddonMgr sAddonMgr;
  return sAddonMgr;
}

IAddonMgrCallback* CAddonMgr::GetCallbackForType(TYPE type)
{
  if (m_managers.find(type) == m_managers.end())
    return NULL;
  else
    return m_managers[type];
}

bool CAddonMgr::RegisterAddonMgrCallback(const TYPE type, IAddonMgrCallback* cb)
{
  if (cb == NULL)
    return false;

  m_managers.erase(type);
  m_managers[type] = cb;

  return true;
}

void CAddonMgr::UnregisterAddonMgrCallback(TYPE type)
{
  m_managers.erase(type);
}

bool CAddonMgr::Init()
{
  CSingleLock lock(m_critSection);
  m_cpluff = std::unique_ptr<DllLibCPluff>(new DllLibCPluff);
  m_cpluff->Load();

  m_database.Open();

  if (!m_cpluff->IsLoaded())
  {
    CLog::Log(LOGERROR, "ADDONS: Fatal Error, could not load libcpluff");
    return false;
  }

  m_cpluff->set_fatal_error_handler(cp_fatalErrorHandler);

  cp_status_t status;
  status = m_cpluff->init();
  if (status != CP_OK)
  {
    CLog::Log(LOGERROR, "ADDONS: Fatal Error, cp_init() returned status: %i", status);
    return false;
  }

  //TODO could separate addons into different contexts
  // would allow partial unloading of addon framework
  m_cp_context = m_cpluff->create_context(&status);
  assert(m_cp_context);
  status = m_cpluff->register_pcollection(m_cp_context, CSpecialProtocol::TranslatePath("special://home/addons").c_str());
  if (status != CP_OK)
  {
    CLog::Log(LOGERROR, "ADDONS: Fatal Error, cp_register_pcollection() returned status: %i", status);
    return false;
  }

  status = m_cpluff->register_pcollection(m_cp_context, CSpecialProtocol::TranslatePath("special://xbmc/addons").c_str());
  if (status != CP_OK)
  {
    CLog::Log(LOGERROR, "ADDONS: Fatal Error, cp_register_pcollection() returned status: %i", status);
    return false;
  }

  status = m_cpluff->register_pcollection(m_cp_context, CSpecialProtocol::TranslatePath("special://xbmcbin/addons").c_str());
  if (status != CP_OK)
  {
    CLog::Log(LOGERROR, "ADDONS: Fatal Error, cp_register_pcollection() returned status: %i", status);
    return false;
  }

  status = m_cpluff->register_logger(m_cp_context, cp_logger,
      &CAddonMgr::GetInstance(), clog_to_cp(g_advancedSettings.m_logLevel));
  if (status != CP_OK)
  {
    CLog::Log(LOGERROR, "ADDONS: Fatal Error, cp_register_logger() returned status: %i", status);
    return false;
  }

  FindAddonsAndNotify();

  // disable some system addons by default because they are optional
  VECADDONS addons;
  GetAddons(addons, ADDON_PVRDLL);
  GetAddons(addons, ADDON_AUDIODECODER);
  std::string systemAddonsPath = CSpecialProtocol::TranslatePath("special://xbmc/addons");
  for (auto &addon : addons)
  {
    if (StringUtils::StartsWith(addon->Path(), systemAddonsPath))
    {
      if (!m_database.IsSystemAddonRegistered(addon->ID()))
      {
        m_database.DisableAddon(addon->ID());
        m_database.AddSystemAddon(addon->ID());
      }
    }
  }

  std::vector<std::string> disabled;
  m_database.GetDisabled(disabled);
  m_disabled.insert(disabled.begin(), disabled.end());

  std::vector<std::string> blacklisted;
  m_database.GetBlacklisted(blacklisted);
  m_updateBlacklist.insert(blacklisted.begin(), blacklisted.end());

  VECADDONS repos;
  if (GetAddons(repos, ADDON_REPOSITORY))
  {
    VECADDONS::iterator it = repos.begin();
    for (;it != repos.end(); ++it)
      CLog::Log(LOGNOTICE, "ADDONS: Using repository %s", (*it)->ID().c_str());
  }

  return true;
}

void CAddonMgr::DeInit()
{
  m_cpluff.reset();
  m_database.Close();
  m_disabled.clear();
}

bool CAddonMgr::HasAddons(const TYPE &type)
{
  VECADDONS addons;
  return GetAddonsInternal(type, addons, true);
}

bool CAddonMgr::HasInstalledAddons(const TYPE &type)
{
  VECADDONS addons;
  return GetAddonsInternal(type, addons, false);
}

void CAddonMgr::AddToUpdateableAddons(AddonPtr &pAddon)
{
  CSingleLock lock(m_critSection);
  m_updateableAddons.push_back(pAddon);
}

void CAddonMgr::RemoveFromUpdateableAddons(AddonPtr &pAddon)
{
  CSingleLock lock(m_critSection);
  VECADDONS::iterator it = std::find(m_updateableAddons.begin(), m_updateableAddons.end(), pAddon);
  
  if(it != m_updateableAddons.end())
  {
    m_updateableAddons.erase(it);
  }
}

struct AddonIdFinder 
{ 
    AddonIdFinder(const std::string& id)
      : m_id(id)
    {}
    
    bool operator()(const AddonPtr& addon) 
    { 
      return m_id == addon->ID();
    }
    private:
    std::string m_id;
};

bool CAddonMgr::ReloadSettings(const std::string &id)
{
  CSingleLock lock(m_critSection);
  VECADDONS::iterator it = std::find_if(m_updateableAddons.begin(), m_updateableAddons.end(), AddonIdFinder(id));
  
  if( it != m_updateableAddons.end())
  {
    return (*it)->ReloadSettings();
  }
  return false;
}

VECADDONS CAddonMgr::GetAvailableUpdates()
{
  CSingleLock lock(m_critSection);
  auto start = XbmcThreads::SystemClockMillis();

  VECADDONS updates;
  VECADDONS installed;
  GetAddons(installed);
  for (const auto& addon : installed)
  {
    AddonPtr remote;
    if (m_database.GetAddon(addon->ID(), remote) && remote->Version() > addon->Version())
      updates.emplace_back(std::move(remote));
  }
  CLog::Log(LOGDEBUG, "CAddonMgr::GetAvailableUpdates took %i ms", XbmcThreads::SystemClockMillis() - start);
  return updates;
}

bool CAddonMgr::HasAvailableUpdates()
{
  return !GetAvailableUpdates().empty();
}

bool CAddonMgr::GetAddons(VECADDONS& addons)
{
  return GetAddonsInternal(ADDON_UNKNOWN, addons, true);
}

bool CAddonMgr::GetAddons(VECADDONS& addons, const TYPE& type)
{
  return GetAddonsInternal(type, addons, true);
}

bool CAddonMgr::GetInstalledAddons(VECADDONS& addons)
{
  return GetAddonsInternal(ADDON_UNKNOWN, addons, false);
}

bool CAddonMgr::GetInstalledAddons(VECADDONS& addons, const TYPE& type)
{
  return GetAddonsInternal(type, addons, false);
}

bool CAddonMgr::GetDisabledAddons(VECADDONS& addons)
{
  return CAddonMgr::GetDisabledAddons(addons, ADDON_UNKNOWN);
}

bool CAddonMgr::GetDisabledAddons(VECADDONS& addons, const TYPE& type)
{
  VECADDONS all;
  if (CAddonMgr::GetInstance().GetInstalledAddons(all, type))
  {
    std::copy_if(all.begin(), all.end(), std::back_inserter(addons),
        [this](const AddonPtr& addon){ return IsAddonDisabled(addon->ID()); });
    return true;
  }
  return false;
}

bool CAddonMgr::GetInstallableAddons(VECADDONS& addons)
{
  return GetInstallableAddons(addons, ADDON_UNKNOWN);
}

bool CAddonMgr::GetInstallableAddons(VECADDONS& addons, const TYPE &type)
{
  CSingleLock lock(m_critSection);

  // get all addons
  VECADDONS installableAddons;
  if (!m_database.GetRepositoryContent(installableAddons))
    return false;

  // go through all addons and remove all that are already installed
  for (const auto& addon : installableAddons)
  {
    // check if the addon matches the provided addon type
    if (type != ADDON::ADDON_UNKNOWN && addon->Type() != type && !addon->IsType(type))
      continue;

    if (!CanAddonBeInstalled(addon))
      continue;

    addons.push_back(addon);
  }

  return true;
}

bool CAddonMgr::GetAddonsInternal(const TYPE &type, VECADDONS &addons, bool enabledOnly)
{
  CSingleLock lock(m_critSection);
  if (!m_cp_context)
    return false;

  std::vector<CAddonBuilder> builders;
  m_database.GetInstalled(builders);

  for (auto& builder : builders)
  {
    cp_status_t status;
    cp_plugin_info_t* cp_addon = m_cpluff->get_plugin_info(m_cp_context, builder.GetId().c_str(), &status);
    if (status == CP_OK && cp_addon)
    {
      if (enabledOnly && IsAddonDisabled(cp_addon->identifier))
      {
        m_cpluff->release_info(m_cp_context, cp_addon);
        continue;
      }

      //FIXME: hack for skipping special dependency addons (xbmc.python etc.).
      //Will break if any extension point is added to them
      cp_extension_t *props = GetFirstExtPoint(cp_addon, type);
      if (props == nullptr)
      {
        m_cpluff->release_info(m_cp_context, cp_addon);
        continue;
      }

      AddonPtr addon = Factory(cp_addon, type, builder);
      m_cpluff->release_info(m_cp_context, cp_addon);

      if (addon)
      {
        // if the addon has a running instance, grab that
        AddonPtr runningAddon = addon->GetRunningInstance();
        if (runningAddon)
          addon = runningAddon;
        addons.push_back(addon);
      }
    }
  }
  return addons.size() > 0;
}

bool CAddonMgr::GetAddon(const std::string &str, AddonPtr &addon, const TYPE &type/*=ADDON_UNKNOWN*/, bool enabledOnly /*= true*/)
{
  CSingleLock lock(m_critSection);

  cp_status_t status;
  cp_plugin_info_t *cpaddon = m_cpluff->get_plugin_info(m_cp_context, str.c_str(), &status);
  if (status == CP_OK && cpaddon)
  {
    addon = Factory(cpaddon, type);
    m_cpluff->release_info(m_cp_context, cpaddon);

    if (addon)
    {
      if (enabledOnly && IsAddonDisabled(addon->ID()))
        return false;

      // if the addon has a running instance, grab that
      AddonPtr runningAddon = addon->GetRunningInstance();
      if (runningAddon)
        addon = runningAddon;
    }
    return NULL != addon.get();
  }
  if (cpaddon)
    m_cpluff->release_info(m_cp_context, cpaddon);

  return false;
}

//TODO handle all 'default' cases here, not just scrapers & vizs
bool CAddonMgr::GetDefault(const TYPE &type, AddonPtr &addon)
{
  std::string setting;
  switch (type)
  {
  case ADDON_VIZ:
    setting = CSettings::GetInstance().GetString(CSettings::SETTING_MUSICPLAYER_VISUALISATION);
    break;
  case ADDON_SCREENSAVER:
    setting = CSettings::GetInstance().GetString(CSettings::SETTING_SCREENSAVER_MODE);
    break;
  case ADDON_SCRAPER_ALBUMS:
    setting = CSettings::GetInstance().GetString(CSettings::SETTING_MUSICLIBRARY_ALBUMSSCRAPER);
    break;
  case ADDON_SCRAPER_ARTISTS:
    setting = CSettings::GetInstance().GetString(CSettings::SETTING_MUSICLIBRARY_ARTISTSSCRAPER);
    break;
  case ADDON_SCRAPER_MOVIES:
    setting = CSettings::GetInstance().GetString(CSettings::SETTING_SCRAPERS_MOVIESDEFAULT);
    break;
  case ADDON_SCRAPER_MUSICVIDEOS:
    setting = CSettings::GetInstance().GetString(CSettings::SETTING_SCRAPERS_MUSICVIDEOSDEFAULT);
    break;
  case ADDON_SCRAPER_TVSHOWS:
    setting = CSettings::GetInstance().GetString(CSettings::SETTING_SCRAPERS_TVSHOWSDEFAULT);
    break;
  case ADDON_WEB_INTERFACE:
    setting = CSettings::GetInstance().GetString(CSettings::SETTING_SERVICES_WEBSKIN);
    break;
  case ADDON_RESOURCE_LANGUAGE:
    setting = CSettings::GetInstance().GetString(CSettings::SETTING_LOCALE_LANGUAGE);
    break;
  default:
    return false;
  }
  return GetAddon(setting, addon, type);
}

bool CAddonMgr::SetDefault(const TYPE &type, const std::string &addonID)
{
  switch (type)
  {
  case ADDON_VIZ:
    CSettings::GetInstance().SetString(CSettings::SETTING_MUSICPLAYER_VISUALISATION, addonID);
    break;
  case ADDON_SCREENSAVER:
    CSettings::GetInstance().SetString(CSettings::SETTING_SCREENSAVER_MODE, addonID);
    break;
  case ADDON_SCRAPER_ALBUMS:
    CSettings::GetInstance().SetString(CSettings::SETTING_MUSICLIBRARY_ALBUMSSCRAPER, addonID);
    break;
  case ADDON_SCRAPER_ARTISTS:
    CSettings::GetInstance().SetString(CSettings::SETTING_MUSICLIBRARY_ARTISTSSCRAPER, addonID);
    break;
  case ADDON_SCRAPER_MOVIES:
    CSettings::GetInstance().SetString(CSettings::SETTING_SCRAPERS_MOVIESDEFAULT, addonID);
    break;
  case ADDON_SCRAPER_MUSICVIDEOS:
    CSettings::GetInstance().SetString(CSettings::SETTING_SCRAPERS_MUSICVIDEOSDEFAULT, addonID);
    break;
  case ADDON_SCRAPER_TVSHOWS:
    CSettings::GetInstance().SetString(CSettings::SETTING_SCRAPERS_TVSHOWSDEFAULT, addonID);
    break;
  case ADDON_RESOURCE_LANGUAGE:
    CSettings::GetInstance().SetString(CSettings::SETTING_LOCALE_LANGUAGE, addonID);
    break;
  case ADDON_SCRIPT_WEATHER:
     CSettings::GetInstance().SetString(CSettings::SETTING_WEATHER_ADDON, addonID);
    break;
  case ADDON_SKIN:
    CSettings::GetInstance().SetString(CSettings::SETTING_LOOKANDFEEL_SKIN, addonID);
    break;
  case ADDON_RESOURCE_UISOUNDS:
    CSettings::GetInstance().SetString(CSettings::SETTING_LOOKANDFEEL_SOUNDSKIN, addonID);
    break;
  default:
    return false;
  }

  return true;
}

bool CAddonMgr::FindAddons()
{
  bool result = false;
  CSingleLock lock(m_critSection);
  if (m_cpluff && m_cp_context)
  {
    result = true;
    m_cpluff->scan_plugins(m_cp_context, CP_SP_UPGRADE);

    //Sync with db
    {
      std::set<std::string> installed;
      cp_status_t status;
      int n;
      cp_plugin_info_t** cp_addons = m_cpluff->get_plugins_info(m_cp_context, &status, &n);
      for (int i = 0; i < n; ++i)
        installed.insert(cp_addons[i]->identifier);
      m_cpluff->release_info(m_cp_context, cp_addons);
      m_database.SyncInstalled(installed);
    }

    SetChanged();
  }

  return result;
}

bool CAddonMgr::FindAddonsAndNotify()
{
  if (!FindAddons())
    return false;

  NotifyObservers(ObservableMessageAddons);

  return true;
}

void CAddonMgr::UnregisterAddon(const std::string& ID)
{
  CSingleLock lock(m_critSection);
  if (m_cpluff && m_cp_context)
  {
    m_cpluff->uninstall_plugin(m_cp_context, ID.c_str());
    SetChanged();
    lock.Leave();
    NotifyObservers(ObservableMessageAddons);
  }
}

void CAddonMgr::OnPostUnInstall(const std::string& id)
{
  CSingleLock lock(m_critSection);
  m_disabled.erase(id);
  m_updateBlacklist.erase(id);
}

bool CAddonMgr::RemoveFromUpdateBlacklist(const std::string& id)
{
  CSingleLock lock(m_critSection);
  if (!IsBlacklisted(id))
    return true;
  return m_database.RemoveAddonFromBlacklist(id) && m_updateBlacklist.erase(id) > 0;
}

bool CAddonMgr::AddToUpdateBlacklist(const std::string& id)
{
  CSingleLock lock(m_critSection);
  if (IsBlacklisted(id))
    return true;
  return m_database.BlacklistAddon(id) && m_updateBlacklist.insert(id).second;
}

bool CAddonMgr::IsBlacklisted(const std::string& id) const
{
  CSingleLock lock(m_critSection);
  return m_updateBlacklist.find(id) != m_updateBlacklist.end();
}

bool CAddonMgr::DisableAddon(const std::string& id)
{
  CSingleLock lock(m_critSection);
  if (m_disabled.find(id) != m_disabled.end())
    return true; //already disabled

  if (!CanAddonBeDisabled(id))
    return false;
  if (!m_database.DisableAddon(id))
    return false;
  if (!m_disabled.insert(id).second)
    return false;

  AddonPtr addon;
  if (GetAddon(id, addon, ADDON_UNKNOWN, false) && addon != NULL)
    CEventLog::GetInstance().Add(EventPtr(new CAddonManagementEvent(addon, 24141)));

  //success
  ADDON::OnDisabled(id);
  return true;
}

bool CAddonMgr::EnableAddon(const std::string& id)
{
  CSingleLock lock(m_critSection);
  if (m_disabled.find(id) == m_disabled.end())
    return true; //already enabled

  if (!m_database.DisableAddon(id, false))
    return false;
  if (m_disabled.erase(id) == 0)
    return false;

  AddonPtr addon;
  if (GetAddon(id, addon, ADDON_UNKNOWN, false) && addon != NULL)
    CEventLog::GetInstance().Add(EventPtr(new CAddonManagementEvent(addon, 24064)));

  //success
  ADDON::OnEnabled(id);
  return true;
}

bool CAddonMgr::IsAddonDisabled(const std::string& ID)
{
  CSingleLock lock(m_critSection);
  return m_disabled.find(ID) != m_disabled.end();
}

bool CAddonMgr::CanAddonBeDisabled(const std::string& ID)
{
  if (ID.empty())
    return false;

  CSingleLock lock(m_critSection);
  AddonPtr localAddon;
  // can't disable an addon that isn't installed
  if (!GetAddon(ID, localAddon, ADDON_UNKNOWN, false))
    return false;

  // can't disable an addon that is in use
  if (localAddon->IsInUse())
    return false;

  // installed PVR addons can always be disabled
  if (localAddon->Type() == ADDON_PVRDLL ||
      localAddon->Type() == ADDON_ADSPDLL)
    return true;

  // installed audio decoder addons can always be disabled
  if (localAddon->Type() == ADDON_AUDIODECODER)
    return true;

  // installed audio encoder addons can always be disabled
  if (localAddon->Type() == ADDON_AUDIOENCODER)
    return true;

  std::string systemAddonsPath = CSpecialProtocol::TranslatePath("special://xbmc/addons");
  // can't disable system addons
  if (StringUtils::StartsWith(localAddon->Path(), systemAddonsPath))
    return false;

  return true;
}

bool CAddonMgr::IsAddonInstalled(const std::string& ID)
{
  AddonPtr tmp;
  return GetAddon(ID, tmp, ADDON_UNKNOWN, false);
}

bool CAddonMgr::CanAddonBeInstalled(const AddonPtr& addon)
{
  if (addon == NULL)
    return false;

  CSingleLock lock(m_critSection);
  // can't install already installed addon
  if (IsAddonInstalled(addon->ID()))
    return false;

  // can't install broken addons
  if (!addon->Broken().empty())
    return false;

  return true;
}

std::string CAddonMgr::GetTranslatedString(const cp_cfg_element_t *root, const char *tag)
{
  if (!root)
    return "";

  std::map<std::string, std::string> translatedValues;
  for (unsigned int i = 0; i < root->num_children; i++)
  {
    const cp_cfg_element_t &child = root->children[i];
    if (strcmp(tag, child.name) == 0)
    {
      // see if we have a "lang" attribute
      const char *lang = m_cpluff->lookup_cfg_value((cp_cfg_element_t*)&child, "@lang");
      if (lang != NULL && g_langInfo.GetLocale().Matches(lang))
        translatedValues.insert(std::make_pair(lang, child.value != NULL ? child.value : ""));
      else if (lang == NULL || strcmp(lang, "en") == 0 || strcmp(lang, "en_GB") == 0)
        translatedValues.insert(std::make_pair("en_GB", child.value != NULL ? child.value : ""));
      else if (strcmp(lang, "no") == 0)
        translatedValues.insert(std::make_pair("nb_NO", child.value != NULL ? child.value : ""));
    }
  }

  // put together a list of languages
  std::set<std::string> languages;
  for (auto const& translatedValue : translatedValues)
    languages.insert(translatedValue.first);

  // find the language from the list that matches the current locale best
  std::string matchingLanguage = g_langInfo.GetLocale().FindBestMatch(languages);
  if (matchingLanguage.empty())
    matchingLanguage = "en_GB";

  auto const& translatedValue = translatedValues.find(matchingLanguage);
  if (translatedValue != translatedValues.end())
    return translatedValue->second;

  return "";
}

/*
 * libcpluff interaction
 */

bool CAddonMgr::PlatformSupportsAddon(const cp_plugin_info_t *plugin)
{
  auto *metadata = CAddonMgr::GetInstance().GetExtension(plugin, "xbmc.addon.metadata");
  if (!metadata)
    metadata = CAddonMgr::GetInstance().GetExtension(plugin, "kodi.addon.metadata");

  // if platforms are not specified, assume supported
  if (!metadata)
    return true;

  std::vector<std::string> platforms;
  if (!CAddonMgr::GetInstance().GetExtList(metadata->configuration, "platform", platforms))
    return true;

  if (platforms.empty())
    return true;

  std::vector<std::string> supportedPlatforms = {
    "all",
#if defined(TARGET_ANDROID)
    "android",
#elif defined(TARGET_RASPBERRY_PI)
    "rbpi",
    "linux",
#elif defined(TARGET_FREEBSD)
    "freebsd",
    "linux",
#elif defined(TARGET_LINUX)
    "linux",
#elif defined(TARGET_WINDOWS) && defined(HAS_DX)
    "windx",
    "windows",
#elif defined(TARGET_DARWIN_IOS)
    "ios",
#elif defined(TARGET_DARWIN_OSX)
    "osx",
#if defined(__x86_64__)
    "osx64",
#else
    "osx32",
#endif
#endif
  };

  return std::find_first_of(platforms.begin(), platforms.end(),
      supportedPlatforms.begin(), supportedPlatforms.end()) != platforms.end();
}

cp_cfg_element_t *CAddonMgr::GetExtElement(cp_cfg_element_t *base, const char *path)
{
  cp_cfg_element_t *element = NULL;
  if (base)
    element = m_cpluff->lookup_cfg_element(base, path);
  return element;
}

bool CAddonMgr::GetExtElements(cp_cfg_element_t *base, const char *path, ELEMENTS &elements)
{
  if (!base || !path)
    return false;

  for (unsigned int i = 0; i < base->num_children; i++)
  {
    std::string temp = base->children[i].name;
    if (!temp.compare(path))
      elements.push_back(&base->children[i]);
  }

  return !elements.empty();
}

const cp_extension_t *CAddonMgr::GetExtension(const cp_plugin_info_t *props, const char *extension) const
{
  if (!props)
    return NULL;
  for (unsigned int i = 0; i < props->num_extensions; ++i)
  {
    if (0 == strcmp(props->extensions[i].ext_point_id, extension))
      return &props->extensions[i];
  }
  return NULL;
}

std::string CAddonMgr::GetExtValue(cp_cfg_element_t *base, const char *path) const
{
  const char *value = "";
  if (base && (value = m_cpluff->lookup_cfg_value(base, path)))
    return value;
  else
    return "";
}

bool CAddonMgr::GetExtList(cp_cfg_element_t *base, const char *path, std::vector<std::string> &result) const
{
  result.clear();
  if (!base || !path)
    return false;
  const char *all = m_cpluff->lookup_cfg_value(base, path);
  if (!all || *all == 0)
    return false;
  StringUtils::Tokenize(all, result, ' ');
  return true;
}

std::string CAddonMgr::GetPlatformLibraryName(cp_cfg_element_t *base) const
{
  std::string libraryName;
#if defined(TARGET_ANDROID)
  libraryName = GetExtValue(base, "@library_android");
#elif defined(TARGET_LINUX) || defined(TARGET_FREEBSD)
#if defined(TARGET_FREEBSD)
  libraryName = GetExtValue(base, "@library_freebsd");
  if (libraryName.empty())
#elif defined(TARGET_RASPBERRY_PI)
  libraryName = GetExtValue(base, "@library_rbpi");
  if (libraryName.empty())
#endif
  libraryName = GetExtValue(base, "@library_linux");
#elif defined(TARGET_WINDOWS) && defined(HAS_DX)
  libraryName = GetExtValue(base, "@library_windx");
  if (libraryName.empty())
    libraryName = GetExtValue(base, "@library_windows");
#elif defined(TARGET_DARWIN)
#if defined(TARGET_DARWIN_IOS)
  libraryName = GetExtValue(base, "@library_ios");
  if (libraryName.empty())
#endif
  libraryName = GetExtValue(base, "@library_osx");
#endif

  return libraryName;
}

// FIXME: This function may not be required
bool CAddonMgr::LoadAddonDescription(const std::string &path, AddonPtr &addon)
{
  cp_status_t status;
  cp_plugin_info_t *info = m_cpluff->load_plugin_descriptor(m_cp_context, CSpecialProtocol::TranslatePath(path).c_str(), &status);
  if (info)
  {
    addon = Factory(info, ADDON_UNKNOWN);
    m_cpluff->release_info(m_cp_context, info);
    return NULL != addon.get();
  }
  return false;
}

bool CAddonMgr::AddonsFromRepoXML(const TiXmlElement *root, VECADDONS &addons)
{
  // create a context for these addons
  cp_status_t status;
  cp_context_t *context = m_cpluff->create_context(&status);
  if (!root || !context)
    return false;

  // each addon XML should have a UTF-8 declaration
  TiXmlDeclaration decl("1.0", "UTF-8", "");
  const TiXmlElement *element = root->FirstChildElement("addon");
  while (element)
  {
    // dump the XML back to text
    std::string xml;
    xml << decl;
    xml << *element;
    cp_status_t status;
    cp_plugin_info_t *info = m_cpluff->load_plugin_descriptor_from_memory(context, xml.c_str(), xml.size(), &status);
    if (info)
    {
      AddonPtr addon = Factory(info, ADDON_UNKNOWN);
      if (addon.get())
        addons.push_back(addon);
      m_cpluff->release_info(context, info);
    }
    element = element->NextSiblingElement("addon");
  }
  m_cpluff->destroy_context(context);
  return true;
}

bool CAddonMgr::LoadAddonDescriptionFromMemory(const TiXmlElement *root, AddonPtr &addon)
{
  // create a context for these addons
  cp_status_t status;
  cp_context_t *context = m_cpluff->create_context(&status);
  if (!root || !context)
    return false;

  // dump the XML back to text
  std::string xml;
  xml << TiXmlDeclaration("1.0", "UTF-8", "");
  xml << *root;
  cp_plugin_info_t *info = m_cpluff->load_plugin_descriptor_from_memory(context, xml.c_str(), xml.size(), &status);
  if (info)
  {
    addon = Factory(info, ADDON_UNKNOWN);
    m_cpluff->release_info(context, info);
  }
  m_cpluff->destroy_context(context);
  return addon != NULL;
}

bool CAddonMgr::StartServices(const bool beforelogin)
{
  CLog::Log(LOGDEBUG, "ADDON: Starting service addons.");

  VECADDONS services;
  if (!GetAddons(services, ADDON_SERVICE))
    return false;

  bool ret = true;
  for (IVECADDONS it = services.begin(); it != services.end(); ++it)
  {
    std::shared_ptr<CService> service = std::dynamic_pointer_cast<CService>(*it);
    if (service)
    {
      if ( (beforelogin && service->GetStartOption() == CService::STARTUP)
        || (!beforelogin && service->GetStartOption() == CService::LOGIN) )
        ret &= service->Start();
    }
  }

  return ret;
}

void CAddonMgr::StopServices(const bool onlylogin)
{
  CLog::Log(LOGDEBUG, "ADDON: Stopping service addons.");

  VECADDONS services;
  if (!GetAddons(services, ADDON_SERVICE))
    return;

  for (IVECADDONS it = services.begin(); it != services.end(); ++it)
  {
    std::shared_ptr<CService> service = std::dynamic_pointer_cast<CService>(*it);
    if (service)
    {
      if ( (onlylogin && service->GetStartOption() == CService::LOGIN)
        || (!onlylogin) )
        service->Stop();
    }
  }
}

int cp_to_clog(cp_log_severity_t lvl)
{
  if (lvl >= CP_LOG_ERROR)
    return LOGINFO;
  return LOGDEBUG;
}

cp_log_severity_t clog_to_cp(int lvl)
{
  if (lvl >= LOG_LEVEL_DEBUG)
    return CP_LOG_INFO;
  return CP_LOG_ERROR;
}

void cp_fatalErrorHandler(const char *msg)
{
  CLog::Log(LOGERROR, "ADDONS: CPluffFatalError(%s)", msg);
}

void cp_logger(cp_log_severity_t level, const char *msg, const char *apid, void *user_data)
{
  if(!apid)
    CLog::Log(cp_to_clog(level), "ADDON: cpluff: '%s'", msg);
  else
    CLog::Log(cp_to_clog(level), "ADDON: cpluff: '%s' reports '%s'", apid, msg);
}

} /* namespace ADDON */

