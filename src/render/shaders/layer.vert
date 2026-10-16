#version 440

layout(location = 0) in vec2 position; // pixels do canvas (origem no topo esquerdo)
layout(location = 1) in vec2 texcoord;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec2 vCanvas; // 0..1 no canvas, y para baixo

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    vec4 canvasSize;   // xy = tamanho do canvas em px
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
    vec4 grade;
    vec4 effect;
} ubuf;

void main()
{
    vUV = texcoord;
    vCanvas = position / ubuf.canvasSize.xy;
    gl_Position = ubuf.mvp * vec4(position, 0.0, 1.0);
}
