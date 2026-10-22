// SPDX-License-Identifier: GPL-3.0-or-later
#include "OffscreenRhi.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QSurfaceFormat>
#ifdef CLIVCUT_HAS_VULKAN
#include <QVulkanInstance>
#endif

namespace clv::render {

OffscreenRhi::OffscreenRhi() = default;

OffscreenRhi::~OffscreenRhi()
{
    m_rhi.reset(); // antes da instância/superfície
}

QSurfaceFormat OffscreenRhi::glFormat()
{
    // GLSL 330 / ES 3.0 no mínimo (texturas 3D dos LUTs).
    QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
    if (fmt.renderableType() != QSurfaceFormat::OpenGLES && fmt.version() < qMakePair(3, 3)) {
        fmt.setVersion(3, 3);
        fmt.setProfile(QSurfaceFormat::CoreProfile);
    }
    return fmt;
}

QOffscreenSurface *OffscreenRhi::createFallbackSurface()
{
#if QT_CONFIG(opengl)
    return QRhiGles2InitParams::newFallbackSurface(glFormat());
#else
    return nullptr;
#endif
}

bool OffscreenRhi::create(QString *error, QOffscreenSurface *fallbackSurface)
{
    const QByteArray forced = qgetenv("CLIVCUT_RHI").toLower();

    // Backend nativo primeiro: D3D11 no Windows, Metal no macOS.
#if defined(Q_OS_WIN)
    if (forced.isEmpty() || forced == "d3d11") {
        QRhiD3D11InitParams params;
        m_rhi.reset(QRhi::create(QRhi::D3D11, &params));
        if (m_rhi)
            return true;
    }
#elif defined(Q_OS_MACOS)
    if (forced.isEmpty() || forced == "metal") {
        QRhiMetalInitParams params;
        m_rhi.reset(QRhi::create(QRhi::Metal, &params));
        if (m_rhi)
            return true;
    }
#endif

#ifdef CLIVCUT_HAS_VULKAN
    if (forced.isEmpty() || forced == "vulkan") {
        m_vulkan = std::make_unique<QVulkanInstance>();
        m_vulkan->setExtensions(QRhiVulkanInitParams::preferredInstanceExtensions());
        if (m_vulkan->create()) {
            QRhiVulkanInitParams params;
            params.inst = m_vulkan.get();
            m_rhi.reset(QRhi::create(QRhi::Vulkan, &params));
            if (m_rhi)
                return true;
        }
        m_vulkan.reset();
    }
#endif

#if QT_CONFIG(opengl)
    if (forced.isEmpty() || forced == "opengl") {
        QRhiGles2InitParams params;
        params.format = glFormat();
        if (fallbackSurface) {
            params.fallbackSurface = fallbackSurface;
        } else {
            m_surface.reset(QRhiGles2InitParams::newFallbackSurface(params.format));
            params.fallbackSurface = m_surface.get();
        }
        m_rhi.reset(QRhi::create(QRhi::OpenGLES2, &params));
        if (m_rhi)
            return true;
    }
#endif

    if (error) {
        // Sem tela (plugin offscreen) o Qt não oferece Vulkan nem OpenGL.
        *error = QGuiApplication::platformName() == QLatin1String("offscreen")
                     ? QCoreApplication::translate("Render", "No graphics backend without a display: run it in a graphical "
                                                             "session or under xvfb-run")
                     : QCoreApplication::translate("Render", "No graphics backend available (Vulkan/OpenGL)");
    }
    return false;
}

QString OffscreenRhi::backendName() const
{
    return m_rhi ? QString::fromLatin1(m_rhi->backendName()) : QString();
}

} // namespace clv::render
