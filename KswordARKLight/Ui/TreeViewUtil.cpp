#include "TreeViewUtil.h"

#include "Controls.h"

namespace Ksword::Ui {

HWND CreateTreeView(HWND parent, int id, int x, int y, int width, int height, DWORD extraStyle) {
    // This helper creates the standard TreeView common control only. It does not
    // add page padding or custom drawing so feature pages keep Windows defaults.
    const DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS |
        TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS | extraStyle;
    HWND hwnd = ::CreateWindowExW(
        WS_EX_CLIENTEDGE,
        WC_TREEVIEWW,
        L"",
        style,
        x,
        y,
        width,
        height,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        ::GetModuleHandleW(nullptr),
        nullptr);
    if (hwnd) {
        ::SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(SystemUIFont()), TRUE);
    }
    return hwnd;
}

HTREEITEM InsertTreeItem(
    HWND treeView,
    HTREEITEM parent,
    HTREEITEM insertAfter,
    const std::wstring& text,
    LPARAM itemData,
    int image,
    int selectedImage) {
    // The caller controls hierarchy and images. This helper only prepares the
    // TVINSERTSTRUCTW fields needed by most feature pages.
    if (!treeView) {
        return nullptr;
    }

    TVINSERTSTRUCTW insert{};
    insert.hParent = parent;
    insert.hInsertAfter = insertAfter ? insertAfter : TVI_LAST;
    insert.item.mask = TVIF_TEXT | TVIF_PARAM;
    insert.item.pszText = const_cast<LPWSTR>(text.c_str());
    insert.item.lParam = itemData;
    if (image >= 0) {
        insert.item.mask |= TVIF_IMAGE;
        insert.item.iImage = image;
    }
    if (selectedImage >= 0) {
        insert.item.mask |= TVIF_SELECTEDIMAGE;
        insert.item.iSelectedImage = selectedImage;
    }
    return TreeView_InsertItem(treeView, &insert);
}

void ClearTreeView(HWND treeView) {
    // TVI_ROOT deletes the full tree. The helper intentionally keeps image
    // lists attached because ownership remains with the caller.
    if (treeView) {
        TreeView_DeleteItem(treeView, TVI_ROOT);
    }
}

HIMAGELIST SetTreeViewImageList(HWND treeView, HIMAGELIST imageList) {
    // Returns the previous image list so callers can decide whether they own and
    // should destroy it. The TreeView itself does not own the image list.
    if (!treeView) {
        return nullptr;
    }
    return TreeView_SetImageList(treeView, imageList, TVSIL_NORMAL);
}

} // namespace Ksword::Ui
