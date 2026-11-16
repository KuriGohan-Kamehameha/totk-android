// SPDX-License-Identifier: GPL-3.0-or-later
// Desktop test of the companion host, with no game and no device:
//
//   duo_host_test <package_root> <romfs_dir> <out.png> [image.bin] [ticks]
//
// The port's services are faked: "main" is a private mapping (image.bin from test/make_fake_image.py when given, zeroes
// otherwise) read through the same pipe trick as the port's host_safe_read, so an unmapped address is refused and
// not a crash; the RomFS is a folder. The host is started exactly as libtotk starts it, a frame is posted every 16 ms,
// and the page the runtime publishes is written as a PNG. Exit 0 needs a published frame with non-background pixels.
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <stb_image_write.h>

#include "duo_host.h"

namespace {
std::string g_romfs;
uint64_t g_frames;

int SafeRead(uint64_t a, void* out, size_t n) {
    static thread_local int fds[2] = {-1, -1};
    if (fds[0] < 0 && pipe(fds) != 0) {
        return 0;
    }
    auto* dst = static_cast<uint8_t*>(out);
    while (n != 0) {
        const size_t chunk = n > 4096 ? 4096 : n;
        const ssize_t w = write(fds[1], reinterpret_cast<const void*>(static_cast<uintptr_t>(a)), chunk);
        if (w <= 0 || read(fds[0], dst, static_cast<size_t>(w)) != w) {
            return 0;
        }
        a += static_cast<uint64_t>(w);
        dst += w;
        n -= static_cast<size_t>(w);
    }
    return 1;
}

int64_t RomfsRead(const char* path, uint64_t offset, void* out, uint64_t n) {
    if (g_romfs.empty()) {
        return -1;
    }
    const std::string full = g_romfs + "/" + path;
    struct stat st;
    if (stat(full.c_str(), &st) != 0) {
        return -1;
    }
    if (*path == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    if (!S_ISREG(st.st_mode)) {
        return -1;
    }
    if (out == nullptr) {
        return st.st_size;
    }
    const int fd = open(full.c_str(), O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    const ssize_t got = pread(fd, out, n, static_cast<off_t>(offset));
    close(fd);
    return got;
}

int64_t NoAoc(const char*, uint64_t, void*, uint64_t) {
    return -1;
}

void Log(int level, const char* line) {
    std::fprintf(stderr, "[duo:%d] %s\n", level, line);
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <package_root> <romfs_dir> <out.png> [image.bin] [ticks]\n", argv[0]);
        return 2;
    }
    g_romfs = argv[2];
    const int ticks = argc > 5 ? std::atoi(argv[5]) : 240;
    // "main": the original image is 0x2D66000 bytes; the port appends one 0x4000 page.
    const size_t main_size = 0x2D66000 + 0x4000;
    auto* const main_image = static_cast<uint8_t*>(
        mmap(nullptr, main_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    if (main_image == MAP_FAILED) {
        return 2;
    }
    if (argc > 4 && *argv[4]) {
        if (std::FILE* const f = std::fopen(argv[4], "rb")) {
            const size_t got = std::fread(main_image, 1, main_size, f);
            std::fclose(f);
            std::fprintf(stderr, "test: image %s, %zu bytes\n", argv[4], got);
        }
    }
    const std::string data_dir = std::string{argv[3]} + ".data";
    DuoPort port{};
    port.abi = DUO_PORT_ABI;
    port.size = sizeof port;
    port.main_base = reinterpret_cast<uintptr_t>(main_image);
    port.main_size = main_size;
    port.title_id = 0x01007EF00011E000ull;
    static const uint8_t build[16] = {0x8E, 0x99, 0x78, 0xD5, 0x0B, 0xDD, 0x20, 0xB4,
                                      0xC8, 0x39, 0x5A, 0x10, 0x6C, 0x27, 0xFF, 0xDE};
    std::memcpy(port.build_id, build, sizeof build);
    port.safe_read = SafeRead;
    port.romfs_read = RomfsRead;
    port.aoc_read = NoAoc;
    port.log = Log;
    port.package_root = argv[1];
    port.data_dir = data_dir.c_str();
    port.frames = [] { return g_frames; };

    // An unmapped address must be refused, not crash: the property the whole reader rests on.
    uint64_t probe = 0;
    if (SafeRead(0x10, &probe, 8) || !SafeRead(port.main_base, &probe, 8)) {
        std::fprintf(stderr, "test: the fault-free reader does not behave\n");
        return 1;
    }
    if (!duo_host_start(&port)) {
        return 1;
    }
    Duo::SetSurfaceSize(1240, 1080, true);
    std::vector<uint32_t> pixels, latest;
    uint32_t w = 0, h = 0;
    uint64_t serial = 0, frames_taken = 0;
    for (int i = 0; i < ticks; ++i) {
        ++g_frames;
        duo_host_frame();
        std::this_thread::sleep_for(std::chrono::milliseconds{16});
        if (Duo::TakeFrame(pixels, w, h, serial)) {
            latest = pixels;
            ++frames_taken;
        }
    }
    const uint64_t ran = Duo::Ticks();
    Duo::Stop();
    std::fprintf(stderr, "test: %llu ticks, %llu page frames taken, last %ux%u\n",
                 static_cast<unsigned long long>(ran), static_cast<unsigned long long>(frames_taken), w, h);
    if (latest.empty() || w == 0 || h == 0) {
        std::fprintf(stderr, "test: FAIL, no page was published\n");
        return 1;
    }
    // Through the presenter's own copy (Duo::BlitFrame), so the PNG is byte for byte what the device's surface gets:
    // once at the page's size and once into a surface of another shape (the letterboxed, scaled path).
    size_t differing = 0, distinct_from_first = 0;
    for (const uint32_t p : latest) {
        differing += (p & 0x00FFFFFFu) != 0;
        distinct_from_first += p != latest[0];
    }
    std::vector<uint32_t> surface(static_cast<size_t>(w) * h);
    Duo::BlitFrame(surface.data(), w, h, w, latest, w, h);
    stbi_write_png(argv[3], static_cast<int>(w), static_cast<int>(h), 4, surface.data(), static_cast<int>(w) * 4);
    const uint32_t ow = 1280, oh = 720, stride = 1296;
    std::vector<uint32_t> other(static_cast<size_t>(stride) * oh);
    Duo::BlitFrame(other.data(), ow, oh, stride, latest, w, h);
    const std::string scaled = std::string{argv[3]} + ".1280x720.png";
    stbi_write_png(scaled.c_str(), static_cast<int>(ow), static_cast<int>(oh), 4, other.data(), static_cast<int>(stride) * 4);
    std::fprintf(stderr, "test: wrote %s and %s; %zu of %zu pixels are not black, %zu differ from the corner pixel\n",
                 argv[3], scaled.c_str(), differing, latest.size(), distinct_from_first);
    if (differing == 0 || distinct_from_first == 0) {
        std::fprintf(stderr, "test: FAIL, the page is blank\n");
        return 1;
    }
    std::fprintf(stderr, "test: PASS\n");
    return 0;
}
