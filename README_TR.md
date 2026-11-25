# Dragonkin Mod Loader + Dragonkin Plus

**Dragonkin: The Banished** için iki modun kaynak kodu (Unreal Engine 5.5, Windows).
English: [README.md](README.md).

| | İndirme | Nedir |
|---|---|---|
| **Dragonkin Mod Loader** | [Nexus Mods](https://www.nexusmods.com/dragonkinthebanished/mods/11) | `dwmapi.dll`: oyun içi mod menüsü (F7), `mod.json` modları ve native eklentiler için mod yükleyici |
| **Dragonkin Plus** | [Nexus Mods](https://www.nexusmods.com/dragonkinthebanished/mods/12) | loader eklentisi: çarpanlar, hileler, kolaylıklar (5 panel sekmesi) |

Oyuncular: Nexus'tan indirin; kurulum ve kullanım orada ve
[release/loader/README_TR.md](release/loader/README_TR.md) /
[release/plus/README_TR.md](release/plus/README_TR.md) içinde anlatılıyor.
Mod yapanlar: [docs/MODDING.md](docs/MODDING.md) (İngilizce) ve [sdk/dml_plugin.h](sdk/dml_plugin.h).

## Derleme
C++ iş yükü kurulu Visual Studio 2022+ ile `build.bat` çalıştırın:
`build\dwmapi.dll` (Mod Loader), `build\plus\DragonkinPlus.dll` (Dragonkin Plus),
`build\plugin\ExamplePlugin.dll` (SDK örneği). `python tools/package.py` iki release
zip'ini `dist\` içinde oluşturur. Arayüz metinleri: `tools/translations.py` düzenlenip
`python tools/gen_lang.py` çalıştırılır.

Nasıl çalıştığı ve dosyaların görevleri İngilizce README'de.

## Notlar
* Offline / tek başına oyun için. Online şehir ve co-op sunucu tarafından denetlenir.
* Üçüncü taraf: [Dear ImGui](https://github.com/ocornut/imgui) v1.92.9b (MIT), `vendor/imgui`
  içinde. Bkz. `THIRD_PARTY_NOTICES.txt`.
* Eko Software veya Dragonkin: The Banished yayıncısıyla bağlantısı yoktur.
