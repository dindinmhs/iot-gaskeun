# ESP32 Gaskeun

Upload `esp32.ino` dengan board ESP32 yang sesuai. Dependensi: ArduinoJson;
WiFi, WebServer, DNSServer, Preferences, dan LittleFS sudah disertakan dalam
paket board ESP32. Tidak perlu memasang library WiFiManager tambahan.

## Konfigurasi awal

Jika belum ada, salin `esp32_secrets.h.example` menjadi `esp32_secrets.h`.
File rahasia ini diabaikan Git. Isi konfigurasi Firebase dan identitas perangkat.
`WIFI_SSID` / `WIFI_PASSWORD` menjadi jaringan awal; keduanya boleh kosong jika
ingin mengatur jaringan pertama melalui hotspot.

Isi `OFFLINE_AP_SSID` (1-32 byte) dan `OFFLINE_AP_PASSWORD` (8-63 byte).
Password hotspot tidak boleh bernilai `GANTI_PASSWORD_HOTSPOT`. Hotspot dan
pengaturan WiFi tidak tersedia jika konfigurasi hotspot belum valid.

## Mengganti jaringan WiFi

1. Saat WiFi tidak tersambung selama 10 detik, ESP32 mengaktifkan hotspot
   `OFFLINE_AP_SSID`. Jika belum ada jaringan awal/tersimpan, hotspot aktif
   langsung. Hotspot juga tetap tersedia saat Firebase tidak terjangkau.
2. Hubungkan HP/laptop ke hotspot dengan `OFFLINE_AP_PASSWORD`. Pilih tetap
   tersambung jika perangkat memperingatkan bahwa hotspot tidak punya internet.
3. Buka portal yang muncul, atau buka `http://192.168.4.1/` secara manual.
   Alamat hotspot juga dicetak di Serial Monitor (115200 baud).
4. Pada **Pengaturan WiFi**, masukkan SSID jaringan 2,4 GHz dan password,
   kemudian tekan **Hubungkan dan simpan**. Kosongkan password untuk jaringan
   terbuka. Tidak ada pemindaian jaringan; nama SSID dimasukkan manual.
5. Tunggu hingga 20 detik. Jika berhasil, jaringan disimpan di NVS dan digunakan
   pada boot berikutnya, mendahului jaringan awal di file rahasia. Jika gagal,
   pengaturan lama dipertahankan dan hotspot tetap melayani percobaan berikutnya.

Saat WiFi berhasil tersambung, Serial Monitor (115200 baud) menampilkan nama
jaringan dan alamat IP ESP32 dari router, misalnya `IP ESP32: 192.168.1.20`.
IP juga ditampilkan pada status WiFi di portal selama hotspot masih aktif.
Alamat IP dicetak kembali saat reconnect atau jika alamat dari router berubah.

Pada pemakaian normal, auto-reconnect ESP32 tetap aktif dan retry menggunakan
`WiFi.reconnect()` seperti firmware awal. Koneksi hanya diputus secara eksplisit
ketika mencoba jaringan baru dari form atau kembali dari percobaan yang gagal.

Penggantian jaringan tidak me-restart ESP32; pembacaan UART Nano tetap diproses.
Hotspot mati otomatis setelah Firebase dan pengiriman data kembali sehat
selama dua pemeriksaan. Konfigurasi WiFi tetap dapat dilakukan meskipun
konfigurasi Firebase belum lengkap. Form dan API hanya bisa diakses melalui
hotspot ESP32, dan password WiFi tidak dikirim balik oleh API.

## Pemeriksaan pada perangkat

- Boot tanpa WiFi: hotspot muncul dan halaman sensor tetap dapat dibuka.
- Masukkan password salah: setelah timeout, coba lagi lewat hotspot yang sama.
- Masukkan kredensial benar: koneksi berhasil dan Firebase kembali mengirim data.
- Matikan/nyalakan ESP32: jaringan baru digunakan tanpa mengubah file rahasia.
- Matikan router: setelah 10 detik, hotspot muncul lagi untuk mengganti jaringan.
- Periksa Serial Monitor saat mengatur WiFi: data Nano tetap diterima.

## Jika login Firebase gagal dengan HTTPClient -1

Nilai `-1` berasal dari HTTPClient di ESP32 saat koneksi gagal sebelum
menerima status HTTP Firebase. Log `[HTTPS]` menunjukkan kegagalan DNS atau
detail koneksi TCP/TLS, beserta IP, gateway, DNS, waktu epoch, dan sisa memori.
Hostname dicetak tanpa URL lengkap, API key, password, atau token Auth.

- `DNS gagal`: periksa DNS router dan apakah jaringan memiliki akses internet.
- `TLS=0` saat koneksi gagal: periksa akses internet, portal login jaringan,
  atau pemblokiran koneksi HTTPS/port 443.
- Error sertifikat TLS: periksa waktu UTC ESP32 dan sertifikat CA.
- Respons HTTP positif dengan pesan Firebase: periksa pesan tersebut untuk
  konfigurasi API key, akun perangkat, atau pengaktifan login Email/Password.

Saat meminta bantuan, sertakan log `[HTTPS]` dari Serial Monitor setelah
mengunggah firmware ini. Jangan sertakan file `esp32_secrets.h`.
