/*
 * MoonlightWeb — native capture & encoding engine: GPU load tool.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "RenderWindow.h"

#include <QFile>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QPlatformSurfaceEvent>
#include <QVector3D>

#include <algorithm>
#include <cmath>
#include <vector>

#if defined(Q_OS_WIN)
#include <rhi/qrhi_platform.h>
#elif !defined(Q_OS_MACOS)
#include <QVulkanInstance>
#include <rhi/qrhi_platform.h>
#endif

namespace {

// The knot: a (2,3) torus knot swept by a tube. Dense enough that the vertex
// stage is a real cost once the shells multiply it.
constexpr int kSegments = 1536;
constexpr int kSides = 48;
constexpr float kTubeRadius = 0.28f;

// std140 block shared by the four shaders (see shaders/*.vert/frag).
struct Uniforms
{
    float viewProj[16];
    float invViewProj[16];
    float camTime[4];  // eye xyz, time in seconds
    float params[4];   // raymarch steps, noise octaves, shells, level
    float viewport[4]; // width, height, kick pulse, unused
    float model[16];   // the orientation the client gives the knot
};
static_assert(sizeof(Uniforms) == 240, "std140 layout of the uniform block");

// How the knot answers the client. The mouse turns it at once, so a stream's
// input delay shows as the knot lagging the pointer; the arrow keys push its
// spin, so a key that was taken leaves the knot turning.
constexpr double kRadiansPerPixel = 0.006;
constexpr double kArrowAcceleration = 3.0; // rad/s², while a key is held
constexpr double kSpinDamping = 0.3;       // 1/s: a spin halves in about 2.3 s
constexpr double kMaxSpin = 12.0;          // rad/s
constexpr double kThrowWindowNs = 80e6;    // a drag released later is a drop

double degrees(double radians)
{
    return radians * 57.29577951308232;
}

// What the level buys: the glow's steps per pixel grow first (a game's
// post-processing), the shells grow with them (its geometry). Both are close to
// linear in cost, which is what LoadController's correction assumes.
// Below level 1 the glow keeps getting cheaper instead of stopping at its
// eight steps of four octaves: first fewer steps, down to one, then fewer
// octaves, down to one — so the cost keeps going about linearly with the
// level. The N95's iGPU managed 35 fps at level 1 against a 45 fps target:
// the floor overloaded it instead of calibrating (22/09/2026).
int stepsFor(double level)
{
    if (level < 1.0) return std::max(1, int(std::lround(8.0 * level)));
    return int(std::min(8.0 + level * 0.5, 1024.0));
}
int octavesFor(double level)
{
    constexpr double kOneStep = 1.0 / 8.0; // the level where stepsFor reaches 1
    if (level >= kOneStep) return 4;
    return std::max(1, int(std::lround(4.0 * level / kOneStep)));
}
int shellsFor(double level)
{
    return int(std::min(1.0 + level / 20.0, 20000.0));
}

QVector3D knotPoint(float t)
{
    const float r = 2.0f + std::cos(3.0f * t);
    return QVector3D(r * std::cos(2.0f * t), std::sin(3.0f * t), r * std::sin(2.0f * t)) * 0.55f;
}

void buildKnot(std::vector<float>& vertices, std::vector<quint32>& indices)
{
    const float twoPi = 6.28318530718f;
    vertices.clear();
    indices.clear();
    vertices.reserve(size_t(kSegments + 1) * (kSides + 1) * 8);
    // Parallel-transport frames keep the tube from twisting along the knot.
    QVector3D normal(0.0f, 1.0f, 0.0f);
    for (int i = 0; i <= kSegments; ++i) {
        const float t = twoPi * float(i) / kSegments;
        const QVector3D center = knotPoint(t);
        const QVector3D tangent = (knotPoint(t + 1e-3f) - knotPoint(t - 1e-3f)).normalized();
        normal = (normal - QVector3D::dotProduct(normal, tangent) * tangent).normalized();
        const QVector3D binormal = QVector3D::crossProduct(tangent, normal);
        for (int j = 0; j <= kSides; ++j) {
            const float a = twoPi * float(j) / kSides;
            const QVector3D n = normal * std::cos(a) + binormal * std::sin(a);
            const QVector3D p = center + n * kTubeRadius;
            vertices.insert(vertices.end(), {p.x(), p.y(), p.z(), n.x(), n.y(), n.z(),
                                             float(i) / kSegments, float(j) / kSides});
        }
    }
    for (int i = 0; i < kSegments; ++i)
        for (int j = 0; j < kSides; ++j) {
            const quint32 a = quint32(i * (kSides + 1) + j);
            const quint32 b = a + kSides + 1;
            indices.insert(indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
}

QShader loadShader(const QString& name)
{
    QFile f(name);
    return f.open(QIODevice::ReadOnly) ? QShader::fromSerialized(f.readAll()) : QShader();
}

} // namespace

RenderWindow::RenderWindow(const GpuEntry& gpu, QVulkanInstance* vk)
    : m_gpu(gpu)
    , m_vk(vk)
{
#if defined(Q_OS_WIN)
    setSurfaceType(QSurface::Direct3DSurface);
#elif defined(Q_OS_MACOS)
    setSurfaceType(QSurface::MetalSurface);
#else
    setSurfaceType(QSurface::VulkanSurface);
    setVulkanInstance(vk);
#endif
    // A zero-interval timer renders whenever the event loop is idle: no vsync,
    // no pacing — the GPU decides the frame rate, as it does under a game.
    m_loop.setInterval(0);
    connect(&m_loop, &QTimer::timeout, this, &RenderWindow::renderFrame);
}

RenderWindow::~RenderWindow()
{
    shutdown();
}

void RenderWindow::shutdown()
{
    m_stopped = true;
    m_loop.stop();
    releaseResources();
}

void RenderWindow::exposeEvent(QExposeEvent*)
{
    if (!isExposed() || m_initialized || m_stopped) return;
    m_initialized = true;
    if (!init()) {
        const QString reason =
            m_deviceText.isEmpty() ? QStringLiteral("could not create the device") : m_deviceText;
        shutdown();
        emit failed(reason, false);
        return;
    }
    m_clock.start();
    m_loop.start();
    emit started(m_deviceText);
}

bool RenderWindow::event(QEvent* e)
{
    if (e->type() == QEvent::PlatformSurface &&
        static_cast<QPlatformSurfaceEvent*>(e)->surfaceEventType() ==
            QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
        shutdown();
    // The pointer comes back elsewhere: that jump is not a move of the hand.
    if (e->type() == QEvent::Leave) m_haveMouse = false;
    return QWindow::event(e);
}

bool RenderWindow::init()
{
    const QRhi::Flags flags = QRhi::EnableTimestamps;
#if defined(Q_OS_WIN)
    // D3D12 by default, the API of the games this stands for; MW_GPU_LOAD_API=d3d11
    // is there to compare. MW_GPU_LOAD_DEBUG turns on the debug layer and clears
    // to magenta, which tells "nothing drawn" from "drawn black".
    if (qEnvironmentVariable("MW_GPU_LOAD_API") == QLatin1String("d3d11")) {
        QRhiD3D11InitParams params;
        params.enableDebugLayer = qEnvironmentVariableIsSet("MW_GPU_LOAD_DEBUG");
        QRhiD3D11NativeHandles adapter;
        adapter.adapterLuidLow = m_gpu.luidLow;
        adapter.adapterLuidHigh = m_gpu.luidHigh;
        m_rhi.reset(QRhi::create(QRhi::D3D11, &params, flags, &adapter));
    } else {
        QRhiD3D12InitParams params;
        params.enableDebugLayer = qEnvironmentVariableIsSet("MW_GPU_LOAD_DEBUG");
        QRhiD3D12NativeHandles adapter;
        adapter.adapterLuidLow = m_gpu.luidLow;
        adapter.adapterLuidHigh = m_gpu.luidHigh;
        m_rhi.reset(QRhi::create(QRhi::D3D12, &params, flags, &adapter));
    }
#elif defined(Q_OS_MACOS)
    QRhiMetalInitParams params;
    m_rhi.reset(QRhi::create(QRhi::Metal, &params, flags));
#else
    QRhiVulkanInitParams params;
    params.inst = m_vk;
    params.window = this;
    QRhiVulkanNativeHandles adapter;
    adapter.physDev = static_cast<VkPhysicalDevice>(m_gpu.vkPhysDev);
    m_rhi.reset(QRhi::create(QRhi::Vulkan, &params, flags, &adapter));
#endif
    if (!m_rhi) return false;

    // The whole point is the SAME GPU as the encoder: refuse to run on any
    // other, rather than load the wrong one and report numbers that look fine.
    const QRhiDriverInfo info = m_rhi->driverInfo();
    const QString opened = QString::fromUtf8(info.deviceName);
    if (m_gpu.deviceId && info.deviceId && info.deviceId != m_gpu.deviceId) {
        m_deviceText = QString("opened %1 instead of %2").arg(opened, m_gpu.name);
        return false;
    }
    m_deviceText = QString("%1 on %2").arg(QString::fromUtf8(m_rhi->backendName()), opened);

    m_sc.reset(m_rhi->newSwapChain());
    m_ds.reset(m_rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, QSize(), 1,
                                      QRhiRenderBuffer::UsedWithSwapChainOnly));
    m_sc->setWindow(this);
    m_sc->setDepthStencil(m_ds.get());
    m_sc->setFlags(QRhiSwapChain::NoVSync);
    m_rp.reset(m_sc->newCompatibleRenderPassDescriptor());
    m_sc->setRenderPassDescriptor(m_rp.get());
    if (!m_sc->createOrResize()) return false;

    std::vector<float> vertices;
    std::vector<quint32> indices;
    buildKnot(vertices, indices);
    m_indexCount = quint32(indices.size());
    m_vbuf.reset(m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer,
                                  quint32(vertices.size() * sizeof(float))));
    m_ibuf.reset(m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::IndexBuffer,
                                  quint32(indices.size() * sizeof(quint32))));
    m_ubuf.reset(
        m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(Uniforms)));
    if (!m_vbuf->create() || !m_ibuf->create() || !m_ubuf->create()) return false;
    m_initialUpdates = m_rhi->nextResourceUpdateBatch();
    m_initialUpdates->uploadStaticBuffer(m_vbuf.get(), vertices.data());
    m_initialUpdates->uploadStaticBuffer(m_ibuf.get(), indices.data());

    m_srb.reset(m_rhi->newShaderResourceBindings());
    m_srb->setBindings({QRhiShaderResourceBinding::uniformBuffer(
        0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
        m_ubuf.get())});
    if (!m_srb->create()) return false;

    m_skyPipe.reset(m_rhi->newGraphicsPipeline());
    m_skyPipe->setShaderStages({{QRhiShaderStage::Vertex, loadShader(":/shaders/sky.vert.qsb")},
                                {QRhiShaderStage::Fragment, loadShader(":/shaders/sky.frag.qsb")}});
    m_skyPipe->setVertexInputLayout({}); // a full-screen triangle from the vertex index
    // An explicit scissor on both pipelines. Without it, Qt's D3D12 backend left
    // the rectangle to the driver: AMD drew, Intel Arc and NVIDIA clipped every
    // pixel away while still clearing the target — a black window that looked
    // rendered, and a "load" that cost the GPU nothing (DualRTX, 22/09).
    m_skyPipe->setFlags(QRhiGraphicsPipeline::UsesScissor);
    m_skyPipe->setShaderResourceBindings(m_srb.get());
    m_skyPipe->setRenderPassDescriptor(m_rp.get());
    if (!m_skyPipe->create()) return false;

    m_knotPipe.reset(m_rhi->newGraphicsPipeline());
    m_knotPipe->setShaderStages(
        {{QRhiShaderStage::Vertex, loadShader(":/shaders/knot.vert.qsb")},
         {QRhiShaderStage::Fragment, loadShader(":/shaders/knot.frag.qsb")}});
    QRhiVertexInputLayout layout;
    layout.setBindings({{8 * sizeof(float)}});
    layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float3, 0},
                          {0, 1, QRhiVertexInputAttribute::Float3, 3 * sizeof(float)},
                          {0, 2, QRhiVertexInputAttribute::Float2, 6 * sizeof(float)}});
    m_knotPipe->setVertexInputLayout(layout);
    m_knotPipe->setDepthTest(true);
    m_knotPipe->setDepthWrite(true);
    m_knotPipe->setCullMode(QRhiGraphicsPipeline::None); // the shells are open cages
    m_knotPipe->setFlags(QRhiGraphicsPipeline::UsesScissor);
    m_knotPipe->setShaderResourceBindings(m_srb.get());
    m_knotPipe->setRenderPassDescriptor(m_rp.get());
    return m_knotPipe->create();
}

void RenderWindow::releaseResources()
{
    if (m_initialUpdates) {
        m_initialUpdates->release();
        m_initialUpdates = nullptr;
    }
    m_knotPipe.reset();
    m_skyPipe.reset();
    m_srb.reset();
    m_ubuf.reset();
    m_ibuf.reset();
    m_vbuf.reset();
    m_rp.reset();
    m_ds.reset();
    m_sc.reset();
    m_rhi.reset();
}

void RenderWindow::renderFrame()
{
    if (m_stopped || !m_rhi) return;
    if (m_sc->currentPixelSize() != m_sc->surfacePixelSize() ||
        m_sc->currentPixelSize().isEmpty()) {
        if (m_sc->surfacePixelSize().isEmpty() || !m_sc->createOrResize()) return;
    }

    const QRhi::FrameOpResult begun = m_rhi->beginFrame(m_sc.get());
    if (begun == QRhi::FrameOpSwapChainOutOfDate) {
        m_sc->createOrResize();
        return;
    }
    if (begun == QRhi::FrameOpDeviceLost) {
        shutdown();
        emit failed(QStringLiteral("device lost"), true);
        return;
    }
    if (begun != QRhi::FrameOpSuccess) return;

    QRhiCommandBuffer* cb = m_sc->currentFrameCommandBuffer();
    QRhiResourceUpdateBatch* updates = m_rhi->nextResourceUpdateBatch();
    if (m_initialUpdates) {
        updates->merge(m_initialUpdates);
        m_initialUpdates->release();
        m_initialUpdates = nullptr;
    }

    const QSize px = m_sc->currentPixelSize();
    const float time = float(m_clock.nsecsElapsed() / 1e9);
    integrate(std::clamp(double(time) - m_lastFrameSeconds, 0.0, 0.1));
    m_lastFrameSeconds = time;
    const QVector3D eye(0.0f, 0.9f, 6.5f);
    QMatrix4x4 proj = m_rhi->clipSpaceCorrMatrix();
    proj.perspective(50.0f, float(px.width()) / float(std::max(1, px.height())), 0.1f, 100.0f);
    QMatrix4x4 view;
    view.lookAt(eye, QVector3D(0.0f, 0.0f, 0.0f), QVector3D(0.0f, 1.0f, 0.0f));
    const QMatrix4x4 viewProj = proj * view;
    const int shells = shellsFor(m_level);

    Uniforms u;
    std::copy(viewProj.constData(), viewProj.constData() + 16, u.viewProj);
    const QMatrix4x4 inv = viewProj.inverted();
    std::copy(inv.constData(), inv.constData() + 16, u.invViewProj);
    const float camTime[4] = {eye.x(), eye.y(), eye.z(), time};
    const float params[4] = {float(stepsFor(m_level)), float(octavesFor(m_level)), float(shells),
                             float(m_level)};
    const float viewport[4] = {float(px.width()), float(px.height()), m_pulse ? m_pulse() : 0.0f,
                               0.0f};
    std::copy(camTime, camTime + 4, u.camTime);
    std::copy(params, params + 4, u.params);
    std::copy(viewport, viewport + 4, u.viewport);
    QMatrix4x4 model;
    model.rotate(m_orientation);
    std::copy(model.constData(), model.constData() + 16, u.model);
    updates->updateDynamicBuffer(m_ubuf.get(), 0, sizeof(Uniforms), &u);

    cb->beginPass(m_sc->currentFrameRenderTarget(),
                  QColor(qEnvironmentVariableIsSet("MW_GPU_LOAD_DEBUG") ? QColor(255, 0, 255)
                                                                        : QColor(7, 9, 11)),
                  {1.0f, 0}, updates);
    cb->setViewport(QRhiViewport(0, 0, float(px.width()), float(px.height())));
    cb->setGraphicsPipeline(m_skyPipe.get());
    cb->setScissor(QRhiScissor(0, 0, px.width(), px.height()));
    cb->setShaderResources();
    cb->draw(3);
    cb->setGraphicsPipeline(m_knotPipe.get());
    cb->setScissor(QRhiScissor(0, 0, px.width(), px.height()));
    cb->setShaderResources();
    const QRhiCommandBuffer::VertexInput input(m_vbuf.get(), 0);
    cb->setVertexInput(0, 1, &input, m_ibuf.get(), 0, QRhiCommandBuffer::IndexUInt32);
    cb->drawIndexed(m_indexCount, quint32(shells));
    cb->endPass();

    const double gpuSeconds = cb->lastCompletedGpuTime();
    if (m_rhi->endFrame(m_sc.get()) == QRhi::FrameOpDeviceLost) {
        shutdown();
        emit failed(QStringLiteral("device lost"), true);
        return;
    }
    emit frameDone(gpuSeconds * 1000.0);
}

void RenderWindow::turn(double yaw, double pitch)
{
    m_orientation =
        (QQuaternion::fromAxisAndAngle(0.0f, 1.0f, 0.0f, float(degrees(yaw))) *
         QQuaternion::fromAxisAndAngle(1.0f, 0.0f, 0.0f, float(degrees(pitch))) * m_orientation)
            .normalized();
}

void RenderWindow::integrate(double dt)
{
    // Right turns the front of the knot to the right (+yaw), up lifts it
    // (-pitch), the same way the mouse does.
    const double push = kArrowAcceleration * dt;
    if (m_arrows[0]) m_spin.setY(float(m_spin.y() - push));
    if (m_arrows[1]) m_spin.setY(float(m_spin.y() + push));
    if (m_arrows[2]) m_spin.setX(float(m_spin.x() - push));
    if (m_arrows[3]) m_spin.setX(float(m_spin.x() + push));
    m_spin *= float(std::exp(-kSpinDamping * dt));
    if (m_spin.length() > kMaxSpin) m_spin = m_spin.normalized() * float(kMaxSpin);
    const double angle = m_spin.length() * dt;
    if (angle > 0.0)
        m_orientation = (QQuaternion::fromAxisAndAngle(m_spin.normalized(), float(degrees(angle))) *
                         m_orientation)
                            .normalized();
}

RenderWindow::InputStats RenderWindow::inputStats() const
{
    InputStats s = m_input;
    s.spin = m_spin.length();
    const QVector3D up = m_orientation.rotatedVector(QVector3D(0.0f, 1.0f, 0.0f));
    s.tiltDegrees = degrees(std::acos(std::clamp(double(up.y()), -1.0, 1.0)));
    return s;
}

void RenderWindow::keyPressEvent(QKeyEvent* e)
{
    int arrow = -1;
    switch (e->key()) {
    case Qt::Key_Left: arrow = 0; break;
    case Qt::Key_Right: arrow = 1; break;
    case Qt::Key_Up: arrow = 2; break;
    case Qt::Key_Down: arrow = 3; break;
    case Qt::Key_Space:
        m_orientation = QQuaternion();
        m_spin = QVector3D();
        break;
    default: break;
    }
    if (e->isAutoRepeat()) return; // a held key is one press, not thirty
    ++m_input.keyPresses;
    m_input.lastKey = QKeySequence(e->key()).toString();
    if (arrow >= 0) {
        m_arrows[arrow] = true;
        emit arrowPressed(arrow == 1 || arrow == 2 ? 1 : -1);
    }
}

void RenderWindow::keyReleaseEvent(QKeyEvent* e)
{
    if (e->isAutoRepeat()) return;
    switch (e->key()) {
    case Qt::Key_Left: m_arrows[0] = false; break;
    case Qt::Key_Right: m_arrows[1] = false; break;
    case Qt::Key_Up: m_arrows[2] = false; break;
    case Qt::Key_Down: m_arrows[3] = false; break;
    default: break;
    }
}

void RenderWindow::mouseMoveEvent(QMouseEvent* e)
{
    ++m_input.mouseMoves;
    const QPointF pos = e->position();
    const qint64 now = m_clock.nsecsElapsed();
    if (m_haveMouse) {
        const QPointF d = pos - m_lastMouse;
        turn(d.x() * kRadiansPerPixel, d.y() * kRadiansPerPixel);
        const double dt = (now - m_lastMoveNs) / 1e9;
        if (m_dragging && dt > 0.0) {
            const QVector3D v(float(d.y() * kRadiansPerPixel / dt),
                              float(d.x() * kRadiansPerPixel / dt), 0.0f);
            m_dragVelocity = m_dragVelocity * 0.5f + v * 0.5f;
        }
    }
    m_lastMouse = pos;
    m_lastMoveNs = now;
    m_haveMouse = true;
}

void RenderWindow::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) return;
    requestActivate(); // the arrow keys come to this window from now on
    m_dragging = true;
    m_dragVelocity = QVector3D();
}

void RenderWindow::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton || !m_dragging) return;
    m_dragging = false;
    // A drag released while still moving throws the knot.
    if (m_clock.nsecsElapsed() - m_lastMoveNs < kThrowWindowNs) m_spin += m_dragVelocity;
}
