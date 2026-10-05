#include "hud.h"

#include <dwrite_1.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace cmh {
namespace {

constexpr wchar_t kClassName[] = L"CyberMusicHudOverlay";
constexpr UINT_PTR kFrameTimer = 1;
constexpr UINT kFrameMs = 33;

// Medidas em DIP (escaladas pelo DPI do monitor).
constexpr float kWidth = 360.0f;
constexpr float kHeight = 112.0f;
constexpr float kMargin = 20.0f;      // distância da borda da área de trabalho
constexpr float kSlideDip = 14.0f;    // quanto o painel sobe ao aparecer
constexpr float kPadX = 18.0f;
constexpr float kRadius = 6.0f;
constexpr float kTextX = 38.0f;       // título/artista alinhados depois do ▶
constexpr float kTitleY = 15.0f;
constexpr float kArtistY = 37.0f;
constexpr float kBarsTop = 64.0f;
constexpr float kBarsBottom = 94.0f;
constexpr float kBarGap = 3.0f;
constexpr float kTagGap = 14.0f;      // folga entre título e etiqueta da plataforma
constexpr float kTitleFontSize = 14.5f;
constexpr float kArtistFontSize = 11.5f;
constexpr float kTagFontSize = 9.0f;
constexpr float kTitleTracking = 0.4f;
constexpr float kTagTracking = 1.8f;
constexpr float kBracket = 9.0f;
constexpr float kPeakMin = 0.15f;  // marcas de pico só onde houve sinal de verdade
constexpr float kLayoutHeight = 40.0f;

// "Decodificação" do texto ao aparecer: início e duração em ms.
constexpr double kTitleDecodeStart = 60, kTitleDecodeMs = 420;
constexpr double kArtistDecodeStart = 170, kArtistDecodeMs = 340;
constexpr double kDecodeFrameMs = 45;  // troca de glifos ~22x/s, não a cada frame

// Paleta da família "cyber" (mesma do Cyber Process Switcher): preto azulado
// translúcido, ciano como destaque único e um traço magenta mínimo.
constexpr D2D1_COLOR_F kPanel = {0.016f, 0.022f, 0.034f, 0.95f};
constexpr D2D1_COLOR_F kBorder = {0.0f, 0.898f, 1.0f, 0.16f};
constexpr D2D1_COLOR_F kAccent = {0.0f, 0.898f, 1.0f, 1.0f};
constexpr D2D1_COLOR_F kMagenta = {1.0f, 0.165f, 0.427f, 0.80f};
constexpr D2D1_COLOR_F kTitle = {0.93f, 0.96f, 0.98f, 0.96f};
constexpr D2D1_COLOR_F kArtist = {0.62f, 0.69f, 0.75f, 0.78f};
constexpr D2D1_COLOR_F kTag = {0.0f, 0.898f, 1.0f, 0.55f};
constexpr D2D1_COLOR_F kBarHot = {0.78f, 0.98f, 1.0f, 1.0f};
constexpr D2D1_COLOR_F kBaseline = {0.0f, 0.898f, 1.0f, 0.12f};

const wchar_t* PickMonoFont(IDWriteFactory* factory) {
  ComPtr<IDWriteFontCollection> fonts;
  if (FAILED(factory->GetSystemFontCollection(&fonts))) return L"Consolas";
  for (const wchar_t* family : {L"Cascadia Mono", L"Cascadia Code", L"Consolas"}) {
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (SUCCEEDED(fonts->FindFamilyName(family, &index, &exists)) && exists) return family;
  }
  return L"Consolas";
}

D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float a) {
  c.a = a;
  return c;
}

D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

double DecodeProgress(double ms, double start, double duration) {
  return std::clamp((ms - start) / duration, 0.0, 1.0);
}

}  // namespace

bool Hud::Create(HINSTANCE instance, AudioCapture* audio) {
  audio_ = audio;
  if (!CreateDeviceIndependent()) return false;

  WNDCLASSEXW wc = {sizeof(wc)};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = kClassName;
  if (!RegisterClassExW(&wc)) return false;
  instance_ = instance;

  hwnd_ = CreateWindowExW(
      WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
      kClassName, L"Cyber Music HUD", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, this);
  samples_.assign(SpectrumAnalyzer::kFftSize, 0.0f);
  return hwnd_ != nullptr;
}

void Hud::Destroy() {
  Hide();
  if (hwnd_) DestroyWindow(hwnd_);
  hwnd_ = nullptr;
  if (instance_) UnregisterClassW(kClassName, instance_);
  instance_ = nullptr;
  if (memDc_) {
    if (oldBitmap_) SelectObject(memDc_, oldBitmap_);
    DeleteDC(memDc_);
    memDc_ = nullptr;
  }
  if (bitmap_) DeleteObject(bitmap_);
  bitmap_ = nullptr;
}

LRESULT CALLBACK Hud::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (msg == WM_NCCREATE) {
    const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
  }
  auto* self = reinterpret_cast<Hud*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_TIMER:
      if (self && wParam == kFrameTimer) self->Tick();
      return 0;
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_NCHITTEST:
      return HTTRANSPARENT;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool Hud::CreateDeviceIndependent() {
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf()))) return false;
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf())))) {
    return false;
  }
  const wchar_t* font = PickMonoFont(dwrite_.Get());
  struct Spec {
    ComPtr<IDWriteTextFormat>* format;
    DWRITE_FONT_WEIGHT weight;
    float size;
  };
  const Spec specs[] = {{&titleFormat_, DWRITE_FONT_WEIGHT_SEMI_BOLD, kTitleFontSize},
                        {&artistFormat_, DWRITE_FONT_WEIGHT_NORMAL, kArtistFontSize},
                        {&tagFormat_, DWRITE_FONT_WEIGHT_SEMI_BOLD, kTagFontSize}};
  for (const Spec& spec : specs) {
    if (FAILED(dwrite_->CreateTextFormat(font, nullptr, spec.weight, DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL, spec.size, L"", spec.format->GetAddressOf()))) {
      return false;
    }
    IDWriteTextFormat* f = spec.format->Get();
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    // Texto longo termina em "…" em vez de vazar do painel.
    ComPtr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(dwrite_->CreateEllipsisTrimmingSign(f, &ellipsis))) {
      const DWRITE_TRIMMING trimming = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
      f->SetTrimming(&trimming, ellipsis.Get());
    }
  }

  // ▶ desenhado como geometria: glifos de seta variam muito entre fontes.
  if (FAILED(d2d_->CreatePathGeometry(&playIcon_))) return false;
  ComPtr<ID2D1GeometrySink> sink;
  if (FAILED(playIcon_->Open(&sink))) return false;
  sink->BeginFigure(D2D1::Point2F(0.0f, 0.0f), D2D1_FIGURE_BEGIN_FILLED);
  sink->AddLine(D2D1::Point2F(9.0f, 5.5f));
  sink->AddLine(D2D1::Point2F(0.0f, 11.0f));
  sink->EndFigure(D2D1_FIGURE_END_CLOSED);
  return SUCCEEDED(sink->Close());
}

bool Hud::EnsureRenderTarget() {
  if (target_) return true;
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_DEFAULT,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  if (FAILED(d2d_->CreateDCRenderTarget(&props, &target_))) return false;
  if (FAILED(target_->CreateSolidColorBrush(kAccent, &brush_))) {
    target_.Reset();
    return false;
  }
  // ClearType não funciona sobre fundo transparente; tons de cinza sim.
  target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  return true;
}

bool Hud::EnsureSurface(int width, int height) {
  if (bitmap_ && width == surfaceWidth_ && height == surfaceHeight_) return true;
  if (!memDc_) {
    memDc_ = CreateCompatibleDC(nullptr);
    if (!memDc_) return false;
  }
  if (bitmap_) {
    SelectObject(memDc_, oldBitmap_);
    DeleteObject(bitmap_);
    bitmap_ = nullptr;
  }
  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
  bmi.bmiHeader.biWidth = width;
  bmi.bmiHeader.biHeight = -height;  // top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  bitmap_ = CreateDIBSection(memDc_, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bitmap_) return false;
  oldBitmap_ = SelectObject(memDc_, bitmap_);
  surfaceWidth_ = width;
  surfaceHeight_ = height;
  return true;
}

ComPtr<IDWriteTextLayout> Hud::MakeLayout(const std::wstring& text, IDWriteTextFormat* format, float maxWidth,
                                          float tracking) {
  ComPtr<IDWriteTextLayout> layout;
  if (FAILED(dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format,
                                       std::max(1.0f, maxWidth), kLayoutHeight, &layout))) {
    return nullptr;
  }
  if (tracking != 0.0f) {
    ComPtr<IDWriteTextLayout1> layout1;
    if (SUCCEEDED(layout.As(&layout1))) {
      layout1->SetCharacterSpacing(0.0f, tracking, 0.0f, DWRITE_TEXT_RANGE{0, static_cast<UINT32>(text.size())});
    }
  }
  return layout;
}

void Hud::PlaceOnPrimaryMonitor() {
  const POINT origin = {0, 0};
  HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
  UINT dpiX = 96, dpiY = 96;
  if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) dpiX = 96;
  dpi_ = static_cast<float>(dpiX);
  MONITORINFO mi = {sizeof(mi)};
  GetMonitorInfoW(monitor, &mi);
  const float scale = dpi_ / 96.0f;
  const int width = static_cast<int>(std::ceil(kWidth * scale));
  const int height = static_cast<int>(std::ceil(kHeight * scale));
  const int margin = static_cast<int>(std::lround(kMargin * scale));
  anchor_.x = mi.rcWork.right - margin - width;
  anchor_.y = mi.rcWork.bottom - margin - height;
}

void Hud::Show(const Track& track) {
  if (!hwnd_) return;
  track_ = track;
  shownTitle_.clear();
  shownArtist_.clear();
  tag_ = MakeLayout(track.platformLabel, tagFormat_.Get(), kWidth, kTagTracking);
  tagWidth_ = 0;
  if (tag_) {
    DWRITE_TEXT_METRICS m = {};
    tag_->GetMetrics(&m);
    tagWidth_ = m.widthIncludingTrailingWhitespace - kTagTracking;
  }

  PlaceOnPrimaryMonitor();
  shownAt_ = lastTick_ = Clock::now();
  frame_ = 0;
  // Com o HUD já na tela, música nova só troca o texto: as barras seguem
  // sem zerar. A captura reinicia porque o player pode ter mudado.
  if (!visible_) spectrum_.Reset();
  if (audio_) audio_->Start(track.audioPid);
  visible_ = true;
  Tick();
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  SetTimer(hwnd_, kFrameTimer, kFrameMs, nullptr);
}

void Hud::Hide() {
  if (hwnd_) {
    KillTimer(hwnd_, kFrameTimer);
    ShowWindow(hwnd_, SW_HIDE);
  }
  if (audio_) audio_->Stop();
  visible_ = false;
}

void Hud::Tick() {
  const Clock::time_point now = Clock::now();
  const double ms = std::chrono::duration<double, std::milli>(now - shownAt_).count();
  const float dt = std::chrono::duration<float>(now - lastTick_).count();
  lastTick_ = now;
  if (ms >= timeline_.Total()) {
    Hide();
    return;
  }

  if (audio_) {
    if (audio_->Samples().TakeArrived() > 0) {
      if (audio_->SampleRate() > 0 && audio_->SampleRate() != sampleRate_) {
        sampleRate_ = audio_->SampleRate();
        spectrum_.SetSampleRate(sampleRate_);
      }
      audio_->Samples().Latest(samples_.data(), samples_.size());
      spectrum_.Process(samples_.data(), std::max(dt, 0.001f));
    } else {
      spectrum_.Decay(dt);
    }
  }
  UpdateText(ms);
  Render(ms);
  Present(ms);
  ++frame_;
}

void Hud::UpdateText(double ms) {
  const uint32_t glyphFrame = static_cast<uint32_t>(ms / kDecodeFrameMs);
  const std::wstring title =
      DecodeText(track_.title, DecodeProgress(ms, kTitleDecodeStart, kTitleDecodeMs), glyphFrame);
  const std::wstring artist =
      DecodeText(track_.artist, DecodeProgress(ms, kArtistDecodeStart, kArtistDecodeMs), glyphFrame);
  if (title != shownTitle_ || !title_) {
    const float maxWidth = kWidth - kTextX - kPadX - (tagWidth_ > 0 ? tagWidth_ + kTagGap : 0.0f);
    title_ = MakeLayout(title, titleFormat_.Get(), maxWidth, kTitleTracking);
    shownTitle_ = title;
  }
  if (artist != shownArtist_ || !artist_) {
    artist_ = MakeLayout(artist, artistFormat_.Get(), kWidth - kTextX - kPadX, 0.0f);
    shownArtist_ = artist;
  }
}

void Hud::Render(double ms) {
  const float scale = dpi_ / 96.0f;
  const int width = static_cast<int>(std::ceil(kWidth * scale));
  const int height = static_cast<int>(std::ceil(kHeight * scale));
  if (!EnsureSurface(width, height) || !EnsureRenderTarget()) return;

  const RECT bounds = {0, 0, width, height};
  if (FAILED(target_->BindDC(memDc_, &bounds))) return;
  target_->SetDpi(dpi_, dpi_);
  target_->BeginDraw();
  target_->Clear(D2D1::ColorF(0, 0, 0, 0));
  DrawFrame(target_.Get(), brush_.Get());
  DrawHeader(target_.Get(), brush_.Get());
  DrawBars(target_.Get(), brush_.Get(), ms);
  if (target_->EndDraw() == D2DERR_RECREATE_TARGET) {
    brush_.Reset();
    target_.Reset();
  }
}

void Hud::DrawFrame(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush) {
  const D2D1_ROUNDED_RECT panel = {D2D1::RectF(0.5f, 0.5f, kWidth - 0.5f, kHeight - 0.5f), kRadius, kRadius};
  brush->SetColor(kPanel);
  rt->FillRoundedRectangle(panel, brush);
  brush->SetColor(kBorder);
  rt->DrawRoundedRectangle(panel, brush, 1.0f);

  // Colchetes nos cantos opostos e um traço magenta: a "assinatura" visual.
  brush->SetColor(WithAlpha(kAccent, 0.85f));
  const float s = 1.4f, i = 4.0f;  // espessura, recuo da borda
  rt->FillRectangle(D2D1::RectF(i, i, i + kBracket, i + s), brush);
  rt->FillRectangle(D2D1::RectF(i, i, i + s, i + kBracket), brush);
  rt->FillRectangle(D2D1::RectF(kWidth - i - kBracket, kHeight - i - s, kWidth - i, kHeight - i), brush);
  rt->FillRectangle(D2D1::RectF(kWidth - i - s, kHeight - i - kBracket, kWidth - i, kHeight - i), brush);
  brush->SetColor(kMagenta);
  rt->FillRectangle(D2D1::RectF(kWidth - i - 12.0f, i, kWidth - i, i + s), brush);
}

void Hud::DrawHeader(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush) {
  // ▶ alinhado ao meio da linha do título.
  rt->SetTransform(D2D1::Matrix3x2F::Translation(kPadX + 1.0f, kTitleY + 4.5f));
  // Pulsa com o grave: a mesma música que mexe as barras mexe o ícone.
  brush->SetColor(WithAlpha(kAccent, 0.55f + 0.45f * std::min(1.0f, spectrum_.Bass() * 1.3f)));
  rt->FillGeometry(playIcon_.Get(), brush);
  rt->SetTransform(D2D1::Matrix3x2F::Identity());

  if (title_) {
    brush->SetColor(kTitle);
    rt->DrawTextLayout(D2D1::Point2F(kTextX, kTitleY), title_.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
  }
  if (artist_) {
    brush->SetColor(kArtist);
    rt->DrawTextLayout(D2D1::Point2F(kTextX, kArtistY), artist_.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
  }
  if (tag_) {
    // Etiqueta menor que o título: desce para dividir a linha de base.
    brush->SetColor(kTag);
    rt->DrawTextLayout(D2D1::Point2F(kWidth - kPadX - tagWidth_, kTitleY + 4.0f), tag_.Get(), brush);
  }
}

void Hud::DrawBars(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, double ms) {
  const std::vector<float>& levels = spectrum_.Levels();
  const std::vector<float>& peaks = spectrum_.Peaks();
  const int n = spectrum_.Bands();
  const float left = kPadX;
  const float right = kWidth - kPadX;
  const float slot = (right - left + kBarGap) / static_cast<float>(n);
  const float barWidth = slot - kBarGap;
  const float span = kBarsBottom - kBarsTop;

  brush->SetColor(kBaseline);
  rt->FillRectangle(D2D1::RectF(left, kBarsBottom + 2.0f, right, kBarsBottom + 3.0f), brush);

  for (int b = 0; b < n; ++b) {
    const float level = levels[static_cast<size_t>(b)];
    const float x = left + slot * static_cast<float>(b);
    const float h = std::max(1.0f, level * span);
    brush->SetColor(WithAlpha(Mix(kAccent, kBarHot, level * level), 0.30f + 0.70f * level));
    rt->FillRectangle(D2D1::RectF(x, kBarsBottom - h, x + barWidth, kBarsBottom), brush);
    const float peak = peaks[static_cast<size_t>(b)];
    if (peak > kPeakMin && peak > level + 0.03f) {
      const float y = kBarsBottom - peak * span;
      brush->SetColor(WithAlpha(kBarHot, 0.55f));
      rt->FillRectangle(D2D1::RectF(x, y - 1.0f, x + barWidth, y), brush);
    }
  }

  // Tempo restante na tela: linha fina que encolhe na borda de baixo.
  const float remaining = static_cast<float>(HudRemaining(timeline_, ms));
  brush->SetColor(WithAlpha(kAccent, 0.35f));
  const float lineLeft = kRadius + 2.0f;
  const float lineRight = lineLeft + (kWidth - 2.0f * lineLeft) * remaining;
  rt->FillRectangle(D2D1::RectF(lineLeft, kHeight - 2.5f, lineRight, kHeight - 1.5f), brush);
}

void Hud::Present(double ms) {
  if (!bitmap_) return;
  const double slide = HudSlide(timeline_, ms) * kSlideDip * dpi_ / 96.0f;
  POINT dst = {anchor_.x, anchor_.y + static_cast<LONG>(std::lround(slide))};
  SIZE size = {surfaceWidth_, surfaceHeight_};
  POINT src = {0, 0};
  BLENDFUNCTION blend = {AC_SRC_OVER, 0, static_cast<BYTE>(std::lround(255.0 * HudAlpha(timeline_, ms))),
                         AC_SRC_ALPHA};
  UpdateLayeredWindow(hwnd_, nullptr, &dst, &size, memDc_, &src, 0, &blend, ULW_ALPHA);
  // Outro app "sempre no topo" pode ter subido por cima desde a última vez.
  if (frame_ == 0) SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

}  // namespace cmh
