# feeder_fuzzy

Repository ini berisi sistem lengkap automated fish feeder untuk riset:

- firmware `Arduino UNO` berbasis PlatformIO
- firmware `ESP32` sebagai UART bridge dan Blynk node
- tooling `Python` untuk otomatisasi pengujian, logging, dan analisis
- notebook untuk analisis eksperimen dan pembahasan fuzzy Mamdani

Struktur utama:

```text
feeder-platformio/   -> firmware UNO + ESP32
feeder-research/     -> runner Python, logger, notebook, README operasional
example/             -> referensi wiring lama, bukan basis logika aktif
paper/               -> referensi paper internal
```

## Clone dan Setup

### 1. Clone repository

```powershell
git clone https://github.com/Frey210/feeder_fuzzy.git
cd feeder_fuzzy
```

### 2. Setup environment Python

```powershell
powershell -ExecutionPolicy Bypass -File .\setup_python_env.ps1
```

Script ini akan:

- membuat `feeder-research/.venv`
- meng-install dependency dari `feeder-research/requirements.txt`

### 3. Setup notebook di VS Code

Pilih interpreter:

```text
<repo>\feeder-research\.venv\Scripts\python.exe
```

Lalu pilih kernel notebook yang sama.

### 4. Build firmware

Lihat panduan operasional lengkap di:

- [feeder-research/README.md](./feeder-research/README.md)

## Yang Sengaja Tidak Di-commit

Repository ini sengaja tidak menyimpan:

- virtual environment
- build output PlatformIO
- cache PlatformIO lokal
- hasil data eksperimen mentah
- `secrets.h` yang bersifat lokal

Untuk Blynk, salin:

```text
feeder-platformio/include/secrets.example.h
```

menjadi:

```text
feeder-platformio/include/secrets.h
```

lalu isi kredensial yang diperlukan.

## Build dan Run Ringkas

### Python

```powershell
cd feeder-research
.\.venv\Scripts\Activate.ps1
python .\python\runner\test_runner.py --port COM4 --test C --pause-s 20
```

### PlatformIO UNO

```powershell
$env:PLATFORMIO_CORE_DIR="$PWD\\feeder-platformio\\.platformio-home"
& 'C:\Users\ASUS TUF\.platformio\penv\Scripts\pio.exe' run -d '.\feeder-platformio' -e uno
```

### PlatformIO ESP32

```powershell
$env:PLATFORMIO_CORE_DIR="$PWD\\feeder-platformio\\.platformio-home"
& 'C:\Users\ASUS TUF\.platformio\penv\Scripts\pio.exe' run -d '.\feeder-platformio' -e esp32dev
```

## Dokumen Utama

- panduan operasional: [feeder-research/README.md](./feeder-research/README.md)
- notebook analisis: [feeder-research/notebooks/analysis.ipynb](./feeder-research/notebooks/analysis.ipynb)
- notebook fuzzy paper-ready: [feeder-research/notebooks/fuzzy_feeder_paper_ready.ipynb](./feeder-research/notebooks/fuzzy_feeder_paper_ready.ipynb)
