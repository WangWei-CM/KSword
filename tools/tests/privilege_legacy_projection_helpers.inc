static bool fixtureEnglish = false;
QString privilegeText(const char*, const QString& source) { return fixtureEnglish ? QStringLiteral("EN:") + source : source; }
    QString boolText(const bool value)
    {
        return value
            ? privilegeText("privilege.bool.yes", QStringLiteral("是"))
            : privilegeText("privilege.bool.no", QStringLiteral("否"));
    }

    class LegacyHeaderProxy final : public QSortFilterProxyModel
    {
    public:
        LegacyHeaderProxy(std::function<QString(int)> header, QObject* parent)
            : QSortFilterProxyModel(parent), m_header(std::move(header)) {}
        QVariant data(const QModelIndex& index, int role) const override
        {
            // Business strings and explicitly localized cells must bypass source-text translation.
            return QSortFilterProxyModel::data(index,
                role == Qt::DisplayRole || role == Qt::ToolTipRole ? Qt::UserRole + 1 : role);
        }
        QVariant headerData(int section, Qt::Orientation orientation, int role) const override
        {
            if (orientation == Qt::Horizontal && role == Qt::DisplayRole) return m_header(section);
            return QSortFilterProxyModel::headerData(section, orientation, role);
        }
        void retranslate() { emit headerDataChanged(Qt::Horizontal, 0, 3); }
    private:
        std::function<QString(int)> m_header;
    };

class PrivilegeDock { public:
    struct PermissionSnapshotRow
    {
        enum class Kind { User, Group, Privilege, UserError, GroupError, TokenError, TokenEmpty };
        Kind kind = Kind::User;
        ks::privilege::legacy::LocalUser user;
        ks::privilege::legacy::LocalGroup group;
        ks::privilege::legacy::TokenPrivilege privilege;
        ks::privilege::legacy::ReadStatus error;
    };
    struct LegacyReadResult
    {
        std::shared_ptr<ks::privilege::legacy::ReadSnapshot> snapshot;
        std::vector<PermissionSnapshotRow> rows;
        QDateTime capturedAt;
    };

static std::shared_ptr<LegacyReadResult> makeReadResult(std::shared_ptr<ks::privilege::legacy::ReadSnapshot>);
QVariant accountCell(const ks::privilege::legacy::LocalUser&,int,int) const;
QVariant permissionCell(const PermissionSnapshotRow&,int) const;
QString fileTimeToDateTimeText(std::uint32_t) const;
QString readErrorText(const ks::privilege::legacy::ReadStatus&) const;
};
std::shared_ptr<PrivilegeDock::LegacyReadResult> PrivilegeDock::makeReadResult(
    std::shared_ptr<ks::privilege::legacy::ReadSnapshot> snapshot)
{
    using namespace ks::privilege::legacy;
    auto result = std::make_shared<LegacyReadResult>();
    result->snapshot = std::move(snapshot);
    result->capturedAt = QDateTime::currentDateTime();
    const auto& source = *result->snapshot;
    if (!source.groupStatus.requested && !source.tokenStatus.requested) return result;
    result->rows.reserve(source.users.size() + source.groups.size() + source.privileges.size() + 3);
    for (const auto& user : source.users)
    {
        PermissionSnapshotRow row;
        row.kind = PermissionSnapshotRow::Kind::User; row.user = user;
        result->rows.push_back(std::move(row));
    }
    if (source.userStatus.requested && !source.userStatus.complete)
    {
        PermissionSnapshotRow row; row.kind = PermissionSnapshotRow::Kind::UserError;
        row.error = source.userStatus; result->rows.push_back(std::move(row));
    }
    for (const auto& group : source.groups)
    {
        PermissionSnapshotRow row;
        row.kind = PermissionSnapshotRow::Kind::Group; row.group = group;
        result->rows.push_back(std::move(row));
    }
    if (source.groupStatus.requested && !source.groupStatus.complete)
    {
        PermissionSnapshotRow row; row.kind = PermissionSnapshotRow::Kind::GroupError;
        row.error = source.groupStatus; result->rows.push_back(std::move(row));
    }
    for (const auto& privilege : source.privileges)
    {
        PermissionSnapshotRow row;
        row.kind = PermissionSnapshotRow::Kind::Privilege; row.privilege = privilege;
        result->rows.push_back(std::move(row));
    }
    if (source.tokenStatus.requested && !source.tokenStatus.complete)
    {
        PermissionSnapshotRow row; row.kind = PermissionSnapshotRow::Kind::TokenError;
        row.error = source.tokenStatus; result->rows.push_back(std::move(row));
    }
    else if (source.tokenStatus.requested && source.privileges.empty())
    {
        PermissionSnapshotRow row; row.kind = PermissionSnapshotRow::Kind::TokenEmpty;
        result->rows.push_back(std::move(row));
    }
    return result;
}

QVariant PrivilegeDock::accountCell(const ks::privilege::legacy::LocalUser& entry, const int column, const int role) const
{

            if (column == 0 && role == Qt::UserRole) return entry.name;
            if (role != Qt::DisplayRole && role != Qt::UserRole + 1) return {};
            switch (column)
            {
            case 0: return entry.name;
            case 1: return entry.fullName;
            case 2: return boolText((entry.flags & UF_ACCOUNTDISABLE) != 0);
            case 3: return fileTimeToDateTimeText(entry.lastLogon);
            default: return {};
            }

}

QString PrivilegeDock::readErrorText(const ks::privilege::legacy::ReadStatus& status) const
{
    return status.nativeError.isEmpty() || status.nativeError == QString::number(status.code)
        ? privilegeText("privilege.error.code", QStringLiteral("错误码 %1")).arg(status.code)
        : status.nativeError;
}

QVariant PrivilegeDock::permissionCell(const PermissionSnapshotRow& row, const int column) const
{
    using Kind = PermissionSnapshotRow::Kind;
    if (column == 0)
    {
        if (row.kind == Kind::User || row.kind == Kind::UserError)
            return privilegeText("privilege.row.type.user", QStringLiteral("用户"));
        if (row.kind == Kind::Group || row.kind == Kind::GroupError)
            return privilegeText("privilege.row.type.group", QStringLiteral("组"));
        return privilegeText("privilege.row.type.process_privilege", QStringLiteral("进程权限"));
    }
    if (row.kind == Kind::User)
    {
        if (column == 1) return row.user.name;
        if (column == 2) return privilegeText("privilege.row.user.status", QStringLiteral("禁用:%1  最后登录:%2"))
            .arg(boolText((row.user.flags & UF_ACCOUNTDISABLE) != 0)).arg(fileTimeToDateTimeText(row.user.lastLogon));
        if (column == 3) return row.user.fullName.isEmpty()
            ? privilegeText("privilege.row.no_full_name", QStringLiteral("<无全名>"))
            : privilegeText("privilege.row.full_name", QStringLiteral("全名:%1")).arg(row.user.fullName);
    }
    else if (row.kind == Kind::Group)
    {
        if (column == 1) return row.group.name;
        if (column == 2) return privilegeText("privilege.row.group.local", QStringLiteral("本地组"));
        if (column == 3) return row.group.comment.isEmpty()
            ? privilegeText("privilege.row.no_description", QStringLiteral("<无说明>")) : row.group.comment;
    }
    else if (row.kind == Kind::Privilege)
    {
        if (column == 1) return row.privilege.nameKnown ? row.privilege.name
            : privilegeText("privilege.row.lookup_privilege_name_failed", QStringLiteral("<LookupPrivilegeName失败>"));
        if (column == 2) return privilegeText("privilege.row.process_privilege.status", QStringLiteral("启用:%1 默认:%2 移除:%3"))
            .arg(boolText((row.privilege.attributes & SE_PRIVILEGE_ENABLED) != 0))
            .arg(boolText((row.privilege.attributes & SE_PRIVILEGE_ENABLED_BY_DEFAULT) != 0))
            .arg(boolText((row.privilege.attributes & SE_PRIVILEGE_REMOVED) != 0));
        if (column == 3) return QStringLiteral("%1, Attributes=0x%2")
            .arg(row.privilege.displayNameKnown ? row.privilege.displayName
                : privilegeText("privilege.row.no_display_name", QStringLiteral("<无显示名>")))
            .arg(QString::number(row.privilege.attributes,16).toUpper());
    }
    else if (row.kind == Kind::TokenEmpty)
    {
        if (column == 1) return privilegeText("privilege.row.empty", QStringLiteral("<空>"));
        if (column == 2) return privilegeText("privilege.row.no_privileges", QStringLiteral("无权限项"));
        if (column == 3) return privilegeText("privilege.row.no_privileges.detail", QStringLiteral("当前进程令牌未返回任何权限条目。"));
    }
    else
    {
        if (column == 1) return privilegeText("privilege.row.read_failed", QStringLiteral("<读取失败>"));
        if (column == 2) return privilegeText("privilege.row.error", QStringLiteral("错误"));
        if (column == 3)
        {
            const QString error = readErrorText(row.error);
            if (row.kind != Kind::TokenError) return error;
            return row.error.stage == ks::privilege::legacy::FailureStage::OpenToken
                ? privilegeText("privilege.error.open_process_token", QStringLiteral("OpenProcessToken失败: %1")).arg(error)
                : privilegeText("privilege.error.get_token_information", QStringLiteral("GetTokenInformation失败: %1")).arg(error);
        }
    }
    return {};
}


QString PrivilegeDock::fileTimeToDateTimeText(const std::uint32_t secondsSince1970) const
{
    if (secondsSince1970 == 0)
    {
        return privilegeText("privilege.time.never", QStringLiteral("从未"));
    }
    return QDateTime::fromSecsSinceEpoch(secondsSince1970).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}
