// Bagliore, sfocatura verticale delle zone più luminose dell'immagine.



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
uniform sampler2D Texture;
COMPAT_VARYING vec4 TEX0;
#ifdef PARAMETER_UNIFORM
uniform float SOGLIA;
#else
#define SOGLIA 0.45
#endif

vec3 bright(vec2 tc) {
    vec3 c = COMPAT_TEXTURE(Texture, tc).rgb;
    return c;
}

void main() {
    vec2 d = vec2(0.0, 1.0) / TextureSize;
    // nove campioni con pesi gaussiani
    vec3 sum = bright(TEX0.xy) * 0.227;
    sum += (bright(TEX0.xy + d) + bright(TEX0.xy - d)) * 0.194;
    sum += (bright(TEX0.xy + 2.0 * d) + bright(TEX0.xy - 2.0 * d)) * 0.121;
    sum += (bright(TEX0.xy + 3.0 * d) + bright(TEX0.xy - 3.0 * d)) * 0.054;
    sum += (bright(TEX0.xy + 4.0 * d) + bright(TEX0.xy - 4.0 * d)) * 0.016;
    FragColor = vec4(sum, 1.0);
}
#endif
