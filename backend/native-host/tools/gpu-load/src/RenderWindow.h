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

#pragma once

#include "GpuList.h"

#include <QElapsedTimer>
#include <QQuaternion>
#include <QTimer>
#include <QVector3D>
#include <QWindow>

#include <rhi/qrhi.h>

#include <functional>
#include <memory>

/// The "Neon Core": a torus knot of hexagonal panels, cyan and magenta edges,
/// wrapped in shells of the same knot and floating in a volumetric glow above a
/// synthwave grid — rendered on ONE chosen GPU, as fast as that GPU can go.
///
/// The swap chain does not wait for vsync and the frames in flight stay queued,
/// so the GPU's queue is always full of this window's work: the situation a
/// game that takes the whole GPU creates for the encoder.
///
/// One knob, the level, sets the cost: the glow's raymarch steps per pixel
/// (pixel load) and the number of knot shells (geometry load) both grow with it.
///
/// The client drives the knot, which is how a stream's input is checked by
/// eye: the mouse turns it as it moves (a drag throws it), the arrow keys
/// accelerate its spin, which keeps going once they are released; Space stops
/// it and sets it straight.
class RenderWindow : public QWindow
{
    Q_OBJECT

public:
    RenderWindow(const GpuEntry& gpu, QVulkanInstance* vk);
    ~RenderWindow() override;

    void setLevel(double level) { m_level = level; }
    /// Stops submitting at once and frees the device. Nothing is rendered
    /// after this returns; the window can then be deleted.
    void shutdown();
    /// The graphics API and adapter QRhi actually opened, once running.
    QString deviceText() const { return m_deviceText; }
    /// Asked once a frame: how far the kick swells the knot (0..1).
    void setPulseSource(std::function<float()> source) { m_pulse = std::move(source); }

    /// What the client sent, counted since the run started.
    struct InputStats
    {
        int mouseMoves = 0;
        int keyPresses = 0;
        QString lastKey;
        /// Spin of the knot, rad/s, and its tilt from upright, degrees.
        double spin = 0.0;
        double tiltDegrees = 0.0;
    };
    InputStats inputStats() const;

signals:
    /// An arrow key went down: +1 up or right, -1 down or left.
    void arrowPressed(int direction);
    /// A frame was submitted; `gpuMs` is the GPU time of the last one the GPU
    /// finished (0 while unknown).
    void frameDone(double gpuMs);
    /// The device could not be created or was lost.
    void failed(const QString& reason, bool deviceLost);
    void started(const QString& deviceText);

protected:
    void exposeEvent(QExposeEvent*) override;
    bool event(QEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;

private:
    bool init();
    void renderFrame();
    void releaseResources();
    void turn(double yaw, double pitch);
    void integrate(double dt);

    GpuEntry m_gpu;
    QVulkanInstance* m_vk = nullptr;
    QString m_deviceText;
    double m_level = 40.0;
    bool m_initialized = false;
    bool m_stopped = false;
    QTimer m_loop;
    QElapsedTimer m_clock;
    double m_lastFrameSeconds = 0.0;
    std::function<float()> m_pulse;

    // The client's hold on the knot.
    QQuaternion m_orientation;
    QVector3D m_spin;                                // angular velocity, rad/s, world axes
    bool m_arrows[4] = {false, false, false, false}; // left, right, up, down
    QPointF m_lastMouse;
    bool m_haveMouse = false;
    bool m_dragging = false;
    QVector3D m_dragVelocity; // rad/s of the last moves, thrown on release
    qint64 m_lastMoveNs = 0;
    InputStats m_input;

    std::unique_ptr<QRhi> m_rhi;
    std::unique_ptr<QRhiSwapChain> m_sc;
    std::unique_ptr<QRhiRenderBuffer> m_ds;
    std::unique_ptr<QRhiRenderPassDescriptor> m_rp;
    std::unique_ptr<QRhiBuffer> m_vbuf;
    std::unique_ptr<QRhiBuffer> m_ibuf;
    std::unique_ptr<QRhiBuffer> m_ubuf;
    std::unique_ptr<QRhiShaderResourceBindings> m_srb;
    std::unique_ptr<QRhiGraphicsPipeline> m_skyPipe;
    std::unique_ptr<QRhiGraphicsPipeline> m_knotPipe;
    quint32 m_indexCount = 0;
    QRhiResourceUpdateBatch* m_initialUpdates = nullptr;
};
