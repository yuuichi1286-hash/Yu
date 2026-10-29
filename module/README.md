# Mic Boost + AI Noise Suppression (Universal) v3.1

Pembaruan modul **Mic_VolBoost_Swap_A2L**. Sekarang ada noise suppression
untuk semua jalur mikrofon — voice note, rekam suara, voice typing, telepon
internet (WhatsApp/Telegram/Discord/Meet/Zoom), dan telepon biasa — dan
modulnya bisa dipasang di HP apa pun.

**Baru di v3.1:** peredam tahap kedua (post-filter Wiener) agar sisa bising
makin bersih, AI noise suppression juga di telepon internet / open mic, dan
**suara bass/deep yang natural** saat bicara.

## Fitur

| Fitur | Untuk apa |
| --- | --- |
| **AI noise suppression (RNNoise)** | Peredam bising berbasis jaringan saraf (mirip Krisp/Discord) untuk sumber `mic` (voice note, perekam), `voice_recognition` (voice typing, asisten) dan `voice_communication` (telepon internet, open mic). |
| **Post-filter Wiener** | Tahap kedua setelah AI: sisa bising (kipas, keyboard, mesin) ditekan lagi 2–10 dB tanpa membuat suara cempreng. |
| **Voice gate** | Jeda saat tidak bicara dibuat senyap (-18 dB), seperti "voice activity" Discord. |
| **Suara deep natural** | Nada turun 2 semitone dengan formant dijaga (bukan efek robot/slow motion), formant turun tipis 4 % dan bass 150 Hz +4 dB: suara lebih berat dan berisi. Bisa diatur atau dimatikan. |
| **Suara lebih jelas** | Low-cut 80 Hz, EQ presence 3 kHz, dan leveler otomatis yang mengangkat suara pelan (maks +12 dB) plus limiter anti-pecah. |
| **Telepon internet / open mic** | Peredam gema (AEC) + NS bawaan HP, lalu AI noise suppression + suara deep di belakangnya (WhatsApp, Telegram, Discord, game, Meet, Zoom). |
| **Qualcomm Fluence** | Peredam bising multi-mic di DSP untuk telepon biasa, speakerphone, voice typing, dan rekaman. |
| **Mi A2 Lite** | Mixer lama tetap dipasang (mic volume boost + tukar mic kiri/kanan stereo), hanya di Mi A2 Lite. |
| **Tombol Action** | AI noise suppression on/off **tanpa reboot** (Magisk 28+, KernelSU, APatch). |

## Cara kerjanya di tiap HP (penting)

Android tidak punya satu tombol noise suppression untuk semua HP, jadi modul
ini memakai jalur yang memang dipakai sistem:

1. **Konfigurasi efek audio** (`audio_effects.xml` / `.conf`) di-patch
   otomatis saat instalasi: setiap jalur rekam mendapat efek noise
   suppression. File asli HP dibaca lalu hanya *ditambah*, jadi efek bawaan
   HP (Dolby, dll.) tetap ada.
2. **HP MediaTek, Unisoc, dan HP lain yang HAL audionya menjalankan efek
   software**: AI RNNoise berjalan langsung pada suara mikrofon.
3. **HP Qualcomm (termasuk Mi A2 Lite)**: HAL Qualcomm tidak menjalankan
   efek software di jalur rekam; noise suppression dikerjakan **DSP**.
   Modul ini memicu DSP lewat konfigurasi tadi (ECNS untuk VoIP, NS untuk
   voice typing) dan menyalakan **Fluence** (peredam dual-mic) untuk
   telepon, speaker, voice typing, dan rekaman/voice note.
4. **Telepon biasa (seluler)** selalu diproses DSP/modem. Di Qualcomm modul
   ini menyalakan Fluence; di chip lain dipakai peredam bawaan HP.

Aplikasi yang punya peredam sendiri (Discord/Krisp, WhatsApp, Telegram)
tetap memakai peredamnya; modul ini menambah lapisan di bawahnya.

### Hasil uji (suara + bising pada SNR 5 dB, 48 kHz)

| Jenis bising | Pengurangan di jeda | Dengan voice gate |
| --- | --- | --- |
| AC, kipas, vacuum, mesin fotokopi | 22–40 dB | 37–58 dB |
| Pintu dibanting, kunyahan | 4–20 dB | 21–38 dB |
| Ketikan keyboard | 5 dB (v3.0: 0 dB) | 23 dB (v3.0: 18 dB) |
| Keramaian / suara orang lain | 1–10 dB | batas model RNNoise |

Post-filter menambah 0,5–10 dB peredaman dibanding v3.0 dan kualitas suara
(SI-SDR) sama atau lebih baik di semua jenis bising. Suara utama tetap utuh
(berubah ≤ 1 dB tanpa leveler).

Suara deep: nada terukur turun 0,892–0,896× (target 2 semitone = 0,891×)
untuk suara pria (110 Hz) maupun wanita (200–230 Hz), tanpa klik atau desis
tambahan. Tambahan jeda (latensi) 35 ms, tidak terasa saat telepon.

## Pengaturan

Edit `/data/adb/modules/Mic_VolBoost_Swap_A2L/settings.conf`, lalu pasang
ulang zip yang sama dan reboot. Pengaturan dipertahankan saat update.

| Kunci | Bawaan | Arti |
| --- | --- | --- |
| `AI_NS` | 1 | AI noise suppression RNNoise |
| `NS_STRENGTH` | 90 | Kekuatan 0–100 |
| `NS_POSTFILTER` | 1 | Peredam tahap kedua (Wiener) |
| `VOICE_GATE` / `VOICE_GATE_DB` | 1 / 18 | Senyapkan jeda, kedalaman dB |
| `VOICE_LOWCUT` | 1 | Potong dengung < 80 Hz |
| `VOICE_CLARITY` | 1 | EQ presence agar lebih jelas |
| `VOICE_LEVELER` | 1 | Penyeimbang volume otomatis |
| `VOICE_GAIN_DB` | 0 | Gain tetap -12..12 dB |
| `VOICE_DEEP` | 1 | Suara bass/deep natural |
| `DEEP_PITCH` | -2 | Nada (semitone): -1 halus, -2 natural, -3 berat, -6..6 |
| `DEEP_FORMANT` | -4 | Formant (%): -4 lebih "besar", 0 asli, -15..15 |
| `DEEP_BASS` | 4 | Bass 150 Hz (dB), 0–12 |
| `NS_CAMCORDER` | 0 | Peredam juga saat rekam video |
| `NS_VOIP` | 1 | AEC + NS bawaan HP untuk telepon internet |
| `NS_VOIP_AI` | 1 | AI NS + suara deep juga di telepon internet / open mic |
| `FLUENCE` | auto | Qualcomm Fluence: auto / force / off |
| `FLUENCE_RECORD` | 1 | Fluence saat merekam |
| `A2L_MIXER` | auto | Mixer Mi A2 Lite: auto / force / off |

Nilai `NS_*`, `VOICE_*` dan `DEEP_*` juga ada di `micns.conf` di dalam modul
dan dibaca ulang setiap kali rekaman dimulai (cukup edit file itu untuk
mencoba nilai lain tanpa reboot).

## Kompatibilitas

- Android 6.0 sampai 15+, arm64, arm, x86, x86_64
- Magisk 20.4+, KernelSU, APatch (tombol Action butuh Magisk 28+)
- Mendukung `audio_effects.xml`, `audio_effects.conf`, konfigurasi AIDL
  (`audio_effects_config.xml`, Android 14+), dan partisi `/odm` terpisah

## Jika ada masalah

- Rekaman jadi terlalu pelan di Qualcomm: `FLUENCE_RECORD=0`.
- Suara aneh / tidak ada suara saat rekam: `FLUENCE=off`, lalu `AI_NS=0`
  bila perlu, pasang ulang, reboot. Menonaktifkan modul selalu
  mengembalikan semua file asli.
- Memakai modul audio lain (ViPER4Android, Dolby, JamesDSP) yang juga
  mengganti `audio_effects`: pasang modul ini **setelah** modul itu.
  Installer memberi peringatan bila mendeteksinya.
- Suara deep terlalu berat: `DEEP_PITCH=-1`, atau `DEEP_FORMANT=0`; tidak
  mau sama sekali: `VOICE_DEEP=0`.
- Lawan bicara mendengar gema / suara terputus di telepon internet:
  `NS_VOIP_AI=0`.
- Setelah update ROM/OTA, pasang ulang modul agar patch dibuat dari file baru.
- Laporan instalasi: `micns_report.txt` di folder modul. Log efek:
  `logcat -s MicNS`.

## Kredit dan lisensi

- Modul asli Mic_VolBoost_Swap_A2L: **pexxie**
- RNNoise (Jean-Marc Valin, Xiph.Org) dan resampler SpeexDSP (Xiph.Org),
  lisensi BSD 3-clause — lihat folder `licenses/`.
