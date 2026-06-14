# Spatial Canvas

*[English](README.md) · **Türkçe***

![Spatial Canvas demo](assets/demo.gif)

Canlı Windows pencerelerini sonsuz, zoom'lanabilir bir tuvale yerleştiren
pencere yöneticisi. 3 monitör yerine (ya da 3 monitörle birlikte) tek bir
"kuşbakışı çalışma alanı": uzaklaş, her şeyi gör; yakınlaş, gerçek pencereye
ışınlan, çalış; başparmak tuşuyla geri çekil.

**Sürüm:** 0.54.0 (M0–M63) · **Platform:** Windows 11
(Windows.Graphics.Capture) · **Durum:** Aktif prototip / dogfood

## Kurulum

Gereksinim: **Windows 10 1903+ / Windows 11** (ekran yakalama API'si için).
Kurulum gerektirmez, **tek dosya**: `SpatialCanvas.exe` (statik bağlı; .NET ya
da VC redist GEREKMEZ).

1. Son sürümün `.zip`'ini indir (Releases) → klasöre çıkar.
2. `SpatialCanvas.exe`'yi çalıştır. Tuval açılır, açık pencerelerin yakalanır.
3. **ESC** ile çık (her şey eski yerine döner). İlk açılışta 3 satırlık ipucu,
   tüm kısayollar için **F1**.

> İmzasız sürümde Windows SmartScreen "bilinmeyen yayıncı" diyebilir →
> *Ek bilgi → Yine de çalıştır*. (İmzalı sürüm yol haritasında.)

## Nasıl çalışır (mimari)

Girdi simülasyonu YOKTUR. Üç katman:

1. **Yakalama** — Her pencere için Windows.Graphics.Capture oturumu
   (FreeThreaded FramePool, poll tabanlı, kilit yok). Kareler kalıcı
   D3D11 dokularına kopyalanır.
2. **Tuval** — Borderless fullscreen D3D11 swapchain; her pencere dünya
   uzayında 1:1 piksel quad. D2D/DWrite overlay: başlık etiketleri,
   hover vurgusu, dünyaya çakılı nokta ızgara, dock, paneller.
3. **Park & Swap** — Pencereler ana monitörün alt kenarında 2px görünür
   şeritte "park" eder (occlusion throttling tetiklenmez → dokular canlı
   kalır). Zoom eşiği geçilince gerçek HWND quad'ın üzerine ışınlanır ve
   odak alır; geri çekilince tekrar parka döner.

Tuval modunda pencere TOPMOST'tur ve görev çubuğu gizlenir; bir pencereye
dalınca normal z-düzenine iner ve görev çubuğu geri gelir.

## Kontroller

| Eylem | Girdi |
|---|---|
| Zoom (imleç odaklı) | Tekerlek |
| Pan (momentum'lu) | Orta/sağ tuş sürükle · kenara sürükleyince otomatik pan |
| Tile taşı (kenara yapışır) | Sol tuş sürükle · **Alt**+sürükle = yapışma kapalı |
| Yapışık kümeyi taşı | **Shift**+sürükle |
| Çoklu seçim | Boş alanda sürükle (marquee) · **Delete** = seçilileri çıkar |
| Pencereye dal | Zoom eşiğini geç · çift tık · **Enter** (klavye odağı) |
| Geri çekil (aynı manzara) | **Fare İLERİ tuşu** · Ctrl+Alt+Tekerlek-aşağı · Ctrl+Alt+Z |
| Klavye gezinme | **Ok tuşları** (yön) · **Tab/Shift+Tab** (son kullanılan) |
| Pencere ara | **Ctrl+F** · üst-orta "Ara" butonu |
| Uygulama başlat | **Ctrl+N** (palet: yaz+Enter / 1-9 / tıkla) |
| Pencere çoğalt | **Ctrl+C** / **Ctrl+V** (imleç konumuna) |
| Tuvale sabitle (HUD) | **Ctrl+P** (pan/zoom'u yok sayar; tekrar = çöz) |
| Yer imi | **Ctrl+Shift+1–4** kaydet · **Ctrl+1–4** git |
| Tümünü / seçimi sığdır | **F** · Shift+1 / Shift+2 |
| Pencereleri ızgaraya diz | **Ctrl+G** (Miro "clean up" tarzı) |
| Klavye: tile odağı / taşı | **Ok** (odak) · **Shift+Ok** (taşı) |
| Minimap (kuşbakışı) | Sağ-alt köşe · tıkla = oraya zıpla (panelden kapatılır) |
| Yapışkan not (anotasyon) | **Ctrl+Shift+N** (ya da boş tuvale **çift tık**) = not · yaz, **Tab**=renk, **Enter/Esc**=bitir |
| Notu taşı / düzenle / boyutla / sil | Sol sürükle · çift tık = düzenle · sağ-alt köşe = boyutlandır · hover **✕** = sil |
| Bölge çerçevesi (pencere grupla) | **Ctrl+Shift+Z** = etiketli bölge · **başlık çubuğundan** sürükle = bölgeyi **ve içindeki pencereleri** taşı (gövde tıklama-geçirgen) · köşe = boyutlandır |
| Bağlayıcı ok | Bir pencereden diğerine **Ctrl+sürükle** · orta-nokta **✕** ile sil (oturum-içi) |
| Tuvali PNG'ye aktar | **Ctrl+Shift+S** (Resimler klasörüne kaydedilir) |
| Tüm kısayollar | **F1** |
| Pencereyi **kapat** (uygulamayı) | Hover → sağ üstte **✕** (kendi kaydet-diyaloğu çıkar) |
| Ayarlar paneli | Sol üst **⚙** veya S |
| Çalışan pencere dock'u | İmleci ana monitör **alt** kenarına götür → tıkla = o pencereye uç |
| Uygulama dock'u (başlatıcı) | İmleci **sağ** kenara götür → sol tık = başlat · **+** = ekle · **sağ tık** = kaldır |
| Çıkış (her şey eski yerine döner) | ESC (önce açık paneli/seçimi kapatır) |

Tüm kısayollar Ayarlar panelinin **Kısayollar** sekmesinden (klavye VEYA
fare tuşuna) yeniden atanabilir. İlk açılışta 3 satırlık ipucu kartı gösterilir.

## Uygulama başlatıcı

Tuvalden çıkmadan uygulama açar. İki yol:

- **Sağ kenar dock'u** (önerilen): İmleci ekranın sağ kenarına götür → kayıtlı
  uygulamalar gerçek ikonlarıyla dikey bir dock olarak açılır; ikona tıkla =
  başlat. En alttaki **+** butonu bir dosya seçici açar, seçtiğin `.exe`
  anında dock'a eklenir (ve `launcher.txt`'ye kaydedilir — elle düzenleme gerekmez).
- **Ctrl+N paleti**: Serbest komut yaz (`cmd`, `notepad`, `chrome --new-window`,
  tırnaklı tam yol) ya da kayıtlı kısayoldan 1-9 / tıkla ile seç.

Başlatılan pencere imleç konumunda tuvale düşer. Kısayollar
`%APPDATA%\SpatialCanvas\launcher.txt`'de `Etiket|program|argümanlar`
biçiminde tutulur (ilk açılışta örnek şablon oluşur).

## Ayarlar paneli

Dil (English/Türkçe), güncelleme kontrolü, yakalama FPS (15/30/60), animasyon
hızı, başlık etiketleri, vurgu çerçevesi, dalış eşiği, maksimum pencere sayısı,
arka plan tonu, nokta ızgara, Windows ile başlat, tuval alanı (ana ekran / tüm
monitörler). Kalıcılık: `%APPDATA%\SpatialCanvas\settings.txt`.

> **Güncelleme kontrolü** (varsayılan açık) açılışta GitHub'dan küçük bir sürüm
> dosyası çeker ve yeni sürüm varsa bildirim gösterir — sadece bildirim, hiçbir
> şey indirilmez/kurulmaz. Ağ çağrısı istemiyorsan Ayarlar'dan kapat.

## Dayanıklılık

- **Tek örnek:** Mutex; ikinci başlatma var olan tuvali öne getirir.
- **Çökme sigortası:** Park durumu `pending_restore.txt`'ye yazılır; üstüne
  bir çökme filtresi (`SetUnhandledExceptionFilter`) anında görev çubuğunu
  geri gösterir ve pencereleri parktan çıkarır. Bir sonraki açılış da mahsur
  pencereleri (HWND/exe/başlık eşleşmesiyle) eski yerlerine koyar.
- **Cihaz kaybı:** TDR / sürücü güncellemesi / uyku-uyanma → D3D/D2D/WGC tam
  yeniden kurulur; başarısızsa pencereler restore edilip temiz çıkılır
  (hiçbir pencere 2px şeritte yetim kalmaz).
- **Çözünürlük/DPI değişimi:** Geometri, swapchain ve park şeridi canlı
  yeniden kurulur.
- **Oturum sonu:** `WM_QUERYENDSESSION`/`WM_ENDSESSION` → pencereler eski
  yerine döner, görev çubuğu geri gelir.
- **explorer.exe yeniden başlarsa:** `TaskbarCreated` dinlenir, yeni görev
  çubuğu tekrar gizlenir.
- **Yaşam döngüsü:** Kapanan pencerelerin tile'ları temizlenir; yakalaması
  ölen ama yaşayan pencere parkta bırakılmaz, yerine döndürülür. Yeni açılan
  pencereler tuval modundayken otomatik tile olur.
- **Layout kalıcılığı:** Tile yerleşimi exe bazlı `layout.txt`'de saklanır.

## Performans

Boşta GPU yakmaz: ana döngü dirty-flag + `MsgWaitForMultipleObjectsEx`
ile yalnızca bir şey değiştiğinde çizer. Nokta ızgara tek wrap-mode bitmap
fırçayla çizilir; overlay fırçaları cache'lidir.

## Pencere kuralları

`%APPDATA%\SpatialCanvas\rules.txt` (elle düzenlenir): `exclude=uygulama.exe`
satırı o exe'yi hiç yakalamaz (ilk açılışta açıklamalı şablon oluşur).

## Derleme

Gereksinimler: VS 2022 (MSVC v143), Windows SDK 10.0.26100.

```
MSBuild Win32CaptureSample.sln -p:Configuration=Release -p:Platform=x64 -m
```

Çıktı: `x64\Release\SpatialCanvas.exe`. Tüm uygulama kodu tek modülde:
`Win32CaptureSample/Canvas.cpp` (~2900 satır, giriş `RunCanvasApp()`).

## Lisans

Kaynak-görünür, **özel (proprietary)** lisans — açık kaynak değildir.
Herkes ücretsiz **indirip kullanabilir** (kişisel / eğitim / kurum-içi).
Yazılı izin olmadan **çoğaltmak, kopyalamak, yeniden dağıtmak ve ticari
olarak sunmak yasaktır.** Ayrıntılar: [LICENSE](LICENSE).

## Teşekkür

Yakalama altyapısı [robmikh/Win32CaptureSample](https://github.com/robmikh/Win32CaptureSample)
(MIT) yapısından yola çıkar; derleme `robmikh.common` (MIT, NuGet) paketini
kullanır. Bu üçüncü taraf bileşenler kendi lisanslarındadır.

---
© 2026 Batuhan Demirbilek · 13auth
