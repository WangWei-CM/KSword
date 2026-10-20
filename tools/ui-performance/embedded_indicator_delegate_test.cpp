#include <QCoreApplication>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QTimer>
#include <QRect>
#include <cstdio>
#include <cstdlib>
#include <random>
#include "EmbeddedCellLayout.h"

class FakeView final : public QObject
{
public:
    bool visible=false;
    int updates=0;
    bool isVisible() const { return visible; }
    FakeView* viewport() { return this; }
    void update() { ++updates; }
    void update(const QRect&) { ++updates; }
    QRect visualRect(const QModelIndex&) const { return {0,0,100,24}; }
};
namespace ks::settings { enum class DetailDisplayScheme { Embedded, Right }; }
namespace ks::ui
{
    class DetailLayoutHost final : public QObject
    {
    public:
        QPointer<FakeView> m_tableView;
        ks::settings::DetailDisplayScheme m_scheme=ks::settings::DetailDisplayScheme::Embedded;
        bool m_indicatorRefreshScheduled=false,m_embeddedIndicatorsDirty=true;
        int installations=0;
        void installEmbeddedRowDelegate() { ++installations; }
        void refreshEmbeddedIndicators();
        void scheduleEmbeddedIndicatorRefresh();
        void setSourceExpandedIndicator(const QPersistentModelIndex&,bool);
    };
#include "embedded_indicator_actual.inc"
}
int main(int argc,char** argv)
{
    QCoreApplication app(argc,argv);
    int checks=0;
    auto check=[&](bool value,const char* message) {++checks;if(!value){std::fprintf(stderr,"FAIL %s\n",message);std::exit(1);}};
    FakeView view;ks::ui::DetailLayoutHost host;host.m_tableView=&view;
    host.refreshEmbeddedIndicators();
    for(int row=0;row<100000;++row)host.scheduleEmbeddedIndicatorRefresh();
    QCoreApplication::processEvents();
    check(host.installations==0&&view.updates==0&&!host.m_indicatorRefreshScheduled,"hidden notifications retain dirty state only");
    view.visible=true;host.refreshEmbeddedIndicators();
    check(host.installations==1&&view.updates==1&&!host.m_embeddedIndicatorsDirty,"Show prepares visible delegate without root traversal");
    for(int row=0;row<100000;++row)host.scheduleEmbeddedIndicatorRefresh();
    check(host.installations==1&&host.m_indicatorRefreshScheduled,"visible notifications coalesce before delivery");
    QCoreApplication::processEvents();
    check(host.installations==2&&view.updates==2&&!host.m_indicatorRefreshScheduled,"visible batch delivers one viewport refresh");
    host.scheduleEmbeddedIndicatorRefresh();host.m_scheme=ks::settings::DetailDisplayScheme::Right;
    QCoreApplication::processEvents();
    check(host.installations==2&&!host.m_indicatorRefreshScheduled,"leaving Embedded cancels obsolete queued preparation");
    host.m_tableView.clear();host.scheduleEmbeddedIndicatorRefresh();host.refreshEmbeddedIndicators();
    check(host.installations==2,"destroyed view cannot be prepared");
    std::mt19937 random(20261020);
    for(int iteration=0;iteration<20000;++iteration)
    {
        const QRect cell(int(random()%100)-50,int(random()%100)-50,int(random()%300),int(random()%200));
        const int height=iteration%3?int(random()%200)+1:-1;
        for(bool rtl:{false,true})
        {
            const auto plain=ks::ui::LayoutEmbeddedCell(cell,height,false,rtl);
            const auto decorated=ks::ui::LayoutEmbeddedCell(cell,height,true,rtl);
            check(plain.source==plain.band&&plain.indicator.isEmpty(),"other columns preserve complete source geometry");
            check(decorated.band.height()==(height>0?std::min(height,cell.height()):cell.height()),"detail band clips only expanded row extent");
            check(decorated.source.width()>=0&&decorated.source.height()>=0,"narrow cells never produce negative source geometry");
            if(!decorated.indicator.isEmpty())
            {
                check(decorated.band.contains(decorated.indicator),"indicator stays inside original source band");
                check(!decorated.source.intersects(decorated.indicator),"business decoration and indicator never overlap");
                check(decorated.indicator.width()<=16&&decorated.indicator.height()<=16,"indicator remains bounded");
            }
        }
    }
    std::printf("PASS %d production indicator/geometry checks; no Widgets\n",checks);
}
