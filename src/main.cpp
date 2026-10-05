// Cyber Music HUD — mostra no canto da tela a música que acabou de começar
// no Spotify ou no YouTube, com um visualizador que reage ao áudio real.
//
// Fluxo: MediaWatcher (thread própria, GSMTC) posta kMsgTrack quando uma
// faixa nova começa; aqui decidimos se dá para mostrar (pausado? tela cheia?)
// e chamamos o Hud, que liga a AudioCapture só enquanto está visível.

#include <windows.h>

#include <objbase.h>
#include <shellapi.h>

#include <cmath>
#include <iterator>
#include <string>
#include <vector>

#include "audio.h"
#include "hud.h"
#include "logic.h"
#include "media.h"
#include "resource.h"

namespace cmh {
namespace {

constexpr wchar_t kAppTitle[] = L"Cyber Music HUD";
constexpr wchar_t kHostClass[] = L"CyberMusicHudHost";
constexpr wchar_t kMutexName[] = L"Local\\CyberMusicHUD.SingleInstance";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"CyberMusicHUD";

constexpr UINT kMsgTrack = WM_APP + 1;
constexpr UINT kMsgTray = WM_APP + 2;
constexpr UINT kTrayId = 1;

enum MenuId : UINT {
  kMenuShowNow = 100,
  kMenuPause,
  kMenuAutostart,
  kMenuExit,
};

void ShowError(const wchar_t* message) { MessageBoxW(nullptr, message, kAppTitle, MB_ICONERROR | MB_OK); }

// Vazio se o caminho não couber no buffer: melhor falhar do que gravar no
// registro um caminho cortado.
std::wstring ExePath() {
  wchar_t buf[MAX_PATH * 2];
  const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
  if (n == 0 || n >= std::size(buf)) return {};
  return std::wstring(buf, n);
}

std::wstring QuotedExePath() {
  const std::wstring path = ExePath();
  return path.empty() ? std::wstring() : L"\"" + path + L"\"";
}

bool AutostartEnabled() {
  wchar_t value[MAX_PATH * 2 + 4];
  DWORD bytes = sizeof(value);
  if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, value, &bytes) != ERROR_SUCCESS) {
    return false;
  }
  const std::wstring expected = QuotedExePath();
  return !expected.empty() && lstrcmpiW(value, expected.c_str()) == 0;
}

bool SetAutostart(bool enable) {
  const std::wstring command = QuotedExePath();
  if (enable && command.empty()) return false;
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return false;
  LSTATUS status;
  if (enable) {
    status = RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
  } else {
    status = RegDeleteValueW(key, kRunValue);
    if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
  }
  RegCloseKey(key);
  return status == ERROR_SUCCESS;
}

// Jogo/vídeo em tela cheia ou apresentação: o HUD não aparece por cima.
bool UserIsBusy() {
  QUERY_USER_NOTIFICATION_STATE state = QUNS_ACCEPTS_NOTIFICATIONS;
  if (FAILED(SHQueryUserNotificationState(&state))) return false;
  return state == QUNS_BUSY || state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_PRESENTATION_MODE;
}

// ------------------------------------------------------------ app

class App {
 public:
  ~App();
  // nullptr se tudo subiu; senão a mensagem de erro para o usuário.
  const wchar_t* Init(HINSTANCE instance);
  int Run();

 private:
  static LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
  LRESULT Handle(UINT msg, WPARAM wParam, LPARAM lParam);
  void OnTrack();
  void ShowCurrent();

  NOTIFYICONDATAW TrayIconData() const;
  void AddTrayIcon();
  void RemoveTrayIcon();
  void ShowTrayMenu();

  HINSTANCE hinstance_ = nullptr;
  HWND host_ = nullptr;
  UINT taskbarCreatedMsg_ = 0;
  HICON icon_ = nullptr;
  bool paused_ = false;
  AudioCapture audio_;
  Hud hud_;
  MediaWatcher watcher_;
};

// Também é o caminho de limpeza quando Init falha no meio. A ordem importa:
// o watcher posta para host_, e o HUD usa audio_.
App::~App() {
  watcher_.Stop();
  hud_.Destroy();
  audio_.Stop();
  if (host_) DestroyWindow(host_);  // WM_DESTROY remove o ícone da bandeja
  if (icon_) DestroyIcon(icon_);
  UnregisterClassW(kHostClass, hinstance_);
}

const wchar_t* App::Init(HINSTANCE instance) {
  hinstance_ = instance;

  WNDCLASSEXW wc = {sizeof(wc)};
  wc.lpfnWndProc = HostProc;
  wc.hInstance = instance;
  wc.lpszClassName = kHostClass;
  if (!RegisterClassExW(&wc)) return L"Não foi possível registrar a janela principal.";
  // Janela de topo oculta (não message-only): só janelas de topo recebem o
  // broadcast "TaskbarCreated" para recriar o ícone se o Explorer reiniciar.
  host_ = CreateWindowExW(WS_EX_TOOLWINDOW, kHostClass, kAppTitle, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                          instance, this);
  if (!host_) return L"Não foi possível criar a janela principal.";

  if (!hud_.Create(instance, &audio_)) {
    return L"Não foi possível iniciar o Direct2D/DirectWrite para desenhar o HUD.";
  }

  icon_ = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
  taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");
  AddTrayIcon();

  if (!watcher_.Start(host_, kMsgTrack)) return L"Não foi possível iniciar o monitor de mídia.";
  return nullptr;
}

int App::Run() {
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK App::HostProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (msg == WM_NCCREATE) {
    const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
  }
  auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (app && app->host_ == hwnd) return app->Handle(msg, wParam, lParam);
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT App::Handle(UINT msg, WPARAM wParam, LPARAM lParam) {
  if (msg == taskbarCreatedMsg_ && taskbarCreatedMsg_ != 0) {
    AddTrayIcon();
    return 0;
  }
  switch (msg) {
    case kMsgTrack:
      OnTrack();
      return 0;
    case kMsgTray:
      // NOTIFYICON_VERSION_4: o evento vem em LOWORD(lParam).
      if (LOWORD(lParam) == WM_CONTEXTMENU || LOWORD(lParam) == NIN_SELECT || LOWORD(lParam) == NIN_KEYSELECT) {
        ShowTrayMenu();
      }
      return 0;
    case WM_COMMAND:
      switch (LOWORD(wParam)) {
        case kMenuShowNow:
          ShowCurrent();
          break;
        case kMenuPause:
          paused_ = !paused_;
          if (paused_) hud_.Hide();
          break;
        case kMenuAutostart:
          if (!SetAutostart(!AutostartEnabled())) ShowError(L"Não foi possível alterar o início automático no registro.");
          break;
        case kMenuExit:
          PostQuitMessage(0);
          break;
      }
      return 0;
    case WM_CLOSE:
      PostQuitMessage(0);
      return 0;
    case WM_DESTROY:
      RemoveTrayIcon();
      host_ = nullptr;
      return 0;
  }
  return DefWindowProcW(host_, msg, wParam, lParam);
}

void App::OnTrack() {
  // Retirar sempre, mesmo sem mostrar: a próxima música não pode herdar esta.
  const std::optional<Track> track = watcher_.TakeAnnounced();
  if (!track || paused_ || UserIsBusy()) return;
  hud_.Show(*track);
}

void App::ShowCurrent() {
  if (const std::optional<Track> track = watcher_.Current()) {
    hud_.Show(*track);
    return;
  }
  // Nada tocando: o HUD aparece mesmo assim, para conferir posição e visual;
  // as barras mostram o que estiver saindo no alto-falante.
  Track idle;
  idle.title = L"Nenhuma música tocando";
  idle.artist = L"Spotify / YouTube em pausa";
  idle.platformLabel = L"STANDBY";
  hud_.Show(idle);
}

NOTIFYICONDATAW App::TrayIconData() const {
  NOTIFYICONDATAW nid = {sizeof(nid)};
  nid.hWnd = host_;
  nid.uID = kTrayId;
  return nid;
}

void App::AddTrayIcon() {
  NOTIFYICONDATAW nid = TrayIconData();
  nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
  nid.uCallbackMessage = kMsgTray;
  nid.hIcon = icon_ ? icon_ : LoadIconW(nullptr, IDI_APPLICATION);
  lstrcpynW(nid.szTip, kAppTitle, ARRAYSIZE(nid.szTip));
  Shell_NotifyIconW(NIM_ADD, &nid);
  nid.uVersion = NOTIFYICON_VERSION_4;
  Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void App::RemoveTrayIcon() {
  NOTIFYICONDATAW nid = TrayIconData();
  Shell_NotifyIconW(NIM_DELETE, &nid);
}

void App::ShowTrayMenu() {
  HMENU menu = CreatePopupMenu();
  if (!menu) return;
  AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Cyber Music HUD");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kMenuShowNow, L"Mostrar agora");
  AppendMenuW(menu, MF_STRING | (paused_ ? MF_CHECKED : 0), kMenuPause, L"Pausar");
  AppendMenuW(menu, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), kMenuAutostart, L"Iniciar com o Windows");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kMenuExit, L"Sair");

  POINT cursor = {};
  GetCursorPos(&cursor);
  SetForegroundWindow(host_);  // sem isso o menu não fecha ao clicar fora
  TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, cursor.x, cursor.y, 0, host_, nullptr);
  PostMessageW(host_, WM_NULL, 0, 0);
  DestroyMenu(menu);
}

// ------------------------------------------------------------ linha de comando

bool HasArg(const wchar_t* arg) {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  bool found = false;
  for (int i = 1; argv && i < argc; ++i) {
    if (lstrcmpiW(argv[i], arg) == 0) found = true;
  }
  LocalFree(argv);
  return found;
}

// Saída UTF-8 para o console de quem chamou (ou para o arquivo/pipe, se
// redirecionado). O exe é GUI, então o console precisa ser anexado.
class Output {
 public:
  Output() {
    out_ = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out_ || out_ == INVALID_HANDLE_VALUE) {
      if (AttachConsole(ATTACH_PARENT_PROCESS)) out_ = GetStdHandle(STD_OUTPUT_HANDLE);
    }
  }
  void Line(const std::wstring& text) {
    if (!out_ || out_ == INVALID_HANDLE_VALUE) return;
    // Metadados de terceiros vão para o terminal: sem caracteres de controle,
    // um título malicioso não injeta sequências de escape (VT/OSC).
    std::wstring line = text;
    for (wchar_t& c : line) {
      if (c < 0x20 || (c >= 0x7F && c <= 0x9F)) c = L'?';
    }
    line += L"\r\n";
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0,
                                          nullptr, nullptr);
    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), utf8.data(), bytes, nullptr,
                        nullptr);
    DWORD written = 0;
    WriteFile(out_, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
  }

 private:
  HANDLE out_ = nullptr;
};

const wchar_t* ModeName(CaptureMode mode) {
  switch (mode) {
    case CaptureMode::Process: return L"loopback do processo";
    case CaptureMode::System: return L"loopback do sistema (fallback)";
    case CaptureMode::None: break;
  }
  return L"nenhuma";
}

// Diagnóstico sem UI: sessões de mídia, faixa detectada e 2 s de captura com
// o espectro em texto. Prova que metadados e áudio são reais.
int Probe() {
  Output out;
  out.Line(L"== Cyber Music HUD :: probe ==");
  out.Line(L"Sessões GSMTC (* = atual):");
  for (const std::wstring& line : MediaWatcher::DescribeSessions()) out.Line(line);

  MediaWatcher watcher;
  watcher.Start(nullptr, 0);
  Sleep(1200);  // duas consultas: tempo para o detector estabilizar
  const std::optional<Track> track = watcher.Current();
  watcher.Stop();
  if (track) {
    out.Line(L"Tocando: \"" + track->title + L"\" — \"" + track->artist + L"\" [" + track->platformLabel +
             L"] pid " + std::to_wstring(track->audioPid));
  } else {
    out.Line(L"Tocando: nada (Spotify/navegador parado ou sem sessão)");
  }

  AudioCapture audio;
  audio.Start(track ? track->audioPid : 0);
  SpectrumAnalyzer spectrum(36, 48000.0f);
  std::vector<float> samples(SpectrumAnalyzer::kFftSize);
  size_t total = 0;
  float peak = 0.0f;
  for (int i = 0; i < 125; ++i) {  // ~2 s de leituras
    Sleep(16);
    const size_t arrived = audio.Samples().TakeArrived();
    total += arrived;
    if (arrived == 0) continue;
    spectrum.SetSampleRate(audio.SampleRate());
    audio.Samples().Latest(samples.data(), samples.size());
    for (float s : samples) peak = std::max(peak, std::abs(s));
    spectrum.Process(samples.data(), 0.016f);
  }
  out.Line(std::wstring(L"Captura: ") + ModeName(audio.Mode()) + L", " +
           std::to_wstring(static_cast<int>(audio.SampleRate())) + L" Hz, " + std::to_wstring(total) +
           L" amostras em 2 s, pico " + std::to_wstring(peak));
  audio.Stop();

  static const wchar_t kBlocks[] = L" ▁▂▃▄▅▆▇█";
  std::wstring bars;
  for (float level : spectrum.Levels()) bars += kBlocks[std::clamp(static_cast<int>(level * 8.0f + 0.5f), 0, 8)];
  out.Line(L"Espectro: |" + bars + L"|");
  return 0;
}

// Mutex de instância única, liberado na saída de wWinMain por qualquer caminho.
class SingleInstance {
 public:
  SingleInstance() {
    mutex_ = CreateMutexW(nullptr, TRUE, kMutexName);
    alreadyRunning_ = !mutex_ || GetLastError() == ERROR_ALREADY_EXISTS;
  }
  ~SingleInstance() {
    if (!mutex_) return;
    if (!alreadyRunning_) ReleaseMutex(mutex_);
    CloseHandle(mutex_);
  }
  SingleInstance(const SingleInstance&) = delete;
  SingleInstance& operator=(const SingleInstance&) = delete;
  bool AlreadyRunning() const { return alreadyRunning_; }

 private:
  HANDLE mutex_ = nullptr;
  bool alreadyRunning_ = false;
};

class ComScope {
 public:
  explicit ComScope(DWORD model = COINIT_APARTMENTTHREADED) : hr_(CoInitializeEx(nullptr, model)) {}
  ~ComScope() {
    if (SUCCEEDED(hr_)) CoUninitialize();
  }
  ComScope(const ComScope&) = delete;
  ComScope& operator=(const ComScope&) = delete;

 private:
  HRESULT hr_;
};

}  // namespace
}  // namespace cmh

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  using namespace cmh;

  // DLLs carregadas depois daqui só vêm do System32, nunca da pasta do app
  // (gravável pelo usuário). As importadas estaticamente já são cobertas por
  // /DEPENDENTLOADFLAG no build.bat. DPI por monitor vem de src/app.manifest.
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);

  if (HasArg(L"--enable-autostart")) return SetAutostart(true) ? 0 : 1;
  if (HasArg(L"--disable-autostart")) return SetAutostart(false) ? 0 : 1;
  if (HasArg(L"--exit")) {
    HWND running = FindWindowW(kHostClass, nullptr);
    if (running) PostMessageW(running, WM_CLOSE, 0, 0);
    return 0;
  }
  if (HasArg(L"--show")) {  // = "Mostrar agora" da bandeja, para scripts
    HWND running = FindWindowW(kHostClass, nullptr);
    if (running) PostMessageW(running, WM_COMMAND, kMenuShowNow, 0);
    return running ? 0 : 1;
  }
  if (HasArg(L"--probe")) {
    // MTA: o probe bloqueia em .get() de chamadas WinRT, proibido em STA.
    ComScope com(COINIT_MULTITHREADED);
    return Probe();
  }

  SingleInstance single;
  if (single.AlreadyRunning()) return 0;
  ComScope com;

  App app;
  if (const wchar_t* error = app.Init(instance)) {
    ShowError(error);
    return 1;
  }
  return app.Run();
}
