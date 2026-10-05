#include "procutil.h"

#include <tlhelp32.h>

#include <algorithm>
#include <unordered_map>

#include "logic.h"

namespace cmh {
namespace {

struct TitleSearch {
  const std::vector<DWORD>* pids;
  std::vector<std::wstring>* titles;
};

BOOL CALLBACK CollectTitle(HWND hwnd, LPARAM param) {
  auto* search = reinterpret_cast<TitleSearch*>(param);
  if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (std::find(search->pids->begin(), search->pids->end(), pid) == search->pids->end()) return TRUE;
  wchar_t buf[512];
  const int n = GetWindowTextW(hwnd, buf, static_cast<int>(std::size(buf)));
  if (n > 0) search->titles->emplace_back(buf, static_cast<size_t>(n));
  return TRUE;
}

}  // namespace

ProcessGroup FindProcesses(const std::wstring& exeLower) {
  ProcessGroup group;
  if (exeLower.empty()) return group;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return group;

  std::unordered_map<DWORD, DWORD> parentOf;
  PROCESSENTRY32W entry = {sizeof(entry)};
  for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
    if (Lower(entry.szExeFile) == exeLower) {
      group.pids.push_back(entry.th32ProcessID);
      parentOf[entry.th32ProcessID] = entry.th32ParentProcessID;
    }
  }
  CloseHandle(snapshot);

  // Raiz: o processo cujo pai não é do mesmo executável. Com mais de uma
  // (perfis separados), vale a que tem janela.
  std::vector<DWORD> roots;
  for (DWORD pid : group.pids) {
    if (!parentOf.contains(parentOf[pid])) roots.push_back(pid);
  }
  for (DWORD root : roots) {
    if (!WindowTitlesOf({root}).empty()) {
      group.root = root;
      return group;
    }
  }
  if (!roots.empty()) group.root = roots.front();
  return group;
}

std::vector<std::wstring> WindowTitlesOf(const std::vector<DWORD>& pids) {
  std::vector<std::wstring> titles;
  if (pids.empty()) return titles;
  TitleSearch search{&pids, &titles};
  EnumWindows(CollectTitle, reinterpret_cast<LPARAM>(&search));
  return titles;
}

}  // namespace cmh
