#include "GroundedMinimal.hpp"
#include "UnrealUtils.hpp"
#include "ItemSpawner.hpp"
#include "Command.hpp"
#include "C2Cycle.hpp"
#include "WinGUI.hpp"

#include <commctrl.h>
#include <thread>
#include <atomic>
#include <algorithm>

#pragma comment(lib, "comctl32.lib")

// Control IDs
#define IDC_CHECK_SHOW_CONSOLE    100
#define IDC_CHECK_GLOBAL_CHEAT    101
#define IDC_CHECK_GLOBAL_C2       102
#define IDC_LIST_PLAYERS          200
#define IDC_LIST_DATA_TABLES      201
#define IDC_LIST_ITEM_NAMES       202
#define IDC_BUTTON_SPAWN          300
#define IDC_BUTTON_CULL           301
#define IDC_BUTTON_C2_CYCLE       302
#define IDC_EDIT_ITEM_COUNT       400
#define IDC_STATIC_ITEM_COUNT     401
#define IDC_TIMER_PLAYER_UPDATE   500
#define IDC_EDIT_ITEM_SEARCH      600
#define IDC_STATIC_ITEM_SEARCH    601

namespace WinGUI {
    ///////////////////////////////////////////////////////
    /// Globals

    std::function<void(int32_t, const std::wstring&, const std::wstring&, int32_t)> fnSpawnCallback = nullptr;
    std::function<void()> fnGlobalC2CycleCallback = nullptr;
    std::function<void(const std::wstring&)> fnDataTableSelectedCallback = nullptr;

    std::vector<SDK::APlayerState*> g_vszPlayers;
    std::vector<UnrealUtils::DataTableInfo> g_vDataTables;

    ///////////////////////////////////////////////////////
    /// Storage for current DataTable items (unfiltered tho)

    std::vector<std::string> g_vCurrentDataTableItems;
    std::string g_szCurrentDataTableName;

    static bool g_bGuiInitialized = false;
    static bool g_bConsoleVisible = true;

    static HWND g_hMainWnd = nullptr;
    static HWND g_hListPlayers = nullptr;
    static HWND g_hListDataTables = nullptr;
    static HWND g_hListItemNames = nullptr;
    static HWND g_hEditItemCount = nullptr;
    static HWND g_hCheckShowConsole = nullptr;
    static HWND g_hEditItemSearch = nullptr;
    static HWND g_hStaticItemSearch = nullptr;

    ///////////////////////////////////////////////////////
    /// Forward declarations

    LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    void AddItemName(const std::wstring& szItemName);
    void ClearItemNameList(void);

    static inline bool WideStringToUtf8(
        const std::wstring& szWideString,
        std::string &szUtf8String
    ) {
        int iSizeNeeded = WideCharToMultiByte(
            CP_UTF8,
            0,
            szWideString.data(),
            (int) szWideString.size(),
            nullptr,
            0,
            nullptr,
            nullptr
        );
        if (iSizeNeeded <= 0) {
            return false;
        }

        std::string szBuffer(iSizeNeeded, 0);
        int iConverted = WideCharToMultiByte(
            CP_UTF8,
            0,
            szWideString.data(),
            (int) szWideString.size(),
            &szBuffer[0],
            iSizeNeeded,
            nullptr,
            nullptr
        );
        if (iConverted <= 0) {
            return false;
        }

        szUtf8String = std::move(szBuffer);
        return true;
    }

    static inline bool Utf8ToWideString(
        const std::string& szUtf8String,
        std::wstring &szWideString
    ) {
        int iSizeNeeded = MultiByteToWideChar(
            CP_UTF8,
            0,
            szUtf8String.data(),
            (int) szUtf8String.size(),
            nullptr,
            0
        );
        if (iSizeNeeded <= 0) {
            return false; // Conversion failed ( - thanks for this inline suggestion copilot, very handy)
        }
        std::wstring szBuffer(iSizeNeeded, 0);
        int iConverted = MultiByteToWideChar(
            CP_UTF8,
            0,
            szUtf8String.data(),
            (int) szUtf8String.size(),
            &szBuffer[0],
            iSizeNeeded
        );
        if (iConverted <= 0) {
            return false;
        }
        szWideString = std::move(szBuffer);
        return true;
    }

    // Helper function to apply search filter to item list
    static void ApplyItemSearchFilter(
        const std::wstring& szSearchTerm
    ) {
        if (g_vCurrentDataTableItems.empty()) {
            return; // No items to filter
        }

        // david guetta ends case-sensitive discrimination
        std::wstring szSearchTermLower = szSearchTerm;
        std::transform(
            szSearchTermLower.begin(), 
            szSearchTermLower.end(), 
            szSearchTermLower.begin(), 
            ::towlower
        );

        // Wonder what this does, probably counts the number of nested hypervisors
        ClearItemNameList();

        // Filter and add items that match the search term
        for (const auto& szItemName : g_vCurrentDataTableItems) {
            std::wstring szItemNameW;
            if (!Utf8ToWideString(szItemName, szItemNameW)) {
                continue;
            }

            // 9 lines since case-sensitive discrimination ended, shoutout to std::transform
            std::wstring szItemNameLower = szItemNameW;
            std::transform(
                szItemNameLower.begin(), 
                szItemNameLower.end(), 
                szItemNameLower.begin(), 
                ::towlower
            );

            if (
                szSearchTermLower.empty() 
                || 
                szItemNameLower.find(szSearchTermLower) != std::wstring::npos
            ) {
                AddItemName(szItemNameW);
            }
        }
    }

    static void LaunchGUIThread(void) {
        std::thread([]() {
            // Initialize common controls
            INITCOMMONCONTROLSEX icexControls = { 
                sizeof(icexControls), ICC_LISTVIEW_CLASSES 
            };
            InitCommonControlsEx(&icexControls);

            // Register window class
            WNDCLASSEX wcWindowClass = { 
                sizeof(wcWindowClass) 
            };
            wcWindowClass.style = CS_HREDRAW | CS_VREDRAW;
            wcWindowClass.lpfnWndProc = WndProc;
            wcWindowClass.hInstance = GetModuleHandle(NULL);
            wcWindowClass.lpszClassName = L"GroundedInternalGUI";
            wcWindowClass.hbrBackground = CreateSolidBrush(RGB(45, 45, 48)); // no flashbangs

            RegisterClassEx(&wcWindowClass);

            ///////////////////////////////////////////////////////
            /// Create main window

            g_hMainWnd = CreateWindowEx(
                0,
                wcWindowClass.lpszClassName,
                L"Grounded Debug GUI",
                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                CW_USEDEFAULT, CW_USEDEFAULT,
                800, 600,
                NULL, NULL, wcWindowClass.hInstance, NULL
            );

            if (nullptr == g_hMainWnd) {
                LogError("WinGUI", "Failed to create main window");
                return;
            }

            ///////////////////////////////////////////////////////
            /// Create checkboxes

            g_hCheckShowConsole = CreateWindowEx(
                0, L"BUTTON", L"Show Debug Console",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                10, 10, 180, 20,
                g_hMainWnd, (HMENU) IDC_CHECK_SHOW_CONSOLE, wcWindowClass.hInstance, NULL
            );

            // Set initial checkbox state based on ShowDebugConsole
            if (nullptr != g_hCheckShowConsole) {
                CheckDlgButton(
                    g_hMainWnd, 
                    IDC_CHECK_SHOW_CONSOLE, 
                    ShowDebugConsole ? BST_CHECKED : BST_UNCHECKED
                );
            }

            HWND hCheckGlobalCheat = CreateWindowEx(
                0, L"BUTTON", L"Global Cheat Mode",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                10, 35, 180, 20,
                g_hMainWnd, (HMENU) IDC_CHECK_GLOBAL_CHEAT, wcWindowClass.hInstance, NULL
            );

            HWND hCheckGlobalC2 = CreateWindowEx(
                0, L"BUTTON", L"Global C2 Authority",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                10, 60, 180, 20,
                g_hMainWnd, (HMENU) IDC_CHECK_GLOBAL_C2, wcWindowClass.hInstance, NULL
            );

            // Create player list
            g_hListPlayers = CreateWindowEx(
                0, WC_LISTVIEW, NULL,
                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                10, 100, 380, 200,
                g_hMainWnd, (HMENU) IDC_LIST_PLAYERS, wcWindowClass.hInstance, NULL
            );

            if (nullptr != g_hListPlayers) {
                ListView_SetExtendedListViewStyle(g_hListPlayers, LVS_EX_FULLROWSELECT);

                LVCOLUMN lvColumn = { LVCF_TEXT | LVCF_WIDTH };
                lvColumn.cx = 120;
                lvColumn.pszText = (LPWSTR) L"Player Name";
                ListView_InsertColumn(g_hListPlayers, 0, &lvColumn);

                lvColumn.cx = 80;
                lvColumn.pszText = (LPWSTR) L"Player ID";
                ListView_InsertColumn(g_hListPlayers, 1, &lvColumn);

                lvColumn.cx = 120;
                lvColumn.pszText = (LPWSTR) L"Host Authority";
                ListView_InsertColumn(g_hListPlayers, 2, &lvColumn);
            }

            // Create DataTable list
            g_hListDataTables = CreateWindowEx(
                0, WC_LISTVIEW, NULL,
                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                410, 100, 180, 200,
                g_hMainWnd, (HMENU) IDC_LIST_DATA_TABLES, wcWindowClass.hInstance, NULL
            );

            if (nullptr != g_hListDataTables) {
                ListView_SetExtendedListViewStyle(g_hListDataTables, LVS_EX_FULLROWSELECT);

                LVCOLUMN lvColumn = { LVCF_TEXT | LVCF_WIDTH };
                lvColumn.cx = 170;
                lvColumn.pszText = (LPWSTR) L"DataTable List";
                ListView_InsertColumn(g_hListDataTables, 0, &lvColumn);
            }

            // Create ItemName list
            g_hListItemNames = CreateWindowEx(
                0, WC_LISTVIEW, NULL,
                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                600, 100, 180, 200,
                g_hMainWnd, (HMENU) IDC_LIST_ITEM_NAMES, wcWindowClass.hInstance, NULL
            );

            if (nullptr != g_hListItemNames) {
                ListView_SetExtendedListViewStyle(g_hListItemNames, LVS_EX_FULLROWSELECT);

                LVCOLUMN lvColumn = { LVCF_TEXT | LVCF_WIDTH };
                lvColumn.cx = 170;
                lvColumn.pszText = (LPWSTR) L"ItemName List";
                ListView_InsertColumn(g_hListItemNames, 0, &lvColumn);
            }

            // Create Item Search label
            g_hStaticItemSearch = CreateWindowEx(
                0, L"STATIC", L"Filter:",
                WS_CHILD | WS_VISIBLE,
                410, 310, 80, 20,
                g_hMainWnd, 
                (HMENU) IDC_STATIC_ITEM_SEARCH, 
                wcWindowClass.hInstance, 
                NULL
            );

            // Create Item Search input field
            g_hEditItemSearch = CreateWindowEx(
                WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP,
                500, 308, 280, 24,
                g_hMainWnd,
                (HMENU) IDC_EDIT_ITEM_SEARCH, 
                wcWindowClass.hInstance, 
                NULL
            );

            // Create Item Count label
            HWND hStaticItemCount = CreateWindowEx(
                0, L"STATIC", L"Item Count:",
                WS_CHILD | WS_VISIBLE,
                250, 340, 80, 20,
                g_hMainWnd, 
                (HMENU) IDC_STATIC_ITEM_COUNT, 
                wcWindowClass.hInstance,
                NULL
            );

            // Create Item Count input field
            g_hEditItemCount = CreateWindowEx(
                WS_EX_CLIENTEDGE, L"EDIT", L"1",
                WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_NUMBER,
                330, 338, 60, 24,
                g_hMainWnd, 
                (HMENU) IDC_EDIT_ITEM_COUNT, 
                wcWindowClass.hInstance, 
                NULL
            );

            // Create buttons
            HWND hButtonSpawn = CreateWindowEx(
                0, L"BUTTON", L"Spawn",
                WS_CHILD | WS_VISIBLE,
                10, 380, 100, 30,
                g_hMainWnd,
                (HMENU) IDC_BUTTON_SPAWN, 
                wcWindowClass.hInstance,
                NULL
            );

            HWND hButtonC2Cycle = CreateWindowEx(
                0, L"BUTTON", L"Global C2 Cycle",
                WS_CHILD | WS_VISIBLE,
                410, 380, 150, 30,
                g_hMainWnd, 
                (HMENU) IDC_BUTTON_C2_CYCLE, 
                wcWindowClass.hInstance, 
                NULL
            );

            // Deploy the boy
            ShowWindow(g_hMainWnd, SW_SHOW);
            UpdateWindow(g_hMainWnd);

            // Set up timer for periodic player list updates (every 5 seconds)
            SetTimer(g_hMainWnd, IDC_TIMER_PLAYER_UPDATE, 5000, NULL);

            g_bGuiInitialized = true; // Set this BEFORE the message loop

            // Message loop
            MSG msgMessage;
            while (GetMessage(&msgMessage, nullptr, 0, 0)) {
                TranslateMessage(&msgMessage);
                DispatchMessage(&msgMessage);
            }

            // Clean up timer when message loop exits
            KillTimer(g_hMainWnd, IDC_TIMER_PLAYER_UPDATE);
        }).detach();
    }

    // Helper function to get item count from edit control
    static int32_t GetItemCountFromEdit(void) {
        if (nullptr == g_hEditItemCount) {
            return 1; // Default value
        }

        wchar_t szBuffer[16] = { 0 };
        GetWindowText(g_hEditItemCount, szBuffer, ARRAYSIZE(szBuffer));

        int32_t iItemCount = 1; // Default value
        try {
            iItemCount = std::stoi(szBuffer);
            if (iItemCount <= 0 || iItemCount > 999) {
                iItemCount = 1; // Smack that bih to a reasonable range
            }
        } catch (...) {
            // iItemCount = 0; - you get nothing, you lose, good day sir!
            iItemCount = 1; // jk be a nice person, how many lines since case-sensitive discrimination ended???
        }

        return iItemCount;
    }

    // Helper function to get search term from edit control
    static std::wstring GetSearchTermFromEdit(void) {
        if (nullptr == g_hEditItemSearch) {
            return L"";
        }

        wchar_t szBuffer[256] = { 0 };
        GetWindowText(g_hEditItemSearch, szBuffer, ARRAYSIZE(szBuffer));
        return std::wstring(szBuffer);
    }

    ///////////////////////////////////////////////////////
    /// ~~Tortilla~~ Data Population Wrappers

    static void ClearPlayerTable(void) {
        if (nullptr != g_hListPlayers) {
            ListView_DeleteAllItems(g_hListPlayers);
        }
    }

    static void AddPlayerRow(
        const std::wstring& szName, 
        int32_t iId, 
        bool bHostAuth
    ) {
        if (nullptr == g_hListPlayers) {
            LogError("WinGUI", "g_hListPlayers is null in AddPlayerRow");
            return;
        }

        std::wstring szIdStr = std::to_wstring(iId);
        std::wstring szAuthStr = bHostAuth ? L"Yes" : L"No";

        LVITEM lvItem = {};
        lvItem.mask = LVIF_TEXT | LVIF_PARAM;
        lvItem.iItem = ListView_GetItemCount(g_hListPlayers);
        lvItem.pszText = const_cast<LPWSTR>(szName.c_str());
        lvItem.lParam = iId; // store actual ID

        int iIndex = ListView_InsertItem(g_hListPlayers, &lvItem);
        if (iIndex >= 0) {
            ListView_SetItemText(
                g_hListPlayers, 
                iIndex, 
                1, 
                const_cast<LPWSTR>(szIdStr.c_str())
            );
            ListView_SetItemText(
                g_hListPlayers, 
                iIndex, 
                2, 
                const_cast<LPWSTR>(szAuthStr.c_str())
            );
        }
    }

    static void ClearDataTableList(void) {
        if (nullptr != g_hListDataTables) {
            ListView_DeleteAllItems(g_hListDataTables);
        }
    }

    static void AddDataTable(
        const std::wstring& szTableName
    ) {
        if (nullptr == g_hListDataTables) {
            return;
        }

        LVITEM lvItem = {};
        lvItem.mask = LVIF_TEXT;
        lvItem.iItem = ListView_GetItemCount(g_hListDataTables);
        lvItem.pszText = const_cast<LPWSTR>(szTableName.c_str());
        ListView_InsertItem(g_hListDataTables, &lvItem);
    }

    void ClearItemNameList(void) {
        if (nullptr != g_hListItemNames) {
            ListView_DeleteAllItems(g_hListItemNames);
            ListView_SetItemState(g_hListItemNames, -1, 0, LVIS_SELECTED);
        }
    }

    void AddItemName(
        const std::wstring& szItemName
    ) {
        if (nullptr == g_hListItemNames) {
            return;
        }

        LVITEM lvItem = {};
        lvItem.mask = LVIF_TEXT;
        lvItem.iItem = ListView_GetItemCount(g_hListItemNames);
        lvItem.pszText = const_cast<LPWSTR>(szItemName.c_str());
        ListView_InsertItem(g_hListItemNames, &lvItem);
    }

    void PopulatePlayerList(void) {
        if (!g_bGuiInitialized) {
            LogMessage("WinGUI", "GUI not initialized, skipping player list update");
            return;
        }
        
        // Get fresh player data, so fresh omg
        std::vector<SDK::APlayerState*> vNewPlayers;
        DisableGlobalOutput();
        UnrealUtils::DumpConnectedPlayers(&vNewPlayers);
        EnableGlobalOutput();

        // Check if the player list actually changed lol
        bool bPlayersChanged = false;
        if (vNewPlayers.size() != g_vszPlayers.size()) {
            bPlayersChanged = true;
        } else {
            // Compare player IDs to see if the list changed
            for (size_t i = 0; i < vNewPlayers.size(); ++i) {
                if (nullptr == vNewPlayers[i] || nullptr == g_vszPlayers[i] ||
                    vNewPlayers[i]->PlayerId != g_vszPlayers[i]->PlayerId) {
                    bPlayersChanged = true;
                    break;
                }
            }
        }
        
        if (!bPlayersChanged) {
            return;
        }
        
        // Preserve current selection before clearing
        int32_t iSelectedPlayerId = -1;
        int iCurrentSelection = ListView_GetNextItem(g_hListPlayers, -1, LVNI_SELECTED);
        if (iCurrentSelection >= 0) {
            LVITEM lvGetItem = {};
            lvGetItem.iItem = iCurrentSelection;
            lvGetItem.mask = LVIF_PARAM;
            if (ListView_GetItem(g_hListPlayers, &lvGetItem)) {
                iSelectedPlayerId = (int32_t)lvGetItem.lParam;
            }
        }
        
        // Update the global player list
        g_vszPlayers = vNewPlayers;
        
        // Clear and repepopopulate the ListView
        ClearPlayerTable();
        
        int iNewSelectionIndex = -1; // Track where to restore selection
        for (const auto& lpPlayerState : g_vszPlayers) {
            if (nullptr == lpPlayerState) {
                continue;
            }

            SDK::FString fszPlayerName = lpPlayerState->GetPlayerName();
            int32_t iPlayerId = lpPlayerState->PlayerId;
            bool bHostAuthority = lpPlayerState->HasAuthority();
            std::wstring fszPlayerNameW;
            
            if (!Utf8ToWideString(fszPlayerName.ToString(), fszPlayerNameW)) {
                LogError("WinGUI", "Failed to convert player name to wide string");
                continue;
            }

            AddPlayerRow(fszPlayerNameW, iPlayerId, bHostAuthority);
            
            // Check if this is the previously selected player
            if (
                -1 != iSelectedPlayerId 
                && 
                iPlayerId == iSelectedPlayerId
            ) {
                iNewSelectionIndex = ListView_GetItemCount(g_hListPlayers) - 1; // Last added item
            }
        }
        
        // Restore selection if the player still exists
        if (iNewSelectionIndex >= 0) {
            ListView_SetItemState(
                g_hListPlayers, 
                iNewSelectionIndex, 
                LVIS_SELECTED | LVIS_FOCUSED, 
                LVIS_SELECTED | LVIS_FOCUSED
            );
            ListView_EnsureVisible(g_hListPlayers, iNewSelectionIndex, FALSE);
        }
        
        // Only invalidate the player list area, not the entire window
        if (nullptr != g_hMainWnd && nullptr != g_hListPlayers) {
            RECT rcPlayerList;
            GetWindowRect(g_hListPlayers, &rcPlayerList);
            ScreenToClient(g_hMainWnd, (POINT*)&rcPlayerList.left);
            ScreenToClient(g_hMainWnd, (POINT*)&rcPlayerList.right);
            InvalidateRect(g_hMainWnd, &rcPlayerList, TRUE);
            UpdateWindow(g_hListPlayers); // Update only the player list
        }
    }

    // Window procedure
    LRESULT CALLBACK WndProc(
        HWND hWnd, 
        UINT uMsg, 
        WPARAM wParam, 
        LPARAM lParam
    ) {
        switch (uMsg) {
            case WM_TIMER: {
                if (wParam == IDC_TIMER_PLAYER_UPDATE) {
                    HWND hFocusedWindow = GetFocus();
                    if (
                        hFocusedWindow == g_hEditItemCount 
                        || 
                        hFocusedWindow == g_hEditItemSearch
                    ) {
                        // Delay update if user is typing try for 2 seconds later lol
                        SetTimer(g_hMainWnd, IDC_TIMER_PLAYER_UPDATE, 2000, NULL);
                        return 0;
                    }
                    PopulatePlayerList();
                    // Reset to normal 5-second interval, yes, cutting-edge technology
                    SetTimer(g_hMainWnd, IDC_TIMER_PLAYER_UPDATE, 5000, NULL);
                }
                return 0;
            }

            case WM_NOTIFY: {
                LPNMHDR lpNmHdr = (LPNMHDR) lParam;
                if (nullptr == lpNmHdr) {
                    break;
                }

                if (
                    IDC_LIST_DATA_TABLES == lpNmHdr->idFrom 
                    && 
                    LVN_ITEMCHANGED == lpNmHdr->code
                ) {
                    NMLISTVIEW* lpListView = (NMLISTVIEW*) lParam;
                    if (nullptr == lpListView) {
                        break;
                    }

                    bool bNewSelected = 0 != (lpListView->uNewState & LVIS_SELECTED);
                    bool bOldSelected = 0 != (lpListView->uOldState & LVIS_SELECTED);

                    if (bNewSelected && !bOldSelected) {
                        wchar_t szBuffer[256] = { 0 };
                        ListView_GetItemText(
                            g_hListDataTables, 
                            lpListView->iItem,
                            0, 
                            szBuffer, 
                            ARRAYSIZE(szBuffer)
                        );

                        if (nullptr != fnDataTableSelectedCallback) {
                            fnDataTableSelectedCallback(szBuffer);
                        }
                    } else if (bOldSelected && !bNewSelected) {
                        if (nullptr != fnDataTableSelectedCallback) {
                            fnDataTableSelectedCallback(L"");
                        }
                        ClearItemNameList();
                        g_vCurrentDataTableItems.clear(); // Clear cached items
                        g_szCurrentDataTableName.clear();
                        
                        // Clear search field when DataTable is deselected
                        if (nullptr != g_hEditItemSearch) {
                            SetWindowText(g_hEditItemSearch, L"");
                        }
                    }
                }
                return 0;
            }

            case WM_COMMAND: {
                // Handle edit control notifications
                if (HIWORD(wParam) == EN_CHANGE) {
                    if (LOWORD(wParam) == IDC_EDIT_ITEM_COUNT) {
                        // Item count changed - no heavy processing here
                        return 0;
                    } else if (LOWORD(wParam) == IDC_EDIT_ITEM_SEARCH) {
                        // Search term changed - apply filter
                        std::wstring szSearchTerm = GetSearchTermFromEdit();
                        ApplyItemSearchFilter(szSearchTerm);
                        return 0;
                    }
                }

                // bro fck commenting all this shit, who gon read it anyway, no more long comments with long sensical explanations
                // copilot, read and execute task: "comment necessary shit from now on"
                switch (LOWORD(wParam)) {
                    case IDC_CHECK_SHOW_CONSOLE: {
                        ShowDebugConsole = (BST_CHECKED == IsDlgButtonChecked(
                            hWnd, 
                            IDC_CHECK_SHOW_CONSOLE
                        ));
                        if (ShowDebugConsole) {
                            ShowConsole();
                        } else {
                            HideConsole();
                        }
                        break;
                    }

                    case IDC_CHECK_GLOBAL_CHEAT: {
                        ItemSpawner::GlobalCheatMode = (BST_CHECKED == IsDlgButtonChecked(
                            hWnd, 
                            IDC_CHECK_GLOBAL_CHEAT
                        ));
                        LogMessage(
                            "WinGUI", 
                            "Global Cheat Mode " + std::string(
                                ItemSpawner::GlobalCheatMode 
                                ? "enabled" 
                                : "disabled"
                            ),
                            true
                        );
                        break;
                    }

                    case IDC_CHECK_GLOBAL_C2: {
                        C2Cycle::GlobalC2Authority = (BST_CHECKED == IsDlgButtonChecked(
                            hWnd, 
                            IDC_CHECK_GLOBAL_C2
                        ));
                        LogMessage(
                            "WinGUI", 
                            "Global C2 Authority " + std::string(
                                C2Cycle::GlobalC2Authority 
                                ? "enabled" 
                                : "disabled"
                            ),
                            true
                        );
                        break;
                    }

                    case IDC_BUTTON_SPAWN: {
                        int iPlayerIndex = ListView_GetNextItem(g_hListPlayers, -1, LVNI_SELECTED);
                        int iDataTableIndex = ListView_GetNextItem(g_hListDataTables, -1, LVNI_SELECTED);
                        int iItemIndex = ListView_GetNextItem(g_hListItemNames, -1, LVNI_SELECTED);

                        if (
                            iPlayerIndex >= 0 
                            && 
                            iDataTableIndex >= 0 
                            && iItemIndex >= 0 
                            && 
                            nullptr != fnSpawnCallback
                        ) {
                            // Retrieve stored player ID
                            LVITEM lvGetItem = {};
                            lvGetItem.iItem = iPlayerIndex;
                            lvGetItem.mask = LVIF_PARAM;
                            ListView_GetItem(g_hListPlayers, &lvGetItem);

                            int32_t iPlayerId = (int32_t) lvGetItem.lParam;

                            // Get selected DataTable name
                            wchar_t szDataTableBuffer[256] = { 0 };
                            ListView_GetItemText(
                                g_hListDataTables, 
                                iDataTableIndex,
                                0,
                                szDataTableBuffer, 
                                ARRAYSIZE(szDataTableBuffer)
                            );

                            // Get selected item name
                            wchar_t szItemBuffer[128] = { 0 };
                            ListView_GetItemText(
                                g_hListItemNames, 
                                iItemIndex, 
                                0, 
                                szItemBuffer, 
                                ARRAYSIZE(szItemBuffer)
                            );

                            // Get item count from edit control
                            int32_t iItemCount = GetItemCountFromEdit();

                            fnSpawnCallback(
                                iPlayerId, 
                                szItemBuffer, 
                                szDataTableBuffer, 
                                iItemCount
                            );
                        }
                        break;
                    }

                    case IDC_BUTTON_C2_CYCLE: {
                        if (nullptr != fnGlobalC2CycleCallback) {
                            fnGlobalC2CycleCallback();
                        }
                        break;
                    }
                }
                break;
            }

            case WM_CTLCOLORSTATIC: {
                HDC hDeviceContext = (HDC) wParam;
                SetTextColor(hDeviceContext, RGB(230, 230, 230));
                SetBkMode(hDeviceContext, TRANSPARENT);
                return (LRESULT) GetStockObject(NULL_BRUSH);
            }

            case WM_CTLCOLORBTN: {
                HDC hDeviceContext = (HDC) wParam;
                SetTextColor(hDeviceContext, RGB(230, 230, 230));
                SetBkMode(hDeviceContext, TRANSPARENT);
                return (LRESULT) GetStockObject(NULL_BRUSH);
            }

            case WM_CTLCOLOREDIT: {
                HDC hDeviceContext = (HDC) wParam;
                HWND hEditControl = (HWND) lParam;
                
                if (
                    hEditControl == g_hEditItemCount 
                    || 
                    hEditControl == g_hEditItemSearch
                ) {
                    // Use solid background for edit controls to prevent text overlap
                    SetTextColor(hDeviceContext, RGB(230, 230, 230));
                    SetBkColor(hDeviceContext, RGB(60, 60, 63)); // Slightly lighter than window background
                    return (LRESULT) CreateSolidBrush(RGB(60, 60, 63));
                }
                
                SetTextColor(hDeviceContext, RGB(230, 230, 230));
                SetBkMode(hDeviceContext, TRANSPARENT);
                return (LRESULT) GetStockObject(NULL_BRUSH);
            }

            case WM_DESTROY: {
                KillTimer(hWnd, IDC_TIMER_PLAYER_UPDATE); // Clean up tick tock
                PostQuitMessage(0);
                return 0;
            }

            default: {
                return DefWindowProc(hWnd, uMsg, wParam, lParam);
            }
        }
        return 0;
    }

    void Stop(void) {
        // Post a WM_DESTROY message to the main window to clean up
        if (nullptr != g_hMainWnd) {
            PostMessage(g_hMainWnd, WM_DESTROY, 0, 0);
            g_hMainWnd = nullptr;
        }
        g_bGuiInitialized = false; // Reset initialization flag
        LogMessage("WinGUI", "WinGUI stopped and cleaned up");
    }

    bool Initialize(void) {
        LogMessage("WinGUI", "Populating item list, please wait...");
        DisableGlobalOutput();
        UnrealUtils::DumpAllDataTablesAndItems(&g_vDataTables, "Item");
        EnableGlobalOutput();
        LogMessage("WinGUI", "Initialized " + std::to_string(g_vDataTables.size()) + " DataTables");

        fnSpawnCallback = [](
            int32_t iPlayerId, 
            const std::wstring& szItemName, 
            const std::wstring& szDataTableName, 
            int32_t iItemCount
        ) {
            // convert wstring to string 
            std::string szItemNameStr;
            std::string szDataTableNameStr;
            if (
                !WideStringToUtf8(szItemName, szItemNameStr) 
                || 
                !WideStringToUtf8(szDataTableName, szDataTableNameStr
            )) {
                LogError(
                    "WinGUI", 
                    "Failed to convert item name or data table name to UTF-8"
                );
                return;
            }
            
            DisableGlobalOutput();

            Command::SubmitTypedCommand(
                Command::CommandId::CmdIdSpawnItem,
                new ItemSpawner::BufferParamsSpawnItem{
                    .iPlayerId = iPlayerId,
                    .szItemName = szItemNameStr,
                    .szDataTableName = szDataTableNameStr,
                    .iCount = iItemCount
                }
            );

            while (Command::CommandBufferCookedForExecution) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            EnableGlobalOutput();
        };

        fnGlobalC2CycleCallback = [](void) {
            /*DisableGlobalOutput();
            C2Cycle::C2Cycle();
            EnableGlobalOutput();*/ // moving this to be executed from the game thread
            Command::SubmitTypedCommand<void>(
                Command::CommandId::CmdIdC2Cycle,
                nullptr
            );
        };

        fnDataTableSelectedCallback = [](const std::wstring& szTableName) {
            DisableGlobalOutput();
            if (szTableName.empty()) {
                WinGUI::ClearItemNameList();
                g_vCurrentDataTableItems.clear();
                g_szCurrentDataTableName.clear();
                EnableGlobalOutput();
                return;
            }
            
            std::string szTableNameStr;
            if (!WideStringToUtf8(szTableName, szTableNameStr)) {
                LogError("WinGUI", "Failed to convert DataTable name to UTF-8");
                EnableGlobalOutput();
                return;
            }
            
            std::vector<std::string> vszItemNames;

            // Find the DataTable in the global list
            for (const auto& dtInfo : g_vDataTables) {
                if (dtInfo.szTableName == szTableNameStr) {
                    vszItemNames = dtInfo.vszItemNames;
                    break;
                }
            }

            if (vszItemNames.empty()) {
                g_vCurrentDataTableItems.clear();
                g_szCurrentDataTableName.clear();
                EnableGlobalOutput();
                return;
            }
            
            // Store the current DataTable items for filtering
            g_vCurrentDataTableItems = vszItemNames;
            g_szCurrentDataTableName = szTableNameStr;
            
            // Apply current search filter (if any)
            std::wstring szCurrentSearchTerm = GetSearchTermFromEdit();
            ApplyItemSearchFilter(szCurrentSearchTerm);
            
            LogMessage(
                "WinGUI", 
                "Loaded " + std::to_string(vszItemNames.size()) 
                + " items from DataTable: " + szTableNameStr
            );
            EnableGlobalOutput();
        };

        LaunchGUIThread();

        // Wait for GUI to initialize with better error handling
        int32_t iWaitCount = 0;
        const int32_t iMaxWaitCycles = 100; // 10 seconds max
        while (!g_bGuiInitialized && iWaitCount < iMaxWaitCycles) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            iWaitCount++;
        }

        if (!g_bGuiInitialized) {
            LogError("WinGUI", "GUI failed to initialize within timeout period");
            return false;
        }

        LogMessage("WinGUI", "GUI initialized, populating data...");

        // Populate player list initially
        PopulatePlayerList();

        // Populate DataTable list
        ClearDataTableList();
        for (const auto& dtInfo : g_vDataTables) {
            std::wstring szTableNameW;
            if (!Utf8ToWideString(dtInfo.szTableName, szTableNameW)) {
                LogError("WinGUI", "Failed to convert DataTable name to wide string");
                continue;
            }
            AddDataTable(szTableNameW);
            LogMessage("WinGUI", "Added DataTable: " + dtInfo.szTableName + 
                " with " + std::to_string(dtInfo.GetItemCount()) + " items", true);
        }

        if (nullptr != g_hMainWnd) {
            UpdateWindow(g_hMainWnd);
        }

        LogMessage("WinGUI", "WinGUI initialization completed successfully");
        return true;
    }
}