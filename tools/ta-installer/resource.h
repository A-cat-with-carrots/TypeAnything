// Integer resource IDs for installer.rc (shared with main.cpp).
// Avoids string-name FindResource gotchas.

#ifndef TA_INSTALLER_RESOURCE_H
#define TA_INSTALLER_RESOURCE_H

#define IDR_RIME_DLL        1001
#define IDR_WEASELX64_DLL   1002
#define IDR_WEASELSERVER    1003
#define IDR_WEASELDEPLOYER  1004
#define IDR_TA_SETTINGS     1005
#define IDR_WV2_LOADER      1006

#define IDR_TARGET_UI_HTML  2001
#define IDR_TARGET_UI_CSS   2002
#define IDR_TARGET_UI_JS    2003
#define IDR_TARGET_UI_PNG   2004

#define IDR_SCHEMA_YAML     3001
#define IDR_DICT_YAML       3002

#define IDR_INSTALL_HTML    4001
#define IDR_INSTALL_CSS     4002
#define IDR_INSTALL_JS      4003
#define IDR_INSTALL_PNG     4004

// Rime base data (luna_pinyin + default presets). Required on cold machines
// that never had Weasel installed — without these, schema compile fails.
#define IDR_DATA_DEFAULT       5001
#define IDR_DATA_LUNA_DICT     5002
#define IDR_DATA_LUNA_SCHEMA   5003
#define IDR_DATA_ESSAY         5004
#define IDR_DATA_SYMBOLS       5005
#define IDR_DATA_PUNCTUATION   5006
#define IDR_DATA_KEY_BINDINGS  5007
// weasel.yaml — base panel/style template (issue #14). Without this,
// WeaselDeployer can't produce build\weasel.yaml and the candidate window
// renders at ~0px on high-DPI screens.
#define IDR_DATA_WEASEL_YAML   5008

// 4-category + classify prompts (UTF-8). Deployed to %APPDATA%\Rime\.
#define IDR_PROMPTS_TXT        6001

// OpenCC t2s (繁→简) data — deployed to <wdir>\data\opencc\.
#define IDR_OPENCC_T2S_JSON    7001
#define IDR_OPENCC_TS_PHRASES  7002
#define IDR_OPENCC_TS_CHARS    7003

#endif
