#pragma once
#include <QRect>
#include <algorithm>

namespace ks::ui
{
    struct EmbeddedCellLayout
    {
        QRect band;
        QRect source;
        QRect indicator;
    };
    // Pure geometry, shared by paint and source-delegate editor/hit testing.
    inline EmbeddedCellLayout LayoutEmbeddedCell(const QRect& cell, int originalHeight,
        bool firstColumn, bool rightToLeft)
    {
        EmbeddedCellLayout result{cell,cell,{}};
        if (originalHeight > 0) result.band.setHeight(std::min(originalHeight,cell.height()));
        result.source = result.band;
        if (!firstColumn || result.band.width() <= 0 || result.band.height() <= 0) return result;
        const int gutter = std::min(20,result.band.width());
        const int size = std::max(0,std::min({16,gutter-4,result.band.height()-4}));
        const int x = rightToLeft ? result.band.right()-gutter+(gutter-size)/2+1
            : result.band.left()+(gutter-size)/2;
        result.indicator = QRect(x,result.band.top()+(result.band.height()-size)/2,size,size);
        if (rightToLeft) result.source.adjust(0,0,-gutter,0);
        else result.source.adjust(gutter,0,0,0);
        return result;
    }
}
