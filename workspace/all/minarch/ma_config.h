#pragma once

char* getScreenScalingDesc(void);
int getScreenScalingCount(void);
void setOverclock(int i);
void updateCPUMonitor(void);
void Config_syncFrontend(char* key, int value);
void Config_init(void);
void Config_quit(void);
void Config_load(void);
void Config_free(void);
void Config_readOptions(void);
void Config_readControls(void);
void Config_write(int override);
void Config_restore(void);
void Config_syncShaders(char* key, int value);
void applyShaderSettings(void);
void initShaders(void);
char** list_files_in_folder(const char* folderPath, int* fileCount, const char* defaultElement, const char* extensionFilter);
