// O HUD em si: painel pequeno no canto inferior direito com a faixa e o
// visualizador de espectro.
//
// Janela em camadas (WS_EX_LAYERED + UpdateLayeredWindow) com alfa por
// pixel, desenhada com Direct2D/DirectWrite num bitmap em memória. Nunca
// recebe foco (WS_EX_NOACTIVATE) nem clique (WS_EX_TRANSPARENT). Enquanto
// visível, um timer de ~30 Hz anima o fade e as barras; escondido, não há
// timer nem captura de áudio — custo zero.
#pragma once

#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <chrono>
#include <string>
#include <vector>

#include "audio.h"
#include "logic.h"
#include "media.h"

namespace cmh {

class Hud {
 public:
  Hud() = default;
  ~Hud() { Destroy(); }
  Hud(const Hud&) = delete;
  Hud& operator=(const Hud&) = delete;

  bool Create(HINSTANCE instance, AudioCapture* audio);
  void Destroy();

  // Mostra (ou reinicia, se já visível) com a faixa dada.
  void Show(const Track& track);
  void Hide();
  bool Visible() const { return visible_; }

 private:
  using Clock = std::chrono::steady_clock;

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
  bool CreateDeviceIndependent();
  bool EnsureRenderTarget();
  bool EnsureSurface(int width, int height);
  Microsoft::WRL::ComPtr<IDWriteTextLayout> MakeLayout(const std::wstring& text, IDWriteTextFormat* format,
                                                      float maxWidth, float tracking);
  void PlaceOnPrimaryMonitor();
  void Tick();
  void UpdateText(double ms);
  void Render(double ms);
  void DrawFrame(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush);
  void DrawHeader(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush);
  void DrawBars(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, double ms);
  void Present(double ms);

  HINSTANCE instance_ = nullptr;
  HWND hwnd_ = nullptr;
  AudioCapture* audio_ = nullptr;
  bool visible_ = false;

  Microsoft::WRL::ComPtr<ID2D1Factory> d2d_;
  Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> titleFormat_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> artistFormat_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> tagFormat_;
  Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> target_;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
  Microsoft::WRL::ComPtr<ID2D1PathGeometry> playIcon_;

  HDC memDc_ = nullptr;
  HBITMAP bitmap_ = nullptr;
  HGDIOBJ oldBitmap_ = nullptr;
  int surfaceWidth_ = 0;
  int surfaceHeight_ = 0;

  Track track_;
  std::wstring shownTitle_;   // texto atualmente nos layouts (muda durante o "decode")
  std::wstring shownArtist_;
  Microsoft::WRL::ComPtr<IDWriteTextLayout> title_;
  Microsoft::WRL::ComPtr<IDWriteTextLayout> artist_;
  Microsoft::WRL::ComPtr<IDWriteTextLayout> tag_;
  float tagWidth_ = 0;

  Timeline timeline_;
  SpectrumAnalyzer spectrum_{36, 48000.0f};
  std::vector<float> samples_;
  Clock::time_point shownAt_;
  Clock::time_point lastTick_;
  uint32_t frame_ = 0;
  float sampleRate_ = 0.0f;  // taxa já aplicada ao spectrum_

  float dpi_ = 96.0f;
  POINT anchor_ = {};  // canto superior esquerdo na posição final, pixels
};

}  // namespace cmh
