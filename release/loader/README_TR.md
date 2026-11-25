# Dragonkin Mod Loader 1.0.0

**Dragonkin: The Banished** için mod yükleyici ve oyun içi mod menüsü.
Oyunda **F7** ile panel açılır.

* Tek dosya (`dwmapi.dll`). Başka araç gerekmez, oyun dosyalarına dokunulmaz.
* Modlar `Binaries\Win64\Mods\` içindeki klasörlerdir. **Modlar** sekmesinden açılıp
  kapatılır; ayarları da orada görünür.
* Çoğu mod tek bir `mod.json` dosyasıdır (programlama gerekmez): slider, aç/kapa, buton,
  kısayol tuşu, oyuncu statları ve oyun verisi değişiklikleri. İleri seviye modlar kendi
  panel sekmeleri olan native bir eklenti ekleyebilir.
* Modlar birbirinin üzerine yazmaz: her değişiklik ortak bir kayıttan geçer, mod
  kapatılınca oyunun kendi değeri geri gelir.
* Çöken bir mod oyunu çökertmez, kapatılır.
* Panel oyunun odağını almaz ve pencere ya da tam ekran fark etmeden oyunun ekranında kalır.
* English, Türkçe, Français, Deutsch, العربية, 繁體中文, 简体中文 (Ayarlar sekmesi).
* Geliştirici sekmesi: mod yapanlar için çalışan oyunun canlı inceleyicisi.

## Kurulum
Arşivi `Dragonkin - The Banished` oyun klasörüne açın
(`DragonkinTheBanished\Binaries\Win64\dwmapi.dll` oluşmalı) ya da `dwmapi.dll` dosyasını
ve `Mods` klasörünü elle `DragonkinTheBanished\Binaries\Win64\` içine kopyalayın.

## Mod kurmak
Modun klasörünü `DragonkinTheBanished\Binaries\Win64\Mods\` içine koyun, oyunu başlatın
(ya da **Modları yeniden yükle**ye basın) ve **Modlar** sekmesinden açın. Program dosyası
(eklenti DLL) içeren modlar için ayrıca **Native koda izin ver** işaretlenmelidir; bunu
sadece güvendiğiniz modlar için yapın.

## Kaldırma
`DragonkinTheBanished\Binaries\Win64\` içindeki `dwmapi.dll` dosyasını silin (isterseniz
`Mods` ve `DragonkinModLoader` klasörlerini, `DragonkinMod.ini` ve `DragonkinMod.log`
dosyalarını da).

## Mod yapmak
`docs\MODDING.md` (İngilizce) mod.json biçimini ve eklenti API'sini (`sdk\dml_plugin.h`)
anlatır. `Mods` içinde üç örnek var (kapalı): temeller, oyun verisi, native eklenti.

## Notlar
* Modları offline / tek başına kullanın. Online şehir ve co-op sunucu tarafından denetlenir.
* Sorun olursa: `Binaries\Win64\DragonkinMod.log` her adımı kaydeder.
* Dear ImGui (MIT) içerir, bkz. `THIRD_PARTY_NOTICES.txt`.
