#include "FieldTreeModel.h"
#include <QCoreApplication>
#include <QItemSelectionModel>
#include <QPointer>
#include <QSignalBlocker>
#include <QThreadPool>
#include <iostream>
#include <functional>

namespace ks::ui {
struct TestInput : QObject { QString value; QString text() const { return value; } void setText(QString s) { value=std::move(s); } void setCurrentIndex(int i) { mode=i; } int mode=0; };
struct TestScroll { int position=0; int value() const { return position; } };
struct TestTree {
    explicit TestTree(FieldTreeModel* model) : selected(model) {}
    QItemSelectionModel selected; TestScroll vertical,horizontal; bool visible=true;
    QModelIndex currentIndex() const { return selected.currentIndex(); }
    QItemSelectionModel* selectionModel() { return &selected; }
    TestScroll* verticalScrollBar() { return &vertical; }
    TestScroll* horizontalScrollBar() { return &horizontal; }
    bool isVisible() const { return visible; }
};
class PooledFieldView : public QObject {
public:
    using Presentation=StructuredFieldView::Presentation;
    std::shared_ptr<const StructuredFieldViewState> captureViewState() const;
    bool restoreViewState(const std::shared_ptr<const StructuredFieldViewState>&);
    void flushValueSelection() const { ++flushes; }
    void applyPresentation() { ++presentations; if(onPresentation) onPresentation(); }
    void scheduleValueProjection() { ++projections; }
    TestTree* m_valueTree=nullptr; bool m_documentOnly=false;
    FieldDocument m_document;
    mutable int flushes=0; int projections=0,presentations=0;
    std::function<void()> onPresentation;
    std::shared_ptr<std::atomic_bool> m_valueCancel;
    quint64 m_valueTicket=0;
    std::shared_ptr<FieldTreeSelection> m_valueSelection=std::make_shared<FieldTreeSelection>();
    QHash<QString,bool> m_valueExpansion;
    TestInput search,style; TestInput* m_search=&search; TestInput* m_style=&style;
    Presentation m_presentation=Presentation::Sections;
    bool m_valueSearchActive=false,m_valueRestoreSelection=false,m_valueClearPending=false,m_valueApplying=false,m_valuePrepared=true;
    bool m_valueRestoreHorizontalScroll=false;
    QString m_valueSelected,m_valueBeforeSearchSelected;
    int m_valueColumn=0,m_valueScroll=0,m_valueHorizontalScroll=0,m_valueBeforeSearchColumn=0,m_valueBeforeSearchScroll=0;
};
}
#include "field_view_state_actual.inc"

int main(int argc,char** argv) {
    QCoreApplication app(argc,argv); using namespace ks::ui; using Presentation=StructuredFieldView::Presentation; int checks=0;
    const auto check=[&](bool v) { ++checks; if(!v) { std::cerr<<"FAIL "<<checks<<'\n'; std::exit(1); } };
    FieldDocument doc; doc.title=QStringLiteral("same title");
    for(int i=0;i<100000;++i) { FieldNode n; n.name=QString::number(i); n.value=QStringLiteral("value"); doc.nodes.append(n); }
    auto basis=PrepareFieldTreeProjection(doc,{},{});
    FieldTreeModel model; model.applySnapshot(basis); TestTree tree(&model);
    PooledFieldView a; a.m_valueTree=&tree; a.m_document=doc;
    a.m_valueSelection->selectAll(basis,{},false);
    a.m_valueSelection->setSelected(basis->entries.at(3).key,false);
    a.m_valueExpansion.insert(QStringLiteral("scope"),false);
    a.search.value=QStringLiteral("query A"); a.m_presentation=Presentation::Tree; a.m_valueSearchActive=true;
    a.m_valueBeforeSearchSelected=QStringLiteral("before key"); a.m_valueBeforeSearchColumn=1; a.m_valueBeforeSearchScroll=753;
    tree.selected.setCurrentIndex(model.index(4,1),QItemSelectionModel::Select);
    tree.vertical.position=127; tree.horizontal.position=31;
    auto saved=a.captureViewState(); check(bool(saved)); check(a.flushes==1);
    check(saved->document.nodes.isSharedWith(doc.nodes));
    check(saved->selection->contains(basis->entries.at(99999).key)); check(!saved->selection->contains(basis->entries.at(3).key));
    a.m_valueSelection->setSelected(basis->entries.at(3).key,true);
    check(!saved->selection->contains(basis->entries.at(3).key));
    PooledFieldView b; b.m_valueTree=&tree; b.m_document=doc;
    b.m_valueCancel=std::make_shared<std::atomic_bool>(false);
    check(b.restoreViewState(saved)); check(b.m_valueCancel->load()); check(b.m_valueTicket==1);
    check(!tree.selected.currentIndex().isValid()); check(tree.selected.selectedRows().isEmpty());
    check(!b.m_valuePrepared); check(b.m_valueRestoreSelection); check(b.projections==1);
    check(b.m_valueSelection->contains(basis->entries.at(99999).key)); check(!b.m_valueSelection->contains(basis->entries.at(3).key));
    check(b.m_valueExpansion.value(QStringLiteral("scope"),true)==false); check(b.search.text()==QStringLiteral("query A"));
    check(b.m_valueSelected==basis->entries.at(4).key); check(b.m_valueColumn==1); check(b.m_valueScroll==127);
    check(b.m_valueHorizontalScroll==31 && b.m_valueRestoreHorizontalScroll);
    check(b.m_valueBeforeSearchSelected==QStringLiteral("before key") && b.m_valueBeforeSearchColumn==1 && b.m_valueBeforeSearchScroll==753);
    check(b.m_presentation==Presentation::Tree && b.style.mode==1 && b.m_valueSearchActive);
    tree.visible=false; auto pending=b.captureViewState(); check(pending->selected==saved->selected && pending->scroll==127 && pending->horizontalScroll==31);
    // An equal-value independently allocated document is a distinct snapshot.
    b.m_document.nodes.detach(); check(!b.restoreViewState(saved));
    check(b.search.text().isEmpty() && b.m_valueSelected.isEmpty() && b.m_valueScroll==0 && b.m_valueHorizontalScroll==0);
    check(b.m_presentation==Presentation::Sections && b.style.mode==0 && !b.m_valueSearchActive);
    check(!b.m_valueSelection->contains(basis->entries.at(99999).key) && b.m_valueExpansion.isEmpty());
    b.m_document=doc; check(!b.restoreViewState({}));
    b.m_documentOnly=true; check(!b.captureViewState()); check(!b.restoreViewState(saved)); b.m_documentOnly=false;
    b.m_valueTree=nullptr; check(!b.captureViewState()); check(!b.restoreViewState(saved));
    // Native selection signals can synchronously replace the restore generation.
    tree.visible=true; tree.selected.setCurrentIndex(model.index(1,0),QItemSelectionModel::Select);
    PooledFieldView c; c.m_valueTree=&tree; c.m_document=doc;
    const auto connection=QObject::connect(&tree.selected,&QItemSelectionModel::currentChanged,&c,[&] { ++c.m_valueTicket; });
    check(c.restoreViewState(saved)); check(c.projections==0); check(!c.m_valueApplying); QObject::disconnect(connection);
    // A nested restore wins; the old row must not overwrite its query/position.
    a.search.value=QStringLiteral("query NEW"); a.m_valueSelected=QStringLiteral("new key");
    a.m_valueRestoreSelection=true; auto newer=a.captureViewState();
    tree.selected.setCurrentIndex(model.index(1,0),QItemSelectionModel::Select);
    PooledFieldView nested; nested.m_valueTree=&tree; nested.m_document=doc;
    bool entered=false;
    const auto reentrant=QObject::connect(&tree.selected,&QItemSelectionModel::currentChanged,&nested,[&] {
        if(!entered) { entered=true; nested.restoreViewState(newer); }
    });
    check(nested.restoreViewState(saved)); check(entered); check(nested.projections==1);
    check(nested.search.text()==QStringLiteral("query NEW") && nested.m_valueSelected==QStringLiteral("new key"));
    check(!nested.m_valueApplying); QObject::disconnect(reentrant);
    // The referenced caller member can be released inside clear's signals.
    std::shared_ptr<const StructuredFieldViewState> callerState=a.captureViewState();
    const std::weak_ptr<const StructuredFieldViewState> weakState=callerState;
    tree.selected.setCurrentIndex(model.index(1,0),QItemSelectionModel::Select);
    PooledFieldView releasing; releasing.m_valueTree=&tree; releasing.m_document=doc;
    bool retainedInside=false;
    const auto release=QObject::connect(&tree.selected,&QItemSelectionModel::currentChanged,&releasing,[&] {
        callerState.reset(); retainedInside=!weakState.expired();
    });
    check(releasing.restoreViewState(callerState)); check(retainedInside); check(weakState.expired());
    check(releasing.search.text()==QStringLiteral("query NEW")); QObject::disconnect(release);
    // Same signal can destroy the host. The port must return without touching it.
    auto* dying=new PooledFieldView; dying->m_valueTree=&tree; dying->m_document=doc;
    tree.selected.setCurrentIndex(model.index(1,0),QItemSelectionModel::Select);
    QPointer<PooledFieldView> guard(dying);
    QObject::connect(&tree.selected,&QItemSelectionModel::currentChanged,dying,[dying] { delete dying; });
    check(dying->restoreViewState(saved)); check(!guard);
    QThreadPool::globalInstance()->waitForDone();
    std::cout<<"PASS "<<checks<<" checks; actual state methods + real canonical selection basis 100000 entries; QtCore only\n";
}
