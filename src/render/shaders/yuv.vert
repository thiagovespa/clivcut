#version 440

layout(location = 0) out vec2 vUV;

layout(std140, binding = 0) uniform buf {
    vec4 flip;   // x = inverter v
    vec4 params; // x = NV12, y = quartos de volta (horário), z = faixa completa, w = BT.709
} ubuf;

void main()
{
    // Triângulo que cobre o alvo inteiro; vUV = (0,0) no canto superior esquerdo da imagem.
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vUV = pos;
    if (ubuf.flip.x > 0.5)
        vUV.y = 1.0 - vUV.y;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
