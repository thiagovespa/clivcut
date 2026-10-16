// SPDX-License-Identifier: GPL-3.0-or-later
#include "TextRaster.h"

#include <QFont>
#include <QFontMetricsF>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QTransform>

#include <algorithm>
#include <cmath>

namespace clv::render {

namespace {

struct Run {
    QString text;
    bool highlight = false;
};

/// Divide uma linha em trechos normais e destacados: *assim* destaca; \* é um asterisco literal.
std::vector<Run> parseRuns(const QString &line)
{
    std::vector<Run> runs(1);
    bool on = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar ch = line.at(i);
        if (ch == QLatin1Char('\\') && i + 1 < line.size() && line.at(i + 1) == QLatin1Char('*')) {
            runs.back().text += QLatin1Char('*');
            ++i;
        } else if (ch == QLatin1Char('*')) {
            on = !on;
            runs.push_back({{}, on});
        } else {
            runs.back().text += ch;
        }
    }
    runs.erase(std::remove_if(runs.begin(), runs.end(), [](const Run &r) { return r.text.isEmpty(); }), runs.end());
    return runs;
}

QPainterPath shapePath(const QString &kind, const QRectF &r)
{
    QPainterPath p;
    const double w = r.width(), h = r.height(), cx = r.center().x(), cy = r.center().y();
    if (kind == QLatin1String("rect")) {
        p.addRect(r);
    } else if (kind == QLatin1String("rounded")) {
        const double rad = std::min(w, h) * 0.18;
        p.addRoundedRect(r, rad, rad);
    } else if (kind == QLatin1String("circle")) {
        p.addEllipse(r);
    } else if (kind == QLatin1String("triangle")) {
        p.moveTo(cx, r.top());
        p.lineTo(r.right(), r.bottom());
        p.lineTo(r.left(), r.bottom());
        p.closeSubpath();
    } else if (kind == QLatin1String("star")) {
        for (int i = 0; i < 10; ++i) {
            const double a = -M_PI / 2 + i * M_PI / 5;
            const double k = i % 2 ? 0.42 : 1.0;
            const QPointF pt(cx + std::cos(a) * w / 2 * k, cy + std::sin(a) * h / 2 * k + h * 0.05);
            i ? p.lineTo(pt) : p.moveTo(pt);
        }
        p.closeSubpath();
    } else if (kind == QLatin1String("arrow")) {
        const double head = std::min(w * 0.4, h);
        p.moveTo(r.left(), cy - h * 0.2);
        p.lineTo(r.right() - head, cy - h * 0.2);
        p.lineTo(r.right() - head, r.top());
        p.lineTo(r.right(), cy);
        p.lineTo(r.right() - head, r.bottom());
        p.lineTo(r.right() - head, cy + h * 0.2);
        p.lineTo(r.left(), cy + h * 0.2);
        p.closeSubpath();
    } else if (kind == QLatin1String("heart")) {
        p.moveTo(cx, r.bottom());
        p.cubicTo(r.left() - w * 0.05, cy + h * 0.15, r.left() + w * 0.05, r.top() - h * 0.1, cx, r.top() + h * 0.28);
        p.cubicTo(r.right() - w * 0.05, r.top() - h * 0.1, r.right() + w * 0.05, cy + h * 0.15, cx, r.bottom());
    } else if (kind == QLatin1String("bubble")) {
        // Balão de fala: caixa arredondada com a ponta embaixo à esquerda.
        const QRectF box(r.left(), r.top(), w, h * 0.8);
        const double rad = std::min(w, box.height()) * 0.22;
        p.addRoundedRect(box, rad, rad);
        QPainterPath tail;
        tail.moveTo(r.left() + w * 0.18, box.bottom() - 2);
        tail.lineTo(r.left() + w * 0.12, r.bottom());
        tail.lineTo(r.left() + w * 0.36, box.bottom() - 2);
        tail.closeSubpath();
        p = p.united(tail);
    }
    return p;
}

} // namespace

QImage rasterizeText(const Clip &clip, double pxScale)
{
    const TextStyle &s = clip.style;
    QFont font(s.fontFamily);
    font.setPixelSize(std::max(1, static_cast<int>(std::lround(s.fontSize * pxScale))));
    font.setBold(s.bold);
    font.setItalic(s.italic);
    font.setStyleStrategy(QFont::PreferAntialias);
    const QFontMetricsF fm(font);

    // Só uma forma, sem texto: não desenha o espaço reservado.
    const bool shapeOnly = !s.shape.isEmpty() && clip.text.trimmed().isEmpty();
    const QStringList lines = clip.text.isEmpty() ? QStringList{QStringLiteral(" ")} : clip.text.split(QLatin1Char('\n'));
    std::vector<std::vector<Run>> runs;
    std::vector<double> widths;
    double textW = 1;
    for (const auto &l : lines) {
        runs.push_back(parseRuns(l));
        double w = 0;
        QString plain;
        for (const auto &r : runs.back())
            plain += r.text;
        w = fm.horizontalAdvance(plain);
        widths.push_back(w);
        textW = std::max(textW, w);
    }
    const double lineH = fm.lineSpacing();
    const double textH = lineH * lines.size();

    // Glifos em coordenadas com o bloco de texto centrado na origem; curvo, cada caractere
    // vai para um arco (e gira junto com ele).
    QPainterPath normal, highlight;
    const double curve = std::clamp(s.curve, -1.0, 1.0);
    const double sign = curve >= 0 ? 1.0 : -1.0;
    const double radius = std::abs(curve) > 0.01 ? textW / (std::abs(curve) * M_PI) : 0;
    // A primeira linha fica num círculo de raio `radius` cujo centro está abaixo (curva > 0, arco
    // para cima) ou acima (curva < 0); as outras linhas ficam em arcos concêntricos.
    const double firstBaseline = -textH / 2 + fm.ascent();
    for (size_t li = 0; li < runs.size() && !shapeOnly; ++li) {
        double x = -textW / 2;
        if (s.align == 1)
            x += (textW - widths[li]) / 2;
        else if (s.align == 2)
            x += textW - widths[li];
        const double baseline = firstBaseline + li * lineH;
        const double r = std::max(lineH, radius - sign * (baseline - firstBaseline));
        QString prefix;
        for (const auto &run : runs[li]) {
            QPainterPath &target = run.highlight ? highlight : normal;
            if (radius <= 0) {
                target.addText(QPointF(x + fm.horizontalAdvance(prefix), baseline), font, run.text);
                prefix += run.text;
                continue;
            }
            for (const QChar ch : run.text) {
                const double before = fm.horizontalAdvance(prefix);
                prefix += ch;
                const double adv = fm.horizontalAdvance(prefix) - before;
                const double theta = (x + before + adv / 2) / r; // centro do caractere, em ângulo
                QPainterPath glyph;
                glyph.addText(QPointF(-adv / 2, 0), font, QString(ch));
                QTransform t;
                t.translate(r * std::sin(theta), firstBaseline + sign * (radius - r * std::cos(theta)));
                t.rotateRadians(sign * theta);
                target.addPath(t.map(glyph));
            }
        }
    }
    QRectF bounds = normal.boundingRect().united(highlight.boundingRect());
    if (bounds.isEmpty() && !shapeOnly)
        bounds = QRectF(-textW / 2, -textH / 2, textW, textH);
    const QRectF textBounds = bounds;

    const double stroke = shapeOnly ? 0.0 : s.strokeWidth * pxScale;
    const double shadowOff = s.shadow ? std::max(1.0, font.pixelSize() * 0.05) : 0.0;
    const double bgPad = s.background.isEmpty() || shapeOnly ? 0.0 : font.pixelSize() * 0.3;

    QPainterPath shape;
    if (!s.shape.isEmpty()) {
        const QSizeF size(s.shapeWidth * pxScale, s.shapeHeight * pxScale);
        shape = shapePath(s.shape, QRectF(QPointF(-size.width() / 2, -size.height() / 2), size));
        bounds = bounds.isEmpty() ? shape.boundingRect() : bounds.united(shape.boundingRect());
    }
    if (bgPad > 0)
        bounds = bounds.united(textBounds.adjusted(-bgPad, -bgPad, bgPad, bgPad));

    const double pad = std::ceil(stroke + shadowOff + 2);
    const QSize size(static_cast<int>(std::ceil(bounds.width() + 2 * pad)), static_cast<int>(std::ceil(bounds.height() + 2 * pad)));
    QImage img(size.expandedTo(QSize(2, 2)), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    p.translate(pad - bounds.left(), pad - bounds.top());

    if (!shape.isEmpty()) {
        if (s.shadow) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 110));
            p.drawPath(shape.translated(shadowOff, shadowOff));
        }
        p.fillPath(shape, QColor(s.shapeColor));
    }
    if (!s.background.isEmpty()) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(s.background));
        const double r = font.pixelSize() * 0.2;
        p.drawRoundedRect(textBounds.adjusted(-bgPad, -bgPad, bgPad, bgPad), r, r);
    }
    if (s.shadow) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 140));
        p.drawPath(normal.translated(shadowOff, shadowOff));
        p.drawPath(highlight.translated(shadowOff, shadowOff));
    }
    if (stroke > 0.01) {
        QPen pen(QColor(s.strokeColor), stroke * 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.strokePath(normal, pen);
        p.strokePath(highlight, pen);
    }
    p.fillPath(normal, QColor(s.color));
    p.fillPath(highlight, QColor(s.highlightColor));
    p.end();
    return img.convertToFormat(QImage::Format_RGBA8888);
}

quint64 textCacheKey(const Clip &clip, double pxScale)
{
    const TextStyle &s = clip.style;
    size_t h = qHashMulti(0, clip.text, s.fontFamily, s.fontSize, s.bold, s.italic, s.color, s.strokeColor);
    h = qHashMulti(h, s.strokeWidth, s.background, s.align, s.shadow, std::lround(pxScale * 10000));
    h = qHashMulti(h, s.highlightColor, s.curve, s.shape, s.shapeColor, s.shapeWidth, s.shapeHeight);
    return h;
}

} // namespace clv::render
