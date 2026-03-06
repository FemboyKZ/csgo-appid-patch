#ifndef _INCLUDE_SOURCEMOD_EXTENSION_CONFIG_H_
#define _INCLUDE_SOURCEMOD_EXTENSION_CONFIG_H_

/**
 * @file smsdk_config.h
 * @brief Contains macros for configuring basic extension information.
 */

/* Basic information exposed publicly */
#define SMEXT_CONF_NAME         "CS:GO AppID Patcher"
#define SMEXT_CONF_DESCRIPTION  "Patches the engine to allow archived CS:GO clients to connect"
#define SMEXT_CONF_VERSION      "0.0.1"
#define SMEXT_CONF_AUTHOR       "jvnipers"
#define SMEXT_CONF_URL          "https://github.com/jvnpers/csgo-appid-patch"
#define SMEXT_CONF_LOGTAG       "appid"
#define SMEXT_CONF_LICENSE      "AGPL"
#define SMEXT_CONF_DATESTRING   __DATE__

#define SMEXT_LINK(name) SDKExtension *g_pExtensionIface = name;
#define SMEXT_CONF_METAMOD

#endif // _INCLUDE_SOURCEMOD_EXTENSION_CONFIG_H_
