
#include "stdafx.h"
#include "../client/DCPlusPlus.h"
#include "Resource.h"
#include "WinUtil.h"
#include "FavoriteDirDlg.h"

#define ATTACH(id, var) var.Attach(GetDlgItem(id))

FavoriteDirDlg::FavoriteDirDlg() {
	vName = _T("");
}

FavoriteDirDlg::~FavoriteDirDlg() {
	ctrlName.Detach();
}

LRESULT FavoriteDirDlg::OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/) {

	CRect rc;
	
	pathListCtrl.Attach(GetDlgItem(IDC_DIRPATHS));
	pathListCtrl.GetClientRect(rc);
	pathListCtrl.InsertColumn(0, _T("Dummy"), LVCFMT_LEFT, (rc.Width() - 17), 0);
	pathListCtrl.SetExtendedListViewStyle(LVS_EX_FULLROWSELECT | LVS_EX_INFOTIP);

	for(auto i = paths.begin(); i != paths.end(); ++i) {
		pathListCtrl.insert(pathListCtrl.GetItemCount(), Text::toT(*i).c_str());
	}

	ATTACH(IDC_FDNAME, ctrlName);
	ctrlName.SetWindowText(vName.c_str());

	::SetWindowText(GetDlgItem(IDOK), (TSTRING(OK)).c_str());
	::SetWindowText(GetDlgItem(IDCANCEL), (TSTRING(CANCEL)).c_str());
	::SetWindowText(GetDlgItem(IDC_SETTINGS_FAVDIR_PATHS), (TSTRING(PATHS_FAVDIRS)).c_str());
	::SetWindowText(GetDlgItem(IDC_FDNAME_TEXT), (TSTRING(FAVORITE_DIR_NAME)).c_str());
	::SetWindowText(GetDlgItem(IDC_SETTINGS_FAVORITE_DIRECTORIES), (TSTRING(SETTINGS_DIRECTORIES)).c_str());

	CenterWindow(GetParent());
	return TRUE;
}

LRESULT FavoriteDirDlg::onBrowse(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	tstring dir = Text::toT(SETTING(TEMP_DOWNLOAD_DIRECTORY));
	if(WinUtil::browseDirectory(dir, m_hWnd))
	{
		// Adjust path string
		if(dir.size() > 0 && dir[dir.size() - 1] != '\\')
			dir += '\\';

		SetDlgItemText(IDC_FAVDIR_EDIT, dir.c_str());
	}
	return 0;
}

LRESULT FavoriteDirDlg::onAddPath(WORD /* wNotifyCode */, WORD /*wID*/, HWND /* hWndCtl */, BOOL& /* bHandled */) {
	TCHAR buf[MAX_PATH];
	if(GetDlgItemText(IDC_FAVDIR_EDIT, buf, MAX_PATH)) {
		addPath(buf);
	}

	SetDlgItemText(IDC_FAVDIR_EDIT, _T(""));
	return 0;
}

void FavoriteDirDlg::addPath(const tstring& aPath) {
	if (find(paths.begin(), paths.end(), Text::fromT(aPath)) == paths.end()) {
		paths.push_back(Text::fromT(aPath));
		pathListCtrl.insert(pathListCtrl.GetItemCount(), aPath);
	} else {

	}
}

LRESULT FavoriteDirDlg::onRemovePath(WORD /* wNotifyCode */, WORD /*wID*/, HWND /* hWndCtl */, BOOL& /* bHandled */) {
	int i = -1;
	
	TCHAR buf[MAX_PATH];

	while((i = pathListCtrl.GetNextItem(-1, LVNI_SELECTED)) != -1) {
		pathListCtrl.GetItemText(i, 0, buf, MAX_PATH);
		paths.erase(remove(paths.begin(), paths.end(), Text::fromT(buf)), paths.end());
		//ignoreList.erase(buf);
		pathListCtrl.DeleteItem(i);
	}

	return 0;
}

LRESULT FavoriteDirDlg::OnCloseCmd(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	if(wID == IDOK) {
		TCHAR buf[MAX_PATH];
		tstring tmp;
		if (ctrlName.GetWindowTextLength() == 0) {
			MessageBox(CTSTRING(LINE_EMPTY));
			return 0;
		}
		GetDlgItemText(IDC_FDNAME, buf, MAX_PATH);
		vName = buf;

		if(paths.empty()) {
			MessageBox(CTSTRING(LINE_EMPTY));
			return 0;
		}
	}
	EndDialog(wID);
	return 0;
}


LRESULT FavoriteDirDlg::onEditChange(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	if(wID == IDC_FAVDIR_EDIT) {
		::EnableWindow(GetDlgItem(IDC_FAVDIR_ADD), (::GetWindowTextLength(GetDlgItem(IDC_FAVDIR_EDIT)) > 0));
	}
	return 0;
}

LRESULT FavoriteDirDlg::onItemchangedDirectories(int /*idCtrl*/, LPNMHDR pnmh, BOOL& /*bHandled*/)
{
	NM_LISTVIEW* lv = (NM_LISTVIEW*) pnmh;
	::EnableWindow(GetDlgItem(IDC_FAVDIR_REMOVE), (lv->uNewState & LVIS_FOCUSED));
	TCHAR buf[MAX_PATH];
	pathListCtrl.GetItemText(pathListCtrl.GetSelectedIndex(), 0, buf, MAX_PATH);
	SetDlgItemText(IDC_FAVDIR_EDIT, buf);
	return 0;
}


LRESULT FavoriteDirDlg::onDropFiles(UINT /*uMsg*/, WPARAM wParam, LPARAM /*lParam*/, BOOL& /*bHandled*/){
	HDROP drop = (HDROP)wParam;
	tstring buf;
	buf.resize(MAX_PATH);

	UINT nrFiles;
	
	nrFiles = DragQueryFile(drop, (UINT)-1, NULL, 0);
	
	for(UINT i = 0; i < nrFiles; ++i){
		if(DragQueryFile(drop, i, &buf[0], MAX_PATH)){
			if (PathIsDirectory(&buf[0]))
				addPath(buf);
		}
	}

	DragFinish(drop);

	return 0;
}
