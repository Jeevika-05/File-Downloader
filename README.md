# 📥 Multithreaded File Downloader in C

A powerful and interactive multithreaded file downloader written in C using POSIX threads. It supports downloading multiple files concurrently with per-file chunking, progress bars, and runtime command controls (pause, resume, cancel).

---

## 🚀 Features

- ⚡ **Multithreaded Chunk Downloads**: Splits each file into chunks and downloads them concurrently.
- 🧵 **POSIX Threading**: Uses `pthread` and semaphores for managing threads and synchronization.
- 📊 **Progress Visualization**: Displays per-file progress in real time.
- ⏸️ **Command Controls**: Interact during download with pause/resume/cancel commands.
- 📂 **Merge Chunks**: Combines downloaded chunks into a single output file.

---

## 🧰 Dependencies

- GCC compiler (Linux recommended)
- POSIX-compliant system
- Basic C standard libraries: `stdio.h`, `stdlib.h`, `pthread.h`, `semaphore.h`, `unistd.h`, `sys/select.h`, etc.
- ✅ OpenSSL (`-lssl -lcrypto`) for secure downloads and SSL-based connections

---

## 🔧 Compilation

To compile the downloader:

```bash
gcc downloader.c -o downloader -lssl -lcrypto -lpthread
```

---

## 🛠️ How It Works

### ✅ User Input

- Number of files
- URLs and output filenames
- Optional download directory

### 📦 Each File

- Gets its size via a HEAD request.
- Is divided into chunks.
- Each chunk is downloaded in a separate thread.

### 🎮 During Download

- A command listener thread monitors `stdin` for input.
- You can pause (`pX`), resume (`rX`), or cancel (`qX`) downloads (`X = file index`).
- Use `qALL` to cancel all downloads.

### 📂 Post-Download

- Chunks are merged into a single file.
- Temporary chunk files are deleted.

---

## 🧾 Command Syntax

| Command | Description |
|--------|-------------|
| `pX`   | Pause download of file X (e.g., `p1`) |
| `rX`   | Resume download of file X (e.g., `r0`) |
| `qX`   | Cancel download of file X (e.g., `q2`) |
| `qALL` | Cancel all downloads |

---

## 🧪 Example Run

```bash
$ ./downloader
Enter the number of files to download: 2
Default download directory is "/home/user/Downloads". Press Enter to accept or type a different directory:

Enter URL for file 1: http://example.com/file1.zip
Enter output filename for file 1: file1.zip

Enter URL for file 2: http://example.com/file2.pdf
Enter output filename for file 2: file2.pdf

Downloading file to /home/user/Downloads/file1.zip
Downloading file to /home/user/Downloads/file2.pdf

File 0 paused
File 0 resumed
Cancelling File 1...
```

---

## 🖼️ Screenshots

### ⏬ Download In Progress
[![Download in Progress](screenshots/Download in progress.png)](screenshots/Download in progress.png)

### ✅ After Download Completion
[![After Download](screenshots/After download.png)](screenshots/After download.png)

### 📁 Downloaded File in Directory
[![Downloaded File](screenshots/Downloaded.png)](screenshots/Downloaded.png)

---

## 🧼 Cleanup & Exit

- Temporary chunk files are deleted after merging.
- All threads cleanly exit after download or cancellation.
