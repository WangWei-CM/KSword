// This class has value-only controls and immutable inputs. A missed Win32
// control call cannot compile: Control has no conversion to HWND.
namespace {
struct KernelValueControl {
    int id = 0;
    explicit operator bool() const noexcept { return id != 0; }
};

class KernelValueProjectionBuilder final {
public:
    explicit KernelValueProjectionBuilder(std::shared_ptr<const KernelProjectionInput> input)
        : input_(std::move(input)), currentColumns_(input_->source->columns),
          currentRawColumns_(input_->source->columns), currentRawRows_(input_->source->rows) {
        if (!currentRawRows_ || (currentRawRows_->empty() && !input_->source->fallbackDisplay.empty())) {
            currentRawRows_ = input_->source->fallbackDisplay.contiguousSnapshot();
        }
        if (!currentRawRows_) currentRawRows_ = EmptyKernelRawRows();
        for(std::size_t index=0;index<input_->selectedChoices.size();++index) {
            auto& choice=choices_[index];choice.items.push_back(L"");
            if(!input_->selectedChoices[index].empty()){choice.items.push_back(input_->selectedChoices[index]);choice.selected=1;}
        }
    }
    std::shared_ptr<KernelValueProjection> build(const std::uint64_t requestId) {
        result_=std::make_shared<KernelValueProjection>();
        result_->input=input_;
        result_->requestId=requestId;
        const auto feature=input_->source->featureId;
        switch(feature) {
        case KernelFeatureId::ObjectNamespaceOverview:
        case KernelFeatureId::ObjectDirectoryRecursive:
        case KernelFeatureId::NamedPipe:
        case KernelFeatureId::BaseNamedObjects:
        case KernelFeatureId::SymbolicLink:
        case KernelFeatureId::DeviceDriverObjects:
        case KernelFeatureId::ObjectTypeMatrix:
        case KernelFeatureId::CommunicationEndpoint: RebuildObjectNamespaceListFromCache(feature); break;
        case KernelFeatureId::CpuHardwareSnapshot:
        case KernelFeatureId::PhysicalMemoryLayout:
        case KernelFeatureId::MutationAudit:
        case KernelFeatureId::KeyboardHotkeys:
        case KernelFeatureId::KeyboardHooks:
        case KernelFeatureId::DynDataCapabilities:
        case KernelFeatureId::MinifilterBypassPids:
        case KernelFeatureId::KernelTimerDpc:
        case KernelFeatureId::IoctlRegistry: RebuildR0ValueProjection(feature); break;
        case KernelFeatureId::DynData:
        case KernelFeatureId::DriverStatus: RebuildDiagnosticDualTableFromCache(feature); break;
        case KernelFeatureId::CallbackEnumeration: RebuildCallbackEnumerationListFromCache(); break;
        case KernelFeatureId::AtomTable: RebuildAtomTableFromCache(); break;
        case KernelFeatureId::KernelExecutableMemory:
        case KernelFeatureId::KernelMemoryEvidence: RebuildKernelMemoryScanListFromCache(feature); break;
        case KernelFeatureId::ProcessCrossView:
        case KernelFeatureId::ThreadCrossView: RebuildCrossViewListFromCache(feature); break;
        case KernelFeatureId::DriverIntegrity:
        case KernelFeatureId::KernelCpuIntegrity: RebuildIntegrityEvidenceListFromCache(feature); break;
        default: RebuildKernelHookListFromCache(feature); break;
        }
        result_->nativeTreeHandles.resize(result_->treeNodes.size());
        for(std::size_t index=0;index<result_->treeNodes.size();++index) {
            const auto& node=result_->treeNodes[index];if(!node.state)continue;
            const auto key=KernelTreeNodeKey(*node.state);
            if(!input_->treeSelectedKey.empty() && key==input_->treeSelectedKey)result_->treeSelected=index+1;
            if(!input_->treeTopKey.empty() && key==input_->treeTopKey)result_->treeTop=index+1;
        }
        result_->choices=std::move(choices_);
        result_->columns=std::move(currentColumns_);
        result_->rows=std::move(currentRows_);
        result_->indents=std::move(currentRowIndents_);
        for(std::size_t index=0;index<result_->rows.size();++index) {
            result_->rowHashes[KernelProjectedRowHash(result_->rows[index])].push_back(index);
        }
        return result_;
    }
private:
    struct R0Values {
        KernelFeatureId featureId=KernelFeatureId::CpuHardwareSnapshot;
        std::vector<std::wstring> columns;
        Ksword::Ui::ImmutableTextRows rows;
        std::wstring statusText,selectedRowSignature;
        int topRow=0;
    };
    struct InsensitiveLess {
        bool operator()(const std::wstring& left,const std::wstring& right) const {return ::_wcsicmp(left.c_str(),right.c_str())<0;}
    };
    const std::vector<std::wstring>& Aliases(const KernelFeatureId feature,const std::wstring& column) {
        const auto key=std::make_pair(feature,column);
        auto found=aliases_.find(key);
        if(found==aliases_.end()) {
            auto aliases=ColumnAliases(feature,column);
            if(aliases.empty())aliases.push_back(column);
            found=aliases_.emplace(key,std::move(aliases)).first;
        }
        return found->second;
    }
    std::wstring ReadField(const std::vector<std::wstring>& row,const std::vector<std::wstring>& columns,
        std::initializer_list<const wchar_t*> names) {
        auto found=columnIndexes_.find(&columns);
        if(found==columnIndexes_.end()) {
            ColumnIndex index;
            for(std::size_t column=0;column<columns.size();++column) index[columns[column]].push_back(column);
            found=columnIndexes_.emplace(&columns,std::move(index)).first;
        }
        for(const auto* name:names) {
            const auto indexes=found->second.find(name);
            if(indexes==found->second.end())continue;
            for(const auto index:indexes->second)if(index<row.size()&&!row[index].empty())return row[index];
        }
        return {};
    }
    bool HasValue(const std::vector<std::wstring>& row,const std::vector<std::wstring>& columns,
        std::initializer_list<const wchar_t*> names){return !ReadField(row,columns,names).empty();}
    std::uint64_t ReadNonZero(const std::vector<std::wstring>& row,const std::vector<std::wstring>& columns,
        std::initializer_list<const wchar_t*> names){std::uint64_t value=0;return HexToUInt64(ReadField(row,columns,names),value)?value:0;}
    const std::wstring& Text(KernelValueControl control) const {
        return control.id==2 ? input_->moduleFilter : input_->filter;
    }
    int Checked(KernelValueControl) const {return input_->riskOnly?BST_CHECKED:BST_UNCHECKED;}
    void WriteText(KernelValueControl control,const wchar_t* text) {
        if(control.id==3){result_->status=text?text:L"";result_->hasStatus=true;}
        if(control.id==4){result_->detail=text?text:L"";result_->hasDetail=true;}
    }
    void SetColumn(int index,const std::wstring&,int width) {
        if(index>=0){if(result_->widths.size()<=static_cast<std::size_t>(index))result_->widths.resize(static_cast<std::size_t>(index)+1);result_->widths[static_cast<std::size_t>(index)]=width;}
    }
    std::array<KernelProjectedChoice,4> choices_;
    const std::wstring& ChoiceText(KernelValueControl control) const {
        static const std::wstring empty;
        if(control.id<10 || control.id>13)return empty;
        const auto& choice=choices_[static_cast<std::size_t>(control.id-10)];
        return choice.selected>=0 && static_cast<std::size_t>(choice.selected)<choice.items.size()?choice.items[static_cast<std::size_t>(choice.selected)]:empty;
    }
    LRESULT ChoiceMessage(KernelValueControl control,UINT message,WPARAM parameter,LPARAM text) {
        if(control.id<10 || control.id>13)return CB_ERR;
        auto& choice=choices_[static_cast<std::size_t>(control.id-10)];
        switch(message) {
        case CB_GETCURSEL:return choice.selected;
        case CB_RESETCONTENT:choice.items.clear();choice.selected=-1;choice.changed=true;return 0;
        case CB_ADDSTRING:choice.items.emplace_back(reinterpret_cast<const wchar_t*>(text));return static_cast<LRESULT>(choice.items.size()-1);
        case CB_SETCURSEL:choice.selected=static_cast<int>(parameter);return choice.selected;
        default:return CB_ERR;
        }
    }
    void RequireTreeValueModel() {RebuildObjectNamespaceTreeFromCurrentRows();}
    void ExpandTreeValue(std::size_t item){if(item>0&&item<=result_->treeNodes.size())result_->treeNodes[item-1].expand=true;}
    void SelectTreeValue(std::size_t item){result_->treeSelected=item;}
    const KernelFeatureDescriptor* CurrentDescriptor() const {return &input_->descriptor;}
    std::vector<int> CurrentCopyColumnIndices() const {
        auto columns=PreferredCopyColumnIndices(input_->source->featureId,currentColumns_);
        if(columns.empty())for(std::size_t index=0;index<currentColumns_.size();++index)columns.push_back(static_cast<int>(index));
        return columns;
    }
    void AppendSummary(KernelValueControl,std::vector<std::wstring> row){result_->summary.push_back(std::move(row));}
    void ClearValueList(KernelValueControl){result_->summary.clear();}
    void ClearTables(){result_->widths.clear();result_->summary.clear();}
    void CommitRows(){}
    void SetSelection(KernelValueControl,int,unsigned,unsigned){result_->selectFirst=true;}
    void EnsureItem(KernelValueControl,int,bool){}
    void SelectionDetail(){result_->selectionDetail=true;}
    std::shared_ptr<const KernelProjectionInput> input_;
    std::shared_ptr<KernelValueProjection> result_;
    std::vector<std::wstring> currentColumns_,currentRawColumns_;
    std::shared_ptr<const KernelRawRows> currentRawRows_;
    Ksword::Ui::ImmutableTextRows currentRows_;
    std::vector<int> currentRowIndents_;
    using ColumnIndex=std::map<std::wstring,std::vector<std::size_t>,InsensitiveLess>;
    std::unordered_map<const std::vector<std::wstring>*,ColumnIndex> columnIndexes_;
    std::map<std::pair<KernelFeatureId,std::wstring>,std::vector<std::wstring>> aliases_;
    const KernelValueControl resultList_{5},filterEdit_{1},moduleFilterEdit_{2},statusText_{3},detailEdit_{4},riskOnlyCheck_{6},summaryList_{7};
    const KernelValueControl deviceDriverDirectoryCombo_{input_->choicePresent[0]?10:0},deviceDriverTypeCombo_{input_->choicePresent[1]?11:0},
        baseNamedScopeCombo_{input_->choicePresent[2]?12:0},baseNamedTypeCombo_{input_->choicePresent[3]?13:0};
void RebuildKernelHookListFromCache(const KernelFeatureId featureId) {
    // RebuildKernelHookListFromCache mirrors the original SSDT/SSSDT/Inline
    // Hook/IAT-EAT tables. Inputs are cached raw rows from KernelFacade plus
    // local filter controls; processing projects only the original visible
    // columns while preserving hidden protocol/detail cells for actions; output
    // is the ListView, detail pane, and status text.
    if (!resultList_ || !IsKernelHookFeature(featureId)) {
        return;
    }

    const std::vector<std::wstring>& rawColumns = !currentRawColumns_.empty() ? currentRawColumns_ : currentColumns_;
    const std::vector<std::vector<std::wstring>>& rawRows = !currentRawRows_->empty() ? *currentRawRows_ : currentRows_;
    const std::wstring filterText = Text(filterEdit_);
    const std::wstring moduleFilterText = Text(moduleFilterEdit_);
    const auto rawValue = [&](const std::vector<std::wstring>& row, std::initializer_list<const wchar_t*> names) -> std::wstring {
        return ReadField(row, rawColumns, names);
    };
    const auto addColumnIfMissing = [](std::vector<std::wstring>& columns, const wchar_t* name) {
        if (!HasColumn(columns, name)) {
            columns.push_back(name);
        }
    };
    const auto displayValue = [&](const std::vector<std::wstring>& row, const std::wstring& columnName) -> std::wstring {
        const auto& aliases = Aliases(featureId, columnName);
        for (const std::wstring& alias : aliases) {
            const std::wstring value = rawValue(row, { alias.c_str() });
            if (!value.empty()) {
                return value;
            }
        }
        return {};
    };
    const auto isMeaningful = [&](const std::vector<std::wstring>& row) {
        switch (featureId) {
        case KernelFeatureId::Ssdt:
        case KernelFeatureId::ShadowSsdt:
            return HasValue(row, rawColumns, { L"Index", L"Name", L"ServiceName", L"Zw", L"Stub", L"Service", L"ServiceAddress" });
        case KernelFeatureId::InlineHook:
            return HasValue(row, rawColumns, { L"Module", L"Function", L"Address", L"Target", L"CurrentBytes", L"DiskBytes" });
        case KernelFeatureId::IatEatHook:
            return HasValue(row, rawColumns, { L"ClassText", L"Class", L"Module", L"Import", L"Function", L"Thunk", L"Current", L"Expected" });
        default:
            return false;
        }
    };
    const auto hiddenColumn = [&](const std::wstring& name) {
        if (HasColumn(CanonicalColumnNames(featureId), name)) {
            return false;
        }
        return true;
    };

    std::vector<std::wstring> displayColumns;
    for (const std::wstring& column : CanonicalColumnNames(featureId)) {
        addColumnIfMissing(displayColumns, column.c_str());
    }
    for (const std::wstring& column : rawColumns) {
        addColumnIfMissing(displayColumns, column.c_str());
    }
    addColumnIfMissing(displayColumns, L"Detail");

    std::vector<std::vector<std::wstring>> displayRows;
    std::size_t totalRows = 0;
    std::size_t unresolvedRows = 0;
    std::size_t suspiciousRows = 0;
    std::size_t cleanRows = 0;
    std::size_t diskDiffRows = 0;
    std::size_t reportedTotalRows = 0;
    std::size_t reportedReturnedRows = 0;
    for (const std::vector<std::wstring>& rawRow : rawRows) {
        const std::uint64_t rawTotal = ReadNonZero(rawRow, rawColumns, { L"Total" });
        const std::uint64_t rawReturned = ReadNonZero(rawRow, rawColumns, { L"Returned" });
        if (rawTotal != 0) {
            reportedTotalRows = static_cast<std::size_t>(rawTotal);
        }
        if (rawReturned != 0) {
            reportedReturnedRows = static_cast<std::size_t>(rawReturned);
        }
        if (!isMeaningful(rawRow)) {
            continue;
        }
        ++totalRows;

        std::vector<std::wstring> cells(displayColumns.size());
        for (std::size_t column = 0; column < displayColumns.size(); ++column) {
            const std::wstring& name = displayColumns[column];
            if (name == L"Detail") {
                cells[column] = rawValue(rawRow, { L"Detail", L"FlagsText", L"DiskDiff", L"Status" });
            } else {
                cells[column] = displayValue(rawRow, name);
            }
        }

        const std::wstring merged = MergeRowText(cells) + L" | " + rawValue(rawRow, { L"Detail" });
        if (!filterText.empty() && !ContainsCaseInsensitive(merged, filterText)) {
            continue;
        }
        if ((featureId == KernelFeatureId::InlineHook || featureId == KernelFeatureId::IatEatHook) && !moduleFilterText.empty()) {
            const std::wstring moduleText =
                ReadField(cells, displayColumns, { L"模块", L"Module" }) + L" | " +
                ReadField(cells, displayColumns, { L"目标模块", L"TargetModule" }) + L" | " +
                ReadField(cells, displayColumns, { L"导入模块", L"Import", L"ImportModule" });
            if (!ContainsCaseInsensitive(moduleText, moduleFilterText)) {
                continue;
            }
        }

        const std::wstring status = ReadField(cells, displayColumns, { L"状态", L"Status" });
        const std::wstring diff = ReadField(cells, displayColumns, { L"差异状态", L"差异", L"DiskDiff" });
        if (ContainsCaseInsensitive(status, L"未解析") ||
            ContainsCaseInsensitive(status, L"unresolved") ||
            ReadField(cells, displayColumns, { L"索引", L"Index" }) == L"<未知>") {
            ++unresolvedRows;
        }
        if (ContainsCaseInsensitive(status, L"Hook") ||
            ContainsCaseInsensitive(status, L"可疑") ||
            ContainsCaseInsensitive(status, L"异常") ||
            ContainsCaseInsensitive(status, L"outside") ||
            ContainsCaseInsensitive(diff, L"不同") ||
            ContainsCaseInsensitive(diff, L"diff")) {
            ++suspiciousRows;
        } else if (!status.empty()) {
            ++cleanRows;
        }
        if (ContainsCaseInsensitive(diff, L"不同") || ContainsCaseInsensitive(diff, L"diff")) {
            ++diskDiffRows;
        }
        displayRows.push_back(std::move(cells));
    }

    currentColumns_ = std::move(displayColumns);
    currentRows_ = std::move(displayRows);
    ClearTables();
    for (std::size_t index = 0; index < currentColumns_.size(); ++index) {
        int width = hiddenColumn(currentColumns_[index]) ? 0 : ColumnWidth(currentColumns_[index]);
        if (currentColumns_[index] == L"索引") {
            width = 90;
        } else if (currentColumns_[index] == L"服务名" || currentColumns_[index] == L"函数") {
            width = 260;
        } else if (currentColumns_[index] == L"Zw导出地址" ||
            currentColumns_[index] == L"Stub地址" ||
            currentColumns_[index] == L"表项地址" ||
            currentColumns_[index] == L"服务例程" ||
            currentColumns_[index] == L"函数地址" ||
            currentColumns_[index] == L"目标地址" ||
            currentColumns_[index] == L"Thunk/EAT项" ||
            currentColumns_[index] == L"当前目标" ||
            currentColumns_[index] == L"期望目标") {
            width = 170;
        } else if (currentColumns_[index] == L"模块" ||
            currentColumns_[index] == L"导入模块" ||
            currentColumns_[index] == L"目标模块") {
            width = 190;
        } else if (currentColumns_[index] == L"内存字节" ||
            currentColumns_[index] == L"磁盘字节") {
            width = 240;
        } else if (currentColumns_[index] == L"差异状态" ||
            currentColumns_[index] == L"状态") {
            width = 180;
        }
        SetColumn(static_cast<int>(index), currentColumns_[index], width);
    }
    CommitRows();

    std::wostringstream status;
    switch (featureId) {
    case KernelFeatureId::Ssdt:
        status << L"状态：已刷新 " << totalRows << L" 项，显示 " << currentRows_.size()
               << L" 项，未解析索引 " << unresolvedRows << L" 项";
        break;
    case KernelFeatureId::ShadowSsdt:
        status << L"状态：已解析 "
               << (reportedReturnedRows == 0 ? totalRows : reportedReturnedRows)
               << L"/"
               << (reportedTotalRows == 0 ? totalRows : reportedTotalRows)
               << L" 项，显示 " << currentRows_.size()
               << L" 项，未解析索引 " << unresolvedRows << L" 项";
        break;
    case KernelFeatureId::InlineHook:
        status << L"状态：Inline Hook 扫描完成，显示 " << currentRows_.size()
               << L" / " << totalRows << L" 项，可疑/差异 " << suspiciousRows
               << L" 项，磁盘差异 " << diskDiffRows << L" 项";
        break;
    case KernelFeatureId::IatEatHook:
        status << L"状态：IAT/EAT 扫描完成，显示 " << currentRows_.size()
               << L" / " << totalRows << L" 项，可疑 " << suspiciousRows
               << L" 项，干净 " << cleanRows << L" 项";
        break;
    default:
        break;
    }
    if (!filterText.empty()) {
        status << L"，过滤=" << filterText;
    }
    if (!moduleFilterText.empty() && (featureId == KernelFeatureId::InlineHook || featureId == KernelFeatureId::IatEatHook)) {
        status << L"，模块过滤=" << moduleFilterText;
    }
    WriteText(statusText_, status.str().c_str());

    if (!currentRows_.empty()) {
        SetSelection(resultList_, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        EnsureItem(resultList_, 0, FALSE);
        SelectionDetail();
    } else {
        WriteText(detailEdit_, EmptyDetailTextForFeature(featureId));
    }
}

void RebuildAtomTableFromCache() {
    // RebuildAtomTableFromCache mirrors the original KernelDock atom page. The
    // input is the unfiltered R3 Win32 atom snapshot cached by RenderResult; the
    // processing projects the fixed Atom值/十六进制/名称/来源/状态 table and applies
    // the edit-box filter locally; the return behavior is direct ListView and
    // detail-pane updates with no extra driver/native calls.
    if (!resultList_) {
        return;
    }

    const std::vector<std::wstring>& rawColumns = !currentRawColumns_.empty() ? currentRawColumns_ : currentColumns_;
    const std::vector<std::vector<std::wstring>>& rawRows = !currentRawRows_->empty() ? *currentRawRows_ : currentRows_;
    const std::wstring filterText = Text(filterEdit_);
    const auto rawValue = [&](const std::vector<std::wstring>& row, std::initializer_list<const wchar_t*> names) -> std::wstring {
        return ReadField(row, rawColumns, names);
    };
    const auto displayValue = [&](const std::vector<std::wstring>& row, const std::wstring& columnName) -> std::wstring {
        const auto& aliases = Aliases(KernelFeatureId::AtomTable, columnName);
        for (const std::wstring& alias : aliases) {
            const std::wstring value = rawValue(row, { alias.c_str() });
            if (!value.empty()) {
                return value;
            }
        }
        return {};
    };

    std::vector<std::wstring> displayColumns{ L"#" };
    for (const std::wstring& column : CanonicalColumnNames(KernelFeatureId::AtomTable)) {
        if (!HasColumn(displayColumns, column)) {
            displayColumns.push_back(column);
        }
    }
    const wchar_t* hiddenColumns[] = {
        L"Id", L"Hex", L"Name", L"Source", L"Kind", L"GlobalName", L"ClipboardName", L"Length", L"Detail"
    };
    for (const wchar_t* column : hiddenColumns) {
        if (!HasColumn(displayColumns, column)) {
            displayColumns.push_back(column);
        }
    }

    std::vector<std::vector<std::wstring>> displayRows;
    std::size_t totalRows = 0;
    for (const std::vector<std::wstring>& rawRow : rawRows) {
        const bool atomRow = HasValue(rawRow, rawColumns, { L"Id", L"Name", L"Source", L"Kind" });
        if (!atomRow) {
            continue;
        }
        ++totalRows;
        if (!filterText.empty() && !ContainsCaseInsensitive(MergeRowText(rawRow), filterText)) {
            continue;
        }

        std::vector<std::wstring> cells(displayColumns.size());
        for (std::size_t column = 0; column < displayColumns.size(); ++column) {
            const std::wstring& name = displayColumns[column];
            if (name == L"#") {
                cells[column] = std::to_wstring(displayRows.size());
            } else if (name == L"Detail") {
                cells[column] = rawValue(rawRow, { L"Detail" });
            } else {
                cells[column] = displayValue(rawRow, name);
            }
        }
        displayRows.push_back(std::move(cells));
    }

    currentColumns_ = std::move(displayColumns);
    currentRows_ = std::move(displayRows);
    currentRowIndents_.clear();
    ClearTables();
    const std::vector<std::wstring> canonical = CanonicalColumnNames(KernelFeatureId::AtomTable);
    for (std::size_t index = 0; index < currentColumns_.size(); ++index) {
        const std::wstring& name = currentColumns_[index];
        const bool hidden = name == L"#" ? false : !HasColumn(canonical, name);
        int width = hidden ? 0 : ColumnWidth(name);
        if (name == L"Atom值" || name == L"十六进制") {
            width = 110;
        } else if (name == L"名称") {
            width = 360;
        } else if (name == L"来源") {
            width = 220;
        } else if (name == L"状态") {
            width = 160;
        }
        SetColumn(static_cast<int>(index), name, width);
    }
    CommitRows();

    std::wostringstream status;
    status << L"状态：原子表已刷新，返回 " << currentRows_.size() << L"/" << totalRows;
    WriteText(statusText_, status.str().c_str());
    if (!currentRows_.empty()) {
        SetSelection(resultList_, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        EnsureItem(resultList_, 0, FALSE);
        SelectionDetail();
    } else {
        WriteText(detailEdit_, totalRows == 0 ? L"尚未返回原子记录。" : L"当前过滤条件下没有原子记录。");
    }
}

void RebuildKernelMemoryScanListFromCache(const KernelFeatureId featureId) {
    // RebuildKernelMemoryScanListFromCache mirrors the original MemoryDock
    // kernel executable/evidence grids. Inputs are raw ArkDriverClient rows
    // cached by RenderResult plus current Win32 filter controls; processing
    // projects only original visible columns while retaining hidden R0 fields for
    // details/actions; return behavior is UI state only.
    if (!resultList_) {
        return;
    }

    const std::vector<std::wstring>& rawColumns = !currentRawColumns_.empty() ? currentRawColumns_ : currentColumns_;
    const std::vector<std::vector<std::wstring>>& rawRows = !currentRawRows_->empty() ? *currentRawRows_ : currentRows_;
    const bool executablePage = featureId == KernelFeatureId::KernelExecutableMemory;
    const bool riskOnly = riskOnlyCheck_ && Checked(riskOnlyCheck_) == BST_CHECKED;
    const std::wstring primaryFilter = executablePage ? Text(moduleFilterEdit_) : Text(filterEdit_);
    const auto rawValue = [&](const std::vector<std::wstring>& row, std::initializer_list<const wchar_t*> names) -> std::wstring {
        return ReadField(row, rawColumns, names);
    };
    const auto addColumnIfMissing = [](std::vector<std::wstring>& columns, const wchar_t* name) {
        if (!HasColumn(columns, name)) {
            columns.push_back(name);
        }
    };
    const auto displayValue = [&](const std::vector<std::wstring>& row, const std::wstring& columnName) -> std::wstring {
        const auto& aliases = Aliases(featureId, columnName);
        for (const std::wstring& alias : aliases) {
            const std::wstring value = rawValue(row, { alias.c_str() });
            if (!value.empty()) {
                return value;
            }
        }
        return {};
    };

    std::vector<std::wstring> displayColumns;
    displayColumns.push_back(L"#");
    for (const std::wstring& column : CanonicalColumnNames(featureId)) {
        if (!HasColumn(displayColumns, column)) {
            displayColumns.push_back(column);
        }
    }
    const wchar_t* hiddenExecutable[] = {
        L"RegionSize", L"Perm", L"Risk", L"OwnerKind", L"OwnerKindText", L"OwnerAddress",
        L"ModuleBase", L"ModuleSize", L"Module", L"Status", L"LastStatus", L"Detail"
    };
    const wchar_t* hiddenEvidence[] = {
        L"Kind", L"RegionSize", L"PageSize", L"Perm", L"Risk", L"OwnerKind", L"OwnerKindText",
        L"OwnerAddress", L"ModuleBase", L"ModuleSize", L"ModuleSizeText", L"Confidence",
        L"BigPoolTag", L"BigPoolFlags", L"SectionRva", L"SectionSize", L"SectionSizeText",
        L"Section", L"HashAlgorithm", L"SampleSize", L"Hash", L"HashText", L"Sample",
        L"LastStatus", L"Detail"
    };
    if (executablePage) {
        for (const wchar_t* column : hiddenExecutable) {
            addColumnIfMissing(displayColumns, column);
        }
    } else {
        for (const wchar_t* column : hiddenEvidence) {
            addColumnIfMissing(displayColumns, column);
        }
    }

    std::vector<std::vector<std::wstring>> displayRows;
    std::size_t totalRows = 0;
    std::size_t returnedRows = 0;
    std::size_t reportedModuleRows = 0;
    std::size_t reportedBigPoolRows = 0;
    std::size_t moduleRows = 0;
    std::size_t bigPoolRows = 0;
    std::size_t riskRows = 0;
    for (const std::vector<std::wstring>& rawRow : rawRows) {
        const std::uint64_t total = ReadNonZero(rawRow, rawColumns, { L"Total" });
        const std::uint64_t returned = ReadNonZero(rawRow, rawColumns, { L"Returned" });
        const std::uint64_t modules = ReadNonZero(rawRow, rawColumns, { L"Modules" });
        const std::uint64_t bigPool = ReadNonZero(rawRow, rawColumns, { L"BigPoolRows" });
        if (total != 0) {
            totalRows = static_cast<std::size_t>(total);
        }
        if (returned != 0) {
            returnedRows = static_cast<std::size_t>(returned);
        }
        if (modules != 0) {
            reportedModuleRows = static_cast<std::size_t>(modules);
        }
        if (bigPool != 0) {
            reportedBigPoolRows = static_cast<std::size_t>(bigPool);
        }
    }
    for (const std::vector<std::wstring>& rawRow : rawRows) {
        const bool isDataRow = executablePage
            ? HasValue(rawRow, rawColumns, { L"VA", L"Pages", L"PermText", L"RiskText", L"OwnerDisplay", L"ModulePath" })
            : HasValue(rawRow, rawColumns, { L"VA", L"KindText", L"RiskText", L"OwnerDisplay", L"HashText" });
        if (!isDataRow) {
            continue;
        }
        if (totalRows == 0) {
            ++returnedRows;
        }
        const std::wstring riskText = rawValue(rawRow, { L"RiskText", L"风险", L"风险标志" });
        const std::wstring riskValue = rawValue(rawRow, { L"Risk" });
        const bool risky = !riskText.empty() &&
            !ContainsCaseInsensitive(riskText, L"正常") &&
            !ContainsCaseInsensitive(riskValue, L"0x0");
        if (risky) {
            ++riskRows;
        }
        if (riskOnly && !risky) {
            continue;
        }

        std::wstring merged = MergeRowText(rawRow);
        if (executablePage) {
            merged += L" | " + rawValue(rawRow, { L"ModulePath", L"Module" });
            if (!primaryFilter.empty() && !ContainsCaseInsensitive(merged, primaryFilter)) {
                continue;
            }
            if (!rawValue(rawRow, { L"ModulePath", L"Module" }).empty()) {
                ++moduleRows;
            }
        } else {
            if (!primaryFilter.empty() && !ContainsCaseInsensitive(merged, primaryFilter)) {
                continue;
            }
            if (ContainsCaseInsensitive(rawValue(rawRow, { L"KindText", L"类型" }), L"BigPool")) {
                ++bigPoolRows;
            }
            if (!rawValue(rawRow, { L"ModuleBase" }).empty()) {
                ++moduleRows;
            }
        }

        std::vector<std::wstring> cells(displayColumns.size());
        for (std::size_t column = 0; column < displayColumns.size(); ++column) {
            const std::wstring& name = displayColumns[column];
            if (name == L"#") {
                cells[column] = std::to_wstring(displayRows.size());
            } else if (name == L"Detail") {
                cells[column] = rawValue(rawRow, { L"Detail" });
            } else {
                cells[column] = displayValue(rawRow, name);
            }
        }
        displayRows.push_back(std::move(cells));
    }

    currentColumns_ = std::move(displayColumns);
    currentRows_ = std::move(displayRows);
    ClearTables();
    for (std::size_t index = 0; index < currentColumns_.size(); ++index) {
        int width = IsKernelMemoryHiddenColumn(currentColumns_[index]) ? 0 : ColumnWidth(currentColumns_[index]);
        if (currentColumns_[index] == L"VA") {
            width = 170;
        } else if (currentColumns_[index] == L"页数" || currentColumns_[index] == L"页大小" || currentColumns_[index] == L"大小") {
            width = 82;
        } else if (currentColumns_[index] == L"权限" || currentColumns_[index] == L"PTE权限") {
            width = 160;
        } else if (currentColumns_[index] == L"Owner") {
            width = 190;
        } else if (currentColumns_[index] == L"模块路径") {
            width = 320;
        } else if (currentColumns_[index] == L"风险标志" || currentColumns_[index] == L"风险") {
            width = 210;
        } else if (currentColumns_[index] == L"text hash/diff") {
            width = 240;
        }
        SetColumn(static_cast<int>(index), currentColumns_[index], width);
    }
    CommitRows();

    std::wostringstream status;
    if (executablePage) {
        const std::size_t effectiveTotal = totalRows == 0 ? returnedRows : totalRows;
        const std::size_t effectiveModules = reportedModuleRows == 0 ? moduleRows : reportedModuleRows;
        status << L"状态：总计 " << effectiveTotal
               << L"，显示 " << currentRows_.size()
               << L"，模块 " << effectiveModules
               << L"，风险项 " << riskRows;
    } else {
        const std::size_t effectiveTotal = totalRows == 0 ? returnedRows : totalRows;
        const std::size_t effectiveReturned = returnedRows == 0 ? effectiveTotal : returnedRows;
        const std::size_t effectiveModules = reportedModuleRows == 0 ? moduleRows : reportedModuleRows;
        const std::size_t effectiveBigPool = reportedBigPoolRows == 0 ? bigPoolRows : reportedBigPoolRows;
        status << L"状态：总计 " << effectiveTotal
               << L"，返回 " << effectiveReturned
               << L"，显示 " << currentRows_.size()
               << L"，模块 " << effectiveModules
               << L"，BigPool seen " << effectiveBigPool;
    }
    WriteText(statusText_, status.str().c_str());
    if (!currentRows_.empty()) {
        SetSelection(resultList_, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        EnsureItem(resultList_, 0, FALSE);
        SelectionDetail();
    } else {
        WriteText(detailEdit_, executablePage
            ? L"当前过滤条件下没有内核可执行页记录。"
            : L"当前过滤条件下没有内核内存证据记录。");
    }
}

void RebuildCrossViewListFromCache(const KernelFeatureId featureId) {
    // RebuildCrossViewListFromCache mirrors the original ProcessDock CrossView
    // matrix table. Inputs are raw R0 rows cached by RenderResult and the local
    // filter edit; processing keeps the visible matrix columns identical while
    // preserving offset/source fields for details; output is ListView/detail UI.
    if (!resultList_) {
        return;
    }

    const std::vector<std::wstring>& rawColumns = !currentRawColumns_.empty() ? currentRawColumns_ : currentColumns_;
    const std::vector<std::vector<std::wstring>>& rawRows = !currentRawRows_->empty() ? *currentRawRows_ : currentRows_;
    const std::wstring filterText = Text(filterEdit_);
    const auto rawValue = [&](const std::vector<std::wstring>& row, std::initializer_list<const wchar_t*> names) -> std::wstring {
        return ReadField(row, rawColumns, names);
    };
    const auto displayValue = [&](const std::vector<std::wstring>& row, const std::wstring& columnName) -> std::wstring {
        const auto& aliases = Aliases(featureId, columnName);
        for (const std::wstring& alias : aliases) {
            const std::wstring value = rawValue(row, { alias.c_str() });
            if (!value.empty()) {
                return value;
            }
        }
        return {};
    };
    const auto addColumnIfMissing = [](std::vector<std::wstring>& columns, const wchar_t* name) {
        if (!HasColumn(columns, name)) {
            columns.push_back(name);
        }
    };

    std::vector<std::wstring> displayColumns;
    for (const std::wstring& column : CanonicalColumnNames(featureId)) {
        addColumnIfMissing(displayColumns, column.c_str());
    }
    const wchar_t* hiddenColumns[] = {
        L"PID", L"PPID", L"TID", L"Image", L"Object", L"ProcessObject", L"ThreadObject",
        L"Start", L"StartAddress", L"SourceMask", L"SourceText", L"Anomaly", L"AnomalyText",
        L"DynData", L"DynDataCapabilityMask", L"EP.UniqueProcessId", L"EP.ActiveProcessLinks",
        L"EP.ThreadListHead", L"EP.ImageFileName", L"ET.Cid", L"ET.ThreadListEntry",
        L"ET.StartAddress", L"ET.Win32StartAddress", L"KT.Process", L"HT.TableCode",
        L"HTE.LowValue", L"PspCidTableRva", L"PspCidTable", L"Confidence", L"LastStatus"
    };
    for (const wchar_t* column : hiddenColumns) {
        addColumnIfMissing(displayColumns, column);
    }

    std::size_t reportedTotalRows = 0;
    std::size_t reportedReturnedRows = 0;
    std::wstring missingDynText;
    for (const std::vector<std::wstring>& rawRow : rawRows) {
        const std::uint64_t total = ReadNonZero(rawRow, rawColumns, { L"Total" });
        const std::uint64_t returned = ReadNonZero(rawRow, rawColumns, { L"Returned" });
        if (total != 0) {
            reportedTotalRows = static_cast<std::size_t>(total);
        }
        if (returned != 0) {
            reportedReturnedRows = static_cast<std::size_t>(returned);
        }
        if (missingDynText.empty()) {
            missingDynText = rawValue(rawRow, { L"MissingDyn", L"MissingDynData", L"MissingCapabilityMask" });
        }
    }

    std::vector<std::vector<std::wstring>> displayRows;
    std::size_t totalRows = 0;
    std::size_t anomalyRows = 0;
    std::size_t cidOnlyRows = 0;
    const bool anomalyOnly = riskOnlyCheck_ && Checked(riskOnlyCheck_) == BST_CHECKED;
    for (const std::vector<std::wstring>& rawRow : rawRows) {
        const bool rowLooksLikeProcess = featureId == KernelFeatureId::ProcessCrossView &&
            HasValue(rawRow, rawColumns, { L"PID", L"ProcessObject", L"Object", L"Image" });
        const bool rowLooksLikeThread = featureId == KernelFeatureId::ThreadCrossView &&
            HasValue(rawRow, rawColumns, { L"TID", L"ThreadObject", L"Object", L"ProcessObject" });
        if (!rowLooksLikeProcess && !rowLooksLikeThread) {
            continue;
        }
        ++totalRows;
        std::uint64_t anomalyMask = 0;
        const bool parsedAnomalyMask = HexToUInt64(rawValue(rawRow, { L"Anomaly" }), anomalyMask);
        const std::wstring anomaly = rawValue(rawRow, { L"异常", L"AnomalyText" });
        const bool anomalous = parsedAnomalyMask
            ? anomalyMask != 0
            : (!anomaly.empty() && !ContainsCaseInsensitive(anomaly, L"正常") && !ContainsCaseInsensitive(anomaly, L"0x0"));
        if (anomalous) {
            ++anomalyRows;
        }
        if (anomalyOnly && !anomalous) {
            continue;
        }
        const std::wstring merged = MergeRowText(rawRow);
        if (!filterText.empty() && !ContainsCaseInsensitive(merged, filterText)) {
            continue;
        }
        const std::wstring publicWalk = rawValue(rawRow, { L"PublicWalk" });
        const std::wstring active = rawValue(rawRow, { L"Active/ThreadList" });
        const std::wstring cid = rawValue(rawRow, { L"CID" });
        if (ContainsCaseInsensitive(cid, L"是") &&
            !ContainsCaseInsensitive(publicWalk, L"是") &&
            !ContainsCaseInsensitive(active, L"是")) {
            ++cidOnlyRows;
        }

        std::vector<std::wstring> cells(displayColumns.size());
        for (std::size_t column = 0; column < displayColumns.size(); ++column) {
            const std::wstring& name = displayColumns[column];
            if (name == L"Detail") {
                cells[column] = rawValue(rawRow, { L"Detail" });
            } else {
                cells[column] = displayValue(rawRow, name);
            }
        }
        displayRows.push_back(std::move(cells));
    }

    currentColumns_ = std::move(displayColumns);
    currentRows_ = std::move(displayRows);
    ClearTables();
    for (std::size_t index = 0; index < currentColumns_.size(); ++index) {
        const std::wstring& name = currentColumns_[index];
        const bool hidden = name == L"PID" ||
            name == L"PPID" ||
            name == L"TID" ||
            name == L"Image" ||
            name == L"Object" ||
            name == L"ProcessObject" ||
            name == L"ThreadObject" ||
            name == L"Start" ||
            name == L"StartAddress" ||
            name == L"SourceMask" ||
            name == L"SourceText" ||
            name == L"Anomaly" ||
            name == L"AnomalyText" ||
            name == L"DynData" ||
            name == L"DynDataCapabilityMask" ||
            name == L"EP.UniqueProcessId" ||
            name == L"EP.ActiveProcessLinks" ||
            name == L"EP.ThreadListHead" ||
            name == L"EP.ImageFileName" ||
            name == L"ET.Cid" ||
            name == L"ET.ThreadListEntry" ||
            name == L"ET.StartAddress" ||
            name == L"ET.Win32StartAddress" ||
            name == L"KT.Process" ||
            name == L"HT.TableCode" ||
            name == L"HTE.LowValue" ||
            name == L"PspCidTableRva" ||
            name == L"PspCidTable" ||
            name == L"Confidence" ||
            name == L"LastStatus";
        int width = hidden ? 0 : ColumnWidth(name);
        if (name == L"ID") {
            width = 84;
        } else if (name == L"对象") {
            width = 170;
        } else if (name == L"进程") {
            width = 180;
        } else if (name == L"PublicWalk" || name == L"Active/ThreadList" || name == L"CID") {
            width = 92;
        } else if (name == L"异常") {
            width = 210;
        } else if (name == L"置信度") {
            width = 72;
        } else if (name == L"Detail") {
            width = 300;
        }
        SetColumn(static_cast<int>(index), name, width);
    }
    CommitRows();

    std::wostringstream status;
    const std::size_t effectiveTotal = reportedTotalRows == 0 ? totalRows : reportedTotalRows;
    const std::size_t effectiveReturned = reportedReturnedRows == 0 ? totalRows : reportedReturnedRows;
    status << L"状态：" << (featureId == KernelFeatureId::ProcessCrossView ? L"进程" : L"线程")
           << L" Cross-View 已刷新，显示 " << currentRows_.size()
           << L"，返回 " << effectiveReturned << L"/" << effectiveTotal
           << L"，异常 " << anomalyRows << L"，CID-only " << cidOnlyRows
           << L"，missingCaps=" << (missingDynText.empty() ? L"0x0" : missingDynText);
    WriteText(statusText_, status.str().c_str());
    if (!currentRows_.empty()) {
        SetSelection(resultList_, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        EnsureItem(resultList_, 0, FALSE);
        SelectionDetail();
    } else {
        WriteText(detailEdit_, L"当前过滤条件下没有 Cross-View 记录。");
    }
}

void RebuildIntegrityEvidenceListFromCache(const KernelFeatureId featureId) {
    // RebuildIntegrityEvidenceListFromCache mirrors the original DriverDock and
    // HardwareDock integrity evidence tables. Inputs are raw R0 rows cached by
    // RenderResult and the local filter edit; processing projects the original
    // visible columns while retaining raw protocol fields for detail/actions;
    // output is the updated ListView, status text, and detail pane selection.
    if (!resultList_) {
        return;
    }

    const std::vector<std::wstring>& rawColumns = !currentRawColumns_.empty() ? currentRawColumns_ : currentColumns_;
    const std::vector<std::vector<std::wstring>>& rawRows = !currentRawRows_->empty() ? *currentRawRows_ : currentRows_;
    const std::wstring filterText = Text(filterEdit_);
    const bool cpuPage = featureId == KernelFeatureId::KernelCpuIntegrity;
    const auto rawValue = [&](const std::vector<std::wstring>& row, std::initializer_list<const wchar_t*> names) -> std::wstring {
        return ReadField(row, rawColumns, names);
    };
    const auto addColumnIfMissing = [](std::vector<std::wstring>& columns, const wchar_t* name) {
        if (!HasColumn(columns, name)) {
            columns.push_back(name);
        }
    };
    const auto displayValue = [&](const std::vector<std::wstring>& row, const std::wstring& columnName) -> std::wstring {
        const auto& aliases = Aliases(featureId, columnName);
        for (const std::wstring& alias : aliases) {
            const std::wstring value = rawValue(row, { alias.c_str() });
            if (!value.empty()) {
                return value;
            }
        }
        return {};
    };

    std::vector<std::wstring> displayColumns;
    displayColumns.push_back(L"#");
    for (const std::wstring& column : CanonicalColumnNames(featureId)) {
        addColumnIfMissing(displayColumns, column.c_str());
    }
    const wchar_t* hiddenColumns[] = {
        L"Class", L"ClassText", L"Risk", L"RiskText", L"Source", L"SourceMask", L"SourceText",
        L"Confidence", L"Group", L"CPU", L"Vector", L"CpuVector", L"Object", L"ObjectAddress",
        L"Target", L"TargetAddress", L"OwnerBase", L"OwnerModuleBase", L"OwnerSize",
        L"OwnerModuleSize", L"OwnerModuleSizeText", L"OwnerModule", L"LastStatus"
    };
    for (const wchar_t* column : hiddenColumns) {
        addColumnIfMissing(displayColumns, column);
    }

    std::vector<std::vector<std::wstring>> displayRows;
    std::size_t reportedTotalRows = 0;
    std::size_t reportedReturnedRows = 0;
    std::size_t reportedCpuRows = 0;
    std::size_t reportedModuleRows = 0;
    std::size_t totalRows = 0;
    std::size_t riskRows = 0;
    std::wstring r0State;
    std::wstring r0Protocol;
    std::wstring dynDataState;
    std::wstring queryStatus;
    std::wstring sourceText;
    for (const std::vector<std::wstring>& rawRow : rawRows) {
        const std::uint64_t total = ReadNonZero(rawRow, rawColumns, { L"Total" });
        const std::uint64_t returned = ReadNonZero(rawRow, rawColumns, { L"Returned" });
        const std::uint64_t cpuCount = ReadNonZero(rawRow, rawColumns, { L"CpuCount" });
        const std::uint64_t moduleCount = ReadNonZero(rawRow, rawColumns, { L"ModuleCount" });
        if (total != 0) {
            reportedTotalRows = static_cast<std::size_t>(total);
        }
        if (returned != 0) {
            reportedReturnedRows = static_cast<std::size_t>(returned);
        }
        if (cpuCount != 0) {
            reportedCpuRows = static_cast<std::size_t>(cpuCount);
        }
        if (moduleCount != 0) {
            reportedModuleRows = static_cast<std::size_t>(moduleCount);
        }
        if (r0State.empty()) {
            r0State = ReadField(rawRow, rawColumns, { L"R0" });
        }
        if (r0Protocol.empty()) {
            r0Protocol = ReadField(rawRow, rawColumns, { L"Protocol" });
        }
        if (dynDataState.empty()) {
            dynDataState = ReadField(rawRow, rawColumns, { L"DynDataStatus" });
        }
        if (queryStatus.empty()) {
            queryStatus = ReadField(rawRow, rawColumns, { L"Status" });
        }
        if (sourceText.empty()) {
            sourceText = ReadField(rawRow, rawColumns, { L"SourceText", L"SourceMask" });
        }
    }
    for (const std::vector<std::wstring>& rawRow : rawRows) {
        const bool evidenceRow = HasValue(rawRow, rawColumns, {
            L"类别", L"Class", L"ClassText", L"对象", L"ObjectAddress", L"目标", L"TargetAddress",
            L"Owner", L"OwnerModule", L"CPU/Vector", L"CpuVector", L"风险", L"RiskText"
        });
        if (!evidenceRow) {
            continue;
        }
        ++totalRows;
        const std::wstring riskText = rawValue(rawRow, { L"风险", L"RiskText", L"Risk" });
        const std::wstring riskMask = rawValue(rawRow, { L"Risk" });
        const bool risky = !riskText.empty() &&
            !ContainsCaseInsensitive(riskText, L"正常") &&
            !ContainsCaseInsensitive(riskMask, L"0x0") &&
            riskMask != L"0";
        if (risky) {
            ++riskRows;
        }
        const bool supportsRiskOnly = featureId == KernelFeatureId::MutationAudit;
        if (supportsRiskOnly && riskOnlyCheck_ && Checked(riskOnlyCheck_) == BST_CHECKED && !risky) {
            continue;
        }
        const std::wstring merged = MergeRowText(rawRow);
        if (!filterText.empty() && !ContainsCaseInsensitive(merged, filterText)) {
            continue;
        }

        std::vector<std::wstring> cells(displayColumns.size());
        for (std::size_t column = 0; column < displayColumns.size(); ++column) {
            const std::wstring& name = displayColumns[column];
            if (name == L"#") {
                cells[column] = std::to_wstring(displayRows.size());
            } else if (name == L"Detail") {
                cells[column] = rawValue(rawRow, { L"Detail" });
            } else {
                cells[column] = displayValue(rawRow, name);
            }
        }
        displayRows.push_back(std::move(cells));
    }

    currentColumns_ = std::move(displayColumns);
    currentRows_ = std::move(displayRows);
    ClearTables();
    for (std::size_t index = 0; index < currentColumns_.size(); ++index) {
        const std::wstring& name = currentColumns_[index];
        const bool canonicalColumn = HasColumn(CanonicalColumnNames(featureId), name);
        int width = (!canonicalColumn && IsIntegrityHiddenColumn(name)) ? 0 : ColumnWidth(name);
        if (name == L"类别") {
            width = 150;
        } else if (name == L"对象" || name == L"目标" || name == L"对象/寄存器" || name == L"目标/入口") {
            width = 170;
        } else if (name == L"Owner" || name == L"Owner模块") {
            width = 240;
        } else if (name == L"CPU/Vector") {
            width = 110;
        } else if (name == L"风险") {
            width = 220;
        } else if (name == L"置信度") {
            width = 72;
        } else if (name == L"Detail") {
            width = 360;
        }
        SetColumn(static_cast<int>(index), name, width);
    }
    CommitRows();

    std::wostringstream status;
    const std::size_t effectiveTotal = reportedTotalRows == 0 ? totalRows : reportedTotalRows;
    const std::size_t effectiveReturned = reportedReturnedRows == 0 ? totalRows : reportedReturnedRows;
    status << L"状态：" << (cpuPage ? L"CPU/IDT 完整性" : L"驱动完整性")
           << L"已刷新，显示 " << currentRows_.size()
           << L"，返回 " << effectiveReturned << L"/" << effectiveTotal
           << L"，风险 " << riskRows;
    if (!queryStatus.empty()) {
        status << L"，Query " << queryStatus;
    }
    if (!sourceText.empty()) {
        status << L"，Source " << sourceText;
    }
    if (reportedCpuRows != 0 || reportedModuleRows != 0) {
        status << L"，CPU " << reportedCpuRows << L"，模块 " << reportedModuleRows;
    }
    if (cpuPage && (!r0State.empty() || !r0Protocol.empty() || !dynDataState.empty())) {
        status << L"，R0 " << (r0State.empty() ? L"<unknown>" : r0State)
               << L"，Protocol " << (r0Protocol.empty() ? L"-" : r0Protocol)
               << L"，DynData " << (dynDataState.empty() ? L"-" : dynDataState);
    }
    WriteText(statusText_, status.str().c_str());
    if (!currentRows_.empty()) {
        SetSelection(resultList_, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        EnsureItem(resultList_, 0, FALSE);
        SelectionDetail();
    } else {
        WriteText(detailEdit_, cpuPage
            ? L"当前过滤条件下没有 CPU/IDT 完整性证据。"
            : L"当前过滤条件下没有驱动完整性证据。");
    }
}

void RebuildDiagnosticDualTableFromCache(const KernelFeatureId featureId) {
    // RebuildDiagnosticDualTableFromCache reshapes DynData and DriverStatus
    // result packets into the original KernelDock dual-table layout. Inputs are
    // raw ArkDriverClient rows and the local filter text; processing builds the
    // original summary labels, visible matrix rows, and status sentence; output
    // updates the summary table, result table, detail editor, and status label.
    if (!resultList_ || !summaryList_) {
        return;
    }

    const std::vector<std::wstring>& rawColumns = !currentRawColumns_.empty() ? currentRawColumns_ : currentColumns_;
    const std::vector<std::vector<std::wstring>>& rawRows = !currentRawRows_->empty() ? *currentRawRows_ : currentRows_;
    const std::wstring filterText = Text(filterEdit_);
    const auto rawValue = [&](const std::vector<std::wstring>& row, std::initializer_list<const wchar_t*> names) -> std::wstring {
        return ReadField(row, rawColumns, names);
    };
    const auto firstValue = [&](std::initializer_list<const wchar_t*> names) -> std::wstring {
        return FirstRowValue(rawRows, rawColumns, names);
    };
    const auto firstNumber = [&](std::initializer_list<const wchar_t*> names, const std::uint64_t fallback = 0) -> std::uint64_t {
        return FirstRowUInt64(rawRows, rawColumns, names, fallback);
    };
    const auto appendSummary = [&](const std::wstring& name, const std::wstring& value) {
        if (!name.empty() && !value.empty()) {
            AppendSummary(summaryList_, { name, value });
        }
    };
    const auto appendSummaryBool = [&](const std::wstring& name, const bool value) {
        AppendSummary(summaryList_, { name, BoolText(value) });
    };
    const auto addHiddenColumn = [](std::vector<std::wstring>& columns, const wchar_t* name) {
        if (!HasColumn(columns, name)) {
            columns.push_back(name);
        }
    };
    const auto displayValue = [&](const std::vector<std::wstring>& row, const std::wstring& columnName) -> std::wstring {
        const auto& aliases = Aliases(featureId, columnName);
        for (const std::wstring& alias : aliases) {
            const std::wstring value = rawValue(row, { alias.c_str() });
            if (!value.empty()) {
                return value;
            }
        }
        if (columnName == L"Capability") {
            return rawValue(row, { L"Mask", L"CapabilityMask" });
        }
        if (columnName == L"策略") {
            return rawValue(row, { L"Policy", L"RequiredPolicy", L"Flags", L"SecurityPolicy" });
        }
        if (columnName == L"依赖字段") {
            return rawValue(row, { L"Dependency", L"DependencyText" });
        }
        if (columnName == L"原因") {
            return rawValue(row, { L"Reason", L"Detail", L"LastError" });
        }
        return {};
    };

    ClearTables();
    ClearValueList(summaryList_);

    std::vector<std::wstring> displayColumns;
    for (const std::wstring& column : CanonicalColumnNames(featureId)) {
        if (!HasColumn(displayColumns, column)) {
            displayColumns.push_back(column);
        }
    }
    addHiddenColumn(displayColumns, L"Id");
    addHiddenColumn(displayColumns, L"StateId");
    addHiddenColumn(displayColumns, L"StatusFlags");
    addHiddenColumn(displayColumns, L"CapabilityMask");
    addHiddenColumn(displayColumns, L"Version");
    addHiddenColumn(displayColumns, L"Protocol");
    addHiddenColumn(displayColumns, L"ExpectedProtocol");
    addHiddenColumn(displayColumns, L"SecurityPolicy");
    addHiddenColumn(displayColumns, L"DynDataStatus");
    addHiddenColumn(displayColumns, L"DynDataCapability");
    addHiddenColumn(displayColumns, L"FeatureTotal");
    addHiddenColumn(displayColumns, L"FeatureReturned");
    addHiddenColumn(displayColumns, L"LastError");
    addHiddenColumn(displayColumns, L"LastErrorSummary");
        addHiddenColumn(displayColumns, L"LastErrorStatus");
        addHiddenColumn(displayColumns, L"Flags");
        addHiddenColumn(displayColumns, L"StatusQueryOk");
        addHiddenColumn(displayColumns, L"FieldsQueryOk");
        addHiddenColumn(displayColumns, L"PdbProfileScanAttempted");
        addHiddenColumn(displayColumns, L"PdbProfileFound");
        addHiddenColumn(displayColumns, L"PdbProfileApplied");
        addHiddenColumn(displayColumns, L"PdbProfileSource");
        addHiddenColumn(displayColumns, L"PdbProfileName");
        addHiddenColumn(displayColumns, L"PdbProfilePath");
        addHiddenColumn(displayColumns, L"PdbProfileStatus");
        addHiddenColumn(displayColumns, L"PdbProfileAppliedFields");
        addHiddenColumn(displayColumns, L"PdbProfileRejectedFields");
        addHiddenColumn(displayColumns, L"PdbProfileUnknownFields");
        addHiddenColumn(displayColumns, L"PdbProfileIgnoredJsonFields");
        addHiddenColumn(displayColumns, L"PdbProfileMessage");
        addHiddenColumn(displayColumns, L"PdbProfileIo");
        addHiddenColumn(displayColumns, L"MatchedProfileOffset");
        addHiddenColumn(displayColumns, L"MatchedFieldsId");
        addHiddenColumn(displayColumns, L"RequiredPolicy");
        addHiddenColumn(displayColumns, L"DeniedPolicy");
        addHiddenColumn(displayColumns, L"StatusBadges");
        addHiddenColumn(displayColumns, L"DynDataStatusQueryOk");
        addHiddenColumn(displayColumns, L"DynDataFieldsQueryOk");
        addHiddenColumn(displayColumns, L"DynDataStatusIo");
        addHiddenColumn(displayColumns, L"DynDataFieldsIo");
        addHiddenColumn(displayColumns, L"FieldCoverage");
        addHiddenColumn(displayColumns, L"FieldSources");
        addHiddenColumn(displayColumns, L"RequiredMissing");
        addHiddenColumn(displayColumns, L"NtosIdentity");
        addHiddenColumn(displayColumns, L"LxcoreIdentity");
        addHiddenColumn(displayColumns, L"LocalPdbProfileMatched");
        addHiddenColumn(displayColumns, L"LocalPdbProfile");
        addHiddenColumn(displayColumns, L"LocalPdbProfileName");
        addHiddenColumn(displayColumns, L"LocalPdbProfilePath");
        addHiddenColumn(displayColumns, L"LocalPdbMessage");
        addHiddenColumn(displayColumns, L"LocalPdbVersion");
        addHiddenColumn(displayColumns, L"ActiveProcessLinksOffset");
        addHiddenColumn(displayColumns, L"CallbackProfileCoverage");
        addHiddenColumn(displayColumns, L"PdbProfileActive");
        addHiddenColumn(displayColumns, L"CallbackProfileActive");
        addHiddenColumn(displayColumns, L"TrustedPdbOffsetsActive");
        addHiddenColumn(displayColumns, L"TrustedOffset");
        addHiddenColumn(displayColumns, L"UnavailableReason");
        addHiddenColumn(displayColumns, L"Detail");

    std::vector<std::vector<std::wstring>> displayRows;
    std::size_t profileRows = 0;
    std::size_t failedRows = 0;
    std::size_t missingRequiredRows = 0;
    std::size_t unavailableRows = 0;
    std::size_t dynDataDependentRows = 0;
    for (const std::vector<std::wstring>& rawRow : rawRows) {
        const bool isDynField = featureId == KernelFeatureId::DynData &&
            HasValue(rawRow, rawColumns, { L"Field", L"字段", L"Offset", L"Source", L"Feature", L"Mask" }) &&
            !rawValue(rawRow, { L"Field", L"字段" }).empty();
        const bool isDriverFeature = featureId == KernelFeatureId::DriverStatus &&
            HasValue(rawRow, rawColumns, { L"Feature", L"功能", L"State", L"RequiredDyn", L"PresentDyn" }) &&
            !rawValue(rawRow, { L"Feature", L"功能" }).empty();
        const std::wstring merged = MergeRowText(rawRow);
        if (featureId == KernelFeatureId::DynData) {
            const std::wstring source = rawValue(rawRow, { L"Source", L"来源" });
            const std::wstring status = rawValue(rawRow, { L"Status", L"状态", L"DynData Fields IO" });
            if (ContainsCaseInsensitive(source, L"profile") || ContainsCaseInsensitive(source, L"pack") ||
                ContainsCaseInsensitive(source, L"pdb")) {
                ++profileRows;
            }
            if (ContainsCaseInsensitive(status, L"fail") || ContainsCaseInsensitive(status, L"失败") ||
                ContainsCaseInsensitive(status, L"missing") || ContainsCaseInsensitive(status, L"缺失")) {
                ++failedRows;
            }
            if (isDynField && ContainsCaseInsensitive(status, L"缺失(必需)")) {
                ++missingRequiredRows;
            }
        } else if (featureId == KernelFeatureId::DriverStatus) {
            const std::wstring state = rawValue(rawRow, { L"State", L"状态", L"Status" });
            const std::wstring requiredDyn = rawValue(rawRow, { L"RequiredDyn", L"所需DynData" });
            if (ContainsCaseInsensitive(state, L"unavailable") || ContainsCaseInsensitive(state, L"disabled") ||
                ContainsCaseInsensitive(state, L"denied") || ContainsCaseInsensitive(state, L"不可用") ||
                ContainsCaseInsensitive(state, L"禁用")) {
                ++unavailableRows;
            }
            if (!IsZeroMaskText(requiredDyn)) {
                ++dynDataDependentRows;
            }
        }
        if (!isDynField && !isDriverFeature) {
            continue;
        }
        if (!filterText.empty() && !ContainsCaseInsensitive(merged, filterText)) {
            continue;
        }

        std::vector<std::wstring> cells(displayColumns.size());
        for (std::size_t column = 0; column < displayColumns.size(); ++column) {
            const std::wstring& name = displayColumns[column];
            if (name == L"Detail") {
                cells[column] = rawValue(rawRow, { L"Detail" });
            } else {
                cells[column] = displayValue(rawRow, name);
            }
        }
        displayRows.push_back(std::move(cells));
    }

    if (featureId == KernelFeatureId::DynData) {
        const std::uint64_t statusFlags = firstNumber({ L"StatusFlags" });
        const bool initialized = FlagEnabled(statusFlags, KSW_DYN_STATUS_FLAG_INITIALIZED);
        const bool ntosActive = FlagEnabled(statusFlags, KSW_DYN_STATUS_FLAG_NTOS_ACTIVE);
        const bool lxcoreActive = FlagEnabled(statusFlags, KSW_DYN_STATUS_FLAG_LXCORE_ACTIVE);
        const bool extraActive = FlagEnabled(statusFlags, KSW_DYN_STATUS_FLAG_EXTRA_ACTIVE);
        const bool pdbActive = FlagEnabled(statusFlags, KSW_DYN_STATUS_FLAG_PDB_PROFILE_ACTIVE);
        const bool callbackActive = FlagEnabled(statusFlags, KSW_DYN_STATUS_FLAG_CALLBACK_PROFILE_ACTIVE);
        appendSummaryBool(L"DynData 初始化", initialized);
        appendSummaryBool(L"ntoskrnl profile", ntosActive);
        appendSummaryBool(L"lxcore profile", lxcoreActive);
        appendSummaryBool(L"Ksword runtime offset", extraActive);
        appendSummaryBool(L"PDB profile active", pdbActive);
        appendSummaryBool(L"PDB profile 扫描", firstValue({ L"PdbProfileScanAttempted" }) == L"是");
        appendSummaryBool(L"PDB profile 命中", firstValue({ L"PdbProfileFound" }) == L"是");
        appendSummaryBool(L"PDB profile 本次应用", firstValue({ L"PdbProfileApplied" }) == L"是");
        appendSummary(L"PDB profile 来源", firstValue({ L"PdbProfileSource" }));
        appendSummary(L"PDB profile 名称", firstValue({ L"PdbProfileName" }));
        appendSummary(L"PDB profile 路径", firstValue({ L"PdbProfilePath" }));
        appendSummary(L"PDB profile 状态", firstValue({ L"PdbProfileStatus" }));
        appendSummary(L"PDB profile 字段", L"applied=" + firstValue({ L"PdbProfileAppliedFields" }) +
            L" rejected=" + firstValue({ L"PdbProfileRejectedFields" }) +
            L" unknown=" + firstValue({ L"PdbProfileUnknownFields" }) +
            L" ignoredJson=" + firstValue({ L"PdbProfileIgnoredJsonFields" }));
        appendSummary(L"PDB profile 消息", firstValue({ L"PdbProfileMessage" }));
        appendSummary(L"PDB profile IO", firstValue({ L"PdbProfileIo" }));
        appendSummaryBool(L"Callback profile active", callbackActive);
        appendSummary(L"状态位", firstValue({ L"StatusFlags" }));
        appendSummary(L"System Informer 版本", firstValue({ L"SI Version" }));
        appendSummary(L"System Informer 数据长度", firstValue({ L"SI Length" }));
        appendSummary(L"MatchedProfileClass", firstValue({ L"MatchedClass", L"ProfileClass" }));
        appendSummary(L"MatchedProfileOffset", firstValue({ L"MatchedProfileOffset" }));
        appendSummary(L"MatchedFieldsId", firstValue({ L"MatchedFieldsId" }));
        appendSummary(L"CapabilityMask", firstValue({ L"CapabilityMask", L"Mask" }));
        appendSummary(L"字段总数/当前返回", firstValue({ L"FieldCount", L"FieldsTotal" }) + L" / " + std::to_wstring(displayRows.size()));
        appendSummary(L"Fields IO", firstValue({ L"DynData Fields IO" }));
        appendSummary(L"ntoskrnl", firstValue({ L"Ntos", L"NtosIdentity" }));
        appendSummary(L"lxcore", firstValue({ L"Lxcore", L"LxcoreIdentity" }));
        appendSummary(L"Profile/Pack 诊断行", std::to_wstring(profileRows));
        appendSummary(L"失败诊断行", std::to_wstring(failedRows));

        std::wstring statusLine = std::wstring(L"状态：") +
            (ntosActive ? L"ntos profile 已命中" : L"ntos profile 未命中") +
            (pdbActive ? L"，PDB profile 已启用" : L"") +
            L"，字段 " + std::to_wstring(displayRows.size()) +
            L" 项，缺失必需 " + std::to_wstring(missingRequiredRows) + L" 项";
        WriteText(statusText_, statusLine.c_str());
    } else {
        const std::uint64_t statusFlags = firstNumber({ L"StatusFlags" });
        const bool driverLoaded = FlagEnabled(statusFlags, KSWORD_ARK_DRIVER_STATUS_FLAG_DRIVER_LOADED);
        const bool protocolOk = FlagEnabled(statusFlags, KSWORD_ARK_DRIVER_STATUS_FLAG_PROTOCOL_OK);
        const bool dynDataMissing = FlagEnabled(statusFlags, KSWORD_ARK_DRIVER_STATUS_FLAG_DYNDATA_MISSING);
        const bool limited = FlagEnabled(statusFlags, KSWORD_ARK_DRIVER_STATUS_FLAG_LIMITED);
        const std::wstring badges = firstValue({ L"StatusBadges" }).empty()
            ? (std::wstring(L"Driver Loaded=") + BoolText(driverLoaded) +
                L"；Protocol OK=" + BoolText(protocolOk) +
                L"；DynData Missing=" + BoolText(dynDataMissing) +
                L"；Limited=" + BoolText(limited))
            : firstValue({ L"StatusBadges" });
        const std::wstring dynStatus = firstValue({ L"DynDataStatus" });
        const std::wstring dynCapability = firstValue({ L"DynDataCapability", L"CapabilityMask" });
        const std::wstring trustedOffset = !firstValue({ L"TrustedOffset" }).empty()
            ? firstValue({ L"TrustedOffset" })
            : (!dynCapability.empty() && dynCapability != L"0x0"
                ? std::wstring(L"已存在 DynData capability ") + dynCapability
                : std::wstring(L"暂无可用可信偏移。"));
        appendSummary(L"状态栏", badges);
        appendSummaryBool(L"Driver Loaded", driverLoaded);
        appendSummaryBool(L"Protocol OK", protocolOk);
        appendSummaryBool(L"DynData Missing", dynDataMissing);
        appendSummaryBool(L"Limited", limited);
        appendSummary(L"当前内核", firstValue({ L"NtosIdentity", L"Ntos" }));
        appendSummary(L"识别版本", firstValue({ L"LocalPdbVersion", L"KernelVersion" }));
        appendSummary(L"本地 PDB profile", firstValue({ L"LocalPdbProfile", L"LocalPdbMessage" }));
        appendSummary(L"ActiveProcessLinks 偏移", firstValue({ L"ActiveProcessLinksOffset" }));
        appendSummary(L"Callback profile 覆盖", firstValue({ L"CallbackProfileCoverage" }));
        appendSummary(L"可信偏移", trustedOffset);
        appendSummary(L"字段覆盖", firstValue({ L"FieldCoverage" }));
        appendSummary(L"字段来源", firstValue({ L"FieldSources" }));
        appendSummary(L"缺失必需字段", firstValue({ L"RequiredMissing" }));
        appendSummary(L"能力协议版本", firstValue({ L"Version" }));
        appendSummary(L"驱动协议版本", firstValue({ L"Protocol" }));
        appendSummary(L"期望协议版本", firstValue({ L"ExpectedProtocol" }));
        appendSummary(L"状态位", firstValue({ L"StatusFlags" }));
        appendSummary(L"安全策略位", firstValue({ L"SecurityPolicy", L"Policy", L"策略" }));
        appendSummary(L"DynData 状态位", dynStatus);
        appendSummary(L"DynData 能力位", dynCapability);
        appendSummary(L"System Informer 数据", L"version=" + firstValue({ L"SI Version" }) + L" length=" + firstValue({ L"SI Length" }));
        appendSummary(L"匹配内置 Profile", L"class=" + firstValue({ L"MatchedClass" }) +
            L" offset=" + firstValue({ L"MatchedProfileOffset" }) +
            L" fieldsId=" + firstValue({ L"MatchedFieldsId" }));
        appendSummary(L"DynData R3 IO", L"Status=" + firstValue({ L"DynDataStatusQueryOk" }) +
            L" (" + firstValue({ L"DynDataStatusIo" }) + L")；Fields=" +
            firstValue({ L"DynDataFieldsQueryOk" }) + L" (" + firstValue({ L"DynDataFieldsIo" }) + L")");
        appendSummary(L"DynData 不可用原因", firstValue({ L"UnavailableReason" }));
        appendSummary(L"功能数", L"显示 " + std::to_wstring(displayRows.size()) +
            L" / 返回 " + firstValue({ L"FeatureReturned" }) +
            L" / 总计 " + firstValue({ L"FeatureTotal" }));
        appendSummary(L"最近错误", firstValue({ L"LastErrorStatus" }) + L" / " +
            firstValue({ L"LastError" }) + L" / " + firstValue({ L"LastErrorSummary" }));
        appendSummary(L"R3 IO", firstValue({ L"IoMessage", L"Driver Capabilities" }));
        appendSummary(L"不可用/禁用行", std::to_wstring(unavailableRows));
        appendSummary(L"DynData 依赖行", std::to_wstring(dynDataDependentRows));

        const bool driverQueryFailed = rawRows.empty() || (!driverLoaded && !protocolOk && firstValue({ L"Version" }).empty());
        if (driverQueryFailed && dynStatus.empty()) {
            WriteText(statusText_, L"状态：驱动与 DynData 查询均失败");
        } else {
            const std::wstring kernelText = firstValue({ L"NtosIdentity", L"Ntos" }).empty() ? L"内核未识别" : firstValue({ L"NtosIdentity", L"Ntos" });
            const std::wstring statusLine = L"状态：" + badges + L"；" + kernelText + L"；功能 " +
                std::to_wstring(displayRows.size()) + L" 项；可信偏移 " + trustedOffset;
            WriteText(statusText_, statusLine.c_str());
        }
    }

    currentColumns_ = std::move(displayColumns);
    currentRows_ = std::move(displayRows);
    currentRowIndents_.clear();
    for (std::size_t index = 0; index < currentColumns_.size(); ++index) {
        const bool hidden = currentColumns_[index] == L"Id" ||
            currentColumns_[index] == L"StateId" ||
            currentColumns_[index] == L"StatusFlags" ||
            currentColumns_[index] == L"CapabilityMask" ||
            currentColumns_[index] == L"Version" ||
            currentColumns_[index] == L"Protocol" ||
            currentColumns_[index] == L"ExpectedProtocol" ||
            currentColumns_[index] == L"SecurityPolicy" ||
            currentColumns_[index] == L"DynDataStatus" ||
            currentColumns_[index] == L"DynDataCapability" ||
            currentColumns_[index] == L"FeatureTotal" ||
            currentColumns_[index] == L"FeatureReturned" ||
            currentColumns_[index] == L"LastError" ||
            currentColumns_[index] == L"LastErrorSummary" ||
            currentColumns_[index] == L"LastErrorStatus" ||
            currentColumns_[index] == L"Flags" ||
            currentColumns_[index] == L"StatusQueryOk" ||
            currentColumns_[index] == L"FieldsQueryOk" ||
            currentColumns_[index].find(L"PdbProfile") == 0 ||
            currentColumns_[index] == L"MatchedProfileOffset" ||
            currentColumns_[index] == L"MatchedFieldsId" ||
            currentColumns_[index] == L"RequiredPolicy" ||
            currentColumns_[index] == L"DeniedPolicy" ||
            currentColumns_[index] == L"StatusBadges" ||
            currentColumns_[index] == L"DynDataStatusQueryOk" ||
            currentColumns_[index] == L"DynDataFieldsQueryOk" ||
            currentColumns_[index] == L"DynDataStatusIo" ||
            currentColumns_[index] == L"DynDataFieldsIo" ||
            currentColumns_[index] == L"FieldCoverage" ||
            currentColumns_[index] == L"FieldSources" ||
            currentColumns_[index] == L"RequiredMissing" ||
            currentColumns_[index] == L"NtosIdentity" ||
            currentColumns_[index] == L"LxcoreIdentity" ||
            currentColumns_[index] == L"LocalPdbProfileMatched" ||
            currentColumns_[index] == L"LocalPdbProfile" ||
            currentColumns_[index] == L"LocalPdbProfileName" ||
            currentColumns_[index] == L"LocalPdbProfilePath" ||
            currentColumns_[index] == L"LocalPdbMessage" ||
            currentColumns_[index] == L"LocalPdbVersion" ||
            currentColumns_[index] == L"ActiveProcessLinksOffset" ||
            currentColumns_[index] == L"CallbackProfileCoverage" ||
            currentColumns_[index] == L"PdbProfileActive" ||
            currentColumns_[index] == L"CallbackProfileActive" ||
            currentColumns_[index] == L"TrustedPdbOffsetsActive" ||
            currentColumns_[index] == L"TrustedOffset" ||
            currentColumns_[index] == L"UnavailableReason" ||
            currentColumns_[index] == L"Detail";
        SetColumn(static_cast<int>(index), currentColumns_[index], hidden ? 0 : ColumnWidth(currentColumns_[index]));
    }
    CommitRows();
    if (!currentRows_.empty()) {
        SetSelection(resultList_, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        EnsureItem(resultList_, 0, FALSE);
        SelectionDetail();
    } else {
        const std::wstring report = BuildDiagnosticReportForCurrentFeature();
        WriteText(detailEdit_, report.empty() ? EmptyDetailTextForFeature(featureId) : report.c_str());
    }
}

void RebuildCallbackEnumerationListFromCache() {
    // RebuildCallbackEnumerationListFromCache mirrors the original
    // KernelDock.CallbackEnum table. Inputs are raw rows from
    // ArkDriverClient::enumerateCallbacks; processing keeps the exact visible
    // registration subtypes and policy glyphs, preserves hidden protocol fields for remove actions, and
    // applies the local filter edit; output updates resultList_ and detailEdit_.
    if (!resultList_) {
        return;
    }
    const std::vector<std::wstring>& rawColumns = !currentRawColumns_.empty() ? currentRawColumns_ : currentColumns_;
    const std::vector<std::vector<std::wstring>>& rawRows = !currentRawRows_->empty() ? *currentRawRows_ : currentRows_;
    const std::wstring filterText = Text(filterEdit_);
    const auto rawValue = [&](const std::vector<std::wstring>& row, std::initializer_list<const wchar_t*> names) -> std::wstring {
        return ReadField(row, rawColumns, names);
    };
    const auto addColumnIfMissing = [](std::vector<std::wstring>& columns, const wchar_t* name) {
        if (!HasColumn(columns, name)) {
            columns.push_back(name);
        }
    };
    const auto displayValue = [&](const std::vector<std::wstring>& row, const std::wstring& columnName) -> std::wstring {
        const auto& aliases = Aliases(KernelFeatureId::CallbackEnumeration, columnName);
        for (const std::wstring& alias : aliases) {
            const std::wstring value = rawValue(row, { alias.c_str() });
            if (!value.empty()) {
                return value;
            }
        }
        return {};
    };

    std::vector<std::wstring> displayColumns;
    for (const std::wstring& column : CanonicalColumnNames(KernelFeatureId::CallbackEnumeration)) {
        addColumnIfMissing(displayColumns, column.c_str());
    }
    addColumnIfMissing(displayColumns, L"SourceIndex");
    addColumnIfMissing(displayColumns, L"Class");
    addColumnIfMissing(displayColumns, L"StatusCode");
    addColumnIfMissing(displayColumns, L"RegistrationType");
    addColumnIfMissing(displayColumns, L"RemovePolicy");
    addColumnIfMissing(displayColumns, L"Source");
    addColumnIfMissing(displayColumns, L"Callback");
    addColumnIfMissing(displayColumns, L"Context");
    addColumnIfMissing(displayColumns, L"Registration");
    addColumnIfMissing(displayColumns, L"ModulePath");
    addColumnIfMissing(displayColumns, L"Win32ModulePath");
    addColumnIfMissing(displayColumns, L"ModuleBase");
    addColumnIfMissing(displayColumns, L"ModuleSize");
    addColumnIfMissing(displayColumns, L"OperationMask");
    addColumnIfMissing(displayColumns, L"ObjectTypeMask");
    addColumnIfMissing(displayColumns, L"FieldFlags");
    addColumnIfMissing(displayColumns, L"Trust");
    addColumnIfMissing(displayColumns, L"Remove");
    addColumnIfMissing(displayColumns, L"RemoveFlags");
    addColumnIfMissing(displayColumns, L"Generation");
    addColumnIfMissing(displayColumns, L"IdentityHash");
    addColumnIfMissing(displayColumns, L"RawStorageValue");
    addColumnIfMissing(displayColumns, L"LastStatus");
    addColumnIfMissing(displayColumns, L"Detail");

    std::vector<std::vector<std::wstring>> displayRows;
    std::size_t unsupportedCount = 0;
    std::size_t totalCallbackRows = 0;
    bool responseTruncated = false;
    for (const std::vector<std::wstring>& rawRow : rawRows) {
        std::uint64_t flagsValue = 0;
        if (ParseUnsigned64Value(rawValue(rawRow, { L"CallbackEnumResponseFlags", L"ResponseFlags", L"Flags" }), flagsValue) &&
            FlagEnabled(flagsValue, KSWORD_ARK_ENUM_CALLBACK_RESPONSE_FLAG_TRUNCATED)) {
            responseTruncated = true;
        }
        if (rawValue(rawRow, { L"CallbackEnumTruncated" }) == L"1") {
            responseTruncated = true;
        }
    }
    for (std::size_t sourceIndex = 0; sourceIndex < rawRows.size(); ++sourceIndex) {
        const std::vector<std::wstring>& rawRow = rawRows[sourceIndex];
        const bool meaningful = HasValue(rawRow, rawColumns, {
            L"ClassText", L"Class", L"SourceText", L"Source", L"Callback", L"Object", L"Registration", L"RawStorageValue", L"Name", L"ModulePath"
        });
        if (!meaningful) {
            continue;
        }
        ++totalCallbackRows;
        const std::wstring status = rawValue(rawRow, { L"Status", L"状态" });
        if (ContainsCaseInsensitive(status, L"unsupported") || ContainsCaseInsensitive(status, L"未支持")) {
            ++unsupportedCount;
        }
        const std::wstring merged = MergeRowText(rawRow);
        if (!filterText.empty() && !ContainsCaseInsensitive(merged, filterText)) {
            continue;
        }

        std::vector<std::wstring> cells(displayColumns.size());
        for (std::size_t column = 0; column < displayColumns.size(); ++column) {
            const std::wstring& name = displayColumns[column];
            if (name == L"SourceIndex") {
                cells[column] = std::to_wstring(sourceIndex);
            } else if (name == L"回调/对象地址") {
                std::wstring value = rawValue(rawRow, { L"Callback" });
                if (value.empty() || value == L"0" || value == L"0x0" || value == L"0x0000000000000000") {
                    value = rawValue(rawRow, { L"Object", L"Registration", L"RawStorageValue", L"Context" });
                }
                cells[column] = std::move(value);
            } else if (name == L"模块") {
                std::wstring module = rawValue(rawRow, { L"ModulePath", L"Module" });
                if (module.empty()) {
                    module = L"<未解析>";
                }
                cells[column] = std::move(module);
            } else if (name == L"名称") {
                std::wstring value = rawValue(rawRow, { L"Name", L"名称" });
                if (value.empty()) {
                    value = L"<无名称>";
                }
                cells[column] = std::move(value);
            } else if (name == L"Win32ModulePath") {
                cells[column] = NormalizeCallbackModulePath(rawValue(rawRow, { L"ModulePath", L"Module" }));
            } else if (name == L"Detail") {
                cells[column] = rawValue(rawRow, { L"Detail", L"FieldText", L"TrustText", L"RemoveText" });
            } else {
                cells[column] = displayValue(rawRow, name);
            }
        }
        displayRows.push_back(std::move(cells));
    }

    currentColumns_ = std::move(displayColumns);
    currentRows_ = std::move(displayRows);
    ClearTables();
    for (std::size_t index = 0; index < currentColumns_.size(); ++index) {
        const bool hidden = currentColumns_[index] == L"类别" ||
            currentColumns_[index] == L"StatusCode" ||
            currentColumns_[index] == L"RegistrationType" ||
            currentColumns_[index] == L"RemovePolicy" ||
            currentColumns_[index] == L"SourceIndex" ||
            currentColumns_[index] == L"Class" ||
            currentColumns_[index] == L"Source" ||
            currentColumns_[index] == L"Callback" ||
            currentColumns_[index] == L"Context" ||
            currentColumns_[index] == L"Registration" ||
            currentColumns_[index] == L"ModulePath" ||
            currentColumns_[index] == L"Win32ModulePath" ||
            currentColumns_[index] == L"ModuleBase" ||
            currentColumns_[index] == L"ModuleSize" ||
            currentColumns_[index] == L"OperationMask" ||
            currentColumns_[index] == L"ObjectTypeMask" ||
            currentColumns_[index] == L"FieldFlags" ||
            currentColumns_[index] == L"Trust" ||
            currentColumns_[index] == L"Remove" ||
            currentColumns_[index] == L"RemoveFlags" ||
            currentColumns_[index] == L"Generation" ||
            currentColumns_[index] == L"IdentityHash" ||
            currentColumns_[index] == L"RawStorageValue" ||
            currentColumns_[index] == L"LastStatus" ||
            currentColumns_[index] == L"Detail";
        int width = hidden ? 0 : ColumnWidth(currentColumns_[index]);
        if (currentColumns_[index] == L"可信状态") {
            width = 170;
        } else if (currentColumns_[index] == L"可移除") {
            width = 64;
        } else if (currentColumns_[index] == L"回调/对象地址") {
            width = 180;
        } else if (currentColumns_[index] == L"模块") {
            width = 220;
        } else if (currentColumns_[index] == L"名称") {
            width = 260;
        }
        SetColumn(static_cast<int>(index), currentColumns_[index], width);
    }
    CommitRows();
    std::wostringstream status;
    status << L"状态：已刷新 " << totalCallbackRows
           << L" 项，私有未支持 " << unsupportedCount << L" 项";
    if (responseTruncated) {
        status << L"，响应截断";
    }
    if (!filterText.empty()) {
        status << L"，显示 " << currentRows_.size() << L" 项";
    }
    WriteText(statusText_, status.str().c_str());
    if (!currentRows_.empty()) {
        SetSelection(resultList_, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        EnsureItem(resultList_, 0, FALSE);
        SelectionDetail();
    } else {
        WriteText(detailEdit_, L"当前环境未返回可见回调记录。");
    }
}

std::wstring BuildDiagnosticReportForCurrentFeature() const {
    // BuildDiagnosticReportForCurrentFeature mirrors the original helper
    // buildDynDataReport/buildDriverStatusReport. Inputs are currentRows_ and
    // currentColumns_; processing emits summary key/value lines followed by the
    // visible field/capability matrix; output is empty for non-diagnostic pages.
    const KernelFeatureDescriptor* descriptor = CurrentDescriptor();
    if (descriptor == nullptr) {
        return {};
    }

    const auto rowValue = [&](const std::vector<std::wstring>& row, std::initializer_list<const wchar_t*> names) -> std::wstring {
        return RowFieldByName(row, currentColumns_, names);
    };
    const auto firstValue = [&](std::initializer_list<const wchar_t*> names) -> std::wstring {
        for (const std::vector<std::wstring>& row : currentRows_) {
            const std::wstring value = rowValue(row, names);
            if (!value.empty()) {
                return value;
            }
        }
        return {};
    };
    const auto firstRawValue = [&](std::initializer_list<const wchar_t*> names) -> std::wstring {
        const std::vector<std::wstring>& rawColumns = !currentRawColumns_.empty() ? currentRawColumns_ : currentColumns_;
        const std::vector<std::vector<std::wstring>>& rawRows = !currentRawRows_->empty() ? *currentRawRows_ : currentRows_;
        for (const std::vector<std::wstring>& row : rawRows) {
            const std::wstring value = RowFieldByName(row, rawColumns, names);
            if (!value.empty()) {
                return value;
            }
        }
        return {};
    };
    const auto firstAnyValue = [&](std::initializer_list<const wchar_t*> names) -> std::wstring {
        const std::wstring visible = firstValue(names);
        return visible.empty() ? firstRawValue(names) : visible;
    };
    const auto appendTableHeader = [&](std::wostringstream& report) {
        const std::vector<int> copyColumns = CurrentCopyColumnIndices();
        for (std::size_t position = 0; position < copyColumns.size(); ++position) {
            if (position > 0) {
                report << L'\t';
            }
            const int column = copyColumns[position];
            if (column >= 0 && column < static_cast<int>(currentColumns_.size())) {
                report << currentColumns_[static_cast<std::size_t>(column)];
            }
        }
        report << L"\r\n";
    };
    const auto appendTableRows = [&](std::wostringstream& report) {
        const std::vector<int> copyColumns = CurrentCopyColumnIndices();
        for (const std::vector<std::wstring>& row : currentRows_) {
            for (std::size_t position = 0; position < copyColumns.size(); ++position) {
                if (position > 0) {
                    report << L'\t';
                }
                const int column = copyColumns[position];
                if (column < row.size()) {
                    report << row[static_cast<std::size_t>(column)];
                }
            }
            report << L"\r\n";
        }
    };

    if (descriptor->id == KernelFeatureId::DynData) {
        std::size_t fieldRows = 0;
        std::size_t profileRows = 0;
        std::size_t failedRows = 0;
        for (const std::vector<std::wstring>& row : currentRows_) {
            const std::wstring field = rowValue(row, { L"Field", L"字段", L"Name" });
            const std::wstring source = rowValue(row, { L"Source", L"来源" });
            const std::wstring status = rowValue(row, { L"Status", L"状态", L"DynData Fields IO" });
            if (!field.empty()) {
                ++fieldRows;
            }
            if (ContainsCaseInsensitive(source, L"profile") || ContainsCaseInsensitive(source, L"pack") ||
                ContainsCaseInsensitive(source, L"pdb")) {
                ++profileRows;
            }
            if (ContainsCaseInsensitive(status, L"fail") || ContainsCaseInsensitive(status, L"失败") ||
                ContainsCaseInsensitive(status, L"missing")) {
                ++failedRows;
            }
        }

        std::wostringstream report;
        report << L"Ksword DynData Diagnostic Report\r\n";
        report << L"========================================\r\n";
        AppendReportLine(report, L"StatusQueryOk", firstAnyValue({ L"StatusQueryOk" }));
        AppendReportLine(report, L"FieldsQueryOk", firstAnyValue({ L"FieldsQueryOk" }));
        AppendReportLine(report, L"StatusFlags", firstAnyValue({ L"StatusFlags" }));
        AppendReportLine(report, L"CapabilityMask", firstAnyValue({ L"CapabilityMask", L"Capability" }));
        AppendReportLine(report, L"SystemInformerDataVersion", firstAnyValue({ L"SI Version" }));
        AppendReportLine(report, L"SystemInformerDataLength", firstAnyValue({ L"SI Length" }));
        AppendReportLine(report, L"LastStatus", firstAnyValue({ L"LastStatus" }));
        AppendReportLine(report, L"MatchedProfileClass", firstAnyValue({ L"MatchedClass", L"ProfileClass" }));
        AppendReportLine(report, L"MatchedProfileOffset", firstAnyValue({ L"MatchedProfileOffset" }));
        AppendReportLine(report, L"MatchedFieldsId", firstAnyValue({ L"MatchedFieldsId" }));
        AppendReportLine(report, L"UnavailableReason", firstAnyValue({ L"UnavailableReason" }));
        AppendReportLine(report, L"PdbProfileActive", FlagEnabled(FirstRowUInt64(!currentRawRows_->empty() ? *currentRawRows_ : currentRows_,
            !currentRawColumns_.empty() ? currentRawColumns_ : currentColumns_, { L"StatusFlags" }), KSW_DYN_STATUS_FLAG_PDB_PROFILE_ACTIVE) ? L"是" : L"否");
        AppendReportLine(report, L"PdbProfileScanAttempted", firstAnyValue({ L"PdbProfileScanAttempted" }));
        AppendReportLine(report, L"PdbProfileFound", firstAnyValue({ L"PdbProfileFound" }));
        AppendReportLine(report, L"PdbProfileAppliedThisRefresh", firstAnyValue({ L"PdbProfileApplied" }));
        AppendReportLine(report, L"PdbProfileSource", firstAnyValue({ L"PdbProfileSource" }));
        AppendReportLine(report, L"PdbProfileName", firstAnyValue({ L"PdbProfileName", L"Profile" }));
        AppendReportLine(report, L"PdbProfilePath", firstAnyValue({ L"PdbProfilePath", L"ProfilePath" }));
        AppendReportLine(report, L"PdbProfileStatus", firstAnyValue({ L"PdbProfileStatus" }));
        AppendReportLine(report, L"PdbProfileAppliedFields", firstAnyValue({ L"PdbProfileAppliedFields" }));
        AppendReportLine(report, L"PdbProfileRejectedFields", firstAnyValue({ L"PdbProfileRejectedFields" }));
        AppendReportLine(report, L"PdbProfileUnknownFields", firstAnyValue({ L"PdbProfileUnknownFields" }));
        AppendReportLine(report, L"PdbProfileIgnoredJsonFields", firstAnyValue({ L"PdbProfileIgnoredJsonFields" }));
        AppendReportLine(report, L"PdbProfileMessage", firstAnyValue({ L"PdbProfileMessage" }));
        AppendReportLine(report, L"PdbProfileIo", firstAnyValue({ L"PdbProfileIo" }));
        AppendReportLine(report, L"Ntos", firstAnyValue({ L"Ntos", L"NtosIdentity" }));
        AppendReportLine(report, L"Lxcore", firstAnyValue({ L"Lxcore", L"LxcoreIdentity" }));
        AppendReportLine(report, L"FieldCount", firstAnyValue({ L"FieldCount", L"FieldsTotal" }));
        AppendReportLine(report, L"FieldsReturned", firstAnyValue({ L"FieldsReturned" }));
        report << L"VisibleFieldRows: " << fieldRows << L"\r\n";
        report << L"ProfileDiagnosticRows: " << profileRows << L"\r\n";
        report << L"FailedDiagnosticRows: " << failedRows << L"\r\n\r\n";
        report << L"Capabilities:\r\n";
        std::uint64_t capabilityMask = 0;
        HexToUInt64(firstAnyValue({ L"CapabilityMask", L"Capability" }), capabilityMask);
        report << DynCapabilityNames(capabilityMask) << L"\r\n\r\n";
        report << L"Fields\r\n";
        report << L"----------------------------------------\r\n";
        appendTableHeader(report);
        appendTableRows(report);
        return report.str();
    }

    if (descriptor->id == KernelFeatureId::DriverStatus) {
        std::size_t capabilityRows = 0;
        std::size_t unavailableRows = 0;
        std::size_t dynDataDependentRows = 0;
        for (const std::vector<std::wstring>& row : currentRows_) {
            const std::wstring feature = rowValue(row, { L"Feature", L"功能", L"Name" });
            const std::wstring state = rowValue(row, { L"State", L"状态", L"Status" });
            const std::wstring requiredDyn = rowValue(row, { L"RequiredDyn", L"所需DynData" });
            if (!feature.empty()) {
                ++capabilityRows;
            }
            if (ContainsCaseInsensitive(state, L"unavailable") || ContainsCaseInsensitive(state, L"disabled") ||
                ContainsCaseInsensitive(state, L"不可用") || ContainsCaseInsensitive(state, L"禁用")) {
                ++unavailableRows;
            }
            if (!requiredDyn.empty() && requiredDyn != L"0" && requiredDyn != L"0x0" && requiredDyn != L"0x0000000000000000") {
                ++dynDataDependentRows;
            }
        }

        std::wostringstream report;
        report << L"Ksword Driver Capability Diagnostic Report\r\n";
        report << L"========================================\r\n";
        AppendReportLine(report, L"Status", firstAnyValue({ L"StatusBadges" }));
        AppendReportLine(report, L"QueryOk", firstAnyValue({ L"IO" }));
        AppendReportLine(report, L"IoMessage", firstAnyValue({ L"IoMessage", L"Driver Capabilities" }));
        AppendReportLine(report, L"CapabilityProtocolVersion", firstAnyValue({ L"Version" }));
        AppendReportLine(report, L"DriverProtocolVersion", firstAnyValue({ L"Protocol" }));
        AppendReportLine(report, L"ExpectedDriverProtocolVersion", firstAnyValue({ L"ExpectedProtocol" }));
        AppendReportLine(report, L"StatusFlags", firstAnyValue({ L"StatusFlags" }));
        std::uint64_t policyMask = 0;
        HexToUInt64(firstAnyValue({ L"SecurityPolicy", L"Policy", L"策略" }), policyMask);
        AppendReportLine(report, L"SecurityPolicyFlags", firstAnyValue({ L"SecurityPolicy", L"Policy", L"策略" }) +
            L" (" + SecurityPolicyNames(static_cast<std::uint32_t>(policyMask)) + L")");
        std::uint64_t dynStatusFlags = 0;
        HexToUInt64(firstAnyValue({ L"DynDataStatus" }), dynStatusFlags);
        AppendReportLine(report, L"DynDataStatusFlags", firstAnyValue({ L"DynDataStatus" }) +
            L" (" + DynDataStatusFlagsText(static_cast<std::uint32_t>(dynStatusFlags)) + L")");
        std::uint64_t dynCapability = 0;
        HexToUInt64(firstAnyValue({ L"DynDataCapability", L"CapabilityMask" }), dynCapability);
        AppendReportLine(report, L"DynDataCapabilityMask", firstAnyValue({ L"DynDataCapability", L"CapabilityMask" }) +
            L" (" + DynCapabilityNames(dynCapability) + L")");
        AppendReportLine(report, L"DynDataStatusQueryOk", firstAnyValue({ L"DynDataStatusQueryOk" }));
        AppendReportLine(report, L"DynDataFieldsQueryOk", firstAnyValue({ L"DynDataFieldsQueryOk" }));
        AppendReportLine(report, L"CurrentKernel", firstAnyValue({ L"NtosIdentity", L"Ntos" }));
        AppendReportLine(report, L"RecognizedVersion", firstAnyValue({ L"LocalPdbVersion", L"KernelVersion" }));
        AppendReportLine(report, L"LocalPdbProfileMatched", firstAnyValue({ L"LocalPdbProfileMatched" }));
        AppendReportLine(report, L"LocalPdbProfileName", firstAnyValue({ L"LocalPdbProfileName" }));
        AppendReportLine(report, L"LocalPdbProfilePath", firstAnyValue({ L"LocalPdbProfilePath" }));
        AppendReportLine(report, L"LocalPdbProfileMessage", firstAnyValue({ L"LocalPdbMessage", L"LocalPdbProfile" }));
        AppendReportLine(report, L"ActiveProcessLinksOffset", firstAnyValue({ L"ActiveProcessLinksOffset" }));
        AppendReportLine(report, L"CallbackProfileCoverage", firstAnyValue({ L"CallbackProfileCoverage" }));
        AppendReportLine(report, L"PdbProfileActive", firstAnyValue({ L"PdbProfileActive" }));
        AppendReportLine(report, L"CallbackProfileActive", firstAnyValue({ L"CallbackProfileActive" }));
        AppendReportLine(report, L"TrustedPdbOffsetsActive", firstAnyValue({ L"TrustedPdbOffsetsActive" }));
        AppendReportLine(report, L"TrustedOffsetSummary", firstAnyValue({ L"TrustedOffset" }));
        AppendReportLine(report, L"FieldCoverage", firstAnyValue({ L"FieldCoverage" }));
        AppendReportLine(report, L"FieldSources", firstAnyValue({ L"FieldSources" }));
        AppendReportLine(report, L"SystemInformerData", L"version=" + firstAnyValue({ L"SI Version" }) +
            L" length=" + firstAnyValue({ L"SI Length" }));
        AppendReportLine(report, L"MatchedProfile", L"class=" + firstAnyValue({ L"MatchedClass" }) +
            L" offset=" + firstAnyValue({ L"MatchedProfileOffset" }) +
            L" fieldsId=" + firstAnyValue({ L"MatchedFieldsId" }));
        AppendReportLine(report, L"DynDataUnavailableReason", firstAnyValue({ L"UnavailableReason" }));
        AppendReportLine(report, L"DynDataIo", L"Status=" + firstAnyValue({ L"DynDataStatusQueryOk" }) +
            L" (" + firstAnyValue({ L"DynDataStatusIo" }) + L")；Fields=" +
            firstAnyValue({ L"DynDataFieldsQueryOk" }) + L" (" + firstAnyValue({ L"DynDataFieldsIo" }) + L")");
        AppendReportLine(report, L"LastError", firstAnyValue({ L"LastErrorStatus" }) + L" / " +
            firstAnyValue({ L"LastError" }) + L" / " + firstAnyValue({ L"LastErrorSummary" }));
        AppendReportLine(report, L"FeatureCount", L"returned=" + firstAnyValue({ L"FeatureReturned" }) +
            L" total=" + firstAnyValue({ L"FeatureTotal" }));
        report << L"VisibleCapabilityRows: " << capabilityRows << L"\r\n";
        report << L"UnavailableRows: " << unavailableRows << L"\r\n";
        report << L"DynDataDependentRows: " << dynDataDependentRows << L"\r\n\r\n";
        report << L"Capabilities\r\n";
        report << L"----------------------------------------\r\n";
        appendTableHeader(report);
        appendTableRows(report);
        return report.str();
    }

    return {};
}

std::wstring NormalizeCallbackModulePath(const std::wstring& modulePath) const {
    // NormalizeCallbackModulePath mirrors the original CallbackEnum module path
    // normalization. Input is an R0 module path such as \SystemRoot\... or
    // \??\C:\...; processing converts it to a Win32 file path when possible;
    // output is empty when the selected row has no usable module file.
    std::wstring path = modulePath;
    while (!path.empty() && std::iswspace(path.front())) {
        path.erase(path.begin());
    }
    while (!path.empty() && std::iswspace(path.back())) {
        path.pop_back();
    }
    if (path.empty() || path == L"<未解析>" || path == L"<unknown>") {
        return {};
    }
    constexpr wchar_t ntPrefix[] = L"\\??\\";
    if (_wcsnicmp(path.c_str(), ntPrefix, 4) == 0) {
        path.erase(0, 4);
    }
    constexpr wchar_t systemRootPrefix[] = L"\\SystemRoot\\";
    if (_wcsnicmp(path.c_str(), systemRootPrefix, 12) == 0) {
        const auto& windowsDir=input_->windowsDirectory;
        if (!windowsDir.empty()) {
            path = std::wstring(windowsDir) + path.substr(11);
        }
    }
    constexpr wchar_t sysrootPrefix[] = L"SystemRoot\\";
    if (_wcsnicmp(path.c_str(), sysrootPrefix, 11) == 0) {
        const auto& windowsDir=input_->windowsDirectory;
        if (!windowsDir.empty()) {
            path = std::wstring(windowsDir) + L"\\" + path.substr(11);
        }
    }
    return path;
}

R0Values BuildR0Values(
    const KernelFeatureId featureId,
    std::wstring pageName,
    const std::vector<std::wstring>& rawColumns,
    const KernelRawRows& rawRows,
    std::wstring filterText,
    const bool riskOnly,
    std::wstring selectedRowSignature,
    const int topRow) {
    R0Values snapshot{};
    snapshot.featureId = featureId;
    snapshot.selectedRowSignature = std::move(selectedRowSignature);
    snapshot.topRow = topRow;

    const auto rawValue = [&](const std::vector<std::wstring>& row, std::initializer_list<const wchar_t*> names) -> std::wstring {
        return ReadField(row, rawColumns, names);
    };
    const auto addColumnIfMissing = [](std::vector<std::wstring>& columns, const wchar_t* name) {
        if (!HasColumn(columns, name)) {
            columns.push_back(name);
        }
    };
    const auto displayValue = [&](const std::vector<std::wstring>& row, const std::wstring& columnName) -> std::wstring {
        const auto& aliases = Aliases(featureId, columnName);
        for (const std::wstring& alias : aliases) {
            const std::wstring value = rawValue(row, { alias.c_str() });
            if (!value.empty()) {
                return value;
            }
        }
        return {};
    };
    const auto isDataRow = [&](const std::vector<std::wstring>& row) {
        switch (featureId) {
        case KernelFeatureId::CpuHardwareSnapshot:
            return HasValue(row, rawColumns, { L"Brand", L"Vendor", L"FeatureMask", L"Leaf1ECX", L"项目" });
        case KernelFeatureId::PhysicalMemoryLayout:
            return HasValue(row, rawColumns, { L"Ranges", L"TotalBytes", L"HighestAddress", L"范围" });
        case KernelFeatureId::MutationAudit:
            return HasValue(row, rawColumns, { L"Seq", L"Tx", L"Operation", L"TargetKind", L"Address" });
        case KernelFeatureId::KeyboardHotkeys:
            return HasValue(row, rawColumns, { L"PID", L"TID", L"热键", L"Object", L"WindowObject" });
        case KernelFeatureId::KeyboardHooks:
            return HasValue(row, rawColumns, { L"PID", L"TID", L"Hook类型", L"Procedure", L"Object" });
        case KernelFeatureId::DynDataCapabilities:
            return HasValue(row, rawColumns, { L"Capability", L"CapabilityMask", L"StatusFlags", L"字段" });
        case KernelFeatureId::MinifilterBypassPids:
            return HasValue(row, rawColumns, { L"PID", L"Process", L"Index" });
        case KernelFeatureId::KernelTimerDpc:
            return HasValue(row, rawColumns, { L"Timer", L"TimerAddress", L"DPC", L"DpcAddress" });
        case KernelFeatureId::IoctlRegistry:
            return HasValue(row, rawColumns, { L"IOCTL", L"IoControlCode", L"Name", L"FunctionNumber" });
        default:
            return false;
        }
    };

    snapshot.columns.push_back(L"#");
    for (const std::wstring& column : CanonicalColumnNames(featureId)) {
        addColumnIfMissing(snapshot.columns, column.c_str());
    }
    for (const std::wstring& column : rawColumns) {
        addColumnIfMissing(snapshot.columns, column.c_str());
    }
    addColumnIfMissing(snapshot.columns, L"Detail");

    std::size_t reportedTotalRows = 0;
    std::size_t reportedReturnedRows = 0;
    std::size_t totalRows = 0;
    std::size_t riskRows = 0;
    std::wstring queryStatus;
    std::wstring unsupportedText;
    std::wstring protocolVersion;
    std::wstring lastStatus;
    for (const std::vector<std::wstring>& rawRow : rawRows) {
        const std::uint64_t total = ReadNonZero(rawRow, rawColumns, { L"Total", L"TotalCount" });
        const std::uint64_t returned = ReadNonZero(rawRow, rawColumns, { L"Returned", L"ReturnedCount" });
        if (total != 0) {
            reportedTotalRows = static_cast<std::size_t>(total);
        }
        if (returned != 0) {
            reportedReturnedRows = static_cast<std::size_t>(returned);
        }
        if (queryStatus.empty()) {
            queryStatus = rawValue(rawRow, { L"Status", L"状态" });
        }
        if (unsupportedText.empty()) {
            unsupportedText = rawValue(rawRow, { L"Unsupported" });
        }
        if (protocolVersion.empty()) {
            protocolVersion = rawValue(rawRow, { L"Version", L"Protocol" });
        }
        if (lastStatus.empty()) {
            lastStatus = rawValue(rawRow, { L"LastStatus" });
        }
    }
    snapshot.rows.reserve(rawRows.size());
    for (const std::vector<std::wstring>& rawRow : rawRows) {
        if (!isDataRow(rawRow)) {
            continue;
        }
        ++totalRows;
        const std::wstring risk = rawValue(rawRow, { L"RiskText", L"Risk", L"FlagsText", L"Flags", L"状态", L"Status" });
        const bool risky = ContainsCaseInsensitive(risk, L"风险") ||
            ContainsCaseInsensitive(risk, L"异常") ||
            ContainsCaseInsensitive(risk, L"failed") ||
            ContainsCaseInsensitive(risk, L"失败") ||
            ContainsCaseInsensitive(risk, L"rejected");
        if (risky) {
            ++riskRows;
        }
        if (riskOnly && !risky) {
            continue;
        }
        if (!filterText.empty() && !ContainsCaseInsensitive(MergeRowText(rawRow), filterText)) {
            continue;
        }
        std::vector<std::wstring> cells(snapshot.columns.size());
        for (std::size_t column = 0; column < snapshot.columns.size(); ++column) {
            const std::wstring& name = snapshot.columns[column];
            if (name == L"#") {
                cells[column] = std::to_wstring(snapshot.rows.size());
            } else if (name == L"Detail") {
                cells[column] = rawValue(rawRow, { L"Detail" });
            } else {
                cells[column] = displayValue(rawRow, name);
            }
        }
        snapshot.rows.push_back(std::move(cells));
    }

    const std::size_t effectiveTotal = reportedTotalRows == 0 ? totalRows : reportedTotalRows;
    const std::size_t effectiveReturned = reportedReturnedRows == 0 ? totalRows : reportedReturnedRows;
    std::wostringstream status;
    status << L"状态：" << pageName << L"已刷新，显示 " << snapshot.rows.size()
           << L"，返回 " << effectiveReturned << L"/" << effectiveTotal;
    if (riskRows != 0) {
        status << L"，风险/异常 " << riskRows;
    }
    if (!queryStatus.empty()) {
        status << L"，Query " << queryStatus;
    }
    if (!unsupportedText.empty()) {
        status << L"，Unsupported " << unsupportedText;
    }
    if (!protocolVersion.empty()) {
        status << L"，Ver " << protocolVersion;
    }
    if (!lastStatus.empty()) {
        status << L"，Last " << lastStatus;
    }
    snapshot.statusText = status.str();
    return snapshot;
}

    void RebuildR0ValueProjection(const KernelFeatureId featureId) {
        auto snapshot=BuildR0Values(featureId,input_->descriptor.title,currentRawColumns_,*currentRawRows_,input_->filter,input_->riskOnly,{},0);
        currentColumns_=std::move(snapshot.columns);currentRows_=std::move(snapshot.rows);currentRowIndents_.clear();
        ClearTables();
        const std::vector<std::wstring> canonical = CanonicalColumnNames(featureId);
        for (std::size_t index = 0; index < currentColumns_.size(); ++index) {
            const std::wstring& name = currentColumns_[index];
            int width = (name != L"#" && !HasColumn(canonical, name)) ? 0 : ColumnWidth(name);
            if (name == L"对象") width = 180;
            else if (name == L"热键ID" || name == L"进程ID" || name == L"线程ID" || name == L"Flags") width = 80;
            else if (name == L"热键" || name == L"类型" || name == L"范围" || name == L"VK/Mod") width = 130;
            else if (name == L"函数/偏移") width = 170;
            else if (name == L"详情") width = 300;
            else if (name == L"项目" || name == L"Capability") width = 170;
            else if (name == L"值" || name == L"摘要" || name == L"Features" || name == L"字段" || name == L"原因") width = 260;
            else if (name == L"Tx" || name == L"Address" || name == L"回调") width = 165;
            else if (name == L"PID" || name == L"TID" || name == L"Bytes") width = 72;
            else if (name == L"进程" || name == L"Process") width = 180;
            else if (name == L"Detail") width = 320;
            SetColumn(static_cast<int>(index), name, width);
        }

        WriteText(statusText_,snapshot.statusText.c_str());
        if(currentRows_.empty())WriteText(detailEdit_,L"当前过滤条件下没有 R0 证据记录。");
        else {result_->selectFirst=true;SelectionDetail();}
    }


void RebuildObjectNamespaceListFromCache(const KernelFeatureId featureId) {
    // RebuildObjectNamespaceListFromCache reshapes generic KernelFacade rows
    // into the original KernelDock object-namespace tree/table schemas. Inputs
    // are currentColumns_/currentRows_ from AddDynamicResultRow; processing adds
    // root rows, tree indentation, original headers and hidden action fields;
    // there is no return value because the ListView owns the visible output.
    const std::vector<std::wstring>& rawColumns = !currentRawColumns_.empty() ? currentRawColumns_ : currentColumns_;
    const std::vector<std::vector<std::wstring>>& rawRows = !currentRawRows_->empty() ? *currentRawRows_ : currentRows_;
    std::vector<std::wstring> displayColumns;
    for (const std::wstring& column : CanonicalColumnNames(featureId)) {
        if (!HasColumn(displayColumns, column)) {
            displayColumns.push_back(column);
        }
    }
    const auto addHiddenColumn = [&](const wchar_t* name) {
        if (!HasColumn(displayColumns, name)) {
            displayColumns.push_back(name);
        }
    };
    addHiddenColumn(L"Parent");
    addHiddenColumn(L"Source");
    addHiddenColumn(L"Directory");
    addHiddenColumn(L"directoryPath");
    addHiddenColumn(L"sourceDirectory");
    addHiddenColumn(L"Depth");
    addHiddenColumn(L"Path");
    addHiddenColumn(L"NtPath");
    addHiddenColumn(L"Target");
    addHiddenColumn(L"Win32Path");
    addHiddenColumn(L"dosCandidate");
    addHiddenColumn(L"fullPath");
    addHiddenColumn(L"targetPath");
    addHiddenColumn(L"symbolicTarget");
    addHiddenColumn(L"objectName");
    addHiddenColumn(L"objectType");
    addHiddenColumn(L"linkName");
    addHiddenColumn(L"Handles");
    addHiddenColumn(L"Pointers");
    addHiddenColumn(L"Detail");
    addHiddenColumn(L"EnumApi");
    addHiddenColumn(L"enumApi");
    addHiddenColumn(L"枚举 API");
    addHiddenColumn(L"EnumerationApi");
    addHiddenColumn(L"NodeKind");

    const auto rawValue = [&](const std::vector<std::wstring>& row, std::initializer_list<const wchar_t*> names) -> std::wstring {
        return ReadField(row, rawColumns, names);
    };
    const auto displayValue = [&](const std::vector<std::wstring>& row, const std::wstring& columnName) -> std::wstring {
        if (columnName == L"Detail") {
            return rawValue(row, { L"Detail" });
        }
        const auto& aliases = Aliases(featureId, columnName);
        for (const std::wstring& alias : aliases) {
            const std::wstring value = ReadField(row, rawColumns, { alias.c_str() });
            if (!value.empty()) {
                return value;
            }
        }
        return {};
    };
    if (featureId == KernelFeatureId::DeviceDriverObjects && deviceDriverDirectoryCombo_ && deviceDriverTypeCombo_) {
        std::set<std::wstring> types;
        for (const std::vector<std::wstring>& rawRow : rawRows) {
            const std::wstring type = rawValue(rawRow, { L"Type", L"objectType", L"对象类型", L"类型" });
            if (!type.empty()) {
                types.insert(type);
            }
        }
        const std::wstring previousDirectory = [this]() -> std::wstring {
            const int selection = static_cast<int>(ChoiceMessage(deviceDriverDirectoryCombo_, CB_GETCURSEL, 0, 0));
            if (selection <= 0) {
                return {};
            }
            std::wstring buffer;
            buffer = ChoiceText(deviceDriverDirectoryCombo_);
            return buffer;
        }();
        const std::wstring previousType = [this]() -> std::wstring {
            const int selection = static_cast<int>(ChoiceMessage(deviceDriverTypeCombo_, CB_GETCURSEL, 0, 0));
            if (selection <= 0) {
                return {};
            }
            std::wstring buffer;
            buffer = ChoiceText(deviceDriverTypeCombo_);
            return buffer;
        }();

        ChoiceMessage(deviceDriverDirectoryCombo_, CB_RESETCONTENT, 0, 0);
        ChoiceMessage(deviceDriverDirectoryCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"全部目录"));
        int restoredDirectory = 0;
        for (const wchar_t* directory : kDeviceDriverFixedDirectories) {
            const int index = static_cast<int>(ChoiceMessage(deviceDriverDirectoryCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(directory)));
            if (!previousDirectory.empty() && _wcsicmp(previousDirectory.c_str(), directory) == 0) {
                restoredDirectory = index;
            }
        }
        ChoiceMessage(deviceDriverDirectoryCombo_, CB_SETCURSEL, restoredDirectory, 0);

        ChoiceMessage(deviceDriverTypeCombo_, CB_RESETCONTENT, 0, 0);
        ChoiceMessage(deviceDriverTypeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"全部类型"));
        int restoredType = 0;
        for (const std::wstring& type : types) {
            const int index = static_cast<int>(ChoiceMessage(deviceDriverTypeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(type.c_str())));
            if (!previousType.empty() && _wcsicmp(previousType.c_str(), type.c_str()) == 0) {
                restoredType = index;
            }
        }
        ChoiceMessage(deviceDriverTypeCombo_, CB_SETCURSEL, restoredType, 0);
    }
    if (featureId == KernelFeatureId::BaseNamedObjects && baseNamedScopeCombo_ && baseNamedTypeCombo_) {
        std::set<std::wstring> scopes;
        std::set<std::wstring> types;
        for (const std::vector<std::wstring>& rawRow : rawRows) {
            const std::wstring scope = rawValue(rawRow, { L"Source", L"Scope", L"scope" });
            const std::wstring type = rawValue(rawRow, { L"Type", L"objectType", L"类型" });
            if (!scope.empty()) {
                scopes.insert(scope);
            }
            if (!type.empty()) {
                types.insert(type);
            }
        }
        const std::wstring previousScope = [this]() -> std::wstring {
            const int selection = static_cast<int>(ChoiceMessage(baseNamedScopeCombo_, CB_GETCURSEL, 0, 0));
            if (selection <= 0) {
                return {};
            }
            std::wstring buffer;
            buffer = ChoiceText(baseNamedScopeCombo_);
            return buffer;
        }();
        const std::wstring previousType = [this]() -> std::wstring {
            const int selection = static_cast<int>(ChoiceMessage(baseNamedTypeCombo_, CB_GETCURSEL, 0, 0));
            if (selection <= 0) {
                return {};
            }
            std::wstring buffer;
            buffer = ChoiceText(baseNamedTypeCombo_);
            return buffer;
        }();
        ChoiceMessage(baseNamedScopeCombo_, CB_RESETCONTENT, 0, 0);
        ChoiceMessage(baseNamedScopeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"全部 Session"));
        int restoredScope = 0;
        for (const std::wstring& scope : scopes) {
            const int index = static_cast<int>(ChoiceMessage(baseNamedScopeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(scope.c_str())));
            if (!previousScope.empty() && _wcsicmp(previousScope.c_str(), scope.c_str()) == 0) {
                restoredScope = index;
            }
        }
        ChoiceMessage(baseNamedScopeCombo_, CB_SETCURSEL, restoredScope, 0);
        ChoiceMessage(baseNamedTypeCombo_, CB_RESETCONTENT, 0, 0);
        ChoiceMessage(baseNamedTypeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"全部类型"));
        int restoredType = 0;
        for (const std::wstring& type : types) {
            const int index = static_cast<int>(ChoiceMessage(baseNamedTypeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(type.c_str())));
            if (!previousType.empty() && _wcsicmp(previousType.c_str(), type.c_str()) == 0) {
                restoredType = index;
            }
        }
        ChoiceMessage(baseNamedTypeCombo_, CB_SETCURSEL, restoredType, 0);
    }
    const auto rowMatchesObjectFilters = [&](const std::vector<std::wstring>& row) -> bool {
        const std::wstring primaryFilter = Text(filterEdit_);
        const std::wstring secondaryFilter = Text(moduleFilterEdit_);
        if (featureId == KernelFeatureId::ObjectNamespaceOverview) {
            const std::wstring merged = rawValue(row, { L"Parent", L"Source" }) + L" | " +
                rawValue(row, { L"Name" }) + L" | " +
                rawValue(row, { L"Type" }) + L" | " +
                rawValue(row, { L"Path" }) + L" | " +
                rawValue(row, { L"Status" }) + L" | " +
                rawValue(row, { L"Target" });
            return primaryFilter.empty() || ContainsCaseInsensitive(merged, primaryFilter);
        }
        if (featureId == KernelFeatureId::ObjectDirectoryRecursive) {
            return true;
        }
        if (featureId == KernelFeatureId::BaseNamedObjects) {
            std::wstring scopeFilter;
            std::wstring typeFilter;
            if (baseNamedScopeCombo_) {
                const int selection = static_cast<int>(ChoiceMessage(baseNamedScopeCombo_, CB_GETCURSEL, 0, 0));
                if (selection > 0) {
                    std::wstring buffer;
                    buffer = ChoiceText(baseNamedScopeCombo_);
                    scopeFilter = buffer;
                }
            }
            if (baseNamedTypeCombo_) {
                const int selection = static_cast<int>(ChoiceMessage(baseNamedTypeCombo_, CB_GETCURSEL, 0, 0));
                if (selection > 0) {
                    std::wstring buffer;
                    buffer = ChoiceText(baseNamedTypeCombo_);
                    typeFilter = buffer;
                }
            }
            const std::wstring merged = rawValue(row, { L"Source", L"Scope" }) + L" | " +
                rawValue(row, { L"Parent", L"Directory" }) + L" | " +
                rawValue(row, { L"Name" }) + L" | " +
                rawValue(row, { L"Type" }) + L" | " +
                rawValue(row, { L"Path" }) + L" | " +
                rawValue(row, { L"Target" }) + L" | " +
                rawValue(row, { L"Status" });
            const bool keywordMatched = primaryFilter.empty() || ContainsCaseInsensitive(merged, primaryFilter);
            const bool scopeMatched = scopeFilter.empty() ||
                _wcsicmp(rawValue(row, { L"Source", L"Scope", L"scope" }).c_str(), scopeFilter.c_str()) == 0;
            const bool typeMatched = typeFilter.empty() ||
                _wcsicmp(rawValue(row, { L"Type", L"objectType" }).c_str(), typeFilter.c_str()) == 0;
            const bool legacyMatched = secondaryFilter.empty() ||
                ContainsCaseInsensitive(rawValue(row, { L"Source", L"Scope" }), secondaryFilter) ||
                ContainsCaseInsensitive(rawValue(row, { L"Type" }), secondaryFilter);
            const bool auxMatched = scopeMatched && typeMatched && legacyMatched;
            return keywordMatched && auxMatched;
        }
        if (featureId == KernelFeatureId::NamedPipe) {
            const std::wstring merged = rawValue(row, { L"Pipe", L"Name" }) + L" | " +
                rawValue(row, { L"NtPath", L"Path" }) + L" | " +
                rawValue(row, { L"Attributes" }) + L" | " +
                rawValue(row, { L"LastWrite" }) + L" | " +
                rawValue(row, { L"Status" }) + L" | " +
                rawValue(row, { L"Directory", L"Source" });
            return primaryFilter.empty() || ContainsCaseInsensitive(merged, primaryFilter);
        }
        if (featureId == KernelFeatureId::ObjectTypeMatrix) {
            const std::wstring merged = rawValue(row, { L"TypeIndex", L"Index" }) + L" | " +
                rawValue(row, { L"Type" }) + L" | " +
                rawValue(row, { L"Objects" }) + L" | " +
                rawValue(row, { L"Handles" }) + L" | " +
                rawValue(row, { L"ValidAccess" }) + L" | " +
                rawValue(row, { L"R0Address" }) + L" | " +
                rawValue(row, { L"R0Validation" }) + L" | " +
                rawValue(row, { L"Detail", L"Status" });
            return primaryFilter.empty() || ContainsCaseInsensitive(merged, primaryFilter);
        }
        if (featureId == KernelFeatureId::CommunicationEndpoint) {
            const std::wstring merged = rawValue(row, { L"Source", L"Parent" }) + L" | " +
                rawValue(row, { L"Name" }) + L" | " +
                rawValue(row, { L"Type" }) + L" | " +
                rawValue(row, { L"Path" }) + L" | " +
                rawValue(row, { L"Status" });
            return primaryFilter.empty() || ContainsCaseInsensitive(merged, primaryFilter);
        }
        if (featureId != KernelFeatureId::SymbolicLink &&
            featureId != KernelFeatureId::DeviceDriverObjects) {
            return true;
        }
        if (featureId == KernelFeatureId::SymbolicLink) {
            const std::wstring merged = rawValue(row, { L"Source", L"Parent" }) + L" | " +
                rawValue(row, { L"Name" }) + L" | " +
                rawValue(row, { L"Path" }) + L" | " +
                rawValue(row, { L"Status" }) + L" | " +
                rawValue(row, { L"Target" }) + L" | " +
                rawValue(row, { L"Win32Path", L"DosCandidates" });
            const bool primaryMatched = primaryFilter.empty() || ContainsCaseInsensitive(merged, primaryFilter);
            const bool secondaryMatched = secondaryFilter.empty() ||
                ContainsCaseInsensitive(rawValue(row, { L"Target" }), secondaryFilter) ||
                ContainsCaseInsensitive(rawValue(row, { L"Win32Path", L"DosCandidates" }), secondaryFilter);
            return primaryMatched && secondaryMatched;
        }
        if (featureId == KernelFeatureId::DeviceDriverObjects) {
            std::wstring directoryFilter;
            std::wstring typeFilter;
            if (deviceDriverDirectoryCombo_) {
                const int selection = static_cast<int>(ChoiceMessage(deviceDriverDirectoryCombo_, CB_GETCURSEL, 0, 0));
                if (selection > 0) {
                    std::wstring buffer;
                    buffer = ChoiceText(deviceDriverDirectoryCombo_);
                    directoryFilter = buffer;
                }
            }
            if (deviceDriverTypeCombo_) {
                const int selection = static_cast<int>(ChoiceMessage(deviceDriverTypeCombo_, CB_GETCURSEL, 0, 0));
                if (selection > 0) {
                    std::wstring buffer;
                    buffer = ChoiceText(deviceDriverTypeCombo_);
                    typeFilter = buffer;
                }
            }
            const std::wstring directory = rawValue(row, { L"Parent", L"Directory", L"Source", L"directoryPath", L"目录路径" });
            const std::wstring type = rawValue(row, { L"Type", L"objectType", L"对象类型", L"类型" });
            const std::wstring keywordMerged = directory + L" | " +
                rawValue(row, { L"Name", L"DriverName", L"objectName", L"对象名称" }) + L" | " +
                type + L" | " +
                rawValue(row, { L"Path", L"NtPath", L"fullPath", L"完整路径" }) + L" | " +
                rawValue(row, { L"Target", L"targetPath", L"symbolicTarget", L"目标路径" }) + L" | " +
                rawValue(row, { L"Status", L"statusText", L"状态" }) + L" | " +
                rawValue(row, { L"Detail", L"Capability", L"capabilityHintText", L"能力提示" });
            const bool keywordMatched = primaryFilter.empty() || ContainsCaseInsensitive(keywordMerged, primaryFilter);
            const bool directoryMatched = directoryFilter.empty() || _wcsicmp(directory.c_str(), directoryFilter.c_str()) == 0;
            const bool typeMatched = typeFilter.empty() || _wcsicmp(type.c_str(), typeFilter.c_str()) == 0;
            return keywordMatched && directoryMatched && typeMatched;
        }
        return true;
    };
    const auto depthOf = [&](const std::vector<std::wstring>& row) -> int {
        const std::wstring text = rawValue(row, { L"Depth", L"深度" });
        if (text.empty()) {
            return 0;
        }
        wchar_t* end = nullptr;
        const long value = std::wcstol(text.c_str(), &end, 10);
        return end != text.c_str() && value > 0 ? static_cast<int>(std::min<long>(value, 16)) : 0;
    };
    std::vector<std::vector<std::wstring>> displayRows;
    std::vector<int> displayIndents;
    std::vector<std::wstring> insertedRoots;
    std::vector<std::wstring> insertedDirectories;
    std::size_t meaningfulRowCount = 0;
    const auto appendDisplayRow = [&](std::vector<std::wstring> cells, const int indent) {
        if (!cells.empty() && !displayColumns.empty() && displayColumns[0] == L"#") {
            cells[0] = std::to_wstring(displayRows.size());
        }
        displayRows.push_back(std::move(cells));
        displayIndents.push_back(std::max(0, indent));
    };
    const auto makeEmptyCells = [&]() {
        return std::vector<std::wstring>(displayColumns.size());
    };
    const auto appendOverviewRoot = [&](const std::wstring& root, const std::wstring& status) {
        if (root.empty()) {
            return;
        }
        const auto duplicate = std::find_if(insertedRoots.begin(), insertedRoots.end(), [&](const std::wstring& existing) {
            return _wcsicmp(existing.c_str(), root.c_str()) == 0;
        });
        if (duplicate != insertedRoots.end()) {
            return;
        }
        insertedRoots.push_back(root);
        std::vector<std::wstring> cells = makeEmptyCells();
        for (std::size_t column = 0; column < displayColumns.size(); ++column) {
            const std::wstring& name = displayColumns[column];
            if (name == L"名称") cells[column] = root;
            else if (name == L"类型") cells[column] = L"根目录";
            else if (name == L"路径/说明") cells[column] = status.empty() ? root : status;
            else if (name == L"状态") cells[column] = L"根节点";
            else if (name == L"Parent" || name == L"Source" || name == L"Path") cells[column] = root;
            else if (name == L"Depth") cells[column] = L"0";
            else if (name == L"NodeKind") cells[column] = L"Root";
            else if (name == L"Detail") cells[column] = L"Object Manager root: " + root;
        }
        appendDisplayRow(std::move(cells), 0);
    };
    const auto appendOverviewDirectory = [&](const std::wstring& root, const std::wstring& directory, const std::wstring& scope) {
        // appendOverviewDirectory reproduces the original middle tree level:
        // each root owns one row per enumerated directory, and object rows sit
        // under that directory. Inputs are root/scope/directory text from the
        // facade row; processing de-duplicates the pair; no value is returned.
        if (directory.empty()) {
            return;
        }
        const std::wstring key = root + L"\n" + directory;
        const auto duplicate = std::find_if(insertedDirectories.begin(), insertedDirectories.end(), [&](const std::wstring& existing) {
            return _wcsicmp(existing.c_str(), key.c_str()) == 0;
        });
        if (duplicate != insertedDirectories.end()) {
            return;
        }
        insertedDirectories.push_back(key);
        std::vector<std::wstring> cells = makeEmptyCells();
        for (std::size_t column = 0; column < displayColumns.size(); ++column) {
            const std::wstring& name = displayColumns[column];
            if (name == L"名称") cells[column] = LeafObjectName(directory);
            else if (name == L"类型") cells[column] = L"目录";
            else if (name == L"路径/说明") cells[column] = directory;
            else if (name == L"状态") cells[column] = L"已展开枚举";
            else if (name == L"Parent" || name == L"Source") cells[column] = root;
            else if (name == L"Directory" || name == L"directoryPath" || name == L"Path" || name == L"fullPath") cells[column] = directory;
            else if (name == L"Depth") cells[column] = L"1";
            else if (name == L"NodeKind") cells[column] = L"Directory";
            else if (name == L"Detail") cells[column] = scope.empty()
                ? (L"Object Manager directory: " + directory)
                : (L"Object Manager directory: " + directory + L"\r\n" + scope);
        }
        appendDisplayRow(std::move(cells), 1);
    };
    if (featureId == KernelFeatureId::ObjectDirectoryRecursive) {
        std::wstring rootPath = Text(filterEdit_);
        if (rootPath.empty()) {
            rootPath = L"\\";
        }
        std::vector<std::wstring> cells = makeEmptyCells();
        for (std::size_t column = 0; column < displayColumns.size(); ++column) {
            const std::wstring& name = displayColumns[column];
            if (name == L"名称") cells[column] = LeafObjectName(rootPath);
            else if (name == L"类型") cells[column] = L"Directory";
            else if (name == L"完整路径" || name == L"Path") cells[column] = rootPath;
            else if (name == L"深度" || name == L"Depth") cells[column] = L"0";
            else if (name == L"状态") cells[column] = L"根目录";
            else if (name == L"Parent" || name == L"Source") cells[column] = ParentObjectPath(rootPath).empty() ? rootPath : ParentObjectPath(rootPath);
            else if (name == L"Detail") cells[column] = L"Object Manager recursive root: " + rootPath;
        }
        appendDisplayRow(std::move(cells), 0);
    }

    for (const std::vector<std::wstring>& rawRow : rawRows) {
        const bool meaningful =
            HasValue(rawRow, rawColumns, { L"Name", L"Path", L"NtPath", L"Pipe", L"TypeIndex", L"Type" }) ||
            HasValue(rawRow, rawColumns, { L"Parent", L"Source", L"Directory", L"Target" });
        if (!meaningful) {
            continue;
        }
        ++meaningfulRowCount;
        const std::wstring parent = rawValue(rawRow, { L"Parent", L"Directory", L"sourceDirectory", L"来源目录", L"目录路径" });
        const std::wstring source = rawValue(rawRow, { L"Source", L"scope" });
        const std::wstring path = rawValue(rawRow, { L"Path", L"NtPath", L"fullPath", L"完整路径" });
        const std::wstring objectName = rawValue(rawRow, { L"Name", L"Pipe", L"objectName", L"linkName", L"名称", L"对象名称", L"Type" });
        const std::wstring objectType = rawValue(rawRow, { L"Type", L"objectType", L"类型", L"对象类型" });
        const bool isQueryHeader = !HasValue(rawRow, rawColumns, { L"Name", L"Pipe", L"objectName", L"linkName", L"TypeIndex" }) &&
            HasValue(rawRow, rawColumns, { L"功能", L"数据源", L"Rows", L"Warnings" });
        if (isQueryHeader) {
            continue;
        }
        if (!rowMatchesObjectFilters(rawRow)) {
            continue;
        }

        if (featureId == KernelFeatureId::ObjectNamespaceOverview) {
            const std::wstring directory = !parent.empty() ? parent : (!rawValue(rawRow, { L"directoryPathText", L"directoryPath", L"Directory" }).empty()
                ? rawValue(rawRow, { L"directoryPathText", L"directoryPath", L"Directory" })
                : ParentObjectPath(path));
            const std::wstring root = !source.empty() ? source : (!rawValue(rawRow, { L"rootPathText", L"Root" }).empty()
                ? rawValue(rawRow, { L"rootPathText", L"Root" })
                : (!directory.empty() ? directory : path));
            const std::wstring scope = rawValue(rawRow, { L"scopeDescriptionText", L"Scope", L"scope", L"Detail" });
            if (Text(filterEdit_).empty() ||
                ContainsCaseInsensitive(root, Text(filterEdit_)) ||
                ContainsCaseInsensitive(directory, Text(filterEdit_)) ||
                ContainsCaseInsensitive(scope, Text(filterEdit_)) ||
                ContainsCaseInsensitive(path, Text(filterEdit_)) ||
                ContainsCaseInsensitive(objectName, Text(filterEdit_)) ||
                ContainsCaseInsensitive(objectType, Text(filterEdit_)) ||
                ContainsCaseInsensitive(rawValue(rawRow, { L"Status" }), Text(filterEdit_)) ||
                ContainsCaseInsensitive(rawValue(rawRow, { L"Target" }), Text(filterEdit_))) {
                appendOverviewRoot(root, scope);
                appendOverviewDirectory(root, directory, scope);
            }
            if (objectName.empty() && objectType.empty() && parent.empty()) {
                continue;
            }
        }

        std::vector<std::wstring> cells = makeEmptyCells();
        int rowIndent = 0;
        if (featureId == KernelFeatureId::ObjectNamespaceOverview) {
            rowIndent = 2;
        } else if (featureId == KernelFeatureId::ObjectDirectoryRecursive) {
            rowIndent = depthOf(rawRow) + 1;
        }
        for (std::size_t column = 0; column < displayColumns.size(); ++column) {
            const std::wstring& name = displayColumns[column];
            if (name == L"#") {
                continue;
            }
            std::wstring value = displayValue(rawRow, name);
            if (featureId == KernelFeatureId::ObjectNamespaceOverview && name == L"名称") {
                value = objectName.empty() ? path : objectName;
            } else if (featureId == KernelFeatureId::ObjectNamespaceOverview && name == L"类型" && value.empty()) {
                value = objectType.empty() ? L"<未知>" : objectType;
            } else if (featureId == KernelFeatureId::ObjectNamespaceOverview && name == L"路径/说明" && value.empty()) {
                value = path;
            } else if (featureId == KernelFeatureId::ObjectNamespaceOverview && name == L"NodeKind") {
                value = L"ObjectEntry";
            } else if (featureId == KernelFeatureId::ObjectDirectoryRecursive && name == L"名称") {
                value = objectName.empty() ? path : objectName;
            } else if (featureId == KernelFeatureId::NamedPipe && name == L"Pipe Name" && value.empty()) {
                const std::wstring ntPath = rawValue(rawRow, { L"NtPath", L"Path" });
                const std::size_t slash = ntPath.find_last_of(L'\\');
                value = slash == std::wstring::npos ? ntPath : ntPath.substr(slash + 1);
            } else if (featureId == KernelFeatureId::DeviceDriverObjects && name == L"目标路径" && value.empty()) {
                value = L"<无>";
            } else if (name == L"Detail" && value.empty()) {
                value = rawValue(rawRow, { L"Status", L"statusText", L"Target", L"symbolicTarget", L"targetPath" });
            }
            cells[column] = std::move(value);
        }
        appendDisplayRow(std::move(cells), rowIndent);
    }

    currentColumns_ = std::move(displayColumns);
    currentRows_ = std::move(displayRows);
    ClearTables();
    for (std::size_t index = 0; index < currentColumns_.size(); ++index) {
        const std::wstring& columnName = currentColumns_[index];
        const bool hidden = columnName == L"Parent" ||
            columnName == L"Source" ||
            columnName == L"Directory" ||
            columnName == L"directoryPath" ||
            columnName == L"sourceDirectory" ||
            columnName == L"Depth" ||
            columnName == L"Path" ||
            columnName == L"NtPath" ||
            columnName == L"Target" ||
            columnName == L"Win32Path" ||
            columnName == L"dosCandidate" ||
            columnName == L"fullPath" ||
            columnName == L"targetPath" ||
            columnName == L"symbolicTarget" ||
            columnName == L"objectName" ||
            columnName == L"objectType" ||
            columnName == L"linkName" ||
            columnName == L"Handles" ||
            columnName == L"Pointers" ||
            columnName == L"EnumApi" ||
            columnName == L"enumApi" ||
            columnName == L"枚举 API" ||
            columnName == L"EnumerationApi" ||
            columnName == L"NodeKind" ||
            columnName == L"Detail";
        int width = hidden ? 0 : ColumnWidth(columnName);
        if (featureId == KernelFeatureId::DeviceDriverObjects) {
            if (columnName == L"目录路径") width = 190;
            else if (columnName == L"对象名称") width = 180;
            else if (columnName == L"对象类型") width = 130;
            else if (columnName == L"完整路径") width = 320;
            else if (columnName == L"目标路径") width = 320;
            else if (columnName == L"状态") width = 260;
            else if (columnName == L"能力提示") width = 360;
        }
        SetColumn(static_cast<int>(index), columnName, width);
    }
    currentRowIndents_ = std::move(displayIndents);
    CommitRows();
    if (featureId == KernelFeatureId::ObjectNamespaceOverview ||
        featureId == KernelFeatureId::ObjectDirectoryRecursive) {
        RequireTreeValueModel();
    }
    std::wostringstream status;
    switch (featureId) {
    case KernelFeatureId::ObjectNamespaceOverview:
    {
        const std::wstring keyword = Text(filterEdit_);
        status << L"状态：对象命名空间已刷新，显示 " << currentRows_.size()
               << L" / " << meaningfulRowCount << L" 项";
        if (!keyword.empty()) {
            status << L"，过滤=" << keyword;
        }
        break;
    }
    case KernelFeatureId::ObjectDirectoryRecursive:
    {
        std::size_t directoryCount = 0;
        std::size_t failedDirectoryCount = 0;
        bool depthLimitReached = false;
        for (const std::vector<std::wstring>& row : currentRows_) {
            const std::wstring type = ReadField(row, currentColumns_, { L"类型", L"Type", L"objectType" });
            const std::wstring rowStatus = ReadField(row, currentColumns_, { L"状态", L"Status", L"statusText" });
            if (_wcsicmp(type.c_str(), L"Directory") == 0) {
                ++directoryCount;
                if (ContainsCaseInsensitive(rowStatus, L"失败") ||
                    ContainsCaseInsensitive(rowStatus, L"denied") ||
                    ContainsCaseInsensitive(rowStatus, L"error") ||
                    ContainsCaseInsensitive(rowStatus, L"0xC")) {
                    ++failedDirectoryCount;
                }
            }
            if (ContainsCaseInsensitive(rowStatus, L"深度") ||
                ContainsCaseInsensitive(rowStatus, L"depth")) {
                depthLimitReached = true;
            }
        }
        status << L"状态：已枚举 " << currentRows_.size()
               << L" 项，目录 " << directoryCount
               << L" 个，失败目录 " << failedDirectoryCount << L" 个";
        if (depthLimitReached) {
            status << L"，触达深度上限";
        }
        break;
    }
    case KernelFeatureId::NamedPipe:
    {
        const std::wstring keyword = Text(filterEdit_);
        status << L"状态：显示 " << currentRows_.size() << L" / " << meaningfulRowCount;
        if (!keyword.empty()) {
            status << L"，过滤=" << keyword;
        }
        break;
    }
    case KernelFeatureId::BaseNamedObjects:
    {
        std::wstring scopeText = L"全部 Session";
        std::wstring typeText = L"全部类型";
        if (baseNamedScopeCombo_) {
            const int selection = static_cast<int>(ChoiceMessage(baseNamedScopeCombo_, CB_GETCURSEL, 0, 0));
            if (selection >= 0) {
                std::wstring buffer;
                buffer = ChoiceText(baseNamedScopeCombo_);
                if (!buffer.empty()) {
                    scopeText = buffer;
                }
            }
        }
        if (baseNamedTypeCombo_) {
            const int selection = static_cast<int>(ChoiceMessage(baseNamedTypeCombo_, CB_GETCURSEL, 0, 0));
            if (selection >= 0) {
                std::wstring buffer;
                buffer = ChoiceText(baseNamedTypeCombo_);
                if (!buffer.empty()) {
                    typeText = buffer;
                }
            }
        }
        const std::wstring keyword = Text(filterEdit_);
        status << L"状态：BaseNamedObjects 已刷新，总数 " << meaningfulRowCount
               << L"，可见 " << currentRows_.size()
               << L"，scope=" << scopeText
               << L"，类型=" << typeText;
        if (!keyword.empty()) {
            status << L"，关键字=" << keyword;
        }
        break;
    }
    case KernelFeatureId::SymbolicLink:
    {
        const std::wstring keyword = Text(filterEdit_);
        const std::wstring targetKeyword = Text(moduleFilterEdit_);
        status << L"状态：显示 " << currentRows_.size() << L" / " << meaningfulRowCount;
        if (!keyword.empty()) {
            status << L"，过滤=" << keyword;
        }
        if (!targetKeyword.empty()) {
            status << L"，目标过滤=" << targetKeyword;
        }
        break;
    }
    case KernelFeatureId::DeviceDriverObjects:
    {
        const std::size_t loadedRows = meaningfulRowCount;
        const std::size_t visibleRows = currentRows_.size();
        if (loadedRows == 0) {
            status << L"状态：暂无结果，点击刷新开始枚举对象目录。";
        } else if (visibleRows == 0) {
            status << L"状态：已加载 " << loadedRows << L" 条，当前过滤后无可见结果。";
        } else {
            status << L"状态：已加载 " << loadedRows << L" 条，当前显示 " << visibleRows << L" 条。";
        }
        break;
    }
    case KernelFeatureId::ObjectTypeMatrix:
    {
        const std::wstring keyword = Text(filterEdit_);
        status << L"状态：已加载 " << meaningfulRowCount << L" 类，显示 " << currentRows_.size() << L" 类";
        if (!keyword.empty()) {
            status << L"，过滤=" << keyword;
        }
        break;
    }
    case KernelFeatureId::CommunicationEndpoint:
    {
        const std::wstring keyword = Text(filterEdit_);
        status << L"状态：已加载 " << meaningfulRowCount << L" 项，显示 " << currentRows_.size() << L" 项";
        if (!keyword.empty()) {
            status << L"，过滤=" << keyword;
        }
        break;
    }
    default:
        status << L"状态：对象页已刷新，显示 " << currentRows_.size() << L" 项";
        break;
    }
    WriteText(statusText_, status.str().c_str());
    if (!currentRows_.empty()) {
        SetSelection(resultList_, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        EnsureItem(resultList_, 0, FALSE);
        SelectionDetail();
    } else {
        WriteText(detailEdit_, EmptyDetailTextForFeature(featureId));
    }
}

void RebuildObjectNamespaceTreeFromCurrentRows() {
    // RebuildObjectNamespaceTreeFromCurrentRows renders the original object
    // namespace overview as a three-level Win32 TreeView. Inputs are
    // currentRows_ and currentColumns_ already projected to the original
    // root/directory/object schema; processing creates stable node payloads in
    // objectNamespaceTreeNodeStorage_ and binds them to TVITEM::lParam; no value
    // is returned.
    result_->tree=true;result_->treeNodes.clear();
    std::unordered_map<KernelObjectNamespaceTreeNodeState*,std::shared_ptr<KernelObjectNamespaceTreeNodeState>> owners;
    std::unordered_map<std::wstring, std::size_t> rootItems;
    std::unordered_map<std::wstring, std::size_t> directoryItems;
    std::size_t firstItem = 0;
    std::size_t firstEntryItem = 0;
    const bool expandAll = !Text(filterEdit_).empty();
    const auto rowText = [&](const std::vector<std::wstring>& row, std::initializer_list<const wchar_t*> names) {
        return ReadField(row, currentColumns_, names);
    };
    const auto safeText = [](const std::wstring& value, const wchar_t* fallback = L"<空>") -> std::wstring {
        return value.empty() ? std::wstring(fallback) : value;
    };
    const auto rootFromPath = [](const std::wstring& path) -> std::wstring {
        if (path.empty()) {
            return L"\\";
        }
        if (path == L"\\") {
            return path;
        }
        const std::size_t nextSlash = path.find(L'\\', 1);
        return nextSlash == std::wstring::npos ? path : path.substr(0, nextSlash);
    };
    const auto makeState = [&](const int rowIndex,
        std::wstring kind,
        std::wstring name,
        std::wstring type,
        std::wstring path,
        std::wstring description) -> KernelObjectNamespaceTreeNodeState* {
        auto state = std::make_shared<KernelObjectNamespaceTreeNodeState>();
        state->rowIndex = rowIndex;
        state->nodeKind = std::move(kind);
        state->nodeName = std::move(name);
        state->nodeType = std::move(type);
        state->nodePath = std::move(path);
        state->nodeDescription = std::move(description);
        KernelObjectNamespaceTreeNodeState* raw = state.get();
        owners.emplace(raw,std::move(state));
        return raw;
    };
    const auto insertTreeItem = [&](std::size_t parent,
        const std::wstring& text,
        KernelObjectNamespaceTreeNodeState* state) -> std::size_t {
        if (text.empty()) {
            return 0;
        }
        result_->treeNodes.push_back(KernelProjectedTreeNode{parent,text,owners.at(state),false});
        return result_->treeNodes.size();
    };

    const auto ensureRoot = [&](std::wstring rootPath, const std::wstring& description) -> std::size_t {
        if (rootPath.empty()) {
            rootPath = L"\\";
        }
        const std::wstring key = LowerInvariantKey(rootPath);
        if (const auto found = rootItems.find(key); found != rootItems.end()) {
            return found->second;
        }
        std::wstring text = rootPath + L"    [根目录]    " + safeText(description, L"<无>");
        KernelObjectNamespaceTreeNodeState* state = makeState(
            -1,
            L"Root",
            rootPath,
            L"根目录",
            rootPath,
            description);
        std::size_t item = insertTreeItem(0, text, state);
        if (item != 0) {
            rootItems.emplace(key, item);
            if (firstItem == 0) {
                firstItem = item;
            }
            ExpandTreeValue(item);
        }
        return item;
    };
    const auto ensureDirectory = [&](std::wstring rootPath,
        std::wstring directoryPath,
        const std::wstring& description) -> std::size_t {
        if (directoryPath.empty()) {
            directoryPath = rootPath.empty() ? L"\\" : rootPath;
        }
        if (rootPath.empty() || rootPath[0] != L'\\') {
            rootPath = rootFromPath(directoryPath);
        }
        std::size_t rootItem = ensureRoot(rootPath, description);
        const std::wstring key = LowerInvariantKey(rootPath + L"\n" + directoryPath);
        if (const auto found = directoryItems.find(key); found != directoryItems.end()) {
            return found->second;
        }
        std::wstring text = LeafObjectName(directoryPath) + L"    [目录]    " + directoryPath;
        KernelObjectNamespaceTreeNodeState* state = makeState(
            -1,
            L"Directory",
            LeafObjectName(directoryPath),
            L"目录",
            directoryPath,
            description);
        std::size_t item = insertTreeItem(rootItem, text, state);
        if (item != 0) {
            directoryItems.emplace(key, item);
            if (firstItem == 0) {
                firstItem = item;
            }
            if (rootItem != 0) {
                ExpandTreeValue(rootItem);
            }
            if (expandAll) {
                ExpandTreeValue(item);
            }
        }
        return item;
    };

    for (std::size_t rowIndex = 0; rowIndex < currentRows_.size(); ++rowIndex) {
        const std::vector<std::wstring>& row = currentRows_[rowIndex];
        std::wstring nodeKind = rowText(row, { L"NodeKind" });
        const std::wstring name = rowText(row, { L"名称", L"Name", L"objectName" });
        const std::wstring type = rowText(row, { L"类型", L"Type", L"objectType" });
        const std::wstring path = rowText(row, { L"Path", L"完整路径", L"fullPath", L"路径/说明" });
        const std::wstring directory = rowText(row, { L"Directory", L"directoryPath", L"Parent", L"sourceDirectory", L"来源目录" });
        std::wstring root = rowText(row, { L"rootPathText", L"Root", L"Source" });
        const std::wstring description = rowText(row, { L"scopeDescriptionText", L"Scope", L"scope", L"Detail", L"路径/说明" });
        const std::wstring status = rowText(row, { L"状态", L"Status", L"statusText" });
        const std::wstring target = rowText(row, { L"符号链接目标", L"Target", L"symbolicTarget", L"targetPath" });
        if (name.empty() && path.empty()) {
            continue;
        }

        if (nodeKind.empty()) {
            if (_wcsicmp(type.c_str(), L"根目录") == 0 || _wcsicmp(status.c_str(), L"根节点") == 0) {
                nodeKind = L"Root";
            } else if (_wcsicmp(type.c_str(), L"目录") == 0 || _wcsicmp(status.c_str(), L"已展开枚举") == 0) {
                nodeKind = L"Directory";
            } else {
                nodeKind = L"ObjectEntry";
            }
        }

        if (_wcsicmp(nodeKind.c_str(), L"Root") == 0) {
            ensureRoot(path.empty() ? name : path, description);
            continue;
        }
        if (_wcsicmp(nodeKind.c_str(), L"Directory") == 0) {
            const std::wstring directoryPath = path.empty() ? directory : path;
            if (root.empty() || root[0] != L'\\') {
                root = rootFromPath(directoryPath);
            }
            ensureDirectory(root, directoryPath, description);
            continue;
        }

        std::wstring directoryPath = !directory.empty() ? directory : ParentObjectPath(path);
        if (root.empty() || root[0] != L'\\') {
            root = rootFromPath(!directoryPath.empty() ? directoryPath : path);
        }
        std::size_t directoryItem = ensureDirectory(root, directoryPath, description);
        std::wstring displayText = safeText(name, L"<未命名对象>");
        displayText += L"    [" + safeText(type) + L"]    " + safeText(path);
        if (!target.empty() && target != L"<无>") {
            displayText += L"    -> " + target;
        } else if (!status.empty()) {
            displayText += L"    " + status;
        }
        KernelObjectNamespaceTreeNodeState* state = makeState(
            static_cast<int>(rowIndex),
            L"ObjectEntry",
            name.empty() ? L"<未命名对象>" : name,
            type,
            path,
            description);
        const std::size_t item = insertTreeItem(directoryItem, displayText, state);
        if (item == 0) {
            continue;
        }
        if (firstItem == 0) {
            firstItem = item;
        }
        if (firstEntryItem == 0) {
            firstEntryItem = item;
        }
        if (expandAll && directoryItem != 0) {
            ExpandTreeValue(directoryItem);
        }
    }

    if (firstEntryItem != 0) {
        SelectTreeValue( firstEntryItem);
    } else if (firstItem != 0) {
        SelectTreeValue( firstItem);
    }
}
};
} // namespace
