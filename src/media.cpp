#include "media.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>

#include "procutil.h"

namespace cmh {
namespace {

using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;

constexpr DWORD kPollMs = 400;
constexpr DWORD kRetryMs = 5000;  // GSMTC indisponível: tenta de novo sem martelar

struct Playing {
  Track track;
  Source source;
  std::wstring sourceId;
};

std::optional<Playing> ReadSession(const GlobalSystemMediaTransportControlsSession& session) {
  Playing p;
  p.sourceId = session.SourceAppUserModelId().c_str();
  p.source = ClassifySource(p.sourceId);
  if (!p.source.relevant) return std::nullopt;
  const auto info = session.GetPlaybackInfo();
  if (!info || info.PlaybackStatus() != GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing) {
    return std::nullopt;
  }
  const auto props = session.TryGetMediaPropertiesAsync().get();
  if (!props) return std::nullopt;
  // Metadados vêm de qualquer app: limitar evita texto gigante sendo
  // relaído a cada quadro do HUD.
  p.track.title = ClampText(props.Title().c_str());
  p.track.artist = ClampText(props.Artist().c_str());
  if (p.track.title.empty()) return std::nullopt;
  p.track.platform = p.source.platform;
  return p;
}

// A sessão "atual" do Windows ganha quando está tocando; senão, a primeira
// relevante que estiver tocando.
std::optional<Playing> FindPlaying(const GlobalSystemMediaTransportControlsSessionManager& manager) {
  if (const auto current = manager.GetCurrentSession()) {
    if (auto p = ReadSession(current)) return p;
  }
  for (const auto& session : manager.GetSessions()) {
    if (auto p = ReadSession(session)) return p;
  }
  return std::nullopt;
}

}  // namespace

void ResolveTrackSource(Track& track, const Source& source) {
  const ProcessGroup group = FindProcesses(source.exe);
  track.audioPid = group.root;
  if (source.browser && track.platform == Platform::Browser) {
    track.platform = PlatformFromWindowTitles(WindowTitlesOf(group.pids), track.title);
  }
  track.platformLabel = PlatformLabel(track.platform, source.label);
}

MediaWatcher::~MediaWatcher() { Stop(); }

bool MediaWatcher::Start(HWND notify, UINT message) {
  Stop();
  notify_ = notify;
  message_ = message;
  stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!stopEvent_) return false;
  thread_ = std::thread(&MediaWatcher::Run, this);
  return true;
}

void MediaWatcher::Stop() {
  if (thread_.joinable()) {
    SetEvent(stopEvent_);
    thread_.join();
  }
  if (stopEvent_) CloseHandle(stopEvent_);
  stopEvent_ = nullptr;
}

std::optional<Track> MediaWatcher::TakeAnnounced() {
  std::lock_guard lock(mutex_);
  std::optional<Track> t = std::move(announced_);
  announced_.reset();
  return t;
}

std::optional<Track> MediaWatcher::Current() const {
  std::lock_guard lock(mutex_);
  return current_;
}

void MediaWatcher::Run() {
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  {
    GlobalSystemMediaTransportControlsSessionManager manager{nullptr};
    ChangeDetector detector;
    std::wstring resolvedKey;
    DWORD wait = 0;  // primeira consulta imediata: anuncia o que já está tocando
    while (WaitForSingleObject(stopEvent_, wait) == WAIT_TIMEOUT) {
      wait = kPollMs;
      std::optional<Playing> playing;
      try {
        if (!manager) manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        playing = FindPlaying(manager);
      } catch (const winrt::hresult_error&) {
        // Serviço reiniciado ou sessão sumiu no meio da consulta.
        manager = nullptr;
        wait = kRetryMs;
        continue;
      }

      const std::wstring key =
          playing ? TrackKey(playing->sourceId, playing->track.title, playing->track.artist) : std::wstring();
      const bool announce = detector.Update(key);
      // Plataforma e PID custam uma enumeração de processos/janelas: só
      // quando a faixa muda ou vai ser anunciada (o título da aba do YouTube
      // pode atualizar depois dos metadados), não a cada consulta.
      if (key == resolvedKey && !announce) continue;
      resolvedKey = key;
      if (playing) ResolveTrackSource(playing->track, playing->source);
      std::lock_guard lock(mutex_);
      current_ = playing ? std::optional<Track>(playing->track) : std::nullopt;
      if (announce && WorthAnnouncing(playing->track.platform, playing->track.artist)) {
        announced_ = playing->track;
        if (notify_) PostMessageW(notify_, message_, 0, 0);
      }
    }
  }
  winrt::uninit_apartment();
}

std::vector<std::wstring> MediaWatcher::DescribeSessions() {
  std::vector<std::wstring> lines;
  try {
    const auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
    const auto current = manager.GetCurrentSession();
    const std::wstring currentId = current ? current.SourceAppUserModelId().c_str() : L"";
    for (const auto& session : manager.GetSessions()) {
      const std::wstring id = session.SourceAppUserModelId().c_str();
      const Source source = ClassifySource(id);
      std::wstring status = L"?";
      if (const auto info = session.GetPlaybackInfo()) {
        switch (info.PlaybackStatus()) {
          case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing: status = L"tocando"; break;
          case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused: status = L"pausado"; break;
          case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Stopped: status = L"parado"; break;
          default: status = L"outro"; break;
        }
      }
      Track t;
      if (const auto props = session.TryGetMediaPropertiesAsync().get()) {
        t.title = props.Title().c_str();
        t.artist = props.Artist().c_str();
      }
      t.platform = source.platform;
      std::wstring platform = L"(ignorada)";
      std::wstring pid = L"-";
      if (source.relevant) {
        ResolveTrackSource(t, source);
        platform = t.platformLabel;
        pid = std::to_wstring(t.audioPid);
      }
      lines.push_back((id == currentId ? L"* " : L"  ") + id + L" | " + status + L" | " + platform +
                      L" | pid " + pid + L" | \"" + t.title + L"\" — \"" + t.artist + L"\"");
    }
    if (lines.empty()) lines.push_back(L"  (nenhuma sessão de mídia aberta)");
  } catch (const winrt::hresult_error& e) {
    lines.push_back(L"  GSMTC indisponível: " + std::wstring(e.message().c_str()));
  }
  return lines;
}

}  // namespace cmh
