// CRT con bagliore, ultimo passaggio: immagine del gioco (OrigTexture) con curvatura,
// scanline e maschera RGB, più il bagliore calcolato dai passaggi precedenti (Texture).

#pragma parameter CURVATURE "Curvatura dello schermo" 0.06 0.0 0.25 0.01
#pragma parameter SCANLINE "Scanline" 0.45 0.0 1.0 0.05
#pragma parameter MASK "Maschera RGB" 0.20 0.0 0.6 0.02
#pragma parameter BAGLIORE "Intensità del bagliore" 0.6 0.0 2.0 0.1

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
uniform vec2 TextureSize;
uniform vec2 InputSize;
uniform vec2 OrigTextureSize;
uniform vec2 OrigInputSize;
uniform sampler2D Texture;
uniform sampler2D OrigTexture;
COMPAT_VARYING vec4 TEX0;
#ifdef PARAMETER_UNIFORM
uniform float CURVATURE, SCANLINE, MASK, BAGLIORE;
#else
#define CURVATURE 0.06
#define SCANLINE 0.45
#define MASK 0.20
#define BAGLIORE 0.6
#endif

vec2 curve(vec2 uv) {
    uv = uv * 2.0 - 1.0;
    uv += uv * (uv.yx * uv.yx) * CURVATURE;
    return uv * 0.5 + 0.5;
}

void main() {
    vec2 uv = curve(TEX0.xy * TextureSize / InputSize);   // 0..1 sul vetro
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec2 otc = uv * OrigInputSize / OrigTextureSize;
    vec2 texel = 1.0 / OrigTextureSize;
    vec3 col = COMPAT_TEXTURE(OrigTexture, otc).rgb * 0.6
             + COMPAT_TEXTURE(OrigTexture, otc + vec2(texel.x, 0.0)).rgb * 0.2
             + COMPAT_TEXTURE(OrigTexture, otc - vec2(texel.x, 0.0)).rgb * 0.2;
    float line = fract(uv.y * OrigInputSize.y) - 0.5;
    col *= 1.0 - SCANLINE * (line * line * 4.0);
    float m = mod(gl_FragCoord.x, 3.0);
    vec3 mask = vec3(1.0 - MASK);
    if (m < 1.0) mask.r = 1.0; else if (m < 2.0) mask.g = 1.0; else mask.b = 1.0;
    col *= mask * 1.25;
    col += COMPAT_TEXTURE(Texture, uv * InputSize / TextureSize).rgb * BAGLIORE;
    vec2 d = abs(uv - 0.5) * 2.0;
    col *= smoothstep(1.0, 0.92, max(d.x, d.y));
    FragColor = vec4(col, 1.0);
}
#endif
