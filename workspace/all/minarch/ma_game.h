#pragma once

void Game_open(char* path);
void Game_close(void);
void Game_changeDisc(char* path);
int Game_getDiscEjectState(void);
int Game_getDiscInsertedState(void);
int Game_setDiscEjectState(int ejected);
int extract_zip(char** extensions);
