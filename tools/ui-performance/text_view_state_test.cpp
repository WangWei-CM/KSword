#include <QCoreApplication>
#include <QPointer>
#include <QSignalBlocker>
#include <QString>
#include <algorithm>
#include <functional>
#include <iostream>
#include <memory>
struct CodeEditorReadOnlyViewState;
struct TestPanel:QObject { bool hidden=true; std::function<void()> hook; bool isHidden()const{return hidden;} void setVisible(bool v){hidden=!v;if(hook)hook();} };
struct TestInput:TestPanel { QString value;int cursor=0,first=-1,length=0;QString text()const{return value;}void setText(QString v){value=std::move(v);first=-1;length=0;}int cursorPosition()const{return cursor;}int selectionStart()const{return first;}QString selectedText()const{return first<0?QString():value.mid(first,length);}void setSelection(int p,int n){first=p;length=n;}void setCursorPosition(int p){cursor=p;} };
struct TestToggle:TestPanel {bool checked=false;bool isChecked()const{return checked;}void setChecked(bool v){checked=v;if(!signalsBlocked()&&hook)hook();} };
struct TestCombo:TestPanel {int index=0;int currentIndex()const{return index;}int count()const{return 4;}void setCurrentIndex(int i){index=i;if(hook)hook();} };
struct TestScroll {int current=0;int value()const{return current;}void setValue(int v){current=v;} };
struct TestDocument {int size=8;int characterCount()const{return size;} };
struct TestCursor {enum MoveMode{MoveAnchor,KeepAnchor};int point=0,start=0;explicit TestCursor(TestDocument*){}TestCursor()=default;int position()const{return point;}int anchor()const{return start;}void setPosition(int p,MoveMode mode=MoveAnchor){point=p;if(mode==MoveAnchor)start=p;} };
struct TestTextEdit:QObject {TestDocument doc;TestCursor cursor;TestScroll vertical,horizontal;TestCursor textCursor()const{return cursor;}TestDocument* document(){return &doc;}TestScroll* verticalScrollBar()const{return const_cast<TestScroll*>(&vertical);}TestScroll* horizontalScrollBar()const{return const_cast<TestScroll*>(&horizontal);}void setTextCursor(TestCursor c){cursor=c;} };
class PooledTextEditor:public QObject {
public:
    struct TestLayout { void activate() {} } fixedLayout;
    TestLayout* layout(){return &fixedLayout;}
    std::shared_ptr<const CodeEditorReadOnlyViewState> captureReadOnlyViewState()const;
    bool restoreReadOnlyViewState(const std::shared_ptr<const CodeEditorReadOnlyViewState>&);
    template<class T>T findChild(const QString&,Qt::FindChildOptions)const{return static_cast<T>(edit);}
    bool wordWrapEnabled()const{return wrap;}void setWordWrapEnabled(bool v){wrap=v;if(wrapHook)wrapHook();}void updateFindHighlights(){++highlights;}
    TestTextEdit* edit=nullptr;bool m_readOnlyMode=true,m_destroying=false,wrap=true;quint64 m_readOnlyViewRestoreGeneration=0;int highlights=0;std::function<void()>wrapHook;
    TestInput find,gotoInput;TestInput* m_findEdit=&find;TestInput* m_gotoLineEdit=&gotoInput;
    TestPanel findPanel,gotoPanel;TestPanel* m_findPanel=&findPanel;TestPanel* m_gotoPanel=&gotoPanel;
    TestToggle match,whole,whitespace;TestToggle* m_matchCaseButton=&match;TestToggle* m_wholeWordButton=&whole;TestToggle* m_whitespaceAction=&whitespace;
    TestCombo syntax;TestCombo* m_languageCombo=&syntax;
};
#include "text_view_state_actual.inc"
int main(int argc,char**argv){QCoreApplication app(argc,argv);int checks=0;const auto check=[&](bool v){++checks;if(!v){std::cerr<<"FAIL "<<checks<<'\n';std::exit(1);}};
    TestTextEdit edit;edit.cursor.point=17;edit.cursor.start=4;edit.vertical.current=92;edit.horizontal.current=14;
    PooledTextEditor source;source.edit=&edit;source.find.value=QStringLiteral("abc query");source.find.first=2;source.find.length=3;source.find.cursor=5;source.gotoInput.value=QStringLiteral("100");source.findPanel.hidden=false;source.wrap=false;source.syntax.index=3;source.match.checked=true;source.whole.checked=true;source.whitespace.checked=true;
    auto saved=source.captureReadOnlyViewState();check(bool(saved));check(saved->position==17&&saved->anchor==4&&saved->vertical==92&&saved->horizontal==14);check(saved->querySelection==2&&saved->querySelectionLength==3&&saved->queryCursor==5);
    PooledTextEditor target;target.edit=&edit;check(target.restoreReadOnlyViewState(saved));check(target.find.value==QStringLiteral("abc query")&&target.gotoInput.value==QStringLiteral("100"));check(target.find.first==2&&target.find.length==3);check(!target.findPanel.hidden&&target.gotoPanel.hidden);check(!target.wrap&&target.syntax.index==3&&target.match.checked&&target.whole.checked&&target.whitespace.checked);check(edit.cursor.point==7&&edit.cursor.start==4);check(edit.vertical.current==92&&edit.horizontal.current==14);check(target.highlights==1);
    check(target.restoreReadOnlyViewState({}));check(target.find.value.isEmpty()&&target.gotoInput.value.isEmpty()&&target.findPanel.hidden&&target.gotoPanel.hidden);check(target.wrap&&target.syntax.index==0&&!target.match.checked&&!target.whole.checked&&!target.whitespace.checked);check(edit.cursor.point==0&&edit.cursor.start==0&&edit.vertical.current==0&&edit.horizontal.current==0);
    target.m_readOnlyMode=false;check(!target.restoreReadOnlyViewState(saved));check(!target.captureReadOnlyViewState());target.m_readOnlyMode=true;target.m_destroying=true;check(!target.restoreReadOnlyViewState(saved));check(!target.captureReadOnlyViewState());target.m_destroying=false;target.edit=nullptr;check(!target.restoreReadOnlyViewState(saved));check(!target.captureReadOnlyViewState());target.edit=&edit;
    source.find.value=QStringLiteral("NEW");source.wrap=true;auto newer=source.captureReadOnlyViewState();bool nested=false;target.syntax.hook=[&]{if(!nested){nested=true;target.restoreReadOnlyViewState(newer);}};check(target.restoreReadOnlyViewState(saved));check(nested&&target.find.value==QStringLiteral("NEW")&&target.wrap);check(target.highlights==3);target.syntax.hook={};
    auto callerState=source.captureReadOnlyViewState();std::weak_ptr<const CodeEditorReadOnlyViewState> weak=callerState;bool retained=false;
    target.wrapHook=[&]{callerState.reset();retained=!weak.expired();};check(target.restoreReadOnlyViewState(callerState));check(retained);check(weak.expired());check(target.find.value==QStringLiteral("NEW"));target.wrapHook={};
    auto* dying=new PooledTextEditor;dying->edit=&edit;QPointer<PooledTextEditor> alive(dying);dying->wrapHook=[dying]{delete dying;};check(dying->restoreReadOnlyViewState(saved));check(!alive);
    std::cout<<"PASS "<<checks<<" checks; actual read-only state methods; QtCore mocks, no Widgets\n";
}
