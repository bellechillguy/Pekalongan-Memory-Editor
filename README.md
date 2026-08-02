# Pekalongan Memory Editor

Pekalongan Memory Editor adalah memory scanner untuk process 64-bit di Linux dan Windows. Program dapat mencari nilai saat target berjalan, menyaring candidate setelah nilainya berubah, lalu membaca atau menulis alamat yang dipilih.

Project ini dibuat untuk latihan process API dan virtual memory. Gunakan hanya pada program milik sendiri, target praktikum yang memang diizinkan, atau game offline. Jangan gunakan pada game online atau process milik orang lain.

## Dukungan platform

| Sistem | Menjalankan editor | Menjalankan target |
|---|---|---|
| Linux x64 | Native | Target Linux x64 |
| Windows x64 | Native | Target Windows x64 |
| macOS Intel atau Apple Silicon | Melalui Docker atau VM | Target Linux x64 di container, atau target Windows x64 di VM |

macOS belum menjadi target native. Binary Linux atau Windows tidak dapat memindai process macOS karena format executable dan API memory berbeda. Pada macOS, editor dan target harus dijalankan di container atau VM yang sama.

## Fitur

| Fitur | Status |
|---|---|
| Memilih target dari daftar process | Tersedia |
| First Scan Exact Value | Tersedia |
| Next Scan Exact, Changed, Unchanged, Increased, dan Decreased | Tersedia |
| Membaca dan menulis memory | Tersedia |
| `int8`, `uint8`, `int16`, `uint16`, `int32`, `uint32`, `int64`, `uint64`, `float`, dan `double` | Tersedia |
| Unknown Initial Value | Tersedia |
| Freeze Value setiap 50 ms | Tersedia |
| GUI Tkinter | Tersedia |
| Single-thread dan multi-thread scan | Tersedia |
| Signature scan dengan wildcard | Tersedia |
| Pointer chain scan dan replay | Tersedia |
| Save dan load konfigurasi | Tersedia |
| Target demo kedua | Tersedia |
| Satu logic scanner untuk Linux x64 dan Windows x64 | Tersedia |
| Backend alternatif `/proc/<pid>/mem` | Linux x64 |
| Enumerasi region melalui `/proc/<pid>/smaps` | Linux x64 |
| Akses tanpa mekanisme user mode | Tidak tersedia |

Bonus terakhir tidak diimplementasikan karena memerlukan driver, kernel module, atau komponen privileged lain. Project ini tidak mencoba melewati permission sistem operasi.

## Arsitektur

CLI dan GUI memakai engine C++ yang sama. `MemoryScanner` bekerja melalui interface `ProcessMemory`, sehingga logic scan tidak diduplikasi untuk setiap sistem operasi.

```text
CLI atau GUI
    |
    v
Engine command C++
    |
    v
MemoryScanner dan Value
    |
    v
ProcessMemory abstraction
    |                     |
    v                     v
Linux implementation     Windows implementation
```

Linux membaca mapping process melalui procfs. Backend utama memakai `process_vm_readv` dan `process_vm_writev`, sedangkan mode alternatif memakai `pread` dan `pwrite` pada `/proc/<pid>/mem`. Windows memakai Toolhelp, `VirtualQueryEx`, `ReadProcessMemory`, dan `WriteProcessMemory`.

First Scan memeriksa region yang readable, writable, dan private. Next Scan hanya membaca ulang candidate lama, jadi hasilnya semakin sempit tanpa memindai seluruh address space dari awal. Freeze berjalan pada worker terpisah dan menulis ulang nilai setiap 50 ms.

## Prasyarat

Untuk build native:

- CMake 3.20 atau lebih baru
- Compiler C++20
- Arsitektur x64
- Python 3 dengan Tkinter jika ingin memakai GUI

Pada Linux, compiler yang umum dipakai adalah GCC atau Clang. Pada Windows, gunakan Visual Studio 2022 dengan workload Desktop development with C++.

## Build dan test di Linux x64

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Hasil build:

```text
build/memory_editor
build/other_target
build/scanner_tests
build/integration_test
```

Integration test memerlukan izin ptrace. Jika kebijakan Linux menolak attach ke process lain, jalankan target melalui command `launch` agar target menjadi child dari editor.

## Build dan test di Windows x64

Buka Developer Command Prompt for Visual Studio di folder project, lalu jalankan:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Binary utama berada di:

```text
build\Release\memory_editor.exe
```

Windows mungkin meminta konfirmasi antivirus karena program membuka dan mengubah memory process lain. Periksa source dan hasil build sendiri. Jangan menonaktifkan perlindungan sistem secara permanen.

## Menjalankan CLI

Linux:

```bash
./build/memory_editor
```

Windows:

```powershell
.\build\Release\memory_editor.exe
```

Ada dua cara memilih target. Cara pertama adalah menjalankannya dari editor:

```text
memedit> launch ./build/other_target
```

Cara kedua adalah mencari process yang sudah berjalan lalu attach menggunakan PID:

```text
memedit> ps other_target
memedit> attach 12345
```

Ganti `12345` dengan PID yang benar-benar muncul di layar.

### Alur First Scan dan Next Scan

Contoh berikut memakai `other_target`, target aman yang dibangun dari folder `demo`:

```text
memedit> launch ./build/other_target
memedit> type i32
memedit> threads auto
memedit> scan exact 150
memedit> results 20
```

Health target berkurang dari 150 menjadi 145 setelah beberapa detik. Setelah perubahan terjadi:

```text
memedit> next decreased
memedit> results 20
memedit> read #0 i32
memedit> write #0 999 i32
memedit> read #0 i32
```

`#0` berarti candidate nomor 0 dari hasil scan aktif. Nomor tersebut bukan absolute address dan dapat berubah setelah scan baru atau `reset`.

### Unknown Initial Value

Gunakan mode ini ketika nilai awal tidak diketahui:

```text
memedit> reset
memedit> scan unknown
memedit> next decreased
memedit> results 20
```

Jalankan `next increased`, `next decreased`, `next changed`, atau `next unchanged` setelah kondisi target berubah.

### Freeze Value

```text
memedit> freeze #0 999 i32
memedit> freezes
memedit> unfreeze #0
```

Gunakan `unfreeze all` untuk menghentikan seluruh freeze.

### Signature scan

Pola mendukung byte hex dan wildcard `??`:

```text
memedit> scan signature B2 57 ?? 02
memedit> results 20
```

Pola tersebut cocok dengan representasi little-endian dari magic value pada `other_target`.

### Pointer chain

Setelah menemukan target address:

```text
memedit> scan pointer #0 3 4096 20
memedit> pointers 20
memedit> follow 0
```

Pointer scan bekerja mundur dari target address. Chain yang mempunyai anchor module dapat di-resolve ulang ketika base module berpindah setelah target restart.

### Save dan load konfigurasi

```text
memedit> save config local-cheat.cfg
memedit> load config local-cheat.cfg
```

File konfigurasi lokal diabaikan oleh Git. Jangan menyimpan data pribadi atau path sensitif di dalam file yang akan dibagikan.

### Benchmark thread

```text
memedit> type i32
memedit> benchmark scan exact 42424242
```

Command ini menjalankan scan dengan satu worker dan jumlah worker otomatis. Durasi dan speedup dicetak oleh program. Hasil bergantung pada ukuran target, jumlah region, CPU, dan apakah target berjalan melalui emulasi.

Daftar command lengkap tersedia melalui:

```text
memedit> help
```

## Menjalankan GUI

Linux:

```bash
python3 gui/memory_editor_gui.py --editor ./build/memory_editor
```

Windows:

```powershell
py gui\memory_editor_gui.py --editor build\Release\memory_editor.exe
```

GUI adalah front-end untuk engine C++. Panel output tetap menampilkan command dan hasil engine, sehingga proses scan dapat diperiksa seperti pada CLI.

## Menjalankan dari macOS dengan Docker

Docker menjalankan versi Linux x64. Pada Mac Apple Silicon, flag `--platform linux/amd64` memakai emulasi sehingga build dan scan dapat lebih lambat.

Build image:

```bash
docker build --platform linux/amd64 -t pekalongan-editor .
```

Jalankan test unit:

```bash
docker run --platform linux/amd64 --rm \
  --entrypoint ./build/scanner_tests \
  pekalongan-editor
```

Jalankan integration test:

```bash
docker run --platform linux/amd64 --rm \
  --cap-add SYS_PTRACE \
  --security-opt seccomp=unconfined \
  --entrypoint ./build/integration_test \
  pekalongan-editor ./build/other_target
```

Jalankan CLI dengan target demo:

```bash
docker run --platform linux/amd64 --rm -it \
  --cap-add SYS_PTRACE \
  --security-opt seccomp=unconfined \
  pekalongan-editor
```

Di prompt editor:

```text
memedit> launch ./build/other_target
```

### Menampilkan target grafis melalui VNC

Image menyediakan Xvfb, Mesa, dan x11vnc. Server hanya dipublikasikan ke `127.0.0.1`. Script menghasilkan password baru setiap container dijalankan, kecuali kamu mengatur `PEKALONGAN_VNC_PASSWORD` sendiri.

```bash
docker run --platform linux/amd64 --rm -it \
  --cap-add SYS_PTRACE \
  --security-opt seccomp=unconfined \
  -p 127.0.0.1:5900:5900 \
  --entrypoint ./scripts/run-vnc.sh \
  pekalongan-editor
```

Catat password yang tercetak, lalu buka Terminal kedua:

```bash
open vnc://localhost:5900
```

Terminal pertama memang tetap aktif selama server VNC dan editor berjalan. Kondisi itu normal, bukan hang.

### Memakai target praktikum lokal

Binary target praktikum tidak disertakan dalam repository publik. Simpan file yang sudah kamu peroleh secara resmi di folder lokal `Citer Pekalongan`. Folder tersebut diabaikan oleh Git dan Docker build context.

Pastikan binary Linux dapat dieksekusi:

```bash
chmod +x "Citer Pekalongan/point_blank"
```

Mount folder itu ketika container dijalankan:

```bash
docker run --platform linux/amd64 --rm -it \
  --cap-add SYS_PTRACE \
  --security-opt seccomp=unconfined \
  -p 127.0.0.1:5900:5900 \
  -v "$PWD/Citer Pekalongan:/app/Citer Pekalongan:ro" \
  --entrypoint ./scripts/run-vnc.sh \
  pekalongan-editor
```

Setelah VNC tersambung:

```text
memedit> launch "./Citer Pekalongan/point_blank"
```

Editor dan game berada di container yang sama, jadi engine dapat mengakses memory target. Peringatan ALSA hanya menandakan container tidak mempunyai perangkat audio. Peringatan tersebut tidak menghentikan game atau scanner.

Untuk membuka GUI editor melalui VNC, tambahkan environment variable berikut pada command Docker:

```text
-e PEKALONGAN_FRONTEND=gui
```

## Backend alternatif Linux

Jalankan editor dengan environment variable berikut:

```bash
PEKALONGAN_IO=procfs \
PEKALONGAN_REGIONS=smaps \
./build/memory_editor
```

`PEKALONGAN_IO=procfs` memakai `pread` dan `pwrite` pada `/proc/<pid>/mem`. `PEKALONGAN_REGIONS=smaps` membaca header region dari `/proc/<pid>/smaps`. Mode ini tetap tunduk pada ownership, ptrace policy, capability, dan permission procfs.

## Struktur repository

```text
.
|-- CMakeLists.txt
|-- Dockerfile
|-- demo/
|   `-- other_target.cpp
|-- docs/
|   |-- PENJELASAN_WEB.md
|   `-- VIDEO_SCRIPT_MACBOOK.md
|-- gui/
|   `-- memory_editor_gui.py
|-- scripts/
|   `-- run-vnc.sh
|-- src/
|   |-- main.cpp
|   |-- process.hpp
|   |-- process_linux.cpp
|   |-- process_windows.cpp
|   |-- scanner.cpp
|   |-- scanner.hpp
|   |-- value.cpp
|   `-- value.hpp
`-- tests/
    |-- integration_test.cpp
    `-- scanner_tests.cpp
```

## Constraint

- Scanner typed value fokus pada mapping readable, writable, dan private.
- Unknown Initial Value dibatasi sampai 30 juta candidate untuk mencegah penggunaan RAM yang tidak terkendali.
- Candidate typed scan hanya berlaku untuk session aktif. Gunakan signature atau pointer chain untuk menemukan kembali nilai setelah restart.
- Pointer scan dapat menghasilkan banyak chain dan tidak menjamin semua chain stabil di setiap game.
- Akses memory selalu mengikuti permission sistem operasi.

## Video Demo
- Link: https://youtu.be/0idnAcvUVL0
