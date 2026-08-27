#pragma once
#include "lora_config.h"

#if LORA_ENABLED
#include <Arduino.h>
#include <RadioLib.h>

// ══ Uchwyt radia: SX1262 ALBO SX1276 ══════════════════════════
// Obie rodziny dziedzicza po PhysicalLayer, ale RÓŻNIĄ SIĘ dokładnie tam, gdzie ten
// projekt ich dotyka: begin()/beginFSK() maja inne ostatnie argumenty (SX126x bierze
// napiecie TCXO i tryb regulatora, SX127x wzmocnienie LNA / flage OOK), przerwanie
// wisi na DIO1 vs DIO0, CRC ustawia sie dlugoscia vs flaga, whitening osobna metoda
// vs setEncoding(), a TCXO i DIO2-jako-przelacznik-RF istnieja tylko w SX126x.
//
// Cienka warstwa ponizej wystawia JEDEN zestaw nazw i sygnatur (kształt SX126x, bo
// tak wyglada caly istniejacy kod wywolujacy), wiec dolozenie SX1276 nie zmienilo ani
// jednego miejsca wywolania w lora_scan.cpp. Obiekt jest ADOPTOWANY, nie posiadany —
// sonda pinoutow tworzy i kasuje kandydatow sama, dokladnie jak przed zmiana.
class LoraRadio {
  public:
    explicit LoraRadio(SX1262* r) : sx62_(r), sx76_(nullptr) {}
    explicit LoraRadio(SX1276* r) : sx62_(nullptr), sx76_(r) {}

    bool is_sx1276() const { return sx76_ != nullptr; }
    const char* chip_name() const { return sx76_ ? "SX1276" : "SX1262"; }

    // SX127x tnie moc w begin() do 17 dBm; realna moc i tak ustawia setOutputPower()
    // zaraz po, gdzie PA_BOOST dopuszcza 20 dBm.
    int16_t begin(float f, float bw, uint8_t sf, uint8_t cr, uint8_t sync,
                  int8_t pwr, uint16_t pre, float tcxo, bool ldo) {
        if (sx76_) return sx76_->begin(f, bw, sf, cr, sync, pwr > 17 ? 17 : pwr, pre, 0);
        return sx62_->begin(f, bw, sf, cr, sync, pwr, pre, tcxo, ldo);
    }
    int16_t beginFSK(float f, float br, float dev, float rxbw,
                     int8_t pwr, uint16_t pre, float tcxo, bool ldo) {
        if (sx76_) return sx76_->beginFSK(f, br, dev, rxbw, pwr > 17 ? 17 : pwr, pre, false);
        return sx62_->beginFSK(f, br, dev, rxbw, pwr, pre, tcxo, ldo);
    }

    int16_t transmit(const uint8_t* d, size_t n) { return sx76_ ? sx76_->transmit(d, n) : sx62_->transmit(d, n); }
    int16_t startReceive()                       { return sx76_ ? sx76_->startReceive() : sx62_->startReceive(); }
    int16_t readData(uint8_t* d, size_t n)       { return sx76_ ? sx76_->readData(d, n) : sx62_->readData(d, n); }
    size_t  getPacketLength(bool upd = true)     { return sx76_ ? sx76_->getPacketLength(upd) : sx62_->getPacketLength(upd); }
    float   getRSSI(bool packet = true)          { return sx76_ ? sx76_->getRSSI(packet) : sx62_->getRSSI(packet); }
    float   getSNR()                             { return sx76_ ? sx76_->getSNR() : sx62_->getSNR(); }
    int16_t scanChannel()                        { return sx76_ ? sx76_->scanChannel() : sx62_->scanChannel(); }
    int16_t standby()                            { return sx76_ ? sx76_->standby() : sx62_->standby(); }
    int16_t setFrequency(float f)                { return sx76_ ? sx76_->setFrequency(f) : sx62_->setFrequency(f); }
    int16_t setOutputPower(int8_t p)             { return sx76_ ? sx76_->setOutputPower(p) : sx62_->setOutputPower(p); }
    int16_t setSyncWord(uint8_t* w, size_t n)    { return sx76_ ? sx76_->setSyncWord(w, n) : sx62_->setSyncWord(w, n); }
    int16_t fixedPacketLengthMode(uint8_t n)     { return sx76_ ? sx76_->fixedPacketLengthMode(n) : sx62_->fixedPacketLengthMode(n); }
    int16_t variablePacketLengthMode(uint8_t n)  { return sx76_ ? sx76_->variablePacketLengthMode(n) : sx62_->variablePacketLengthMode(n); }

    // CRC: SX126x podaje DLUGOSC pola (0 = wylaczone), SX127x flage wlacz/wylacz.
    int16_t setCRC(uint8_t len) { return sx76_ ? sx76_->setCRC(len != 0) : sx62_->setCRC(len); }
    // Whitening: w SX127x to jedno z kodowan linii, nie osobny przelacznik.
    int16_t setWhitening(bool on) {
        return sx76_ ? sx76_->setEncoding(on ? RADIOLIB_ENCODING_WHITENING : RADIOLIB_ENCODING_NRZ)
                     : sx62_->setWhitening(on);
    }
    // Nazwa zostaje "dio1" — tak nazywa to caly istniejacy kod; w SX127x zrodlem jest DIO0.
    void setDio1Action(void (*fn)(void)) {
        if (sx76_) sx76_->setDio0Action(fn, RISING);
        else       sx62_->setDio1Action(fn);
    }
    void setRfSwitchPins(uint32_t rx, uint32_t tx) {
        if (sx76_) sx76_->setRfSwitchPins(rx, tx);
        else       sx62_->setRfSwitchPins(rx, tx);
    }
    // Tylko SX126x — w SX127x przelacznik anteny jest sterowany sprzetowo.
    void setDio2AsRfSwitch(bool on) { if (sx62_) sx62_->setDio2AsRfSwitch(on); }

  private:
    SX1262* sx62_;
    SX1276* sx76_;
};

// Radio chodzi we własnym tasku — wszystkie funkcje poniżej tylko wrzucają zlecenie
// do kolejki i wracają. Skany trwają dziesiątki sekund (łowca sync worda kilka minut),
// więc NIC z tego nie może się wykonywać w loop().
void lora_scan_init();

// Z loop(): opróżnia skrzynkę nadawczą taska radiowego. MUSI być wołane z pętli — task
// nie może sam pisać po WS, bo ws_client trzyma jeden bufor enc i licznik sekwencji
// bezpieczne wyłącznie w kontekście loop.
void lora_pump();

bool lora_available();    // radio wystartowało (płytka faktycznie ma SX1262)
const char* lora_board_name();   // nazwa wykrytej płytki albo nullptr
bool lora_busy();         // trwa zlecenie

// Zwracają false gdy radia nie ma albo kolejka zajęta.
bool lora_sweep(float from, float to, float step);
bool lora_camp(float freq, uint16_t secs);
bool lora_listen(float freq, float bw, uint8_t sf, uint8_t cr, uint8_t sync, uint16_t secs);
bool lora_cad(float freq, float bw, uint8_t sf, uint16_t secs);   // detekcja preambuły — bez sync worda
bool lora_hunt(float freq, float bw, uint8_t sf, uint8_t cr, uint16_t dwell_ms);

void lora_json(String& out);   // stan pomiarowy dla GET /lora/last
void lora_bg_set(bool on);
bool lora_bg_get();
void lora_status();

// ── Tryb LINK (beacon + ciągły nasłuch + uplink po WS) ────────
// Konfiguracja przychodzi z BE ramką lora_cfg; parsuje ją ws_client i woła to.
// mode 0 = LoRa (sf/cr/sync), mode 1 = FSK (br/dev/syncb + flagi).
// Pole bw służy obu: dla LoRa to szerokość pasma, dla FSK szerokość filtru odbiornika.
struct LoraLinkCh {
    float   freq;
    float   bw;
    uint8_t sf;
    uint8_t cr;
    uint8_t sync;
    uint8_t mode;        // 0 LoRa | 1 FSK
    float   br;          // FSK: kbps
    float   dev;         // FSK: dewiacja kHz
    uint8_t syncn;       // FSK: ile bajtów sync (0 = bez sync worda)
    uint8_t syncb[8];    // FSK: bajty sync worda
    uint8_t flags;       // FSK: bit0 = CRC 2B, bit1 = whitening, bit2 = stała długość
    uint8_t len;         // FSK: długość ramki przy stałej długości
};
// seed = 16 B sekretu do kodu w beaconie (nullptr = brak, kasuje poprzedni).
void lora_link_set(bool on, bool beacon, uint8_t slot, uint16_t beacon_s,
                   uint8_t min_per_ch, const LoraLinkCh* chans, uint8_t n_chans,
                   const uint8_t* seed);
bool lora_link_on();
void lora_link_status_json(String& out);

// ── Region (plan czestotliwosci) ──────────────────────────────
// Jeden wiersz z LORA_REGIONS rzadzi kanalem SENSMOS, kanalem Meshtastica, limitami
// mocy i modelem duty/dwell obu podpasm. Domyslka (brak wpisu w NVS) = EU868.
const LoraRegion* lora_region();
const char*       lora_region_name();
// false = nieznana nazwa; konfiguracja pozostaje NIETKNIETA (odpowiedz "bad_region").
bool              lora_region_set(const char* name);

// ── Uplink podpisanej paczki po SMOSB ─────────────────────────
// Jedna paczka w locie; kolejne ramki wychodza po jednej na sekunde z link_tick.
bool lora_uplink_enqueue(const char* json, size_t len);
bool lora_uplink_pending();

// ── Tryb awaryjny (brak WiFi -> transport LoRa) ───────────────
// lora_fallback_tick() woła się z loop(); ocenia stan WiFi i wchodzi/wychodzi z trybu.
// lora_fallback_active() czyta wifi_manager, zeby WSTRZYMAC twardy reboot watchdoga —
// node bez WiFi, ale nadajacy po LoRa, pracuje i restart tylko urwalby mu transport.
void lora_fallback_tick();
bool lora_fallback_active();
#endif
