// Pixel nitidi: ogni pixel del gioco resta un quadrato pieno, ma il bordo tra due pixel
// viene sfumato su un solo pixel dello schermo. Così l'immagine non ha scalettature
// irregolari anche quando lo schermo non è un multiplo esatto della risoluzione del gioco.
// Richiede il filtro lineare ("smooth": true). Formato GLSL di RetroArch.

#if defined(VERTEX)
#if __VERSION__ >= 130
#define COMPAT_VARYING out
#define COMPAT_ATTRIBUTE in
#else
#define COMPAT_VARYING varying
#define COMPAT_ATTRIBUTE attribute
#endif

COMPAT_ATTRIBUTE vec4 VertexCoord;
COMPAT_ATTRIBUTE vec4 TexCoord;
COMPAT_VARYING vec4 TEX0;
uniform mat4 MVPMatrix;

void main() {
    gl_Position = MVPMatrix * VertexCoord;
    TEX0 = TexCoord;
}

#elif defined(FRAGMENT)
#ifdef GL_ES
precision mediump float;
#endif
#if __VERSION__ >= 130
#define COMPAT_VARYING in
#define COMPAT_TEXTURE texture
out vec4 FragColor;
#else
#define COMPAT_VARYING varying
#define FragColor gl_FragColor
#define COMPAT_TEXTURE texture2D
#endif

uniform vec2 OutputSize;
uniform vec2 TextureSize;
uniform vec2 InputSize;
uniform sampler2D Texture;
COMPAT_VARYING vec4 TEX0;

void main() {
    vec2 pix = TEX0.xy * TextureSize;                 // posizione in pixel del gioco
    vec2 zoom = OutputSize / InputSize;               // pixel dello schermo per pixel del gioco
    vec2 cell = floor(pix);
    vec2 inside = fract(pix) - 0.5;                   // -0.5..0.5 dentro il pixel
    // zona piena al centro del pixel; la sfumatura occupa mezzo pixel di schermo per lato
    vec2 edge = 0.5 - 0.5 / zoom;   /* non "flat": è una parola riservata in GLSL ES (la GPU della console non lo compilava) */
    vec2 blend = (inside - clamp(inside, -edge, edge)) * zoom + 0.5;
    FragColor = vec4(COMPAT_TEXTURE(Texture, (cell + blend) / TextureSize).rgb, 1.0);
}
#endif
