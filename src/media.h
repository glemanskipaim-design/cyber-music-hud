// Observa as sessões de mídia do Windows (GSMTC — o mesmo painel de mídia do
// volume/central de ações) e avisa quando uma música nova começa no Spotify
// ou num navegador.
//
// O GSMTC é consultado numa thread MTA própria a cada kPollMs: é uma chamada
// local barata e mais confiável que os eventos, que o Spotify às vezes
// dispara antes de atualizar os metadados.
#pragma once

#include <windows.h>

#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "logic.h"

namespace cmh {

struct Track {
  std::wstring title;
  std::wstring artist;
  std::wstring platformLabel;  // "SPOTIFY", "YOUTUBE", "CHROME"...
  Platform platform = Platform::Browser;
  DWORD audioPid = 0;          // raiz da árvore do player, 0 se não achou
};

class MediaWatcher {
 public:
  ~MediaWatcher();

  // Posta `message` (sem parâmetros) para `notify` a cada música nova; a
  // faixa fica em TakeAnnounced().
  bool Start(HWND notify, UINT message);
  void Stop();

  std::optional<Track> TakeAnnounced();
  // Faixa tocando na última consulta (para "mostrar agora" da bandeja).
  std::optional<Track> Current() const;

  // Linhas legíveis com todas as sessões, para o modo --probe.
  static std::vector<std::wstring> DescribeSessions();

 private:
  void Run();

  HWND notify_ = nullptr;
  UINT message_ = 0;
  HANDLE stopEvent_ = nullptr;
  std::thread thread_;

  mutable std::mutex mutex_;
  std::optional<Track> announced_;
  std::optional<Track> current_;
};

// Completa plataforma e PID de áudio (enumera processos e janelas). Separado
// da consulta porque só roda quando há algo para anunciar.
void ResolveTrackSource(Track& track, const Source& source);

}  // namespace cmh
