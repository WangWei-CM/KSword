#pragma once

#include <QDialog>
#include <QString>

#include <cstdint>
#include <string>

namespace ks::ui { class AsyncOperation; }
namespace ksword::ark { struct DriverObjectQueryResult; struct DriverDispatchControlResult; }

class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

// 通用 DriverObject.MajorFunction 编辑器。
// UI 负责醒目告知后果；R0 协议不限制目标驱动类型或目标地址归属。
class KernelDriverDispatchEditorDialog final : public QDialog
{
public:
    explicit KernelDriverDispatchEditorDialog(
        const QString& driverObjectName,
        QWidget* parent = nullptr);
    ~KernelDriverDispatchEditorDialog() override;

private:
    void initializeUi();
    void refreshDriverSnapshot();
    bool refreshSelectedTransaction();
    void refreshSelectedTransactionAsync();
    void applyDriverSnapshot(const ksword::ark::DriverObjectQueryResult& result);
    bool applyTransactionResult(const ksword::ark::DriverDispatchControlResult& result);
    struct SlotQuery
    {
        std::uint64_t moduleBase = 0;
        std::uint64_t driverObject = 0;
        std::uint32_t major = 0;
        std::wstring driverName;
    };
    bool isCurrentReadTarget(const SlotQuery& request) const;
    void applySelectedDispatch();
    void restoreSelectedDispatch();
    void abandonSelectedRecord();
    void setActionsEnabled(bool enabled);
    void setStatus(const QString& text, const QString& colorHex);

    int selectedRow() const;
    std::uint32_t selectedMajorFunction() const;

    static QString pointerText(std::uint64_t address);
    static QString ntStatusText(long status);
    static QString majorFunctionName(std::uint32_t majorFunction);
    static bool parsePointer(const QString& text, std::uint64_t& addressOut);

    ks::ui::AsyncOperation* m_readOperation = nullptr;
    bool m_identityRefreshPending = false;
    bool m_mutationBusy = false;
    QString m_requestedDriverName;
    QString m_canonicalDriverName;
    QLabel* m_riskLabel = nullptr;
    QLabel* m_identityLabel = nullptr;
    QLabel* m_statusLabel = nullptr;
    QTableWidget* m_table = nullptr;
    QLineEdit* m_desiredAddressEdit = nullptr;
    QPushButton* m_refreshButton = nullptr;
    QPushButton* m_querySlotButton = nullptr;
    QPushButton* m_applyButton = nullptr;
    QPushButton* m_restoreButton = nullptr;
    QPushButton* m_abandonButton = nullptr;

    std::uint64_t m_moduleBase = 0;
    std::uint64_t m_driverObjectAddress = 0;
    std::uint64_t m_currentDispatchAddress = 0;
    std::uint32_t m_generation = 0;
    std::uint32_t m_responseFlags = 0;
};
