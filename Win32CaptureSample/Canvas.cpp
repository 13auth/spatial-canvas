#include "pch.h"
#include "Canvas.h"
#include <windowsx.h>
#include <d3dcompiler.h>
#include <d2d1.h>
#include <dwrite.h>
#include <fstream>
#include <string>
#include <cmath>
#include <cwctype>
#include <algorithm>
#include <shellapi.h>
#include <wincodec.h>
#include <commdlg.h>
#include <commoncontrols.h> // M33: IImageList (keskin dock ikonu)
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "comctl32.lib")
#include <exception>
#include <thread>
#include <unordered_map>
#include <mutex>
#include <wininet.h>             // M48: yeni-sürüm bildirimi (HTTP GET)
#pragma comment(lib, "wininet.lib")
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace winrt
{
    using namespace Windows::Foundation;
    using namespace Windows::Graphics;
    using namespace Windows::Graphics::Capture;
    using namespace Windows::Graphics::DirectX;
    using namespace Windows::Graphics::DirectX::Direct3D11;
}

// ---- Shader (VS + PS tek kaynak) ----
static const char g_shaderSrc[] = R"(
cbuffer Cb : register(b0) { float4 rect; float4 extra; }; // M28: extra.x = opacity
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID)
{
    float2 c = float2(id & 1, id >> 1);
    VSOut o;
    o.pos = float4(rect.x + c.x * rect.z, rect.y - c.y * rect.w, 0, 1);
    o.uv = c;
    return o;
}
Texture2D tex : register(t0);
SamplerState samp : register(s0);
// M28: extra.x=opacity · M34: extra.y=blur yarıçapı(texel), extra.zw=texel boyutu
float4 PSMain(VSOut i) : SV_Target
{
    float r = extra.y;
    if (r < 0.5) return float4(tex.Sample(samp, i.uv).rgb, extra.x);
    float3 c = 0; float n = 0;
    [loop] for (float y = -r; y <= r; y += 1.0)
        [loop] for (float x = -r; x <= r; x += 1.0)
        { c += tex.Sample(samp, i.uv + float2(x * extra.z, y * extra.w)).rgb; n += 1.0; }
    return float4(c / n, extra.x);
}
)";

// ---- Veri yapıları ----
struct Camera
{
    float x = 0.0f;     // dünya koordinatı (ekranın sol-üstü)
    float y = 0.0f;
    float zoom = 0.25f; // ekran piksel / dünya piksel
};

struct Tile
{
    HWND source{};
    winrt::GraphicsCaptureItem item{ nullptr };
    winrt::Direct3D11CaptureFramePool pool{ nullptr };
    winrt::GraphicsCaptureSession session{ nullptr };
    winrt::com_ptr<ID3D11Texture2D> tex;
    winrt::com_ptr<ID3D11ShaderResourceView> srv;
    winrt::SizeInt32 lastSize{};
    std::wstring exe;       // layout kalıcılığı anahtarı
    std::wstring title;     // overlay etiketi (~1sn'de bir tazelenir)
    ULONGLONG titleTick = 0;
    float wx = 0, wy = 0;   // dünya pozisyonu
    std::wstring exePath;   // M11: çoğaltma için tam exe yolu
    HICON icon = nullptr;   // M13: dock ikonu (paylaşılan handle - DestroyIcon ETME)
    winrt::com_ptr<ID2D1Bitmap> icoBmp; // D2D RT'ye bağlı; recreate'te sıfırlanır
    float ww = 0, wh = 0;   // dünya boyutu (1:1 piksel)
    RECT origRect{};        // park öncesi orijinal konum (çıkışta geri yüklenir)
    int frameDX = 0, frameDY = 0; // GetWindowRect ile görünür çerçeve farkı
    bool parked = false;
    bool everParked = false; // origRect yalnızca ilk parkta kaydedilir
    bool wasMax = false;     // park öncesi maximize miydi (restore'da geri uygulanır)
    bool alive = true;
    float opacity = 1.0f;    // M28: rules.txt opacity kuralı (0..1)
    float blur = 0.0f;       // M34: rules.txt blur kuralı (texel yarıçapı)
    ULONGLONG activeSeq = 0; // M21: son aktiflik sırası (Tab MRU döngüsü)
    bool pinnedFlag = false; // M22: ekrana sabit (pan/zoom'u yok sayar)
    float px = 0, py = 0, pw = 0, ph = 0; // M22: ekran-uzayı rect (client)
};

// ---- M5: Ayarlar ----
struct Key { int vk = 0; int mods = 0; }; // mods: 1=Ctrl 2=Alt 4=Shift

// M48: uygulama sürümü (app.rc VERSIONINFO ile SENKRON tut - RELEASE.md sürüm listesinde)
constexpr const wchar_t* APP_VERSION = L"0.56.0";

struct Settings
{
    int lang = 0;           // M47: 0 English (varsayilan), 1 Turkce
    bool restoreView = true; // M50: açılışta son kamera görünümünü geri yükle
    bool updateCheck = true; // M48: açılışta yeni-sürüm kontrolü (sadece bildirim)
    // M48: sürüm feed'i (raw VERSION dosyası, içerik "0.47.0"). HTTP/HTTPS (WinINet).
    std::wstring updateUrl = L"https://raw.githubusercontent.com/13auth/spatial-canvas/main/VERSION";
    std::wstring lastRun; // M53: son çalıştırılan sürüm (güncelleme-sonrası bildirim)
    int fpsCap = 30;        // 15 / 30 / 60
    int animSpeed = 1;      // 0 yavaş, 1 normal, 2 hızlı
    bool labels = true;     // başlık etiketleri
    bool hover = true;      // vurgu çerçevesi
    float diveZoom = 0.92f; // swap-in eşiği
    int maxTiles = 12;      // 6 / 9 / 12 / 16
    int bgPreset = 0;       // 0 koyu, 1 gece, 2 siyah
    bool grid = true;       // M12: dünyaya çakılı nokta ızgara
    bool minimap = true;    // M36: sağ-alt kuşbakışı minimap
    bool autostart = false; // Windows ile başlat (kaynak: registry)
    int canvasSpan = 0;     // 0 ana ekran, 1 tüm sanal ekran
    // M8/M10: özelleştirilebilir kısayollar (klavye VEYA fare tuşu - vk birleşik)
    int wheelMod = 0;       // 0 Ctrl+Alt, 1 Ctrl+Shift, 2 Alt+Shift, 3 Ctrl, 4 Alt
    Key kbPull{ VK_XBUTTON2, 0 }; // global geri çekil (varsayılan fare İLERİ)
    Key kbPanel{ 'S', 0 };
    Key kbFit{ 'F', 0 };
    Key kbExit{ VK_ESCAPE, 0 };
    Key kbSearch{ 'F', 1 }; // M9: pencere ara (varsayılan Ctrl+F)
    Key kbLaunch{ 'N', 1 }; // M23: uygulama başlatıcı (varsayılan Ctrl+N)
};

// ---- Global durum (M1 pragmatizmi) ----
namespace
{
    HWND g_hwnd{};
    int g_sw = 0, g_sh = 0;             // tuval penceresi boyutu
    int g_vx = 0, g_vy = 0;             // tuval penceresi ekran orijini (span'da negatif olabilir)
    int g_priW = 0, g_priH = 0;         // ana monitör boyutu (park şeridi referansı)
    winrt::com_ptr<ID3D11Device> g_device;
    winrt::com_ptr<ID3D11DeviceContext> g_ctx;
    winrt::com_ptr<IDXGISwapChain1> g_swap;
    winrt::com_ptr<ID3D11RenderTargetView> g_rtv;
    winrt::com_ptr<ID3D11VertexShader> g_vs;
    winrt::com_ptr<ID3D11PixelShader> g_ps;
    winrt::com_ptr<ID3D11Buffer> g_cb;
    winrt::com_ptr<ID3D11SamplerState> g_sampler;
    winrt::com_ptr<ID3D11RasterizerState> g_raster;
    winrt::com_ptr<ID3D11BlendState> g_blend; // M28: opacity alpha blend
    winrt::IDirect3DDevice g_winrtDevice{ nullptr };
    // M4: D2D/DWrite overlay (başlık etiketleri + hover vurgusu)
    winrt::com_ptr<ID2D1Factory> g_d2dFactory;
    winrt::com_ptr<ID2D1RenderTarget> g_d2dRT;
    winrt::com_ptr<ID2D1SolidColorBrush> g_brText, g_brBg, g_brHover;
    // M19: kare-başı yaratılan fırçalar artık cache'li (InitD2D'de, RT'ye bağlı)
    winrt::com_ptr<ID2D1SolidColorBrush> g_brPanelBg, g_brSel, g_brPick;
    winrt::com_ptr<ID2D1SolidColorBrush> g_brNote; // M44: not dolgusu (renk not başına ayarlanır)
    winrt::com_ptr<ID2D1Bitmap> g_gridBmp;       // M19: tek noktalı 64x64 doku
    winrt::com_ptr<ID2D1BitmapBrush> g_gridBrush; // wrap-mode: ızgara tek çağrı
    winrt::com_ptr<ID2D1RadialGradientBrush> g_bgRadial; // M29: vinyet zemin
    winrt::com_ptr<IDWriteFactory> g_dwFactory;
    winrt::com_ptr<IDWriteTextFormat> g_textFmt;
    winrt::com_ptr<IDWriteTextFormat> g_textFmtL;  // sola hizalı (panel etiketleri)
    winrt::com_ptr<IDWriteTextFormat> g_textFmtN;  // M44: not metni (sol-üst hizalı, satır kaydırmalı)
    // M5: ayarlar paneli
    Settings g_set;
    bool g_panelOpen = false;
    float g_panelA = 0.0f;          // 0 kapalı, 1 açık (animasyonlu)
    constexpr float PANEL_W = 320.0f;
    struct PanelRow { D2D1_RECT_F rect; int id; };
    std::vector<PanelRow> g_panelRows;
    int g_captureRow = -1;          // M8: kısayol yakalama modu (-1 = kapalı)
    int g_panelTab = 0;             // M10: 0 Ayarlar, 1 Kısayollar
    // M9: pencere arama
    bool g_searchOpen = false;
    std::wstring g_searchText;
    std::vector<int> g_matches;
    std::vector<int> g_noteMatches; // M49: eşleşen not indeksleri (tile'lardan sonra gezilir)
    std::vector<int> g_zoneMatches; // M67: eşleşen bölge indeksleri (notlardan sonra gezilir)
    int g_searchSel = 0;            // birleşik indeks: [0..g_matches) tile, [g_matches..) not
    D2D1_RECT_F g_searchBtnRect{};  // M11: üst-orta Ara butonu
    // M11: çoklu seçim + çoğaltma
    std::unordered_set<HWND> g_selSet;
    bool g_marquee = false;
    float g_marqAX = 0, g_marqAY = 0, g_marqBX = 0, g_marqBY = 0; // dünya
    struct GrpItem { HWND h; float x0, y0; };
    std::vector<GrpItem> g_grp;     // grup taşıma anlık görüntüsü
    std::vector<GrpItem> g_zoneTiles; // M55: zon sürüklenirken içindeki tile'lar (birlikte taşınır)
    float g_zoneDragX0 = 0, g_zoneDragY0 = 0; // M55: zonun sürükleme başı konumu
    bool g_groupDrag = false;
    float g_grpX0 = 0, g_grpY0 = 0; // grup taşıma başlangıç dünya noktası
    struct CopyItem { std::wstring path; std::wstring exe; };
    std::vector<CopyItem> g_copyItems;
    struct PasteSlot { std::wstring exe; float wx, wy; ULONGLONG tick = 0; };
    std::vector<PasteSlot> g_pastePending;
    // M13: tuval yer imleri (Ctrl+Shift+1..4 kaydet, Ctrl+1..4 zıpla)
    struct Anchor { float x, y, zoom; bool set = false; };
    Anchor g_anchors[4]{};
    // M13: dock (alt-orta, imleç alt kenara inince açılır)
    float g_dockA = 0.0f;           // 0 gizli, 1 açık (animasyonlu)
    struct DockChip { D2D1_RECT_F rect; int tile; };
    std::vector<DockChip> g_dockChips;
    winrt::com_ptr<IWICImagingFactory> g_wic; // HICON -> D2D bitmap
    // M6: tile serbest bırakma çipi + dışlananlar
    D2D1_RECT_F g_releaseRect{};
    int g_releaseTile = -1;
    std::unordered_set<HWND> g_excluded;
    // M15: pencere kuralları (rules.txt) - küçük harfli exe adları
    std::unordered_set<std::wstring> g_ruleExclude;
    std::unordered_map<std::wstring, float> g_ruleOpacity; // M28: exe -> saydamlık
    std::unordered_map<std::wstring, float> g_ruleBlur;    // M34: exe -> blur yarıçapı
    // M23: uygulama başlatıcı (Ctrl+N paleti + launcher.txt kısayolları)
    bool g_launchOpen = false;
    std::wstring g_launchText;
    int g_launchSel = 0;
    struct Launcher
    {
        std::wstring label, exe, args;
        HICON icon = nullptr;               // M24: exe'den çıkarılan ikon
        winrt::com_ptr<ID2D1Bitmap> icoBmp; // D2D RT'ye bağlı; recreate'te sıfırlanır
    };
    std::vector<Launcher> g_launchers;
    std::vector<D2D1_RECT_F> g_launchRows; // Ctrl+N paleti satır geometrisi
    D2D1_RECT_F g_launchBox{};              // palet kutusu (dış tık = kapat)
    // M24: sağ kenar uygulama dock'u (launcher.txt'yi ikon olarak gösterir)
    float g_appDockA = 0.0f;                // 0 gizli, 1 açık (animasyonlu)
    struct AppDockChip { D2D1_RECT_F rect; int idx; };
    std::vector<AppDockChip> g_appDockChips;
    // M36: minimap (tıklama için iç dünya dönüşümü)
    D2D1_RECT_F g_minimapRect{};
    float g_mmX = 0, g_mmY = 0, g_mmMinX = 0, g_mmMinY = 0, g_mmScale = 0;
    // M7.1: UI (dişli+panel) her modda ANA monitöre sabitlenir
    float g_uiX = 0, g_uiY = 0; // ana monitörün client-uzayı orijini (-g_vx, -g_vy)
    std::vector<Tile> g_tiles;
    Camera g_cam;                   // gerçek (render edilen) kamera
    Camera g_camT;                  // hedef kamera (animasyon hedefi)
    ULONGLONG g_lastTick = 0;
    ULONGLONG g_lastAdopt = 0;
    std::vector<std::pair<std::wstring, POINT>> g_savedLayout;
    // M27: pinned tile kalıcılığı (restart'ta ekrana-sabit konumu geri yükle)
    struct SavedPin { std::wstring exe; float px, py, pw, ph; };
    std::vector<SavedPin> g_savedPins;
    // M44: tuval notları (yapışkan not - mekansal anotasyon, dünya-uzayında)
    // M46: w/h not başına (yeniden boyutlandırılabilir; uzun metin taşmasın)
    struct Note { float wx = 0, wy = 0, w = 240.0f, h = 150.0f; int color = 0; std::wstring text; };
    std::vector<Note> g_notes;
    int g_lastNoteColor = 0, g_lastZoneColor = 0; // M64: son kullanılan renk (yeni nesne miras alır)
    int g_editNote = -1;    // düzenlenen not indeksi (-1 = yok)
    int g_dragNote = -1;    // sürüklenen not indeksi
    int g_resizeNote = -1;  // M46: yeniden boyutlandırılan not indeksi
    int g_hoverNote = -1;   // hover'daki not (her kare DrawNotes günceller)
    int g_noteDelIdx = -1;  // hover ✕ butonunun ait olduğu not
    int g_noteResIdx = -1;  // M46: hover boyut tutamağının ait olduğu not
    D2D1_RECT_F g_noteDelRect{}; // hover ✕ ekran-uzayı hit-test
    D2D1_RECT_F g_noteResRect{}; // M46: boyut tutamağı ekran-uzayı hit-test
    float g_noteGrabDX = 0, g_noteGrabDY = 0;
    constexpr float NOTE_W = 240.0f, NOTE_H = 150.0f; // varsayılan not boyutu (dünya-birimi)
    constexpr float NOTE_MIN_W = 110.0f, NOTE_MIN_H = 70.0f, NOTE_MAX = 1600.0f; // M46 sınır
    // M54: bölge/zon çerçeveleri (etiketli renkli bölge; gövde tıklama-geçirgen, sadece
    // başlık-çubuğu sürükler → altındaki tile'lar engellenmez). FigJam "frame" tarzı.
    struct Zone { float wx = 0, wy = 0, w = 640.0f, h = 440.0f; int color = 0; std::wstring title; };
    std::vector<Zone> g_zones;
    int g_editZone = -1, g_dragZone = -1, g_resizeZone = -1, g_hoverZone = -1;
    int g_zoneDelIdx = -1, g_zoneResIdx = -1, g_zoneArrIdx = -1; // M56: ⊞ düzenle butonu
    D2D1_RECT_F g_zoneDelRect{}, g_zoneResRect{}, g_zoneArrRect{};
    float g_zoneGrabDX = 0, g_zoneGrabDY = 0;
    constexpr float ZONE_W = 640.0f, ZONE_H = 440.0f, ZONE_MIN_W = 200.0f, ZONE_MIN_H = 140.0f;
    constexpr float ZONE_BAR = 34.0f; // başlık çubuğu yüksekliği (dünya-birimi)
    // M57: bağlayıcı oklar (tile'lar arası ilişki çizgisi; Ctrl+sürükle ile kur).
    // Oturum-içi (HWND kimliği restart'ta değişir → kalıcı değil, bilinçli).
    struct Connector { HWND a = nullptr, b = nullptr; };
    std::vector<Connector> g_connectors;
    bool g_connecting = false;        // Ctrl+sürükle ile bağlantı kuruluyor
    HWND g_connectFrom = nullptr;     // bağlantının başladığı tile
    int g_connDelIdx = -1;            // hover ✕'in ait olduğu bağlantı
    D2D1_RECT_F g_connDelRect{};
    bool g_panning = false;
    POINT g_curClient{};            // M19: kare-başı tek imleç okuması (client)
    bool g_firstRun = false;        // M20: ilk açılış ipucu kartı
    bool g_helpOpen = false;        // M20: F1 kısayol listesi
    HWND g_focusWnd = nullptr;      // M21: klavye odağındaki tile (HWND - indeks değil)
    ULONGLONG g_activeCounter = 0;  // M21: MRU sırası sayacı
    bool g_pinDrag = false;         // M22: pinned tile ekran-uzayında sürükleniyor
    int g_pinDragTile = -1;
    POINT g_pinGrab{};              // M22: pin sürükleme tutamağı (ekran ofseti)
    // M12: momentum (flick) pan
    float g_panVX = 0, g_panVY = 0; // kamera hızı, dünya birimi/sn
    bool g_momentum = false;
    ULONGLONG g_panTick = 0;        // son pan hareketinin zamanı
    int g_dragTile = -1;
    POINT g_lastMouse{};
    float g_grabDX = 0, g_grabDY = 0;
    int g_activeTile = -1;          // swap-in yapılmış tile (yoksa -1)
    bool g_swapArmed = true;        // histerezis: swap sonrası zoom<0.75'te yeniden kurulur
    float g_preSwapZoom = 0.25f;    // swap öncesi kamera (geri dönüşte aynı manzara)
    float g_preSwapX = 0, g_preSwapY = 0;
    HHOOK g_mouseHook = nullptr;
    constexpr float SWAP_IN_ZOOM = 0.92f;
    constexpr float SWAP_OUT_ZOOM = 0.85f;
    constexpr UINT MSG_GLOBAL_WHEEL = WM_APP + 1;
    constexpr UINT MSG_PULLBACK = WM_APP + 2;  // fare ileri tuşu: geri çekil
    constexpr UINT MSG_IPC = WM_APP + 3;       // M30: named pipe komutu geldi
    std::vector<std::wstring> g_ipcQueue;      // M30: pipe thread → ana thread
    std::mutex g_ipcMutex;
    constexpr UINT MSG_UPDATE = WM_APP + 4;    // M48: yeni sürüm bulundu (thread → ana)
    std::wstring g_updateVer;                  // M48: feed'deki yeni sürüm (boş=yok)
    std::mutex g_updateMutex;
    bool g_updateAvail = false;                // M48: kalıcı HUD ipucu için
    D2D1_RECT_F g_updateRect{};                // M51: pill hit-test (tıkla=release aç)
    bool g_pngRequest = false;                 // M52: tuvali PNG'ye aktar isteği (Render'da işlenir)
    // M50: oturum görünüm restore (settings'ten yüklenen son kamera)
    float g_loadCamX = 0, g_loadCamY = 0, g_loadCamZ = 0;
    bool g_hasSavedCam = false;
    constexpr int HOTKEY_TOGGLE = 1;
    UINT g_msgTaskbarCreated = 0;   // M16: explorer yeniden başlama yayını
    bool g_geomDirty = false;       // M18: çözünürlük/DPI değişti, yeniden kur
    bool g_deviceLost = false;      // M18: D3D cihazı düştü (TDR/sürücü/uyku)
    // M17: geçici bildirim (toast) - eylem geri bildirimi
    std::wstring g_toast;
    ULONGLONG g_toastTick = 0;
}

// M47: i18n - g_set.lang'a gore EN/TR string sec (0=English varsayilan, 1=Turkce).
// Kullanim: TL(L"English text", L"Turkce metin"). Birlestirmede ilk parca string'e
// donusur: TL(a,b)+std::to_wstring(x)+TL(c,d) calisir (std operator+(const wchar_t*, wstring)).
static inline const wchar_t* TL(const wchar_t* en, const wchar_t* tr)
{
    return g_set.lang == 1 ? tr : en;
}

// M17: toast göster - 1.6sn görünür, son 400ms'de söner (DrawOverlay çizer)
static void ShowToast(const std::wstring& s)
{
    g_toast = s;
    g_toastTick = GetTickCount64();
}

// İleri bildirimler
static void ParkWindow(Tile& t, int idx);
static void FitCamera(bool ignoreSel = false); // M17: seçim varsa onu sığdırır
static void ArrangeGrid(); // M35/M40: ızgaraya diz (komut paletinden de çağrılır)
static void ArrangeZone(int zi); // M56: bir bölgenin içindeki pencereleri diz
static D2D1_COLOR_F NoteColor(int c); // M44: not/zon renk paleti
static void SaveNamedLayout(const std::wstring& name); // M42: workspace
static void LoadNamedLayout(const std::wstring& name);
static std::wstring ExeNameOf(HWND hwnd); // M18: kurtarmada hwnd doğrulaması
static void SavePendingRestore();
static void SaveLayout();
static void ForceForeground(HWND hwnd);
static int HitTile(float wx, float wy);
static void SaveSettings();
static void ApplyFpsCap();
static void RestoreOriginal(Tile& t);
static bool QueryAutostart();
static void ApplyAutostart();
static void InitD2D();
static void ReRegisterPullHotkey();
static void UpdateMatches();
static void RaiseCanvasTopmost();
static void LowerCanvas();

// M10: girdi yardımcıları (klavye + fare birleşik)
static bool IsMouseVk(int vk)
{
    return vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
}

static int CurMods()
{
    return ((GetKeyState(VK_CONTROL) & 0x8000) ? 1 : 0)
         | ((GetKeyState(VK_MENU)    & 0x8000) ? 2 : 0)
         | ((GetKeyState(VK_SHIFT)   & 0x8000) ? 4 : 0);
}

// M11: Windows görev çubuğunu gizle/göster (tuval aktifken gizli)
static void ShowTaskbars(bool show)
{
    int cmd = show ? SW_SHOW : SW_HIDE;
    HWND t = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (t) ShowWindow(t, cmd);
    HWND s = nullptr;
    while ((s = FindWindowExW(nullptr, s, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr)
        ShowWindow(s, cmd);
}

// ---- Çökme sigortası: park durumu diske yazılır ----
static std::wstring PendingFilePath()
{
    wchar_t* appdata = nullptr;
    size_t len = 0;
    _wdupenv_s(&appdata, &len, L"APPDATA");
    std::wstring path = appdata ? appdata : L"C:";
    free(appdata);
    path += L"\\SpatialCanvas";
    CreateDirectoryW(path.c_str(), nullptr);
    return path + L"\\pending_restore.txt";
}

static void SavePendingRestore()
{
    std::wofstream f(PendingFilePath(), std::ios::trunc);
    if (!f) return;
    for (auto& t : g_tiles)
    {
        if (!t.everParked || !IsWindow(t.source)) continue;
        wchar_t title[256]{};
        GetWindowTextW(t.source, title, 256);
        // M18: hwnd|exe|max|x|y|title (title SONDA - '|' içerebilir).
        // hwnd kendi çökmemizden sağ çıkar; exe handle geri dönüşüm kontrolü.
        f << (ULONG_PTR)t.source << L"|" << t.exe << L"|" << (t.wasMax ? 1 : 0)
          << L"|" << t.origRect.left << L"|" << t.origRect.top
          << L"|" << title << L"\n";
    }
}

static void RecoverFromCrash()
{
    std::wifstream f(PendingFilePath());
    if (!f) return;
    std::wstring line;
    std::vector<std::wstring> keep; // penceresi henüz açılmamış satırlar kalır
    while (std::getline(f, line))
    {
        // M18 formatı: hwnd|exe|max|x|y|title (5 ayraç, soldan). Eski format
        // (title|x|y) ayraç sayısından anlaşılır - tek geçişlik göç.
        std::vector<size_t> sep;
        for (size_t i = 0; i < line.size() && sep.size() < 5; i++)
            if (line[i] == L'|') sep.push_back(i);
        HWND w = nullptr;
        bool wasMax = false; int x = 0, y = 0;
        std::wstring title;
        if (sep.size() == 5)
        {
            ULONG_PTR hv = (ULONG_PTR)_wtoi64(line.substr(0, sep[0]).c_str());
            std::wstring exe = line.substr(sep[0] + 1, sep[1] - sep[0] - 1);
            wasMax = _wtoi(line.substr(sep[1] + 1, sep[2] - sep[1] - 1).c_str()) != 0;
            x = _wtoi(line.substr(sep[2] + 1, sep[3] - sep[2] - 1).c_str());
            y = _wtoi(line.substr(sep[3] + 1, sep[4] - sep[3] - 1).c_str());
            title = line.substr(sep[4] + 1);
            // hwnd bizim çökmemizden sağ çıkar; geri dönüşüme karşı exe kontrolü
            HWND cand = (HWND)hv;
            if (IsWindow(cand) && _wcsicmp(ExeNameOf(cand).c_str(), exe.c_str()) == 0)
                w = cand;
        }
        else
        {
            size_t p1 = line.rfind(L'|');
            if (p1 == std::wstring::npos) continue;
            size_t p2 = line.rfind(L'|', p1 - 1);
            if (p2 == std::wstring::npos) continue;
            title = line.substr(0, p2);
            x = _wtoi(line.substr(p2 + 1, p1 - p2 - 1).c_str());
            y = _wtoi(line.substr(p1 + 1).c_str());
        }
        if (!w && !title.empty()) w = FindWindowW(nullptr, title.c_str());
        if (w && IsWindow(w))
        {
            // M18: bayat dosya normal duran pencereyi ÇEKMESİN - sadece
            // gerçekten park görünümlü (alt kenara gömülü) pencereye dokun
            RECT r{}; GetWindowRect(w, &r);
            if (r.top > GetSystemMetrics(SM_CYSCREEN) - 60)
            {
                SetWindowPos(w, HWND_NOTOPMOST, x, y, 0, 0,
                    SWP_NOSIZE | SWP_NOACTIVATE);
                if (wasMax) ShowWindow(w, SW_MAXIMIZE);
            }
        }
        else if (!w)
            keep.push_back(line); // uygulama yeniden açılınca diye sakla
    }
    f.close();
    // M18: koşulsuz silme yok - kurtarılamayanlar sigortada kalır
    if (keep.empty())
        DeleteFileW(PendingFilePath().c_str());
    else
    {
        std::wofstream o(PendingFilePath(), std::ios::trunc);
        for (auto& l : keep) o << l << L"\n";
    }
}


// ---- WinRT / D3D interop yardımcıları ----
static winrt::GraphicsCaptureItem CreateItemForWindow(HWND hwnd)
{
    auto factory = winrt::get_activation_factory<winrt::GraphicsCaptureItem>();
    auto interop = factory.as<IGraphicsCaptureItemInterop>();
    winrt::GraphicsCaptureItem item{ nullptr };
    winrt::check_hresult(interop->CreateForWindow(
        hwnd, winrt::guid_of<winrt::GraphicsCaptureItem>(), winrt::put_abi(item)));
    return item;
}

static winrt::com_ptr<ID3D11Texture2D> TextureFromSurface(
    winrt::IDirect3DSurface const& surface)
{
    auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<ID3D11Texture2D> tex;
    winrt::check_hresult(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), tex.put_void()));
    return tex;
}

static winrt::IDirect3DDevice CreateWinrtDevice(winrt::com_ptr<ID3D11Device> const& device)
{
    auto dxgi = device.as<IDXGIDevice>();
    winrt::com_ptr<::IInspectable> insp;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put()));
    return insp.as<winrt::IDirect3DDevice>();
}

// ---- Pencere numaralandırma ----
static BOOL CALLBACK EnumCb(HWND hwnd, LPARAM lp)
{
    auto* out = reinterpret_cast<std::vector<HWND>*>(lp);
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return TRUE;
    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return TRUE;
    if (GetWindow(hwnd, GW_OWNER)) return TRUE; // sahipli (dialog/popup) dışla
    wchar_t title[256];
    if (GetWindowTextW(hwnd, title, 256) == 0) return TRUE;
    DWORD cloaked = 0;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return TRUE;
    RECT r{}; GetWindowRect(hwnd, &r);
    if (r.right - r.left < 300 || r.bottom - r.top < 200) return TRUE;
    DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;
    wchar_t cls[128]; GetClassNameW(hwnd, cls, 128);
    if (!wcscmp(cls, L"SpatialCanvasWnd")) return TRUE; // M16: kendi tuvalimiz (ikinci örnek dahil)
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW")) return TRUE;
    if (!wcscmp(cls, L"ApplicationFrameWindow")) return TRUE; // UWP: park/restore guvenilmez
    out->push_back(hwnd);
    return TRUE;
}

// ---- D3D kurulum ----
static void InitD3D()
{
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE,
        nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
        g_device.put(), nullptr, g_ctx.put()));
    g_winrtDevice = CreateWinrtDevice(g_device);

    // Swapchain (flip model)
    auto dxgiDevice = g_device.as<IDXGIDevice>();
    winrt::com_ptr<IDXGIAdapter> adapter;
    winrt::check_hresult(dxgiDevice->GetAdapter(adapter.put()));
    winrt::com_ptr<IDXGIFactory2> factory;
    winrt::check_hresult(adapter->GetParent(winrt::guid_of<IDXGIFactory2>(), factory.put_void()));
    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = g_sw; scd.Height = g_sh;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc = { 1, 0 };
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    winrt::check_hresult(factory->CreateSwapChainForHwnd(
        g_device.get(), g_hwnd, &scd, nullptr, nullptr, g_swap.put()));
    winrt::com_ptr<ID3D11Texture2D> back;
    winrt::check_hresult(g_swap->GetBuffer(0, winrt::guid_of<ID3D11Texture2D>(), back.put_void()));
    winrt::check_hresult(g_device->CreateRenderTargetView(back.get(), nullptr, g_rtv.put()));

    // Shader derleme
    winrt::com_ptr<ID3DBlob> vsb, psb, err;
    if (FAILED(D3DCompile(g_shaderSrc, strlen(g_shaderSrc), "canvas", nullptr, nullptr,
        "VSMain", "vs_5_0", 0, 0, vsb.put(), err.put())))
        winrt::throw_hresult(E_FAIL);
    err = nullptr;
    if (FAILED(D3DCompile(g_shaderSrc, strlen(g_shaderSrc), "canvas", nullptr, nullptr,
        "PSMain", "ps_5_0", 0, 0, psb.put(), err.put())))
        winrt::throw_hresult(E_FAIL);
    winrt::check_hresult(g_device->CreateVertexShader(
        vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, g_vs.put()));
    winrt::check_hresult(g_device->CreatePixelShader(
        psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, g_ps.put()));

    // Sabit buffer + sampler + rasterizer
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 32; // M28: float4 rect + float4 extra (opacity)
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    winrt::check_hresult(g_device->CreateBuffer(&bd, nullptr, g_cb.put()));
    // M28: alpha blend (opacity kuralı için)
    D3D11_BLEND_DESC bld{};
    bld.RenderTarget[0].BlendEnable = TRUE;
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    winrt::check_hresult(g_device->CreateBlendState(&bld, g_blend.put()));
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    winrt::check_hresult(g_device->CreateSamplerState(&sd, g_sampler.put()));
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    winrt::check_hresult(g_device->CreateRasterizerState(&rd, g_raster.put()));
}

// ---- M4: D2D/DWrite overlay kurulumu ----
static void InitD2D()
{
    try
    {
        winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory), nullptr, g_d2dFactory.put_void()));
        winrt::com_ptr<IDXGISurface> surf;
        winrt::check_hresult(g_swap->GetBuffer(0, __uuidof(IDXGISurface), surf.put_void()));
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        winrt::check_hresult(g_d2dFactory->CreateDxgiSurfaceRenderTarget(
            surf.get(), &props, g_d2dRT.put()));
        g_d2dRT->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.95f), g_brText.put());
        g_d2dRT->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.55f), g_brBg.put());
        g_d2dRT->CreateSolidColorBrush(D2D1::ColorF(0.35f, 0.65f, 1.0f, 0.9f), g_brHover.put());
        // M19: cache'li kare-başı fırçalar (panel/dock/arama zemini, seçim, marquee)
        g_d2dRT->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.065f, 0.09f, 0.93f), g_brPanelBg.put());
        g_d2dRT->CreateSolidColorBrush(D2D1::ColorF(0.96f, 0.65f, 0.14f, 1.0f), g_brSel.put());
        g_d2dRT->CreateSolidColorBrush(D2D1::ColorF(0.20f, 0.85f, 0.65f, 1.0f), g_brPick.put());
        g_d2dRT->CreateSolidColorBrush(D2D1::ColorF(0.98f, 0.82f, 0.25f, 0.95f), g_brNote.put()); // M44 (renk not başına)
        // M19: ızgara = wrap-mode bitmap brush - binlerce FillRectangle yerine tek
        {
            std::vector<uint32_t> px(64 * 64, 0); // şeffaf, (0,0)-(1,1) beyaz nokta
            px[0] = px[1] = px[64] = px[65] = 0xFFFFFFFF;
            g_d2dRT->CreateBitmap(D2D1::SizeU(64, 64), px.data(), 64 * 4,
                D2D1::BitmapProperties(D2D1::PixelFormat(
                    DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
                g_gridBmp.put());
            g_d2dRT->CreateBitmapBrush(g_gridBmp.get(),
                D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP,
                    D2D1_EXTEND_MODE_WRAP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR),
                g_gridBrush.put());
        }
        // M29: radyal vinyet zemin (prosedürel his - merkez parlak, kenar koyu)
        {
            D2D1_GRADIENT_STOP gs[3] = {
                { 0.0f, D2D1::ColorF(0.13f, 0.14f, 0.20f, 1.0f) },
                { 0.6f, D2D1::ColorF(0.06f, 0.07f, 0.11f, 1.0f) },
                { 1.0f, D2D1::ColorF(0.015f, 0.02f, 0.035f, 1.0f) } };
            winrt::com_ptr<ID2D1GradientStopCollection> coll;
            if (SUCCEEDED(g_d2dRT->CreateGradientStopCollection(gs, 3,
                D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, coll.put())))
            {
                float cx = g_sw / 2.0f, cy = g_sh / 2.0f;
                g_d2dRT->CreateRadialGradientBrush(
                    D2D1::RadialGradientBrushProperties(
                        D2D1::Point2F(cx, cy), D2D1::Point2F(0, 0),
                        cx * 1.15f, cy * 1.15f),
                    coll.get(), g_bgRadial.put());
            }
        }
        // M13: ikon dönüştürme için WIC (bir kez; başarısızlık overlay'i öldürmez)
        if (!g_wic)
            CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), g_wic.put_void());
        winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(g_dwFactory.put())));
        winrt::check_hresult(g_dwFactory->CreateTextFormat(L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 15.0f, L"tr-tr", g_textFmt.put()));
        g_textFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g_textFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        g_textFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        winrt::check_hresult(g_dwFactory->CreateTextFormat(L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"tr-tr", g_textFmtL.put()));
        g_textFmtL->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        g_textFmtL->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        g_textFmtL->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        // M44: not metni - sol-üst hizalı, satır kaydırmalı (dünya-ölçekli transform altında çizilir)
        winrt::check_hresult(g_dwFactory->CreateTextFormat(L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 15.0f, L"tr-tr", g_textFmtN.put()));
        g_textFmtN->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        g_textFmtN->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        g_textFmtN->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }
    catch (...) { g_d2dRT = nullptr; } // overlay süs - olmazsa app yine yaşar
}

// ---- M8: Tuval alanı CANLI uygulanır (yeniden başlatma yok) ----
static void ComputeGeometry()
{
    g_priW = GetSystemMetrics(SM_CXSCREEN);
    g_priH = GetSystemMetrics(SM_CYSCREEN);
    if (g_set.canvasSpan == 1)
    {
        g_vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        g_vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        g_sw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        g_sh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    }
    else
    {
        g_vx = 0; g_vy = 0;
        g_sw = g_priW; g_sh = g_priH;
    }
    g_uiX = (float)(-g_vx);
    g_uiY = (float)(-g_vy);
}

static void RecreateRenderTargets()
{
    g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    g_rtv = nullptr;
    g_d2dRT = nullptr;
    g_brText = nullptr; g_brBg = nullptr; g_brHover = nullptr;
    g_brPanelBg = nullptr; g_brSel = nullptr; g_brPick = nullptr; // M19
    g_brNote = nullptr;                                           // M44
    g_gridBrush = nullptr; g_gridBmp = nullptr;                    // M19
    g_bgRadial = nullptr;                                          // M29
    g_d2dFactory = nullptr; g_dwFactory = nullptr;
    g_textFmt = nullptr; g_textFmtL = nullptr; g_textFmtN = nullptr; // M44
    for (auto& t : g_tiles) t.icoBmp = nullptr; // M13: eski RT'nin bitmap'leri
    for (auto& l : g_launchers) l.icoBmp = nullptr; // M24: launcher ikonları
    g_ctx->Flush();
    winrt::check_hresult(g_swap->ResizeBuffers(0, g_sw, g_sh, DXGI_FORMAT_UNKNOWN, 0));
    winrt::com_ptr<ID3D11Texture2D> back;
    winrt::check_hresult(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), back.put_void()));
    winrt::check_hresult(g_device->CreateRenderTargetView(back.get(), nullptr, g_rtv.put()));
    InitD2D();
}

static void ApplyCanvasSpan()
{
    ComputeGeometry();
    SetWindowPos(g_hwnd, nullptr, g_vx, g_vy, g_sw, g_sh,
        SWP_NOZORDER | SWP_NOACTIVATE);
    try { RecreateRenderTargets(); } catch (...) {}
    FitCamera();
    RaiseCanvasTopmost(); // M11: span değişiminde topmost + park katmanı korunur
}


// ---- M5: Ayarlar paneli ----
// M8: tuş adı yardımcıları
static std::wstring KeyName(int vk)
{
    if (vk == VK_ESCAPE) return L"ESC";
    if (vk == VK_MBUTTON) return TL(L"Middle click", L"Orta tık");
    if (vk == VK_XBUTTON1) return TL(L"Mouse Back", L"Fare Geri");
    if (vk == VK_XBUTTON2) return TL(L"Mouse Fwd", L"Fare İleri");
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LONG l = (LONG)(sc << 16);
    if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN ||
        vk == VK_PRIOR || vk == VK_NEXT || vk == VK_HOME || vk == VK_END ||
        vk == VK_INSERT || vk == VK_DELETE) l |= (1 << 24);
    wchar_t buf[64]{};
    if (GetKeyNameTextW(l, buf, 64) > 0) return buf;
    return L"?";
}

static std::wstring HotkeyName(Key k)
{
    std::wstring s;
    if (k.mods & 1) s += L"Ctrl+";
    if (k.mods & 2) s += L"Alt+";
    if (k.mods & 4) s += L"Shift+";
    s += KeyName(k.vk);
    return s;
}

static std::wstring RowLabel(int id)
{
    switch (id)
    {
    case 11: return TL(L"Language", L"Dil"); // M47
    case 12: return TL(L"Update check", L"Güncelleme kontrolü"); // M48
    case 13: return TL(L"Restore last view", L"Son görünümü geri yükle"); // M50
    case 0: return TL(L"Capture FPS", L"Yakalama FPS");
    case 1: return TL(L"Animation speed", L"Animasyon hızı");
    case 2: return TL(L"Title labels", L"Başlık etiketleri");
    case 3: return TL(L"Hover frame", L"Vurgu çerçevesi");
    case 4: return TL(L"Dive threshold", L"Dalış eşiği");
    case 5: return TL(L"Max windows", L"Maks. pencere");
    case 6: return TL(L"Background", L"Arka plan");
    case 7: return TL(L"Start with Windows", L"Windows ile başlat");
    case 8: return TL(L"Canvas area", L"Tuval alanı");
    case 9: return TL(L"Dot grid", L"Nokta ızgara");
    case 10: return TL(L"Minimap", L"Minimap");
    case 101: return TL(L"Pull back", L"Geri çekil");
    case 102: return TL(L"Toggle panel", L"Panel aç/kapa");
    case 103: return TL(L"Fit selection/all", L"Seçimi/tümünü sığdır");
    case 104: return TL(L"Global zoom key", L"Global zoom tuşu");
    case 105: return TL(L"Exit", L"Çıkış");
    case 106: return TL(L"Search windows", L"Pencere ara");
    case 107: return TL(L"Launch app", L"Uygulama başlat");
    }
    return L"";
}

static std::wstring RowValue(int id)
{
    switch (id)
    {
    case 11: return g_set.lang == 1 ? L"Türkçe" : L"English"; // M47
    case 12: // M48 (g_updateVer mutex'li okunur)
        if (g_updateAvail) { std::lock_guard<std::mutex> lk(g_updateMutex);
            return L"v" + g_updateVer + TL(L" available!", L" mevcut!"); }
        return g_set.updateCheck ? TL(L"On", L"Açık") : TL(L"Off", L"Kapalı");
    case 13: return g_set.restoreView ? TL(L"On", L"Açık") : TL(L"Off", L"Kapalı"); // M50
    case 0: return std::to_wstring(g_set.fpsCap);
    case 1: return g_set.animSpeed == 0 ? TL(L"Slow", L"Yavaş") : (g_set.animSpeed == 1 ? TL(L"Normal", L"Normal") : TL(L"Fast", L"Hızlı"));
    case 2: return g_set.labels ? TL(L"On", L"Açık") : TL(L"Off", L"Kapalı");
    case 3: return g_set.hover ? TL(L"On", L"Açık") : TL(L"Off", L"Kapalı");
    case 4: return g_set.diveZoom < 0.9f ? TL(L"Early", L"Erken") : TL(L"Normal", L"Normal");
    case 5: return std::to_wstring(g_set.maxTiles);
    case 6: return g_set.bgPreset == 0 ? TL(L"Dark", L"Koyu") : (g_set.bgPreset == 1 ? TL(L"Night", L"Gece")
        : (g_set.bgPreset == 2 ? TL(L"Black", L"Siyah") : TL(L"Vignette", L"Vinyet")));
    case 7: return g_set.autostart ? TL(L"On", L"Açık") : TL(L"Off", L"Kapalı");
    case 8: return g_set.canvasSpan ? TL(L"All", L"Tümü") : TL(L"Primary", L"Ana ekran");
    case 9: return g_set.grid ? TL(L"On", L"Açık") : TL(L"Off", L"Kapalı");
    case 10: return g_set.minimap ? TL(L"On", L"Açık") : TL(L"Off", L"Kapalı");
    case 101: return g_captureRow == 101 ? TL(L"press key…", L"tuşa bas…") : HotkeyName(g_set.kbPull);
    case 102: return g_captureRow == 102 ? TL(L"press key…", L"tuşa bas…") : HotkeyName(g_set.kbPanel);
    case 103: return g_captureRow == 103 ? TL(L"press key…", L"tuşa bas…") : HotkeyName(g_set.kbFit);
    case 104:
    {
        static const wchar_t* M[5] = { L"Ctrl+Alt", L"Ctrl+Shift", L"Alt+Shift", L"Ctrl", L"Alt" };
        return M[g_set.wheelMod];
    }
    case 105: return g_captureRow == 105 ? TL(L"press key…", L"tuşa bas…") : HotkeyName(g_set.kbExit);
    case 106: return g_captureRow == 106 ? TL(L"press key…", L"tuşa bas…") : HotkeyName(g_set.kbSearch);
    case 107: return g_captureRow == 107 ? TL(L"press key…", L"tuşa bas…") : HotkeyName(g_set.kbLaunch);
    }
    return L"";
}

static void CycleRow(int id)
{
    switch (id)
    {
    case 0:
        g_set.fpsCap = (g_set.fpsCap == 15) ? 30 : (g_set.fpsCap == 30 ? 60 : 15);
        ApplyFpsCap();
        break;
    case 1: g_set.animSpeed = (g_set.animSpeed + 1) % 3; break;
    case 2: g_set.labels = !g_set.labels; break;
    case 3: g_set.hover = !g_set.hover; break;
    case 4: g_set.diveZoom = (g_set.diveZoom < 0.9f) ? 0.92f : 0.80f; break;
    case 5:
        g_set.maxTiles = (g_set.maxTiles == 6) ? 9 : (g_set.maxTiles == 9 ? 12
            : (g_set.maxTiles == 12 ? 16 : 6));
        break;
    case 6: g_set.bgPreset = (g_set.bgPreset + 1) % 4; break; // M29: +Vinyet
    case 7: g_set.autostart = !g_set.autostart; ApplyAutostart(); break;
    case 8: g_set.canvasSpan = 1 - g_set.canvasSpan; ApplyCanvasSpan(); break; // M8: canlı
    case 9: g_set.grid = !g_set.grid; break; // M12
    case 10: g_set.minimap = !g_set.minimap; break; // M36
    case 11: g_set.lang = 1 - g_set.lang; break; // M47: EN/TR
    case 12: g_set.updateCheck = !g_set.updateCheck; break; // M48
    case 13: g_set.restoreView = !g_set.restoreView; break; // M50
    case 104: g_set.wheelMod = (g_set.wheelMod + 1) % 5; break;
    case -10: g_panelTab = 0; g_captureRow = -1; return; // M10: sekmeler
    case -11: g_panelTab = 1; return;
    case 101: case 102: case 103: case 105: case 106: case 107:
        // yakalama modu: bir sonraki tuş basışı bu kısayola atanır
        g_captureRow = (g_captureRow == id) ? -1 : id;
        return; // kayıt tuş atandığında yapılır
    }
    SaveSettings();
}

// Dişli butonu + açık panel imlecin altında mı? (UI ana monitöre sabit)
static bool OverPanelUi(POINT p)
{
    if (p.x >= g_uiX + 12 && p.x <= g_uiX + 52 &&
        p.y >= g_uiY + 12 && p.y <= g_uiY + 52) return true;
    if (g_updateAvail && g_updateRect.right > g_updateRect.left && // M51: sürüm pill'i
        (float)p.x >= g_updateRect.left && (float)p.x <= g_updateRect.right &&
        (float)p.y >= g_updateRect.top && (float)p.y <= g_updateRect.bottom) return true;
    if (g_panelOpen &&
        (float)p.x >= g_uiX + (g_panelA - 1.0f) * PANEL_W &&
        (float)p.x <= g_uiX + g_panelA * PANEL_W &&
        (float)p.y >= g_uiY && (float)p.y <= g_uiY + (float)g_priH) return true;
    return false;
}

static bool HandlePanelClick(POINT p)
{
    if (p.x >= g_uiX + 12 && p.x <= g_uiX + 52 &&
        p.y >= g_uiY + 12 && p.y <= g_uiY + 52)
    {
        g_captureRow = -1;
        g_panelOpen = !g_panelOpen;
        return true;
    }
    if (!g_panelOpen) return false;
    bool inPanel = (float)p.x >= g_uiX + (g_panelA - 1.0f) * PANEL_W &&
                   (float)p.x <= g_uiX + g_panelA * PANEL_W &&
                   (float)p.y >= g_uiY && (float)p.y <= g_uiY + (float)g_priH;
    if (inPanel)
    {
        for (auto& r : g_panelRows)
            if (p.x >= r.rect.left && p.x <= r.rect.right &&
                p.y >= r.rect.top && p.y <= r.rect.bottom)
            {
                if (r.id != g_captureRow) g_captureRow = -1;
                CycleRow(r.id);
                return true;
            }
        g_captureRow = -1;
        return true; // panel içi boş tık - yut
    }
    g_captureRow = -1;
    g_panelOpen = false; // panel dışına tık - kapat
    return true;
}

static void DrawPanel(POINT cur)
{
    float ux = g_uiX, uy = g_uiY; // ana monitör orijini (client uzayı)
    // dişli butonu (her zaman ANA monitörün sol üstünde)
    bool gearHov = (cur.x >= ux + 12 && cur.x <= ux + 52 &&
                    cur.y >= uy + 12 && cur.y <= uy + 52);
    D2D1_ROUNDED_RECT gear{ D2D1::RectF(ux + 12, uy + 12, ux + 52, uy + 52), 10, 10 };
    g_d2dRT->FillRoundedRectangle(gear, g_brBg.get());
    if (gearHov) g_d2dRT->DrawRoundedRectangle(gear, g_brHover.get(), 2.0f);
    g_d2dRT->DrawText(L"\u2699", 1, g_textFmt.get(), gear.rect, g_brText.get());
    // M48: yeni s\u00fcr\u00fcm g\u00f6stergesi (kal\u0131c\u0131 amber pill, di\u015flinin sa\u011f\u0131nda - toast ka\u00e7\u0131r\u0131l\u0131rsa)
    if (g_updateAvail)
    {
        std::wstring ut;
        { std::lock_guard<std::mutex> lk(g_updateMutex); ut = L"\u2191 v" + g_updateVer; }
        float pw = 26.0f + (float)ut.size() * 8.5f;
        D2D1_RECT_F up = D2D1::RectF(ux + 58, uy + 14, ux + 58 + pw, uy + 50);
        g_updateRect = up; // M51: t\u0131klanabilir
        bool upHov = (cur.x >= up.left && cur.x <= up.right && cur.y >= up.top && cur.y <= up.bottom);
        D2D1_ROUNDED_RECT upr{ up, 9, 9 };
        g_d2dRT->FillRoundedRectangle(upr, g_brBg.get());
        g_d2dRT->DrawRoundedRectangle(upr, g_brSel.get(), upHov ? 2.5f : 1.5f); // M51: hover
        g_d2dRT->DrawText(ut.c_str(), (UINT32)ut.size(), g_textFmt.get(), up, g_brSel.get());
    }
    else g_updateRect = D2D1::RectF(0, 0, 0, 0);

    g_panelRows.clear();
    if (g_panelA < 0.01f) return;
    float left = ux + (g_panelA - 1.0f) * PANEL_W;
    float top = uy, bot = uy + (float)g_priH;
    // panel zemini (ana monitör yüksekliğinde) - M19: cache'li fırça
    g_d2dRT->FillRectangle(D2D1::RectF(left, top, left + PANEL_W, bot), g_brPanelBg.get());
    g_d2dRT->DrawLine(D2D1::Point2F(left + PANEL_W, top),
        D2D1::Point2F(left + PANEL_W, bot), g_brHover.get(), 1.0f);
    // M10: sekmeler (Ayarlar / Kısayollar)
    const wchar_t* TABS[2] = { TL(L"Settings", L"Ayarlar"), TL(L"Shortcuts", L"Kısayollar") };
    float tx = left + 14;
    for (int tb = 0; tb < 2; tb++)
    {
        D2D1_RECT_F tr = D2D1::RectF(tx, top + 58, tx + 138, top + 96);
        D2D1_ROUNDED_RECT trr{ tr, 9, 9 };
        if (g_panelTab == tb) g_d2dRT->FillRoundedRectangle(trr, g_brBg.get());
        g_d2dRT->DrawRoundedRectangle(trr, g_brHover.get(),
            g_panelTab == tb ? 2.0f : 1.0f);
        g_d2dRT->DrawText(TABS[tb], (UINT32)wcslen(TABS[tb]), g_textFmt.get(),
            tr, g_brText.get());
        g_panelRows.push_back({ tr, -10 - tb });
        tx += 150;
    }
    // satırlar (aktif sekmeye göre)
    float y = top + 116;
    static const int T0[] = { 11,12,13,0,1,2,3,4,5,6,7,8,9,10 }; // M47/M48/M50 üstte
    static const int T1[] = { 101,102,103,104,105,106,107 };
    const int* ids = g_panelTab ? T1 : T0;
    int idCount = g_panelTab ? 7 : 14;
    // M50: satır yüksekliğini ekrana göre kıs (14 satır kısa ekranda taşmasın)
    const float rowH = std::min(50.0f, ((float)g_priH - 130.0f) / (float)idCount);
    for (int idx = 0; idx < idCount; idx++)
    {
        int id = ids[idx];
        D2D1_RECT_F row = D2D1::RectF(left + 14, y, left + PANEL_W - 14, y + rowH - 8);
        bool hovRow = (cur.x >= row.left && cur.x <= row.right &&
                       cur.y >= row.top && cur.y <= row.bottom);
        if (hovRow)
        {
            D2D1_ROUNDED_RECT hr{ row, 8, 8 };
            g_d2dRT->FillRoundedRectangle(hr, g_brBg.get());
        }
        float chipW = (id >= 100) ? 124.0f : 84.0f;
        std::wstring lab = RowLabel(id);
        g_d2dRT->DrawText(lab.c_str(), (UINT32)lab.size(), g_textFmtL.get(),
            D2D1::RectF(row.left + 12, row.top, row.right - chipW - 16, row.bottom),
            g_brText.get());
        std::wstring val = RowValue(id);
        D2D1_ROUNDED_RECT chip{ D2D1::RectF(row.right - chipW - 8, row.top + 7,
            row.right - 8, row.bottom - 7), 7, 7 };
        g_d2dRT->FillRoundedRectangle(chip, g_brBg.get());
        g_d2dRT->DrawRoundedRectangle(chip, g_brHover.get(),
            (g_captureRow == id) ? 2.5f : 1.0f);
        g_d2dRT->DrawText(val.c_str(), (UINT32)val.size(), g_textFmt.get(),
            chip.rect, g_brText.get());
        g_panelRows.push_back({ row, id });
        y += rowH;
    }
    // alt bilgi
    if (g_panelTab == 1)
    {
        std::wstring foot2 = TL(L"F1: all shortcuts (incl. fixed)", L"F1: tüm kısayollar (sabitler dahil)"); // M20
        g_d2dRT->DrawText(foot2.c_str(), (UINT32)foot2.size(), g_textFmtL.get(),
            D2D1::RectF(left + 26, bot - 80, left + PANEL_W - 14, bot - 52),
            g_brHover.get());
        std::wstring foot = TL(L"Click chip, press a key or MOUSE button", L"Çipe tıkla, klavye veya FARE tuşuna bas");
        g_d2dRT->DrawText(foot.c_str(), (UINT32)foot.size(), g_textFmtL.get(),
            D2D1::RectF(left + 26, bot - 48, left + PANEL_W - 14, bot - 16),
            g_brText.get());
    }
}

// ---- M13: dock (macOS tarzı; imleç ana monitör alt kenarına inince açılır) ----
// HICON -> D2D bitmap (RT'ye bağlı; cache parametresi recreate'te null'lanır)
static ID2D1Bitmap* IconToBitmap(HICON icon, winrt::com_ptr<ID2D1Bitmap>& cache)
{
    if (cache) return cache.get();
    if (!icon || !g_wic || !g_d2dRT) return nullptr;
    winrt::com_ptr<IWICBitmap> wb;
    if (FAILED(g_wic->CreateBitmapFromHICON(icon, wb.put()))) return nullptr;
    winrt::com_ptr<IWICFormatConverter> cv;
    if (FAILED(g_wic->CreateFormatConverter(cv.put()))) return nullptr;
    if (FAILED(cv->Initialize(wb.get(), GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return nullptr;
    if (FAILED(g_d2dRT->CreateBitmapFromWicBitmap(cv.get(), nullptr, cache.put())))
        return nullptr;
    return cache.get();
}
static ID2D1Bitmap* EnsureIconBitmap(Tile& t) { return IconToBitmap(t.icon, t.icoBmp); }

static void DrawDock(POINT cur)
{
    g_dockChips.clear();
    if (g_dockA < 0.01f || g_tiles.empty()) return;
    const float CH = 56.0f, GAP = 8.0f, PAD = 10.0f;
    int n = (int)g_tiles.size();
    float w = n * (CH + GAP) - GAP + PAD * 2;
    float cx = g_uiX + (float)g_priW / 2.0f;
    float bot = g_uiY + (float)g_priH - 8.0f + (1.0f - g_dockA) * 100.0f; // kayar
    D2D1_RECT_F bar = D2D1::RectF(cx - w / 2, bot - CH - PAD * 2, cx + w / 2, bot);
    ID2D1SolidColorBrush* bgD = g_brPanelBg.get(); // M19: cache'li
    D2D1_ROUNDED_RECT rb{ bar, 14, 14 };
    g_d2dRT->FillRoundedRectangle(rb, bgD);
    g_d2dRT->DrawRoundedRectangle(rb, g_brHover.get(), 1.0f);
    float x = bar.left + PAD;
    for (int i = 0; i < n; i++)
    {
        D2D1_RECT_F c = D2D1::RectF(x, bar.top + PAD, x + CH, bar.top + PAD + CH);
        bool hov = (cur.x >= c.left && cur.x <= c.right &&
                    cur.y >= c.top && cur.y <= c.bottom);
        if (hov)
        {
            D2D1_ROUNDED_RECT cr{ c, 10, 10 };
            g_d2dRT->FillRoundedRectangle(cr, g_brBg.get());
            g_d2dRT->DrawRoundedRectangle(cr, g_brHover.get(), 2.0f);
            Tile& ht = g_tiles[i]; // başlık balonu
            std::wstring tt = ht.title.empty() ? ht.exe : ht.title;
            if (tt.size() > 44) tt = tt.substr(0, 43) + L"…";
            float tw = 28.0f + (float)tt.size() * 7.5f;
            float tcx = (c.left + c.right) / 2.0f;
            D2D1_RECT_F tip = D2D1::RectF(tcx - tw / 2, bar.top - 42,
                                          tcx + tw / 2, bar.top - 12);
            D2D1_ROUNDED_RECT tr2{ tip, 8, 8 };
            g_d2dRT->FillRoundedRectangle(tr2, bgD);
            g_d2dRT->DrawText(tt.c_str(), (UINT32)tt.size(), g_textFmt.get(),
                tip, g_brText.get());
        }
        ID2D1Bitmap* bmp = EnsureIconBitmap(g_tiles[i]);
        if (bmp)
            g_d2dRT->DrawBitmap(bmp,
                D2D1::RectF(c.left + 8, c.top + 8, c.right - 8, c.bottom - 8));
        else
        {
            std::wstring ini = g_tiles[i].exe.substr(0, 1); // ikon yoksa baş harf
            g_d2dRT->DrawText(ini.c_str(), 1, g_textFmt.get(), c, g_brText.get());
        }
        g_dockChips.push_back({ c, i });
        x += CH + GAP;
    }
}

// ---- M24: uygulama dock'u (SAĞ kenar; imleç sağ kenara gelince açılır) ----
// launcher.txt kısayollarını ikon olarak gösterir; tıkla = başlat.
static void DrawAppDock(POINT cur)
{
    g_appDockChips.clear();
    if (g_appDockA < 0.01f) return; // M25: boşken bile "+" için açılır
    float CW = 52.0f, GAP = 8.0f; const float PAD = 10.0f;
    int n = (int)g_launchers.size();
    int slots = n + 1; // M25: kısayollar + "+" ekle butonu
    // M24: çok kısayolda ekran dışına taşmasın - ikon adımını sığacak şekilde küçült
    float avail = (float)g_priH - 24.0f;
    float step = CW + GAP;
    float h = slots * step - GAP + PAD * 2;
    if (h > avail)
    {
        step = std::max((avail - PAD * 2 + GAP) / slots, 22.0f);
        CW = std::max(step - GAP, 16.0f);
        h = std::min(slots * step - GAP + PAD * 2, avail);
    }
    float cy = g_uiY + (float)g_priH / 2.0f;
    float right = g_uiX + (float)g_priW - 8.0f + (1.0f - g_appDockA) * 80.0f; // sağdan kayar
    D2D1_RECT_F bar = D2D1::RectF(right - CW - PAD * 2, cy - h / 2, right, cy + h / 2);
    D2D1_ROUNDED_RECT rb{ bar, 14, 14 };
    g_d2dRT->FillRoundedRectangle(rb, g_brPanelBg.get());
    g_d2dRT->DrawRoundedRectangle(rb, g_brHover.get(), 1.0f);
    float y = bar.top + PAD;
    for (int i = 0; i < n; i++)
    {
        D2D1_RECT_F c = D2D1::RectF(bar.left + PAD, y, bar.left + PAD + CW, y + CW);
        bool hov = (cur.x >= c.left && cur.x <= c.right &&
                    cur.y >= c.top && cur.y <= c.bottom);
        if (hov)
        {
            D2D1_ROUNDED_RECT cr{ c, 10, 10 };
            g_d2dRT->FillRoundedRectangle(cr, g_brBg.get());
            g_d2dRT->DrawRoundedRectangle(cr, g_brHover.get(), 2.0f);
            std::wstring tt = g_launchers[i].label + TL(L"   (right-click: remove)", L"   (sağ tık: kaldır)"); // etiket balonu (SOL)
            if (tt.size() > 48) tt = tt.substr(0, 47) + L"…";
            float tw = 28.0f + (float)tt.size() * 7.5f;
            float tcy = (c.top + c.bottom) / 2.0f;
            D2D1_RECT_F tip = D2D1::RectF(c.left - tw - 12, tcy - 16, c.left - 8, tcy + 16);
            D2D1_ROUNDED_RECT tr2{ tip, 8, 8 };
            g_d2dRT->FillRoundedRectangle(tr2, g_brPanelBg.get());
            g_d2dRT->DrawText(tt.c_str(), (UINT32)tt.size(), g_textFmt.get(),
                tip, g_brText.get());
        }
        ID2D1Bitmap* bmp = IconToBitmap(g_launchers[i].icon, g_launchers[i].icoBmp);
        float ip = CW * 0.16f; // ikon iç boşluğu (CW küçülünce orantılı)
        if (bmp)
            g_d2dRT->DrawBitmap(bmp,
                D2D1::RectF(c.left + ip, c.top + ip, c.right - ip, c.bottom - ip));
        else
        {
            // ikon yoksa: etiketin (yoksa exe'nin) baş harfi
            const std::wstring& src = g_launchers[i].label.empty()
                ? g_launchers[i].exe : g_launchers[i].label;
            std::wstring ini = src.empty() ? L"?" : src.substr(0, 1);
            g_d2dRT->DrawText(ini.c_str(), (UINT32)ini.size(), g_textFmt.get(),
                c, g_brText.get());
        }
        g_appDockChips.push_back({ c, i });
        y += step;
    }
    // M25: "+" ekle butonu (en altta) - tıkla → dosya seç → kısayol ekle
    {
        D2D1_RECT_F pc = D2D1::RectF(bar.left + PAD, y, bar.left + PAD + CW, y + CW);
        bool ph = (cur.x >= pc.left && cur.x <= pc.right &&
                   cur.y >= pc.top && cur.y <= pc.bottom);
        D2D1_ROUNDED_RECT pr{ pc, 10, 10 };
        g_d2dRT->FillRoundedRectangle(pr, g_brBg.get());
        g_d2dRT->DrawRoundedRectangle(pr, ph ? g_brHover.get() : g_brPanelBg.get(),
            ph ? 2.0f : 1.0f);
        if (ph) // hover balonu
        {
            std::wstring tt = L"Uygulama ekle";
            float tw = 28.0f + (float)tt.size() * 7.5f;
            float tcy = (pc.top + pc.bottom) / 2.0f;
            D2D1_RECT_F tip = D2D1::RectF(pc.left - tw - 12, tcy - 16, pc.left - 8, tcy + 16);
            D2D1_ROUNDED_RECT tr2{ tip, 8, 8 };
            g_d2dRT->FillRoundedRectangle(tr2, g_brPanelBg.get());
            g_d2dRT->DrawText(tt.c_str(), (UINT32)tt.size(), g_textFmt.get(),
                tip, g_brText.get());
        }
        g_d2dRT->DrawText(L"+", 1, g_textFmt.get(), pc, g_brText.get());
        g_appDockChips.push_back({ pc, -1 }); // -1 = ekle butonu
    }
}

// M36: minimap (sağ-alt kuşbakışı) - tüm tile'lar + viewport çerçevesi; tıkla=zıpla
static void DrawMinimap()
{
    g_minimapRect = D2D1::RectF(0, 0, 0, 0);
    if (!g_set.minimap || g_activeTile >= 0) return;
    if (g_tiles.empty() && g_notes.empty()) return;
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f; int n = 0;
    for (auto& t : g_tiles)
    {
        if (t.pinnedFlag) continue;
        minX = std::min(minX, t.wx); minY = std::min(minY, t.wy);
        maxX = std::max(maxX, t.wx + t.ww); maxY = std::max(maxY, t.wy + t.wh);
        n++;
    }
    // M45: notları da minimap sınırına kat (uzaktaki not noktası kırpılmasın)
    for (auto& nt : g_notes)
    {
        minX = std::min(minX, nt.wx); minY = std::min(minY, nt.wy);
        maxX = std::max(maxX, nt.wx + nt.w); maxY = std::max(maxY, nt.wy + nt.h);
        n++;
    }
    for (auto& z : g_zones) // M58: bölgeleri de minimap sınırına kat
    {
        minX = std::min(minX, z.wx); minY = std::min(minY, z.wy);
        maxX = std::max(maxX, z.wx + z.w); maxY = std::max(maxY, z.wy + z.h);
        n++;
    }
    if (n == 0) return;
    // kamera görüşünü de dahil et (dışarıdaysa minimap'te yön belli olsun)
    float vx = g_cam.x, vy = g_cam.y, vw = g_sw / g_cam.zoom, vh = g_sh / g_cam.zoom;
    minX = std::min(minX, vx); minY = std::min(minY, vy);
    maxX = std::max(maxX, vx + vw); maxY = std::max(maxY, vy + vh);
    float wW = std::max(maxX - minX, 1.0f), wH = std::max(maxY - minY, 1.0f);
    const float MAXW = 200.0f, MAXH = 130.0f, pad = 10.0f;
    float scale = std::min(MAXW / wW, MAXH / wH);
    float mw = wW * scale, mh = wH * scale;
    float mx = g_uiX + (float)g_priW - mw - 18.0f; // sağ-alt köşe
    float my = g_uiY + (float)g_priH - mh - 18.0f;
    g_mmX = mx; g_mmY = my; g_mmMinX = minX; g_mmMinY = minY; g_mmScale = scale;
    g_minimapRect = D2D1::RectF(mx - pad, my - pad, mx + mw + pad, my + mh + pad);
    D2D1_ROUNDED_RECT bg{ g_minimapRect, 8, 8 };
    g_d2dRT->FillRoundedRectangle(bg, g_brPanelBg.get());
    g_d2dRT->DrawRoundedRectangle(bg, g_brHover.get(), 1.0f);
    g_brHover->SetOpacity(0.55f);
    for (auto& t : g_tiles) // tile'lar (küçük dolu dikdörtgen)
    {
        if (t.pinnedFlag) continue;
        float rx = mx + (t.wx - minX) * scale, ry = my + (t.wy - minY) * scale;
        g_d2dRT->FillRectangle(
            D2D1::RectF(rx, ry, rx + std::max(t.ww * scale, 2.0f),
                        ry + std::max(t.wh * scale, 2.0f)), g_brHover.get());
    }
    g_brHover->SetOpacity(1.0f);
    // viewport çerçevesi (amber)
    float vrx = mx + (vx - minX) * scale, vry = my + (vy - minY) * scale;
    g_d2dRT->DrawRectangle(
        D2D1::RectF(vrx, vry, vrx + vw * scale, vry + vh * scale), g_brSel.get(), 1.5f);
    // M39: kayıtlı yer imleri (Ctrl+1..4) minimap'te küçük nokta - keşfedilebilirlik
    for (int a = 0; a < 4; a++)
    {
        if (!g_anchors[a].set || g_anchors[a].zoom <= 0) continue;
        float ax = g_anchors[a].x + (g_sw / g_anchors[a].zoom) / 2.0f;
        float ay = g_anchors[a].y + (g_sh / g_anchors[a].zoom) / 2.0f;
        float px = mx + (ax - minX) * scale, py = my + (ay - minY) * scale;
        if (px < mx - 2 || px > mx + mw + 2 || py < my - 2 || py > my + mh + 2) continue;
        D2D1_ELLIPSE e{ D2D1::Point2F(px, py), 3.5f, 3.5f };
        g_d2dRT->FillEllipse(e, g_brSel.get());
        g_d2dRT->DrawEllipse(e, g_brPanelBg.get(), 1.0f);
    }
    // M44: notlar minimap'te küçük dolu kare (mekansal yön)
    g_brPick->SetOpacity(0.8f);
    for (auto& n : g_notes)
    {
        float nxp = mx + (n.wx + n.w / 2 - minX) * scale;
        float nyp = my + (n.wy + n.h / 2 - minY) * scale;
        if (nxp < mx - 2 || nxp > mx + mw + 2 || nyp < my - 2 || nyp > my + mh + 2) continue;
        g_d2dRT->FillRectangle(D2D1::RectF(nxp - 2, nyp - 2, nxp + 2, nyp + 2), g_brPick.get());
    }
    g_brPick->SetOpacity(1.0f);
    // M58: bölgeler minimap'te renkli dış-çizgi dikdörtgen (mekansal bağlam)
    for (auto& z : g_zones)
    {
        float zx = mx + (z.wx - minX) * scale, zy = my + (z.wy - minY) * scale;
        float zw = z.w * scale, zh = z.h * scale;
        D2D1_COLOR_F zc = NoteColor(z.color); zc.a = 0.85f;
        g_brNote->SetColor(zc);
        g_d2dRT->DrawRectangle(D2D1::RectF(zx, zy, zx + zw, zy + zh), g_brNote.get(), 1.0f);
    }
    // M59: bağlayıcılar minimap'te ince teal çizgi (tam kuşbakışı: tile+not+zon+bağlantı)
    if (!g_connectors.empty())
    {
        auto miniCenter = [&](HWND h, float& px, float& py) -> bool {
            for (auto& t : g_tiles) if (t.source == h && !t.pinnedFlag) {
                px = mx + (t.wx + t.ww / 2 - minX) * scale;
                py = my + (t.wy + t.wh / 2 - minY) * scale; return true; }
            return false;
        };
        g_brPick->SetOpacity(0.7f);
        for (auto& c : g_connectors)
        {
            float ax, ay, bx, by;
            if (miniCenter(c.a, ax, ay) && miniCenter(c.b, bx, by))
                g_d2dRT->DrawLine(D2D1::Point2F(ax, ay), D2D1::Point2F(bx, by), g_brPick.get(), 0.8f);
        }
        g_brPick->SetOpacity(1.0f);
    }
}

// M44: not paleti rengi (4 yapışkan ton - açık zemin, koyu metin okunur)
static D2D1_COLOR_F NoteColor(int c)
{
    switch (c & 3)
    {
    case 1:  return D2D1::ColorF(0.55f, 0.85f, 0.45f, 0.95f); // yeşil
    case 2:  return D2D1::ColorF(0.50f, 0.75f, 0.98f, 0.95f); // mavi
    case 3:  return D2D1::ColorF(0.97f, 0.62f, 0.74f, 0.95f); // pembe
    default: return D2D1::ColorF(0.98f, 0.82f, 0.25f, 0.95f); // amber
    }
}

// M44: dünya noktasındaki notu bul (üstte yüzenden alta - en son eklenen önce)
static int NoteAt(float wx, float wy)
{
    for (int i = (int)g_notes.size() - 1; i >= 0; i--)
        if (wx >= g_notes[i].wx && wx <= g_notes[i].wx + g_notes[i].w &&
            wy >= g_notes[i].wy && wy <= g_notes[i].wy + g_notes[i].h)
            return i;
    return -1;
}

// M54: (wx,wy) bir zonun BAŞLIK ÇUBUĞUNDA mı (gövde tıklama-geçirgen). Üstten alta.
// Çubuk yüksekliği görseldeki ekran-uzayı bar ile eşleşir (min 22px → dünya 22/zoom).
static int ZoneTitleAt(float wx, float wy)
{
    float barW = std::max(ZONE_BAR, 22.0f / std::max(g_cam.zoom, 0.001f));
    for (int i = (int)g_zones.size() - 1; i >= 0; i--)
    {
        Zone& z = g_zones[i];
        if (wx >= z.wx && wx <= z.wx + z.w && wy >= z.wy && wy <= z.wy + barW)
            return i;
    }
    return -1;
}

// M54: bölge/zon çerçevelerini çiz (tile'ların ÜSTÜNDE ama notların ALTINDA; sadece
// kenar + başlık çubuğu + soluk dolgu → gövde içeriği görünür kalır).
static void DrawZones()
{
    if (g_zones.empty()) { g_hoverZone = -1; g_zoneDelIdx = -1; g_zoneResIdx = -1; g_zoneArrIdx = -1; return; }
    POINT cur = g_curClient;
    float mwx = g_cam.x + cur.x / g_cam.zoom, mwy = g_cam.y + cur.y / g_cam.zoom;
    g_hoverZone = -1; g_zoneDelIdx = -1; g_zoneResIdx = -1; g_zoneArrIdx = -1;
    D2D1_MATRIX_3X2_F world = D2D1::Matrix3x2F::Scale(g_cam.zoom, g_cam.zoom) *
        D2D1::Matrix3x2F::Translation(-g_cam.x * g_cam.zoom, -g_cam.y * g_cam.zoom);
    for (int i = 0; i < (int)g_zones.size(); i++)
    {
        Zone& z = g_zones[i];
        float sx = (z.wx - g_cam.x) * g_cam.zoom, sy = (z.wy - g_cam.y) * g_cam.zoom;
        float sw = z.w * g_cam.zoom, sh = z.h * g_cam.zoom;
        if (sx > (float)g_sw || sy > (float)g_sh || sx + sw < 0 || sy + sh < 0) continue;
        bool barHov = (mwx >= z.wx && mwx <= z.wx + z.w && mwy >= z.wy && mwy <= z.wy + ZONE_BAR);
        if (barHov) g_hoverZone = i;
        bool editing = (i == g_editZone);
        D2D1_COLOR_F col = NoteColor(z.color);
        // --- gövde kenarı + başlık çubuğu: dünya-uzayında ---
        g_d2dRT->SetTransform(world);
        D2D1_RECT_F body = D2D1::RectF(z.wx, z.wy, z.wx + z.w, z.wy + z.h);
        D2D1_ROUNDED_RECT brr{ body, 10, 10 };
        col.a = 0.06f; g_brNote->SetColor(col); g_d2dRT->FillRoundedRectangle(brr, g_brNote.get()); // soluk dolgu
        g_d2dRT->SetTransform(D2D1::Matrix3x2F::Identity());
        // M54: kenar + başlık çubuğu EKRAN-uzayında (her zoom'da görünür; dünya-kalınlık zoom-out'ta kaybolurdu)
        D2D1_RECT_F sr = D2D1::RectF(sx, sy, sx + sw, sy + sh);
        D2D1_ROUNDED_RECT srr{ sr, 10, 10 };
        col.a = editing ? 1.0f : 0.9f; g_brNote->SetColor(col);
        g_d2dRT->DrawRoundedRectangle(srr, g_brNote.get(), editing ? 4.0f : 2.5f); // renkli kenar (ekran-sabit)
        // M67: arama eşleşmesi - amber halka (seçili olan kalın)
        if (g_searchOpen && !g_searchText.empty() &&
            std::find(g_zoneMatches.begin(), g_zoneMatches.end(), i) != g_zoneMatches.end())
        {
            int selZone = -1;
            int base = (int)(g_matches.size() + g_noteMatches.size());
            if (g_searchSel >= base && g_searchSel - base < (int)g_zoneMatches.size())
                selZone = g_zoneMatches[g_searchSel - base];
            g_d2dRT->DrawRoundedRectangle(D2D1_ROUNDED_RECT{ D2D1::RectF(sx - 3, sy - 3, sx + sw + 3, sy + sh + 3), 10, 10 },
                g_brSel.get(), (i == selZone) ? 4.0f : 2.0f);
        }
        float barH = std::max(ZONE_BAR * g_cam.zoom, 22.0f); // başlık çubuğu - min okunur yükseklik
        D2D1_RECT_F bar = D2D1::RectF(sx, sy, sx + sw, sy + barH);
        D2D1_ROUNDED_RECT barr{ bar, 8, 8 };
        col.a = 0.9f; g_brNote->SetColor(col); g_d2dRT->FillRoundedRectangle(barr, g_brNote.get());
        std::wstring disp = z.title;
        if (editing) disp += L"_";
        if (!disp.empty())
            g_d2dRT->DrawText(disp.c_str(), (UINT32)disp.size(), g_textFmtL.get(),
                D2D1::RectF(sx + 12, sy, sx + sw - 52, sy + barH), g_brPanelBg.get());
        // --- başlık-çubuğu hover kontrolleri: ekran-sabit ---
        if (barHov && sw > 90)
        {
            D2D1_RECT_F xr = D2D1::RectF(sx + sw - 28, sy + 5, sx + sw - 6, sy + 27);
            D2D1_ROUNDED_RECT xrr{ xr, 6, 6 };
            g_d2dRT->FillRoundedRectangle(xrr, g_brBg.get());
            bool xh = cur.x >= xr.left && cur.x <= xr.right && cur.y >= xr.top && cur.y <= xr.bottom;
            if (xh) g_d2dRT->DrawRoundedRectangle(xrr, g_brHover.get(), 2.0f);
            g_d2dRT->DrawText(L"✕", 1, g_textFmt.get(), xr, g_brText.get());
            g_zoneDelRect = xr; g_zoneDelIdx = i;
            // M56: ⊞ düzenle butonu (✕'in solunda) - içindeki pencereleri ızgaraya diz
            if (sw > 120)
            {
                D2D1_RECT_F ar = D2D1::RectF(sx + sw - 54, sy + 5, sx + sw - 32, sy + 27);
                D2D1_ROUNDED_RECT arr{ ar, 6, 6 };
                g_d2dRT->FillRoundedRectangle(arr, g_brBg.get());
                bool ah = cur.x >= ar.left && cur.x <= ar.right && cur.y >= ar.top && cur.y <= ar.bottom;
                if (ah) g_d2dRT->DrawRoundedRectangle(arr, g_brHover.get(), 2.0f);
                g_d2dRT->DrawText(L"⊞", 1, g_textFmt.get(), ar, g_brText.get());
                g_zoneArrRect = ar; g_zoneArrIdx = i;
            }
        }
        // boyut tutamağı (sağ-alt köşe) - hover değil, her zaman tıklanabilir (gövde geçirgen)
        if (sw > 90 && sh > 60)
        {
            D2D1_RECT_F gr = D2D1::RectF(sx + sw - 20, sy + sh - 20, sx + sw - 2, sy + sh - 2);
            bool gh = (i == g_resizeZone) || (cur.x >= gr.left && cur.x <= gr.right &&
                       cur.y >= gr.top && cur.y <= gr.bottom);
            g_d2dRT->DrawLine(D2D1::Point2F(gr.left + 4, gr.bottom), D2D1::Point2F(gr.right, gr.top + 4),
                gh ? g_brHover.get() : g_brPanelBg.get(), 2.0f);
            g_d2dRT->DrawLine(D2D1::Point2F(gr.left + 11, gr.bottom), D2D1::Point2F(gr.right, gr.top + 11),
                gh ? g_brHover.get() : g_brPanelBg.get(), 2.0f);
            g_zoneResRect = gr; g_zoneResIdx = i;
        }
    }
    g_d2dRT->SetTransform(D2D1::Matrix3x2F::Identity());
}

// M44: notları dünya-uzayında çiz (tile'ların üstünde yüzer, panellerin altında).
// Gövde+metin dünya-ölçekli transform altında (zoom'la büyür); ✕ ekran-sabit.
static void DrawNotes()
{
    if (g_notes.empty()) { g_hoverNote = -1; g_noteDelIdx = -1; g_noteResIdx = -1; return; }
    POINT cur = g_curClient;
    float mwx = g_cam.x + cur.x / g_cam.zoom, mwy = g_cam.y + cur.y / g_cam.zoom;
    g_hoverNote = -1; g_noteDelIdx = -1; g_noteResIdx = -1;
    // dünya→ekran matrisi: p' = p*zoom + (-cam*zoom)
    D2D1_MATRIX_3X2_F world = D2D1::Matrix3x2F::Scale(g_cam.zoom, g_cam.zoom) *
        D2D1::Matrix3x2F::Translation(-g_cam.x * g_cam.zoom, -g_cam.y * g_cam.zoom);
    for (int i = 0; i < (int)g_notes.size(); i++)
    {
        Note& n = g_notes[i];
        float sx = (n.wx - g_cam.x) * g_cam.zoom, sy = (n.wy - g_cam.y) * g_cam.zoom;
        float sw = n.w * g_cam.zoom, sh = n.h * g_cam.zoom; // M46: per-note boyut
        if (sx > (float)g_sw || sy > (float)g_sh || sx + sw < 0 || sy + sh < 0) continue;
        bool hovered = (mwx >= n.wx && mwx <= n.wx + n.w && mwy >= n.wy && mwy <= n.wy + n.h);
        if (hovered) g_hoverNote = i;
        bool editing = (i == g_editNote);
        // M49: arama modunda eşleşmeyen not karartılır (tile'larla tutarlı)
        bool searchDim = g_searchOpen && !g_searchText.empty();
        bool noteMatch = !searchDim ||
            std::find(g_noteMatches.begin(), g_noteMatches.end(), i) != g_noteMatches.end();
        // --- gövde + metin: dünya-uzayında (zoom'la ölçeklenir) ---
        g_d2dRT->SetTransform(world);
        D2D1_RECT_F wr = D2D1::RectF(n.wx, n.wy, n.wx + n.w, n.wy + n.h);
        D2D1_ROUNDED_RECT rr{ wr, 8, 8 };
        g_brNote->SetColor(NoteColor(n.color));
        g_d2dRT->FillRoundedRectangle(rr, g_brNote.get());
        g_d2dRT->DrawRoundedRectangle(rr, editing ? g_brSel.get() : g_brPanelBg.get(),
            editing ? 3.0f : 1.0f);
        const float pad = 12.0f;
        std::wstring disp = n.text;
        if (editing) disp += L"_"; // caret (arama/palet ile tutarlı)
        if (!disp.empty())
            g_d2dRT->DrawText(disp.c_str(), (UINT32)disp.size(), g_textFmtN.get(),
                D2D1::RectF(n.wx + pad, n.wy + pad, n.wx + n.w - pad, n.wy + n.h - pad),
                g_brPanelBg.get());
        if (!noteMatch) g_d2dRT->FillRoundedRectangle(rr, g_brBg.get()); // M49: eşleşmeyeni karart
        g_d2dRT->SetTransform(D2D1::Matrix3x2F::Identity());
        // M49: arama eşleşmesi - amber halka (seçili olan daha kalın)
        if (searchDim && noteMatch)
        {
            int selNote = (g_searchSel >= (int)g_matches.size() &&
                g_searchSel - (int)g_matches.size() < (int)g_noteMatches.size())
                ? g_noteMatches[g_searchSel - (int)g_matches.size()] : -1;
            bool isSel = (i == selNote);
            g_d2dRT->DrawRectangle(D2D1::RectF(sx - 3, sy - 3, sx + sw + 3, sy + sh + 3),
                g_brSel.get(), isSel ? 4.0f : 2.0f);
        }
        // --- hover kontrolleri: ekran-sabit boyut, tıklanabilir ---
        if (hovered && sw > 80 && sh > 44)
        {
            // ✕ sil (sağ-üst)
            D2D1_RECT_F xr = D2D1::RectF(sx + sw - 28, sy + 6, sx + sw - 6, sy + 28);
            D2D1_ROUNDED_RECT xrr{ xr, 6, 6 };
            g_d2dRT->FillRoundedRectangle(xrr, g_brBg.get());
            bool xh = cur.x >= xr.left && cur.x <= xr.right && cur.y >= xr.top && cur.y <= xr.bottom;
            if (xh) g_d2dRT->DrawRoundedRectangle(xrr, g_brHover.get(), 2.0f);
            g_d2dRT->DrawText(L"✕", 1, g_textFmt.get(), xr, g_brText.get());
            g_noteDelRect = xr; g_noteDelIdx = i;
            // M46: boyut tutamağı (sağ-alt köşe) - sürükle = yeniden boyutlandır
            D2D1_RECT_F gr = D2D1::RectF(sx + sw - 18, sy + sh - 18, sx + sw - 2, sy + sh - 2);
            bool gh = (i == g_resizeNote) || (cur.x >= gr.left && cur.x <= gr.right &&
                       cur.y >= gr.top && cur.y <= gr.bottom);
            g_d2dRT->DrawLine(D2D1::Point2F(gr.left + 4, gr.bottom), D2D1::Point2F(gr.right, gr.top + 4),
                gh ? g_brHover.get() : g_brPanelBg.get(), 2.0f);
            g_d2dRT->DrawLine(D2D1::Point2F(gr.left + 10, gr.bottom), D2D1::Point2F(gr.right, gr.top + 10),
                gh ? g_brHover.get() : g_brPanelBg.get(), 2.0f);
            g_noteResRect = gr; g_noteResIdx = i;
        }
    }
    g_d2dRT->SetTransform(D2D1::Matrix3x2F::Identity());
}

// M57: bağlayıcı okları çiz (tile-merkezleri arası teal çizgi + ok başı). Ölü
// bağlantıları (kapanan pencere) temizler; Ctrl+sürükle sırasında amber rubber-band.
static void DrawConnectors()
{
    g_connDelIdx = -1;
    if (g_connectors.empty() && !g_connecting) return;
    POINT cur = g_curClient;
    auto boxOf = [&](HWND h, float& cx, float& cy, float& hw, float& hh) -> bool {
        for (auto& t : g_tiles) if (t.source == h && !t.pinnedFlag) {
            cx = t.wx + t.ww / 2; cy = t.wy + t.wh / 2; hw = t.ww / 2; hh = t.wh / 2; return true; }
        return false;
    };
    // M63: bir kutu merkezinden hedefe doğru, kutu KENARINDAki çıkış noktası
    auto edgePoint = [](float cx, float cy, float hw, float hh, float tx, float ty, float& ex, float& ey) {
        float dx = tx - cx, dy = ty - cy;
        float sx = (fabsf(dx) > 1e-4f) ? hw / fabsf(dx) : 1e9f;
        float sy = (fabsf(dy) > 1e-4f) ? hh / fabsf(dy) : 1e9f;
        float s = std::min(std::min(sx, sy), 1.0f); // kutu içinde kal (örtüşme degenere)
        ex = cx + dx * s; ey = cy + dy * s;
    };
    // ölü bağlantıları temizle (HWND artık canlı tile değil)
    float t1, t2, t3, t4;
    g_connectors.erase(std::remove_if(g_connectors.begin(), g_connectors.end(),
        [&](const Connector& c){ return !boxOf(c.a, t1, t2, t3, t4) || !boxOf(c.b, t1, t2, t3, t4); }),
        g_connectors.end());
    for (int i = 0; i < (int)g_connectors.size(); i++)
    {
        float cax, cay, hwa, hha, cbx, cby, hwb, hhb;
        if (!boxOf(g_connectors[i].a, cax, cay, hwa, hha) ||
            !boxOf(g_connectors[i].b, cbx, cby, hwb, hhb)) continue;
        float ax, ay, bx, by; // M63: kenar-kenara (çizgi pencerelerin içinden geçmez)
        edgePoint(cax, cay, hwa, hha, cbx, cby, ax, ay);
        edgePoint(cbx, cby, hwb, hhb, cax, cay, bx, by);
        float sax = (ax - g_cam.x) * g_cam.zoom, say = (ay - g_cam.y) * g_cam.zoom;
        float sbx = (bx - g_cam.x) * g_cam.zoom, sby = (by - g_cam.y) * g_cam.zoom;
        float mx = (sax + sbx) / 2, my = (say + sby) / 2; // orta nokta (✕ + hover)
        bool chov = fabsf((float)cur.x - mx) < 13 && fabsf((float)cur.y - my) < 13; // M66
        float lw = chov ? 3.8f : 2.5f; // M66: hover'da kalın+parlak (hangi çizgi silinecek belli)
        g_brPick->SetOpacity(chov ? 1.0f : 0.9f);
        g_d2dRT->DrawLine(D2D1::Point2F(sax, say), D2D1::Point2F(sbx, sby), g_brPick.get(), lw);
        float dx = sbx - sax, dy = sby - say, len = sqrtf(dx * dx + dy * dy);
        if (len > 1) { dx /= len; dy /= len; const float ah = 13.0f; // ok başı (b ucunda)
            D2D1_POINT_2F tip{ sbx, sby };
            g_d2dRT->DrawLine(tip, D2D1::Point2F(sbx - dx * ah - dy * ah * 0.55f, sby - dy * ah + dx * ah * 0.55f), g_brPick.get(), lw);
            g_d2dRT->DrawLine(tip, D2D1::Point2F(sbx - dx * ah + dy * ah * 0.55f, sby - dy * ah - dx * ah * 0.55f), g_brPick.get(), lw);
        }
        g_brPick->SetOpacity(1.0f);
        if (chov) // orta nokta hover ✕ (sil)
        {
            D2D1_RECT_F xr = D2D1::RectF(mx - 11, my - 11, mx + 11, my + 11);
            D2D1_ROUNDED_RECT xrr{ xr, 6, 6 };
            g_d2dRT->FillRoundedRectangle(xrr, g_brBg.get());
            g_d2dRT->DrawRoundedRectangle(xrr, g_brHover.get(), 2.0f);
            g_d2dRT->DrawText(L"✕", 1, g_textFmt.get(), xr, g_brText.get());
            g_connDelRect = xr; g_connDelIdx = i;
        }
    }
    if (g_connecting && g_connectFrom) // rubber-band
    {
        float ax, ay, hw, hh;
        if (boxOf(g_connectFrom, ax, ay, hw, hh))
        {
            float sax = (ax - g_cam.x) * g_cam.zoom, say = (ay - g_cam.y) * g_cam.zoom;
            g_brSel->SetOpacity(0.8f);
            g_d2dRT->DrawLine(D2D1::Point2F(sax, say), D2D1::Point2F((float)cur.x, (float)cur.y), g_brSel.get(), 2.5f);
            g_brSel->SetOpacity(1.0f);
        }
    }
}

// Etiketler + hover vurgusu (D3D çiziminden sonra, Present'tan önce)
static void DrawOverlay()
{
    if (!g_d2dRT || g_activeTile >= 0) return;
    g_d2dRT->BeginDraw();
    g_releaseTile = -1; // her kare yeniden hesaplanır
    g_searchBtnRect = D2D1::RectF(0, 0, 0, 0); // M11: buton çizilirse dolar
    // M19: arama/seçim fırçaları cache'li (brDim = g_brBg ile aynı: 0,0,0,0.55)
    bool searching = g_searchOpen && !g_searchText.empty();
    POINT cur = g_curClient; // M19: kare-başı tek okuma (ana döngüde güncellenir)
    int hov = HitTile(g_cam.x + cur.x / g_cam.zoom, g_cam.y + cur.y / g_cam.zoom);
    if (OverPanelUi(cur)) hov = -1; // panel altındaki tile'a vurgu/çip verme
    for (int i = 0; i < (int)g_tiles.size(); i++)
    {
        Tile& t = g_tiles[i];
        if (t.pinnedFlag) continue; // M22: pinned ayrı pass'te (ekran-uzayı)
        float sx = (t.wx - g_cam.x) * g_cam.zoom;
        float sy = (t.wy - g_cam.y) * g_cam.zoom;
        float sw = t.ww * g_cam.zoom, sh = t.wh * g_cam.zoom;
        if (sx > (float)g_sw || sy > (float)g_sh || sx + sw < 0 || sy + sh < 0)
            continue;
        if (searching) // M9: eşleşmeyenleri karart, eşleşenleri çerçevele
        {
            bool isMatch = std::find(g_matches.begin(), g_matches.end(), i)
                != g_matches.end();
            bool isSel = isMatch && !g_matches.empty() && g_matches[g_searchSel] == i;
            if (!isMatch)
                g_d2dRT->FillRectangle(D2D1::RectF(sx, sy, sx + sw, sy + sh), g_brBg.get());
            else
                g_d2dRT->DrawRectangle(
                    D2D1::RectF(sx - 3, sy - 3, sx + sw + 3, sy + sh + 3),
                    isSel ? g_brSel.get() : g_brHover.get(), isSel ? 4.0f : 2.0f);
        }
        if (g_set.hover && i == hov)
            g_d2dRT->DrawRectangle(
                D2D1::RectF(sx - 2, sy - 2, sx + sw + 2, sy + sh + 2),
                g_brHover.get(), 3.0f);
        // M11: seçili tile çerçevesi (teal)
        if (g_selSet.count(t.source))
            g_d2dRT->DrawRectangle(
                D2D1::RectF(sx - 3, sy - 3, sx + sw + 3, sy + sh + 3),
                g_brPick.get(), 3.0f);
        // M21: klavye odak halkası (amber, en dışta)
        if (t.source == g_focusWnd)
            g_d2dRT->DrawRectangle(
                D2D1::RectF(sx - 5, sy - 5, sx + sw + 5, sy + sh + 5),
                g_brSel.get(), 2.5f);
        // M6: serbest bırak çipi (hover'da, tile yeterince büyükse)
        if (i == hov && sw > 120 && sh > 60)
        {
            D2D1_RECT_F xr = D2D1::RectF(sx + sw - 34, sy + 6, sx + sw - 6, sy + 34);
            bool xrHov = (cur.x >= xr.left && cur.x <= xr.right &&
                          cur.y >= xr.top && cur.y <= xr.bottom);
            D2D1_ROUNDED_RECT xrr{ xr, 7, 7 };
            g_d2dRT->FillRoundedRectangle(xrr, g_brBg.get());
            if (xrHov) g_d2dRT->DrawRoundedRectangle(xrr, g_brHover.get(), 2.0f);
            g_d2dRT->DrawText(L"\u2715", 1, g_textFmt.get(), xr, g_brText.get());
            g_releaseRect = xr;
            g_releaseTile = i;
        }
        // M37: etiket uzakta (zoom<0.7) otomatik VEYA hover'da her zaman görünür
        // (yakından bile "bu hangi pencere" - keşfedilebilirlik)
        if (!t.title.empty() && sw > 40 &&
            ((g_set.labels && g_cam.zoom < 0.7f) || i == hov))
        {
            float lw = std::min(std::max(sw, 180.0f), 460.0f); // hover'da min okunur genişlik
            D2D1_RECT_F bg = D2D1::RectF(sx, sy - 32, sx + lw, sy - 6);
            D2D1_ROUNDED_RECT rr{ bg, 6, 6 };
            g_d2dRT->FillRoundedRectangle(rr, g_brBg.get());
            // M41: etikette exe ikonu (başlık okumadan görsel tanıma)
            ID2D1Bitmap* ico = IconToBitmap(t.icon, t.icoBmp);
            float tx = bg.left + 8;
            if (ico)
            {
                g_d2dRT->DrawBitmap(ico, D2D1::RectF(bg.left + 5, bg.top + 4, bg.left + 25, bg.top + 24));
                tx = bg.left + 30;
            }
            g_d2dRT->DrawText(t.title.c_str(), (UINT32)t.title.size(), g_textFmtL.get(),
                D2D1::RectF(tx, bg.top, bg.right - 6, bg.bottom), g_brText.get());
        }
    }
    DrawConnectors(); // M57: bağlayıcı oklar (tile'ların üstünde)
    DrawZones(); // M54: bölgeler (notların altında, tile'ların üstünde - kenar+başlık)
    DrawNotes(); // M44: notlar tile'ların üstünde, paneller/marquee altında
    // M11: marquee seçim dikdörtgeni
    if (g_marquee && g_brPick)
    {
        float mL = (std::min(g_marqAX, g_marqBX) - g_cam.x) * g_cam.zoom;
        float mT = (std::min(g_marqAY, g_marqBY) - g_cam.y) * g_cam.zoom;
        float mR = (std::max(g_marqAX, g_marqBX) - g_cam.x) * g_cam.zoom;
        float mB = (std::max(g_marqAY, g_marqBY) - g_cam.y) * g_cam.zoom;
        g_brPick->SetOpacity(0.12f);
        g_d2dRT->FillRectangle(D2D1::RectF(mL, mT, mR, mB), g_brPick.get());
        g_brPick->SetOpacity(1.0f);
        g_d2dRT->DrawRectangle(D2D1::RectF(mL, mT, mR, mB), g_brPick.get(), 1.5f);
    }
    // M11: üst-orta Ara butonu (tıklama işleme WM_LBUTTONDOWN'da)
    if (!g_searchOpen)
    {
        float cx = g_uiX + g_priW / 2.0f;
        D2D1_RECT_F br = D2D1::RectF(cx - 72, g_uiY + 14, cx + 72, g_uiY + 46);
        bool bHov = (cur.x >= br.left && cur.x <= br.right &&
                     cur.y >= br.top && cur.y <= br.bottom);
        D2D1_ROUNDED_RECT brr{ br, 16, 16 };
        g_d2dRT->FillRoundedRectangle(brr, g_brBg.get());
        if (bHov) g_d2dRT->DrawRoundedRectangle(brr, g_brHover.get(), 2.0f);
        std::wstring bt = TL(L"Search  ", L"Ara  ") + HotkeyName(g_set.kbSearch);
        g_d2dRT->DrawText(bt.c_str(), (UINT32)bt.size(), g_textFmt.get(),
            D2D1::RectF(br.left + 20, br.top + 6, br.right - 8, br.bottom),
            g_brText.get());
        g_searchBtnRect = br;
    }
    // M9: arama kutusu (ana ekran üst-orta)
    if (g_searchOpen)
    {
        float cx = g_uiX + g_priW / 2.0f;
        D2D1_RECT_F box = D2D1::RectF(cx - 220, g_uiY + 18, cx + 220, g_uiY + 62);
        D2D1_ROUNDED_RECT rb{ box, 10, 10 };
        g_d2dRT->FillRoundedRectangle(rb, g_brPanelBg.get()); // M19: cache'li (0.93α)
        g_d2dRT->DrawRoundedRectangle(rb, g_brSel.get(), 1.5f);
        std::wstring st = TL(L"Search: ", L"Ara: ") + g_searchText + L"_";
        if (searching)
        {
            int total = (int)(g_matches.size() + g_noteMatches.size() + g_zoneMatches.size()); // M49/M67
            st += L"   (" + std::to_wstring(total == 0 ? 0 : g_searchSel + 1)
                + L"/" + std::to_wstring(total) + L")";
            if (!g_noteMatches.empty()) // not eşleşmesi varsa belirt
                st += L"  " + std::to_wstring((int)g_noteMatches.size()) + TL(L" note", L" not");
            if (!g_zoneMatches.empty()) // M67: bölge eşleşmesi
                st += L"  " + std::to_wstring((int)g_zoneMatches.size()) + TL(L" zone", L" bölge");
        }
        g_d2dRT->DrawText(st.c_str(), (UINT32)st.size(), g_textFmtL.get(),
            D2D1::RectF(box.left + 16, box.top, box.right - 12, box.bottom), g_brText.get());
    }
    // M22: pinned tile overlay'i (ekran-uzayı) - hover halkası + pin rozeti
    for (auto& t : g_tiles)
    {
        if (!t.pinnedFlag) continue;
        bool hov = (cur.x >= t.px && cur.x <= t.px + t.pw &&
                    cur.y >= t.py && cur.y <= t.py + t.ph);
        if (g_set.hover && hov)
            g_d2dRT->DrawRectangle(
                D2D1::RectF(t.px - 2, t.py - 2, t.px + t.pw + 2, t.py + t.ph + 2),
                g_brHover.get(), 3.0f);
        // pin rozeti (sol üst köşe) - sabitlenmiş olduğunu belli eder.
        // Emoji yerine teal dolu daire: her fontta garantili render olur.
        D2D1_ELLIPSE pin{ D2D1::Point2F(t.px + 16, t.py + 16), 7, 7 };
        g_d2dRT->FillEllipse(pin, g_brSel.get());
        g_d2dRT->DrawEllipse(pin, g_brPanelBg.get(), 2.0f);
    }
    // M45: boş tuval onboarding ipucu (pencere ve not yokken - ilk izlenim/demo).
    // İlk-açılış kartı, panel/arama/başlatıcı/yardım kapalıyken merkezde soluk.
    if (g_tiles.empty() && g_notes.empty() && !g_firstRun && !g_helpOpen &&
        !g_searchOpen && !g_launchOpen && g_panelA < 0.02f)
    {
        const wchar_t* hint = TL(
            L"Canvas is empty\n\n"
            L"Right edge → launch app   ·   Ctrl+N: launcher palette\n"
            L"Double-click / Ctrl+Shift+N: note   ·   Ctrl+Shift+Z: zone\n"
            L"Ctrl+drag between windows: link   ·   F1: all shortcuts",
            L"Tuval boş\n\n"
            L"Sağ kenar → uygulama başlat   ·   Ctrl+N: başlatıcı palet\n"
            L"Çift tık / Ctrl+Shift+N: not   ·   Ctrl+Shift+Z: bölge\n"
            L"Pencereler arası Ctrl+sürükle: bağla   ·   F1: kısayollar");
        winrt::com_ptr<ID2D1SolidColorBrush> hb;
        g_d2dRT->CreateSolidColorBrush(D2D1::ColorF(0.62f, 0.66f, 0.74f, 0.55f), hb.put());
        float cx = g_uiX + g_priW / 2.0f, cy = g_uiY + g_priH / 2.0f;
        g_d2dRT->DrawText(hint, (UINT32)wcslen(hint), g_textFmt.get(),
            D2D1::RectF(cx - 380, cy - 70, cx + 380, cy + 70), hb.get());
    }
    DrawDock(cur);     // M13: alt dock (çalışan pencereler)
    DrawAppDock(cur);  // M24: sağ dock (launcher.txt uygulamaları)
    DrawMinimap();     // M36: sağ-alt kuşbakışı minimap
    // M17: toast - dock balonunun üstünde (g_priH-126'ya kadar uzanır)
    if (!g_toast.empty())
    {
        ULONGLONG tdt = GetTickCount64() - g_toastTick;
        if (tdt < 1600)
        {
            float a = tdt > 1200 ? 1.0f - (tdt - 1200) / 400.0f : 1.0f;
            float tcx = g_uiX + g_priW / 2.0f;
            float tw2 = (40.0f + (float)g_toast.size() * 8.5f) / 2.0f;
            float yb = g_uiY + (float)g_priH - 150.0f;
            D2D1_RECT_F trc = D2D1::RectF(tcx - tw2, yb - 36, tcx + tw2, yb);
            winrt::com_ptr<ID2D1SolidColorBrush> tb, tt;
            g_d2dRT->CreateSolidColorBrush(
                D2D1::ColorF(0.06f, 0.065f, 0.09f, 0.93f * a), tb.put());
            g_d2dRT->CreateSolidColorBrush(
                D2D1::ColorF(0.92f, 0.93f, 0.95f, a), tt.put());
            D2D1_ROUNDED_RECT trr{ trc, 10, 10 };
            g_d2dRT->FillRoundedRectangle(trr, tb.get());
            g_d2dRT->DrawText(g_toast.c_str(), (UINT32)g_toast.size(),
                g_textFmt.get(), trc, tt.get());
        }
    }
    DrawPanel(cur); // M5: dişli + ayarlar paneli en üstte
    // M23: uygulama başlatıcı paleti (üst-orta, input + kısayol listesi)
    if (g_launchOpen)
    {
        float cx = g_uiX + g_priW / 2.0f;
        int n = (int)g_launchers.size();
        float rowH = 34.0f;
        float h = 64.0f + n * rowH + 16.0f;
        D2D1_RECT_F box = D2D1::RectF(cx - 280, g_uiY + 70, cx + 280, g_uiY + 70 + h);
        g_launchBox = box;
        g_launchRows.clear();
        D2D1_ROUNDED_RECT rb{ box, 12, 12 };
        g_d2dRT->FillRoundedRectangle(rb, g_brPanelBg.get());
        g_d2dRT->DrawRoundedRectangle(rb, g_brSel.get(), 1.5f);
        // giriş satırı
        std::wstring st = TL(L"Run:  ", L"Çalıştır:  ") + g_launchText + L"_";
        g_d2dRT->DrawText(st.c_str(), (UINT32)st.size(), g_textFmtL.get(),
            D2D1::RectF(box.left + 18, box.top + 14, box.right - 14, box.top + 50),
            g_brText.get());
        g_d2dRT->DrawLine(D2D1::Point2F(box.left + 14, box.top + 56),
            D2D1::Point2F(box.right - 14, box.top + 56), g_brHover.get(), 1.0f);
        // kısayol listesi (boş girişte rakam ipucu + seçili vurgu)
        float y = box.top + 64;
        for (int i = 0; i < n; i++)
        {
            D2D1_RECT_F row = D2D1::RectF(box.left + 10, y, box.right - 10, y + rowH - 4);
            bool rowHov = (cur.x >= row.left && cur.x <= row.right &&
                           cur.y >= row.top && cur.y <= row.bottom);
            if (g_launchText.empty() && (i == g_launchSel || rowHov))
            {
                D2D1_ROUNDED_RECT hr{ row, 7, 7 };
                g_d2dRT->FillRoundedRectangle(hr, g_brBg.get());
                g_d2dRT->DrawRoundedRectangle(hr, g_brHover.get(), 1.0f);
            }
            std::wstring num = (i < 9) ? (std::to_wstring(i + 1) + L"  ") : L"   ";
            std::wstring lab = num + g_launchers[i].label;
            g_d2dRT->DrawText(lab.c_str(), (UINT32)lab.size(), g_textFmtL.get(),
                D2D1::RectF(row.left + 14, row.top, row.right - 12, row.bottom),
                g_brText.get());
            g_launchRows.push_back(row);
            y += rowH;
        }
        std::wstring hint = TL(L"App / command (fit · grid · quit)   ·   1-9 or click", L"Uygulama / komut (fit · grid · quit)   ·   1-9 ya da tıkla");
        g_d2dRT->DrawText(hint.c_str(), (UINT32)hint.size(), g_textFmtL.get(),
            D2D1::RectF(box.left + 18, box.bottom - 22, box.right - 14, box.bottom - 2),
            g_brHover.get());
    }
    // M20: ilk açılış ipucu kartı (her şeyin üstünde, herhangi bir girdiyle kapanır)
    if (g_firstRun)
    {
        float cx = g_uiX + g_priW / 2.0f, cy = g_uiY + g_priH / 2.0f;
        D2D1_RECT_F card = D2D1::RectF(cx - 360, cy - 90, cx + 360, cy + 90);
        D2D1_ROUNDED_RECT cr{ card, 16, 16 };
        g_d2dRT->FillRoundedRectangle(cr, g_brPanelBg.get());
        g_d2dRT->DrawRoundedRectangle(cr, g_brHover.get(), 1.5f);
        std::wstring l1 = TL(L"Right-drag: pan   ·   Ctrl+Alt+Wheel: zoom   ·   Double-click: dive into window",
            L"Sağ tık sürükle: kaydır   ·   Ctrl+Alt+Tekerlek: yakınlaş   ·   Çift tık: pencereye dal");
        std::wstring l2 = HotkeyName(g_set.kbFit) + TL(L": fit   ·   ", L": sığdır   ·   ") +
            HotkeyName(g_set.kbSearch) + TL(L": search   ·   ", L": ara   ·   ") +
            HotkeyName(g_set.kbExit) + TL(L": exit", L": çıkış");
        std::wstring l3 = HotkeyName(g_set.kbLaunch) +
            TL(L": launch app   ·   F1: all shortcuts", L": uygulama başlat   ·   F1: tüm kısayollar");
        g_d2dRT->DrawText(l1.c_str(), (UINT32)l1.size(), g_textFmt.get(),
            D2D1::RectF(card.left + 20, cy - 54, card.right - 20, cy - 24), g_brText.get());
        g_d2dRT->DrawText(l2.c_str(), (UINT32)l2.size(), g_textFmt.get(),
            D2D1::RectF(card.left + 20, cy - 14, card.right - 20, cy + 16), g_brText.get());
        g_d2dRT->DrawText(l3.c_str(), (UINT32)l3.size(), g_textFmt.get(),
            D2D1::RectF(card.left + 20, cy + 26, card.right - 20, cy + 56), g_brHover.get());
    }
    // M20: F1 kısayol listesi - tam ekran karartma + iki sütun
    if (g_helpOpen)
    {
        winrt::com_ptr<ID2D1SolidColorBrush> dim;
        g_d2dRT->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.62f), dim.put());
        g_d2dRT->FillRectangle(
            D2D1::RectF(g_uiX, g_uiY, g_uiX + g_priW, g_uiY + g_priH), dim.get());
        float lx = g_uiX + g_priW / 2.0f - 360, rx = g_uiX + g_priW / 2.0f + 20;
        float ty = g_uiY + g_priH / 2.0f - 220;
        const wchar_t* helpTitle = TL(L"Shortcuts", L"Kısayollar");
        g_d2dRT->DrawText(helpTitle, (UINT32)wcslen(helpTitle), g_textFmt.get(),
            D2D1::RectF(g_uiX, ty - 44, g_uiX + g_priW, ty - 8), g_brHover.get());
        std::wstring colA = g_set.lang == 1 ?
            std::wstring(L"Geri çekil:  ") + HotkeyName(g_set.kbPull) + L"\n"
            L"Panel:  " + HotkeyName(g_set.kbPanel) + L"\n"
            L"Sığdır:  " + HotkeyName(g_set.kbFit) + L"\n"
            L"Ara:  " + HotkeyName(g_set.kbSearch) + L"\n"
            L"Uygulama başlat:  " + HotkeyName(g_set.kbLaunch) + L"\n"
            L"Çıkış:  " + HotkeyName(g_set.kbExit) + L"\n"
            L"Çift tık:  pencereye dal\n"
            L"Boş alanda sürükle:  çoklu seçim\n"
            L"Alt kenar:  uygulama dock'u"
          :
            std::wstring(L"Pull back:  ") + HotkeyName(g_set.kbPull) + L"\n"
            L"Panel:  " + HotkeyName(g_set.kbPanel) + L"\n"
            L"Fit:  " + HotkeyName(g_set.kbFit) + L"\n"
            L"Search:  " + HotkeyName(g_set.kbSearch) + L"\n"
            L"Launch app:  " + HotkeyName(g_set.kbLaunch) + L"\n"
            L"Exit:  " + HotkeyName(g_set.kbExit) + L"\n"
            L"Double-click:  dive into window\n"
            L"Drag empty space:  multi-select\n"
            L"Bottom edge:  app dock";
        std::wstring colB = g_set.lang == 1 ?
            L"Ctrl+A:  tüm pencereleri seç\n"
            L"Ctrl+C / Ctrl+V:  pencere çoğalt\n"
            L"Ctrl+P:  tile'ı ekrana sabitle (HUD)\n"
            L"Ctrl+G:  pencereleri ızgaraya diz\n"
            L"Ctrl+Shift+N:  yapışkan not (Tab=renk)\n"
            L"Ctrl+Shift+Z:  bölge çerçevesi (başlıktan sürükle)\n"
            L"Ctrl+Shift+S:  tuvali PNG'ye aktar\n"
            L"Delete:  seçilileri çıkar\n"
            L"Ctrl+Shift+1..4:  yer imi kaydet\n"
            L"Ctrl+1..4:  yer imine git\n"
            L"Shift+sürükle:  yapışık kümeyi taşı\n"
            L"Ctrl+sürükle (tile→tile):  bağlayıcı ok\n"
            L"Alt+sürükle:  yapışma kapalı\n"
            L"Ok tuşları:  tile'lar arası odak · Shift+Ok: taşı\n"
            L"Shift+1 / Shift+2:  tümünü / seçimi sığdır\n"
            L"F1 / ESC:  bu listeyi kapat"
          :
            L"Ctrl+A:  select all windows\n"
            L"Ctrl+C / Ctrl+V:  duplicate window\n"
            L"Ctrl+P:  pin tile to screen (HUD)\n"
            L"Ctrl+G:  arrange windows into grid\n"
            L"Ctrl+Shift+N:  sticky note (Tab=color)\n"
            L"Ctrl+Shift+Z:  zone frame (drag the title bar)\n"
            L"Ctrl+Shift+S:  export canvas to PNG\n"
            L"Delete:  remove selected\n"
            L"Ctrl+Shift+1..4:  save bookmark\n"
            L"Ctrl+1..4:  go to bookmark\n"
            L"Shift+drag:  move snapped cluster\n"
            L"Ctrl+drag (tile→tile):  connector arrow\n"
            L"Alt+drag:  snapping off\n"
            L"Arrow keys:  focus between tiles · Shift+Arrow: move\n"
            L"Shift+1 / Shift+2:  fit all / selection\n"
            L"F1 / ESC:  close this list";
        g_d2dRT->DrawText(colA.c_str(), (UINT32)colA.size(), g_textFmtL.get(),
            D2D1::RectF(lx, ty, lx + 340, ty + 400), g_brText.get());
        g_d2dRT->DrawText(colB.c_str(), (UINT32)colB.size(), g_textFmtL.get(),
            D2D1::RectF(rx, ty, rx + 340, ty + 400), g_brText.get());
    }
    if (FAILED(g_d2dRT->EndDraw())) g_d2dRT = nullptr;
}

// M12: dünyaya çakılı nokta ızgara - pan/zoom hissini güçlendirir.
// Tile çiziminden ÖNCE ayrı D2D pass (noktalar tile'ların altında kalır).
static void DrawGrid()
{
    if (!g_d2dRT || g_activeTile >= 0) return;
    // M29: vinyet zemin ızgaradan bağımsız; ızgara kapalı olsa da çizilir
    bool wantGrid = g_set.grid && g_gridBrush;
    bool wantVignette = g_set.bgPreset == 3 && g_bgRadial;
    if (!wantGrid && !wantVignette) return;
    float worldStep = 64.0f;
    float sp = worldStep * g_cam.zoom;
    while (sp < 34.0f)  { worldStep *= 2.0f; sp = worldStep * g_cam.zoom; }
    while (sp > 136.0f) { worldStep *= 0.5f; sp = worldStep * g_cam.zoom; }
    // sık nokta = soluk, seyrek nokta = belirgin (seviye geçişi yumuşar)
    float a = 0.05f + 0.09f * std::clamp((sp - 34.0f) / 102.0f, 0.0f, 1.0f);
    g_d2dRT->BeginDraw();
    if (wantVignette) // M29: prosedürel radyal vinyet
        g_d2dRT->FillRectangle(D2D1::RectF(0, 0, (float)g_sw, (float)g_sh), g_bgRadial.get());
    if (wantGrid) // M19: tek FillRectangle, wrap-mode bitmap brush
    {
        float x0 = (ceilf(g_cam.x / worldStep) * worldStep - g_cam.x) * g_cam.zoom;
        float y0 = (ceilf(g_cam.y / worldStep) * worldStep - g_cam.y) * g_cam.zoom;
        // 64px dokuyu sp'ye ölçekle; -1px*ölçek ofset noktayı ızgara üstüne ortalar
        g_gridBrush->SetTransform(
            D2D1::Matrix3x2F::Scale(sp / 64.0f, sp / 64.0f) *
            D2D1::Matrix3x2F::Translation(x0 - sp / 64.0f, y0 - sp / 64.0f));
        g_gridBrush->SetOpacity(a);
        g_d2dRT->FillRectangle(
            D2D1::RectF(0, 0, (float)g_sw, (float)g_sh), g_gridBrush.get());
    }
    if (FAILED(g_d2dRT->EndDraw())) g_d2dRT = nullptr;
}

// ---- M3: Layout kalıcılığı ----
static std::wstring LayoutFilePath()
{
    std::wstring p = PendingFilePath();
    return p.substr(0, p.find_last_of(L'\\')) + L"\\layout.txt";
}

// ---- M44/M46: tuval notları kalıcılığı (notes.txt: wx|wy|w|h|renk|metin;
//      M44 eski formatı wx|wy|renk|metin de okunur - geri uyumlu) ----
static std::wstring NotesFilePath()
{
    std::wstring p = PendingFilePath();
    return p.substr(0, p.find_last_of(L'\\')) + L"\\notes.txt";
}

static void SaveNotes()
{
    std::wofstream f(NotesFilePath(), std::ios::trunc);
    if (!f) return;
    int written = 0;
    for (auto& n : g_notes)
    {
        if (written++ >= 200) break;
        std::wstring t = n.text;
        for (auto& ch : t) if (ch == L'\n' || ch == L'\r') ch = L' '; // tek satır kaydı
        f << (int)n.wx << L"|" << (int)n.wy << L"|" << (int)n.w << L"|" << (int)n.h
          << L"|" << (n.color & 3) << L"|" << t << L"\n";
    }
}

static void LoadNotes()
{
    g_notes.clear();
    std::wifstream f(NotesFilePath());
    if (!f) return;
    std::wstring line;
    while (std::getline(f, line))
    {
        while (!line.empty() && line.back() == L'\r') line.pop_back();
        // alan ayraçlarını topla
        std::vector<size_t> b;
        for (size_t i = 0; i < line.size(); i++) if (line[i] == L'|') b.push_back(i);
        if (b.size() < 3) continue; // en az eski format (3 ayraç)
        Note n;
        n.wx = (float)_wtof(line.substr(0, b[0]).c_str());
        n.wy = (float)_wtof(line.substr(b[0] + 1, b[1] - b[0] - 1).c_str());
        if (b.size() >= 5) // M46 yeni format: wx|wy|w|h|renk|metin
        {
            n.w = (float)_wtof(line.substr(b[1] + 1, b[2] - b[1] - 1).c_str());
            n.h = (float)_wtof(line.substr(b[2] + 1, b[3] - b[2] - 1).c_str());
            n.color = _wtoi(line.substr(b[3] + 1, b[4] - b[3] - 1).c_str()) & 3;
            n.text = line.substr(b[4] + 1);
            n.w = std::clamp(n.w, NOTE_MIN_W, NOTE_MAX);
            n.h = std::clamp(n.h, NOTE_MIN_H, NOTE_MAX);
        }
        else // M44 eski format: wx|wy|renk|metin (w/h varsayılan)
        {
            n.color = _wtoi(line.substr(b[1] + 1, b[2] - b[1] - 1).c_str()) & 3;
            n.text = line.substr(b[2] + 1);
        }
        g_notes.push_back(n);
        if (g_notes.size() >= 200) break;
    }
}

// ---- M54: bölge/zon kalıcılığı (zones.txt: wx|wy|w|h|renk|başlık) ----
static std::wstring ZonesFilePath()
{
    std::wstring p = PendingFilePath();
    return p.substr(0, p.find_last_of(L'\\')) + L"\\zones.txt";
}

static void SaveZones()
{
    std::wofstream f(ZonesFilePath(), std::ios::trunc);
    if (!f) return;
    int written = 0;
    for (auto& z : g_zones)
    {
        if (written++ >= 100) break;
        std::wstring t = z.title;
        for (auto& ch : t) if (ch == L'\n' || ch == L'\r') ch = L' ';
        f << (int)z.wx << L"|" << (int)z.wy << L"|" << (int)z.w << L"|" << (int)z.h
          << L"|" << (z.color & 3) << L"|" << t << L"\n";
    }
}

static void LoadZones()
{
    g_zones.clear();
    std::wifstream f(ZonesFilePath());
    if (!f) return;
    std::wstring line;
    while (std::getline(f, line))
    {
        while (!line.empty() && line.back() == L'\r') line.pop_back();
        std::vector<size_t> b;
        for (size_t i = 0; i < line.size(); i++) if (line[i] == L'|') b.push_back(i);
        if (b.size() < 5) continue;
        Zone z;
        z.wx = (float)_wtof(line.substr(0, b[0]).c_str());
        z.wy = (float)_wtof(line.substr(b[0] + 1, b[1] - b[0] - 1).c_str());
        z.w = std::clamp((float)_wtof(line.substr(b[1] + 1, b[2] - b[1] - 1).c_str()), ZONE_MIN_W, 8000.0f);
        z.h = std::clamp((float)_wtof(line.substr(b[2] + 1, b[3] - b[2] - 1).c_str()), ZONE_MIN_H, 8000.0f);
        z.color = _wtoi(line.substr(b[3] + 1, b[4] - b[3] - 1).c_str()) & 3;
        z.title = line.substr(b[4] + 1);
        g_zones.push_back(z);
        if (g_zones.size() >= 100) break;
    }
}

// ---- M15: pencere kuralları (rules.txt - elle düzenlenir, açılışta okunur) ----
static std::wstring RulesFilePath()
{
    std::wstring p = PendingFilePath();
    return p.substr(0, p.find_last_of(L'\\')) + L"\\rules.txt";
}

static void LoadRules()
{
    g_ruleExclude.clear();
    g_ruleOpacity.clear(); // M28
    g_ruleBlur.clear();    // M34
    std::wstring path = RulesFilePath();
    std::wifstream f(path);
    if (!f) // ilk çalıştırma: açıklamalı şablon bırak
    {
        std::wofstream o(path);
        if (o)
        {
            o << L"# Spatial Canvas kurallari (kaydet, uygulamayi yeniden baslat)\n";
            o << L"# exclude=uygulama.exe -> bu exe hic yakalanmaz\n";
            o << L"# opacity=uygulama.exe:0.8 -> tuvalde %80 saydamlikta cizilir (0..1)\n";
            o << L"# blur=uygulama.exe:3 -> tuvalde bulaniklastirilir (yaricap 1..6)\n";
            o << L"# ornek: exclude=obs64.exe\n";
            o << L"# ornek: opacity=notepad.exe:0.6\n";
            o << L"# ornek: blur=discord.exe:3\n";
        }
        return;
    }
    std::wstring line;
    while (std::getline(f, line))
    {
        if (line.empty() || line[0] == L'#') continue;
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring k = line.substr(0, eq);
        std::wstring v = line.substr(eq + 1);
        while (!v.empty() && (v.back() == L' ' || v.back() == L'\r')) v.pop_back();
        if (k == L"exclude" && !v.empty())
        {
            std::transform(v.begin(), v.end(), v.begin(), ::towlower);
            g_ruleExclude.insert(v);
        }
        else if (k == L"opacity") // M28: exe:deger
        {
            size_t c = v.rfind(L':');
            if (c != std::wstring::npos)
            {
                std::wstring ex = v.substr(0, c);
                float val = std::clamp((float)_wtof(v.substr(c + 1).c_str()), 0.1f, 1.0f);
                std::transform(ex.begin(), ex.end(), ex.begin(), ::towlower);
                if (!ex.empty()) g_ruleOpacity[ex] = val;
            }
        }
        else if (k == L"blur") // M34: exe:yaricap (texel, 1..6)
        {
            size_t c = v.rfind(L':');
            if (c != std::wstring::npos)
            {
                std::wstring ex = v.substr(0, c);
                float val = std::clamp((float)_wtof(v.substr(c + 1).c_str()), 0.0f, 6.0f);
                std::transform(ex.begin(), ex.end(), ex.begin(), ::towlower);
                if (!ex.empty() && val >= 1.0f) g_ruleBlur[ex] = val;
            }
        }
    }
}

// ---- M23: uygulama başlatıcı (launcher.txt - elle düzenlenir) ----
static std::wstring LauncherFilePath()
{
    std::wstring p = PendingFilePath();
    return p.substr(0, p.find_last_of(L'\\')) + L"\\launcher.txt";
}

// M33: exe'den KESKİN ikon çıkar. Tam yolu çöz (PATH → App Paths registry,
// chrome/edge/spotify gibi PATH'te olmayanlar için), sonra SHIL_JUMBO (256px)
// image-list ikonu (32px ExtractIcon yerine - dock'ta keskin küçültme).
static HICON ExtractExeIcon(const std::wstring& exe)
{
    std::wstring path = exe;
    while (!path.empty() && path.front() == L'"') path.erase(path.begin());
    while (!path.empty() && (path.back() == L'"' || path.back() == L' ')) path.pop_back();
    if (path.empty()) return nullptr;
    wchar_t full[MAX_PATH];
    if (path.find(L'\\') == std::wstring::npos)
    {
        if (SearchPathW(nullptr, path.c_str(), L".exe", MAX_PATH, full, nullptr))
            path = full;
        else // App Paths: HKLM sonra HKCU
        {
            std::wstring key = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + path;
            wchar_t val[MAX_PATH]; DWORD sz = sizeof(val);
            if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), nullptr,
                    RRF_RT_REG_SZ, nullptr, val, &sz) == ERROR_SUCCESS)
                path = val;
            else { sz = sizeof(val);
                if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), nullptr,
                        RRF_RT_REG_SZ, nullptr, val, &sz) == ERROR_SUCCESS)
                    path = val; }
        }
    }
    // keskin: sistem ikon indeksi + SHIL_JUMBO (256px) image-list
    SHFILEINFOW sfi{};
    if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX))
    {
        IImageList* il = nullptr;
        if (SUCCEEDED(SHGetImageList(SHIL_JUMBO, IID_IImageList, (void**)&il)) && il)
        {
            HICON ico = nullptr;
            il->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &ico);
            il->Release();
            if (ico) return ico;
        }
        // image-list olmazsa normal büyük shell ikonu
        SHFILEINFOW s2{};
        if (SHGetFileInfoW(path.c_str(), 0, &s2, sizeof(s2), SHGFI_ICON | SHGFI_LARGEICON)
            && s2.hIcon) return s2.hIcon;
    }
    HICON big = nullptr; // son çare
    ExtractIconExW(path.c_str(), 0, &big, nullptr, 1);
    return big;
}

static void LoadLaunchers(); // ileri bildirim (AddLauncherViaDialog yeniden yükler)

// M25: dosya seç diyaloğuyla dock'a kısayol ekle (launcher.txt'ye yazar + yeniden yükler)
static void AddLauncherViaDialog()
{
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = TL(L"Applications (*.exe)\0*.exe\0All files (*.*)\0*.*\0",
                         L"Uygulamalar (*.exe)\0*.exe\0Tüm dosyalar (*.*)\0*.*\0");
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = TL(L"Select an application to add to the dock", L"Dock'a eklenecek uygulamayı seç");
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    LowerCanvas(); // TOPMOST tuval diyaloğu örtmesin
    BOOL ok = GetOpenFileNameW(&ofn);
    RaiseCanvasTopmost();
    if (!ok) return; // iptal
    std::wstring full = file;
    std::wstring base = full;
    size_t s = base.find_last_of(L"\\/");
    if (s != std::wstring::npos) base = base.substr(s + 1);
    size_t dot = base.find_last_of(L'.');
    std::wstring label = (dot != std::wstring::npos) ? base.substr(0, dot) : base;
    {
        std::wofstream o(LauncherFilePath(), std::ios::app); // satır: Etiket|tamyol|
        if (o) o << label << L"|" << full << L"|\n";
    }
    LoadLaunchers(); // ikonuyla birlikte yeniden yükle
    ShowToast(label + TL(L" added to dock", L" dock'a eklendi"));
}

// M26: launcher.txt'yi mevcut g_launchers'tan yeniden yaz (ekleme/kaldırma sonrası)
static void SaveLaunchers()
{
    std::wofstream o(LauncherFilePath(), std::ios::trunc);
    if (!o) return;
    o << L"# Spatial Canvas baslaticilari (kaydet, uygulamayi yeniden baslat)\n";
    o << L"# Bicim: Etiket|program|argumanlar\n";
    o << L"# Sag kenar dock: ikona SOL-TIK = baslat, SAG-TIK = kaldir, + = ekle\n";
    for (auto& l : g_launchers)
        o << l.label << L"|" << l.exe << L"|" << l.args << L"\n";
}

// M26: dock kısayolunu kaldır (sağ-tık) - launcher.txt'den de siler
static void RemoveLauncher(int idx)
{
    if (idx < 0 || idx >= (int)g_launchers.size()) return;
    std::wstring lbl = g_launchers[idx].label;
    g_launchers.erase(g_launchers.begin() + idx);
    SaveLaunchers();
    ShowToast(lbl + TL(L" removed from dock", L" dock'tan kaldırıldı"));
}

static void LoadLaunchers()
{
    // M43: reload'da eski ikonları serbest bırak (JUMBO ikonlar GDI sızıntısı)
    for (auto& l : g_launchers) if (l.icon) DestroyIcon(l.icon);
    g_launchers.clear();
    std::wstring path = LauncherFilePath();
    { // ilk çalıştırma: kullanışlı şablon bırak (sonra yine de okunur → hemen dolu)
        std::wifstream test(path);
        if (!test)
        {
            std::wofstream o(path);
            if (o)
            {
                o << L"# Spatial Canvas baslaticilari (kaydet, uygulamayi yeniden baslat)\n";
                o << L"# Bicim: Etiket|program|argumanlar   (argumanlar istege bagli)\n";
                o << L"# Sag kenara imleci goturunce uygulama dock'u acilir; ikona tikla.\n";
                o << L"Terminal|cmd.exe|\n";
                o << L"Yeni Chrome|chrome.exe|--new-window\n";
                o << L"Not Defteri|notepad.exe|\n";
                o << L"Hesap Makinesi|calc.exe|\n";
                o << L"Dosya Gezgini|explorer.exe|\n";
            }
        }
    }
    std::wifstream f(path);
    if (!f) return;
    std::wstring line;
    while (std::getline(f, line))
    {
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        if (line.empty() || line[0] == L'#') continue;
        size_t b1 = line.find(L'|');
        if (b1 == std::wstring::npos) continue;
        size_t b2 = line.find(L'|', b1 + 1);
        Launcher l;
        l.label = line.substr(0, b1);
        if (b2 == std::wstring::npos) l.exe = line.substr(b1 + 1);
        else { l.exe = line.substr(b1 + 1, b2 - b1 - 1); l.args = line.substr(b2 + 1); }
        if (l.exe.empty()) continue;
        l.icon = ExtractExeIcon(l.exe); // M24: dock ikonu
        g_launchers.push_back(std::move(l));
    }
}

// Komut başlat + AdoptNewWindows'un imleç konumuna yerleştirmesi için
// paste slotu bırak (M11 mekanizması). exe adı .exe'ye normalize edilir.
static void RunCommand(const std::wstring& exe, const std::wstring& args, POINT cur)
{
    std::wstring e = exe;
    while (!e.empty() && e.front() == L'"') e.erase(e.begin());
    while (!e.empty() && (e.back() == L'"' || e.back() == L' ')) e.pop_back();
    if (e.empty()) return;
    HINSTANCE r = ShellExecuteW(nullptr, L"open", e.c_str(),
        args.empty() ? nullptr : args.c_str(), nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) { ShowToast(TL(L"Couldn't launch: ", L"Başlatılamadı: ") + e); return; }
    // paste slotu için exe basename + .exe (AdoptNewWindows exe-adıyla eşler)
    std::wstring base = e;
    size_t s = base.find_last_of(L"\\/");
    if (s != std::wstring::npos) base = base.substr(s + 1);
    if (base.size() < 4 || _wcsicmp(base.c_str() + base.size() - 4, L".exe") != 0)
        base += L".exe";
    float wx = g_cam.x + cur.x / g_cam.zoom;
    float wy = g_cam.y + cur.y / g_cam.zoom;
    g_pastePending.push_back({ base, wx, wy, GetTickCount64() });
    ShowToast(TL(L"Launching: ", L"Başlatılıyor: ") + e);
}

// Serbest giriş veya kısayol indeksini çalıştır, paleti kapat
static void RunLauncherInput(POINT cur)
{
    std::wstring in = g_launchText;
    while (!in.empty() && (in.front() == L' ')) in.erase(in.begin());
    while (!in.empty() && (in.back() == L' ' || in.back() == L'\r')) in.pop_back();
    // M40: komut paleti - tanınan komut kelimeleri uygulama değil eylem çalıştırır
    if (!in.empty())
    {
        std::wstring lc = in;
        std::transform(lc.begin(), lc.end(), lc.begin(), ::towlower);
        bool cmd = true;
        if (lc == L"fit" || lc == L"sigdir" || lc == L"sığdır") FitCamera(true);
        else if (lc == L"grid" || lc == L"arrange" || lc == L"diz") ArrangeGrid();
        else if (lc == L"quit" || lc == L"exit" || lc == L"cikis" || lc == L"çıkış") PostQuitMessage(0);
        else if (lc.rfind(L"save ", 0) == 0 && in.size() > 5) SaveNamedLayout(in.substr(5)); // M42
        else if (lc.rfind(L"load ", 0) == 0 && in.size() > 5) LoadNamedLayout(in.substr(5));
        else cmd = false;
        if (cmd) { g_launchOpen = false; g_launchText.clear(); g_launchSel = 0; return; }
    }
    if (!in.empty()) // serbest komut: program + argüman ayrımı
    {
        if (in.front() == L'"') // tırnaklı yol: kapanış tırnağına kadar program
        {
            size_t q = in.find(L'"', 1);
            if (q != std::wstring::npos)
            {
                std::wstring exe = in.substr(1, q - 1);
                std::wstring args = (q + 1 < in.size()) ? in.substr(q + 1) : L"";
                while (!args.empty() && args.front() == L' ') args.erase(args.begin());
                RunCommand(exe, args, cur);
            }
            else RunCommand(in.substr(1), L"", cur); // kapanmamış tırnak
        }
        else // ilk boşluğa kadar program, gerisi argüman
        {
            size_t sp = in.find(L' ');
            if (sp == std::wstring::npos) RunCommand(in, L"", cur);
            else RunCommand(in.substr(0, sp), in.substr(sp + 1), cur);
        }
    }
    else if (!g_launchers.empty()) // boş giriş + seçili kısayol
    {
        int i = std::clamp(g_launchSel, 0, (int)g_launchers.size() - 1);
        RunCommand(g_launchers[i].exe, g_launchers[i].args, cur);
    }
    g_launchOpen = false;
    g_launchText.clear();
    g_launchSel = 0;
}

// M11: tam exe yolu (çoğaltma için)
static std::wstring ExePathOf(HWND hwnd)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    std::wstring path;
    if (h)
    {
        wchar_t buf[MAX_PATH]; DWORD len = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, buf, &len)) path = buf;
        CloseHandle(h);
    }
    return path;
}

static std::wstring ExeNameOf(HWND hwnd)
{
    std::wstring p = ExePathOf(hwnd);
    if (p.empty()) return L"?";
    size_t s = p.find_last_of(L"\\/");
    return (s == std::wstring::npos) ? p : p.substr(s + 1);
}

static void LoadLayout()
{
    std::wifstream f(LayoutFilePath());
    if (!f) return;
    std::wstring line;
    while (std::getline(f, line))
    {
        while (!line.empty() && (line.back() == L'\r')) line.pop_back();
        // M27: pinned satır - PIN|exe|px|py|pw|ph
        if (line.rfind(L"PIN|", 0) == 0)
        {
            std::vector<size_t> b;
            for (size_t i = 0; i < line.size(); i++) if (line[i] == L'|') b.push_back(i);
            if (b.size() >= 5)
            {
                SavedPin sp;
                sp.exe = line.substr(b[0] + 1, b[1] - b[0] - 1);
                sp.px = (float)_wtof(line.substr(b[1] + 1, b[2] - b[1] - 1).c_str());
                sp.py = (float)_wtof(line.substr(b[2] + 1, b[3] - b[2] - 1).c_str());
                sp.pw = (float)_wtof(line.substr(b[3] + 1, b[4] - b[3] - 1).c_str());
                sp.ph = (float)_wtof(line.substr(b[4] + 1).c_str());
                if (!sp.exe.empty() && sp.pw > 1 && sp.ph > 1) g_savedPins.push_back(sp);
            }
            continue;
        }
        size_t p1 = line.rfind(L'|');
        if (p1 == std::wstring::npos) continue;
        size_t p2 = line.rfind(L'|', p1 - 1);
        if (p2 == std::wstring::npos) continue;
        POINT pt{ _wtoi(line.substr(p2 + 1, p1 - p2 - 1).c_str()),
                  _wtoi(line.substr(p1 + 1).c_str()) };
        g_savedLayout.push_back({ line.substr(0, p2), pt });
    }
}

static bool TryConsumeSavedPos(std::wstring const& exe, float& x, float& y)
{
    for (size_t i = 0; i < g_savedLayout.size(); i++)
    {
        if (_wcsicmp(g_savedLayout[i].first.c_str(), exe.c_str()) == 0)
        {
            x = (float)g_savedLayout[i].second.x;
            y = (float)g_savedLayout[i].second.y;
            g_savedLayout.erase(g_savedLayout.begin() + i);
            return true;
        }
    }
    return false;
}

static void SaveLayout()
{
    std::wofstream f(LayoutFilePath(), std::ios::trunc);
    if (!f) return;
    int written = 0;
    for (auto& t : g_tiles)
    {
        if (t.pinnedFlag) // M27: pinned tile ekran-sabit konumuyla kaydedilir
            f << L"PIN|" << t.exe << L"|" << (int)t.px << L"|" << (int)t.py
              << L"|" << (int)t.pw << L"|" << (int)t.ph << L"\n";
        else
            f << t.exe << L"|" << (int)t.wx << L"|" << (int)t.wy << L"\n";
        written++;
    }
    // M4: tüketilmemiş + oturum içinde kapananların yerleri de korunur
    for (auto& s : g_savedLayout)
    {
        if (written >= 40) break;
        f << s.first << L"|" << s.second.x << L"|" << s.second.y << L"\n";
        written++;
    }
    // M27: henüz açılmamış pinned exe'lerin kaydı korunur
    for (auto& sp : g_savedPins)
    {
        if (written >= 60) break;
        f << L"PIN|" << sp.exe << L"|" << (int)sp.px << L"|" << (int)sp.py
          << L"|" << (int)sp.pw << L"|" << (int)sp.ph << L"\n";
        written++;
    }
}

// ---- M42: adlandırılmış workspace (düzen profilleri) ----
static std::wstring NamedLayoutPath(const std::wstring& name)
{
    std::wstring p = PendingFilePath();
    std::wstring dir = p.substr(0, p.find_last_of(L'\\'));
    std::wstring safe; // güvenli dosya adı (yol enjeksiyonu yok)
    for (wchar_t c : name) if (iswalnum(c) || c == L'_' || c == L'-') safe += (wchar_t)towlower(c);
    if (safe.empty()) safe = L"ws";
    return dir + L"\\layout_" + safe + L".txt";
}

static void SaveNamedLayout(const std::wstring& name)
{
    SaveLayout(); // güncel layout.txt'yi tazele
    CopyFileW(LayoutFilePath().c_str(), NamedLayoutPath(name).c_str(), FALSE);
    ShowToast(TL(L"Layout saved: ", L"Düzen kaydedildi: ") + name);
}

static void LoadNamedLayout(const std::wstring& name)
{
    std::wifstream f(NamedLayoutPath(name));
    if (!f) { ShowToast(TL(L"Layout not found: ", L"Düzen bulunamadı: ") + name); return; }
    struct Rec { std::wstring exe; float x, y, pw, ph; bool pin; };
    std::vector<Rec> recs;
    std::wstring line;
    while (std::getline(f, line))
    {
        while (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.rfind(L"PIN|", 0) == 0)
        {
            std::vector<size_t> b;
            for (size_t i = 0; i < line.size(); i++) if (line[i] == L'|') b.push_back(i);
            if (b.size() >= 5)
                recs.push_back({ line.substr(b[0] + 1, b[1] - b[0] - 1),
                    (float)_wtof(line.substr(b[1] + 1, b[2] - b[1] - 1).c_str()),
                    (float)_wtof(line.substr(b[2] + 1, b[3] - b[2] - 1).c_str()),
                    (float)_wtof(line.substr(b[3] + 1, b[4] - b[3] - 1).c_str()),
                    (float)_wtof(line.substr(b[4] + 1).c_str()), true });
        }
        else
        {
            size_t p1 = line.rfind(L'|'); if (p1 == std::wstring::npos) continue;
            size_t p2 = line.rfind(L'|', p1 - 1); if (p2 == std::wstring::npos) continue;
            recs.push_back({ line.substr(0, p2),
                (float)_wtoi(line.substr(p2 + 1, p1 - p2 - 1).c_str()),
                (float)_wtoi(line.substr(p1 + 1).c_str()), 0, 0, false });
        }
    }
    std::vector<bool> used(recs.size(), false);
    int applied = 0;
    for (auto& t : g_tiles)
        for (size_t i = 0; i < recs.size(); i++)
        {
            if (used[i] || _wcsicmp(recs[i].exe.c_str(), t.exe.c_str()) != 0) continue;
            if (recs[i].pin)
            {
                t.pinnedFlag = true; t.pw = recs[i].pw; t.ph = recs[i].ph;
                t.px = std::clamp(recs[i].x, g_uiX, std::max(g_uiX, g_uiX + g_priW - t.pw));
                t.py = std::clamp(recs[i].y, g_uiY, std::max(g_uiY, g_uiY + g_priH - t.ph));
                // M43: artık açık pinned tile - g_savedPins'teki bekleyen kaydı sil (çift PIN satırı önle)
                g_savedPins.erase(std::remove_if(g_savedPins.begin(), g_savedPins.end(),
                    [&](const SavedPin& sp){ return _wcsicmp(sp.exe.c_str(), t.exe.c_str()) == 0; }),
                    g_savedPins.end());
            }
            else { t.pinnedFlag = false; t.wx = recs[i].x; t.wy = recs[i].y; }
            used[i] = true; applied++; break;
        }
    SaveLayout();
    FitCamera(true);
    ShowToast(TL(L"Layout loaded: ", L"Düzen yüklendi: ") + name + L" (" + std::to_wstring(applied) + TL(L" windows)", L" pencere)"));
}

// ---- Tile oluşturma ----
static std::wstring SettingsFilePath()
{
    std::wstring p = PendingFilePath();
    return p.substr(0, p.find_last_of(L'\\')) + L"\\settings.txt";
}

static void LoadSettings()
{
    std::wifstream f(SettingsFilePath());
    if (!f) { g_firstRun = true; return; } // M20: ilk açılış - ipucu kartı göster
    std::wstring line;
    bool migMouse = false; // M10: eski mpull aktifse kpull onu ezmesin
    while (std::getline(f, line))
    {
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring k = line.substr(0, eq);
        std::wstring v = line.substr(eq + 1);
        if (k == L"fps") g_set.fpsCap = _wtoi(v.c_str());
        else if (k == L"anim") g_set.animSpeed = _wtoi(v.c_str());
        else if (k == L"labels") g_set.labels = _wtoi(v.c_str()) != 0;
        else if (k == L"hover") g_set.hover = _wtoi(v.c_str()) != 0;
        else if (k == L"dive") g_set.diveZoom = (float)_wtof(v.c_str());
        else if (k == L"max") g_set.maxTiles = _wtoi(v.c_str());
        else if (k == L"lang") g_set.lang = _wtoi(v.c_str()); // M47
        else if (k == L"updchk") g_set.updateCheck = _wtoi(v.c_str()) != 0; // M48
        else if (k == L"updurl") g_set.updateUrl = v; // M48 (test/override)
        else if (k == L"lastrun") g_set.lastRun = v; // M53
        else if (k == L"restview") g_set.restoreView = _wtoi(v.c_str()) != 0; // M50
        else if (k == L"camx") { g_loadCamX = (float)_wtof(v.c_str()); g_hasSavedCam = true; } // M50
        else if (k == L"camy") g_loadCamY = (float)_wtof(v.c_str()); // M50
        else if (k == L"camz") g_loadCamZ = (float)_wtof(v.c_str()); // M50
        else if (k == L"bg") g_set.bgPreset = _wtoi(v.c_str());
        else if (k == L"grid") g_set.grid = _wtoi(v.c_str()) != 0; // M12
        else if (k == L"mmap") g_set.minimap = _wtoi(v.c_str()) != 0; // M36
        else if (k.size() == 4 && k.compare(0, 3, L"anc") == 0) // M13: yer imleri
        {
            int ai = k[3] - L'0';
            size_t c1 = v.find(L':');
            size_t c2 = (c1 == std::wstring::npos) ? c1 : v.find(L':', c1 + 1);
            if (ai >= 0 && ai < 4 && c2 != std::wstring::npos)
            {
                g_anchors[ai].x = (float)_wtof(v.substr(0, c1).c_str());
                g_anchors[ai].y = (float)_wtof(v.substr(c1 + 1, c2 - c1 - 1).c_str());
                float az = (float)_wtof(v.substr(c2 + 1).c_str());
                if (az > 0.01f) // bozuk dosya: clamp'siz zoom NaN üretir
                {
                    g_anchors[ai].zoom = std::clamp(az, 0.02f, 4.0f);
                    g_anchors[ai].set = true;
                }
            }
        }
        else if (k == L"span") g_set.canvasSpan = _wtoi(v.c_str());
        else if (k == L"mpull") // M10 migrasyon: eski fare seçimi kbPull'a taşınır
        {
            int mp = _wtoi(v.c_str());
            if (mp == 1) { g_set.kbPull = { VK_XBUTTON1, 0 }; migMouse = true; }
            else if (mp == 2) { g_set.kbPull = { VK_XBUTTON2, 0 }; migMouse = true; }
        }
        else if (k == L"wmod") g_set.wheelMod = _wtoi(v.c_str());
        else if (k == L"kpull" || k == L"kpanel" || k == L"kfit" || k == L"kexit" || k == L"ksearch" || k == L"klaunch")
        {
            size_t c = v.find(L':');
            if (c != std::wstring::npos)
            {
                int m = _wtoi(v.substr(0, c).c_str());
                int vk = _wtoi(v.substr(c + 1).c_str());
                if (vk > 0)
                {
                    Key nk{ vk, m & 7 };
                    if (k == L"kpull") { if (!migMouse) g_set.kbPull = nk; }
                    else if (k == L"kpanel") g_set.kbPanel = nk;
                    else if (k == L"kfit") g_set.kbFit = nk;
                    else if (k == L"ksearch") g_set.kbSearch = nk;
                    else if (k == L"klaunch") g_set.kbLaunch = nk;
                    else g_set.kbExit = nk;
                }
            }
        }
    }
    // savunma: aralık dışı değerleri toparla
    if (g_set.fpsCap != 15 && g_set.fpsCap != 30 && g_set.fpsCap != 60) g_set.fpsCap = 30;
    g_set.animSpeed = std::clamp(g_set.animSpeed, 0, 2);
    if (g_set.diveZoom < 0.5f || g_set.diveZoom > 1.0f) g_set.diveZoom = 0.92f;
    g_set.maxTiles = std::clamp(g_set.maxTiles, 4, 16);
    g_set.lang = std::clamp(g_set.lang, 0, 1); // M47
    g_set.bgPreset = std::clamp(g_set.bgPreset, 0, 3); // M29
    g_set.canvasSpan = std::clamp(g_set.canvasSpan, 0, 1);
    g_set.wheelMod = std::clamp(g_set.wheelMod, 0, 4);
    g_set.autostart = QueryAutostart(); // kaynak doğruluk: registry
}

static void SaveSettings()
{
    std::wofstream f(SettingsFilePath(), std::ios::trunc);
    if (!f) return;
    f << L"lang=" << g_set.lang << L"\n"; // M47
    f << L"updchk=" << (g_set.updateCheck ? 1 : 0) << L"\n"; // M48
    f << L"updurl=" << g_set.updateUrl << L"\n"; // M48
    f << L"lastrun=" << g_set.lastRun << L"\n"; // M53
    f << L"restview=" << (g_set.restoreView ? 1 : 0) << L"\n"; // M50
    // M50: son kamera görünümü (hedef kamera = kullanıcının baktığı yer)
    f << L"camx=" << g_camT.x << L"\ncamy=" << g_camT.y << L"\ncamz=" << g_camT.zoom << L"\n";
    f << L"fps=" << g_set.fpsCap << L"\n";
    f << L"anim=" << g_set.animSpeed << L"\n";
    f << L"labels=" << (g_set.labels ? 1 : 0) << L"\n";
    f << L"hover=" << (g_set.hover ? 1 : 0) << L"\n";
    f << L"dive=" << g_set.diveZoom << L"\n";
    f << L"max=" << g_set.maxTiles << L"\n";
    f << L"bg=" << g_set.bgPreset << L"\n";
    f << L"grid=" << (g_set.grid ? 1 : 0) << L"\n";
    f << L"mmap=" << (g_set.minimap ? 1 : 0) << L"\n"; // M36
    f << L"span=" << g_set.canvasSpan << L"\n";
    f << L"wmod=" << g_set.wheelMod << L"\n";
    f << L"kpull=" << g_set.kbPull.mods << L":" << g_set.kbPull.vk << L"\n";
    f << L"kpanel=" << g_set.kbPanel.mods << L":" << g_set.kbPanel.vk << L"\n";
    f << L"kfit=" << g_set.kbFit.mods << L":" << g_set.kbFit.vk << L"\n";
    f << L"kexit=" << g_set.kbExit.mods << L":" << g_set.kbExit.vk << L"\n";
    f << L"ksearch=" << g_set.kbSearch.mods << L":" << g_set.kbSearch.vk << L"\n";
    f << L"klaunch=" << g_set.kbLaunch.mods << L":" << g_set.kbLaunch.vk << L"\n"; // M23
    for (int i = 0; i < 4; i++) // M13: tuval yer imleri
        if (g_anchors[i].set)
            f << L"anc" << i << L"=" << g_anchors[i].x << L":" << g_anchors[i].y
              << L":" << g_anchors[i].zoom << L"\n";
}

static void ApplyFpsCap()
{
    for (auto& t : g_tiles)
    {
        try
        {
            t.session.MinUpdateInterval(
                winrt::TimeSpan{ std::chrono::milliseconds(1000 / std::max(1, g_set.fpsCap)) });
        }
        catch (...) {}
    }
}

// ---- M6: Windows ile başlat (HKCU Run anahtarı - kaynak doğruluk registry) ----
static bool QueryAutostart()
{
    HKEY k{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
        KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return false;
    bool exists = RegQueryValueExW(k, L"SpatialCanvas",
        nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return exists;
}

static void ApplyAutostart()
{
    HKEY k{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
        KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (g_set.autostart)
    {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring v = L"\"" + std::wstring(path) + L"\"";
        RegSetValueExW(k, L"SpatialCanvas", 0, REG_SZ,
            (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    }
    else RegDeleteValueW(k, L"SpatialCanvas");
    RegCloseKey(k);
}

// M8: global geri-çekil kısayolunu (yeniden) kaydet
static void ReRegisterPullHotkey()
{
    UnregisterHotKey(g_hwnd, HOTKEY_TOGGLE);
    if (IsMouseVk(g_set.kbPull.vk)) return; // M10: fare tuşuysa LL hook devralır
    UINT m = 0;
    if (g_set.kbPull.mods & 1) m |= MOD_CONTROL;
    if (g_set.kbPull.mods & 2) m |= MOD_ALT;
    if (g_set.kbPull.mods & 4) m |= MOD_SHIFT;
    RegisterHotKey(g_hwnd, HOTKEY_TOGGLE, m, (UINT)g_set.kbPull.vk);
}

// M10: yakalanan girdiyi (klavye veya fare vk) aktif satıra ata
static void AssignCapture(int vk, int mods)
{
    Key nk{ vk, mods };
    switch (g_captureRow)
    {
    case 101: g_set.kbPull = nk; ReRegisterPullHotkey(); break;
    case 102: g_set.kbPanel = nk; break;
    case 103: g_set.kbFit = nk; break;
    case 105: g_set.kbExit = nk; break;
    case 106: g_set.kbSearch = nk; break;
    case 107: g_set.kbLaunch = nk; break; // M23
    }
    g_captureRow = -1;
    SaveSettings();
}

// M10: atanmış eylemi çalıştır (klavye + fare ortak yol). true = işlendi
static bool ExecuteBoundAction(int vk, int mods)
{
    // M23: palet/arama açıkken fare-tuşuna atanmış eylemler çalışmasın
    // (klavye yolu zaten bu modlarda WM_KEYDOWN'da yutulur; bu, mouse yolu için)
    if (g_launchOpen || g_searchOpen) return false;
    auto is = [&](Key k) { return vk == k.vk && mods == k.mods; };
    if (is(g_set.kbExit))
    {
        // M17: bağlam merdiveni - refleks ESC uygulamayı kapatmasın.
        // Önce açık bağlamı kapat; çıkış ancak temiz durumda.
        if (g_panelOpen)       { g_panelOpen = false; return true; }
        if (g_marquee)         { g_marquee = false; ReleaseCapture(); return true; }
        if (!g_selSet.empty()) { g_selSet.clear(); return true; }
        PostQuitMessage(0);
        return true;
    }
    if (is(g_set.kbFit))   { FitCamera(); return true; }
    if (is(g_set.kbPanel)) { g_panelOpen = !g_panelOpen; return true; }
    if (is(g_set.kbSearch) && g_activeTile < 0)
    {
        g_searchOpen = true;
        g_searchText.clear();
        UpdateMatches();
        return true;
    }
    if (is(g_set.kbLaunch) && g_activeTile < 0) // M23: başlatıcı paleti
    {
        g_panelOpen = false; // panel açıksa kapat (üst üste binmesin)
        g_launchOpen = true;
        g_launchText.clear();
        g_launchSel = 0;
        return true;
    }
    return false;
}

static bool AddTile(HWND hwnd)
{
    Tile t;
    t.source = hwnd;
    t.exe = ExeNameOf(hwnd);
    // M15: kural - dışlanan exe hiç yakalanmaz (CreateTiles + AdoptNewWindows)
    {
        std::wstring exeL = t.exe;
        std::transform(exeL.begin(), exeL.end(), exeL.begin(), ::towlower);
        if (g_ruleExclude.count(exeL)) return false;
        auto oit = g_ruleOpacity.find(exeL); // M28: saydamlık kuralı
        if (oit != g_ruleOpacity.end()) t.opacity = oit->second;
        auto bit = g_ruleBlur.find(exeL);    // M34: blur kuralı
        if (bit != g_ruleBlur.end()) t.blur = bit->second;
    }
    t.exePath = ExePathOf(hwnd); // M11: çoğaltma için
    // M13: dock için uygulama ikonu (paylaşılan handle, sahibi pencere)
    SendMessageTimeoutW(hwnd, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 80,
        (PDWORD_PTR)&t.icon);
    if (!t.icon) t.icon = (HICON)GetClassLongPtrW(hwnd, GCLP_HICON);
    if (!t.icon) t.icon = (HICON)GetClassLongPtrW(hwnd, GCLP_HICONSM);
    try
    {
        t.item = CreateItemForWindow(hwnd);
        t.lastSize = t.item.Size();
        t.pool = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(
            g_winrtDevice, winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2, t.lastSize);
        t.session = t.pool.CreateCaptureSession(t.item);
        try { t.session.IsCursorCaptureEnabled(false); } catch (...) {}
        // NOT: M16'da eklenen IsBorderRequired(false)+IncludeSecondaryWindows(true)
        // KALDIRILDI - tile'ları siyah bırakıyordu. IsBorderRequired bazı Win
        // sürümlerinde capture'ı bozuyor (openai/codex #25178); RequestAccessAsync
        // (Borderless) manifest capability + consent ister, bizde yok. Sarı yakalama
        // çerçevesi (kozmetik) geri geldi - capture'ın çalışmasından önemsiz.
        // M4: yakalamayı sınırla (varsayılan 30fps - ayarlardan değişir)
        try { t.session.MinUpdateInterval(winrt::TimeSpan{ std::chrono::milliseconds(1000 / std::max(1, g_set.fpsCap)) }); } catch (...) {}
        t.session.StartCapture();
    }
    catch (...) { return false; }
    t.ww = (float)t.lastSize.Width;
    t.wh = (float)t.lastSize.Height;
    // Konum: yapıştırma slotu > kayıtlı layout > mevcut kümenin sağı
    float px = 0, py = 0;
    bool placed = false;
    for (size_t i = 0; i < g_pastePending.size(); i++) // M11
    {
        if (_wcsicmp(g_pastePending[i].exe.c_str(), t.exe.c_str()) == 0)
        {
            px = g_pastePending[i].wx;
            py = g_pastePending[i].wy;
            g_pastePending.erase(g_pastePending.begin() + i);
            placed = true;
            break;
        }
    }
    if (!placed && !TryConsumeSavedPos(t.exe, px, py))
    {
        float maxR = 0, minY = 0;
        for (auto& o : g_tiles)
        {
            maxR = std::max(maxR, o.wx + o.ww);
            minY = std::min(minY, o.wy);
        }
        px = g_tiles.empty() ? 0.0f : maxR + 150.0f;
        py = minY;
    }
    t.wx = px; t.wy = py;
    // M27: bu exe için kayıtlı pin varsa ekrana-sabitle (restart kalıcılığı)
    for (size_t i = 0; i < g_savedPins.size(); i++)
        if (_wcsicmp(g_savedPins[i].exe.c_str(), t.exe.c_str()) == 0)
        {
            t.pinnedFlag = true;
            t.pw = g_savedPins[i].pw; t.ph = g_savedPins[i].ph;
            t.px = std::clamp(g_savedPins[i].px, g_uiX, std::max(g_uiX, g_uiX + g_priW - t.pw));
            t.py = std::clamp(g_savedPins[i].py, g_uiY, std::max(g_uiY, g_uiY + g_priH - t.ph));
            g_savedPins.erase(g_savedPins.begin() + i);
            break;
        }
    g_tiles.push_back(std::move(t));
    ParkWindow(g_tiles.back(), (int)g_tiles.size() - 1);
    SaveLayout();
    return true;
}

static void CreateTiles()
{
    std::vector<HWND> wins;
    EnumWindows(EnumCb, reinterpret_cast<LPARAM>(&wins));
    for (HWND w : wins)
    {
        if ((int)g_tiles.size() >= g_set.maxTiles) break;
        AddTile(w);
    }
}

static void FitCamera(bool ignoreSel)
{
    g_momentum = false; // M12: sığdırma flick'i keser
    // M17: seçim varsa F çalışma setine odaklanır, tümüne değil
    bool sel = !ignoreSel && !g_selSet.empty();
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    int n = 0;
    auto fold = [&](float x, float y, float w, float h) {
        minX = std::min(minX, x); minY = std::min(minY, y);
        maxX = std::max(maxX, x + w); maxY = std::max(maxY, y + h); n++;
    };
    for (auto& t : g_tiles)
    {
        if (t.pinnedFlag) continue; // M22: pinned zaten ekranda - sınıra katma
        if (sel && !g_selSet.count(t.source)) continue;
        fold(t.wx, t.wy, t.ww, t.wh);
    }
    if (n == 0 && sel) // bayat seçim (HWND'ler tile olmaktan çıkmış) - tümüne düş
    {
        sel = false;
        for (auto& t : g_tiles) if (!t.pinnedFlag) fold(t.wx, t.wy, t.ww, t.wh);
    }
    // M45: seçim odaklı değilken notları da çerçevele (uzaktaki not kaybolmasın)
    if (!sel)
        for (auto& nt : g_notes) fold(nt.wx, nt.wy, nt.w, nt.h);
    if (!sel) // M54: bölgeleri de çerçevele
        for (auto& z : g_zones) fold(z.wx, z.wy, z.w, z.h);
    if (n == 0) return; // sığdıracak içerik yok (hepsi pinned / boş tuval)
    float bw = std::max(maxX - minX, 1.0f); // dejenere yerleşimde inf zoom yok
    float bh = std::max(maxY - minY, 1.0f);
    g_camT.zoom = std::min((float)g_sw / bw, (float)g_sh / bh) * 0.9f;
    g_camT.x = minX - ((float)g_sw / g_camT.zoom - bw) * 0.5f;
    g_camT.y = minY - ((float)g_sh / g_camT.zoom - bh) * 0.5f;
}

// ---- Kare güncelleme (poll, render thread'inde - kilit yok) ----
// M19: true döner = bu karede ekranı değiştiren bir şey oldu (yeni kare veya
// başlık değişti) - boşta çizimi atlamak için dirty bayrağını besler
static bool UpdateTiles()
{
    bool any = false;
    for (auto& t : g_tiles)
    {
        if (!t.alive) continue;
        if (!IsWindow(t.source)) { t.alive = false; continue; }
        ULONGLONG tn = GetTickCount64();
        if (tn - t.titleTick > 1000)
        {
            wchar_t b[200]{};
            GetWindowTextW(t.source, b, 200);
            if (t.title != b) { t.title = b; any = true; } // zoom-out etiketi tazelensin
            t.titleTick = tn;
        }
        try
        {
        // NOT: M18'in drain-to-newest + CopySubresourceRegion(içerik-boyut)
        // değişikliği tile'ları SİYAH bıraktı (capture kopyası boş kaldı).
        // M15'teki kanıtlı tek-frame + CopyResource(tam) yoluna dönüldü.
        auto frame = t.pool.TryGetNextFrame();
        if (!frame) continue;
        auto size = frame.ContentSize();
        auto frameTex = TextureFromSurface(frame.Surface());
        D3D11_TEXTURE2D_DESC fd{};
        frameTex->GetDesc(&fd);
        bool recreate = !t.tex;
        if (t.tex)
        {
            D3D11_TEXTURE2D_DESC pd{};
            t.tex->GetDesc(&pd);
            recreate = (pd.Width != fd.Width || pd.Height != fd.Height);
        }
        if (recreate)
        {
            t.tex = nullptr; t.srv = nullptr;
            D3D11_TEXTURE2D_DESC nd = fd;
            nd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            nd.MiscFlags = 0;
            nd.Usage = D3D11_USAGE_DEFAULT;
            nd.CPUAccessFlags = 0;
            winrt::check_hresult(g_device->CreateTexture2D(&nd, nullptr, t.tex.put()));
            winrt::check_hresult(g_device->CreateShaderResourceView(t.tex.get(), nullptr, t.srv.put()));
        }
        g_ctx->CopyResource(t.tex.get(), frameTex.get());
        any = true; // M19: yeni kare geldi - ekran değişti
        if (size.Width != t.lastSize.Width || size.Height != t.lastSize.Height)
        {
            t.lastSize = size;
            t.ww = (float)size.Width; t.wh = (float)size.Height;
            t.pool.Recreate(g_winrtDevice,
                winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        }
        }
        catch (...)
        {
            // M18: cihaz kaybını pencere ölümünden AYIR - cihaz kaybında tile
            // öldürmek tüm pencereleri şeritte yetim bırakırdı; reinit devralır
            if (g_device && g_device->GetDeviceRemovedReason() != S_OK)
                g_deviceLost = true;
            else
                t.alive = false; // pencere/oturum gerçekten öldü
        }
    }
    return any;
}

// ---- M3: Yaşam döngüsü ----
// Tek tile'ı kaldır (indeks kaydırmalarını düzelt). restoreWindow=true:
// pencere eski yerine döner ve oturum boyunca yeniden kapılmaz (M6).
static void RemoveTileAt(int i, bool restoreWindow)
{
    Tile& t = g_tiles[i];
    if (restoreWindow && IsWindow(t.source))
    {
        RestoreOriginal(t);
        g_excluded.insert(t.source);
    }
    if (g_activeTile == i)
    {
        g_activeTile = -1;
        g_camT.zoom = g_preSwapZoom;
        g_camT.x = g_preSwapX;
        g_camT.y = g_preSwapY;
        ForceForeground(g_hwnd);
    }
    else if (g_activeTile > i) g_activeTile--;
    if (g_dragTile == i) g_dragTile = -1;
    else if (g_dragTile > i) g_dragTile--;
    g_selSet.erase(t.source); // M11: seçimden düş
    if (g_focusWnd == t.source) g_focusWnd = nullptr; // M21: odaktan düş
    // pozisyonu hatırla: aynı exe yeniden açılırsa yerine düşsün
    g_savedLayout.push_back({ t.exe, POINT{ (LONG)t.wx, (LONG)t.wy } });
    try { if (t.session) t.session.Close(); } catch (...) {}
    try { if (t.pool) t.pool.Close(); } catch (...) {}
    g_tiles.erase(g_tiles.begin() + i);
    SavePendingRestore();
}

// M25: ✕ çipi = gerçek pencereyi KAPAT (eskiden sadece tuvalden çıkarıyordu).
// WM_CLOSE pencerenin kendi "kaydet?" diyaloğunu tetikler (zorla kill değil).
static void CloseTileWindow(int i)
{
    if (i < 0 || i >= (int)g_tiles.size()) return;
    HWND h = g_tiles[i].source;
    RECT orig = g_tiles[i].origRect;
    bool everP = g_tiles[i].everParked;
    RemoveTileAt(i, false); // oturum/pool kapat + tile kaldır (restore etme - kapanıyor)
    if (IsWindow(h))
    {
        // SADE WM_CLOSE = tek pencere kapatma (Alt+F4 eşdeğeri; Windows
        // Terminal dahil testte tek pencereyi kapattığı kanıtlandı).
        // Pencereyi park şeridinden NOTOPMOST normale al (kapatma diyaloğu
        // görünür olsun). ForceForeground/ShowWindow/SC_CLOSE KALDIRILDI:
        // bu manevralar WT'de WM_CLOSE'un işlenmesini bozuyordu ("tuvalden
        // gidiyor ama pencere açık kalıyor").
        if (everP) SetWindowPos(h, HWND_NOTOPMOST, orig.left, orig.top, 0, 0,
            SWP_NOSIZE | SWP_NOACTIVATE);
        PostMessageW(h, WM_CLOSE, 0, 0);
    }
}

// Kapanan pencerelerin tile'larını temizle
static void SweepDeadTiles()
{
    for (int i = (int)g_tiles.size() - 1; i >= 0; i--)
    {
        Tile& t = g_tiles[i];
        if (t.alive && IsWindow(t.source)) continue;
        // M16: yakalama öldü ama pencere yaşıyorsa 2px şeritte bırakma -
        // yerine döndür. g_excluded'a EKLEME ki AdoptNewWindows temiz bir
        // oturumla yeniden kapabilsin.
        if (IsWindow(t.source)) RestoreOriginal(t);
        RemoveTileAt(i, false);
    }
}

// Yeni açılan pencereleri tuvale al (tuval modundayken, 1.5sn'de bir)
static void AdoptNewWindows()
{
    // M16: bayat yapıştırma slotları - tek-instance app yeni pencere açmaz,
    // slot sonsuza kadar yaşayıp sonraki aynı-exe pencereyi kaçırırdı.
    // Erken return'lerden ÖNCE: dalıştayken de eskimeli.
    ULONGLONG nowT = GetTickCount64();
    size_t slotsBefore = g_pastePending.size();
    g_pastePending.erase(std::remove_if(g_pastePending.begin(), g_pastePending.end(),
        [&](const PasteSlot& s) { return nowT - s.tick > 15000; }),
        g_pastePending.end());
    if (g_pastePending.size() < slotsBefore) // M17: sessiz iptal olmasın
        ShowToast(TL(L"Paste timed out — app opened no new window", L"Yapıştırma zaman aşımı — uygulama yeni pencere açmadı"));
    if (g_activeTile >= 0) return; // çalışma modunda pencere kapma
    ULONGLONG now = GetTickCount64();
    if (now - g_lastAdopt < 1500) return;
    g_lastAdopt = now;
    std::vector<HWND> wins;
    EnumWindows(EnumCb, reinterpret_cast<LPARAM>(&wins));
    for (HWND w : wins)
    {
        if (g_excluded.count(w)) continue; // serbest bırakılanlar kapılmaz (M6)
        bool known = false;
        for (auto& t : g_tiles) if (t.source == w) { known = true; break; }
        if (!known)
        {
            // M17: sınır doluyken yeni pencere SESSİZCE yutulmasın -
            // kullanıcı bunu yakalama hatası sanıyordu (30sn'de bir uyar)
            if ((int)g_tiles.size() >= g_set.maxTiles)
            {
                static ULONGLONG s_lastCapToast = 0;
                if (now - s_lastCapToast > 30000)
                {
                    s_lastCapToast = now;
                    ShowToast(TL(L"Window limit reached (", L"Pencere sınırı dolu (") +
                        std::to_wstring(g_set.maxTiles) +
                        TL(L") — Settings > Max windows", L") — Ayarlar > Maks. pencere"));
                }
                break;
            }
            AddTile(w);
        }
    }
}

// M52: tuvalin o anki görünümünü PNG'ye kaydet (paylaşım için). Ana thread'de,
// Render içinden Present'tan ÖNCE çağrılır (backbuffer dolu). Yol döner (boş=hata).
static std::wstring SaveCanvasPng()
{
    if (!g_swap || !g_ctx || !g_device || !g_wic) return L"";
    winrt::com_ptr<ID3D11Texture2D> back;
    if (FAILED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), back.put_void()))) return L"";
    D3D11_TEXTURE2D_DESC d{}; back->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    winrt::com_ptr<ID3D11Texture2D> stage;
    if (FAILED(g_device->CreateTexture2D(&sd, nullptr, stage.put()))) return L"";
    g_ctx->CopyResource(stage.get(), back.get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g_ctx->Map(stage.get(), 0, D3D11_MAP_READ, 0, &m))) return L"";
    // dosya yolu: %USERPROFILE%\Pictures\SpatialCanvas_<zaman>.png
    wchar_t* up = nullptr; size_t ul = 0; _wdupenv_s(&up, &ul, L"USERPROFILE");
    std::wstring dir = (up ? up : L"C:"); free(up);
    dir += L"\\Pictures"; CreateDirectoryW(dir.c_str(), nullptr);
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t name[80];
    swprintf_s(name, L"\\SpatialCanvas_%04d%02d%02d_%02d%02d%02d.png",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring path = dir + name;
    bool ok = false;
    winrt::com_ptr<IWICBitmapEncoder> enc;
    winrt::com_ptr<IWICStream> stream;
    if (SUCCEEDED(g_wic->CreateStream(stream.put())) &&
        SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(g_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.put())) &&
        SUCCEEDED(enc->Initialize(stream.get(), WICBitmapEncoderNoCache)))
    {
        winrt::com_ptr<IWICBitmapFrameEncode> frame;
        IPropertyBag2* props = nullptr;
        if (SUCCEEDED(enc->CreateNewFrame(frame.put(), &props)) &&
            SUCCEEDED(frame->Initialize(props)))
        {
            frame->SetSize(d.Width, d.Height);
            WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA; // swapchain BGRA (D2D interop)
            frame->SetPixelFormat(&fmt);
            if (SUCCEEDED(frame->WritePixels(d.Height, m.RowPitch, m.RowPitch * d.Height,
                    (BYTE*)m.pData)) &&
                SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit()))
                ok = true;
        }
        if (props) props->Release();
    }
    g_ctx->Unmap(stage.get(), 0);
    return ok ? path : L"";
}

// ---- Render ----
static void Render()
{
    static const float BG[4][3] = {
        { 0.07f, 0.07f, 0.09f },     // koyu
        { 0.035f, 0.04f, 0.055f },   // gece
        { 0.0f, 0.0f, 0.0f },        // siyah
        { 0.015f, 0.02f, 0.035f } }; // M29: vinyet (D2D radyal üstüne biner)
    float clear[4] = { BG[g_set.bgPreset][0], BG[g_set.bgPreset][1],
                       BG[g_set.bgPreset][2], 1.0f };
    ID3D11RenderTargetView* rtv = g_rtv.get();
    g_ctx->ClearRenderTargetView(rtv, clear);
    DrawGrid(); // M12: ızgara tile'ların altında (D2D, D3D durumundan önce)
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{ 0, 0, (float)g_sw, (float)g_sh, 0, 1 };
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->RSSetState(g_raster.get());
    if (g_blend) g_ctx->OMSetBlendState(g_blend.get(), nullptr, 0xffffffff); // M28
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    g_ctx->VSSetShader(g_vs.get(), nullptr, 0);
    g_ctx->PSSetShader(g_ps.get(), nullptr, 0);
    ID3D11Buffer* cb = g_cb.get();
    g_ctx->VSSetConstantBuffers(0, 1, &cb);
    g_ctx->PSSetConstantBuffers(0, 1, &cb); // M28 FIX: PS extra.x(opacity) okur - yoksa alpha=0, tile görünmez
    ID3D11SamplerState* smp = g_sampler.get();
    g_ctx->PSSetSamplers(0, 1, &smp);

    auto drawQuad = [&](float sx, float sy, float sw, float sh,
                        ID3D11ShaderResourceView* srv, float opacity,
                        float blur, float texW, float texH)
    {
        if (sx > g_sw || sy > g_sh || sx + sw < 0 || sy + sh < 0) return;
        float cbuf[8] = { // M28/M34: rect + extra(opacity, blur, texelW, texelH)
            sx / g_sw * 2.0f - 1.0f, 1.0f - sy / g_sh * 2.0f,
            sw / g_sw * 2.0f, sh / g_sh * 2.0f,
            opacity, blur,
            blur > 0 && texW > 0 ? 1.0f / texW : 0.0f,
            blur > 0 && texH > 0 ? 1.0f / texH : 0.0f };
        g_ctx->UpdateSubresource(g_cb.get(), 0, nullptr, cbuf, 0, 0);
        g_ctx->PSSetShaderResources(0, 1, &srv);
        g_ctx->Draw(4, 0);
    };
    for (auto& t : g_tiles) // dünya tile'ları (kameralı)
    {
        if (!t.srv || t.pinnedFlag) continue; // M22: pinned ayrı pass'te
        drawQuad((t.wx - g_cam.x) * g_cam.zoom, (t.wy - g_cam.y) * g_cam.zoom,
                 t.ww * g_cam.zoom, t.wh * g_cam.zoom, t.srv.get(), t.opacity,
                 t.blur, t.ww, t.wh);
    }
    for (auto& t : g_tiles) // M22: pinned tile'lar - ekran-sabit, dünyanın üstünde
    {
        if (!t.srv || !t.pinnedFlag) continue;
        drawQuad(t.px, t.py, t.pw, t.ph, t.srv.get(), t.opacity, t.blur, t.ww, t.wh);
    }
    DrawOverlay(); // M4: başlık etiketleri + hover vurgusu
    if (g_pngRequest) // M52: PNG dışa aktar (Present'tan ÖNCE - backbuffer dolu, toast karede yok)
    {
        g_pngRequest = false;
        std::wstring p = SaveCanvasPng();
        ShowToast(p.empty() ? TL(L"PNG export failed", L"PNG dışa aktarma başarısız")
            : TL(L"Saved: ", L"Kaydedildi: ") + p);
    }
    HRESULT hr = g_swap->Present(1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
        g_deviceLost = true; // M18: ana döngü HandleDeviceLost ile kurtarır
}

// M18: cihaz kaybı (TDR, sürücü güncellemesi, uyku) - tam yeniden kurulum.
// Başarısızlıkta pencereler yerine döner ve temiz çıkılır; hiçbir pencere
// 2px şeritte yetim KALMAZ (bu, uygulamanın en kötü veri-kaybı senaryosuydu).
static void HandleDeviceLost()
{
    g_deviceLost = false;
    g_dragTile = -1; g_groupDrag = false; g_marquee = false; // etkileşim sıfırla
    g_dragNote = -1; g_resizeNote = -1; // M44/M46: not sürükle/boyutlandır
    g_dragZone = -1; g_resizeZone = -1; g_zoneTiles.clear(); // M54/M55: zon sürükle/boyutlandır
    g_connecting = false; g_connectFrom = nullptr; // M57: bağlantı kurma
    g_pinDrag = false; g_pinDragTile = -1; // M24 doğrulama: pin sürükleme de
    // 1) Cihaza bağlı TÜM kaynakları bırak (com_ptr.put() null ister)
    for (auto& t : g_tiles)
    {
        try { if (t.session) t.session.Close(); } catch (...) {}
        try { if (t.pool) t.pool.Close(); } catch (...) {}
        t.session = nullptr; t.pool = nullptr; t.item = nullptr;
        t.tex = nullptr; t.srv = nullptr; t.icoBmp = nullptr;
    }
    for (auto& l : g_launchers) l.icoBmp = nullptr; // M24: device-lost'ta launcher ikonları
    g_d2dRT = nullptr;
    g_brText = nullptr; g_brBg = nullptr; g_brHover = nullptr;
    g_brNote = nullptr; // M44
    g_textFmt = nullptr; g_textFmtL = nullptr; g_textFmtN = nullptr; // M44
    g_d2dFactory = nullptr; g_dwFactory = nullptr;
    g_rtv = nullptr; g_vs = nullptr; g_ps = nullptr;
    g_cb = nullptr; g_sampler = nullptr; g_raster = nullptr; g_blend = nullptr;
    g_swap = nullptr;
    g_winrtDevice = nullptr;
    if (g_ctx) g_ctx->ClearState();
    g_ctx = nullptr; g_device = nullptr;
    // 2) Yeniden kur - sürücü tam ortadaysa 1sn bekleyip bir kez daha dene
    for (int attempt = 0; attempt < 2; attempt++)
    {
        try
        {
            InitD3D();
            InitD2D();
            // 3) Yakalama oturumlarını tazele (tile konum/yerleşimi korunur)
            for (int i = (int)g_tiles.size() - 1; i >= 0; i--)
            {
                Tile& t = g_tiles[i];
                if (!IsWindow(t.source)) { RemoveTileAt(i, false); continue; }
                try
                {
                    t.item = CreateItemForWindow(t.source);
                    t.lastSize = t.item.Size();
                    t.pool = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(
                        g_winrtDevice, winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                        2, t.lastSize);
                    t.session = t.pool.CreateCaptureSession(t.item);
                    try { t.session.IsCursorCaptureEnabled(false); } catch (...) {}
                    try { t.session.MinUpdateInterval(winrt::TimeSpan{
                        std::chrono::milliseconds(1000 / std::max(1, g_set.fpsCap)) }); } catch (...) {}
                    t.session.StartCapture();
                    t.alive = true;
                }
                catch (...)
                {
                    // bu pencere artık yakalanamıyor: şeritte bırakma
                    RestoreOriginal(t);
                    RemoveTileAt(i, false);
                }
            }
            ShowToast(TL(L"Graphics device restored", L"Grafik cihazı yenilendi"));
            return;
        }
        catch (...)
        {
            if (attempt == 0) Sleep(1000);
        }
    }
    // 4) Kurtarılamadı: temiz çıkış (done: bloğu pencereleri restore eder)
    PostQuitMessage(0);
}

// ---- M2: Park & Swap ----
// Park stratejisi A': alt kenarda 2px görünür şerit, x kaydırmalı.
// Tam örtülme olmadığı için occlusion throttling tetiklenmez (Risk #1 çaresi).
static void ParkWindow(Tile& t, int idx)
{
    if (!t.everParked) t.wasMax = IsZoomed(t.source) != 0;
    if (IsZoomed(t.source)) ShowWindow(t.source, SW_RESTORE);
    if (!t.everParked)
    {
        GetWindowRect(t.source, &t.origRect);
        RECT vis{};
        if (SUCCEEDED(DwmGetWindowAttribute(t.source, DWMWA_EXTENDED_FRAME_BOUNDS,
            &vis, sizeof(vis))))
        {
            t.frameDX = vis.left - t.origRect.left;
            t.frameDY = vis.top - t.origRect.top;
        }
        t.everParked = true;
        SavePendingRestore();
    }
    int h = t.origRect.bottom - t.origRect.top;
    // M11: tuval TOPMOST olduğundan park penceresi de TOPMOST'a (tuvalin
    // üstüne) itilir - 2px şerit görünür kalır, occlusion throttle çaresi yaşar
    SetWindowPos(t.source, HWND_TOPMOST, 60 * idx, g_priH - 2 + t.frameDY - 0,
        0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    // pencerenin görünür üst kenarı ANA monitörün 2px altından başlasın:
    RECT now{}; GetWindowRect(t.source, &now);
    int visTop = now.top + t.frameDY;
    int delta = (g_priH - 2) - visTop;
    if (delta != 0)
        SetWindowPos(t.source, nullptr, now.left, now.top + delta,
            0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    t.parked = true;
}

static void RestoreOriginal(Tile& t)
{
    if (!IsWindow(t.source)) return;
    SetWindowPos(t.source, HWND_NOTOPMOST, t.origRect.left, t.origRect.top,
        0, 0, SWP_NOSIZE | SWP_NOACTIVATE); // M11: topmost'tan da indir
    if (t.wasMax) ShowWindow(t.source, SW_MAXIMIZE);
    t.parked = false;
}

// M16: çökme anı acil kurtarma - SADECE Win32 çağrıları (CRT heap yok,
// çökmüş süreçte alloc güvensiz). pending_restore.txt ikinci sigorta olarak
// diskte KALIR (bu handler'ın kendisi çökerse sonraki açılış devralır).
static void EmergencyRestore() noexcept
{
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1)) return;
    ShowTaskbars(true);
    for (auto& t : g_tiles)
        if (t.everParked && IsWindow(t.source))
            SetWindowPos(t.source, HWND_NOTOPMOST,
                t.origRect.left, t.origRect.top, 0, 0,
                SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS*)
{
    EmergencyRestore();
    return EXCEPTION_CONTINUE_SEARCH; // WER/dump raporlaması yaşasın
}

// M11: tuval z-katmanı yönetimi - tuval modunda görev çubuğunun ÜSTÜNDE,
// çalışma modunda normal z'de (görev çubuğu görünür)
static void RaiseCanvasTopmost()
{
    SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    // park edilmiş pencereleri tuvalin de üstüne it (2px şerit görünür kalsın)
    for (auto& t : g_tiles)
        if (t.parked && IsWindow(t.source))
            SetWindowPos(t.source, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void LowerCanvas()
{
    SetWindowPos(g_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// Arka plandan güvenilir odak alma (AttachThreadInput - sahte tuş YOK,
// modifier durumunu bozmaz)
static void ForceForeground(HWND hwnd)
{
    HWND fg = GetForegroundWindow();
    DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD myTid = GetCurrentThreadId();
    if (fgTid && fgTid != myTid) AttachThreadInput(myTid, fgTid, TRUE);
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    if (fgTid && fgTid != myTid) AttachThreadInput(myTid, fgTid, FALSE);
}

static void TrySwapIn(int idx, POINT clientAnchor)
{
    if (idx < 0 || idx >= (int)g_tiles.size()) return;
    Tile& t = g_tiles[idx];
    if (!t.alive || !IsWindow(t.source)) return;
    // Geri dönüş için kamerayı kaydet (tek tıkla aynı manzaraya dönülür)
    g_preSwapZoom = g_cam.zoom;
    g_preSwapX = g_cam.x;
    g_preSwapY = g_cam.y;
    // Kamerayı 1.0'a sabitle (çapa: imleç korunur), sonra quad'ı imlecin
    // bulunduğu MONİTÖRE TAM OTURT (M7: span modunda pencere iki monitöre yayılmaz)
    float worldX = g_cam.x + clientAnchor.x / g_cam.zoom;
    float worldY = g_cam.y + clientAnchor.y / g_cam.zoom;
    float qx0 = t.wx - (worldX - (float)clientAnchor.x);
    float qy0 = t.wy - (worldY - (float)clientAnchor.y);
    POINT scrA{ clientAnchor.x + g_vx, clientAnchor.y + g_vy };
    HMONITOR hm = MonitorFromPoint(scrA, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    float mL = 0, mT = 0, mR = (float)g_sw, mB = (float)g_sh;
    if (GetMonitorInfoW(hm, &mi))
    {
        mL = (float)(mi.rcMonitor.left - g_vx);
        mT = (float)(mi.rcMonitor.top - g_vy);
        mR = (float)(mi.rcMonitor.right - g_vx);
        mB = (float)(mi.rcMonitor.bottom - g_vy);
    }
    float qxF = (t.ww <= mR - mL)
        ? std::clamp(qx0, mL, mR - t.ww)
        : std::clamp(qx0, mR - t.ww, mL);
    float qyF = (t.wh <= mB - mT)
        ? std::clamp(qy0, mT, mB - t.wh)
        : std::clamp(qy0, mB - t.wh, mT);
    g_cam.zoom = 1.0f;
    g_cam.x = floorf(t.wx - qxF);
    g_cam.y = floorf(t.wy - qyF);
    g_camT = g_cam; // anlık hizalama - animasyonsuz (piksel hassas)
    int qx = (int)(t.wx - g_cam.x);
    int qy = (int)(t.wy - g_cam.y);
    // Hizalama kalibrasyonu: yakalama boyutu görünür sınırlara mı,
    // pencere rect'ine mi denk? Ölç, ona göre ofset uygula.
    int offX = 0, offY = 0;
    RECT er{};
    if (SUCCEEDED(DwmGetWindowAttribute(t.source, DWMWA_EXTENDED_FRAME_BOUNDS,
        &er, sizeof(er))))
    {
        int erw = er.right - er.left;
        if (abs((int)t.lastSize.Width - erw) <= 2) { offX = t.frameDX; offY = t.frameDY; }
    }
    LowerCanvas(); // M11: gerçek pencere görev çubuğuyla normal z'de yaşasın
    SetWindowPos(t.source, HWND_TOP, qx + g_vx - offX, qy + g_vy - offY,
        0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(t.source);
    t.parked = false;
    g_activeTile = idx;
    t.activeSeq = ++g_activeCounter; // M21: Tab MRU sırası
    g_swapArmed = false;
}

static void SwapOut()
{
    if (g_activeTile < 0) return;
    Tile& t = g_tiles[g_activeTile];
    if (IsWindow(t.source)) ParkWindow(t, g_activeTile);
    g_activeTile = -1;
    RaiseCanvasTopmost(); // M11: tuval moduna dönüş - görev çubuğunun üstüne
    // Swap öncesi manzaraya YUMUŞAK dönüş (hedef kamera - animasyonlu)
    g_camT.zoom = g_preSwapZoom;
    g_camT.x = g_preSwapX;
    g_camT.y = g_preSwapY;
    ForceForeground(g_hwnd);
}

// Global hook: ayarlı-modifikatör+Wheel = her yerden zoom, ayarlı fare tuşu = geri çekil
static LRESULT CALLBACK LLMouseProc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION)
    {
        auto* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
        // M10: geri çekil fare tuşuna atanmışsa GLOBAL yakala
        // (yakalama modunda dokunma - tuş pencereye düşsün ki yeniden atanabilsin)
        if (g_captureRow < 0 && IsMouseVk(g_set.kbPull.vk))
        {
            bool down = false, match = false;
            if (wp == WM_XBUTTONDOWN || wp == WM_XBUTTONUP)
            {
                WORD btn = HIWORD(info->mouseData);
                match = (g_set.kbPull.vk == VK_XBUTTON1 && btn == XBUTTON1) ||
                        (g_set.kbPull.vk == VK_XBUTTON2 && btn == XBUTTON2);
                down = (wp == WM_XBUTTONDOWN);
            }
            else if (wp == WM_MBUTTONDOWN || wp == WM_MBUTTONUP)
            {
                match = (g_set.kbPull.vk == VK_MBUTTON);
                down = (wp == WM_MBUTTONDOWN);
            }
            if (match)
            {
                int m = ((GetAsyncKeyState(VK_CONTROL) & 0x8000) ? 1 : 0)
                      | ((GetAsyncKeyState(VK_MENU)    & 0x8000) ? 2 : 0)
                      | ((GetAsyncKeyState(VK_SHIFT)   & 0x8000) ? 4 : 0);
                if (m == g_set.kbPull.mods)
                {
                    if (down) PostMessageW(g_hwnd, MSG_PULLBACK, 0, 0);
                    return 1; // tüket (down+up)
                }
            }
        }
        if (wp == WM_MOUSEWHEEL)
        {
            bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
            bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            // 0 Ctrl+Alt, 1 Ctrl+Shift, 2 Alt+Shift, 3 Ctrl, 4 Alt (tam eşleşme)
            static const bool WMOD[5][3] = {
                {true,true,false},{true,false,true},{false,true,true},
                {true,false,false},{false,true,false} };
            const bool* w = WMOD[g_set.wheelMod];
            if (ctrl == w[0] && alt == w[1] && shift == w[2])
            {
                short delta = (short)HIWORD(info->mouseData);
                PostMessageW(g_hwnd, MSG_GLOBAL_WHEEL, (WPARAM)(int)delta,
                    MAKELPARAM(info->pt.x, info->pt.y));
                return 1; // tüket - alttaki uygulamaya gitmesin
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// ---- Girdi ----
static int HitTile(float wx, float wy)
{
    for (int i = (int)g_tiles.size() - 1; i >= 0; i--)
    {
        auto& t = g_tiles[i];
        if (t.pinnedFlag) continue; // M22: pinned dünya hit-test'inde değil
        if (wx >= t.wx && wx <= t.wx + t.ww && wy >= t.wy && wy <= t.wy + t.wh)
            return i;
    }
    return -1;
}

// M22: pinned rect'i ekran içine kıstır - tile ekrandan büyükse (zoom'da
// pinlenince olur) lo>hi olur ve std::clamp UB verirdi; hi'yi lo'ya tabanla
static float ClampPin(float v, float lo, float hiRaw) { return std::clamp(v, lo, std::max(lo, hiRaw)); }

// M22: ekran-uzayı (client px) pinned tile vuruşu
static int HitPinned(POINT cp)
{
    for (int i = (int)g_tiles.size() - 1; i >= 0; i--)
    {
        auto& t = g_tiles[i];
        if (!t.pinnedFlag) continue;
        if (cp.x >= t.px && cp.x <= t.px + t.pw &&
            cp.y >= t.py && cp.y <= t.py + t.ph) return i;
    }
    return -1;
}

// M22: imleç altındaki tile'ı ekrana sabitle / sabitlenmişi çöz (Ctrl+P)
static void TogglePin(POINT cp)
{
    int pi = HitPinned(cp);
    if (pi >= 0) // sabitlenmişi çöz - dünya konumuna geri çevir
    {
        Tile& t = g_tiles[pi];
        t.pinnedFlag = false;
        t.wx = g_cam.x + t.px / g_cam.zoom;
        t.wy = g_cam.y + t.py / g_cam.zoom;
        SaveLayout(); // M27: pin durumu kalıcı
        ShowToast(TL(L"Unpinned", L"Sabitleme kaldırıldı"));
        return;
    }
    int hi = HitTile(g_cam.x + cp.x / g_cam.zoom, g_cam.y + cp.y / g_cam.zoom);
    if (hi < 0) return;
    Tile& t = g_tiles[hi];
    t.pw = t.ww * g_cam.zoom; t.ph = t.wh * g_cam.zoom;
    t.px = ClampPin((t.wx - g_cam.x) * g_cam.zoom, g_uiX, g_uiX + g_priW - t.pw);
    t.py = ClampPin((t.wy - g_cam.y) * g_cam.zoom, g_uiY, g_uiY + g_priH - t.ph);
    t.pinnedFlag = true;
    g_selSet.erase(t.source);
    SaveLayout(); // M27: pin durumu kalıcı (restart'ta geri yüklenir)
    ShowToast(TL(L"Pinned to screen (HUD) — Ctrl+P to unpin", L"Tuvale sabitlendi (HUD) — Ctrl+P ile çöz"));
}

// ---- M9: pencere arama + tile'a uçuş ----
static std::wstring LowerW(std::wstring s)
{
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

static void UpdateMatches()
{
    g_matches.clear();
    g_noteMatches.clear(); // M49
    g_zoneMatches.clear(); // M67
    std::wstring q = LowerW(g_searchText);
    if (q.empty()) { g_searchSel = 0; return; }
    for (int i = 0; i < (int)g_tiles.size(); i++)
    {
        if (LowerW(g_tiles[i].title).find(q) != std::wstring::npos ||
            LowerW(g_tiles[i].exe).find(q) != std::wstring::npos)
            g_matches.push_back(i);
    }
    for (int i = 0; i < (int)g_notes.size(); i++) // M49: not metninde de ara
        if (LowerW(g_notes[i].text).find(q) != std::wstring::npos)
            g_noteMatches.push_back(i);
    for (int i = 0; i < (int)g_zones.size(); i++) // M67: bölge başlığında da ara
        if (LowerW(g_zones[i].title).find(q) != std::wstring::npos)
            g_zoneMatches.push_back(i);
    if (g_searchSel >= (int)(g_matches.size() + g_noteMatches.size() + g_zoneMatches.size())) g_searchSel = 0;
}

static void CloseSearch()
{
    g_searchOpen = false;
    g_searchText.clear();
    g_matches.clear();
    g_noteMatches.clear(); // M49
    g_zoneMatches.clear(); // M67
    g_searchSel = 0;
}

// Kamerayı tile'a animasyonla uçur (dalmadan: dalış eşiğinin altında durur)
static void FlyToTile(int i)
{
    if (i < 0 || i >= (int)g_tiles.size()) return;
    g_momentum = false; // M12: uçuş flick'i keser
    Tile& t = g_tiles[i];
    float fitZ = std::min((float)g_sw / t.ww, (float)g_sh / t.wh);
    float z = std::clamp(fitZ * 0.8f, 0.02f, g_set.diveZoom * 0.85f);
    g_camT.zoom = z;
    g_camT.x = t.wx + t.ww / 2.0f - g_sw / (2.0f * z);
    g_camT.y = t.wy + t.wh / 2.0f - g_sh / (2.0f * z);
    if (z < 0.75f) g_swapArmed = true;
}

// M49: kamerayı nota uçur (okunur zoom; dalış yok - notlar dalınmaz)
static void FlyToNote(int i)
{
    if (i < 0 || i >= (int)g_notes.size()) return;
    g_momentum = false;
    Note& n = g_notes[i];
    float z = std::clamp(std::min((float)g_sw / (n.w * 1.6f), (float)g_sh / (n.h * 1.6f)),
        0.02f, 1.5f);
    g_camT.zoom = z;
    g_camT.x = n.wx + n.w / 2.0f - g_sw / (2.0f * z);
    g_camT.y = n.wy + n.h / 2.0f - g_sh / (2.0f * z);
    g_swapArmed = true;
}

// M67: kamerayı bölgeye uçur (okunur zoom; dalış yok)
static void FlyToZone(int i)
{
    if (i < 0 || i >= (int)g_zones.size()) return;
    g_momentum = false;
    Zone& z = g_zones[i];
    float zoom = std::clamp(std::min((float)g_sw / (z.w * 1.15f), (float)g_sh / (z.h * 1.15f)),
        0.02f, g_set.diveZoom * 0.85f);
    g_camT.zoom = zoom;
    g_camT.x = z.wx + z.w / 2.0f - g_sw / (2.0f * zoom);
    g_camT.y = z.wy + z.h / 2.0f - g_sh / (2.0f * zoom);
    g_swapArmed = true;
}

// M49/M67: arama seçimi (birleşik) - tile / not / bölge, hangisiyse ona uç
static void FlyToSearchSel()
{
    int nt = (int)g_matches.size(), nn = (int)g_noteMatches.size();
    if (g_searchSel < nt) FlyToTile(g_matches[g_searchSel]);
    else if (g_searchSel < nt + nn) FlyToNote(g_noteMatches[g_searchSel - nt]);
    else
    {
        int zi = g_searchSel - nt - nn;
        if (zi >= 0 && zi < (int)g_zoneMatches.size()) FlyToZone(g_zoneMatches[zi]);
    }
}

// ---- M11: çoğaltma + çoklu seçim eylemleri ----
static void CopySelection(POINT cur)
{
    g_copyItems.clear();
    if (!g_selSet.empty())
    {
        for (auto& t : g_tiles)
            if (g_selSet.count(t.source) && !t.exePath.empty())
                g_copyItems.push_back({ t.exePath, t.exe });
    }
    else
    {
        int hov = HitTile(g_cam.x + cur.x / g_cam.zoom,
                          g_cam.y + cur.y / g_cam.zoom);
        if (hov >= 0 && !g_tiles[hov].exePath.empty())
            g_copyItems.push_back({ g_tiles[hov].exePath, g_tiles[hov].exe });
    }
    ShowToast(g_copyItems.empty() ? TL(L"No window to copy", L"Kopyalanacak pencere yok") // M17
        : std::to_wstring(g_copyItems.size()) + TL(L" window(s) copied", L" pencere kopyalandı"));
}

static void PasteCopies(POINT cur)
{
    if (g_copyItems.empty()) return;
    float wx = g_cam.x + cur.x / g_cam.zoom;
    float wy = g_cam.y + cur.y / g_cam.zoom;
    int launched = 0; // M17: geri bildirim için say
    for (size_t i = 0; i < g_copyItems.size(); i++)
    {
        HINSTANCE r = ShellExecuteW(nullptr, L"open", g_copyItems[i].path.c_str(),
            nullptr, nullptr, SW_SHOWNORMAL);
        if ((INT_PTR)r <= 32) continue; // M16: başlatılamadı - bayat slot bırakma
        launched++;
        g_pastePending.push_back({ g_copyItems[i].exe,
            wx + (float)i * 80.0f, wy + (float)i * 80.0f, GetTickCount64() });
    }
    ShowToast(launched ? std::to_wstring(launched) + TL(L" app(s) launching…", L" uygulama başlatılıyor…")
                       : TL(L"Couldn't launch app", L"Uygulama başlatılamadı")); // M17
}

static void RemoveSelected()
{
    int n = 0; // M17: gerçekten bırakılan tile sayısı
    for (int i = (int)g_tiles.size() - 1; i >= 0; i--)
        if (g_selSet.count(g_tiles[i].source))
        {
            RemoveTileAt(i, true);
            n++;
        }
    g_selSet.clear();
    if (n) ShowToast(std::to_wstring(n) + TL(L" window(s) removed", L" pencere bırakıldı")); // M17
}

// M35: dağınık tile'ları düzgün ızgaraya diz (Ctrl+G) - Miro "clean up" tarzı.
// Pinned tile'lar dokunulmaz; kalanlar ~kare ızgaraya uniform hücreyle yerleşir.
static void ArrangeGrid()
{
    std::vector<int> idx;
    for (int i = 0; i < (int)g_tiles.size(); i++)
        if (!g_tiles[i].pinnedFlag) idx.push_back(i);
    if (idx.empty()) return;
    int cols = (int)ceilf(sqrtf((float)idx.size()));
    if (cols < 1) cols = 1;
    float cw = 0, ch = 0;
    for (int i : idx) { cw = std::max(cw, g_tiles[i].ww); ch = std::max(ch, g_tiles[i].wh); }
    cw += 90.0f; ch += 90.0f; // hücre boşluğu
    for (size_t k = 0; k < idx.size(); k++)
    {
        int r = (int)k / cols, c = (int)k % cols;
        g_tiles[idx[k]].wx = c * cw;
        g_tiles[idx[k]].wy = r * ch;
    }
    SaveLayout();
    FitCamera(true);
    ShowToast(std::to_wstring((int)idx.size()) + TL(L" windows arranged into grid", L" pencere ızgaraya dizildi"));
}

// M56: bir bölgenin İÇİNDEKİ (merkezi zonda) pencereleri zon-içinde ızgaraya diz;
// gerekirse zonu içeriği saracak şekilde büyüt. (Zon ⊞ butonundan tetiklenir.)
static void ArrangeZone(int zi)
{
    if (zi < 0 || zi >= (int)g_zones.size()) return;
    Zone& z = g_zones[zi];
    std::vector<int> idx;
    for (int i = 0; i < (int)g_tiles.size(); i++)
    {
        if (g_tiles[i].pinnedFlag) continue;
        float cx = g_tiles[i].wx + g_tiles[i].ww / 2, cy = g_tiles[i].wy + g_tiles[i].wh / 2;
        if (cx >= z.wx && cx <= z.wx + z.w && cy >= z.wy && cy <= z.wy + z.h) idx.push_back(i);
    }
    if (idx.empty()) { ShowToast(TL(L"Zone is empty", L"Bölge boş")); return; }
    int cols = (int)ceilf(sqrtf((float)idx.size())); if (cols < 1) cols = 1;
    int rows = ((int)idx.size() + cols - 1) / cols;
    float cw = 0, ch = 0;
    for (int i : idx) { cw = std::max(cw, g_tiles[i].ww); ch = std::max(ch, g_tiles[i].wh); }
    cw += 50.0f; ch += 50.0f;
    float ox = z.wx + 30, oy = z.wy + ZONE_BAR + 18; // zon içi sol-üst (başlık çubuğunun altı)
    for (size_t k = 0; k < idx.size(); k++)
    {
        int r = (int)k / cols, c = (int)k % cols;
        g_tiles[idx[k]].wx = ox + c * cw;
        g_tiles[idx[k]].wy = oy + r * ch;
    }
    z.w = std::max(z.w, cols * cw + 60);       // zonu içeriği saracak şekilde büyüt
    z.h = std::max(z.h, ZONE_BAR + rows * ch + 40);
    SaveLayout(); SaveZones();
    ShowToast(std::to_wstring((int)idx.size()) + TL(L" windows tidied in zone", L" pencere bölgede dizildi"));
}

// M14: sürüklenen tile yakın kenarlara yapışır (Alt basılıyken serbest).
// Eksen başına en yakın aday: kenar-kenara bitişme veya kenar hizalama.
static void SnapTile(int i)
{
    Tile& d = g_tiles[i];
    float th = 14.0f / g_cam.zoom; // ekranda ~14 px
    float bestX = 1e9f, bx = 0, bestY = 1e9f, by = 0;
    for (int j = 0; j < (int)g_tiles.size(); j++)
    {
        if (j == i) continue;
        Tile& o = g_tiles[j];
        if (o.pinnedFlag) continue; // M22: pinned snap adayı değil
        bool ovY = d.wy < o.wy + o.wh + th && d.wy + d.wh > o.wy - th;
        bool ovX = d.wx < o.wx + o.ww + th && d.wx + d.ww > o.wx - th;
        if (ovY)
        {
            struct { float dist, pos; } c[4] = {
                { fabsf(d.wx - (o.wx + o.ww)), o.wx + o.ww },
                { fabsf((d.wx + d.ww) - o.wx), o.wx - d.ww },
                { fabsf(d.wx - o.wx), o.wx },
                { fabsf((d.wx + d.ww) - (o.wx + o.ww)), o.wx + o.ww - d.ww } };
            for (auto& cc : c)
                if (cc.dist < th && cc.dist < bestX) { bestX = cc.dist; bx = cc.pos; }
        }
        if (ovX)
        {
            struct { float dist, pos; } c[4] = {
                { fabsf(d.wy - (o.wy + o.wh)), o.wy + o.wh },
                { fabsf((d.wy + d.wh) - o.wy), o.wy - d.wh },
                { fabsf(d.wy - o.wy), o.wy },
                { fabsf((d.wy + d.wh) - (o.wy + o.wh)), o.wy + o.wh - d.wh } };
            for (auto& cc : c)
                if (cc.dist < th && cc.dist < bestY) { bestY = cc.dist; by = cc.pos; }
        }
    }
    if (bestX < 1e9f) d.wx = bx;
    if (bestY < 1e9f) d.wy = by;
}

// ---- M21: klavye ile tile gezinme (fare olmadan pencere yöneticisi) ----
static int FindTileByHwnd(HWND h)
{
    for (int i = 0; i < (int)g_tiles.size(); i++)
        if (g_tiles[i].source == h) return i;
    return -1;
}

// Odağı yönde en yakın komşuya taşı (90° koni). dir: 0 sol,1 sağ,2 yukarı,3 aşağı
static void MoveFocusDir(int dir)
{
    if (g_tiles.empty()) return;
    int cur = FindTileByHwnd(g_focusWnd);
    if (cur < 0) // odak yok: görüş merkezine en yakın tile'a kur
    {
        float vcx = g_cam.x + g_sw / (2.0f * g_cam.zoom);
        float vcy = g_cam.y + g_sh / (2.0f * g_cam.zoom);
        float best = 1e30f; int bi = -1;
        for (int i = 0; i < (int)g_tiles.size(); i++)
        {
            if (!g_tiles[i].alive || g_tiles[i].pinnedFlag) continue; // M22
            float cx = g_tiles[i].wx + g_tiles[i].ww / 2;
            float cy = g_tiles[i].wy + g_tiles[i].wh / 2;
            float d = (cx - vcx) * (cx - vcx) + (cy - vcy) * (cy - vcy);
            if (d < best) { best = d; bi = i; }
        }
        if (bi >= 0) g_focusWnd = g_tiles[bi].source;
        return;
    }
    Tile& c = g_tiles[cur];
    float ccx = c.wx + c.ww / 2, ccy = c.wy + c.wh / 2;
    float best = 1e30f; int bi = -1;
    for (int i = 0; i < (int)g_tiles.size(); i++)
    {
        if (i == cur || !g_tiles[i].alive || g_tiles[i].pinnedFlag) continue; // M22
        float dx = (g_tiles[i].wx + g_tiles[i].ww / 2) - ccx;
        float dy = (g_tiles[i].wy + g_tiles[i].wh / 2) - ccy;
        float prim, orth;
        switch (dir)
        {
        case 0: prim = -dx; orth = dy; break;
        case 1: prim = dx;  orth = dy; break;
        case 2: prim = -dy; orth = dx; break;
        default: prim = dy; orth = dx; break;
        }
        if (prim <= 0 || fabsf(orth) > prim) continue; // yanlış yön / koni dışı
        float score = prim + 2.5f * fabsf(orth);
        if (score < best) { best = score; bi = i; }
    }
    if (bi < 0) return;
    g_focusWnd = g_tiles[bi].source;
    // odaklanan tile görüş dışındaysa kamerayı minimal kaydır (zoom korunur)
    Tile& f = g_tiles[bi];
    float pad = 48.0f / g_cam.zoom;
    float vw = g_sw / g_cam.zoom, vh = g_sh / g_cam.zoom;
    if (f.wx - pad < g_camT.x) g_camT.x = f.wx - pad;
    else if (f.wx + f.ww + pad > g_camT.x + vw) g_camT.x = f.wx + f.ww + pad - vw;
    if (f.wy - pad < g_camT.y) g_camT.y = f.wy - pad;
    else if (f.wy + f.wh + pad > g_camT.y + vh) g_camT.y = f.wy + f.wh + pad - vh;
}

static void TrySwapIn(int idx, POINT clientAnchor); // ileri bildirim
static void FocusDive()
{
    int idx = FindTileByHwnd(g_focusWnd);
    if (idx < 0) return;
    Tile& t = g_tiles[idx];
    float sx = (t.wx + t.ww / 2 - g_cam.x) * g_cam.zoom;
    float sy = (t.wy + t.wh / 2 - g_cam.y) * g_cam.zoom;
    POINT a{ (LONG)std::clamp(sx, 0.0f, (float)g_sw),
             (LONG)std::clamp(sy, 0.0f, (float)g_sh) };
    TrySwapIn(idx, a);
}

static void FocusTabCycle(bool back)
{
    if (g_tiles.empty()) return;
    std::vector<int> order((int)g_tiles.size());
    for (int i = 0; i < (int)order.size(); i++) order[i] = i;
    // M22: pinned tile'lar Tab döngüsünde değil (ekranda sabitler)
    order.erase(std::remove_if(order.begin(), order.end(),
        [](int i) { return g_tiles[i].pinnedFlag || !g_tiles[i].alive; }), order.end());
    if (order.empty()) return;
    std::sort(order.begin(), order.end(), [](int a, int b) {
        if (g_tiles[a].activeSeq != g_tiles[b].activeSeq)
            return g_tiles[a].activeSeq > g_tiles[b].activeSeq; // en son aktif önce
        return a < b;
    });
    int cur = FindTileByHwnd(g_focusWnd), pos = -1;
    for (int i = 0; i < (int)order.size(); i++) if (order[i] == cur) { pos = i; break; }
    int next = (pos < 0) ? 0
        : (pos + (back ? -1 : 1) + (int)order.size()) % (int)order.size();
    g_focusWnd = g_tiles[order[next]].source;
}

// ---- M30: IPC / scripting (named pipe \\.\pipe\SpatialCanvas) ----
// Başka süreç (script, kısayol, otomasyon) komut gönderir; pipe thread kuyruğa
// atar, ana thread MSG_IPC'de işler. driftwm `driftwm msg` eşdeğeri.
static void ProcessIpcCommand(const std::wstring& cmd)
{
    auto starts = [&](const wchar_t* p) { return cmd.rfind(p, 0) == 0; };
    if (cmd == L"fit") FitCamera(true);
    else if (cmd == L"png") g_pngRequest = true; // M52: tuvali PNG'ye aktar
    else if (starts(L"zone:")) // M54: görüş merkezine bölge ekle
    {
        Zone z; z.title = cmd.substr(5);
        z.wx = g_cam.x + (g_sw / g_cam.zoom) / 2.0f - ZONE_W / 2;
        z.wy = g_cam.y + (g_sh / g_cam.zoom) / 2.0f - ZONE_H / 2;
        if (z.title.size() > 80) z.title.resize(80);
        g_zones.push_back(z); SaveZones();
        ShowToast(TL(L"Zone added", L"Bölge eklendi"));
    }
    else if (cmd == L"zonetidy" && !g_zones.empty()) // M56: son zonu içinden düzenle (test/scripting)
        ArrangeZone((int)g_zones.size() - 1);
    else if (cmd == L"linktest" && g_tiles.size() >= 2) // M57: ilk iki tile'ı bağla (test)
        g_connectors.push_back({ g_tiles[0].source, g_tiles[1].source });
    else if (cmd == L"arrange") ArrangeGrid(); // M35
    else if (cmd == L"quit") PostQuitMessage(0);
    else if (starts(L"save:")) SaveNamedLayout(cmd.substr(5)); // M42
    else if (starts(L"load:")) LoadNamedLayout(cmd.substr(5));
    else if (cmd == L"pull") { if (g_activeTile >= 0) SwapOut(); else FitCamera(true); }
    else if (starts(L"launch:"))
    {
        POINT c{ g_sw / 2, g_sh / 2 };
        RunCommand(cmd.substr(7), L"", c);
    }
    else if (starts(L"search:") && g_activeTile < 0)
    {
        g_searchOpen = true; g_searchText = cmd.substr(7); UpdateMatches();
    }
    else if (starts(L"bookmark:"))
    {
        int n = _wtoi(cmd.substr(9).c_str()) - 1;
        if (n >= 0 && n < 4 && g_anchors[n].set)
        {
            g_momentum = false;
            g_camT.x = g_anchors[n].x; g_camT.y = g_anchors[n].y;
            g_camT.zoom = std::clamp(g_anchors[n].zoom, 0.02f, g_set.diveZoom * 0.95f);
        }
    }
    else if (starts(L"note:")) // M44: görüntü merkezine yapışkan not ekle
    {
        Note nt;
        nt.text = cmd.substr(5);
        nt.wx = g_cam.x + (g_sw / g_cam.zoom) / 2.0f - NOTE_W / 2;
        nt.wy = g_cam.y + (g_sh / g_cam.zoom) / 2.0f - NOTE_H / 2;
        if (nt.text.size() > 280) nt.text.resize(280);
        g_notes.push_back(nt);
        SaveNotes();
        ShowToast(TL(L"Note added", L"Not eklendi"));
    }
}

// M48: "a.b.c" sürüm karşılaştır - feed > yerel ise true
static bool VersionNewer(const std::wstring& feed, const std::wstring& local)
{
    int f[3] = { 0,0,0 }, l[3] = { 0,0,0 };
    swscanf_s(feed.c_str(), L"%d.%d.%d", &f[0], &f[1], &f[2]);
    swscanf_s(local.c_str(), L"%d.%d.%d", &l[0], &l[1], &l[2]);
    for (int i = 0; i < 3; i++) if (f[i] != l[i]) return f[i] > l[i];
    return false;
}

// M48: sürüm feed'ini çek (arka plan thread; ağ yoksa/başarısızsa SESSİZ - asla çökmez).
// Sadece BİLDİRİM - indirme/kurulum YOK. Feed = raw "0.47.0" metni.
static void UpdateCheckThread(std::wstring url)
{
    if (url.empty()) return;
    std::string body;
    HINTERNET hNet = InternetOpenW(L"SpatialCanvas-UpdateCheck",
        INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (hNet)
    {
        HINTERNET hUrl = InternetOpenUrlW(hNet, url.c_str(), nullptr, 0,
            INTERNET_FLAG_NO_UI | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
        if (hUrl)
        {
            char buf[128]; DWORD rd = 0;
            while (body.size() < 64 && InternetReadFile(hUrl, buf, sizeof(buf), &rd) && rd > 0)
                body.append(buf, rd);
            InternetCloseHandle(hUrl);
        }
        InternetCloseHandle(hNet);
    }
    // ilk satır, sadece sürüm karakterleri (rakam/nokta) - HTML hata sayfası vs. elenir
    std::wstring ver;
    for (char c : body)
    {
        if (c == '\r' || c == '\n') break;
        if ((c >= '0' && c <= '9') || c == '.') ver += (wchar_t)c;
        else if (c == ' ' && ver.empty()) continue; // baştaki boşluk
        else break; // beklenmeyen karakter → feed bozuk, dur
    }
    if (ver.empty() || !VersionNewer(ver, APP_VERSION)) return;
    { std::lock_guard<std::mutex> lk(g_updateMutex); g_updateVer = ver; }
    if (g_hwnd) PostMessageW(g_hwnd, MSG_UPDATE, 0, 0);
}

static void IpcServerThread()
{
    for (;;)
    {
        HANDLE pipe = CreateNamedPipeW(L"\\\\.\\pipe\\SpatialCanvas",
            PIPE_ACCESS_INBOUND,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1, 0, 4096, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) { Sleep(1000); continue; }
        BOOL ok = ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
        if (ok)
        {
            char buf[1024]; DWORD rd = 0;
            if (ReadFile(pipe, buf, sizeof(buf) - 1, &rd, nullptr) && rd > 0)
            {
                buf[rd] = 0;
                int wl = MultiByteToWideChar(CP_UTF8, 0, buf, -1, nullptr, 0);
                if (wl > 1)
                {
                    std::wstring w(wl - 1, 0);
                    MultiByteToWideChar(CP_UTF8, 0, buf, -1, &w[0], wl);
                    {
                        std::lock_guard<std::mutex> lk(g_ipcMutex);
                        g_ipcQueue.push_back(std::move(w));
                    }
                    if (g_hwnd) PostMessageW(g_hwnd, MSG_IPC, 0, 0);
                }
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

static LRESULT CALLBACK CanvasProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    // M16: explorer yeniden başladı - yeni görev çubuğu görünür + topmost
    // doğar, tuval modundaysak gizle ve z-bandını yeniden kur
    if (g_msgTaskbarCreated && msg == g_msgTaskbarCreated)
    {
        if (g_activeTile < 0 && GetForegroundWindow() == hwnd)
        {
            ShowTaskbars(false);
            RaiseCanvasTopmost();
        }
        return 0;
    }
    switch (msg)
    {
    case WM_ACTIVATE:
        // M11: tuval öndeyken görev çubuğu gizli; odak gidince geri gelir
        ShowTaskbars(LOWORD(wp) == WA_INACTIVE);
        return 0;
    case WM_MOUSEWHEEL:
    {
        // M23/M24: modal overlay açıkken zoom-dalış görünmez tuzak/garip - kilitle
        if (g_launchOpen || g_searchOpen || g_firstRun || g_helpOpen) return 0;
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &p);
        if (OverPanelUi(p)) return 0; // panel/dişli üstünde zoom yapma
        g_momentum = false; // M12: zoom flick'i keser
        float factor = (GET_WHEEL_DELTA_WPARAM(wp) > 0) ? 1.15f : 1.0f / 1.15f;
        // Zoom hedef kamera üzerinden; render yumuşakça takip eder (M3)
        float worldX = g_camT.x + p.x / g_camT.zoom;
        float worldY = g_camT.y + p.y / g_camT.zoom;
        g_camT.zoom = std::clamp(g_camT.zoom * factor, 0.02f, 4.0f);
        g_camT.x = worldX - p.x / g_camT.zoom;
        g_camT.y = worldY - p.y / g_camT.zoom;
        if (g_camT.zoom < 0.75f) g_swapArmed = true;
        // M2: eşik geçildi ve imleç bir tile üstündeyse gerçek pencereye geç
        if (g_activeTile < 0 && g_swapArmed && g_camT.zoom >= g_set.diveZoom && factor > 1.0f)
        {
            int hit = HitTile(g_camT.x + p.x / g_camT.zoom,
                              g_camT.y + p.y / g_camT.zoom);
            if (hit >= 0) TrySwapIn(hit, p);
        }
        return 0;
    }
    case WM_MOUSEHWHEEL: // M32: touchpad iki-parmak yatay kaydırma = yatay pan
    {
        if (g_launchOpen || g_searchOpen || g_activeTile >= 0) return 0;
        float d = (float)GET_WHEEL_DELTA_WPARAM(wp) / 120.0f;
        float dx = d * 90.0f / g_cam.zoom; // sağ-yatay → dünyada sağa kay
        g_cam.x += dx;  g_camT.x += dx;
        return 0;
    }
    case WM_MBUTTONDOWN:
        // M10: orta tık bir eyleme atanmışsa pan yerine onu çalıştır
        if (g_captureRow >= 0) { AssignCapture(VK_MBUTTON, CurMods()); return 0; }
        if (ExecuteBoundAction(VK_MBUTTON, CurMods())) return 0;
        [[fallthrough]];
    case WM_RBUTTONDOWN:
        // M26: sağ uygulama dock'u ikonuna SAĞ-TIK = kaldır (orta-tık fallthrough hariç)
        if (msg == WM_RBUTTONDOWN && g_appDockA > 0.5f && !g_appDockChips.empty())
        {
            POINT rp{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            for (auto& ach : g_appDockChips)
                if (ach.idx >= 0 && rp.x >= ach.rect.left && rp.x <= ach.rect.right &&
                    rp.y >= ach.rect.top && rp.y <= ach.rect.bottom)
                {
                    RemoveLauncher(ach.idx);
                    return 0;
                }
        }
        g_panning = true;
        g_momentum = false; // M12: yeni pan, süren flick'i keser
        g_panVX = g_panVY = 0;
        g_panTick = GetTickCount64();
        g_lastMouse = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        SetCapture(hwnd);
        return 0;
    case WM_XBUTTONDOWN:
    {
        // M10: hook yutmadıysa (geri çekil'e atanmamış yan tuş) buraya düşer
        int xvk = (HIWORD(wp) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        if (g_captureRow >= 0) { AssignCapture(xvk, CurMods()); return TRUE; }
        ExecuteBoundAction(xvk, CurMods());
        return TRUE;
    }
    case WM_XBUTTONUP:
        return TRUE;
    case WM_MBUTTONUP: case WM_RBUTTONUP:
        g_panning = false;
        // M12: hızlı bırakıldıysa flick - kamera momentumla süzülür
        if ((GetTickCount64() - g_panTick) < 90 &&
            (fabsf(g_panVX) + fabsf(g_panVY)) * g_cam.zoom > 120.0f)
            g_momentum = true;
        // M46 düzeltme: pan-tuşu bırakılırken DEVAM EDEN sol-sürüklemenin capture'ını
        // koru (eskiden sadece g_dragTile bakılıyordu - grup/marquee/pin/not-sürükle/
        // boyutlandır sırasında orta-tık+bırakma sürüklemeyi koparıyordu)
        {
            bool anyDrag = g_dragTile >= 0 || g_groupDrag || g_marquee || g_pinDrag ||
                           g_dragNote >= 0 || g_resizeNote >= 0 ||
                           g_dragZone >= 0 || g_resizeZone >= 0 || g_connecting; // M54/M55/M57
            if (!anyDrag) ReleaseCapture();
        }
        return 0;
    case WM_CAPTURECHANGED: // M57: capture beklenmedik şekilde gitti - bağlantı kurmayı iptal et
        g_connecting = false; g_connectFrom = nullptr;
        return 0;
    case WM_LBUTTONDBLCLK:
    {
        // M9: çift tık = doğrudan dal (zoom eşiğini beklemeden)
        POINT cp{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (OverPanelUi(cp)) { HandlePanelClick(cp); return 0; }
        if (g_activeTile >= 0) return 0;
        // M44: nota çift tık = düzenle (tile dalışından önce; not üstte yüzer)
        {
            int nh = NoteAt(g_cam.x + cp.x / g_cam.zoom, g_cam.y + cp.y / g_cam.zoom);
            if (nh >= 0) { g_editNote = nh; g_dragNote = -1; return 0; }
            // M54: zon başlık-çubuğuna çift tık = başlığı düzenle
            int zh = ZoneTitleAt(g_cam.x + cp.x / g_cam.zoom, g_cam.y + cp.y / g_cam.zoom);
            if (zh >= 0) { g_editZone = zh; g_dragZone = -1; return 0; }
        }
        // M22: pinned tile'a çift tık = çöz + dal (imleç merkezli dünyaya koy)
        int pdb = HitPinned(cp);
        if (pdb >= 0)
        {
            Tile& pt = g_tiles[pdb];
            pt.pinnedFlag = false;
            pt.wx = g_cam.x + cp.x / g_cam.zoom - pt.ww / 2;
            pt.wy = g_cam.y + cp.y / g_cam.zoom - pt.wh / 2;
            g_dragTile = -1; ReleaseCapture();
            TrySwapIn(pdb, cp);
            return 0;
        }
        int hit = HitTile(g_cam.x + cp.x / g_cam.zoom,
                          g_cam.y + cp.y / g_cam.zoom);
        if (hit >= 0)
        {
            g_dragTile = -1;
            ReleaseCapture();
            TrySwapIn(hit, cp);
        }
        else if (g_activeTile < 0) // M60: boş tuvale çift tık = hızlı yapışkan not (boşsa oto-silinir)
        {
            Note n; n.color = g_lastNoteColor; // M64
            n.wx = g_cam.x + cp.x / g_cam.zoom - NOTE_W / 2;
            n.wy = g_cam.y + cp.y / g_cam.zoom - NOTE_H / 2;
            g_notes.push_back(n);
            g_editNote = (int)g_notes.size() - 1;
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
    {
        POINT cp{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        g_momentum = false; // M12: tık flick'i keser
        // M20: ilk açılış kartı / yardım overlay'i tıkla-kapat (marquee başlatma)
        if (g_firstRun) { g_firstRun = false; SaveSettings(); return 0; }
        if (g_helpOpen) { g_helpOpen = false; return 0; }
        // M23: başlatıcı paleti açıkken - satıra tık başlatır, dışına tık kapatır
        if (g_launchOpen)
        {
            for (int i = 0; i < (int)g_launchRows.size(); i++)
                if (cp.x >= g_launchRows[i].left && cp.x <= g_launchRows[i].right &&
                    cp.y >= g_launchRows[i].top && cp.y <= g_launchRows[i].bottom)
                {
                    g_launchText.clear(); g_launchSel = i;
                    RunLauncherInput(cp);
                    return 0;
                }
            bool inBox = (cp.x >= g_launchBox.left && cp.x <= g_launchBox.right &&
                          cp.y >= g_launchBox.top && cp.y <= g_launchBox.bottom);
            if (!inBox) { g_launchOpen = false; g_launchText.clear(); g_launchSel = 0; }
            return 0;
        }
        if (HandlePanelClick(cp)) return 0; // M5: panel önceliklidir
        // M61: düzenlenen not/zon DIŞINA tık = düzenlemeyi bitir (boş not terk edilir).
        // Düzenlenen nesnenin üstüne tık = mevcut sürükle/✕ handler'ı devralır.
        if (g_activeTile < 0 && (g_editNote >= 0 || g_editZone >= 0))
        {
            float ewx = g_cam.x + cp.x / g_cam.zoom, ewy = g_cam.y + cp.y / g_cam.zoom;
            if (g_editNote >= 0 && g_editNote < (int)g_notes.size() && NoteAt(ewx, ewy) != g_editNote)
            {
                if (g_notes[g_editNote].text.empty()) g_notes.erase(g_notes.begin() + g_editNote);
                g_editNote = -1; SaveNotes();
            }
            if (g_editZone >= 0 && g_editZone < (int)g_zones.size() && ZoneTitleAt(ewx, ewy) != g_editZone)
            {
                g_editZone = -1; SaveZones();
            }
        }
        // M51: yeni-sürüm pill'ine tık → release sayfasını tarayıcıda aç (sadece bildirim)
        if (g_updateAvail && g_updateRect.right > g_updateRect.left &&
            cp.x >= g_updateRect.left && cp.x <= g_updateRect.right &&
            cp.y >= g_updateRect.top && cp.y <= g_updateRect.bottom)
        {
            ShellExecuteW(nullptr, L"open",
                L"https://github.com/13auth/spatial-canvas/releases/latest",
                nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        // M22: pinned tile sürükleme (ekran-uzayı; tuval içeriğinden önce)
        int phit = HitPinned(cp);
        if (phit >= 0)
        {
            g_pinDrag = true; g_pinDragTile = phit;
            g_pinGrab.x = cp.x - (LONG)g_tiles[phit].px;
            g_pinGrab.y = cp.y - (LONG)g_tiles[phit].py;
            SetCapture(hwnd);
            return 0;
        }
        // M6: serbest bırak çipi
        if (g_releaseTile >= 0 && g_releaseTile < (int)g_tiles.size() &&
            cp.x >= g_releaseRect.left && cp.x <= g_releaseRect.right &&
            cp.y >= g_releaseRect.top && cp.y <= g_releaseRect.bottom)
        {
            CloseTileWindow(g_releaseTile); // M25: gerçek pencereyi kapat
            g_releaseTile = -1;
            return 0;
        }
        float wx = g_cam.x + GET_X_LPARAM(lp) / g_cam.zoom;
        float wy = g_cam.y + GET_Y_LPARAM(lp) / g_cam.zoom;
        // M11: Ara butonu
        if (g_activeTile < 0 && !g_searchOpen &&
            cp.x >= g_searchBtnRect.left && cp.x <= g_searchBtnRect.right &&
            cp.y >= g_searchBtnRect.top && cp.y <= g_searchBtnRect.bottom &&
            g_searchBtnRect.right > g_searchBtnRect.left)
        {
            g_searchOpen = true;
            g_searchText.clear();
            UpdateMatches();
            return 0;
        }
        // M36: minimap tıklaması = kamerayı o dünya noktasına taşı
        // (M43: g_activeTile<0 - aktif modda minimap çizilmez, rect bayat kalır)
        if (g_activeTile < 0 && g_minimapRect.right > g_minimapRect.left &&
            cp.x >= g_minimapRect.left && cp.x <= g_minimapRect.right &&
            cp.y >= g_minimapRect.top && cp.y <= g_minimapRect.bottom && g_mmScale > 0)
        {
            float twx = g_mmMinX + (cp.x - g_mmX) / g_mmScale;
            float twy = g_mmMinY + (cp.y - g_mmY) / g_mmScale;
            g_momentum = false;
            g_camT.x = twx - (g_sw / g_camT.zoom) / 2.0f;
            g_camT.y = twy - (g_sh / g_camT.zoom) / 2.0f;
            return 0;
        }
        // M24: sağ uygulama dock'u ikonu tıklaması (launcher.txt'yi başlat)
        if (g_appDockA > 0.5f && !g_appDockChips.empty())
        {
            for (auto& ach : g_appDockChips)
                if (cp.x >= ach.rect.left && cp.x <= ach.rect.right &&
                    cp.y >= ach.rect.top && cp.y <= ach.rect.bottom)
                {
                    int i = ach.idx;
                    if (i == -1) AddLauncherViaDialog(); // M25: "+" ekle butonu
                    else if (i >= 0 && i < (int)g_launchers.size())
                        RunCommand(g_launchers[i].exe, g_launchers[i].args, cp);
                    return 0;
                }
            D2D1_RECT_F bb = g_appDockChips.front().rect; // gövdeye tık - yut
            bb.left -= 12; bb.top -= 12;
            bb.right += 12; bb.bottom = g_appDockChips.back().rect.bottom + 12;
            if (cp.x >= bb.left && cp.x <= bb.right &&
                cp.y >= bb.top && cp.y <= bb.bottom) return 0;
        }
        // M13: dock çipi tıklaması (tuval içeriğinden önceliklidir)
        if (g_dockA > 0.5f && !g_dockChips.empty())
        {
            for (auto& dch : g_dockChips)
                if (cp.x >= dch.rect.left && cp.x <= dch.rect.right &&
                    cp.y >= dch.rect.top && cp.y <= dch.rect.bottom)
                {
                    FlyToTile(dch.tile);
                    return 0;
                }
            // bar gövdesine tık - yut (altında marquee başlamasın)
            D2D1_RECT_F bb = g_dockChips.front().rect;
            bb.left -= 12; bb.top -= 12;
            bb.right = g_dockChips.back().rect.right + 12;
            bb.bottom += 12;
            if (cp.x >= bb.left && cp.x <= bb.right &&
                cp.y >= bb.top && cp.y <= bb.bottom) return 0;
        }
        // M57: bağlayıcı orta-nokta ✕ tıklaması = bağlantıyı sil (her şeyden önce)
        if (g_activeTile < 0 && g_connDelIdx >= 0 && g_connDelIdx < (int)g_connectors.size() &&
            cp.x >= g_connDelRect.left && cp.x <= g_connDelRect.right &&
            cp.y >= g_connDelRect.top && cp.y <= g_connDelRect.bottom)
        {
            g_connectors.erase(g_connectors.begin() + g_connDelIdx);
            g_connDelIdx = -1;
            return 0;
        }
        // M44: not etkileşimi (✕=sil, gövde=sürükle). Tile/marquee'den önce - not üstte yüzer.
        if (g_activeTile < 0)
        {
            if (g_noteDelIdx >= 0 && g_noteDelIdx < (int)g_notes.size() &&
                cp.x >= g_noteDelRect.left && cp.x <= g_noteDelRect.right &&
                cp.y >= g_noteDelRect.top && cp.y <= g_noteDelRect.bottom)
            {
                if (g_editNote == g_noteDelIdx) g_editNote = -1;
                else if (g_editNote > g_noteDelIdx) g_editNote--;
                g_notes.erase(g_notes.begin() + g_noteDelIdx);
                g_noteDelIdx = -1; SaveNotes();
                return 0;
            }
            // M46: boyut tutamağı (sağ-alt köşe) - NoteAt'tan ÖNCE (köşe not içinde)
            if (g_noteResIdx >= 0 && g_noteResIdx < (int)g_notes.size() &&
                cp.x >= g_noteResRect.left && cp.x <= g_noteResRect.right &&
                cp.y >= g_noteResRect.top && cp.y <= g_noteResRect.bottom)
            {
                g_editNote = -1; g_resizeNote = g_noteResIdx;
                SetCapture(hwnd);
                return 0;
            }
            int nh = NoteAt(wx, wy);
            if (nh >= 0)
            {
                g_editNote = -1; // tek tık = sürükle moduna gir
                g_dragNote = nh;
                g_noteGrabDX = wx - g_notes[nh].wx;
                g_noteGrabDY = wy - g_notes[nh].wy;
                SetCapture(hwnd);
                return 0;
            }
            // M56: zon ⊞ düzenle butonu - içindeki pencereleri ızgaraya diz
            if (g_zoneArrIdx >= 0 && g_zoneArrIdx < (int)g_zones.size() &&
                cp.x >= g_zoneArrRect.left && cp.x <= g_zoneArrRect.right &&
                cp.y >= g_zoneArrRect.top && cp.y <= g_zoneArrRect.bottom)
            {
                ArrangeZone(g_zoneArrIdx);
                return 0;
            }
            // M54: zon ✕ / boyut / başlık-çubuğu (gövde GEÇİRGEN - tile'a düşer)
            if (g_zoneDelIdx >= 0 && g_zoneDelIdx < (int)g_zones.size() &&
                cp.x >= g_zoneDelRect.left && cp.x <= g_zoneDelRect.right &&
                cp.y >= g_zoneDelRect.top && cp.y <= g_zoneDelRect.bottom)
            {
                if (g_editZone == g_zoneDelIdx) g_editZone = -1;
                else if (g_editZone > g_zoneDelIdx) g_editZone--;
                g_zones.erase(g_zones.begin() + g_zoneDelIdx);
                g_zoneDelIdx = -1; SaveZones();
                return 0;
            }
            if (g_zoneResIdx >= 0 && g_zoneResIdx < (int)g_zones.size() &&
                cp.x >= g_zoneResRect.left && cp.x <= g_zoneResRect.right &&
                cp.y >= g_zoneResRect.top && cp.y <= g_zoneResRect.bottom)
            {
                g_editZone = -1; g_resizeZone = g_zoneResIdx;
                SetCapture(hwnd);
                return 0;
            }
            int zh = ZoneTitleAt(wx, wy);
            if (zh >= 0)
            {
                g_editZone = -1; g_dragZone = zh;
                g_zoneGrabDX = wx - g_zones[zh].wx;
                g_zoneGrabDY = wy - g_zones[zh].wy;
                // M55: zon içindeki (merkezi zonda olan) tile'ları yakala → birlikte taşı
                Zone& z = g_zones[zh];
                g_zoneDragX0 = z.wx; g_zoneDragY0 = z.wy;
                g_zoneTiles.clear();
                for (auto& t : g_tiles)
                {
                    if (t.pinnedFlag) continue;
                    float cxw = t.wx + t.ww / 2, cyw = t.wy + t.wh / 2;
                    if (cxw >= z.wx && cxw <= z.wx + z.w && cyw >= z.wy && cyw <= z.wy + z.h)
                        g_zoneTiles.push_back({ t.source, t.wx, t.wy });
                }
                SetCapture(hwnd);
                return 0;
            }
        }
        int hit = HitTile(wx, wy);
        if (hit >= 0)
        {
            // M57: Ctrl+sürükle bir tile'dan = bağlayıcı ok kur (taşıma yerine)
            if ((GetKeyState(VK_CONTROL) & 0x8000) && !g_tiles[hit].pinnedFlag)
            {
                g_connecting = true; g_connectFrom = g_tiles[hit].source;
                SetCapture(hwnd);
                return 0;
            }
            // M11: tıklanan tile seçiliyse ve seçim çoklu ise GRUP taşı
            if (g_selSet.count(g_tiles[hit].source) && g_selSet.size() > 1)
            {
                g_groupDrag = true;
                g_grpX0 = wx; g_grpY0 = wy;
                g_grp.clear();
                for (auto& t : g_tiles)
                    if (g_selSet.count(t.source))
                        g_grp.push_back({ t.source, t.wx, t.wy });
                SetCapture(hwnd);
                return 0;
            }
            // M14: Shift+sürükle = yapışık kümeyi birlikte taşı (driftwm)
            if (GetKeyState(VK_SHIFT) & 0x8000)
            {
                std::vector<int> st{ hit };
                std::unordered_set<HWND> clus{ g_tiles[hit].source };
                const float E = 3.0f; // bitişiklik toleransı (dünya px)
                while (!st.empty())
                {
                    int ci = st.back(); st.pop_back();
                    Tile& c = g_tiles[ci];
                    for (int j = 0; j < (int)g_tiles.size(); j++)
                    {
                        Tile& o = g_tiles[j];
                        if (clus.count(o.source)) continue;
                        bool tX = fabsf(c.wx - (o.wx + o.ww)) < E ||
                                  fabsf((c.wx + c.ww) - o.wx) < E;
                        bool tY = fabsf(c.wy - (o.wy + o.wh)) < E ||
                                  fabsf((c.wy + c.wh) - o.wy) < E;
                        bool ovY = c.wy < o.wy + o.wh + E && c.wy + c.wh > o.wy - E;
                        bool ovX = c.wx < o.wx + o.ww + E && c.wx + c.ww > o.wx - E;
                        if ((tX && ovY) || (tY && ovX))
                        {
                            clus.insert(o.source);
                            st.push_back(j);
                        }
                    }
                }
                if (clus.size() > 1)
                {
                    g_groupDrag = true;
                    g_grpX0 = wx; g_grpY0 = wy;
                    g_grp.clear();
                    for (auto& t : g_tiles)
                        if (clus.count(t.source))
                            g_grp.push_back({ t.source, t.wx, t.wy });
                    SetCapture(hwnd);
                    return 0;
                }
            }
            g_selSet.clear(); // seçili olmayana tık = seçimi bırak, tek sürükle
            g_dragTile = hit;
            g_grabDX = wx - g_tiles[hit].wx;
            g_grabDY = wy - g_tiles[hit].wy;
            SetCapture(hwnd);
        }
        else
        {
            // M11: boş alanda sürükleme = marquee seçim
            g_marquee = true;
            g_marqAX = g_marqBX = wx;
            g_marqAY = g_marqBY = wy;
            SetCapture(hwnd);
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (g_connecting) // M57: bağlantıyı bırakılan tile'a tamamla
        {
            POINT up{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            int th = HitTile(g_cam.x + up.x / g_cam.zoom, g_cam.y + up.y / g_cam.zoom);
            if (th >= 0 && g_tiles[th].source != g_connectFrom && !g_tiles[th].pinnedFlag)
            {
                HWND b = g_tiles[th].source;
                bool dup = false;
                for (auto& c : g_connectors)
                    if ((c.a == g_connectFrom && c.b == b) || (c.a == b && c.b == g_connectFrom)) dup = true;
                if (!dup) { g_connectors.push_back({ g_connectFrom, b });
                    ShowToast(TL(L"Linked", L"Bağlandı")); }
            }
            g_connecting = false; g_connectFrom = nullptr;
            if (!g_panning) ReleaseCapture();
            return 0;
        }
        if (g_dragNote >= 0) { g_dragNote = -1; SaveNotes(); // M44: not taşımayı kalıcılaştır
            if (!g_panning) ReleaseCapture(); return 0; }
        if (g_resizeNote >= 0) { g_resizeNote = -1; SaveNotes(); // M46: boyutu kalıcılaştır
            if (!g_panning) ReleaseCapture(); return 0; }
        if (g_dragZone >= 0) { g_dragZone = -1; SaveZones(); // M54
            if (!g_zoneTiles.empty()) { SaveLayout(); g_zoneTiles.clear(); } // M55: taşınan tile'lar
            if (!g_panning) ReleaseCapture(); return 0; }
        if (g_resizeZone >= 0) { g_resizeZone = -1; SaveZones(); // M54
            if (!g_panning) ReleaseCapture(); return 0; }
        if (g_pinDrag) { g_pinDrag = false; g_pinDragTile = -1; // M22
            if (!g_panning) ReleaseCapture(); return 0; }
        if (g_dragTile >= 0 || g_groupDrag) SaveLayout(); // yerleşimi kalıcılaştır
        g_dragTile = -1;
        if (g_groupDrag) { g_groupDrag = false; g_grp.clear(); }
        if (g_marquee) // M11: seçimi sonuçlandır
        {
            g_marquee = false;
            float L = std::min(g_marqAX, g_marqBX), R = std::max(g_marqAX, g_marqBX);
            float T = std::min(g_marqAY, g_marqBY), B = std::max(g_marqAY, g_marqBY);
            g_selSet.clear();
            if ((R - L) * g_cam.zoom > 8.0f || (B - T) * g_cam.zoom > 8.0f)
            {
                for (auto& t : g_tiles)
                    if (!t.pinnedFlag && // M22: pinned marquee'ye girmez
                        t.wx < R && t.wx + t.ww > L && t.wy < B && t.wy + t.wh > T)
                        g_selSet.insert(t.source);
            }
        }
        if (!g_panning) ReleaseCapture();
        return 0;
    case WM_MOUSEMOVE:
    {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (g_dragNote >= 0 && g_dragNote < (int)g_notes.size()) // M44: notu dünyada taşı
        {
            g_notes[g_dragNote].wx = g_cam.x + p.x / g_cam.zoom - g_noteGrabDX;
            g_notes[g_dragNote].wy = g_cam.y + p.y / g_cam.zoom - g_noteGrabDY;
            g_lastMouse = p;
            return 0;
        }
        if (g_resizeNote >= 0 && g_resizeNote < (int)g_notes.size()) // M46: notu boyutlandır
        {
            Note& n = g_notes[g_resizeNote];
            n.w = std::clamp(g_cam.x + p.x / g_cam.zoom - n.wx, NOTE_MIN_W, NOTE_MAX);
            n.h = std::clamp(g_cam.y + p.y / g_cam.zoom - n.wy, NOTE_MIN_H, NOTE_MAX);
            g_lastMouse = p;
            return 0;
        }
        if (g_dragZone >= 0 && g_dragZone < (int)g_zones.size()) // M54: zonu taşı
        {
            Zone& z = g_zones[g_dragZone];
            z.wx = g_cam.x + p.x / g_cam.zoom - g_zoneGrabDX;
            z.wy = g_cam.y + p.y / g_cam.zoom - g_zoneGrabDY;
            // M55: içindeki tile'ları zonla aynı delta kadar taşı (grup-taşıma)
            float dx = z.wx - g_zoneDragX0, dy = z.wy - g_zoneDragY0;
            for (auto& gi : g_zoneTiles)
                for (auto& t : g_tiles)
                    if (t.source == gi.h) { t.wx = gi.x0 + dx; t.wy = gi.y0 + dy; }
            g_lastMouse = p; return 0;
        }
        if (g_resizeZone >= 0 && g_resizeZone < (int)g_zones.size()) // M54: zonu boyutlandır
        {
            Zone& z = g_zones[g_resizeZone];
            z.w = std::clamp(g_cam.x + p.x / g_cam.zoom - z.wx, ZONE_MIN_W, 8000.0f);
            z.h = std::clamp(g_cam.y + p.y / g_cam.zoom - z.wy, ZONE_MIN_H, 8000.0f);
            g_lastMouse = p; return 0;
        }
        if (g_pinDrag && g_pinDragTile >= 0 && g_pinDragTile < (int)g_tiles.size())
        {
            // M22: pinned tile ekran-uzayında taşınır (ekran içine kıstır)
            Tile& t = g_tiles[g_pinDragTile];
            t.px = ClampPin((float)(p.x - g_pinGrab.x), g_uiX, g_uiX + g_priW - t.pw);
            t.py = ClampPin((float)(p.y - g_pinGrab.y), g_uiY, g_uiY + g_priH - t.ph);
        }
        else if (g_panning)
        {
            float dx = (p.x - g_lastMouse.x) / g_cam.zoom;
            float dy = (p.y - g_lastMouse.y) / g_cam.zoom;
            g_cam.x -= dx; g_cam.y -= dy;     // 1:1 his (gerçek)
            g_camT.x -= dx; g_camT.y -= dy;   // hedef de takip etsin
            // M12: flick hızı (üstel düzleştirme - tek seğirme flick sayılmaz)
            ULONGLONG nt = GetTickCount64();
            float pdt = (nt - g_panTick) / 1000.0f;
            g_panTick = nt;
            if (pdt > 0.0f && pdt < 0.2f)
            {
                float ax = std::clamp(pdt * 12.0f, 0.0f, 1.0f);
                g_panVX += (-dx / pdt - g_panVX) * ax;
                g_panVY += (-dy / pdt - g_panVY) * ax;
            }
        }
        else if (g_groupDrag) // M11: seçili grubu birlikte taşı
        {
            float dx = g_cam.x + p.x / g_cam.zoom - g_grpX0;
            float dy = g_cam.y + p.y / g_cam.zoom - g_grpY0;
            for (auto& gi : g_grp)
                for (auto& t : g_tiles)
                    if (t.source == gi.h) { t.wx = gi.x0 + dx; t.wy = gi.y0 + dy; }
        }
        else if (g_marquee) // M11: seçim dikdörtgenini büyüt
        {
            g_marqBX = g_cam.x + p.x / g_cam.zoom;
            g_marqBY = g_cam.y + p.y / g_cam.zoom;
        }
        else if (g_dragTile >= 0 && g_dragTile < (int)g_tiles.size())
        {
            g_tiles[g_dragTile].wx = g_cam.x + p.x / g_cam.zoom - g_grabDX;
            g_tiles[g_dragTile].wy = g_cam.y + p.y / g_cam.zoom - g_grabDY;
            // M14: kenar yapışması (Alt = serbest taşıma)
            if (!(GetKeyState(VK_MENU) & 0x8000)) SnapTile(g_dragTile);
        }
        g_lastMouse = p;
        return 0;
    }
    case MSG_GLOBAL_WHEEL:
    {
        if (g_launchOpen || g_searchOpen || g_firstRun || g_helpOpen) return 0; // M23/M24: modal kilit
        // Ctrl+Alt+Wheel her yerden gelir (LL hook); koordinatlar ekran cinsinden
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &p);
        int delta = (int)(short)wp;
        // Çalışma modunda yukarı tekerlek = yoksay (mod kilidi)
        if (g_activeTile >= 0 && delta > 0) return 0;
        // Çalışma modunda aşağı tekerlek = geri çekil (SwapOut animasyonlu döner)
        if (g_activeTile >= 0 && delta < 0) { SwapOut(); return 0; }
        float factor = (delta > 0) ? 1.15f : 1.0f / 1.15f;
        float worldX = g_camT.x + p.x / g_camT.zoom;
        float worldY = g_camT.y + p.y / g_camT.zoom;
        g_camT.zoom = std::clamp(g_camT.zoom * factor, 0.02f, 4.0f);
        g_camT.x = worldX - p.x / g_camT.zoom;
        g_camT.y = worldY - p.y / g_camT.zoom;
        if (g_camT.zoom < 0.75f) g_swapArmed = true;
        if (g_activeTile < 0 && g_swapArmed && g_camT.zoom >= g_set.diveZoom && factor > 1.0f)
        {
            int hit = HitTile(g_camT.x + p.x / g_camT.zoom,
                              g_camT.y + p.y / g_camT.zoom);
            if (hit >= 0) TrySwapIn(hit, p);
        }
        return 0;
    }
    case MSG_PULLBACK:
        // Fare ileri tuşu: çalışma modunda = tuvale dön (aynı manzara),
        // tuvaldeyken = tümünü sığdır
        if (g_activeTile >= 0) SwapOut();
        else FitCamera(true); // M17: geri çekil jesti her zaman TÜMÜNÜ gösterir
        return 0;
    case MSG_IPC: // M30: named pipe komutlarını işle (ana thread'de)
    {
        std::vector<std::wstring> cmds;
        { std::lock_guard<std::mutex> lk(g_ipcMutex); cmds.swap(g_ipcQueue); }
        for (auto& c : cmds)
        {
            std::wstring t = c;
            while (!t.empty() && (t.back() == L'\n' || t.back() == L'\r' || t.back() == L' '))
                t.pop_back();
            if (!t.empty()) ProcessIpcCommand(t);
        }
        return 0;
    }
    case MSG_UPDATE: // M48: feed'de yeni sürüm bulundu - bildir (indirme YOK)
    {
        std::wstring v;
        { std::lock_guard<std::mutex> lk(g_updateMutex); v = g_updateVer; }
        if (!v.empty())
        {
            g_updateAvail = true;
            ShowToast(TL(L"New version ", L"Yeni sürüm ") + v +
                TL(L" available — github.com/13auth/spatial-canvas",
                   L" mevcut — github.com/13auth/spatial-canvas"));
        }
        return 0;
    }
    case WM_HOTKEY:
        if (wp == HOTKEY_TOGGLE)
        {
            if (g_activeTile >= 0) SwapOut();
            else ForceForeground(hwnd);
        }
        return 0;
    case WM_KEYDOWN:
    {
        int vk = (int)wp;
        int mods = ((GetKeyState(VK_CONTROL) & 0x8000) ? 1 : 0)
                 | ((GetKeyState(VK_MENU)    & 0x8000) ? 2 : 0)
                 | ((GetKeyState(VK_SHIFT)   & 0x8000) ? 4 : 0);
        auto is = [&](Key k) { return vk == k.vk && mods == k.mods; };
        // M20: ilk açılış kartı herhangi bir tuşla kapanır (tuş işlenmeye devam)
        if (g_firstRun) { g_firstRun = false; SaveSettings(); }
        if (g_launchOpen) // M23: başlatıcı paleti tüm tuşları yutar
        {
            if (vk == VK_ESCAPE || is(g_set.kbLaunch))
            {
                g_launchOpen = false; g_launchText.clear(); g_launchSel = 0; return 0;
            }
            if (vk == VK_RETURN)
            {
                POINT cur{}; GetCursorPos(&cur); ScreenToClient(hwnd, &cur);
                RunLauncherInput(cur);
                return 0;
            }
            // boş giriş: rakam (1-9) doğrudan kısayolu başlatır
            if (g_launchText.empty() && vk >= '1' && vk <= '9' &&
                (vk - '1') < (int)g_launchers.size())
            {
                POINT cur{}; GetCursorPos(&cur); ScreenToClient(hwnd, &cur);
                g_launchSel = vk - '1';
                RunLauncherInput(cur);
                return 0;
            }
            if (vk == VK_DOWN && !g_launchers.empty())
                g_launchSel = (g_launchSel + 1) % (int)g_launchers.size();
            else if (vk == VK_UP && !g_launchers.empty())
                g_launchSel = (g_launchSel + (int)g_launchers.size() - 1) % (int)g_launchers.size();
            return 0; // karakterler WM_CHAR'da
        }
        if (g_searchOpen) // M9: arama modu tüm tuşları yutar
        {
            if (vk == VK_ESCAPE || is(g_set.kbSearch)) { CloseSearch(); return 0; }
            int total = (int)(g_matches.size() + g_noteMatches.size() + g_zoneMatches.size()); // M49/M67
            if (vk == VK_RETURN)
            {
                if (total > 0) FlyToSearchSel(); // M49: tile ya da nota uç
                CloseSearch();
                return 0;
            }
            if (vk == VK_DOWN || vk == VK_TAB)
            {
                if (total > 0) g_searchSel = (g_searchSel + 1) % total;
                return 0;
            }
            if (vk == VK_UP)
            {
                if (total > 0) g_searchSel = (g_searchSel + total - 1) % total;
                return 0;
            }
            return 0; // karakterler WM_CHAR'da işlenir
        }
        // M44: not düzenleme modu - tuşları yutar (metin WM_CHAR'da); Enter/Esc bitirir,
        // Tab renk döngüsü, Delete notu siler. Esc-merdiveni/Delete-seçim'den ÖNCE.
        if (g_editNote >= 0 && g_activeTile < 0)
        {
            if (g_editNote >= (int)g_notes.size()) { g_editNote = -1; return 0; }
            if (vk == VK_ESCAPE || vk == VK_RETURN)
            {
                if (g_notes[g_editNote].text.empty()) // boş not = iptal, sil
                    g_notes.erase(g_notes.begin() + g_editNote);
                g_editNote = -1; SaveNotes(); return 0;
            }
            if (vk == VK_TAB) // renk döngüsü
            {
                g_notes[g_editNote].color = (g_notes[g_editNote].color + 1) & 3;
                g_lastNoteColor = g_notes[g_editNote].color; // M64
                SaveNotes(); return 0;
            }
            if (vk == VK_DELETE)
            {
                g_notes.erase(g_notes.begin() + g_editNote);
                g_editNote = -1; SaveNotes(); return 0;
            }
            return 0; // karakter/backspace WM_CHAR'da
        }
        // M54: zon başlık düzenleme modu (notlarla aynı; boş başlık iptal etmez - zon kalır)
        if (g_editZone >= 0 && g_activeTile < 0)
        {
            if (g_editZone >= (int)g_zones.size()) { g_editZone = -1; return 0; }
            if (vk == VK_ESCAPE || vk == VK_RETURN) { g_editZone = -1; SaveZones(); return 0; }
            if (vk == VK_TAB) { g_zones[g_editZone].color = (g_zones[g_editZone].color + 1) & 3; g_lastZoneColor = g_zones[g_editZone].color; SaveZones(); return 0; } // M64
            if (vk == VK_DELETE) { g_zones.erase(g_zones.begin() + g_editZone); g_editZone = -1; SaveZones(); return 0; }
            return 0;
        }
        // M21: ok tuşları = odak gezinme (autorepeat'ten ÖNCE: tutunca adımlasın)
        if (g_captureRow < 0 && !g_helpOpen && g_activeTile < 0 && mods == 0 &&
            (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN))
        {
            MoveFocusDir(vk == VK_LEFT ? 0 : vk == VK_RIGHT ? 1 : vk == VK_UP ? 2 : 3);
            return 0;
        }
        // M38: Shift+ok = odaklı tile'ı dünyada taşı (klavye-only düzenleme)
        if (g_captureRow < 0 && !g_helpOpen && g_activeTile < 0 && mods == 4 &&
            g_focusWnd && (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN))
        {
            int fi = FindTileByHwnd(g_focusWnd);
            if (fi >= 0 && !g_tiles[fi].pinnedFlag)
            {
                float step = 60.0f / g_cam.zoom; // ekranda ~60px adım
                if (vk == VK_LEFT)  g_tiles[fi].wx -= step;
                if (vk == VK_RIGHT) g_tiles[fi].wx += step;
                if (vk == VK_UP)    g_tiles[fi].wy -= step;
                if (vk == VK_DOWN)  g_tiles[fi].wy += step;
                // M43: autorepeat'te her tuşta diske yazma - 500ms throttle
                static ULONGLONG s_lastMoveSave = 0;
                ULONGLONG nowT = GetTickCount64();
                if (nowT - s_lastMoveSave > 500) { SaveLayout(); s_lastMoveSave = nowT; }
            }
            return 0;
        }
        if (lp & 0x40000000) return 0; // autorepeat yoksay
        if (g_captureRow >= 0) // M8/M10: yakalama modu HER ŞEYDEN önce -
        {                      // sabit kısayollar atamayı gölgelemesin
            if (vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT ||
                vk == VK_LWIN || vk == VK_RWIN) return 0; // asıl tuşu bekle
            AssignCapture(vk, mods);
            return 0;
        }
        // M20: F1 kısayol listesi - açıkken Esc/F1 kapatır, diğer tuşları yutar
        if (g_helpOpen)
        {
            if (vk == VK_ESCAPE || vk == VK_F1) g_helpOpen = false;
            return 0;
        }
        if (vk == VK_F1 && mods == 0) { g_helpOpen = true; return 0; }
        // M21: Tab MRU döngü, Enter odaklı tile'a dal, Esc odağı temizle
        if (g_activeTile < 0 && vk == VK_TAB && (mods == 0 || mods == 4))
        {
            FocusTabCycle(mods == 4); return 0;
        }
        if (g_activeTile < 0 && vk == VK_RETURN && mods == 0 && g_focusWnd)
        {
            FocusDive(); return 0;
        }
        if (vk == VK_ESCAPE && g_connecting) // M57: bağlantı kurmayı iptal et
        {
            g_connecting = false; g_connectFrom = nullptr;
            if (!g_panning) ReleaseCapture(); return 0;
        }
        if (vk == VK_ESCAPE && mods == 0 && g_focusWnd && g_activeTile < 0)
        {
            g_focusWnd = nullptr; return 0; // bir ESC odağı bırakır, sonraki çıkar
        }
        // M11: sabit kısayollar (Delete: seçilileri çıkar, Ctrl+C/V: çoğalt)
        if (vk == VK_DELETE && !g_selSet.empty()) { RemoveSelected(); return 0; }
        // M65: seçim yokken Delete = hover'daki notu/bölgeyi sil (✕'ten hızlı)
        if (vk == VK_DELETE && g_selSet.empty() && g_activeTile < 0 && g_editNote < 0 && g_editZone < 0)
        {
            if (g_hoverNote >= 0 && g_hoverNote < (int)g_notes.size())
            { g_notes.erase(g_notes.begin() + g_hoverNote); g_hoverNote = -1; SaveNotes(); return 0; }
            if (g_hoverZone >= 0 && g_hoverZone < (int)g_zones.size())
            { g_zones.erase(g_zones.begin() + g_hoverZone); g_hoverZone = -1; SaveZones(); return 0; }
        }
        if (mods == 1 && (vk == 'C' || vk == 'V') && g_activeTile < 0)
        {
            POINT cur{}; GetCursorPos(&cur); ScreenToClient(hwnd, &cur);
            if (vk == 'C') CopySelection(cur); else PasteCopies(cur);
            return 0;
        }
        // M22: Ctrl+P - imleç altındaki tile'ı ekrana sabitle/çöz
        if (mods == 1 && vk == 'P' && g_activeTile < 0)
        {
            POINT cur{}; GetCursorPos(&cur); ScreenToClient(hwnd, &cur);
            TogglePin(cur);
            return 0;
        }
        // M35: Ctrl+G - tüm tile'ları düzgün ızgaraya diz
        if (mods == 1 && vk == 'G' && g_activeTile < 0) { ArrangeGrid(); return 0; }
        // M68: Ctrl+A - tüm (pinned olmayan) pencereleri seç (sonra Ctrl+G / Delete / grup-taşı)
        if (mods == 1 && vk == 'A' && g_activeTile < 0)
        {
            g_selSet.clear();
            for (auto& t : g_tiles) if (!t.pinnedFlag) g_selSet.insert(t.source);
            if (!g_selSet.empty()) ShowToast(std::to_wstring((int)g_selSet.size()) +
                TL(L" windows selected", L" pencere seçildi"));
            return 0;
        }
        // M44: Ctrl+Shift+N - imleç konumunda yapışkan not oluştur + düzenle moduna gir
        if (mods == 5 && vk == 'N' && g_activeTile < 0)
        {
            POINT cur{}; GetCursorPos(&cur); ScreenToClient(hwnd, &cur);
            Note n; n.color = g_lastNoteColor; // M64
            n.wx = g_cam.x + cur.x / g_cam.zoom - NOTE_W / 2;
            n.wy = g_cam.y + cur.y / g_cam.zoom - NOTE_H / 2;
            g_notes.push_back(n);
            g_editNote = (int)g_notes.size() - 1;
            ShowToast(TL(L"Note: type, Tab=color, Enter=done", L"Not: yaz, Tab=renk, Enter=bitir"));
            return 0;
        }
        // M52: Ctrl+Shift+S - tuvali PNG'ye aktar (Render bir sonraki karede kaydeder)
        if (mods == 5 && vk == 'S' && g_activeTile < 0)
        {
            g_pngRequest = true;
            return 0;
        }
        // M54: Ctrl+Shift+Z - imleçte bölge/zon oluştur (başlık-çubuğu imlecin altında) + başlık düzenle
        if (mods == 5 && vk == 'Z' && g_activeTile < 0)
        {
            POINT cur{}; GetCursorPos(&cur); ScreenToClient(hwnd, &cur);
            Zone z; z.color = g_lastZoneColor; // M64
            z.wx = g_cam.x + cur.x / g_cam.zoom - ZONE_W / 2;
            z.wy = g_cam.y + cur.y / g_cam.zoom - ZONE_BAR / 2; // başlık çubuğu imleçte
            g_zones.push_back(z);
            g_editZone = (int)g_zones.size() - 1;
            ShowToast(TL(L"Zone: type a title, Enter=done; drag the title bar",
                         L"Bölge: başlık yaz, Enter=bitir; başlık çubuğundan sürükle"));
            return 0;
        }
        // M13: tuval yer imleri - Ctrl+Shift+1..4 kaydet, Ctrl+1..4 zıpla
        if (vk >= '1' && vk <= '4' && g_activeTile < 0 && (mods == 1 || mods == 5))
        {
            int ai = vk - '1';
            if (mods == 5)
            {
                g_anchors[ai] = { g_camT.x, g_camT.y, g_camT.zoom, true };
                SaveSettings();
                ShowToast(TL(L"Bookmark ", L"Yer imi ") + std::to_wstring(ai + 1) + TL(L" saved", L" kaydedildi"));
            }
            else if (g_anchors[ai].set)
            {
                g_momentum = false;
                g_camT.x = g_anchors[ai].x;
                g_camT.y = g_anchors[ai].y;
                // dalış eşiğinin altında kal: zıplama swap tetiklemez,
                // eşik üstü hedef "takılı zoom" durumu yaratırdı
                g_camT.zoom = std::clamp(g_anchors[ai].zoom, 0.02f,
                    g_set.diveZoom * 0.95f);
                if (g_camT.zoom < 0.75f) g_swapArmed = true;
            }
            else // M17: boş yer imine zıplama sessiz kalmasın
                ShowToast(TL(L"Bookmark ", L"Yer imi ") + std::to_wstring(ai + 1) +
                    TL(L" empty — save with Ctrl+Shift+", L" boş — Ctrl+Shift+") + std::to_wstring(ai + 1) + TL(L"", L" ile kaydet"));
            return 0;
        }
        if (!ExecuteBoundAction(vk, mods))
        {
            // M17: Figma kas hafızası (kullanıcı ataması her zaman önceliklidir)
            if (mods == 4 && vk == '1') FitCamera(true);  // Shift+1: tümünü sığdır
            else if (mods == 4 && vk == '2') FitCamera(); // Shift+2: seçimi sığdır
        }
        return 0;
    }
    case WM_CHAR:
    {
        if (g_editNote >= 0 && g_editNote < (int)g_notes.size()) // M44: not düzenleme
        {
            wchar_t c = (wchar_t)wp;
            if (c == 8) { if (!g_notes[g_editNote].text.empty()) g_notes[g_editNote].text.pop_back(); }
            else if (c >= 32 && g_notes[g_editNote].text.size() < 280) g_notes[g_editNote].text += c;
            // Enter/Esc commit WM_KEYDOWN'da; kaydetme commit'te (kare-başı disk yazma yok)
            return 0;
        }
        if (g_editZone >= 0 && g_editZone < (int)g_zones.size()) // M54: zon başlığı düzenleme
        {
            wchar_t c = (wchar_t)wp;
            if (c == 8) { if (!g_zones[g_editZone].title.empty()) g_zones[g_editZone].title.pop_back(); }
            else if (c >= 32 && g_zones[g_editZone].title.size() < 80) g_zones[g_editZone].title += c;
            return 0;
        }
        if (g_launchOpen) // M23: başlatıcı giriş kutusu
        {
            wchar_t c = (wchar_t)wp;
            if (c == 8) { if (!g_launchText.empty()) g_launchText.pop_back(); }
            else if (c >= 32 && g_launchText.size() < 120) g_launchText += c;
            return 0;
        }
        if (!g_searchOpen) return 0;
        wchar_t c = (wchar_t)wp;
        if (c == 8) { if (!g_searchText.empty()) g_searchText.pop_back(); }
        else if (c >= 32 && g_searchText.size() < 40) g_searchText += c;
        UpdateMatches();
        return 0;
    }
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        // M18: çözünürlük/DPI değişti - bayrakla (mesaj patlamasını birleştir;
        // gerçek iş ana döngüde, D2D referansları canlıyken ResizeBuffers
        // re-entrancy'sini önlemek için ertelenir)
        g_geomDirty = true;
        return 0;
    case WM_QUERYENDSESSION:
        // M18: kapanış/oturum sonu - süreç öldürülmeden önce pencereleri
        // yerine koy (yoksa park konumlarında, alt kenara gömülü açılırlar)
        for (auto& t : g_tiles) RestoreOriginal(t);
        ShowTaskbars(true);
        SaveLayout();
        SaveSettings(); // M50 fix: oturum-sonu/shutdown'da da kamerayı kaydet (restore)
        DeleteFileW(PendingFilePath().c_str());
        return TRUE;
    case WM_ENDSESSION:
        if (wp) // kapanış kesinleşti
        {
            for (auto& t : g_tiles) RestoreOriginal(t);
            ShowTaskbars(true);
        }
        else if (g_activeTile < 0) // kapanış İPTAL - yeniden parkla, yoksa
        {                          // restore edilen pencereler tuval altında donar
            int i = 0;
            for (auto& t : g_tiles) ParkWindow(t, i++);
            SavePendingRestore();
            RaiseCanvasTopmost();
            ShowTaskbars(false);
        }
        return 0;
    case WM_DESTROY:
        ShowTaskbars(true); // M11: görev çubuğu asla gizli kalmasın
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- Giriş noktası ----
int RunCanvasApp()
{
    // M16: tek örnek kilidi - HER ŞEYDEN önce. İkinci kopya RecoverFromCrash
    // ile canlı oturumun pending_restore'unu yer, taskbar'ı geri açar ve
    // park şeridi/LL hook için ilk kopyayla kavga ederdi.
    CreateMutexW(nullptr, TRUE, L"Local\\SpatialCanvas.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        HWND prev = FindWindowW(L"SpatialCanvasWnd", nullptr);
        if (prev) SetForegroundWindow(prev);
        return 0;
    }

    // M16: çökme filtresi - taskbar gizli + pencereler şeritte kalmasın.
    // CONTINUE_SEARCH ile WER raporu yaşar; set_terminate winrt::hresult_error
    // kaçaklarını da yakalar.
    SetUnhandledExceptionFilter(CrashFilter);
    std::set_terminate([] { EmergencyRestore(); std::abort(); });

    // DPI: fiziksel piksel hizalaması için PMv2 (manifest'te yok, koddan)
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Önceki oturum çökmüşse parkta kalan pencereleri kurtar
    RecoverFromCrash();
    ShowTaskbars(true); // M11: önceki oturum görev çubuğunu gizli bıraktıysa kurtar

    // M3: kayıtlı tile yerleşimini yükle (exe bazlı)
    LoadLayout();
    LoadNotes(); // M44: tuval notlarını geri yükle
    LoadZones(); // M54: bölge çerçevelerini geri yükle

    // M5: kullanıcı ayarlarını yükle
    LoadSettings();

    // M15: pencere kuralları (exclude listesi)
    LoadRules();

    // M23: uygulama başlatıcı kısayolları (launcher.txt)
    LoadLaunchers();

    // NOT: M16'da RequestAccessAsync(Borderless) eklenmişti - KALDIRILDI.
    // Borderless capture manifest'te graphicsCaptureWithoutBorder capability
    // + kullanıcı consent prompt'u ister (bizde yok); IsBorderRequired ile
    // birlikte tile'ları siyah bırakıyordu. Sarı çerçeve kabul edildi.

    // Önce hedef pencereleri topla (kendi penceremiz henüz yok)
    ComputeGeometry(); // M8: span + UI orijini tek yerden (canlı değişimde de kullanılır)

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = CanvasProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = L"SpatialCanvasWnd";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, L"SpatialCanvasWnd", L"Spatial Canvas",
        WS_POPUP, g_vx, g_vy, g_sw, g_sh, nullptr, nullptr, wc.hInstance, nullptr);

    // M16: explorer yeniden başlarsa yeni görev çubuğunu tekrar gizleyebilmek
    // için yayın mesajına abone ol (filtre: yükseltilmiş çalışmada UIPI düşürür)
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_hwnd, g_msgTaskbarCreated, MSGFLT_ALLOW, nullptr);

    // M30: IPC named pipe sunucusu (g_hwnd hazır - PostMessage güvenli)
    std::thread(IpcServerThread).detach();
    // M48: yeni-sürüm bildirimi (opt-in; ağ yoksa sessiz, sadece bildirim)
    if (g_set.updateCheck)
        std::thread(UpdateCheckThread, g_set.updateUrl).detach();
    // M53: bu sürüm son çalıştırılandan farklıysa = güncellendi → bir kez bildir
    if (!g_set.lastRun.empty() && g_set.lastRun != APP_VERSION)
        ShowToast(TL(L"Updated to v", L"Güncellendi: v") + std::wstring(APP_VERSION) +
            TL(L" — press F1 for shortcuts", L" — kısayollar için F1"));
    g_set.lastRun = APP_VERSION;
    SaveSettings(); // yeni sürümü kaydet (bildirim bir kez gösterilsin)

    InitD3D();
    InitD2D();
    CreateTiles();
    if (g_tiles.empty())
    {
        MessageBoxW(nullptr, TL(L"No window found to capture.", L"Yakalanacak pencere bulunamadı."),
            L"Spatial Canvas", MB_OK | MB_ICONWARNING);
        return 1;
    }
    // M50: son görünümü geri yükle (kayıtlı + açık); yoksa/ilk açılışta tümünü sığdır
    if (g_set.restoreView && g_hasSavedCam && g_loadCamZ > 0.001f)
    {
        g_camT.x = g_loadCamX; g_camT.y = g_loadCamY;
        g_camT.zoom = std::clamp(g_loadCamZ, 0.02f, g_set.diveZoom * 0.95f);
        g_cam = g_camT; // anında (animasyonsuz başla)
        if (g_camT.zoom < 0.75f) g_swapArmed = true;
    }
    else FitCamera();
    ShowWindow(g_hwnd, SW_SHOW);
    RaiseCanvasTopmost(); // M11: tuval görev çubuğunun üstünde başlar

    // M2: global hotkey + global Ctrl+Alt+Wheel hook
    ReRegisterPullHotkey(); // M8: ayarlı global geri-çekil kısayolu
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LLMouseProc,
        GetModuleHandleW(nullptr), 0);

    MSG msg{};
    g_lastTick = GetTickCount64();
    while (true)
    {
        bool dirty = false; // M19: bu kare ekranı değiştiren bir şey oldu mu?
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            dirty = true; // girdi/mesaj işlendi
        }
        // M18: çözünürlük/DPI değişti - geometri+swapchain+UI yeniden kur ve
        // park şeritlerini yeni alt kenara taşı. g_activeTile<0 kapısı şart:
        // ApplyCanvasSpan->RaiseCanvasTopmost dalıştaki aktif pencereyi
        // tuvalin altında bırakırdı (dönüşte SwapOut zaten yeniden parklar).
        if (g_geomDirty && g_activeTile < 0)
        {
            g_geomDirty = false;
            ApplyCanvasSpan();
            for (int i = 0; i < (int)g_tiles.size(); ++i)
            {
                if (g_tiles[i].parked && IsWindow(g_tiles[i].source))
                    ParkWindow(g_tiles[i], i);
                // M22: pinned HUD yeni (küçülmüş) ekranda dışarı kaçmasın
                if (g_tiles[i].pinnedFlag)
                {
                    g_tiles[i].px = ClampPin(g_tiles[i].px, g_uiX, g_uiX + g_priW - g_tiles[i].pw);
                    g_tiles[i].py = ClampPin(g_tiles[i].py, g_uiY, g_uiY + g_priH - g_tiles[i].ph);
                }
            }
            dirty = true;
        }
        // M18: cihaz kaybı kurtarma (Render/UpdateTiles bayrağı kaldırır)
        if (g_deviceLost) { HandleDeviceLost(); dirty = true; }
        // M3: yaşam döngüsü + kamera animasyonu
        size_t tilesBefore = g_tiles.size();
        SweepDeadTiles();
        AdoptNewWindows();
        if (g_tiles.size() != tilesBefore)
        {
            dirty = true; // tile geldi/gitti
            if (g_searchOpen) UpdateMatches(); // bayat g_matches indeksi → yanlış uçuş
        }
        ULONGLONG now = GetTickCount64();
        float dt = (now - g_lastTick) / 1000.0f;
        g_lastTick = now;
        if (dt > 0.1f) dt = 0.1f;
        // M19: imleci kare-başı bir kez oku (DrawOverlay + autopan + dock paylaşır)
        POINT prevCur = g_curClient;
        GetCursorPos(&g_curClient); ScreenToClient(g_hwnd, &g_curClient);
        if (g_curClient.x != prevCur.x || g_curClient.y != prevCur.y) dirty = true;
        static const float SPD[3] = { 6.0f, 12.0f, 20.0f };
        float k = 1.0f - expf(-dt * SPD[g_set.animSpeed]);
        g_cam.x += (g_camT.x - g_cam.x) * k;
        g_cam.y += (g_camT.y - g_cam.y) * k;
        g_cam.zoom += (g_camT.zoom - g_cam.zoom) * k;
        // M19: kamera oturmadıysa çizmeye devam; oturduysa hedefe snap'le
        if (fabsf(g_camT.x - g_cam.x) * g_cam.zoom > 0.1f ||
            fabsf(g_camT.y - g_cam.y) * g_cam.zoom > 0.1f ||
            fabsf(g_camT.zoom - g_cam.zoom) > 0.0005f)
            dirty = true;
        else
            g_cam = g_camT;
        // M12: momentum pan (sürtünmeli süzülme; cam+camT birlikte kayar)
        if (g_momentum)
        {
            dirty = true;
            g_cam.x += g_panVX * dt;  g_cam.y += g_panVY * dt;
            g_camT.x += g_panVX * dt; g_camT.y += g_panVY * dt;
            float fr = expf(-dt * 4.0f);
            g_panVX *= fr; g_panVY *= fr;
            if ((fabsf(g_panVX) + fabsf(g_panVY)) * g_cam.zoom < 24.0f)
                g_momentum = false;
        }
        // M12: sürükleme sırasında kenara yaklaşınca otomatik pan
        if (g_dragTile >= 0 || g_groupDrag || g_marquee)
        {
            dirty = true; // M19: aktif sürükleme her kare çizilmeli
            POINT cp = g_curClient; // M19: kare-başı tek okuma
            const float M = 36.0f, V = 1100.0f; // eşik px, hız ekran-px/sn
            float pxs = std::clamp(cp.x < M ? -(M - cp.x) / M
                : (cp.x > g_sw - M ? (M - (g_sw - cp.x)) / M : 0.0f), -1.0f, 1.0f);
            float pys = std::clamp(cp.y < M ? -(M - cp.y) / M
                : (cp.y > g_sh - M ? (M - (g_sh - cp.y)) / M : 0.0f), -1.0f, 1.0f);
            if (pxs != 0.0f || pys != 0.0f)
            {
                float adx = pxs * V * dt / g_cam.zoom;
                float ady = pys * V * dt / g_cam.zoom;
                g_cam.x += adx;  g_cam.y += ady;
                g_camT.x += adx; g_camT.y += ady;
                // sürüklenen öğe fare olayı beklemeden imleci takip etsin
                float wx = g_cam.x + cp.x / g_cam.zoom;
                float wy = g_cam.y + cp.y / g_cam.zoom;
                if (g_dragTile >= 0 && g_dragTile < (int)g_tiles.size())
                {
                    g_tiles[g_dragTile].wx = wx - g_grabDX;
                    g_tiles[g_dragTile].wy = wy - g_grabDY;
                }
                else if (g_groupDrag)
                {
                    float gdx = wx - g_grpX0, gdy = wy - g_grpY0;
                    for (auto& gi : g_grp)
                        for (auto& t : g_tiles)
                            if (t.source == gi.h) { t.wx = gi.x0 + gdx; t.wy = gi.y0 + gdy; }
                }
                else if (g_marquee) { g_marqBX = wx; g_marqBY = wy; }
            }
        }
        // M5: panel kayma animasyonu
        float pk = 1.0f - expf(-dt * 14.0f);
        if (fabsf((g_panelOpen ? 1.0f : 0.0f) - g_panelA) > 0.003f) dirty = true;
        g_panelA += ((g_panelOpen ? 1.0f : 0.0f) - g_panelA) * pk;
        // M13: dock tetikleyici + animasyon (ana monitör alt kenarı)
        {
            bool hot = false;
            if (g_activeTile < 0)
            {
                POINT dc = g_curClient; // M19: kare-başı tek okuma
                hot = dc.y >= g_uiY + (float)g_priH - 5 &&
                      dc.y <= g_uiY + (float)g_priH &&
                      dc.x >= g_uiX && dc.x <= g_uiX + (float)g_priW;
                if (!hot && g_dockA > 0.25f && !g_dockChips.empty())
                    hot = dc.y >= g_dockChips.front().rect.top - 52.0f &&
                          dc.y <= g_uiY + (float)g_priH &&
                          dc.x >= g_dockChips.front().rect.left - 28.0f &&
                          dc.x <= g_dockChips.back().rect.right + 28.0f;
            }
            if (fabsf((hot ? 1.0f : 0.0f) - g_dockA) > 0.003f) dirty = true;
            g_dockA += ((hot ? 1.0f : 0.0f) - g_dockA) * pk;
        }
        // M24: sağ kenar uygulama dock'u tetikleyici + animasyon
        {
            bool hot = false;
            if (g_activeTile < 0) // M25: boşken bile "+" için açılır
            {
                POINT dc = g_curClient;
                hot = dc.x >= g_uiX + (float)g_priW - 5 &&
                      dc.x <= g_uiX + (float)g_priW &&
                      dc.y >= g_uiY && dc.y <= g_uiY + (float)g_priH;
                if (!hot && g_appDockA > 0.25f && !g_appDockChips.empty())
                    hot = dc.x >= g_appDockChips.front().rect.left - 52.0f &&
                          dc.x <= g_uiX + (float)g_priW &&
                          dc.y >= g_appDockChips.front().rect.top - 28.0f &&
                          dc.y <= g_appDockChips.back().rect.bottom + 28.0f;
            }
            if (fabsf((hot ? 1.0f : 0.0f) - g_appDockA) > 0.003f) dirty = true;
            g_appDockA += ((hot ? 1.0f : 0.0f) - g_appDockA) * pk;
        }
        // M17: toast animasyonu sürerken çiz
        if (!g_toast.empty() && GetTickCount64() - g_toastTick < 1600) dirty = true;
        if (UpdateTiles()) dirty = true; // M19: yeni kare/başlık geldi
        if (dirty)
            Render();
        else
            // M19: boşta GPU yakma - girdi/zamanlayıcıda anında uyan. Çalışma
            // modunda 33ms (tile'lar swap için taze kalır), tuvalde 10ms.
            // LL hook bu thread'de - MWMO_INPUTAVAILABLE girdiyi kaçırmaz.
            MsgWaitForMultipleObjectsEx(0, nullptr,
                g_activeTile >= 0 ? 33 : 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
done:
    // M2 temizlik: hook/hotkey kaldır, TÜM pencereleri orijinal yerine koy
    if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
    UnregisterHotKey(g_hwnd, HOTKEY_TOGGLE);
    for (auto& t : g_tiles)
    {
        try { if (t.session) t.session.Close(); } catch (...) {}
        try { if (t.pool) t.pool.Close(); } catch (...) {}
        RestoreOriginal(t);
    }
    ShowTaskbars(true); // M11: çıkışta görev çubuğu garantili görünür
    SaveLayout(); // son yerleşimi diske yaz (sonraki oturum hatırlar)
    SaveSettings(); // M50: son kamera görünümünü de yaz (restore için)
    DeleteFileW(PendingFilePath().c_str()); // temiz çıkış - sigorta dosyası silinir
    return (int)msg.wParam;
}
