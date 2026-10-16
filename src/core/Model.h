// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

/**
 * Modelo de projeto do clivcut.
 *
 * Todos os tempos são inteiros em microssegundos (µs), a mesma unidade dos
 * drafts JSON importados: a importação fica direta e dividir/aparar clipes não
 * acumula erro de ponto flutuante.
 */
namespace clv {

using Micros = std::int64_t;
constexpr Micros kSecond = 1'000'000;
constexpr int kProjectFormatVersion = 1;

enum class MediaKind { Video, Audio, Image };

struct MediaAsset {
    QString id;
    MediaKind kind = MediaKind::Video;
    QString path;
    QString name;
    Micros duration = 0; ///< 0 para imagens (duração livre)
    int width = 0;
    int height = 0;
    double fps = 0;
    bool hasAudio = false;
    bool hasVideo = false;
};

enum class Easing { Linear, EaseIn, EaseOut, EaseInOut, Hold, Bezier };

struct Keyframe {
    Micros t = 0; ///< relativo ao início do clipe na timeline
    double v = 0;
    Easing ease = Easing::Linear; ///< curva do keyframe atual até o próximo
    /// Pontos de controle da Bézier cúbica (como cubic-bezier do CSS), usados com Easing::Bezier.
    double bx1 = 0.33, by1 = 0, bx2 = 0.67, by2 = 1;
};

/** Propriedades numéricas animáveis por keyframe. */
enum class Prop { X, Y, Scale, Rotation, Opacity, Volume, Brightness, Contrast, Saturation, Count };
constexpr int kPropCount = static_cast<int>(Prop::Count);

using KeyframeTable = std::array<std::vector<Keyframe>, kPropCount>;

struct Transform {
    double x = 0;     ///< centro normalizado: 0 = centro do canvas, ±1 = borda
    double y = 0;     ///< +y para baixo
    double scale = 1; ///< 1 = mídia encaixada (fit) no canvas
    double rotation = 0; ///< graus, sentido horário
    double opacity = 1;
    bool flipH = false;
    bool flipV = false;
};

struct Crop {
    double left = 0, top = 0, right = 0, bottom = 0; ///< frações 0..1 removidas de cada borda
    bool isIdentity() const { return left == 0 && top == 0 && right == 0 && bottom == 0; }
};

/// Ponto de curva de tons (entrada x → saída y, ambos 0..1).
struct CurvePoint {
    double x = 0, y = 0;
    bool operator==(const CurvePoint &o) const { return x == o.x && y == o.y; }
};

/// Ajuste de uma faixa de cor do HSL (todos -1..1).
struct HslAdjust {
    double hue = 0, saturation = 0, luminance = 0;
    bool isZero() const { return hue == 0 && saturation == 0 && luminance == 0; }
    bool operator==(const HslAdjust &o) const = default;
};

/// Roda de cor: deslocamento de cor (x, y no disco unitário) e de luminância (-1..1).
struct ColorWheel {
    double x = 0, y = 0, luma = 0;
    bool isZero() const { return x == 0 && y == 0 && luma == 0; }
    bool operator==(const ColorWheel &o) const = default;
};

/// Faixas do HSL: vermelho, laranja, amarelo, verde, ciano, azul, roxo, magenta.
constexpr int kHslBands = 8;

struct ColorAdjust {
    double brightness = 0;  ///< -1..1
    double contrast = 0;    ///< -1..1
    double saturation = 0;  ///< -1..1
    double temperature = 0; ///< -1..1 (frio..quente)
    double exposure = 0;    ///< -1..1 (±2 stops)
    double tint = 0;        ///< -1..1 (verde..magenta)
    double highlights = 0;  ///< -1..1
    double shadows = 0;     ///< -1..1
    double vignette = 0;    ///< 0..1
    double sharpen = 0;     ///< 0..1
    QString filter;         ///< id de filtro embutido (core/Lut.h)
    double filterIntensity = 0.8;
    QString lutPath;        ///< LUT .cube do usuário
    double lutIntensity = 1;

    /// Curvas de tons: 0 = mestre (luminância), 1..3 = R, G, B. Vazia = identidade.
    std::array<std::vector<CurvePoint>, 4> curves;
    std::array<HslAdjust, kHslBands> hsl{};
    ColorWheel lift, gamma, gain; ///< sombras, meios-tons, realces

    bool hasFilter() const { return !filter.isEmpty() && filterIntensity > 0; }
    /// Curvas, HSL ou rodas fora do neutro (viram um LUT gerado; ver core/Lut.h).
    bool hasGrade() const;
    /// Chave estável do LUT gerado ("grade:{...}"), vazia se neutro.
    QString gradeKey() const;
    bool hasLut() const { return !lutPath.isEmpty() && lutIntensity > 0; }
};

enum class BlendMode { Normal, Multiply, Screen, Overlay, Darken, Lighten, ColorDodge, ColorBurn, SoftLight, Difference };

struct ChromaKey {
    bool enabled = false;
    QString color = QStringLiteral("#00ff00");
    double similarity = 0.3;
    double smoothness = 0.1;
};

/** Ponto da curva de velocidade: u = posição no clipe (0..1), v = velocidade (0,1×–10×). */
struct SpeedPoint {
    double u = 0;
    double v = 1;
    bool operator==(const SpeedPoint &) const = default;
};

enum class MaskShape { None, Linear, Mirror, Circle, Rectangle };

/**
 * Máscara de forma no quadro do clipe. Coordenadas em frações do quadro
 * (0..1, origem no topo esquerdo); tamanhos relativos à largura/altura.
 */
struct Mask {
    MaskShape shape = MaskShape::None;
    double x = 0.5, y = 0.5;          ///< centro
    double width = 0.5, height = 0.5; ///< círculo/retângulo; espelho usa a altura como espessura
    double rotation = 0;              ///< graus
    double feather = 0.05;            ///< suavização da borda (0..1)
    double roundness = 0;             ///< cantos do retângulo (0..1)
    bool invert = false;

    bool active() const { return shape != MaskShape::None; }
};

enum class TransitionKind {
    Dissolve, FadeBlack, WipeLeft, WipeRight, SlideLeft, SlideRight, CircleOpen, ZoomIn,
    SlideUp, SlideDown, WipeUp, WipeDown, Spin, ZoomOut, FlashWhite, Glitch, BlurDissolve
};

/// Efeito de vídeo do clipe (core/Effects.h); id vazio = nenhum.
struct ClipEffect {
    QString id;
    double intensity = 0.6; ///< 0..1
};

struct Transition {
    TransitionKind kind = TransitionKind::Dissolve;
    Micros duration = 500'000; ///< centrada no corte com o clipe anterior
};

enum class AnimationKind { None, Fade, Zoom, SlideUp, SlideDown, SlideLeft, SlideRight };

struct ClipAnimation {
    AnimationKind kind = AnimationKind::None;
    Micros duration = 500'000;
};

struct TextStyle {
    QString fontFamily = QStringLiteral("Sans Serif");
    double fontSize = 72; ///< em pixels de um canvas 1080p
    bool bold = true;
    bool italic = false;
    QString color = QStringLiteral("#ffffff");
    QString strokeColor = QStringLiteral("#000000");
    double strokeWidth = 4;
    QString background; ///< vazio = sem caixa de fundo
    int align = 1; ///< 0 esquerda, 1 centro, 2 direita
    bool shadow = true;
    /// Palavras entre *asteriscos* no texto saem nesta cor (destaque por palavra).
    QString highlightColor = QStringLiteral("#ffe14d");
    /// Curvatura do texto (-1..1): positivo arqueia para cima, 1 = meio círculo.
    double curve = 0;
    /// Forma atrás do texto: "", "rect", "rounded", "circle", "triangle", "star", "arrow", "heart", "bubble".
    QString shape;
    QString shapeColor = QStringLiteral("#f5a524");
    double shapeWidth = 480;  ///< em pixels de um canvas 1080p
    double shapeHeight = 270;
};

/// Adjust: clipe de filtro/ajuste que altera tudo o que está abaixo dele na composição.
enum class ClipType { Media, Text, Adjust };

struct Clip {
    QString id;
    QString groupId; ///< clipes com o mesmo id de grupo são selecionados/movidos juntos
    ClipType type = ClipType::Media;
    Micros start = 0;    ///< início na timeline
    Micros duration = 0; ///< duração na timeline
    Transform transform;
    KeyframeTable keyframes;
    ClipAnimation animIn;
    ClipAnimation animOut;

    // --- mídia ---
    QString assetId;
    Micros sourceIn = 0; ///< ponto de entrada na mídia
    double speed = 1;    ///< duração de origem = duration * speed (média, se houver curva); 0 = quadro congelado
    /// Curva de velocidade (vazia = constante). Linear por partes, u crescente de 0 a 1.
    std::vector<SpeedPoint> speedCurve;
    bool reverse = false;
    double volume = 1;   ///< 1 = 100%
    bool audioMuted = false; ///< silencia o áudio deste clipe (ex.: depois de separar o áudio)
    double gainDb = 0;       ///< ganho extra em dB (normalização de volume ou ajuste manual)
    double denoise = 0;      ///< redução de ruído 0..1 (0 = desligada)
    ClipEffect effect;         ///< efeito de vídeo (shader)
    QString voiceEffect;       ///< efeito de voz (engine/VoiceEffects.h), vazio = nenhum
    double ducking = 0;        ///< abaixa sob a voz (0..1): para música de fundo
    double voiceIsolation = 0; ///< isolamento de voz por IA 0..1 (0 = desligado)
    bool keepPitch = true;   ///< mudança de velocidade preserva o tom (WSOLA)
    bool removeBackground = false; ///< alfa vindo do matte gerado por IA (engine/Matting.h)
    bool audioOnly = false;  ///< usa só o áudio da mídia (clipe de áudio separado de um vídeo)
    Micros fadeIn = 0;
    Micros fadeOut = 0;
    Crop crop;
    ColorAdjust color;
    BlendMode blend = BlendMode::Normal;
    ChromaKey chroma;
    Mask mask;
    std::optional<Transition> transitionIn;

    // --- texto ---
    QString text;
    TextStyle style;

    Micros end() const { return start + duration; }
    Micros sourceDuration() const { return static_cast<Micros>(duration * speed); }
    Micros sourceOut() const { return sourceIn + sourceDuration(); }
    bool isText() const { return type == ClipType::Text; }
    bool isAdjust() const { return type == ClipType::Adjust; }
    bool hasMedia() const { return type == ClipType::Media; }
};

enum class TrackKind { Video, Audio, Text, Adjust };

struct Track {
    QString id;
    TrackKind kind = TrackKind::Video;
    QString name;
    bool muted = false;
    bool hidden = false;
    bool locked = false;
    bool captions = false; ///< faixa de legendas (texto gerado/importado)
    std::vector<Clip> clips; ///< sempre ordenados por start, sem sobreposição

    bool isVisual() const { return kind != TrackKind::Audio; }
};

struct Marker {
    QString id;
    Micros t = 0;
    QString label;
    QString color = QStringLiteral("#f5c242");
    bool beat = false; ///< criado pela detecção de batidas (substituído a cada nova detecção)
};

struct Project {
    QString id;
    QString name = QStringLiteral("New project");
    int width = 1920;
    int height = 1080;
    double fps = 30;
    QString background = QStringLiteral("#000000");
    /// Fundo desfocado: o clipe da faixa principal ampliado e borrado atrás de tudo (0 = só a cor).
    double backgroundBlur = 0;
    /// Faixa principal magnética: clipes sempre encostados, sem buracos.
    bool mainTrackMagnet = false;
    /// tracks[0] é a faixa principal de vídeo. Faixas visuais seguintes ficam por cima.
    std::vector<Track> tracks;
    std::map<QString, MediaAsset> assets;
    std::vector<Marker> markers;
    /// Pontos de entrada/saída (I/O) para exportar ou revisar um trecho; -1 = não definido.
    Micros inPoint = -1;
    Micros outPoint = -1;

    Micros duration() const;
    /// Trecho de entrada/saída válido (fim -1 = até o final do projeto).
    bool hasRange() const { return inPoint >= 0 || outPoint > 0; }
    Micros rangeStart() const { return inPoint >= 0 ? inPoint : 0; }
    Micros rangeEnd() const { return outPoint > 0 ? outPoint : duration(); }
    const MediaAsset *asset(const QString &id) const;
    Micros frameDuration() const { return static_cast<Micros>(kSecond / fps + 0.5); }
    Micros snapToFrame(Micros t) const;
};

QString newId(const char *prefix);

Project createProject(const QString &name = QStringLiteral("New project"), int width = 1920, int height = 1080,
                      double fps = 30);
Track createTrack(TrackKind kind, const QString &name = {});
Clip createMediaClip(const MediaAsset &asset, Micros start);
Clip createTextClip(const QString &text, Micros start, Micros duration = 3 * kSecond);
Clip createAdjustClip(Micros start, Micros duration = 3 * kSecond, const QString &filter = {});

constexpr Micros kImageDefaultDuration = 5 * kSecond;

} // namespace clv
