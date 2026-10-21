#version 440

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec2 vCanvas;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    vec4 canvasSize;
    vec4 adjust;       // brilho, contraste, saturação, temperatura
    vec4 chromaColor;  // rgb + ativo (a)
    vec4 chromaParams; // similaridade, suavidade
    vec4 mask;         // tipo, progresso, aspecto
    vec4 misc;         // opacidade, modo de mesclagem, backdrop invertido (y), camada de ajuste
    vec4 adjust2;      // exposição, matiz, realces, sombras
    vec4 adjust3;      // vinheta, nitidez, intensidade do filtro, intensidade do LUT
    vec4 clipMask;     // forma, inverter, suavização, arredondamento
    vec4 clipMaskGeom; // centro x, centro y, largura, altura (frações do quadro)
    vec4 clipMaskExtra; // rotação (rad), proporção do quadro
    vec4 grade;        // correção avançada (curvas/HSL/rodas) ativa
    vec4 effect;       // efeito de vídeo: índice, intensidade, tempo no clipe (s); w = fonte pré-multiplicada
} ubuf;

layout(binding = 1) uniform sampler2D layerTex;
layout(binding = 2) uniform sampler2D backdropTex;
layout(binding = 3) uniform sampler3D filterLut;
layout(binding = 4) uniform sampler3D userLut;
layout(binding = 5) uniform sampler3D gradeLut; // curvas, HSL e rodas de cor (gerado em core/Lut.cpp)

const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);

vec3 rgb2yuv(vec3 c)
{
    float y = dot(c, LUMA);
    return vec3(y, (c.b - y) * 0.5389, (c.r - y) * 0.6350);
}

vec3 applyLut(sampler3D lut, vec3 c)
{
    float n = float(textureSize(lut, 0).x);
    vec3 coord = clamp(c, 0.0, 1.0) * ((n - 1.0) / n) + 0.5 / n; // centro dos texels
    return texture(lut, coord).rgb;
}

vec3 grade(vec3 c, vec2 uv)
{
    // Exposição (±2 stops), depois brilho/contraste/saturação.
    c *= exp2(ubuf.adjust2.x * 2.0);
    c += ubuf.adjust.x * 0.5;
    c = (c - 0.5) * (1.0 + ubuf.adjust.y) + 0.5;
    float l = dot(c, LUMA);
    c = mix(vec3(l), c, 1.0 + ubuf.adjust.z);
    // Temperatura (azul..âmbar) e matiz (verde..magenta).
    float t = ubuf.adjust.w;
    float tint = ubuf.adjust2.y;
    c += vec3(t, t * 0.2, -t) * 0.15 + vec3(tint, -tint * 2.0, tint) * 0.05;
    // Realces e sombras atuam em faixas de luminância.
    l = dot(clamp(c, 0.0, 1.0), LUMA);
    c += ubuf.adjust2.z * 0.35 * smoothstep(0.45, 1.0, l);
    c += ubuf.adjust2.w * 0.35 * (1.0 - smoothstep(0.0, 0.55, l));
    c = clamp(c, 0.0, 1.0);
    // Correção avançada: rodas de cor, HSL e curvas, juntas num LUT.
    if (ubuf.grade.x > 0.5)
        c = applyLut(gradeLut, c);
    // Filtro (LUT embutido) e LUT do usuário, com intensidade.
    if (ubuf.adjust3.z > 0.0)
        c = mix(c, applyLut(filterLut, c), ubuf.adjust3.z);
    if (ubuf.adjust3.w > 0.0)
        c = mix(c, applyLut(userLut, c), ubuf.adjust3.w);
    // Vinheta relativa ao quadro do clipe.
    if (ubuf.adjust3.x > 0.0) {
        float d = distance(uv, vec2(0.5)) * 1.41421;
        c *= 1.0 - ubuf.adjust3.x * smoothstep(0.35, 1.0, d);
    }
    return c;
}

// ---------------- Efeitos de vídeo (índices em core/Effects.cpp) ----------------

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

/** Desfoque gaussiano 5×5 com raio `r` (fração da altura). */
vec4 blur5(vec2 uv, float r, vec2 res)
{
    vec4 acc = vec4(0.0);
    float wsum = 0.0;
    vec2 step = vec2(r * 0.5 * res.y / res.x, r * 0.5);
    for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x) {
            float w = exp(-float(x * x + y * y) / 4.0);
            acc += texture(layerTex, uv + vec2(float(x), float(y)) * step) * w;
            wsum += w;
        }
    return acc / wsum;
}

/** Amostra da camada com os efeitos que mexem na leitura da imagem. */
vec4 effectSample(vec2 uv)
{
    int e = int(ubuf.effect.x + 0.5);
    float I = ubuf.effect.y;
    float t = ubuf.effect.z;
    if (e == 0 || I <= 0.0)
        return texture(layerTex, uv);
    vec2 res = vec2(textureSize(layerTex, 0));
    if (e == 1) { // tremor: solavancos 12 vezes por segundo, com leve zoom para esconder a borda
        float k = floor(t * 12.0);
        vec2 o = vec2(hash(vec2(k, 1.7)), hash(vec2(k, 9.2))) - 0.5;
        return texture(layerTex, 0.5 + (uv - 0.5) / (1.0 + 0.08 * I) + o * 0.05 * I);
    }
    if (e == 2) // desfoque
        return blur5(uv, 0.03 * I, res);
    if (e == 3) { // glitch: faixas deslocadas e cores separadas, em rajadas
        float k = floor(t * 14.0);
        float band = floor(uv.y * 18.0);
        float burst = step(1.0 - 0.5 * I, hash(vec2(k, 3.1)));
        if (hash(vec2(band, k)) > 1.0 - 0.45 * I)
            uv.x += (hash(vec2(band, k + 5.0)) - 0.5) * 0.15 * I * (0.3 + burst);
        float s = 0.012 * I * (0.3 + burst);
        vec4 c = texture(layerTex, uv);
        return vec4(texture(layerTex, uv + vec2(s, 0.0)).r, c.g, texture(layerTex, uv - vec2(s, 0.0)).b, c.a);
    }
    if (e == 4) { // separação RGB
        float s = 0.015 * I;
        vec4 c = texture(layerTex, uv);
        return vec4(texture(layerTex, uv + vec2(s, 0.0)).r, c.g, texture(layerTex, uv - vec2(s, 0.0)).b, c.a);
    }
    if (e == 5) { // VHS: ondulação horizontal e cor que escorre
        uv.x += sin(uv.y * 40.0 + t * 6.0) * 0.0015 * I + (hash(vec2(floor(uv.y * 240.0), floor(t * 30.0))) - 0.5) * 0.002 * I;
        float s = 0.004 * I;
        vec4 c = texture(layerTex, uv);
        return vec4(texture(layerTex, uv + vec2(s, 0.0)).r, c.g, texture(layerTex, uv - vec2(s * 1.5, 0.0)).b, c.a);
    }
    if (e == 6) { // brilho: realces borrados somados de volta
        vec4 c = texture(layerTex, uv);
        vec3 acc = vec3(0.0);
        float wsum = 0.0;
        vec2 step = vec2(0.012 * res.y / res.x, 0.012);
        for (int y = -2; y <= 2; ++y)
            for (int x = -2; x <= 2; ++x) {
                float w = exp(-float(x * x + y * y) / 4.0);
                vec3 s = texture(layerTex, uv + vec2(float(x), float(y)) * step).rgb;
                acc += max(s - 0.55, 0.0) / 0.45 * w;
                wsum += w;
            }
        return vec4(c.rgb + acc / wsum * 1.2 * I * c.a, c.a);
    }
    if (e == 8) { // pixelado
        float b = mix(1.0 / 400.0, 1.0 / 20.0, I);
        vec2 cell = vec2(b * res.y / res.x, b);
        return texture(layerTex, (floor(uv / cell) + 0.5) * cell);
    }
    if (e == 9) { // espelho: a metade esquerda se repete invertida (acima de 50%, também na vertical)
        if (uv.x > 0.5) uv.x = 1.0 - uv.x;
        if (I > 0.5 && uv.y > 0.5) uv.y = 1.0 - uv.y;
        return texture(layerTex, uv);
    }
    if (e == 11) { // zoom no ritmo (2 batidas por segundo)
        float pulse = pow(abs(sin(t * 3.14159265 * 2.0)), 6.0);
        return texture(layerTex, 0.5 + (uv - 0.5) / (1.0 + 0.15 * I * pulse));
    }
    return texture(layerTex, uv);
}

/** Efeitos sobre a cor final (depois da correção de cor). */
vec3 effectPost(vec3 rgb, vec2 uv)
{
    int e = int(ubuf.effect.x + 0.5);
    float I = ubuf.effect.y;
    float t = ubuf.effect.z;
    if (e == 0 || I <= 0.0)
        return rgb;
    vec2 res = vec2(textureSize(layerTex, 0));
    if (e == 5) { // VHS: linhas, chiado e cor gasta
        rgb *= 0.88 + 0.12 * sin(uv.y * res.y * 1.5);
        rgb += (hash(uv * res + vec2(t * 61.0, t * 37.0)) - 0.5) * 0.08 * I;
        rgb = mix(rgb, vec3(dot(rgb, LUMA)), 0.15 * I);
    } else if (e == 7) { // grão de filme, novo a cada quadro
        rgb += (hash(uv * res + vec2(t * 61.0, t * 37.0)) - 0.5) * 0.22 * I;
    } else if (e == 10) { // neve: três camadas de flocos caindo em velocidades diferentes
        float aspect = res.x / res.y;
        float snow = 0.0;
        for (int i = 0; i < 3; ++i) {
            float n = 10.0 + float(i) * 9.0;
            vec2 p = vec2(uv.x * aspect, uv.y - t * (0.12 + 0.06 * float(i))) * n;
            p.x += sin(t * 0.8 + p.y * 0.4 + float(i)) * 0.4;
            vec2 cell = floor(p);
            vec2 f = fract(p) - vec2(hash(cell), hash(cell + 7.3));
            float r = 0.06 + 0.05 * hash(cell + 3.1);
            snow += (1.0 - smoothstep(r * 0.5, r, length(f))) * step(0.35, hash(cell + 1.9));
        }
        rgb = mix(rgb, vec3(1.0), clamp(snow, 0.0, 1.0) * 0.9 * I);
    } else if (e == 12) { // estroboscópio: 4 clarões por segundo
        float f = 1.0 - smoothstep(0.0, 0.25, fract(t * 4.0));
        rgb = mix(rgb, vec3(1.0), f * 0.8 * I);
    } else if (e == 13) { // branco (transição de clarão)
        rgb = mix(rgb, vec3(1.0), I);
    }
    return clamp(rgb, 0.0, 1.0);
}

/** Máscara de forma (linear, espelho, elipse, retângulo arredondado) em coordenadas do quadro. */
float shapeMask(vec2 uv)
{
    int shape = int(ubuf.clipMask.x + 0.5);
    if (shape == 0)
        return 1.0;
    float aspect = ubuf.clipMaskExtra.y;
    vec2 p = uv - ubuf.clipMaskGeom.xy;
    p.x *= aspect; // espaço isotrópico: 1 unidade = altura do quadro
    float r = ubuf.clipMaskExtra.x;
    float c = cos(r), s = sin(r);
    p = vec2(c * p.x + s * p.y, -s * p.x + c * p.y);
    float f = max(ubuf.clipMask.z * 0.5, 0.0015);
    vec2 halfSize = max(vec2(ubuf.clipMaskGeom.z * aspect, ubuf.clipMaskGeom.w) * 0.5, vec2(1e-4));
    float d;
    if (shape == 1) {
        d = p.y; // linear: mantém o lado de cima da linha
    } else if (shape == 2) {
        d = abs(p.y) - halfSize.y; // espelho: faixa
    } else if (shape == 3) {
        d = (length(p / halfSize) - 1.0) * min(halfSize.x, halfSize.y); // elipse
    } else {
        float rr = ubuf.clipMask.w * min(halfSize.x, halfSize.y);
        vec2 q = abs(p) - halfSize + rr;
        d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - rr; // retângulo arredondado
    }
    float a = 1.0 - smoothstep(-f, f, d);
    return ubuf.clipMask.y > 0.5 ? 1.0 - a : a;
}

float blendChannel(int mode, float b, float s)
{
    if (mode == 1) return b * s;                                   // multiply
    if (mode == 2) return b + s - b * s;                           // screen
    if (mode == 3) return b < 0.5 ? 2.0 * b * s : 1.0 - 2.0 * (1.0 - b) * (1.0 - s); // overlay
    if (mode == 4) return min(b, s);                               // darken
    if (mode == 5) return max(b, s);                               // lighten
    if (mode == 6) return s >= 1.0 ? 1.0 : min(1.0, b / (1.0 - s)); // color dodge
    if (mode == 7) return s <= 0.0 ? 0.0 : 1.0 - min(1.0, (1.0 - b) / s); // color burn
    if (mode == 8) {                                               // soft light
        float d = b <= 0.25 ? ((16.0 * b - 12.0) * b + 4.0) * b : sqrt(b);
        return s <= 0.5 ? b - (1.0 - 2.0 * s) * b * (1.0 - b) : b + (2.0 * s - 1.0) * (d - b);
    }
    if (mode == 9) return abs(b - s);                              // difference
    return s;
}

void main()
{
    vec4 tex = effectSample(vUV);
    // Clipe composto: a fonte é outra composição (alfa pré-multiplicado).
    if (ubuf.effect.w > 0.5 && ubuf.misc.w < 0.5)
        tex.rgb = tex.a > 0.0 ? tex.rgb / tex.a : vec3(0.0);

    // Nitidez: máscara de nitidez com 4 vizinhos.
    if (ubuf.adjust3.y > 0.0) {
        vec2 px = 1.0 / vec2(textureSize(layerTex, 0));
        vec3 blur = (texture(layerTex, vUV + vec2(px.x, 0)).rgb + texture(layerTex, vUV - vec2(px.x, 0)).rgb +
                     texture(layerTex, vUV + vec2(0, px.y)).rgb + texture(layerTex, vUV - vec2(0, px.y)).rgb) * 0.25;
        tex.rgb = clamp(tex.rgb + (tex.rgb - blur) * ubuf.adjust3.y * 2.0, 0.0, 1.0);
    }

    // Camada de ajuste: a "textura" é o fundo já composto (pré-multiplicado).
    if (ubuf.misc.w > 0.5) {
        vec3 base = tex.a > 0.0 ? tex.rgb / tex.a : vec3(0.0);
        vec3 graded = effectPost(grade(base, vCanvas), vUV);
        float amount = clamp(ubuf.misc.x, 0.0, 1.0) * shapeMask(vCanvas);
        fragColor = vec4(mix(base, graded, amount) * tex.a, tex.a);
        return;
    }

    vec3 rgb = effectPost(grade(tex.rgb, vUV), vUV); // alfa reto
    float alpha = tex.a * ubuf.misc.x * shapeMask(vUV);

    // Chroma key no espaço de crominância.
    if (ubuf.chromaColor.a > 0.5) {
        float d = distance(rgb2yuv(tex.rgb).yz, rgb2yuv(ubuf.chromaColor.rgb).yz);
        float sim = ubuf.chromaParams.x * 0.5;
        float smoothness = max(ubuf.chromaParams.y * 0.5, 0.0001);
        alpha *= smoothstep(sim, sim + smoothness, d);
    }

    // Máscaras de transição.
    int maskKind = int(ubuf.mask.x + 0.5);
    float p = ubuf.mask.y;
    if (maskKind == 1) alpha *= step(vCanvas.x, p);          // entra pela esquerda
    else if (maskKind == 2) alpha *= step(1.0 - p, vCanvas.x); // entra pela direita
    else if (maskKind == 4) alpha *= step(1.0 - p, vCanvas.y); // sobe a partir de baixo
    else if (maskKind == 5) alpha *= step(vCanvas.y, p);       // desce a partir de cima
    else if (maskKind == 3) {
        vec2 d = (vCanvas - 0.5) * vec2(ubuf.mask.z, 1.0);
        float radius = p * 0.5 * sqrt(ubuf.mask.z * ubuf.mask.z + 1.0);
        alpha *= 1.0 - smoothstep(radius - 0.005, radius, length(d));
    }

    int mode = int(ubuf.misc.y + 0.5);
    if (mode == 0) {
        fragColor = vec4(rgb * alpha, alpha); // pré-multiplicado; mistura fixa na pipeline
        return;
    }
    // Modos avançados: lê o fundo já composto e substitui o pixel.
    vec2 buv = vec2(vCanvas.x, ubuf.misc.z > 0.5 ? 1.0 - vCanvas.y : vCanvas.y);
    vec4 back = texture(backdropTex, buv); // pré-multiplicado
    vec3 b = back.a > 0.0 ? back.rgb / back.a : vec3(0.0);
    vec3 blended = vec3(blendChannel(mode, b.r, rgb.r), blendChannel(mode, b.g, rgb.g), blendChannel(mode, b.b, rgb.b));
    vec3 mixed = mix(rgb, blended, back.a);
    float outA = alpha + back.a * (1.0 - alpha);
    fragColor = vec4(mixed * alpha + back.rgb * (1.0 - alpha), outA);
}
