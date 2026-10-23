#include "TableSearchFilterSnapshot.h"
#include <QAbstractTableModel>
#include <QCoreApplication>
#include <QPointer>
#include <QSet>
#include <memory>
#include <random>
#include <cstdio>
#include <cstdlib>

class QTableView final : public QObject
{
public:
    QAbstractItemModel* value = nullptr;
    QSet<int> hidden, frozen;
    QAbstractItemModel* model() const { return value; }
    QModelIndex rootIndex() const { return {}; }
    bool isColumnHidden(int column) const { return hidden.contains(column); }
};
namespace ks::ui { bool isColumnHiddenByFreeze(QTableView* table,int column) { return table->frozen.contains(column); } }
class TableSearchAccessWidget;
static QTableView* installedTable = nullptr;
static TableSearchAccessWidget* installedSupport = nullptr;
static TableSearchAccessWidget* searchSupportForTable(QTableView* table)
{ return table && table==installedTable ? installedSupport : nullptr; }
#include "actual_search_filter_methods.inc"

class Model final : public QAbstractTableModel
{
public:
    QStringList cells;
    mutable int rowsRead=0, columnsRead=0, cellsRead=0;
    int rowCount(const QModelIndex& parent={}) const override { ++rowsRead;return parent.isValid()?0:1; }
    int columnCount(const QModelIndex& parent={}) const override { ++columnsRead;return parent.isValid()?0:cells.size(); }
    QVariant data(const QModelIndex& index,int role) const override
    { ++cellsRead;return role==Qt::DisplayRole&&index.isValid()?QVariant(cells.value(index.column())):QVariant(); }
};
int main(int argc,char** argv)
{
    QCoreApplication app(argc,argv);int checks=0;
    const auto check=[&](bool value,const char* message){++checks;if(!value){std::fprintf(stderr,"FAIL %s at %d\n",message,checks);std::exit(1);}};
    Model model;model.cells={QStringLiteral("alpha"),QStringLiteral("隐藏独有"),QStringLiteral("FROZEN"),QStringLiteral("last")};
    QTableView table;table.value=&model;table.hidden={1,2};table.frozen={2};
    TableSearchAccessWidget support;support.m_tableView=&table;support.m_filterModel=&model;
    installedTable=&table;installedSupport=&support;
    check(!ks::ui::CaptureTableSearchResultFilterSpec(nullptr).active,"null source inactive");
    check(!ks::ui::CaptureTableSearchResultFilterSpec(&table).active,"popup-only highlight inactive");
    support.m_resultFilterActive=true;support.m_resultFilterQuery=QStringLiteral("frozen");
    model.rowsRead=model.columnsRead=model.cellsRead=0;
    auto spec=ks::ui::CaptureTableSearchResultFilterSpec(&table);
    check(model.rowsRead==0&&model.cellsRead==0&&model.columnsRead==1,"capture reads column scope only");
    check(spec.active&&spec.query==QStringLiteral("frozen")&&spec.columns==QVector<int>({0,2,3}),"capture includes visible/frozen columns and excludes user hidden");
    check(ks::ui::TableSearchResultFilterMatches(spec,model.cells),"frozen column matches canonical displayed values");
    support.m_searchColumns=spec.columns;
    check(support.rowMatchesQuery(0)==ks::ui::TableSearchResultFilterMatches(spec,model.cells),"actual row matcher agrees");
    table.hidden.insert(3);support.m_resultFilterQuery=QStringLiteral("changed");
    check(spec.columns==QVector<int>({0,2,3})&&spec.query==QStringLiteral("frozen"),"captured values independent of future UI edits");
    const auto current=ks::ui::CaptureTableSearchResultFilterSpec(&table);
    check(current.columns==QVector<int>({0,2})&&current.query==QStringLiteral("changed"),"capture reads current column scope before queued projection");
    table.value=nullptr;check(!ks::ui::CaptureTableSearchResultFilterSpec(&table).active,"detached model inactive");table.value=&model;
    auto inactive=spec;inactive.active=false;check(ks::ui::TableSearchResultFilterMatches(inactive,{}),"inactive spec retains every canonical row");
    auto empty=spec;empty.columns.clear();check(!ks::ui::TableSearchResultFilterMatches(empty,model.cells),"active zero searched columns excludes row");
    std::mt19937 random(0x53c11);const QStringList words={QStringLiteral("Alpha"),QStringLiteral("BETA"),QStringLiteral("事件42"),QStringLiteral("路径\\last"),QStringLiteral(""),QStringLiteral("école"),QStringLiteral("unmatched")};
    for(int iteration=0;iteration<20000;++iteration)
    {
        model.cells.clear();support.m_searchColumns.clear();
        for(int column=0;column<6;++column){model.cells.push_back(words.at(random()%words.size()));if(random()%2)support.m_searchColumns.push_back(column);}
        support.m_resultFilterQuery=words.at(random()%words.size());
        bool expected=false;for(int column:support.m_searchColumns)if(model.cells.at(column).contains(support.m_resultFilterQuery,Qt::CaseInsensitive))expected=true;
        const ks::ui::TableSearchResultFilterSpec value{true,support.m_resultFilterQuery,support.m_searchColumns};
        check(support.rowMatchesQuery(0)==expected,"actual generic matcher preserves original QString contains semantics");
        check(ks::ui::TableSearchResultFilterMatches(value,model.cells)==expected,"immutable archive matcher equals original generic matcher");
    }
    std::printf("PASS %d actual capture/generic/value filter checks; no Widgets\n",checks);
}
