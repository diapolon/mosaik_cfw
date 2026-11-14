/* Musica di sottofondo del launcher: file in Music/Menu (MP3, OGG, FLAC, WAV), in ordine casuale,
   spenta di partenza. Usa SDL_mixer, che prende l'uscita audio: i suoni dell'interfaccia
   passano dal mixer su un canale riservato. */
#pragma once
#include <stdbool.h>

void music_init(const char *root);   /* elenca i brani; parte se il volume musica è > 0 */
void music_update(void);             /* ogni fotogramma: volume e brano successivo */
void music_next(void);
void music_user_start(void);         /* scelta esplicita: toglie il silenzio lasciato dal Lettore musicale */
void music_stop(void);
void music_quit(void);              /* solo uscendo: chiama prima ui_exit_code */
int music_count(void);
const char *music_title(void);       /* brano in riproduzione, "" se nessuno */
