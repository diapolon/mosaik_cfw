// CRT curvo: schermo a tubo con curvatura, scanline, maschera RGB, bagliore e angoli
// arrotondati. Formato GLSL a passaggio singolo di RetroArch (sezioni VERTEX/FRAGMENT).

#pragma parameter CURVATURE "Curvatura dello schermo" 0.07 0.0 0.25 0.01
#pragma parameter SCANLINE "Scanline" 0.50 0.0 1.0 0.05
#pragma parameter MASK "Maschera RGB" 0.22 0.0 0.6 0.02
#pragma parameter GLOW "Bagliore" 0.15 0.0 0.6 0.05
#pragma parameter BOOST "Luminosità" 1.30 0.8 2.0 0.05

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

#ifdef PARAMETER_UNIFORM
uniform float CURVATURE, SCANLINE, MASK, GLOW, BOOST;
#else
#define CURVATURE 0.07   // quanto è bombato lo schermo
#define SCANLINE 0.50    // profondità delle righe scure tra le linee
#define MASK 0.22        // intensità della maschera RGB
#define GLOW 0.15        // bagliore attorno ai pixel luminosi
#define BOOST 1.30       // compensa la luminosità persa con scanline e maschera
#endif

// coordinate 0..1 sullo schermo visibile -> coordinate sul vetro curvo
vec2 curve(vec2 uv) {
    uv = uv * 2.0 - 1.0;
    uv += uv * (uv.yx * uv.yx) * CURVATURE;
    return uv * 0.5 + 0.5;
}

void main() {
    vec2 scale = TextureSize / InputSize;          // coordinata di texture -> 0..1
    vec2 uv = curve(TEX0.xy * scale);
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec2 tc = uv / scale;
    vec2 texel = 1.0 / TextureSize;

    // campionamento con una leggera sfocatura orizzontale, come il fascio di un tubo
    vec3 col = COMPAT_TEXTURE(Texture, tc).rgb * 0.6
             + COMPAT_TEXTURE(Texture, tc + vec2(texel.x, 0.0)).rgb * 0.2
             + COMPAT_TEXTURE(Texture, tc - vec2(texel.x, 0.0)).rgb * 0.2;
    vec3 glow = (COMPAT_TEXTURE(Texture, tc + texel * vec2(2.0, 1.0)).rgb
               + COMPAT_TEXTURE(Texture, tc - texel * vec2(2.0, 1.0)).rgb
               + COMPAT_TEXTURE(Texture, tc + texel * vec2(-2.0, 1.0)).rgb
               + COMPAT_TEXTURE(Texture, tc - texel * vec2(-2.0, 1.0)).rgb) * 0.25;
    col += glow * GLOW;

    // scanline: massima luminosità al centro di ogni riga del gioco
    float line = fract(uv.y * InputSize.y) - 0.5;
    col *= 1.0 - SCANLINE * (line * line * 4.0);

    // maschera RGB sui pixel dello schermo
    float m = mod(gl_FragCoord.x, 3.0);
    vec3 mask = vec3(1.0 - MASK);
    if (m < 1.0) mask.r = 1.0; else if (m < 2.0) mask.g = 1.0; else mask.b = 1.0;
    col *= mask * BOOST;

    // bordi più scuri e angoli arrotondati del vetro
    vec2 d = abs(uv - 0.5) * 2.0;
    float edge = smoothstep(1.0, 0.92, max(d.x, d.y)) * (1.0 - 0.25 * dot(d, d) * 0.5);
    FragColor = vec4(col * edge, 1.0);
}
#endif
