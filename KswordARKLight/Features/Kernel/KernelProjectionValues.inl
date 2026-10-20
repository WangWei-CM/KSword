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
    const KernelValueControl resultList_{5},filterEdit_{1},moduleFilterEdit_{2},statusText_{3},detailEdit_{4},riskOnlyCheck_{6};
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
};
} // namespace
