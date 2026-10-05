// Processos e janelas de um executável: raiz da árvore (para o loopback de
// áudio) e títulos das janelas (para descobrir se o navegador está no YouTube).
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace cmh {

struct ProcessGroup {
  std::vector<DWORD> pids;  // todos os processos com esse executável
  DWORD root = 0;           // pai da árvore (o navegador/Spotify "principal")
};

// exeLower: nome do executável em minúsculas ("chrome.exe").
ProcessGroup FindProcesses(const std::wstring& exeLower);

// Títulos das janelas de topo visíveis dos processos dados.
std::vector<std::wstring> WindowTitlesOf(const std::vector<DWORD>& pids);

}  // namespace cmh
