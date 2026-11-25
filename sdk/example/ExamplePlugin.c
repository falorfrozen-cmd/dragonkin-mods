/* Example native plugin for the Dragonkin Mod Loader (see ../dml_plugin.h).
 * Build: cl /nologo /O2 /LD ExamplePlugin.c /I.. /FeExamplePlugin.dll */
#include <stdio.h>
#include "dml_plugin.h"

static const DmlHost* host;
static float sinceCheck;

static void OnEnable(void) { host->Log("example plugin enabled"); }
static void OnDisable(void) { host->Log("example plugin disabled"); }

static void OnTick(float seconds) {
    sinceCheck += seconds;
    if (sinceCheck < 0.5f) return; /* twice a second is plenty */
    sinceCheck = 0;
    if (host->GetControl("potions") == 0) return;
    float charges, chargesCur, max, maxCur;
    if (host->GetAttribute("PotionCharges", &charges, &chargesCur) && host->GetAttribute("PotionChargesMax", &max, &maxCur) &&
        chargesCur < maxCur)
        host->SetAttribute("PotionCharges", maxCur, maxCur);
}

static void OnAction(const char* action) {
    if (action && action[0] == 'w') { /* "where" */
        char line[256];
        snprintf(line, sizeof(line), "you are at %s", host->Exec("pos")); /* output: "  x y z" */
        host->Log(line);
    }
}

__declspec(dllexport) int DmlInit(const DmlHost* h, DmlPlugin* plugin) {
    if (!h || h->apiVersion < 1) return 0;
    host = h;
    plugin->apiVersion = DML_API_VERSION;
    plugin->OnEnable = OnEnable;
    plugin->OnDisable = OnDisable;
    plugin->OnTick = OnTick;
    plugin->OnAction = OnAction;
    return 1;
}
