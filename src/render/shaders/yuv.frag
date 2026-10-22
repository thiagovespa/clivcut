#version 440

// YUV 4:2:0 de 8 bits (I420 ou NV12) → RGBA, com a rotação do vídeo.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    vec4 flip;
    vec4 params; // x = NV12, y = quartos de volta (horário), z = faixa completa, w = BT.709
} ubuf;

layout(binding = 1) uniform sampler2D yTex;
layout(binding = 2) uniform sampler2D uTex; // NV12: UV intercalado (RG)
layout(binding = 3) uniform sampler2D vTex;

void main()
{
    // Ponto da imagem original que cai neste pixel da imagem girada.
    vec2 uv = vUV;
    int turns = int(ubuf.params.y + 0.5);
    if (turns == 1)
        uv = vec2(vUV.y, 1.0 - vUV.x);
    else if (turns == 2)
        uv = vec2(1.0 - vUV.x, 1.0 - vUV.y);
    else if (turns == 3)
        uv = vec2(1.0 - vUV.y, vUV.x);

    float y = texture(yTex, uv).r;
    float cb, cr;
    if (ubuf.params.x > 0.5) {
        vec2 c = texture(uTex, uv).rg;
        cb = c.x;
        cr = c.y;
    } else {
        cb = texture(uTex, uv).r;
        cr = texture(vTex, uv).r;
    }
    if (ubuf.params.z > 0.5) {
        cb -= 0.5;
        cr -= 0.5;
    } else {
        y = (y - 16.0 / 255.0) * (255.0 / 219.0);
        cb = (cb - 128.0 / 255.0) * (255.0 / 224.0);
        cr = (cr - 128.0 / 255.0) * (255.0 / 224.0);
    }
    vec3 rgb = ubuf.params.w > 0.5 ? vec3(y + 1.5748 * cr, y - 0.187324 * cb - 0.468124 * cr, y + 1.8556 * cb)
                                   : vec3(y + 1.402 * cr, y - 0.344136 * cb - 0.714136 * cr, y + 1.772 * cb);
    fragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
