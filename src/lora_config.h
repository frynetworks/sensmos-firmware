#pragma once

// ══════════════════════════════════════════════════════════════
// LoRa (SX1262) — build LOKALNY/EKSPERYMENTALNY.
//
// LORA_ENABLED 0 = ani jednej instrukcji w binie → biny floty bez zmian.
// Włączane TYLKO na potrzeby testów na płytce z radiem.
//
// Radio głównie nasłuchuje (skan pasma, odbiór ramek). W trybie LINK dochodzi
// OKRESOWE nadawanie beaconu (lora_scan.cpp: link_tx_beacon → s_radio.transmit),
// ograniczone licznikiem duty cycle i slotami TX. Sam skan/odbiór duty cycle nie
// podlega; nadawanie owszem, stąd budżet w firmwarze.
// ══════════════════════════════════════════════════════════════

#ifndef LORA_ENABLED
#define LORA_ENABLED 0
#endif

// ── Płytka (pinout SX1262) ────────────────────────────────────
// Piny NIE są wybierane w kompilacji. Przy starcie próbujemy kolejnych pinoutów z tablicy,
// aż SX1262 odpowie — dzięki temu JEDEN bin radiowy obsługuje wszystkie płytki, a dołożenie
// nowej to jeden wiersz zamiast: bloku #if, nazwy targetu OTA, wpisu w build-all.ps1, wpisu
// w CHIPS po stronie BE i kolejnego pliku do wydawania na zawsze.
//
// Znika też cała klasa cichych awarii: bin zbudowany pod XIAO, wgrany na Helteca, pukał
// w piny 41/39/42/40 zamiast 8/14/12/13 — node wstawał, WiFi działało, tylko radio było
// głuche i wyglądało to na usterkę anteny.
//
// Sonda jest bezpieczna: nic nie nadajemy, RadioLib zwraca RADIOLIB_ERR_CHIP_NOT_FOUND, gdy
// układ nie odpowiada na odczyt rejestru — to realne pytanie do krzemu, nie zgadywanka po
// czasie. Nieudana próba dotyka jednak 7 GPIO, które na innej płytce mogą należeć do
// wyświetlacza albo Vext, dlatego lista jest KRÓTKA i celowo nie zawiera wszystkich 27
// pinoutów znanych z Meshtastica (pełna tabela: DOCS/dev/LORA-PINOUTS.md).

#include <stdint.h>
// sdkconfig.h wnosi CONFIG_IDF_TARGET_* — bez niego #if defined(CONFIG_IDF_TARGET_ESP32)
// nizej jest ZAWSZE falszywe i goly ESP32 dostaje tablice pinoutow S3 (piny 6-11 = SPI flash),
// co wiesza sonde radia w petli TG1WDT jeszcze przed wierszem T-Beama. Ten sam typ pulapki
// co w fw_digest.h (0.90): makro rodziny musi byc widoczne, zanim sie je testuje.
#include <sdkconfig.h>

// Rodzina układu radiowego. Do 0.89 tablica opisywala WYLACZNIE SX1262 i typ byl domyslny;
// T-Beam v1.1 (SX1276) wymusil jawne pole, bo obie rodziny roznia sie w RadioLib sygnaturami
// begin()/beginFSK(), zrodlem przerwania (DIO1 vs DIO0), sterowaniem CRC/whiteningiem oraz
// tym, ze TCXO/DIO2-RF-switch istnieja tylko w SX126x. Patrz LoraRadio w lora_scan.cpp.
#define LORA_CHIP_SX1262  0
#define LORA_CHIP_SX1276  1

struct LoraPinout {
    const char* name;
    int8_t nss, dio1, rst, busy, sck, miso, mosi, rxen;   // rxen < 0 = brak przełącznika
    float  tcxo;                                          // napięcie TCXO z DIO3 (SX126x; SX127x: 0)
    uint8_t chip;                                         // LORA_CHIP_*
    int8_t  dio0;                                         // SX127x: pin przerwania; SX126x: -1
    uint8_t pmu;                                          // 1 = radio zasilane przez AXP192 (LDO2)
};

// Kolejność: najpierw to, co mamy we własnej flocie, potem popularne. Wpisy zweryfikowane
// z variants/*/variant.h Meshtastica; XIAO i Heltec dodatkowo potwierdzone na naszym sprzęcie.
// SZESC PIERWSZYCH WIERSZY JEST NIETYKALNE i sondowane w tej samej kolejnosci co dotad —
// plytki S3 maja zachowywac sie identycznie jak przed dolozeniem SX1276.
//
// ...ale WYLACZNIE na S3. Wszystkie szesc to pinouty ESP32-S3 i na GOLYM ESP32 ich numery
// albo nie istnieja (41/42/47/48/33/34 > GPIO39), albo — co gorsza — trafiaja w GPIO 6-11,
// czyli w linie wbudowanego SPI flash. Sonda sterujaca tymi pinami wiesza plytke, zanim
// dojdzie do wiersza T-Beama. Do 0.89 bylo to niegrozne, bo goly ESP32 nigdy nie byl
// budowany z LORA_ENABLED; T-Beam v1.1 to zmienia, wiec tutaj tablica jest per rodzina.
// Zachowanie S3 nie zmienia sie ani o krok: tam kompiluje sie dokladnie te szesc wierszy,
// w tej samej kolejnosci, i zaden wiersz SX1276 sie nie dokłada.
#if defined(CONFIG_IDF_TARGET_ESP32)
  #define LORA_PINOUTS_SX1262
#else
//                      nazwa           nss dio1 rst busy  sck miso mosi rxen  tcxo  chip   dio0 pmu
#define LORA_PINOUTS_SX1262 \
    { "xiao-s3",       41, 39, 42, 40,   7,   8,   9,  38, 1.8f, LORA_CHIP_SX1262, -1, 0 },  /* Seeed XIAO S3 + Wio-SX1262 (B2B)                */ \
    { "heltec-s3",      8, 14, 12, 13,   9,  11,  10,  -1, 1.6f, LORA_CHIP_SX1262, -1, 0 },  /* Heltec V3/V4/Wireless Paper/WSL/Vision/Tracker   */ \
    { "heltec-s3@1v8",  8, 14, 12, 13,   9,  11,  10,  -1, 1.8f, LORA_CHIP_SX1262, -1, 0 },  /* te same piny — Meshtastic podaje 1.8 zamiast 1.6 */ \
    { "lilygo-t3s3",    7, 33,  8, 34,   5,   3,   6,  -1, 1.8f, LORA_CHIP_SX1262, -1, 0 },  /* LilyGo T3-S3, T3-S3 e-paper, CDEBYTE EoRa-S3     */ \
    { "tbeam-s3-core", 10,  1,  5,  4,  12,  13,  11,  -1, 1.8f, LORA_CHIP_SX1262, -1, 0 },  /* LilyGo T-Beam S3 Core                            */ \
    { "rak3312",        7, 47,  8, 48,   5,   3,   6,  -1, 1.8f, LORA_CHIP_SX1262, -1, 0 },  /* RAK3312, RAK WisMesh Tap v2                      */
#endif

// T-Beam v1.1 to GOLY ESP32 (nie S3): SX1276 na SPI2, brak pinu BUSY (SX127x go nie ma),
// przerwanie z DIO0, brak TCXO sterowanego z radia. Radio jest MARTWE, dopoki AXP192 (I2C 0x34)
// nie zalaczy LDO2 — stad pmu=1. Wiersz istnieje TYLKO w buildzie na golego ESP32: piny 21/22
// (I2C do AXP192) i 27 nie istnieja na S3, a sonda i tak nigdy tu nie dochodzi na plytce,
// ktora odpowiedziala wczesniej jako SX1262.
#if defined(CONFIG_IDF_TARGET_ESP32)
  //                    nazwa           nss dio1 rst busy  sck miso mosi rxen  tcxo  chip   dio0 pmu
  #define LORA_PINOUTS_SX1276 \
    { "tbeam-v1.1",    18,  -1, 23,  -1,   5,  19,  27,  -1, 0.0f, LORA_CHIP_SX1276, 26, 1 },  /* LilyGo T-Beam v1.1 (AXP192 + SX1276)          */
#else
  #define LORA_PINOUTS_SX1276
#endif

#define LORA_PINOUTS { LORA_PINOUTS_SX1262 LORA_PINOUTS_SX1276 }

// AXP192 (T-Beam v1.1) — minimalna inicjalizacja przez Wire, bez dokladania biblioteki PMU.
// Odpowiednik tego, co Meshtastic robi w src/power.cpp przez XPowersLib: LDO2 = 3.3 V zasila
// radio, LDO3 = 3.3 V zasila GPS; oba zalaczane bitami w rejestrze 0x12.
#define AXP192_I2C_ADDR      0x34
#define AXP192_REG_LDO23_V   0x28   // starszy nibble = LDO2, mlodszy = LDO3; V = 1.8 + 0.1*n
#define AXP192_REG_DCDC_EN   0x12   // bit2 = LDO2 on, bit3 = LDO3 on
#define AXP192_LDO23_3V3     0xFF   // n=15 dla obu -> 3.3 V
#define AXP192_SDA           21
#define AXP192_SCL           22

// Furtka: LORA_PIN_FORCE=<indeks> wymusza jeden pinout i pomija sondowanie. Do użycia, gdyby
// jakaś płytka źle znosiła próbowanie cudzych pinów.
#ifndef LORA_PIN_FORCE
#define LORA_PIN_FORCE -1
#endif

// ── Plan kanałów tła (EU863-870) ──────────────────────────────
// 868.1/868.3/868.5 = domyślne kanały uplinku LoRaWAN EU868.
// 869.525 = tam siedzi mesh (Meshtastic/MeshCore) — podpasmo o luźniejszym duty cycle.
#define LORA_BG_CHANNELS   { 868.1f, 868.3f, 868.5f, 867.1f, 869.525f }
#define LORA_BG_SFS        { 7, 9, 11 }

// Preset okna nasłuchu w cyklu tła — domyślnie LoRaWAN EU868 SF7 (sync 0x34 = publiczny).
#define LORA_BG_FREQ      868.1f
#define LORA_BG_BW        125.0f
#define LORA_BG_SF        7
#define LORA_BG_CR        5
#define LORA_BG_SYNC      0x34
#define LORA_BG_LISTEN_S  20

#define LORA_BG_PERIOD_S  300      // pełny cykl tła co 5 min
#define LORA_BG_DEFAULT   true     // czy tło startuje samo po boocie

#define LORA_BUSY_MARGIN_DB  6     // ile dB nad szumem = kanał zajęty
#define LORA_SWEEP_SAMPLES  40     // próbek RSSI na kanał

// ══ Tryb LINK (beacon + ciągły nasłuch + uplink ramek po WS) ══
// Harmonogram bez negocjacji: WSZYSTKIE nody liczą kanał tym samym wzorem z zegara UTC
// (ws_epoch_now), więc w tej samej minucie siedzą na tej samej częstotliwości. Nasłuch to
// stan spoczynkowy radia — okien odbioru nie planujemy, planujemy tylko sloty NADAWANIA.
#define LORA_LINK_DEFAULT     false   // tryb link startuje wyłączony (BE włącza przez lora_cfg)
#define LORA_LINK_MAX_CH      6       // ile pozycji planu kanałów maksymalnie
#define LORA_ENT_PERIOD_S     300     // co ile sekund tryb link wypycha encje RF (mon.lora_*)
#define LORA_LINK_MIN_PER_CH  10      // domyślnie: zmiana kanału co 10 min
#define LORA_LINK_GUARD_S     3       // ±3 s wokół zmiany kanału: nikt nie nadaje (tam robimy sweep)
#define LORA_LINK_SLOT0_S     10      // pierwszy slot beaconu: 10 s po pełnej minucie
#define LORA_LINK_SLOT_GAP_S  7       // odstęp między slotami nadawców
#define LORA_LINK_TX_POWER    14      // dBm — 14 = limit EU868 (ERP 25 mW) dla 868.1/.3/.5

// Duty cycle EU868: 1% na godzinę w podpaśmie. Licznik pilnuje budżetu ZAMIAST dobrej woli —
// beacon odmawia nadania po przekroceniu, nawet gdy BE każe nadawać częściej.
#define LORA_LINK_DUTY_MS_H   36000UL // 1% z 3600 s = 36 s airtime/h

// Nasza ramka: 0xE0 (LoRaWAN "Proprietary" — kulturalnie mówimy obcym bramkom "to nie uplink")
// + ASCII "SMOS <id8> <seq>". Bez szyfrowania: w środku nie ma nic tajnego, a czytelność
// w logach cudzych bramek jest tu zaletą, nie wyciekiem.
#define LORA_BEACON_MAGIC     0xE0
#define LORA_BEACON_PREFIX    "SMOS "
#define LORA_RX_BATCH_MAX     6       // ramek w paczce (mniej, bo payload urósł do 128 B)
#define LORA_RX_CAP_PER_MIN   60      // twardy limit uplinku — w mieście 868.1 potrafi tętnić
// 128 B: ramka wM-Bus po dekodowaniu 3-z-6 traci 1/3 długości, więc przy 32 B zostawało
// 21 B treści — za mało, by rozłożyć nagłówek (L, C, producent, nr seryjny, typ medium).
#define LORA_RX_HEX_MAX       128     // bajtów payloadu wysyłanych jako hex (reszta = same metadane)

// ── Uplink batcha (port z nRF) ────────────────────────────────
// Podpisane paczki danych wychodza z noda jako pociete ramki LoRa:
//   [0xE1]["SMOSB "][id8][' '][seq hex4][' '][idx]['/'][cnt][' '][base64 kawalka]
// 0xE1 = przestrzen "Proprietary" LoRaWAN jak 0xE0 w beaconie, ale INNA magia, zeby odbiorca
// odroznil beacon od paczki. Payload miesci sie w LORA_RX_HEX_MAX (128 B).
#define LORA_UPLINK_MAGIC     0xE1
#define LORA_UPLINK_PREFIX    "SMOSB "
#define LORA_UPLINK_CHUNK     96      // znakow base64 na ramke
#define LORA_UPLINK_BUF       2048    // maks. rozmiar zserializowanej podpisanej paczki

// ── Tryb awaryjny: brak WiFi -> transport LoRa ────────────────
// ZALOZENIE (udokumentowane): SENSMOS po LoRa NIE MA potwierdzen (ACK). Nie da sie wiec
// wykryc "dotarlo/nie dotarlo" — jedynym sensownym wyzwalaczem jest NIEOBECNOSC WiFi, po
// czym nadajemy bezwarunkowo w obie strony (SMOSB + Meshtastic), dokladnie jak port nRF.
// 180 s < WIFI_DOWN_REBOOT_MS (240 s, wifi_manager.cpp) — tryb awaryjny MUSI zdazyc wstac,
// zanim watchdog WiFi zrestartuje plytke, a gdy juz stoi, ten reboot jest wstrzymywany.
#define LORA_FALLBACK_AFTER_S 180

// ══ Plany regionalne (worldwide) ══════════════════════════════
// Do 0.89 kazda czestotliwosc byla literalem EU868 rozsypanym po pliku. Tablica ponizej
// zbiera je w JEDEN wiersz na region; wiersz EU868 zawiera DOKLADNIE te same wartosci co
// wczesniejsze literaly, wiec domyslne zachowanie i tresc logow nie zmieniaja sie ani o bajt.
//
// Kanal 0 Meshtastica liczony wzorem z meshtastic/firmware src/mesh/RadioInterface.cpp:
//   numChannels = floor((freqEnd - freqStart) / (bw/1000))
//   channel_num = hash("LongFast") % numChannels        // hash = djb2, RadioInterface.cpp
//   freq        = freqStart + bw/2000 + channel_num * (bw/1000)
// dla presetu LONG_FAST (BW 250 kHz). djb2("LongFast") = 130429955, stad per region:
//   EU868  869.4 -869.65 -> 1 kan.,  ch  0 -> 869.400 + 0.125 + 0.00 = 869.525
//   US915  902.0 -928.0  -> 104 kan., ch 19 -> 902.000 + 0.125 + 4.75 = 906.875
//   AU915  915.0 -928.0  -> 52 kan.,  ch 19 -> 915.000 + 0.125 + 4.75 = 919.875
//   AS923  920.0 -925.0  -> 20 kan.,  ch 15 -> 920.000 + 0.125 + 3.75 = 923.875
//   IN865  865.0 -867.0  -> 8 kan.,   ch  3 -> 865.000 + 0.125 + 0.75 = 865.875
//   KR920  920.0 -923.0  -> 12 kan.,  ch 11 -> 920.000 + 0.125 + 2.75 = 922.875
//   RU864  868.7 -869.2  -> 2 kan.,   ch  1 -> 868.700 + 0.125 + 0.25 = 869.075
// Granice freqStart/freqEnd i limity mocy Meshtastica pochodzas z tablicy `regions[]` w
// src/mesh/RadioInterface.cpp (regiony EU_868, US, ANZ, TH, IN, KR, RU).
//
// Kanal SENSMOS to podstawowy kanal uplinku danego planu LoRaWAN (RP002-1.0.4):
//   EU868 868.1 | US915 902.3 | AU915 915.2 | AS923-1 923.2 | IN865 865.0625
//   KR920 922.1 | RU864 868.9
// Moc jest MIN(limit regulacyjny, 20 dBm) — 20 dBm to sufit PA_BOOST SX1276, wiec jeden
// wiersz obowiazuje obie rodziny radiowe.
//
// Model pasma: kazdy region ma DWA rozlaczne podpasma, [0] dla SENSMOS i [1] dla Meshtastica,
// rozstrzygane PO CZESTOTLIWOSCI (duty_for_freq) — dokladnie jak w porcie nRF, gdzie sumowanie
// obu protokolow do jednego licznika zaglodzilo je nawzajem.
//   · limit_ms = budzet airtime na okno godzinowe. EU868/RU864 maja realny duty cycle
//     (1% = 36 s/h, 10% = 360 s/h); regiony FCC/AS ida 100% (3 600 000 ms), tak jak
//     `dutyCycle 100` w tablicy Meshtastica — tam prawo nie ogranicza duty, tylko dwell.
//   · dwell_ms = limit POJEDYNCZEJ transmisji (FCC 15.247 / AS923: 400 ms). UPROSZCZENIE:
//     egzekwujemy go na pasmie SENSMOS, gdzie nasze ramki (SF7/BW125, <=133 B) mieszcza sie
//     z duzym zapasem. Na pasmie Meshtastica dwell jest 0 CELOWO: preset LongFast to
//     SF11/BW250, czyli ~1 s w powietrzu — kazda wartosc dwell zablokowalaby caly ruch mesh.
//     Upstreamowy Meshtastic nadaje ten preset bez zmian i interoperacyjnosc tego wymaga;
//     zmiana SF/BW "pod dwell" dalaby ramki, ktorych zaden sasiedni node nie odbierze.
struct LoraBand {
    const char* name;
    float       lo, hi;        // MHz, granice podpasma (wlacznie)
    uint32_t    limit_ms;      // budzet airtime na okno godzinowe
    uint32_t    dwell_ms;      // 0 = brak limitu pojedynczej transmisji
};
struct LoraRegion {
    const char* name;
    float    smos_freq;        // kanal uplinku/beaconu SENSMOS
    int8_t   smos_power;       // dBm
    float    mesh_freq;        // Meshtastic LongFast, kanal 0
    int8_t   mesh_power;       // dBm
    // Calkowity zakres RF regionu — NIE to samo co podpasma duty ponizej.
    // band[] modeluje KSIEGOWANIE airtime i celowo pokrywa tylko te dwa podpasma, na
    // ktorych nadajemy; duty_for_freq od zawsze ma tolerancyjny fallback ("kanal spoza
    // tablicy -> obciaz najostrzejsze pasmo"), bo plan moze legalnie zawierac kanaly,
    // ktorych nie ksiegujemy osobno — 867.1 z LORA_BG_CHANNELS to zwykly kanal EU868.
    // Na pytanie "czy WOLNO tu nadawac" odpowiada WYLACZNIE ta koperta. Uzycie band[]
    // w tej roli odrzucalo poprawne plany EU868.
    float    rf_lo, rf_hi;
    LoraBand band[2];          // [0] pasmo SENSMOS, [1] pasmo Meshtastic
};

// EU868 MUSI byc pierwszy: to domyslny region przy braku wpisu w NVS, a jego wiersz [0]
// (g1) jest jednoczesnie pasmem "najostrzejszym", na ktore duty_for_freq odsyla kanaly
// spoza tablicy (np. 867.1 z LORA_BG_CHANNELS) — tak samo jak przed uogolnieniem.
// Koperta rf_lo/rf_hi to CALE pasmo ISM regionu (nie podpasma duty):
//   EU868 863-870 (ERC 70-03) | US915 902-928 (FCC 15.247) | AU915 915-928 (AS/NZS 4268)
//   AS923-1 920-925 | IN865 865-867 | KR920 920-923.5 | RU864 864-870
#define LORA_REGIONS { \
  { "EU868",  868.1f,    14, 869.525f, 14, 863.0f, 870.0f, \
                                           { { "g1", 868.0f,  868.6f,   36000UL,   0 }, \
                                             { "g4", 869.4f,  869.65f, 360000UL,   0 } } }, \
  { "US915",  902.3f,    20, 906.875f, 20, 902.0f, 928.0f, \
                                           { { "us1", 902.0f, 906.0f, 3600000UL, 400 }, \
                                             { "us2", 906.0f, 928.0f, 3600000UL,   0 } } }, \
  { "AU915",  915.2f,    20, 919.875f, 20, 915.0f, 928.0f, \
                                           { { "au1", 915.0f, 919.0f, 3600000UL, 400 }, \
                                             { "au2", 919.0f, 928.0f, 3600000UL,   0 } } }, \
  { "AS923-1",923.2f,    16, 923.875f, 16, 920.0f, 925.0f, \
                                           { { "as1", 920.0f, 923.5f, 3600000UL, 400 }, \
                                             { "as2", 923.5f, 925.0f, 3600000UL,   0 } } }, \
  { "IN865",  865.0625f, 20, 865.875f, 20, 865.0f, 867.0f, \
                                           { { "in1", 865.0f, 865.5f, 3600000UL,   0 }, \
                                             { "in2", 865.5f, 867.0f, 3600000UL,   0 } } }, \
  { "KR920",  922.1f,    14, 922.875f, 14, 920.0f, 923.5f, \
                                           { { "kr1", 920.0f, 922.5f, 3600000UL,   0 }, \
                                             { "kr2", 922.5f, 923.0f, 3600000UL,   0 } } }, \
  { "RU864",  868.9f,    14, 869.075f, 14, 864.0f, 870.0f, \
                                           { { "ru1", 868.7f, 869.0f,   36000UL,   0 }, \
                                             { "ru2", 869.0f, 869.2f,   36000UL,   0 } } }, \
}
#define LORA_REGION_DEFAULT   0        // EU868
#define LORA_REGION_NAME_MAX  12

// Nazwy zachowane dla czytelnosci wywolan — to po prostu wartosci wiersza EU868.
#define DUTY_G1_LIMIT_MS      36000UL
#define DUTY_G4_LIMIT_MS      360000UL

// ══ Meshtastic — druga polowa dwustosu ════════════════════════
// Ta sama podpisana paczka wychodzi rowniez jako pakiety Meshtastica, wiec istniejaca siec
// mesh (i jej most MQTT) moze przeniesc dane SENSMOS bez zadnej bramki SENSMOS.
// Parametry to preset "LongFast" wg meshtastic/firmware:
//   modem = LONG_FAST: BW 250 kHz, SF11, CR 4/5 (MeshRadio.h modemPresetToParams)
//   sync  = 0x2b (RadioLibInterface.h), preambula 16 symboli (RadioInterface.h)
// MESH_FREQ/MESH_TX_POWER to wartosci wiersza EU868 — od 0.90 czestotliwosc i moc bierze
// sie z aktywnego regionu, a te literaly zostaja jako jego zrodlo i jako domyslka.
#define MESH_FREQ             869.525f
#define MESH_BW               250.0f
#define MESH_SF               11
#define MESH_CR               5
#define MESH_SYNCWORD         0x2B
#define MESH_PREAMBLE         16
#define MESH_TX_POWER         LORA_LINK_TX_POWER
#define MESH_HOP_LIMIT        3

#define MESH_TX_DEFAULT       true          // dwustos wlaczony od razu po starcie
#define MESH_CHANNEL_DEFAULT  "LongFast"
#define MESH_PSK_DEFAULT      "AQ=="        // PSK indeks 1 = domyslny klucz kanalu
#define MESH_CHUNK_RAW        160           // bajtow paczki na jeden pakiet Meshtastica
#define MESH_UPLINK_BUF       2048          // maks. rozmiar zserializowanej podpisanej paczki
