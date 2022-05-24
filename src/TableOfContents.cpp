/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/Dpi.h"
#include "utils/BitManip.h"
#include "utils/FileUtil.h"
#include "utils/GdiPlusUtil.h"
#include "utils/UITask.h"
#include "utils/WinUtil.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"

#include "wingui/LabelWithCloseWnd.h"

#include "Settings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "SumatraConfig.h"
#include "GlobalPrefs.h"
#include "Annotation.h"
#include "AppColors.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "DisplayModel.h"
#include "Favorites.h"
#include "TabInfo.h"
#include "resource.h"
#include "Commands.h"
#include "AppTools.h"
#include "TableOfContents.h"
#include "Translations.h"
#include "Tabs.h"
#include "Menu.h"
#include "Accelerators.h"

#include "utils/Log.h"

/* Define if you want page numbers to be displayed in the ToC sidebar */
// #define DISPLAY_TOC_PAGE_NUMBERS

#ifdef DISPLAY_TOC_PAGE_NUMBERS
#define WM_APP_REPAINT_TOC (WM_APP + 1)
#endif

// set tooltip for this item but only if the text isn't fully shown
// TODO: I might have lost something in translation
static void TocCustomizeTooltip(TreeItemGetTooltipEvent* ev) {
    auto treeView = ev->treeView;
    auto tm = treeView->treeModel;
    auto ti = ev->treeItem;
    auto nm = ev->info;
    TocItem* tocItem = (TocItem*)ti;
    IPageDestination* link = tocItem->GetPageDestination();
    if (!link) {
        return;
    }
    char* path = link->GetValue();
    if (!path) {
        path = tocItem->title;
    }
    if (!path) {
        return;
    }
    auto k = link->GetKind();
    // TODO: TocItem from Chm contain other types
    // we probably shouldn't set TocItem::dest there
    if (k == kindDestinationScrollTo) {
        return;
    }
    if (k == kindDestinationNone) {
        return;
    }

    bool isOk = (k == kindDestinationLaunchURL) || (k == kindDestinationLaunchFile) ||
                (k == kindDestinationLaunchEmbedded) || (k == kindDestinationMupdf) || (k = kindDestinationDjVu);
    CrashIf(!isOk);

    str::Str infotip;

    // Display the item's full label, if it's overlong
    RECT rcLine, rcLabel;
    treeView->GetItemRect(ev->treeItem, false, rcLine);
    treeView->GetItemRect(ev->treeItem, true, rcLabel);

    // TODO: this causes a duplicate. Not sure what changed
    if (false && rcLine.right + 2 < rcLabel.right) {
        char* currInfoTip = tm->Text(ti);
        infotip.Append(currInfoTip);
        infotip.Append("\r\n");
    }

    if (kindDestinationLaunchEmbedded == k) {
        AutoFreeStr tmp = str::Format(_TRA("Attachment: %s"), path);
        infotip.Append(tmp.Get());
    } else {
        infotip.Append(path);
    }

    str::BufSet(nm->pszText, nm->cchTextMax, infotip.Get());
}

#ifdef DISPLAY_TOC_PAGE_NUMBERS
static void RelayoutTocItem(LPNMTVCUSTOMDRAW ntvcd) {
    // code inspired by http://www.codeguru.com/cpp/controls/treeview/multiview/article.php/c3985/
    LPNMCUSTOMDRAW ncd = &ntvcd->nmcd;
    HWND hTV = ncd->hdr.hwndFrom;
    HTREEITEM hItem = (HTREEITEM)ncd->dwItemSpec;
    RECT rcItem;
    if (0 == ncd->rc.right - ncd->rc.left || 0 == ncd->rc.bottom - ncd->rc.top)
        return;
    if (!TreeView_GetItemRect(hTV, hItem, &rcItem, TRUE))
        return;
    if (rcItem.right > ncd->rc.right)
        rcItem.right = ncd->rc.right;

    // Clear the label
    RECT rcFullWidth = rcItem;
    rcFullWidth.right = ncd->rc.right;
    FillRect(ncd->hdc, &rcFullWidth, GetSysColorBrush(COLOR_WINDOW));

    // Get the label's text
    WCHAR szText[MAX_PATH];
    TVITEM item;
    item.hItem = hItem;
    item.mask = TVIF_TEXT | TVIF_PARAM;
    item.pszText = szText;
    item.cchTextMax = MAX_PATH;
    TreeView_GetItem(hTV, &item);

    // Draw the page number right-aligned (if there is one)
    MainWindow* win = FindWindowInfoByHwnd(hTV);
    TocItem* tocItem = (TocItem*)item.lParam;
    AutoFreeWstr label;
    if (tocItem->pageNo && win && win->IsDocLoaded()) {
        label.Set(win->ctrl->GetPageLabel(tocItem->pageNo));
        label.Set(str::Join(L"  ", label));
    }
    if (label && str::EndsWith(item.pszText, label)) {
        RECT rcPageNo = rcFullWidth;
        InflateRect(&rcPageNo, -2, -1);

        SIZE txtSize;
        GetTextExtentPoint32(ncd->hdc, label, str::Len(label), &txtSize);
        rcPageNo.left = rcPageNo.right - txtSize.cx;

        SetTextColor(ncd->hdc, GetSysColor(COLOR_WINDOWTEXT));
        SetBkColor(ncd->hdc, GetSysColor(COLOR_WINDOW));
        DrawTextW(ncd->hdc, label, -1, &rcPageNo, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

        // Reduce the size of the label and cut off the page number
        rcItem.right = std::max(rcItem.right - txtSize.cx, 0);
        szText[str::Len(szText) - str::Len(label)] = '\0';
    }

    SetTextColor(ncd->hdc, ntvcd->clrText);
    SetBkColor(ncd->hdc, ntvcd->clrTextBk);

    // Draw the focus rectangle (including proper background color)
    HBRUSH brushBg = CreateSolidBrush(ntvcd->clrTextBk);
    FillRect(ncd->hdc, &rcItem, brushBg);
    DeleteObject(brushBg);
    if ((ncd->uItemState & CDIS_FOCUS))
        DrawFocusRect(ncd->hdc, &rcItem);

    InflateRect(&rcItem, -2, -1);
    DrawTextW(ncd->hdc, szText, -1, &rcItem, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_WORD_ELLIPSIS);
}
#endif

static void GoToTocLinkTask(TocItem* tocItem, TabInfo* tab, DocController* ctrl) {
    MainWindow* win = tab->win;
    // tocItem is invalid if the DocController has been replaced
    if (!WindowInfoStillValid(win) || win->currentTab != tab || tab->ctrl != ctrl) {
        return;
    }

    // make sure that the tree item that the user selected
    // isn't unselected in UpdateTocSelection right again
    win->tocKeepSelection = true;
    int pageNo = tocItem->pageNo;
    IPageDestination* dest = tocItem->GetPageDestination();
    if (dest) {
        ctrl->HandleLink(dest, win->linkHandler);
    } else if (pageNo) {
        ctrl->GoToPage(pageNo, true);
    }
    win->tocKeepSelection = false;
}

static bool IsScrollToLink(IPageDestination* link) {
    if (!link) {
        return false;
    }
    auto kind = link->GetKind();
    return kind == kindDestinationScrollTo;
}

static void GoToTocTreeItem(MainWindow* win, TreeItem ti, bool allowExternal) {
    if (!ti) {
        return;
    }
    TocItem* tocItem = (TocItem*)ti;
    bool validPage = (tocItem->pageNo > 0);
    bool isScroll = IsScrollToLink(tocItem->GetPageDestination());
    if (validPage || (allowExternal || isScroll)) {
        // delay changing the page until the tree messages have been handled
        TabInfo* tab = win->currentTab;
        DocController* ctrl = win->ctrl;
        uitask::Post([=] { GoToTocLinkTask(tocItem, tab, ctrl); });
    }
}

void ClearTocBox(MainWindow* win) {
    if (!win->tocLoaded) {
        return;
    }

    win->tocTreeCtrl->Clear();

    win->currPageNo = 0;
    win->tocLoaded = false;
}

void ToggleTocBox(MainWindow* win) {
    if (!win->IsDocLoaded()) {
        return;
    }
    if (win->tocVisible) {
        SetSidebarVisibility(win, false, gGlobalPrefs->showFavorites);
        return;
    }
    SetSidebarVisibility(win, true, gGlobalPrefs->showFavorites);
    if (win->tocVisible) {
        win->tocTreeCtrl->SetFocus();
    }
}

// find the closest item in tree view to a given page number
static TocItem* TreeItemForPageNo(TreeView* treeView, int pageNo) {
    TocItem* bestMatch = nullptr;
    int bestMatchPageNo = 0;

    TreeModel* tm = treeView->treeModel;
    if (!tm) {
        return 0;
    }
    int nItems = 0;
    VisitTreeModelItems(tm, [&](TreeModel* tm, TreeItem ti) {
        auto tocItem = (TocItem*)ti;
        if (!tocItem) {
            return true;
        }
        if (!bestMatch) {
            // if nothing else matches, match the root node
            bestMatch = tocItem;
        }
        ++nItems;
        int page = tocItem->pageNo;
        if ((page <= pageNo) && (page >= bestMatchPageNo) && (page >= 1)) {
            bestMatch = tocItem;
            bestMatchPageNo = page;
            if (pageNo == bestMatchPageNo) {
                // we can stop earlier if we found the exact match
                return false;
            }
        }
        return true;
    });
    // if there's only one item, we want to unselect it so that it can
    // be selected by the user
    if (nItems < 2) {
        return 0;
    }
    return bestMatch;
}

// TODO: I can't use TreeItem->IsExpanded() because it's not in sync with
// the changes user makes to TreeCtrl
static TocItem* FindVisibleParentTreeItem(TreeView* treeView, TocItem* ti) {
    if (!ti) {
        return nullptr;
    }
    while (true) {
        auto parent = ti->parent;
        if (parent == nullptr) {
            // ti is a root node
            return ti;
        }
        if (treeView->IsExpanded((TreeItem)parent)) {
            return ti;
        }
        ti = parent;
    }
    return nullptr;
}

void UpdateTocSelection(MainWindow* win, int currPageNo) {
    if (!win->tocLoaded || !win->tocVisible || win->tocKeepSelection) {
        return;
    }

    auto treeView = win->tocTreeCtrl;
    auto item = TreeItemForPageNo(treeView, currPageNo);
    // only select the items that are visible i.e. are top nodes or
    // children of expanded node
    TreeItem toSelect = (TreeItem)FindVisibleParentTreeItem(treeView, item);
    treeView->SelectItem(toSelect);
}

static void UpdateDocTocExpansionStateRecur(TreeView* treeView, Vec<int>& tocState, TocItem* tocItem) {
    while (tocItem) {
        // items without children cannot be toggled
        if (tocItem->child) {
            // we have to query the state of the tree view item because
            // isOpenToggled is not kept in sync
            // TODO: keep toggle state on TocItem in sync
            // by subscribing to the right notifications
            bool isExpanded = treeView->IsExpanded((TreeItem)tocItem);
            bool wasToggled = isExpanded != tocItem->isOpenDefault;
            if (wasToggled) {
                tocState.Append(tocItem->id);
            }
            UpdateDocTocExpansionStateRecur(treeView, tocState, tocItem->child);
        }
        tocItem = tocItem->next;
    }
}

void UpdateTocExpansionState(Vec<int>& tocState, TreeView* treeView, TocTree* docTree) {
    if (treeView->treeModel != docTree) {
        // CrashMe();
        return;
    }
    tocState.Reset();
    TocItem* tocItem = docTree->root->child;
    UpdateDocTocExpansionStateRecur(treeView, tocState, tocItem);
}

static bool inRange(WCHAR c, WCHAR low, WCHAR hi) {
    return (low <= c) && (c <= hi);
}

// copied from mupdf/fitz/dev_text.c
// clang-format off
static bool isLeftToRightChar(WCHAR c) {
    return (
        inRange(c, 0x0041, 0x005A) ||
        inRange(c, 0x0061, 0x007A) ||
        inRange(c, 0xFB00, 0xFB06)
    );
}

static bool isRightToLeftChar(WCHAR c) {
    return (
        inRange(c, 0x0590, 0x05FF) ||
        inRange(c, 0x0600, 0x06FF) ||
        inRange(c, 0x0750, 0x077F) ||
        inRange(c, 0xFB50, 0xFDFF) ||
        inRange(c, 0xFE70, 0xFEFE)
    );
}
// clang-format off

static void GetLeftRightCounts(TocItem* node, int& l2r, int& r2l) {
next:
    if (!node) {
        return;
    }
    // short-circuit because this could overflow the stack due to recursion
    // (happened in doc from https://github.com/sumatrapdfreader/sumatrapdf/issues/1795)
    if (l2r + r2l > 1024) {
        return;
    }
    if (node->title) {
        WCHAR* ws = ToWstrTemp(node->title);
        for (const WCHAR* c = ws; *c; c++) {
            if (isLeftToRightChar(*c)) {
                l2r++;
            } else if (isRightToLeftChar(*c)) {
                r2l++;
            }
        }
    }
    GetLeftRightCounts(node->child, l2r, r2l);
    // could be: GetLeftRightCounts(node->next, l2r, r2l);
    // but faster if not recursive
    node = node->next;
    goto next;
}

static void SetInitialExpandState(TocItem* item, Vec<int>& tocState) {
    while (item) {
        if (tocState.Contains(item->id)) {
            item->isOpenToggled = true;
        }
        SetInitialExpandState(item->child, tocState);
        item = item->next;
    }
}

static void AddFavoriteFromToc(MainWindow* win, TocItem* dti) {
    int pageNo = 0;
    if (!dti) {
        return;
    }
    if (dti->dest) {
        pageNo = dti->dest->GetPageNo();
    }
    char* name = dti->title;
    AutoFreeStr pageLabel = win->ctrl->GetPageLabel(pageNo);
    AddFavoriteWithLabelAndName(win, pageNo, pageLabel, name);
}

static void OpenEmbeddedFile(TabInfo* tab, IPageDestination* dest) {
    CrashIf(!tab || !dest);
    if (!tab || !dest) {
        return;
    }
    auto win = tab->win;
    PageDestinationFile *destFile = (PageDestinationFile*)dest;
    char* path = destFile->path;
    char* tabPath = tab->filePath.Get();
    if (!str::StartsWith(path, tabPath)) {
        return;
    }
    MainWindow* newWin = FindWindowInfoByFile(path, true);
    if (!newWin) {
        LoadArgs* args = new LoadArgs(path, win);
        newWin = LoadDocument(args);
    }
    if (newWin) {
        newWin->Focus();
    }
}

static void SaveEmbeddedFile(TabInfo* tab, const char* srcPath, const char* fileName) {
    ByteSlice data = LoadEmbeddedPDFFile(srcPath);
    if (data.empty()) {
        // TODO: show an error message
        return;
    }
    char* dir = path::GetDirTemp(tab->filePath);
    fileName = path::GetBaseNameTemp(fileName);
    AutoFreeStr dstPath = path::Join(dir, fileName);
    SaveDataToFile(tab->win->hwndFrame, dstPath, data);
    str::Free(data.data());
}

static void TocContextMenu(ContextMenuEvent2* ev) {
    MainWindow* win = FindWindowInfoByHwnd(ev->w->hwnd);
    const char* filePath = win->ctrl->GetFilePath();

    POINT pt{};

    TreeView* treeView = (TreeView*)ev->w;
    TreeModel* tm = treeView->treeModel;
    TreeItem ti = GetOrSelectTreeItemAtPos(ev, pt);
    if (ti == TreeModel::kNullItem) {
        pt = {ev->mouseGlobal.x, ev->mouseGlobal.y};
    }
    int pageNo = 0;
    TocItem* dti = (TocItem*)ti;
    IPageDestination* dest = dti ? dti->dest : nullptr;
    if (dest) {
        pageNo = dti->dest->GetPageNo();
    }

    TabInfo* tab = win->currentTab;
    HMENU popup = BuildMenuFromMenuDef(menuDefContextToc, CreatePopupMenu(), nullptr);

    const char* embeddedFilePath = nullptr;
    char* fileName = nullptr;
    if (dest && dest->GetKind() == kindDestinationLaunchEmbedded) {
        auto embeddedFile = (PageDestinationFile*)dest;
        // this is a path to a file on disk, e.g. a path to opened PDF
        // with the embedded stream number
        embeddedFilePath = embeddedFile->path;
        // this is name of the file as set inside PDF file
        fileName = dti->dest->GetName();
        char* ext = path::GetExtTemp(fileName);
        bool canOpenEmbedded = str::EqI(ext, ".pdf");
        if (!canOpenEmbedded) {
            menu::Remove(popup, CmdOpenEmbeddedPDF);
        }
    }
    else {
        // TODO: maybe move this to BuildMenuFromMenuDef
        menu::Remove(popup, CmdSaveEmbeddedFile);
        menu::Remove(popup, CmdOpenEmbeddedPDF);
    }

    if (pageNo > 0) {
        AutoFreeStr pageLabel = win->ctrl->GetPageLabel(pageNo);
        bool isBookmarked = gFavorites.IsPageInFavorites(filePath, pageNo);
        if (isBookmarked) {
            menu::Remove(popup, CmdFavoriteAdd);

            // %s and not %d because re-using translation from RebuildFavMenu()
            const char* tr = _TRA("Remove page %s from favorites");
            AutoFreeStr s = str::Format(tr, pageLabel.Get());
            menu::SetText(popup, CmdFavoriteDel, s);
        } else {
            menu::Remove(popup, CmdFavoriteDel);
            // %s and not %d because re-using translation from RebuildFavMenu()
            str::Str str = _TRA("Add page %s to favorites");
            ACCEL a;
            bool ok = GetAccelByCmd(CmdFavoriteAdd, a);
            if (ok) {
                AppendAccelKeyToMenuString(str, a);
            }
            AutoFreeStr s(str::Format(str.Get(), pageLabel.Get()));
            menu::SetText(popup, CmdFavoriteAdd, s);
        }
    } else {
        menu::Remove(popup, CmdFavoriteAdd);
        menu::Remove(popup, CmdFavoriteDel);
    }
    RemoveBadMenuSeparators(popup);
    MarkMenuOwnerDraw(popup);
    uint flags = TPM_RETURNCMD | TPM_RIGHTBUTTON;
    int cmd = TrackPopupMenu(popup, flags, pt.x, pt.y, 0, win->hwndFrame, nullptr);
    FreeMenuOwnerDrawInfoData(popup);
    DestroyMenu(popup);
    switch (cmd) {
        case CmdExpandAll:
            win->tocTreeCtrl->ExpandAll();
            break;
        case CmdCollapseAll:
            win->tocTreeCtrl->CollapseAll();
            break;
        case CmdFavoriteAdd:
            AddFavoriteFromToc(win, dti);
            break;
        case CmdFavoriteDel:
            DelFavorite(filePath, pageNo);
            break;
        case CmdSaveEmbeddedFile: {
            SaveEmbeddedFile(tab, embeddedFilePath, fileName);
        } break;

        case CmdOpenEmbeddedPDF:
            // TODO: maybe also allow for a fileName hint
            OpenEmbeddedFile(tab, dest);
            break;
    }
}

static bool ShouldCustomDraw(MainWindow* win) {
    // we only want custom drawing for pdf and pdf multi engines
    // as they are the only ones supporting custom colors and fonts
    DisplayModel* dm = win->AsFixed();
    if (!dm) {
        return false;
    }
    EngineBase* engine = dm->GetEngine();
    if (!engine) {
        return false;
    }
    Kind kind = dm->GetEngineType();
    return kind == kindEngineMupdf || kind == kindEngineMulti;
}

LRESULT OnTocCustomDraw(TreeItemCustomDrawEvent*);

// auto-expand root level ToC nodes if there are at most two
static void AutoExpandTopLevelItems(TocItem* root) {
    if (!root) {
        return;
    }

    if (root->next && root->next->next) {
        return;
    }

    if (!root->IsExpanded()) {
        root->isOpenToggled = !root->isOpenToggled;
    }
    if (!root->next) {
        return;
    }
    if (!root->next->IsExpanded()) {
        root->next->isOpenToggled = !root->next->isOpenToggled;
    }
}

void LoadTocTree(MainWindow* win) {
    TabInfo* tab = win->currentTab;
    CrashIf(!tab);

    if (win->tocLoaded) {
        return;
    }

    win->tocLoaded = true;

    auto* tocTree = tab->ctrl->GetToc();
    if (!tocTree || !tocTree->root) {
        return;
    }

    tab->currToc = tocTree;

    // consider a ToC tree right-to-left if a more than half of the
    // alphabetic characters are in a right-to-left script
    int l2r = 0, r2l = 0;
    GetLeftRightCounts(tocTree->root, l2r, r2l);
    bool isRTL = r2l > l2r;

    TreeView* treeView = win->tocTreeCtrl;
    HWND hwnd = treeView->hwnd;
    SetRtl(hwnd, isRTL);

    UpdateTreeCtrlColors(win);
    SetInitialExpandState(tocTree->root, tab->tocState);
    AutoExpandTopLevelItems(tocTree->root->child);

    treeView->SetTreeModel(tocTree);

    treeView->onTreeItemCustomDraw = nullptr;
    if (ShouldCustomDraw(win)) {
        treeView->onTreeItemCustomDraw = OnTocCustomDraw;
    }
    LayoutTreeContainer(win->tocLabelWithClose, win->tocTreeCtrl->hwnd);
    // uint fl = RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN;
    // RedrawWindow(hwnd, nullptr, nullptr, fl);
}

// TODO: use https://docs.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-getobject?redirectedfrom=MSDN
// to get LOGFONT from existing font and then create a derived font
static void UpdateFont(HDC hdc, int fontFlags) {
    // TODO: this is a bit hacky, in that we use default font
    // and not the font from TreeCtrl. But in this case they are the same
    bool italic = bit::IsSet(fontFlags, fontBitItalic);
    bool bold = bit::IsSet(fontFlags, fontBitBold);
    HFONT hfont = GetDefaultGuiFont(bold, italic);
    SelectObject(hdc, hfont);
}

// https://docs.microsoft.com/en-us/windows/win32/controls/about-custom-draw
// https://docs.microsoft.com/en-us/windows/win32/api/commctrl/ns-commctrl-nmtvcustomdraw
LRESULT OnTocCustomDraw(TreeItemCustomDrawEvent* ev) {
#if defined(DISPLAY_TOC_PAGE_NUMBERS)
    if (false)
        return CDRF_DODEFAULT;
    switch (((LPNMCUSTOMDRAW)pnmtv)->dwDrawStage) {
        case CDDS_PREPAINT:
            return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT:
            return CDRF_DODEFAULT | CDRF_NOTIFYPOSTPAINT;
        case CDDS_ITEMPOSTPAINT:
            RelayoutTocItem((LPNMTVCUSTOMDRAW)pnmtv);
            // fall through
        default:
            return CDRF_DODEFAULT;
    }
    break;
#endif

    NMTVCUSTOMDRAW* tvcd = ev->nm;
    NMCUSTOMDRAW* cd = &(tvcd->nmcd);
    if (cd->dwDrawStage == CDDS_PREPAINT) {
        // ask to be notified about each item
        return CDRF_NOTIFYITEMDRAW;
    }

    if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
        // called before drawing each item
        TocItem* tocItem = (TocItem*)ev->treeItem;
        if (!tocItem) {
            return CDRF_DODEFAULT;
        }
        if (tocItem->color != ColorUnset) {
            tvcd->clrText = tocItem->color;
        }
        if (tocItem->fontFlags != 0) {
            UpdateFont(cd->hdc, tocItem->fontFlags);
            return CDRF_NEWFONT;
        }
        return CDRF_DODEFAULT;
    }
    return CDRF_DODEFAULT;
}

// disabled becaues of https://github.com/sumatrapdfreader/sumatrapdf/issues/2202
// it was added for https://github.com/sumatrapdfreader/sumatrapdf/issues/1716
// but unclear if its still needed
// this calls GoToTocLinkTask) which will eventually call GoToPage()
// which adds nav point. Maybe I should not add nav point
// if going to the same page?
LRESULT TocTreeClick(TreeClickEvent* ev) {
#if 0
    ev->didHandle = true;
    if (!ev->treeItem) {
        return;
    }
    MainWindow* win = FindWindowInfoByHwnd(ev->w->hwnd);
    CrashIf(!win);
    bool allowExternal = false;
    GoToTocTreeItem(win, ev->treeItem, allowExternal);
#endif
    return -1;
}

static void TocTreeSelectionChanged(TreeSelectionChangedEvent* ev) {
    MainWindow* win = FindWindowInfoByHwnd(ev->treeView->hwnd);
    CrashIf(!win);

    // When the focus is set to the toc window the first item in the treeview is automatically
    // selected and a TVN_SELCHANGEDW notification message is sent with the special code pnmtv->action ==
    // 0x00001000. We have to ignore this message to prevent the current page to be changed.
    // The case pnmtv->action==TVC_UNKNOWN is ignored because
    // it corresponds to a notification sent by
    // the function TreeView_DeleteAllItems after deletion of the item.
    bool shouldHandle = ev->byKeyboard || ev->byMouse;
    if (!shouldHandle) {
        return;
    }
    bool allowExternal = ev->byMouse;
    GoToTocTreeItem(win, ev->selectedItem, allowExternal);
}

LRESULT TocTreeKeyDown2(TreeKeyDownEvent* ev) {
    // TODO: trying to fix https://github.com/sumatrapdfreader/sumatrapdf/issues/1841
    // doesn't work i.e. page up / page down seems to be processed anyway by TreeCtrl
#if 0
    if ((ev->keyCode == VK_PRIOR) || (ev->keyCode == VK_NEXT)) {
        // up/down in tree is not very useful, so instead
        // send it to frame so that it scrolls document instead
        MainWindow* win = FindWindowInfoByHwnd(ev->hwnd);
        // this is sent as WM_NOTIFY to TreeCtrl but for frame it's WM_KEYDOWN
        // alternatively, we could call FrameOnKeydown(ev->wp, ev->lp, false);
        SendMessageW(win->hwndFrame, WM_KEYDOWN, ev->wp, ev->lp);
        ev->didHandle = true;
        ev->result = 1;
        return;
    }
#endif
    if (ev->keyCode != VK_TAB) {
        return 0;
    }

    MainWindow* win = FindWindowInfoByHwnd(ev->treeView->hwnd);
    if (win->tabsVisible && IsCtrlPressed()) {
        TabsOnCtrlTab(win, IsShiftPressed());
        return 1;
    }
    AdvanceFocus(win);
    return 1;
}

#ifdef DISPLAY_TOC_PAGE_NUMBERS
static void TocTreeMsgFilter(__unused WndEvent* ev) {
    switch (msg) {
        case WM_SIZE:
        case WM_HSCROLL:
            // Repaint the ToC so that RelayoutTocItem is called for all items
            PostMessageW(hwnd, WM_APP_REPAINT_TOC, 0, 0);
            break;
        case WM_APP_REPAINT_TOC:
            InvalidateRect(hwnd, nullptr, TRUE);
            UpdateWindow(hwnd);
            break;
    }
}
#endif

// Position label with close button and tree window within their parent.
// Used for toc and favorites.
void LayoutTreeContainer(LabelWithCloseWnd* l, HWND hwndTree) {
    HWND hwndContainer = GetParent(hwndTree);
    Size labelSize = l->GetIdealSize();
    Rect rc = WindowRect (hwndContainer);
    int dy = rc.dy;
    int y = 0;
    MoveWindow(l->hwnd, y, 0, rc.dx, labelSize.dy, TRUE);
    dy -= labelSize.dy;
    y += labelSize.dy;
    MoveWindow(hwndTree, 0, y, rc.dx, dy, TRUE);
}

static WNDPROC gWndProcTocBox = nullptr;

static LRESULT CALLBACK WndProcTocBox(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR subclassId, DWORD_PTR data) {
    MainWindow* win = FindWindowInfoByHwnd(hwnd);
    if (!win) {
        return CallWindowProc(gWndProcTocBox, hwnd, msg, wp, lp);
    }

    LRESULT res = 0;
    res = TryReflectMessages(hwnd, msg, wp, lp);
    if (res) {
        return res;
    }

    TreeView* treeView = win->tocTreeCtrl;

    switch (msg) {
        case WM_SIZE:
            LayoutTreeContainer(win->tocLabelWithClose, treeView->hwnd);
            break;

        case WM_COMMAND:
            if (LOWORD(wp) == IDC_TOC_LABEL_WITH_CLOSE) {
                ToggleTocBox(win);
            }
            break;
    }
    return CallWindowProc(gWndProcTocBox, hwnd, msg, wp, lp);
}

// TODO: restore
#if 0
void TocTreeMouseWheelHandler(MouseWheelEvent* ev) {
    MainWindow* win = FindWindowInfoByHwnd(ev->hwnd);
    CrashIf(!win);
    if (!win) {
        return;
    }
    // scroll the canvas if the cursor isn't over the ToC tree
    if (!IsCursorOverWindow(ev->hwnd)) {
        ev->didHandle = true;
        ev->result = SendMessageW(win->hwndCanvas, ev->msg, ev->wp, ev->lp);
    }
}
#endif

// TODO: restore
#if 0
void TocTreeCharHandler(CharEvent* ev) {
    MainWindow* win = FindWindowInfoByHwnd(ev->hwnd);
    CrashIf(!win);
    if (!win) {
        return;
    }
    if (VK_ESCAPE != ev->keyCode) {
        return;
    }
    if (!gGlobalPrefs->escToExit) {
        return;
    }
    if (!CanCloseWindow(win)) {
        return;
    }

    CloseWindow(win, true, false);
    ev->didHandle = true;
}
#endif

extern HFONT GetTreeFont();

void CreateToc(MainWindow* win) {
    HMODULE hmod = GetModuleHandle(nullptr);
    int dx = gGlobalPrefs->sidebarDx;
    DWORD style = WS_CHILD | WS_CLIPCHILDREN;
    HWND parent = win->hwndFrame;
    win->hwndTocBox = CreateWindowExW(0, WC_STATIC, L"", style, 0, 0, dx, 0, parent, nullptr, hmod, nullptr);

    auto* l = new LabelWithCloseWnd();
    l->Create(win->hwndTocBox, IDC_TOC_LABEL_WITH_CLOSE);
    win->tocLabelWithClose = l;
    l->SetPaddingXY(2, 2);
    // TODO: use the same font size as in GetTreeFont()?
    l->SetFont(GetDefaultGuiFont(true, false));
    // label is set in UpdateToolbarSidebarText()

    auto* treeView = new TreeView();
    TreeViewCreateArgs args;
    args.parent = win->hwndTocBox;
    args.font =  GetTreeFont();
    args.fullRowSelect = true;
    args.exStyle = WS_EX_STATICEDGE;

    treeView->onGetTooltip = TocCustomizeTooltip;
    treeView->onContextMenu = TocContextMenu;
    treeView->onTreeSelectionChanged = TocTreeSelectionChanged;
    treeView->onTreeKeyDown = TocTreeKeyDown2;
    //treeView->onTreeClick = TocTreeClick; // TODO: maybe not necessary
    //treeView->onChar = TocTreeCharHandler;
    //treeView->onMouseWheel = TocTreeMouseWheelHandler;

    treeView->Create(args);
    CrashIf(!treeView->hwnd);;
    win->tocTreeCtrl = treeView;

    if (nullptr == gWndProcTocBox) {
        gWndProcTocBox = (WNDPROC)GetWindowLongPtr(win->hwndTocBox, GWLP_WNDPROC);
    }
    SetWindowLongPtr(win->hwndTocBox, GWLP_WNDPROC, (LONG_PTR)WndProcTocBox);

    UpdateTreeCtrlColors(win);
}
