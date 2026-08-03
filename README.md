# Pekalongan Memory Editor

Pekalongan Memory Editor adalah pemindai memori untuk proses 64-bit di Linux dan Windows. Program dapat mencari nilai saat target sedang berjalan, mempersempit hasil setelah nilainya berubah, lalu membaca atau menulis alamat yang dipilih.

Proyek ini dibuat untuk mempelajari API proses dan memori virtual. Gunakan hanya pada program milik sendiri, target praktikum yang memang mengizinkannya, atau game offline. Jangan gunakan pada game online atau proses milik orang lain.

## Dukungan platform

| Sistem | Menjalankan editor | Menjalankan target |
|---|---|---|
| Linux x64 | Native | Target Linux x64 |
| Windows x64 | Native | Target Windows x64 |
| macOS Intel atau Apple Silicon | Melalui Docker atau VM | Target Linux x64 di container atau target Windows x64 di VM |

macOS belum didukung secara native. Binary Linux dan Windows tidak dapat memindai proses macOS karena format executable serta API memorinya berbeda. Pada macOS, jalankan editor dan target di dalam container atau VM yang sama.

## Fitur

| Fitur | Status |
|---|---|
| Memilih target dari daftar proses | Tersedia |
| First Scan Exact Value | Tersedia |
| Next Scan Exact, Changed, Unchanged, Increased, dan Decreased | Tersedia |
| Membaca dan menulis memori | Tersedia |
| `int8`, `uint8`, `int16`, `uint16`, `int32`, `uint32`, `int64`, `uint64`, `float`, dan `double` | Tersedia |
| Unknown Initial Value | Tersedia |
| Freeze Value setiap 50 ms | Tersedia |
| GUI Tkinter | Tersedia |
| Single-thread dan multi-thread scan | Tersedia |
| Signature scan dengan wildcard | Tersedia |
| Pointer chain scan dan replay | Tersedia |
| Menyimpan dan memuat konfigurasi | Tersedia |
| Target demo kedua | Tersedia |
| Satu logika scanner untuk Linux x64 dan Windows x64 | Tersedia |
| Backend alternatif `/proc/<pid>/mem` | Linux x64 |
| Enumerasi region melalui `/proc/<pid>/smaps` | Linux x64 |
| Akses di luar mekanisme user mode | Tidak tersedia |

Program tidak mencoba melewati izin sistem operasi. Akses di luar user mode memerlukan driver, kernel module, atau komponen lain dengan hak istimewa dan tidak disertakan dalam proyek ini.

## Arsitektur

CLI dan GUI menggunakan engine C++ yang sama. `MemoryScanner` mengakses proses melalui interface `ProcessMemory`, sehingga logika pemindaian tidak perlu ditulis ulang untuk setiap sistem operasi.

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

Di Linux, program membaca mapping proses melalui procfs. Backend utama menggunakan `process_vm_readv` dan `process_vm_writev`. Backend alternatif menggunakan `pread` dan `pwrite` pada `/proc/<pid>/mem`.

Di Windows, program menggunakan Toolhelp, `VirtualQueryEx`, `ReadProcessMemory`, dan `WriteProcessMemory`.

First Scan memeriksa region yang readable, writable, dan private. Next Scan hanya membaca kembali candidate dari hasil sebelumnya. Cara ini mempersempit hasil tanpa memindai seluruh address space dari awal.

Freeze berjalan pada worker terpisah dan menulis ulang nilai setiap 50 ms.

## Prasyarat

Untuk build native, siapkan:

- CMake 3.20 atau versi yang lebih baru
- compiler C++20
- arsitektur x64
- Python 3 dengan Tkinter untuk menjalankan GUI

Pada Linux, gunakan GCC atau Clang. Pada Windows, gunakan Visual Studio 2022 dengan workload **Desktop development with C++**.

## Build dan test di Linux x64

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Hasil build berada di:

```text
build/memory_editor
build/other_target
build/scanner_tests
build/integration_test
```

Integration test memerlukan izin `ptrace`. Jika kebijakan Linux menolak proses editor melakukan attach ke proses lain, jalankan target melalui command `launch`. Target kemudian menjadi child process dari editor.

## Build dan test di Windows x64

Buka **Developer Command Prompt for Visual Studio** pada folder proyek, lalu jalankan:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Binary utama berada di:

```text
build\Release\memory_editor.exe
```

Windows dapat menampilkan peringatan antivirus karena program membuka dan mengubah memori proses lain. Periksa kode sumber dan hasil build sebelum menjalankannya. Jangan menonaktifkan perlindungan sistem secara permanen.

## Menjalankan CLI

### Linux

```bash
./build/memory_editor
```

### Windows

```powershell
.\build\Release\memory_editor.exe
```

Target dapat dipilih dengan dua cara.

Cara pertama adalah menjalankan target langsung dari editor:

```text
memedit> launch ./build/other_target
```

Cara kedua adalah mencari proses yang sudah berjalan, lalu melakukan attach dengan PID:

```text
memedit> ps other_target
memedit> attach 12345
```

Ganti `12345` dengan PID yang muncul pada hasil pencarian.

### First Scan dan Next Scan

Contoh berikut menggunakan `other_target`, yaitu target aman yang dibangun dari folder `demo`:

```text
memedit> launch ./build/other_target
memedit> type i32
memedit> threads auto
memedit> scan exact 150
memedit> results 20
```

Nilai health pada target berkurang dari 150 menjadi 145 setelah beberapa detik. Setelah nilainya berubah, jalankan:

```text
memedit> next decreased
memedit> results 20
memedit> read #0 i32
memedit> write #0 999 i32
memedit> read #0 i32
```

`#0` adalah candidate nomor 0 dari hasil scan yang sedang aktif. Nomor ini bukan alamat absolut dan dapat berubah setelah scan baru atau command `reset`.

### Unknown Initial Value

Gunakan mode ini jika nilai awal belum diketahui:

```text
memedit> reset
memedit> scan unknown
memedit> next decreased
memedit> results 20
```

Setelah kondisi target berubah, gunakan salah satu command berikut:

```text
memedit> next increased
memedit> next decreased
memedit> next changed
memedit> next unchanged
```

### Freeze Value

```text
memedit> freeze #0 999 i32
memedit> freezes
memedit> unfreeze #0
```

Gunakan `unfreeze all` untuk menghentikan seluruh freeze.

### Signature scan

Pola signature menerima byte heksadesimal dan wildcard `??`:

```text
memedit> scan signature B2 57 ?? 02
memedit> results 20
```

Pola tersebut cocok dengan representasi little-endian dari magic value pada `other_target`.

### Pointer chain

Setelah menemukan alamat target, jalankan:

```text
memedit> scan pointer #0 3 4096 20
memedit> pointers 20
memedit> follow 0
```

Pointer scan mencari chain secara mundur dari alamat target. Chain yang memiliki anchor module dapat di-resolve kembali ketika base module berpindah setelah target di-restart.

### Menyimpan dan memuat konfigurasi

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

Command ini menjalankan scan dua kali. Pengujian pertama menggunakan satu worker, sedangkan pengujian kedua memakai jumlah worker otomatis.

Program menampilkan durasi dan speedup. Hasilnya bergantung pada ukuran target, jumlah region, CPU, dan penggunaan emulasi.

Daftar command lengkap tersedia melalui:

```text
memedit> help
```

## Menjalankan GUI

### Linux

```bash
python3 gui/memory_editor_gui.py --editor ./build/memory_editor
```

### Windows

```powershell
py gui\memory_editor_gui.py --editor build\Release\memory_editor.exe
```

GUI berfungsi sebagai front-end untuk engine C++. Panel output tetap menampilkan command yang dikirim dan hasil dari engine, sehingga proses scan masih dapat diperiksa seperti pada CLI.

## Menjalankan dari macOS dengan Docker

Docker menjalankan versi Linux x64. Pada Mac Apple Silicon, opsi `--platform linux/amd64` menggunakan emulasi. Build dan pemindaian dapat berjalan lebih lambat dibandingkan pada mesin x64 native.

### Build image

```bash
docker build --platform linux/amd64 -t pekalongan-editor .
```

### Menjalankan unit test

```bash
docker run --platform linux/amd64 --rm \
  --entrypoint ./build/scanner_tests \
  pekalongan-editor
```

### Menjalankan integration test

```bash
docker run --platform linux/amd64 --rm \
  --cap-add SYS_PTRACE \
  --security-opt seccomp=unconfined \
  --entrypoint ./build/integration_test \
  pekalongan-editor ./build/other_target
```

### Menjalankan CLI dengan target demo

```bash
docker run --platform linux/amd64 --rm -it \
  --cap-add SYS_PTRACE \
  --security-opt seccomp=unconfined \
  pekalongan-editor
```

Setelah prompt editor muncul, jalankan:

```text
memedit> launch ./build/other_target
```

### Menampilkan target grafis melalui VNC

Image menyediakan Xvfb, Mesa, dan x11vnc. Port VNC hanya dipublikasikan ke `127.0.0.1`.

Script membuat password baru setiap kali container dijalankan. Untuk memakai password sendiri, atur `PEKALONGAN_VNC_PASSWORD`.

```bash
docker run --platform linux/amd64 --rm -it \
  --cap-add SYS_PTRACE \
  --security-opt seccomp=unconfined \
  -p 127.0.0.1:5900:5900 \
  --entrypoint ./scripts/run-vnc.sh \
  pekalongan-editor
```

Catat password yang ditampilkan, lalu buka Terminal kedua:

```bash
open vnc://localhost:5900
```

Terminal pertama harus tetap aktif selama server VNC dan editor berjalan. Kondisi ini normal dan bukan tanda bahwa program hang.

### Memakai target praktikum lokal

Binary target praktikum tidak disertakan dalam repositori publik. Simpan binary yang diperoleh secara resmi di folder lokal `Citer Pekalongan`.

Folder tersebut diabaikan oleh Git dan tidak dimasukkan ke Docker build context.

Pastikan binary Linux dapat dieksekusi:

```bash
chmod +x "Citer Pekalongan/point_blank"
```

Mount folder tersebut saat menjalankan container:

```bash
docker run --platform linux/amd64 --rm -it \
  --cap-add SYS_PTRACE \
  --security-opt seccomp=unconfined \
  -p 127.0.0.1:5900:5900 \
  -v "$PWD/Citer Pekalongan:/app/Citer Pekalongan:ro" \
  --entrypoint ./scripts/run-vnc.sh \
  pekalongan-editor
```

Setelah koneksi VNC terbuka, jalankan:

```text
memedit> launch "./Citer Pekalongan/point_blank"
```

Editor dan target berjalan di dalam container yang sama, sehingga engine dapat mengakses memori target.

Peringatan ALSA hanya menunjukkan bahwa container tidak memiliki perangkat audio. Peringatan tersebut tidak menghentikan target maupun scanner.

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

`PEKALONGAN_IO=procfs` menggunakan `pread` dan `pwrite` pada `/proc/<pid>/mem`.

`PEKALONGAN_REGIONS=smaps` membaca header region dari `/proc/<pid>/smaps`.

Mode ini tetap mengikuti ownership, kebijakan `ptrace`, capability, dan izin procfs yang berlaku pada sistem.

## Struktur repositori

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

## Batasan

- Typed value scan hanya memeriksa mapping yang readable, writable, dan private.
- Unknown Initial Value dibatasi hingga 30 juta candidate agar penggunaan RAM tetap terkendali.
- Candidate dari typed scan hanya berlaku selama session aktif. Gunakan signature atau pointer chain untuk menemukan kembali nilai setelah target di-restart.
- Pointer scan dapat menghasilkan banyak chain. Tidak semua chain akan tetap stabil pada setiap game atau setelah restart.
- Seluruh akses memori tetap mengikuti izin yang diberikan oleh sistem operasi.

## Video demo

[Video demo Pekalongan Memory Editor](https://youtu.be/0idnAcvUVL0)
