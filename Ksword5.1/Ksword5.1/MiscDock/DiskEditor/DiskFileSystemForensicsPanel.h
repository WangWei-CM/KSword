#pragma once

#include "DiskDeletedEntryForensics.h"
#include "DiskFileSystemForensics.h"
#include "DiskRawFileSystemBrowser.h"

#include <QWidget>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTableView;
namespace ks::ui { template<typename RowT> class FlatTableModel; }
class QAction;
class QToolButton;

namespace ks::misc
{
    struct DiskForensicsSelection
    {
        int diskIndex = -1;
        unsigned long backend = 1UL;
        std::uint64_t partitionOffset = 0;
        std::uint64_t partitionLength = 0;
        std::uint32_t logicalSectorSize = 512U;
        std::uint32_t backendMask = 1U;
        std::uint32_t capabilityFlags = 0U;
        QString displayText;
    };

    class DiskFileSystemForensicsPanel final : public QWidget
    {
    public:
        using SelectionProvider =
            std::function<std::optional<DiskForensicsSelection>()>;
        using JumpCallback = std::function<void(std::uint64_t)>;

        DiskFileSystemForensicsPanel(
            SelectionProvider selectionProvider,
            JumpCallback jumpCallback,
            QWidget* parent = nullptr);

    private:
        void initializeUi();
        void initializeConnections();
        void probeCurrentPartition();
        void resolveCurrentFile();
        void reverseLookupCurrentCluster();
        void scanDeletedEntries();
        void eraseSelectedDeletedEntry();
        void browseRawDirectory();
        void previewSelectedRawFile();
        void exportSelectedRawFile();
        void analyzeSelectedRawPe();
        void applyProbeResult(
            DiskForensicsSelection selection,
            FileSystemProbeResult result);
        void applyExtentResult(FileExtentResult result);
        void applyReverseResult(ReverseClusterResult result);
        void applyDeletedResult(
            DiskForensicsSelection selection,
            DeletedEntryScanResult result);
        void applyEraseResult(ExtentEraseResult result);
        void applyRawDirectoryResult(
            DiskForensicsSelection selection,
            RawDirectoryResult result);
        void applyRawReadResult(RawFileReadResult result);
        void applyRawExportResult(RawFileExportResult result);

        const RawFileEntry* selectedRawEntry() const;
        const DeletedDirectoryEntry* selectedDeletedEntry() const;

        SelectionProvider m_selectionProvider;
        JumpCallback m_jumpCallback;
        QPushButton* m_probeButton = nullptr;
        QLabel* m_probeSummaryLabel = nullptr;
        QTableWidget* m_probeTable = nullptr;
        QLineEdit* m_rawPathEdit = nullptr;
        QPushButton* m_rawUpButton = nullptr;
        QPushButton* m_rawListButton = nullptr;
        QPushButton* m_rawPreviewButton = nullptr;
        QPushButton* m_rawExportButton = nullptr;
        QToolButton* m_rawMoreButton = nullptr;
        QAction* m_rawPeAction = nullptr;
        QLabel* m_rawSummaryLabel = nullptr;
        QTableView* m_rawTable = nullptr;
        ks::ui::FlatTableModel<RawFileEntry>* m_rawModel = nullptr;
        QLineEdit* m_filePathEdit = nullptr;
        QPushButton* m_fileBrowseButton = nullptr;
        QPushButton* m_extentButton = nullptr;
        QTableView* m_extentTable = nullptr;
        ks::ui::FlatTableModel<PhysicalFileExtent>* m_extentModel = nullptr;
        QLineEdit* m_volumePathEdit = nullptr;
        QLineEdit* m_clusterEdit = nullptr;
        QPushButton* m_reverseButton = nullptr;
        QTableView* m_reverseTable = nullptr;
        ks::ui::FlatTableModel<ReverseClusterEntry>* m_reverseModel = nullptr;
        QPushButton* m_deletedScanButton = nullptr;
        QPushButton* m_deletedEraseButton = nullptr;
        QLabel* m_deletedSummaryLabel = nullptr;
        QTableView* m_deletedTable = nullptr;
        ks::ui::FlatTableModel<DeletedDirectoryEntry>* m_deletedModel = nullptr;
        std::optional<DiskForensicsSelection> m_deletedSelection;
        std::optional<DiskForensicsSelection> m_rawSelection;
        ForensicFileSystemKind m_rawFileSystem =
            ForensicFileSystemKind::Unknown;
        bool m_busy = false;
    };
}
