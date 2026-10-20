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
        RebuildKernelHookListFromCache(input_->source->featureId);
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
    const KernelValueControl resultList_{5},filterEdit_{1},moduleFilterEdit_{2},statusText_{3},detailEdit_{4};
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
};
} // namespace
