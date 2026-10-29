# Mic Boost + AI Noise Suppression (Universal) v3.0

Pembaruan modul **Mic_VolBoost_Swap_A2L**. Sekarang ada noise suppression
untuk semua jalur mikrofon — voice note, rekam suara, voice typing, telepon
internet (WhatsApp/Telegram/Discord/Meet/Zoom), dan telepon biasa — dan
modulnya bisa dipasang di HP apa pun.

## Fitur

| Fitur | Untuk apa |
| --- | --- |
| **AI noise suppression (RNNoise)** | Peredam bising berbasis jaringan saraf (mirip Krisp/Discord) untuk sumber `mic` (voice note, perekam) dan `voice_recognition` (voice typing, asisten). |
| **Voice gate** | Jeda saat tidak bicara dibuat senyap (-15 dB), seperti "voice activity" Discord. |
| **Suara lebih jelas** | Low-cut 80 Hz, EQ presence 3 kHz, dan leveler otomatis yang mengangkat suara pelan (maks +12 dB) plus limiter anti-pecah. |
| **AEC + NS untuk telepon internet** | Mengaktifkan peredam gema dan bising bawaan HP untuk semua aplikasi VoIP (`voice_communication`). |
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

### Hasil uji (RNNoise, suara + bising pada SNR 5 dB)

| Jenis bising | Pengurangan di jeda | Catatan |
| --- | --- | --- |
| AC, kipas, mesin fotokopi, vacuum | 17–38 dB (31–53 dB dengan voice gate) | sangat efektif |
| Keramaian / suara orang lain | 1–8 dB | batas model RNNoise |
| Ketikan keyboard | 0–15 dB | batas model RNNoise |

Suara utama tetap utuh (berubah < 0,5 dB tanpa leveler; leveler
mengangkatnya sekitar +4 dB pada contoh uji).

## Pengaturan

Edit `/data/adb/modules/Mic_VolBoost_Swap_A2L/settings.conf`, lalu pasang
ulang zip yang sama dan reboot. Pengaturan dipertahankan saat update.

| Kunci | Bawaan | Arti |
| --- | --- | --- |
| `AI_NS` | 1 | AI noise suppression RNNoise |
| `NS_STRENGTH` | 90 | Kekuatan 0–100 |
| `VOICE_GATE` / `VOICE_GATE_DB` | 1 / 15 | Senyapkan jeda, kedalaman dB |
| `VOICE_LOWCUT` | 1 | Potong dengung < 80 Hz |
| `VOICE_CLARITY` | 1 | EQ presence agar lebih jelas |
| `VOICE_LEVELER` | 1 | Penyeimbang volume otomatis |
| `VOICE_GAIN_DB` | 0 | Gain tetap -12..12 dB |
| `NS_CAMCORDER` | 0 | Peredam juga saat rekam video |
| `NS_VOIP` | 1 | AEC + NS untuk telepon internet |
| `FLUENCE` | auto | Qualcomm Fluence: auto / force / off |
| `FLUENCE_RECORD` | 1 | Fluence saat merekam |
| `A2L_MIXER` | auto | Mixer Mi A2 Lite: auto / force / off |

Nilai `NS_STRENGTH`, `VOICE_*` juga ada di `micns.conf` di dalam modul dan
dibaca ulang setiap kali rekaman dimulai.

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
- Setelah update ROM/OTA, pasang ulang modul agar patch dibuat dari file baru.
- Laporan instalasi: `micns_report.txt` di folder modul. Log efek:
  `logcat -s MicNS`.

## Kredit dan lisensi

- Modul asli Mic_VolBoost_Swap_A2L: **pexxie**
- RNNoise (Jean-Marc Valin, Xiph.Org) dan resampler SpeexDSP (Xiph.Org),
  lisensi BSD 3-clause — lihat folder `licenses/`.
