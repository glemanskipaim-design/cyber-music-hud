#include "media.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

#include <wincodec.h>
#include <wrl/client.h>

#include <chrono>

#include "procutil.h"

namespace cmh {
namespace {

using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;

constexpr DWORD kPollMs = 400;
constexpr DWORD kRetryMs = 5000;  // GSMTC indisponível: tenta de novo sem martelar
constexpr std::chrono::milliseconds kArtTimeout{1500};  // provedor da capa travado não segura a thread

struct Playing {
  Track track;
  Source source;
  std::wstring sourceId;
  winrt::Windows::Storage::Streams::IRandomAccessStreamReference thumbnail{nullptr};
};

template <typename Async>
bool Finished(const Async& op) {
  if (op.wait_for(kArtTimeout) == winrt::Windows::Foundation::AsyncStatus::Completed) return true;
  op.Cancel();
  return false;
}

// Copia a imagem para a memória, com prazo e limite de tamanho próprios: o
// provedor (outro app) pode travar ou mentir em Size().
std::vector<uint8_t> ReadAllBounded(const winrt::Windows::Storage::Streams::IRandomAccessStreamReference& ref) {
  using namespace winrt::Windows::Storage::Streams;
  const auto open = ref.OpenReadAsync();
  if (!Finished(open)) return {};
  const auto stream = open.GetResults();
  std::vector<uint8_t> out;
  Buffer chunk(64 * 1024);
  while (true) {
    const auto read = stream.ReadAsync(chunk, chunk.Capacity(), InputStreamOptions::None);
    if (!Finished(read)) return {};
    const IBuffer got = read.GetResults();
    if (got.Length() == 0) return out;
    if (out.size() + got.Length() > kMaxArtFileBytes) return {};
    out.insert(out.end(), got.data(), got.data() + got.Length());
  }
}

const GUID* ContainerFor(ImageFormat format) {
  switch (format) {
    case ImageFormat::Png: return &GUID_ContainerFormatPng;
    case ImageFormat::Jpeg: return &GUID_ContainerFormatJpeg;
    case ImageFormat::Gif: return &GUID_ContainerFormatGif;
    case ImageFormat::Bmp: return &GUID_ContainerFormatBmp;
    case ImageFormat::Unknown: break;
  }
  return nullptr;
}

// Decodifica a capa (PNG/JPEG/GIF/BMP), corta o quadrado central —
// miniatura de vídeo do YouTube é 16:9 —, reduz para kArtPixels e tinge.
// Vazio em qualquer falha: o HUD só deixa de mostrar a capa.
std::vector<uint8_t> LoadArt(const winrt::Windows::Storage::Streams::IRandomAccessStreamReference& ref) {
  using Microsoft::WRL::ComPtr;
  if (!ref) return {};
  try {
    const std::vector<uint8_t> bytes = ReadAllBounded(ref);
    const GUID* container = ContainerFor(SniffImage(bytes.data(), bytes.size()));
    if (!container) return {};

    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> source;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    // Decoder escolhido pelo formato e restrito aos codecs nativos: dado não
    // confiável nunca chega a um codec de terceiro (HEIF, RAW...).
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateStream(&source)) ||
        FAILED(source->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()))) ||
        FAILED(wic->CreateDecoder(*container, &GUID_VendorMicrosoftBuiltIn, &decoder)) ||
        FAILED(decoder->Initialize(source.Get(), WICDecodeMetadataCacheOnDemand)) ||
        FAILED(decoder->GetFrame(0, &frame))) {
      return {};
    }
    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0 || w > kMaxArtSide || h > kMaxArtSide) return {};
    const UINT side = std::min(w, h);
    const WICRect square = {static_cast<INT>((w - side) / 2), static_cast<INT>((h - side) / 2),
                            static_cast<INT>(side), static_cast<INT>(side)};

    ComPtr<IWICBitmapClipper> clipper;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateBitmapClipper(&clipper)) || FAILED(clipper->Initialize(frame.Get(), &square)) ||
        FAILED(wic->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(clipper.Get(), kArtPixels, kArtPixels, WICBitmapInterpolationModeFant)) ||
        FAILED(wic->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                     0.0, WICBitmapPaletteTypeCustom))) {
      return {};
    }
    std::vector<uint8_t> pixels(kArtBytes);
    if (FAILED(converter->CopyPixels(nullptr, kArtStride, static_cast<UINT>(pixels.size()), pixels.data()))) {
      return {};
    }
    TintArt(pixels, kArtPixels);
    return pixels;
  } catch (const winrt::hresult_error&) {
    return {};
  }
}

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
  p.thumbnail = props.Thumbnail();
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
    std::wstring artKey;  // capa da última faixa anunciada, reaplicada ao retomar
    std::vector<uint8_t> art;
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
      const bool show = announce && WorthAnnouncing(playing->track.platform, playing->track.artist);
      // A capa entra junto com o anúncio: "Mostrar agora" também a reaproveita.
      if (show) {
        art = LoadArt(playing->thumbnail);
        artKey = key;
      }
      if (playing && key == artKey) playing->track.art = art;
      std::lock_guard lock(mutex_);
      current_ = playing ? std::optional<Track>(playing->track) : std::nullopt;
      if (show) {
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
