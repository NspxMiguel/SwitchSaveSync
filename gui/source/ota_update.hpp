#pragma once
#include <string>
void checkForUpdates(const std::string& nroPath, bool automatic);
// Called after the UI exits and before any requested hbloader restart.
bool finishOtaUpdate();
