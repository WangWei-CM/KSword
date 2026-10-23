#pragma once
#include "WindowControlInspection.h"
class QWidget;

namespace ks::control_inspection
{
    // Properties not painted by the overlay remain in the complete canonical
    // Node snapshot; they do not invalidate geometry or a hover card.
    inline bool SameOverlayNode(const Node& a, const Node& b)
    {
        return a.id == b.id && a.host == b.host && a.bounds == b.bounds
            && a.hostBounds == b.hostBounds && a.offscreen == b.offscreen
            && a.name == b.name && a.type == b.type && a.enabled == b.enabled;
    }
    inline bool OverlaySceneChanged(quint64 revision, quint64 previousRevision,
        const QVector<Node>& nodes, const QVector<Node>& previous)
    {
        if (revision != 0) return revision != previousRevision;
        if (nodes.size() != previous.size()) return true;
        for (qsizetype i = 0; i < nodes.size(); ++i)
            if (!SameOverlayNode(nodes[i], previous[i])) return true;
        return false;
    }
    QWidget* CreateOverlay(QWidget* owner);
    void FitOverlay(QWidget* overlay);
    bool ClipOverlay(QWidget* overlay, HWND root, HWND excluded);
    void UpdateOverlay(QWidget* overlay, const QVector<Node>& nodes,
        const Node& hovered, const Node& selected, bool all, bool tips, QPoint physicalCursor, quint64 sceneRevision = 0);
}
