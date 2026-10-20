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
    }
    std::shared_ptr<KernelValueProjection> build(const std::uint64_t requestId) {
        result_=std::make_shared<KernelValueProjection>();
        result_->input=input_;
        result_->requestId=requestId;
        const auto feature=input_->source->featureId;
        switch(feature) {
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
        result_->columns=std::move(currentColumns_);
        result_->rows=std::move(currentRows_);
        result_->indents=std::move(currentRowIndents_);
        for(std::size_t index=0;index<result_->rows.size();++index) {
            result_->rowHashes[KernelProjectedRowHash(result_->rows[index])].push_back(index);
        }
        return result_;
    }
private:
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
    std::vector<std::vector<std::wstring>> currentRows_;
    std::vector<int> currentRowIndents_;
    using ColumnIndex=std::map<std::wstring,std::vector<std::size_t>,InsensitiveLess>;
    std::unordered_map<const std::vector<std::wstring>*,ColumnIndex> columnIndexes_;
    std::map<std::pair<KernelFeatureId,std::wstring>,std::vector<std::wstring>> aliases_;
    const KernelValueControl resultList_{5},filterEdit_{1},moduleFilterEdit_{2},statusText_{3},detailEdit_{4},riskOnlyCheck_{6},summaryList_{7};
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
};
} // namespace
