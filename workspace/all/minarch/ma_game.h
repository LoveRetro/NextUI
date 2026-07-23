#pragma once

void Game_open(char* path);
void Game_close(void);
void Game_changeDisc(char* path);
int extract_zip(char** extensions);
int extract_7z(char** extensions);
