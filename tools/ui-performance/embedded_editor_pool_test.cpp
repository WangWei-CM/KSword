#include <QAbstractTableModel>
#include <QCoreApplication>
#include <QEvent>
#include <QHash>
#include <QList>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QRect>
#include <QSet>
#include <QTimer>
#include <algorithm>
#include <functional>
#include <iostream>
#include <memory>
#include <vector>
template<class T,class U>T test_cast(U* value){return dynamic_cast<T>(value);}
struct MockWidget:QObject {
    explicit MockWidget(QObject*p=nullptr):QObject(p){}
    bool hidden=true;QRect area{0,0,600,400};int geometryWrites=0,shows=0,raises=0;std::function<void()> hook;
    bool isHidden()const{return hidden;}bool isVisible()const{return !hidden;}int width()const{return area.width();}int height()const{return area.height();}QRect rect()const{return QRect(0,0,width(),height());}QRect geometry()const{return area;}
    void setGeometry(QRect r){area=r;++geometryWrites;if(hook)hook();}void setMinimumSize(int,int){}
    void hide(){hidden=true;if(hook)hook();}void show(){hidden=false;++shows;if(hook)hook();}void raise(){++raises;if(hook)hook();}
};
struct Model:QAbstractTableModel {
    int total=100000;mutable int reads=0;
    int rowCount(const QModelIndex&p={})const override{return p.isValid()?0:total;}
    int columnCount(const QModelIndex& parent={})const override{Q_UNUSED(parent);return 3;}
    QModelIndex index(int r,int c,const QModelIndex&p={})const override{++reads;return QAbstractTableModel::index(r,c,p);}
    QVariant data(const QModelIndex&,int)const override{return{};}
};
struct MockItemView:MockWidget {
    explicit MockItemView(Model*m):data(m){hidden=false;port.hidden=false;}
    Model*data;MockWidget port;int scroll=0;std::vector<int> heights,prefix;
    Model*model()const{return data;}QModelIndex rootIndex()const{return{};}MockWidget*viewport()const{return const_cast<MockWidget*>(&port);}
    void rebuild(){prefix.assign(1,0);for(int h:heights)prefix.push_back(prefix.back()+h);}
    int rowAt(int y)const{int pos=y+scroll;if(pos<0||prefix.empty()||pos>=prefix.back())return-1;return int(std::upper_bound(prefix.begin(),prefix.end(),pos)-prefix.begin())-1;}
    int rowViewportPosition(int r)const{return prefix.at(r)-scroll;}int rowHeight(int r)const{return heights.at(r);}
    virtual QRect visualRect(const QModelIndex&i)const{return i.isValid()?QRect(0,rowViewportPosition(i.row()),600,rowHeight(i.row())):QRect();}
};
struct MockTable:MockItemView{using MockItemView::MockItemView;};
struct MockHeader {
    bool firstHidden=false;int offset=0;
    int count()const{return 3;}int logicalIndex(int v)const{return v;}bool isSectionHidden(int c)const{return c==0&&firstHidden;}int sectionViewportPosition(int c)const{return 200*c-offset;}int sectionSize(int)const{return 200;}
};
struct MockTree:MockItemView {
    using MockItemView::MockItemView;MockHeader head;mutable int rectReads=0,belowReads=0;
    MockHeader*header()const{return const_cast<MockHeader*>(&head);}
    QModelIndex indexAt(QPoint p)const{const int r=rowAt(p.y()),c=(p.x()+head.offset)/200;return r<0||head.isSectionHidden(c)?QModelIndex():model()->index(r,c);}
    QModelIndex indexBelow(const QModelIndex&i)const{++belowReads;int row=i.row()+1;while(row<int(heights.size())&&!heights.at(row))++row;return row<int(heights.size())?model()->index(row,0):QModelIndex();}
    QRect visualRect(const QModelIndex&i)const override{++rectReads;return head.isSectionHidden(i.column())?QRect():MockItemView::visualRect(i);}
};
struct CodeEditorReadOnlyViewState{int reading=0;};
struct MockText:MockWidget {
    using MockWidget::MockWidget;QString text;int reading=0;bool readonly=false;
    void setReadOnly(bool v){readonly=v;}void setRawText(QString s){text=std::move(s);reading=0;}
    std::shared_ptr<const CodeEditorReadOnlyViewState>captureReadOnlyViewState()const{auto s=std::make_shared<CodeEditorReadOnlyViewState>();s->reading=reading;return s;}
    bool restoreReadOnlyViewState(const std::shared_ptr<const CodeEditorReadOnlyViewState>&s){reading=s?s->reading:0;return readonly;}
};
namespace ks::settings {enum class DetailDisplayScheme{Embedded,Other};}
namespace ks::ui {
struct MockDocument {QString text;};struct StructuredFieldViewState{MockDocument document;int reading=0;};
struct MockFields:MockWidget {
    enum class Presentation{Sections,Tree};using MockWidget::MockWidget;MockDocument doc;int reading=0;bool valueModel=false;Presentation presentation=Presentation::Sections;
    void setValueModelProjection(){valueModel=true;}void setDocument(MockDocument d){doc=std::move(d);reading=0;}
    std::shared_ptr<const StructuredFieldViewState>captureViewState()const{auto s=std::make_shared<StructuredFieldViewState>();s->document=doc;s->reading=reading;return s;}
    bool restoreViewState(const std::shared_ptr<const StructuredFieldViewState>&s){reading=s&&s->document.text==doc.text?s->reading:0;return bool(s);}
    void setPresentation(Presentation p){presentation=p;}
};
class DetailLayoutHost:public QObject {
public:
    struct EmbeddedEntry{QPersistentModelIndex sourceIndex;QPointer<MockText>textEditor;QPointer<MockFields>fieldView;QPointer<MockWidget>editorWidget;int originalRowHeight=24,detailHeight=128;MockDocument document;QString detailText;MockFields::Presentation presentation=MockFields::Presentation::Sections;std::shared_ptr<const StructuredFieldViewState>fieldState;std::shared_ptr<const CodeEditorReadOnlyViewState>textState;bool structured=false;};
    int embeddedOriginalRowHeight(const QModelIndex&)const;void updateEmbeddedEditorGeometries();QList<QPersistentModelIndex>visibleEmbeddedRows()const;void releaseEmbeddedEditor(const std::shared_ptr<EmbeddedEntry>&);bool realizeEmbeddedEntry(const std::shared_ptr<EmbeddedEntry>&);void trimEmbeddedEditorPool();void flushEmbeddedEditorGeometries();
    QPointer<MockItemView>m_tableView;QHash<QPersistentModelIndex,std::shared_ptr<EmbeddedEntry>>m_embeddedEntries;QSet<QPersistentModelIndex>m_activeEmbeddedRows;QList<QPointer<MockText>>m_textEditorPool;QList<QPointer<MockFields>>m_fieldEditorPool;bool m_embeddedGeometryScheduled=false;quint64 m_embeddedRevision=0,m_bindingGeneration=0;ks::settings::DetailDisplayScheme m_scheme=ks::settings::DetailDisplayScheme::Embedded;
};
}
#include "embedded_editor_pool_actual.inc"
int main(int argc,char**argv){QCoreApplication app(argc,argv);using namespace ks::ui;int checks=0;const auto check=[&](bool v){++checks;if(!v){std::cerr<<"FAIL "<<checks<<'\n';std::exit(1);}};
    Model model;MockTable table(&model);table.heights.assign(model.total,24);DetailLayoutHost host;host.m_tableView=&table;
    for(int row=0;row<1000;++row){auto e=std::make_shared<DetailLayoutHost::EmbeddedEntry>();e->sourceIndex=model.index(row,0);e->structured=row%2;e->detailText=QString::number(row);e->document.text=e->detailText;host.m_embeddedEntries.insert(e->sourceIndex,e);table.heights[row]=152;}
    table.rebuild();model.reads=0;for(int repeat=0;repeat<100000;++repeat)host.updateEmbeddedEditorGeometries();check(host.m_embeddedGeometryScheduled);check(model.reads==0);QCoreApplication::processEvents();check(!host.m_embeddedGeometryScheduled);check(model.reads<30);check(host.m_activeEmbeddedRows.size()==3);
    auto first=host.m_embeddedEntries.value(model.index(0,0));check(first->textEditor&&first->textEditor->readonly);first->textEditor->reading=778;
    auto field=host.m_embeddedEntries.value(model.index(1,0));check(field->fieldView&&field->fieldView->valueModel);field->fieldView->reading=991;
    const int geometry=first->editorWidget->geometryWrites,raised=first->editorWidget->raises;host.flushEmbeddedEditorGeometries();check(first->editorWidget->geometryWrites==geometry&&first->editorWidget->raises==raised);
    int maxActive=0;
    for(int row=5;row<995;row+=7){table.scroll=table.prefix[row];model.reads=0;host.flushEmbeddedEditorGeometries();maxActive=std::max(maxActive,int(host.m_activeEmbeddedRows.size()));check(model.reads<40);check(host.m_activeEmbeddedRows.size()<=4);check(host.m_textEditorPool.size()<=1&&host.m_fieldEditorPool.size()<=1);QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);}
    check(maxActive<=4&&host.m_embeddedEntries.size()==1000);check(!first->editorWidget&&first->textState&&first->textState->reading==778);check(!field->editorWidget&&field->fieldState&&field->fieldState->reading==991);
    table.scroll=0;host.flushEmbeddedEditorGeometries();check(first->textEditor&&first->textEditor->reading==778&&first->textEditor->text==QStringLiteral("0"));check(field->fieldView&&field->fieldView->reading==991&&field->fieldView->doc.text==QStringLiteral("1"));
    for(int r=0;r<100000;r+=37)check(host.embeddedOriginalRowHeight(model.index(r,2))==(r<1000?24:-1));
    table.hidden=true;host.flushEmbeddedEditorGeometries();check(host.m_activeEmbeddedRows.isEmpty());check(host.m_embeddedEntries.size()==1000);table.hidden=false;
    MockTree tree(&model);tree.heights=table.heights;tree.heights[1]=0;tree.rebuild();tree.head.firstHidden=true;tree.head.offset=220;host.m_tableView=&tree;host.flushEmbeddedEditorGeometries();check(!host.m_activeEmbeddedRows.isEmpty());check(!host.m_activeEmbeddedRows.contains(model.index(1,0)));check(tree.belowReads<10&&tree.rectReads<20);
    // A geometry callback can replace the binding; remaining old rows stop.
    table.scroll=0;host.m_tableView=&table;host.flushEmbeddedEditorGeometries();auto visible=host.m_embeddedEntries.value(model.index(0,0));check(bool(visible->editorWidget));visible->editorWidget->hook=[&]{++host.m_bindingGeneration;};table.port.area.setWidth(620);host.flushEmbeddedEditorGeometries();visible->editorWidget->hook={};check(host.m_bindingGeneration==1);
    std::cout<<"PASS "<<checks<<" checks; actual viewport/pool/geometry methods with QtCore view mocks, 1000 retained expanded rows\n";
}
