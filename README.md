# Rozhodovač (Waveshare ESP32-S3-Touch-AMOLED-2.16)

Fyzický stolní terminál pro rozhodování, přehled o stavu a interaktivní parťák s **2,16" dotykovým AMOLED displejem (480 × 480)** pro **Claude Code / Claude Desktop** i **Antigravity CLI (AGY)**.

> 🛒 **Kde koupit krabičku:** [Koupit v RPishop](https://rpishop.cz/684624/waveshare-vyvojova-deska-esp32-s3-kapacitnim-amoled-2-16-displejem)
> 🌐 **Prezentační web a interaktivní simulátor:** [rozhodovac.zkusmo.cz](https://rozhodovac.zkusmo.cz). Simulátor ukazuje chování displeje, maskoty Clawd a Agy, otázky s volbami a zvuky. Web je ve složce `docs/`, otevřít ho jde i přímo ze souboru [`docs/index.html`](docs/index.html).

---

## Filozofie Rozhodovače

Moderní vývoj s AI agenty (Claude Code, AGY) většinou běží v **autonomním režimu**, kdy se rutinní úkoly (čtení souborů, běžné úpravy kódu a bezpečné příkazy) provádějí automaticky. Vyžadovat stisk tlačítka u každého souboru nedává smysl a zdržuje.

**Rozhodovač proto funguje ve dvou rovinách:**
1. **Živý stavový monitor na stole:** Průběžně ukazuje, co agent dělá, posouvá historii chatu, počítá tokeny a v klidu slouží jako stolní hodiny.
2. **Cílené rozhodování člověka:** Vyruší tě akusticky a probudí se k zásahu **pouze ve chvílích, kdy je to skutečně nutné**:
   - **Otázky agenta (`ask_question`) s volbami (2 až 8 možností):** Zobrazí otázku a 2 až 8 dotykových tlačítek (2 vedle sebe, od 3 voleb řádky pod sebou; od 5 voleb se maskot schová a řádky vyplní celý displej). U 3 a více voleb první klepnutí volbu jen vybere a druhé klepnutí na ni odpověď odešle, aby se u úzkých řádků netrefila omylem sousední. Hardwarově BOOT cykluje výběr a IO18 potvrzuje. Zvolenou možnost dostane agent jako odpověď bez sahání na klávesnici.
   - **Destruktivní operace:** Zachytí rizikové příkazy (`git push --force`, `reset --hard`, `rm -rf`, `Remove-Item -Recurse`, `drop table`, `delete_file` a další).

---

## Co umí

- **Dotykové rozhodování na 480 × 480 AMOLED displeji (`CO5300` + `CST9220`):**
  - Při otázkách z AGY zobrazí znění dotazu a tlačítka voleb (2 až 8 možností).
  - Při rizikových akcích velká tlačítka **`[ SCHVÁLIT ]`** a **`[ ZAMÍTNOUT ]`** a zobrazení příkazu. Kritickou akci schvaluje **podržení** tlačítka SCHVÁLIT (nebo BOOT) po dobu 1 s, klepnutí nestačí. Zamítnutí stačí klepnutím. Z AGY a z Claude Code v terminálu ji označí hook, u Claude Desktopu ji krabička pozná podle celého příkazu (`rm -rf`, `push --force`, `reset --hard`, `drop table`, `Remove-Item` a podobně). Když je riziková část dlouhého příkazu daleko od začátku, karta ukáže výřez od ní, aby nezůstala za okrajem. Karta vždy ukazuje skutečný příkaz nebo celou cestu k mazanému souboru, popis od agenta jen jako doplněk za svislou čarou.
  - **Ochranná doba:** prvních 0,7 s po objevení dotazu jsou tlačítka zašedlá a dotyk ani stisk nic neodešle. Klepnutí mířené na předchozí obrazovku tak nemůže omylem schválit.
  - **U AGY a Claude Code v terminálu karta ukazuje, kolik času zbývá** do vrácení dotazu do terminálu (u Claude Desktopu jak dlouho dotaz čeká). Když most přijetí odpovědi do 5 s nepotvrdí (spojení spadlo cestou), ozvou se dva hluboké tóny a dotaz se znovu ukáže na hlavní obrazovce (i s ochrannou dobou), aby šlo odpovědět znovu. Potvrzení, které dorazí později, odpověď ještě přijme. U Claude Desktopu, který příjem nepotvrzuje, se tlačítka vrátí, když dotaz ani po 20 s nezmizí. Statistika se u dotazů z mostu započítá až po potvrzení, opakovaný pokus ji nezdvojí. Po odeslání ukazuje karta „Odesláno…“ a tlačítka ani dotyk do vyřízení nic nepřepnou. Během zobrazení PINu pro párování tlačítka ani dotyk na dotaz nepůsobí.
  - Fyzická tlačítka krabičky: **BOOT** = u 2 voleb 1. možnost, u 3 a více voleb cyklování výběru / Schválit (u kritické akce podržet 1 s) / další obrazovka (podržením = Menu, během dotazu se podržení ignoruje), **IO18** = u 2 voleb 2. možnost, u 3 a více voleb potvrzení vybrané možnosti / Zamítnout / posun stránek a zpráv, **PWR** = vypnutí a zapnutí displeje (podržením 6 s = vypnutí napájení).
- **Celoobrazovkový chat a historie zpráv na klepnutí:**
  - Klepnutím na okno chatu na hlavní obrazovce (`Akce`) se panel zpráv roztáhne přes celý displej (**13 řádků** najednou, historie **16 zpráv**, bez maskota). Nejnovější zpráva je nahoře.
  - Tažením prstu nahoru a dolů (nebo tlačítkem `IO18`) lze posouvat historii. Klepnutím do chatu nebo na `Akce` se zobrazení vrátí zpět k maskotovi.
- **Plná podpora češtiny s diakritikou (UTF-8):**
  - Vlastní pixelový UTF-8 renderer (`hwDrawUtf8`) vykresluje všechna česká písmena (`á č ď é ě í ň ó ř š ť ú ů ý ž` / `Á Č Ď É Ě Í Ň Ó Ř Š Ť Ú Ů Ý Ž`).
  - Kompletně české prostředí: spodní lišta (**`Akce` | `Profil` | `Info` | `Menu`**), srozumitelné názvy akcí i dny v týdnu u stolních hodin.
- **Kompatibilní s Claude Desktop / Claude Code přes Bluetooth LE** (`Developer → Open Hardware Buddy…`, Nordic UART Service `6e400001-b5a3-f393-e0a9-e50e24dcca9e`, 6místný párovací PIN na displeji).
- **Přímá podpora Antigravity CLI (AGY)** přes lokální most (`bridge/`). Funguje **přes USB-C kabel (`USB`) i bezdrátově přes Bluetooth LE na baterii (`BLE`)**.
- **Claude Code v terminálu** přes stejný most: krabička ukazuje průběh práce a místo terminálu se ptá na žádosti o povolení a na otázky, i na několik otázek po sobě a s vícenásobným výběrem (oddíl 4).
- **Maskot podle připojeného klienta:** pro Claude pixelový **Clawd** z Claude Code (`#D97757` cihlově oranžová), pro AGY modrofialový **Agy** se zvonovitým tělem, velkýma očima a botkami. Oba mají animace spánku, klidu, přemýšlení, čekání na rozhodnutí, oslavy, zatřesení i pohlazení. Štítek vlevo nahoře ukazuje klienta i kanál, třeba `AGY·USB` nebo `CLAUDE·BLE`.
- **Měření a senzory:** měření baterie přes **AXP2101**, detekce zatřesení a otočení displejem dolů přes **QMI8658 IMU**, velké stolní hodiny **`HH:MM:SS`** s kalendářem přes **PCF85063 RTC** a zvukové pípnutí přes vestavěný reproduktor (**ES8311**).
- **Stolní hodiny:** jen čas a datum, bez maskota. Zobrazí se samy na obrazovce Akce při napájení z USB, když agent nic nedělá a 20 s se nikdo krabičky nedotkl. Ručně je kdykoli zapne Menu → Hodiny, i na baterii: takto zapnuté hodiny na baterii nezhasnou, po 45 s se jen ztlumí na nejnižší jas. První dotyk nebo stisk tlačítka hodiny jen ukončí, práce agenta nebo dotaz také. Čas se bere z hodin RTC zálohovaných baterií a platí hned po zapnutí, pokud je rozumný (rok 2025 a novější). Jinak se čeká na synchronizaci z počítače.
- **Úvodní obrazovka:** po zapnutí ukáže na 3 s název, podporované klienty a Bluetooth jméno krabičky. Dotyk nebo tlačítko ji zkrátí.
- **Profil a nálada:** profil počítá schválené akce (`OK`), zamítnuté akce (`NE`) a zvlášť odpovědi na otázky (`Odp`), protože volba druhé možnosti není zamítnutí. Počítadla přežijí restart, vynuluje je jen tovární reset. Nálada (srdíčka) vychází z mediánu doby posledních osmi schválení a odpovědí: do 15 s čtyři srdíčka, do 30 s tři, do 60 s dvě, do 120 s jedno. Sníží ji převaha zamítnutí mezi posledními osmi schváleními a zamítnutími (počítá se od tří rozhodnutí; odpovědi na otázky se sem nepočítají). Rozhodnutí do 8 s vyvolá srdíčka. Demo režim statistiky nemění.
- **Level:** roste s tokeny, první level stojí 50K a každý další o polovinu víc, takže ani milionová spotřeba Claude nehraje melodii nového levelu pořád dokola.
- **Demo režim** (Menu → Demo režim) jen ukazuje obrazovky. Data od klienta se čtou dál a skutečný dotaz demo sám ukončí, aby nepropadl.
- **Na baterii** displej po 45 s bez dotyku zhasne. Nové zprávy v chatu ho nerozsvítí, rozsvítí ho dotaz, dokončení práce agenta nebo varování baterie. Dotaz, který čeká déle než 2 min (Claude Desktop nemá časový limit), po minutě bez dotyku displej ztlumí, dotyk nebo tlačítko jas vrátí.
- **Tokeny:** z Claude Desktopu přichází skutečná spotřeba, u Claude Code v terminálu ji most čte ze záznamu relace (výstupní tokeny). AGY ji hookům nedává, proto most počítá hrubý odhad 800 tokenů na každé volání nástroje. Denní počítadlo se o půlnoci nuluje. Každý klient má své počítadlo, po přepnutí klienta nebo výpadku spojení krabička začne počítat od nového stavu a rozdíl mezi počítadly nepřičte.

---

## Zvuky

Krabička dává zvukem vědět hlavně o tom, co vyžaduje pozornost. Tóny přehrává úloha na pozadí, takže melodie nezdržují displej ani dotyk.

| Událost | Zvuk |
|---|---|
| Otázka s volbami (2–8 možností) | dva přátelské stoupající tóny, další otázka téhož dotazování jen cvakne |
| Riziková akce | trojí naléhavé vysoké pípnutí |
| Žádost o schválení z Claude | dva stoupající tóny |
| Nevyřízený dotaz | připomínka stejným zvukem: u AGY a Claude Code v terminálu v polovině a ve 4/5 doby na odpověď, u Claude Desktopu po 30, 60 a 90 s, pak ticho |
| Odpověď nedoručena do mostu | dva hluboké tóny, tlačítka se vrátí |
| Dotaz vypršel nebo se vyřídil v terminálu | klesající tři tóny |
| Schválení, zamítnutí, odpověď | stoupající dvojtón, klesající dvojtón, neutrální dvojtón |
| Agent dokončil práci | krátká melodie C–E–G |
| Připojení a odpojení klienta | stoupající a klesající dvojtón |
| Párování Bluetooth | PIN dvojí pípnutí, úspěch stoupající melodie, neúspěch dva hluboké tóny |
| Baterie | varování při 20 %, naléhavé při 10 %, každé jen jednou až do nabití nad 25 % |
| Nabíječka připojena a odpojena | krátké stoupající a klesající cvrknutí |
| Spánek displejem dolů a probuzení | klesající a stoupající dvojtón |
| Nový level, zatřesení, pohlazení, úvodní obrazovka | krátké melodie |
| Ovládání (tlačítka, dotyk, menu) | tiché cvaknutí |

**Nastavení** (Menu → Nastavení):
- **Zvuk:** VYP, TICHÝ, STŘEDNÍ (výchozí) nebo HLASITÝ. Po přepnutí zazní ukázka.
- **Zvuk ovládání:** cvakání při ovládání lze vypnout, důležité zvuky zůstanou.
- **Noční klid 22–7 h** (výchozí zapnuto, podle hodin v krabičce): informativní zvuky se vynechají, dotazy, rozhodnutí a varování zazní potichu.
- **Jas displeje:** výchozí 3/4, krabička si ho pamatuje i po vypnutí.
- **Spodní panel (HUD):** okno chatu na hlavní obrazovce lze vypnout, zůstane jen maskot a spodní lišta. Dotaz se zobrazí vždy.
- **Tovární reset…:** smaže statistiky, nastavení, jméno i BLE párování. Potvrzuje se druhým klepnutím do 3,5 s.

**Spotřeba:** zesilovač je zapnutý jen během melodie. Mezi melodiemi se zastaví i hodiny I2S, takže kodek ES8311 stojí a procesor nebudí přenos dat. Úloha zvuku spí, dokud nepřijde tón. Kdyby po této úspoře krabička nehrála, přelož firmware s `-DAUDIO_IDLE_STOP_I2S=0` v `build_flags`. Displej se v klidu překresluje asi 8× za sekundu (plnou rychlostí jen při dotyku, stisku a chvíli po nich). Se zhasnutým displejem procesor běží na 80 MHz místo 240 MHz a před zhasnutím se displej vyčerní, aby po probuzení neukázal starý obsah. Bluetooth vysílá reklamu po 100–200 ms.

---

## 0. Požadavky

- Krabička **[Waveshare ESP32-S3-Touch-AMOLED-2.16](https://rpishop.cz/684624/waveshare-vyvojova-deska-esp32-s3-kapacitnim-amoled-2-16-displejem)** (koupit v RPishop; 2,16″ dotykový AMOLED displej 480 × 480, pouzdro s tlačítky, reproduktor, RTC).
- Windows s PowerShellem (most funguje i na Linuxu a macOS, `flash.ps1` je jen pro Windows).
- Python 3.10 nebo novější a balíčky pro build a most:

```powershell
python -m pip install platformio pyserial bleak
```

Balíček `bleak` potřebuje jen Bluetooth spojení mostu. Bez něj most pracuje s krabičkou jen přes USB kabel.

Firmware potřebuje Arduino core 3.x. `platformio.ini` proto fixuje platformu `espressif32 @ ~8.0.0` (ověřeno s verzí 8.0.0 a Arduino core 3.3.12). První build stáhne toolchain ESP32 (stovky MB) a trvá několik minut.

---

## 1. Nahrání firmwaru do krabičky

1. Připoj krabičku Waveshare k PC **datovým USB-C kabelem**.
2. V terminálu ve složce projektu spusť:

```powershell
.\flash.ps1
```

Skript zkompiluje firmware, zastaví daemon mostu (jen jeho proces, jiné Python skripty nechá běžet), nahraje firmware a daemon znovu spustí na pozadí. Když kompilace nebo nahrání selže, skript skončí chybou a daemon nespouští.

Pro kontrolu výpisů ze sériové linky:
```powershell
.\flash.ps1 -Monitor
```

Další parametry: `-Port COM8` vynutí konkrétní port, `-EraseFirst` před nahráním smaže celou flash (včetně statistik a párování).

---

## 2. Propojení s Antigravity CLI (AGY) — USB i Bluetooth

Bridge daemon (`bridge/agy_buddy_daemon.py`) automaticky volí nejlepší dostupný kanál:
1. **USB kabel (priorita č. 1, zelený štítek `AGY·USB`):** Pokud je krabička připojená k PC datovým USB-C kabelem (`VID 0x303A`), komunikuje po sériové lince bez párování. Za připojenou se považuje, až když odpoví na dotaz na stav, takže jiná deska ESP32-S3 na USB ji nezamění. Port, který drží jiný program (sériový monitor, PlatformIO) nebo na kterém krabička neodpověděla, daemon na 30 s přeskočí a jede přes BLE. Port zadaný přes `--port` zkouší jen tehdy, když v systému existuje. Po zapojení kabelu během spojení přes BLE se do 5 s přepne na USB.
2. **Bluetooth Low Energy (záloha, modrý štítek `AGY·BLE`):** Jakmile USB kabel odpojíš, daemon vyhledá zařízení `Claude-XXXX` přes BLE a pokračuje bezdrátově. Po prvním úspěšném spojení si adresu krabičky zapamatuje (`bridge/state.json`) a připojuje se jen k ní, aby cizí krabička v dosahu nemohla schvalovat. Jinou krabičku povolíš příkazem `forget-device`.
3. **Stav `OFFLINE`:** Pokud krabička nedostane data déle než 30 s, přepne indikátor na `OFFLINE`. Daemon se krabičky ptá na stav s každým heartbeatem (8 s) a zavře spojení, až když 40 s neodpoví. Zápis na USB má limit 2 s, takže ani zamrzlý firmware daemon nezablokuje. Čekající dotazy vrátí terminálu AGY, pokud se spojení do 5 s neobnoví.

Daemon přidává do každé zprávy pro krabičku pole `"src":"agy"`. Claude Desktop ho neposílá, takže krabička pozná, který klient je připojený, a podle toho přepne maskota a štítek.

Když posílají data oba kanály naráz (most po USB a Claude Desktop po BLE), krabička drží jen jeden z nich. Druhý převezme, když přinese dotaz a ten první žádný nemá, když na něm agent pracuje a na prvním ne, nebo když první 20 s nic nepošle. Krabička tak ukazuje toho klienta, který zrovna pracuje nebo se ptá. Po přepnutí kanálu začne tokeny počítat od nového stavu.

Dotaz od daemonu nese `ttl` (zbývající sekundy) a `age` (stáří v sekundách). Odpověď z krabičky obsahuje u otázek i pořadí volby `opt` (od 0) a daemon její přijetí potvrdí zprávou `{"ack":"permission","ok":…,"id":…}`. Odpověď jde jen kanálem (USB nebo BLE), ze kterého dotaz přišel, a krabička bere potvrzení jen z něj. Odpověď na příkaz (třeba `status`) jde jen kanálu, který se ptal.

Daemon poslouchá jen na `127.0.0.1:18765` a odmítá požadavky z webového prohlížeče (hlavička `Origin`) i s cizím `Host`. Webová stránka tak nemůže krabičce posílat příkazy. Kromě `/health` navíc vyžaduje token, který při startu zapíše do `~/.rozhodovac/token` (profil uživatele). Hook i `rozhodovac_ctl.py` ho posílají samy, jiný program na PC bez přístupu k profilu krabičce nic nepošle.

Dotazy z více relací AGY naráz čekají souběžně: krabička je ukáže po sobě a každý má svůj odpočet. Když hook skončí dřív (AGY nástroj zruší), dotaz z krabičky hned zmizí.

### Zapnutí integrace pro AGY
- **Pouze pro jeden projekt** (`.agents/hooks.json` ve složce projektu, ve které příkaz spustíš, nebo zadané přes `--dir`):
  ```powershell
  python bridge/rozhodovac_ctl.py enable-local --dir C:\cesta\k\projektu
  ```
- **Globálně pro všechny projekty v AGY** (`~/.gemini/config/hooks.json`):
  ```powershell
  python bridge/rozhodovac_ctl.py enable-global
  ```
- **Vypnutí:** `disable-local` (zase pro aktuální nebo zadanou složku) nebo `disable-global`. Zapnutí globálně nastavení jednotlivých projektů nemění.

Skript spusť systémovým Pythonem, ne z virtuálního prostředí: cesta k Pythonu se zapíše do `hooks.json` a po smazání venv by hook přestal fungovat (skript na to upozorní). Ostatní hooky v `hooks.json` zůstanou zachované. Soubor se zapisuje přes dočasný soubor a záloha `hooks.json.bak` vznikne jen při skutečné změně. `hooks.json` uložený s BOM (PowerShell, Poznámkový blok) se načte bez chyby. Když `hooks.json` není platný JSON, skript ho nepřepíše a skončí chybou. Starší záznam pod klíčem `schvalovac` se při zápisu nahradí klíčem `rozhodovac`.

### Jak hook rozhoduje
| Situace | Odpověď hooku pro AGY |
|---|---|
| Běžný nástroj (čtení, úpravy, bezpečné příkazy) | `allow`: krabička nezastaví, platí tvoje nastavení oprávnění v AGY |
| Schváleno na krabičce | `auto_approve`: AGY se už v terminálu znovu neptá |
| Zamítnuto na krabičce | `deny` s důvodem pro agenta |
| Otázka s volbami zodpovězená na krabičce (2–8 možností) | `deny` s textem zvolené možnosti v důvodu: AGY neumí hookem doplnit odpověď do `ask_question`, agent ji proto dostane jako zprávu |
| Otázka s více než 8 možnostmi, s více otázkami naráz nebo s vícenásobným výběrem | `allow`: ptá se terminál, krabička jen ukáže „Otázka v terminálu“ |
| Krabička vypnutá, odpojená, připojená ke Claude Desktopu nebo nikdo nerozhodl včas | `ask`: rozhodnutí se vrátí do terminálu AGY; u otázky `allow`, otázku položí terminál |

Na rizikovou akci se čeká 45 s, na otázku 90 s. Seznam destruktivních příkazů je v `bridge/agy_hook.py` (`_DANGEROUS_I`), pozná i dlouhé a sloučené přepínače (`rm --recursive`, `git push -fu`, `git push --delete`/`--mirror`/`origin :větev`, `git branch -Df`, `git clean --force`, `git -C repo checkout .`, `Remove-Item -Recurse`, `del /s`, `find -delete`, `rimraf`). U dlouhého příkazu ukáže krabička výřez s rizikovou částí, aby `… && rm -rf ~` na konci nezmizelo. Je to seznam vzorů, ne sandbox: co do něj nepatří, projde v režimu `decide` bez dotazu na krabičce.

> **Ověřit v AGY:** předání odpovědi na otázku přes důvod zamítnutí vychází z dokumentace hooků AGY. Pokud by agent důvod nedostal, zopakuje otázku v terminálu.

### Režimy Rozhodovače (`mode`)
```powershell
python bridge/rozhodovac_ctl.py mode          # Zobrazí aktuální režim a odkud pochází
python bridge/rozhodovac_ctl.py mode decide   # (Výchozí) Krabička jen u otázek a rizikových akcí
python bridge/rozhodovac_ctl.py mode write    # Navíc u všech zápisů souborů a příkazů
python bridge/rozhodovac_ctl.py mode all      # U každého volání nástroje
```

Otázky, které krabička neumí zodpovědět (více než 8 možností, více otázek naráz, vícenásobný výběr), jdou v každém režimu do terminálu.

Režim se uloží do `bridge/state.json` a platí pro všechna další volání hooku. Proměnná prostředí `ROZHODOVAC_MODE` (dříve `SCHVALOVAC_GATE`) má přednost před uloženým režimem.

### Rychlý test rozhodování a displeje
```powershell
# Test zobrazení nebezpečné akce ke schválení:
python bridge/rozhodovac_ctl.py test

# Test zobrazení otázky s volbami (podpora 2 až 8 možností; výchozí 3):
python bridge/rozhodovac_ctl.py test-question 3
```

### Další příkazy `rozhodovac_ctl.py`
```powershell
python bridge/rozhodovac_ctl.py status            # Stav spojení, režim, % baterie a statistiky
python bridge/rozhodovac_ctl.py start             # Spustí trvalý daemon na pozadí
python bridge/rozhodovac_ctl.py stop              # Zastaví daemon, hooky ho znovu spustí až po start
python bridge/rozhodovac_ctl.py name "Rozhodovac" # Přejmenuje parťáka v krabičce
python bridge/rozhodovac_ctl.py unpair            # Smaže v krabičce uložená BLE párování
python bridge/rozhodovac_ctl.py forget-device     # Daemon zapomene zapamatovanou krabičku
```

Krabička si pamatuje nejvýš 3 spárované počítače, čtvrtý vytlačí nejstarší. Po `unpair` (nebo továrním resetu) odeber krabičku i ve Windows v Nastavení → Bluetooth → Odebrat zařízení, jinak se znovu nespáruje.

Původní název `schvalovac_ctl.py` funguje dál jako alias.

### Proměnné prostředí
| Proměnná | Význam |
|---|---|
| `ROZHODOVAC_MODE` | Režim hooku, přebíjí `mode` uložený v `bridge/state.json` |
| `ROZHODOVAC_DAEMON_URL` | Adresa daemonu pro hook a ovládání (výchozí `http://127.0.0.1:18765`). Daemon spuštěný z hooku poslouchá na portu z této adresy; host musí být `127.0.0.1` nebo `localhost` |
| `ROZHODOVAC_OWNER` | Jméno vlastníka zobrazené v profilu (výchozí přihlášený uživatel) |

Starší názvy `SCHVALOVAC_GATE`, `SCHVALOVAC_DAEMON_URL` a `SCHVALOVAC_OWNER` fungují dál.

Bez balíčku `bleak` daemon hledá krabičku jen na USB. Když se daemon nepodaří spustit, hook zkusí další spuštění nejdřív za 60 s. Volání nástrojů se tak nezdržují. Log daemonu je v `bridge/daemon.log` (rotuje po 1 MB).

---

## 3. Propojení s aplikací Claude Desktop (BLE)

Platí pro Cowork a Claude Code otevřený v aplikaci Claude Desktop. Claude Code v terminálu aplikace nevidí, ten se připojuje přes most (oddíl 4).

1. Pokud na PC běží AGY daemon a drží BLE spojení, zastav ho: `python bridge/rozhodovac_ctl.py stop`. Most pak zůstane vypnutý, hooky AGY ani Claude Code ho samy nespustí, dokud nepoužiješ `python bridge/rozhodovac_ctl.py start`.
2. V aplikaci **Claude Desktop** zapni vývojářský režim: **Help → Troubleshooting → Enable Developer Mode**.
3. Otevři v horním menu **Developer → Open Hardware Buddy…**.
4. Klikni na **Connect**, vyber zařízení **`Claude-XXXX`** a opiš 6místný PIN z displeje krabičky.

---

## 4. Propojení s Claude Code v terminálu (CLI)

Claude Code spuštěný v terminálu (příkaz `claude`) aplikace Claude Desktop nevidí, krabičku proto obsluhuje stejný most jako AGY. Hooky Claude Code posílají mostu události relace a most je předává krabičce po USB kabelu, nebo po Bluetooth LE, když je nainstalovaný balíček `bleak` a krabičku zrovna nedrží Claude Desktop.

**Co krabička ukazuje:** zadání (`> …`), právě spuštěné nástroje (`Příkaz: …`, `Úprava: soubor.cs`, `Čtení: …`), začátek poslední odpovědi po dokončení práce, počet pracujících relací a výstupní tokeny. Maskot je Clawd, štítek `CLAUDE·USB` nebo `CLAUDE·BLE`.

**Na co se krabička ptá:** jen na to, na co by se jinak ptal terminál, a se stejnými volbami.
- **Žádost o povolení nástroje** (příkaz, zápis souboru a podobně): když terminál nabízí i „už se neptat“, krabička ukáže tři volby pod sebou: **Ano**, **Ano, už se neptat** (pod ní, jaké pravidlo se uloží, třeba `Příkaz npm test:* v tomto projektu`) a **Ne**. Výběr začíná na Ne. Jinak SCHVÁLIT nebo ZAMÍTNOUT. Rizikový příkaz (`rm -rf`, `git push --force`, `reset --hard`…) má vždy jen SCHVÁLIT podržením 1 s nebo ZAMÍTNOUT. Zamítnutí dostane Claude jako zprávu, že akci uživatel zamítl na Rozhodovači.
- **Otázky** (nástroj AskUserQuestion): všechny, i několik otázek najednou a s vícenásobným výběrem. Krabička je ukazuje po jedné, v záhlaví pořadí a téma (`2/3 · Funkce`), pod otázkou volby s popisem. U jedné volby klepnutí volbu vybere a druhé klepnutí odešle. U vícenásobného výběru se volby zaškrtávají a pokračuje se tlačítkem DALŠÍ (u jediné otázky ODESLAT). Tlačítko ZPĚT vrátí předchozí otázku i s vybranými odpověďmi. U více otázek je na konci shrnutí odpovědí s tlačítkem ODESLAT, jako v terminálu. Když se volby nevejdou, písmo se nezmenšuje: nahoře a dole je vidět oříznutý kousek sousední volby, vpravo posuvník a seznam se posune tahem prstu, klepnutím na oříznutou volbu nebo s výběrem tlačítkem BOOT. Claude Code dostane odpovědi stejně, jako by je uživatel zadal v terminálu.
- **Tlačítko TERMINÁL** vrátí dialog do terminálu hned, třeba když chceš odpovědět vlastním textem (volba „Other“) nebo si přečíst víc.
- **Fyzická tlačítka** na obrazovce s volbami: BOOT přesouvá výběr po volbách a tlačítkách, IO18 vybranou volbu odešle, zaškrtne nebo stiskne vybrané tlačítko.

Během čekání terminál ukazuje „Čekám na odpověď z Rozhodovače…“. Když na krabičce nikdo do **30 s** (u otázek **60 s** na každou otázku a shrnutí, celé dotazování nejvýš 9 min) nerozhodne nebo krabička není připojená, dialog se ukáže v terminálu jako obvykle. Krabička zbývající čas odpočítává. Schválení plánu (ExitPlanMode) zůstává vždy v terminálu: plán je dlouhý text a terminál k němu nabízí i volby, které hook vrátit neumí (vymazání kontextu, automatický režim). Se starším firmwarem krabička zvládne jen jednu otázku s jedinou volbou a povolení bez „už se neptat“. V režimu oprávnění auto se Claude Code ptá málokdy, krabička pak hlavně ukazuje průběh práce.

**Zapnutí** (zapíše hooky do `~/.claude/settings.json`, ostatní nastavení a cizí hooky nechá beze změny a před změnou udělá zálohu `settings.json.bak`):

```powershell
python bridge/rozhodovac_ctl.py enable-claude
```

Projeví se v nově spuštěném Claude Code. Jen pro jeden projekt: `enable-claude --dir C:\cesta\k\projektu` (zápis do `.claude/settings.local.json` projektu). Vypnutí: `disable-claude` (s `--dir` v projektu). Hook si daemon mostu spustí sám při první události, ručně `python bridge/rozhodovac_ctl.py start`.

Události průběhu běží v Claude Code na pozadí (`async`) a práci nezdržují. Na krabičku čeká jen hook `PermissionRequest` (limit 600 s kvůli otázkám po krocích, po změně verze mostu znovu spusť `enable-claude`). Tokeny most čte ze záznamu relace (`transcript_path`), u obnovené relace započítá jen nové odpovědi. Když se na krabičce nic neděje, spusť Claude Code s proměnnou prostředí `ROZHODOVAC_HOOK_DEBUG=1`: hook zapisuje do `bridge/claude_hook.log`, co dostal a jestli to most přijal.

Souběh s Claude Desktopem: aplikace drží krabičku přes Bluetooth, most přes USB. Krabička ukazuje toho, kdo zrovna pracuje nebo se ptá (viz oddíl 2).

**Protokol obrazovky otázky** (pro vývojáře): firmware v odpovědi na `{"cmd":"status"}` hlásí `"ui":2`. Jen tehdy most posílá řádek až 3800 B a dotaz s `"q":1` a poli `head` (záhlaví), `descs` (popisy voleb), `multi` (vícenásobný výběr), `sel` (předvybrané volby od 0), `back` a `term` (tlačítka ZPĚT a TERMINÁL), `ok` (popisek potvrzovacího tlačítka) a `cont` (další krok téhož dotazování, jen cvakne). Krabička odpoví `decision` s popiskem volby a jejím pořadím `opt`, u vícenásobného výběru `"multi"` s polem `opts`, jinak `"back"`, `"terminal"` nebo `"ok"` (potvrzení shrnutí). Každý krok dotazování má vlastní `id`. Firmware bez `ui` dostane jen jednu otázku s jedinou volbou v dosavadním tvaru.

---

## Struktura projektu

- `platformio.ini`, `partitions.csv` — konfigurace PlatformIO pro `ESP32-S3R8` (16 MB Flash, 8 MB OPI PSRAM, USB CDC).
- `include/pin_config.h` — zapojení pinů dle schématu Waveshare Rev2.
- `src/hw_waveshare.*` — ovladače QSPI AMOLED `CO5300` (480×480 PSRAM Canvas), UTF-8 české vykreslování písma, dotyku `CST9220`, napájení `AXP2101`, akcelerometru `QMI8658`, hodin `PCF85063` a zvuku `ES8311`.
- `src/ble_bridge.*`, `src/data.h`, `src/xfer.h`, `src/stats.h` — protokol Nordic UART BLE + USB Serial JSON (jeden JSON řádek do 4 KB), volby a statistiky.
- `src/main.cpp`, `src/buddy.*`, `src/character.h` — dotykové 480×480 UI, dialogy rozhodování, celoobrazovkový chat a animace maskotů Clawd a Agy.
- `bridge/agy_buddy_daemon.py` — daemon, který drží spojení s krabičkou a obsluhuje lokální HTTP.
- `bridge/agy_hook.py` — hook pro AGY, rozhoduje co jde na krabičku.
- `bridge/claude_hook.py` — hook pro Claude Code v terminálu: průběh práce, žádosti o povolení a otázky.
- `bridge/rozhodovac_ctl.py` — ovládání z příkazové řádky, `bridge/rozhodovac_common.py` — sdílené nastavení mostu.
- `docs/` — prezentační web se simulátorem. Obrázky v galerii jsou rendery, ne fotografie: galerie načítá ostré zmenšeniny `gallery_*_600.jpg` a `_1200.jpg`, po kliknutí se otevře plné rozlišení `gallery_*.jpg` (2000 × 1250).
- `tools/fw_screen.py` — Python port kreslení obrazovek firmwaru (písmo glcdfont, české akcenty, rozvržení, maskoti Clawd a Agy). Z něj je i SVG maskota Agy v simulátoru. Při změně rozvržení v `src/main.cpp` nebo maskota v `src/buddy.cpp` ho je potřeba upravit taky. Dotaz pro AGY a Claude Desktop kreslí jen se dvěma volbami, rozvržení 3 a více voleb pod sebou v galerii není. Obrazovku otázky pro Claude Code v terminálu kreslí `question_screen()`, ukázky jsou v `Q_SCENES` (zatím mimo galerii).
- `tools/render_gallery.py` — vykreslí galerii webu: obrazovky z `fw_screen.py` na 3D modelu krabičky podle rozměrového výkresu Waveshare. Spuštění: `python -m pip install pillow numpy` a pak `python tools/render_gallery.py`.
- `lib/` — přibalené knihovny GFX Library for Arduino, SensorLib a XPowersLib.
- `LICENSE`, `LICENSES/` — licence projektu a převzatých částí.

---

## Původ

Firmware vychází z referenčního projektu [claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy) od Anthropicu, který popisuje protokol Hardware Buddy pro Claude Desktop. Z něj pochází základ spojení přes Bluetooth LE, zpracování JSON zpráv a statistik (`src/ble_bridge.*`, `src/data.h`, `src/stats.h`, `src/xfer.h`, části `src/main.cpp`). Displej, dotykové rozhodování, maskoti, zvuky, most pro AGY a web jsou napsané pro tuto krabičku.

## Licence

Rozhodovač © 2026 Stanislav Kaska, licence [CC BY-NC-ND 4.0](https://creativecommons.org/licenses/by-nc-nd/4.0/deed.cs) (Uveďte původ-Neužívejte komerčně-Nezpracovávejte). Projekt si můžete stáhnout, používat a nezměněný sdílet s uvedením autora. Nesmíte ho prodávat ani jinak užívat komerčně a nesmíte šířit upravené verze.

Výjimky: části převzaté z claude-desktop-buddy jsou pod licencí MIT (© 2026 Anthropic, PBC), ovladač kodeku `src/es8311.*` od Espressifu pod licencí Apache 2.0 a knihovny ve složce `lib/` mají vlastní licence (BSD, MIT). Podrobnosti v souboru [LICENSE](LICENSE).

Claude, Claude Code, Claude Desktop a postavička Clawd jsou ochranné známky a dílo společnosti Anthropic, PBC, Antigravity je ochranná známka společnosti Google LLC. Rozhodovač je neoficiální projekt a není s těmito společnostmi nijak spojen.
