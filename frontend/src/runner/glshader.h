/* Shader GLSL sull'immagine del gioco, disegnati dentro il renderer SDL (OpenGL sul Mac,
   OpenGL ES 2 sulla console).

   Formato: quello degli shader GLSL di RetroArch.
   - .glsl: un passaggio; un solo file con le sezioni "#if defined(VERTEX)" e
     "#elif defined(FRAGMENT)"; attributi VertexCoord, TexCoord, COLOR; uniform MVPMatrix,
     FrameDirection, FrameCount, OutputSize, TextureSize, InputSize, Texture.
   - .glslp: catena di più passaggi (shaders, shaderN, scale_typeN, scaleN,
     filter_linearN, aliasN, frame_count_modN, textures, parameters).
   Ogni passaggio vede anche OrigTexture/OrigInputSize/OrigTextureSize (immagine del
   gioco), PassPrevNTexture e <alias>Texture dei passaggi precedenti e le texture
   aggiuntive (LUT) per nome.
   "#pragma parameter NOME "Descrizione" predefinito minimo massimo passo" diventa un
   parametro regolabile dal menu in gioco. */
#pragma once
#include <SDL.h>
#include <stdbool.h>

typedef struct {
    char name[48], desc[96];
    float def, min, max, step, value;
} glsh_param_t;

bool glsh_available(void);                 /* il renderer è OpenGL/GLES e le funzioni ci sono */
bool glsh_load(const char *path);          /* .glsl o .glslp; false se qualcosa non va (dettagli nel log) */
void glsh_unload(void);
bool glsh_loaded(void);

int glsh_param_count(void);
glsh_param_t *glsh_param(int i);
glsh_param_t *glsh_param_find(const char *name);

/* Disegna frame_w×frame_h pixel della texture del gioco nel rettangolo dst del canvas,
   ruotati di rot quarti di giro in senso antiorario. */
void glsh_draw(SDL_Texture *tex, int tex_w, int tex_h, int frame_w, int frame_h,
               SDL_Rect dst, unsigned rot, bool smooth, unsigned frame_count);
