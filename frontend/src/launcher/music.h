/* Musica di sottofondo del launcher: file in Music/ (MP3, OGG, FLAC, WAV, MOD/XM/IT/S3M),
   in ordine casuale. Usa SDL_mixer, che prende l'uscita audio: i suoni dell'interfaccia
   passano dal mixer su un canale riservato. */
#pragma once
#include <stdbool.h>

void music_init(const char *root);   /* elenca i brani; parte se il volume musica è > 0 */
void music_update(void);             /* ogni fotogramma: volume e brano successivo */
void music_next(void);
void music_stop(void);
void music_quit(void);
int music_count(void);
const char *music_title(void);       /* brano in riproduzione, "" se nessuno */
