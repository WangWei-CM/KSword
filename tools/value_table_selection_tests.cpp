// QtCore-only canonical selection tests; no QWidget, clipboard or account/token APIs.
#include "../Ksword5.1/Ksword5.1/UI/ValueTableSelection.h"
#include <QCoreApplication>
#include <cstdio>
int main(int argc,char**argv)
{
    QCoreApplication app(argc,argv);
    using Selection=ks::ui::ValueTableSelection;
    int checks=0,failures=0;
    const auto expect=[&](bool ok,const char* what){++checks;if(!ok){++failures;std::printf("FAIL %s\n",what);}};
    auto keys=std::make_shared<QVector<QString>>();
    for(int row=0;row<400000;++row)keys->push_back(QString::number(row));
    Selection all;all.append(keys,{{0,399999}},{});
    auto selected=all.resolve([]{return false;});
    expect(selected&&selected->size()==400000,"one range captures full400k including unfetched rows");
    Selection changed;changed.setResolved(selected);changed.append(keys,{},{{0,0}});
    auto withoutFirst=changed.resolve([]{return false;});
    expect(withoutFirst->size()==399999&&!withoutFirst->contains("0")&&withoutFirst->contains("399999"),"deselect first preserves all hidden selections");
    expect(selected->size()==400000,"worker detaches copied selection value");
    auto onlyFirst=std::make_shared<QVector<QString>>(QVector<QString>{"0"});
    changed.append(onlyFirst,{{0,0}},{});
    expect(changed.resolve([]{return false;})->size()==400000,"reselect using filtered projection canonical key");
    Selection pair;pair.append(keys,{{0,1}},{});
    auto pairResolved=pair.resolve([]{return false;});
    expect(pairResolved->size()==2&&pairResolved->contains("1"),"A/B remain selected when B temporarily filtered");
    Selection clearAndSelect;clearAndSelect.append(onlyFirst,{{0,0}},{});
    auto replaced=clearAndSelect.resolve([]{return false;});
    expect(replaced->size()==1&&replaced->contains("0"),"explicit clear replaces complete hidden set");
    Selection empty;expect(empty.resolve([]{return false;})->isEmpty(),"empty completed selection has non-null empty value");
    int checksBeforeCancel=0;expect(!all.resolve([&]{return ++checksBeforeCancel>1;}),"large range cancels within128-row checks");
    expect(!empty.resolve([]{return true;}),"cancellation is distinct from empty selection");
    Selection clipped;clipped.append(onlyFirst,{{-5,10}},{});
    expect(clipped.resolve([]{return false;})->size()==1,"ranges clip to source extent");
    std::printf("VALUE_TABLE_SELECTION_CHECKS=%d FAILURES=%d\n",checks,failures);
    return failures?1:0;
}
