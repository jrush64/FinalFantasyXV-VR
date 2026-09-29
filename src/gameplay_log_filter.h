#pragma once
#include <cstring>
#include "research_diagnostics.h"
inline bool QuietGameplayLog(const char* text){
 if(ResearchDiagnostics::Enabled || !text)return false;
 const char* prefixes[]={"[UIOBJECTIVE]","[DRAWPOSE]","[DRAWPOSE_ROUTE]","[HEADPOSE]","[HEADFRAME]","[PACE]","[SPLITFOV]","[FRAMETIME]","[HUDLENS]","[OBJECTIVE3D]","[TARGET3D]","[INTERACTION3D]","[FOVSET] hit=","[CAMOBS] tracked=","[ISEQ] heartbeat","heartbeat present=","[HUDLAYER] submitted"};
 for(auto* p:prefixes)if(!strncmp(text,p,strlen(p)))return true;return false;
}
