# Changelog

Spatial Canvas — kayda değer kullanıcı-yönelimli değişiklikler. Sürümleme M-kilometre
taşı tabanlı (M = milestone). Tarihler 2026.

## 0.46.0 — Tuval notları + yayın hazırlığı
- **Yapışkan notlar (M44–46):** `Ctrl+Shift+N` ile tuvale not bırak; renk (Tab),
  sürükle, çift-tık düzenle, sağ-alt köşeden **yeniden boyutlandır**, hover-`✕` sil.
  Notlar `notes.txt`'de kalıcı; fit/minimap/F1 ile entegre. IPC: `note:metin`.
- Exe sürüm damgası + ikon + markalama; tek-dosya dağıtım (redist gerektirmez).
- robmikh örnek kodu temizlendi; `package.ps1` ile sürümlü `.zip` üretimi.

## 0.42–0.45 — Workspace + keşfedilebilirlik
- **Adlandırılmış workspace'ler (M42):** düzen profillerini kaydet/yükle
  (`save <ad>` / `load <ad>`, IPC `save:`/`load:`).
- Tile etiketinde exe ikonu (M41); F1 listesi + boş-tuval onboarding ipucu.

## 0.40.0 — Otonom geliştirme dalgası (M33–M40)
- Keskin dock ikonları (App Paths + JUMBO), pencere blur kuralı (`rules.txt`).
- `Ctrl+G` ızgaraya diz (Miro clean-up tarzı).
- **Minimap** (sağ-alt kuşbakışı; viewport + yer imi noktaları; tıkla-zıpla).
- Hover etiketi (zoom'dan bağımsız), `Shift+Ok` ile odaklı tile taşıma.
- Komut paleti genişledi (`Ctrl+N`'e fit/grid/quit).

## 0.30–0.34 — Rakip-parite (M27–M34)
- Pinned tile kalıcılığı, pencere **opacity** kuralı (alpha blend), **vinyet** zemin.
- **IPC / scripting** (named pipe: fit/quit/pull/launch/search/bookmark).
- Yatay touchpad pan; box-blur pencere kuralı.

## 0.16–0.26 — Sağlamlık + UX (M16–M26)
- Tek-örnek, çökme sigortası, device-lost tam reinit, çözünürlük/DPI canlı.
- Toast HUD, ESC bağlam merdiveni, ilk-açılış kartı + **F1** kısayol listesi.
- Klavye navigasyonu (ok/Enter/Tab-MRU), ekrana-sabit HUD tile (`Ctrl+P`).
- Uygulama başlatıcı (`Ctrl+N` palet + sağ-kenar dock, gerçek ikonlu CRUD).
- Performans: ızgara bitmap brush, fırça cache, boşta GPU kısma.

## 0.11–0.15 — Tuval kabuğu (M11–M15)
- Görev çubuğu gizleme, üst-orta Ara butonu, `Ctrl+C/V` çoğaltma.
- Marquee çoklu seçim + Delete; momentum pan, kenar autopan, nokta ızgara.
- Yer imleri (`Ctrl+Shift+1–4` / `Ctrl+1–4`), alt-kenar dock, kenar yapışması,
  pencere kuralları (`rules.txt`).

## 0.1–0.10 — Çekirdek (M0–M10)
- WGC yakalama → D3D11 doku; sonsuz zoom/pan tuval; **park & swap** (gerçek
  HWND'ye dalış, input simülasyonu yok); özelleştirilebilir kısayollar.
