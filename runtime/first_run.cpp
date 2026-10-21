// First start (release builds): the repo and the download hold no game files, so when no disc is found next to the
// exe the player picks their own Shadow the Hedgehog GameCube copy here. Every file is checked against the size +
// SHA-1 of a clean USA disc (GUPE8P rev 0, first_run_manifest.inc - hashes only) and unpacked to <exe dir>\Shadow
// (sys\ + files\ + import.json), which find_disc() in main.cpp reads from then on.
// Same rules and messages as tools/game_import/gameimport.py; plan in docs/release/GAME-IMPORT.md.
// Inputs: .iso / .gcm, an extracted folder (Dolphin "Extract Entire Disc": sys\ + files\, also DATA\sys, GCRebuilder
// &&SystemData + root), and Dolphin's compressed .rvz / .wia / .gcz through DolphinTool when Dolphin is installed.
// Tests: SHADOW_IMPORT_FROM=<disc or folder> imports without asking (with --quiet: no window at all, unless
// SHADOW_FIRSTRUN_WINDOW=1 - then the window shows without taking focus).
#include "first_run.h"
#include <windows.h>
#include <shobjidl.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

namespace fs = std::filesystem;

namespace first_run {
namespace {

struct Entry { const char* path; uint64_t size; const char* sha1; };
const Entry kManifest[] = {
#include "first_run_manifest.inc"
};
constexpr const char* kGameId = "GUPE8P";
constexpr int kRevision = 0;

// --- small helpers --------------------------------------------------------------------------------------------------

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}
std::string utf8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}
std::string lower(std::string s) {
  for (auto& c : s) c = (char)tolower((unsigned char)c);
  return s;
}
uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
fs::path exe_dir() {
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  return fs::path(exe).parent_path();
}
bool read_at(const fs::path& p, uint64_t off, void* buf, size_t n) {
  FILE* f = _wfopen(p.c_str(), L"rb");
  if (!f) return false;
  bool ok = _fseeki64(f, (long long)off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
  fclose(f);
  return ok;
}
bool is_dir(const fs::path& p) { std::error_code ec; return fs::is_directory(p, ec); }
bool is_file(const fs::path& p) { std::error_code ec; return fs::is_regular_file(p, ec); }
fs::path child_nocase(const fs::path& dir, const char* name) {   // dir\name, any letter case
  std::error_code ec;
  for (auto& e : fs::directory_iterator(dir, ec))
    if (lower(utf8(e.path().filename().wstring())) == lower(name)) return e.path();
  return {};
}

struct Sha1 {
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE h = nullptr;
  Sha1() {
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, BCRYPT_HASH_REUSABLE_FLAG) == 0)
      BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, BCRYPT_HASH_REUSABLE_FLAG);
  }
  ~Sha1() {
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
  }
  void add(const void* p, size_t n) { BCryptHashData(h, (PUCHAR)p, (ULONG)n, 0); }
  std::string hex() {   // also resets for the next file (reusable hash)
    UCHAR d[20] = {};
    BCryptFinishHash(h, d, 20, 0);
    char s[41];
    for (int i = 0; i < 20; ++i) snprintf(s + 2 * i, 3, "%02x", d[i]);
    return s;
  }
};

// --- what did the player pick ---------------------------------------------------------------------------------------

struct Info {
  bool ok = false;
  std::string kind;      // iso | folder | gcrebuilder | rvz | wia | gcz | ciso | wbfs | nkit | unknown | missing
  fs::path root;         // the folder itself for folder kinds (a picked sys\boot.bin points at its disc folder)
  std::string message;   // one plain sentence for the player
};

fs::path find_dolphin_tool() {
  std::vector<fs::path> c;
  wchar_t buf[MAX_PATH];
  if (GetEnvironmentVariableW(L"SHADOW_DOLPHINTOOL", buf, MAX_PATH)) c.push_back(buf);
  c.push_back(exe_dir() / L"DolphinTool.exe");
  for (const wchar_t* var : {L"ProgramFiles", L"ProgramFiles(x86)", L"LOCALAPPDATA"}) {
    if (!GetEnvironmentVariableW(var, buf, MAX_PATH)) continue;
    fs::path b = buf;
    if (std::wstring(var) == L"LOCALAPPDATA") b /= L"Programs";
    c.push_back(b / L"Dolphin" / L"DolphinTool.exe");
    c.push_back(b / L"Dolphin-x64" / L"DolphinTool.exe");
  }
  if (SearchPathW(nullptr, L"DolphinTool.exe", nullptr, MAX_PATH, buf, nullptr)) c.push_back(buf);
  for (auto& p : c)
    if (is_file(p)) return p;
  return {};
}

Info from_boot(const std::string& kind, const uint8_t* boot) {
  Info i;
  i.kind = kind;
  if (be32(boot + 0x1C) != 0xC2339F3D) {
    i.kind = "unknown";
    i.message = "This isn't a GameCube disc image.";
    return i;
  }
  std::string gid((const char*)boot, 6);
  for (auto& ch : gid) if (ch < 32 || ch > 126) ch = '?';
  const int rev = boot[7];
  std::string title((const char*)boot + 0x20, strnlen((const char*)boot + 0x20, 0x40));
  if (memcmp(boot + 0x200, "NKIT", 4) == 0) {
    i.kind = "nkit";
    i.message = "This is a trimmed NKit image (files are missing). Restore it to a full ISO with NKit first.";
  } else if (gid.compare(0, 3, "GUP") != 0 || gid.compare(4, 2, "8P") != 0) {
    i.message = "This isn't Shadow the Hedgehog (it's " + (title.empty() ? std::string("another game") : title) + ", " + gid + ").";
  } else if (gid != kGameId) {
    const char* region = gid[3] == 'P' ? "European (PAL)" : gid[3] == 'J' ? "Japanese" : "other";
    i.message = std::string("This is the ") + region + " version (" + gid + "). Only the USA version (GUPE8P) is supported for now.";
  } else if (rev != kRevision) {
    i.message = "This is USA revision " + std::to_string(rev) + ". Only the first release (revision 0) is supported for now.";
  } else {
    i.ok = true;
    i.message = "Shadow the Hedgehog, USA (GUPE8P) - OK.";
  }
  return i;
}

Info identify(fs::path p) {
  Info none;
  std::error_code ec;
  if (!fs::exists(p, ec)) {
    none.kind = "missing";
    none.message = "That file or folder does not exist.";
    return none;
  }
  // a file picked inside an extracted disc (sys\boot.bin, sys\main.dol): use its disc folder
  if (is_file(p) && lower(utf8(p.parent_path().filename().wstring())) == "sys") {
    const std::string n = lower(utf8(p.filename().wstring()));
    if (n == "boot.bin" || n == "main.dol" || n == "bi2.bin" || n == "fst.bin" || n == "apploader.img") p = p.parent_path().parent_path();
  }
  uint8_t boot[0x440] = {};
  if (is_dir(p)) {
    Info i;
    if (read_at(p / L"sys" / L"boot.bin", 0, boot, sizeof boot)) {
      i = from_boot("folder", boot);
      i.root = p;
    } else if (read_at(p / L"DATA" / L"sys" / L"boot.bin", 0, boot, sizeof boot)) {
      i = from_boot("folder", boot);
      i.root = p / L"DATA";
    } else if (fs::path s = child_nocase(p, "&&systemdata"); !s.empty() && read_at(s / L"ISO.hdr", 0, boot, sizeof boot)) {
      i = from_boot("gcrebuilder", boot);
      i.root = p;
    } else {
      none.kind = "folder";
      none.message = "This folder isn't an extracted GameCube disc (there is no sys\\boot.bin in it).";
      return none;
    }
    return i;
  }
  if (!read_at(p, 0, boot, sizeof boot)) {
    none.kind = "unknown";
    none.message = "This file is too small to be a GameCube disc image.";
    return none;
  }
  const char* kind = !memcmp(boot, "RVZ\x01", 4) ? "rvz" : !memcmp(boot, "WIA\x01", 4) ? "wia"
                   : !memcmp(boot, "\x01\xc0\x0b\xb1", 4) ? "gcz" : !memcmp(boot, "CISO", 4) ? "ciso"
                   : !memcmp(boot, "WBFS", 4) ? "wbfs" : nullptr;
  if (kind) {
    Info i;
    i.kind = kind;
    std::string up = kind;
    for (auto& ch : up) ch = (char)toupper((unsigned char)ch);
    if (i.kind == "ciso" || i.kind == "wbfs") {
      i.message = up + " images are not supported. Convert it to ISO in Dolphin (right-click the game > Convert File... > Format: ISO).";
    } else if (find_dolphin_tool().empty()) {
      i.message = "This is a compressed " + up + " image. Install Dolphin, or in Dolphin right-click the game > Convert File... > Format: ISO, then choose the ISO.";
    } else {
      i.ok = true;   // the full check runs on the ISO Dolphin makes
      i.message = "Compressed " + up + " image: it is checked after Dolphin converts it.";
    }
    return i;
  }
  return from_boot("iso", boot);
}

// --- where every disc file is in the picked source ------------------------------------------------------------------

struct Src {
  fs::path file;
  uint64_t off = 0, size = 0;
  bool slice = false;   // a byte range of file (disc images, GCRebuilder ISO.hdr) instead of the whole file
};

uint64_t dol_size(const uint8_t* h) {
  uint64_t end = 0x100;
  for (int i = 0; i < 18; ++i) {
    uint32_t off = be32(h + i * 4), sz = be32(h + 0x90 + i * 4);
    if (sz) end = std::max<uint64_t>(end, (uint64_t)off + sz);
  }
  return end;
}

bool iso_entries(const fs::path& iso, std::map<std::string, Src>& out, std::string& err) {
  uint8_t boot[0x440], al[0x20], dolh[0x100];
  if (!read_at(iso, 0, boot, sizeof boot) || !read_at(iso, 0x2440, al, sizeof al)) { err = "The disc image ends early (it is cut short or damaged)."; return false; }
  const uint32_t dol_off = be32(boot + 0x420), fst_off = be32(boot + 0x424), fst_size = be32(boot + 0x428);
  std::vector<uint8_t> fst(fst_size);
  if (!read_at(iso, dol_off, dolh, sizeof dolh) || fst_size < 12 || !read_at(iso, fst_off, fst.data(), fst_size)) {
    err = "The disc image ends early (it is cut short or damaged).";
    return false;
  }
  auto add = [&](const std::string& rel, uint64_t off, uint64_t size) { out[lower(rel)] = Src{iso, off, size, true}; };
  add("sys/boot.bin", 0, 0x440);
  add("sys/bi2.bin", 0x440, 0x2000);
  add("sys/apploader.img", 0x2440, 0x20ull + be32(al + 0x14) + be32(al + 0x18));
  add("sys/main.dol", dol_off, dol_size(dolh));
  add("sys/fst.bin", fst_off, fst_size);
  const uint32_t count = be32(fst.data() + 8);
  if ((uint64_t)count * 12 > fst_size) { err = "The disc's file table is damaged."; return false; }
  const size_t strings = (size_t)count * 12;
  auto name = [&](uint32_t i) {
    size_t o = strings + (be32(fst.data() + i * 12) & 0xFFFFFF);
    std::string s;
    while (o < fst.size() && fst[o]) s += (char)fst[o++];
    return s;
  };
  std::vector<std::pair<uint32_t, std::string>> stack{{count, ""}};   // open directories: (end index, path prefix)
  for (uint32_t i = 1; i < count; ++i) {
    while (stack.size() > 1 && i >= stack.back().first) stack.pop_back();
    const std::string prefix = stack.back().second;
    const uint8_t* e = fst.data() + i * 12;
    if (e[0]) stack.push_back({be32(e + 8), prefix + name(i) + "/"});
    else add("files/" + prefix + name(i), be32(e + 4), be32(e + 8));
  }
  return true;
}

void folder_entries(const Info& info, std::map<std::string, Src>& out) {
  fs::path files;
  if (info.kind == "gcrebuilder") {
    fs::path s = child_nocase(info.root, "&&systemdata");
    out["sys/main.dol"] = Src{s / L"Start.dol"};
    out["sys/apploader.img"] = Src{s / L"AppLoader.ldr"};
    out["sys/fst.bin"] = Src{s / L"Game.toc"};
    out["sys/boot.bin"] = Src{s / L"ISO.hdr", 0, 0x440, true};
    out["sys/bi2.bin"] = Src{s / L"ISO.hdr", 0x440, 0x2000, true};
    files = info.root / L"root";
  } else {
    for (const char* n : {"boot.bin", "bi2.bin", "apploader.img", "main.dol", "fst.bin"})
      out[std::string("sys/") + n] = Src{info.root / L"sys" / widen(n)};
    files = info.root / L"files";
  }
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(files, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    if (it->is_regular_file(ec)) out[lower("files/" + utf8(fs::relative(it->path(), files, ec).generic_wstring()))] = Src{it->path()};
}

// --- the import -----------------------------------------------------------------------------------------------------

struct Progress {
  std::atomic<uint64_t> done{0}, total{1};
  std::atomic<bool> cancel{false};
  std::mutex m;
  std::string file;   // what is being done now (under m)
  void set(const std::string& s) { std::lock_guard<std::mutex> l(m); file = s; }
  std::string get() { std::lock_guard<std::mutex> l(m); return file; }
};

struct Result { bool ok = false; std::string message; };

std::string json_str(const std::string& s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    if ((unsigned char)c >= 32) o += c;
  }
  return o + "\"";
}

bool run_dolphin_convert(const fs::path& tool, const fs::path& in, const fs::path& out, Progress& pr) {
  std::wstring cmd = L"\"" + tool.wstring() + L"\" convert -i \"" + in.wstring() + L"\" -o \"" + out.wstring() + L"\" -f iso";
  STARTUPINFOW si{sizeof si};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
  while (WaitForSingleObject(pi.hProcess, 200) == WAIT_TIMEOUT)
    if (pr.cancel) TerminateProcess(pi.hProcess, 1);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return code == 0 && !pr.cancel && is_file(out);
}

Result import_disc(const fs::path& src_in, const fs::path& dest, Progress& pr) {
  Result r;
  std::error_code ec;
  const fs::path work = dest.wstring() + L".importing";
  const fs::path conv = dest.wstring() + L".convert.iso";
  fs::path src = src_in;
  auto fail = [&](const std::string& msg) {
    fs::remove_all(work, ec);
    fs::remove(conv, ec);
    r.ok = false;
    r.message = pr.cancel ? "Import stopped." : msg;
    fprintf(stderr, "[first-run] import failed: %s\n", r.message.c_str());
    return r;
  };
  pr.set("Checking what you picked...");
  Info info = identify(src);
  if (!info.ok) return fail(info.message);
  uint64_t total = 0;
  for (auto& e : kManifest) total += e.size;
  pr.total = total;
  pr.done = 0;
  fs::create_directories(dest.parent_path(), ec);
  ULARGE_INTEGER freeb{};
  if (GetDiskFreeSpaceExW(dest.parent_path().c_str(), &freeb, nullptr, nullptr)) {
    const uint64_t need = total + (64ull << 20) + (info.kind == "rvz" || info.kind == "wia" || info.kind == "gcz" ? 1460ull << 20 : 0);
    if (freeb.QuadPart < need) {
      char m[200];
      snprintf(m, sizeof m, "Not enough free space: the game needs %.1f GB, %.1f GB is free on that drive.", need / 1073741824.0, freeb.QuadPart / 1073741824.0);
      return fail(m);
    }
  }
  if (info.kind == "rvz" || info.kind == "wia" || info.kind == "gcz") {
    pr.set("Converting the compressed image with Dolphin (a minute or two)...");
    fs::remove(conv, ec);
    if (!run_dolphin_convert(find_dolphin_tool(), src, conv, pr)) return fail("Dolphin could not convert the image to ISO.");
    src = conv;
    info = identify(src);
    if (!info.ok) return fail(info.message);
  }
  std::map<std::string, Src> have;
  std::string err;
  if (info.kind == "iso") {
    if (!iso_entries(src, have, err)) return fail(err);
  } else {
    folder_entries(info, have);
  }
  fs::remove_all(work, ec);
  if (!fs::create_directories(work, ec) && !is_dir(work))
    return fail("Can't write next to shadow.exe. Move the game folder somewhere you own (for example Documents) and start it again.");
  std::vector<uint8_t> buf(4 << 20);
  Sha1 sha;
  std::vector<std::string> bad, missing;
  int checked = 0;
  for (auto& e : kManifest) {
    if (pr.cancel) return fail("Import stopped.");
    auto it = have.find(lower(e.path));
    if (it == have.end()) { missing.push_back(e.path); pr.done += e.size; continue; }
    const Src& s = it->second;
    pr.set(e.path);
    const fs::path out = work / widen(e.path);
    fs::create_directories(out.parent_path(), ec);
    FILE* in = _wfopen(s.file.c_str(), L"rb");
    FILE* w = _wfopen(out.c_str(), L"wb");
    if (!w) { if (in) fclose(in); return fail("Can't write the game files (is the drive full or read-only?)."); }
    uint64_t n = 0;
    bool short_read = !in;
    if (in) {
      uint64_t left = s.slice ? s.size : UINT64_MAX;
      if (s.slice && _fseeki64(in, (long long)s.off, SEEK_SET) != 0) short_read = true;
      while (!short_read && left > 0 && !pr.cancel) {
        const size_t want = (size_t)std::min<uint64_t>(buf.size(), left);
        const size_t got = fread(buf.data(), 1, want, in);
        if (got == 0) { if (s.slice) short_read = true; break; }
        sha.add(buf.data(), got);
        if (fwrite(buf.data(), 1, got, w) != got) { fclose(in); fclose(w); return fail("Can't write the game files (is the drive full?)."); }
        n += got;
        left -= got;
        pr.done += got;
      }
      fclose(in);
    }
    fclose(w);
    if (short_read && info.kind == "iso") return fail("The disc image ends early (it is cut short or damaged).");
    if (n < e.size) pr.done += e.size - n;
    ++checked;
    if (n != e.size || sha.hex() != e.sha1) bad.push_back(e.path);
  }
  if (!bad.empty() || !missing.empty()) {
    std::string m = "This copy is damaged or modified: ";
    if (!bad.empty()) m += std::to_string(bad.size()) + " file(s) do not match a clean disc (first: " + bad[0] + ")";
    if (!missing.empty()) m += std::string(bad.empty() ? "" : "; ") + std::to_string(missing.size()) + " file(s) are missing (first: " + missing[0] + ")";
    return fail(m + ". Make a fresh dump of your disc and try again.");
  }
  if (FILE* j = _wfopen((work / L"import.json").c_str(), L"w")) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(j, "{\n \"game_id\": \"%s\",\n \"revision\": %d,\n \"source\": %s,\n \"format\": \"%s\",\n \"files\": %d,\n \"bytes\": %llu,\n"
               " \"imported\": \"%04d-%02d-%02d %02d:%02d:%02d\",\n \"tool\": \"shadow.exe first run (runtime/first_run.cpp)\"\n}\n",
            kGameId, kRevision, json_str(src == conv ? std::string("converted image") : utf8(fs::absolute(src_in, ec).wstring())).c_str(),
            info.kind.c_str(), checked, (unsigned long long)total, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    fclose(j);
  }
  fs::remove(conv, ec);
  if (fs::exists(dest, ec)) {
    const fs::path old = dest.wstring() + L".old";
    fs::remove_all(old, ec);
    fs::rename(dest, old, ec);
    fs::rename(work, dest, ec);
    fs::remove_all(old, ec);
  } else {
    fs::rename(work, dest, ec);
  }
  if (ec) return fail("Could not finish unpacking: " + ec.message());
  pr.done = total;
  r.ok = true;
  r.message = "Shadow the Hedgehog (USA) checked: all " + std::to_string(checked) + " files match.";
  fprintf(stderr, "[first-run] %s Unpacked to %s\n", r.message.c_str(), utf8(dest.wstring()).c_str());
  return r;
}

// --- the window -----------------------------------------------------------------------------------------------------

enum : UINT { WM_IMPORT_DONE = WM_APP + 1 };
enum { ID_IMAGE = 101, ID_FOLDER, ID_QUIT };

struct Ui {
  HWND wnd = nullptr, heading = nullptr, body = nullptr, status = nullptr, b_image = nullptr, b_folder = nullptr, b_quit = nullptr;
  HFONT f_body = nullptr, f_head = nullptr;
  HBRUSH bg = nullptr;
  int dpi = 96;
  bool importing = false, error = false, ok = false;
  fs::path dest, auto_src;
  Progress pr;
  Result result;
  std::thread worker;
  int px(int v) const { return MulDiv(v, dpi, 96); }
  RECT bar() const { return RECT{px(24), px(146), px(576), px(164)}; }
};
Ui* g_ui = nullptr;

void set_status(Ui& u, const std::string& s, bool error) {
  u.error = error;
  SetWindowTextW(u.status, widen(s).c_str());
  InvalidateRect(u.status, nullptr, TRUE);
}

void start_import(Ui& u, const fs::path& src) {
  u.importing = true;
  u.pr.cancel = false;
  u.pr.done = 0;
  EnableWindow(u.b_image, FALSE);
  EnableWindow(u.b_folder, FALSE);
  SetWindowTextW(u.b_quit, L"Stop");
  set_status(u, "Checking...", false);
  SetTimer(u.wnd, 1, 100, nullptr);
  if (u.worker.joinable()) u.worker.join();
  u.worker = std::thread([&u, src] {
    u.result = import_disc(src, u.dest, u.pr);
    PostMessageW(u.wnd, WM_IMPORT_DONE, 0, 0);
  });
}

fs::path pick(HWND owner, bool folder) {
  fs::path out;
  IFileOpenDialog* d = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return out;
  DWORD opts = 0;
  d->GetOptions(&opts);
  d->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
  if (folder) {
    d->SetTitle(L"Choose the extracted Shadow the Hedgehog disc folder (the one with sys and files in it)");
  } else {
    d->SetTitle(L"Choose your Shadow the Hedgehog disc image");
    const COMDLG_FILTERSPEC types[] = {{L"GameCube disc image (*.iso, *.gcm, *.rvz, *.wia, *.gcz)", L"*.iso;*.gcm;*.rvz;*.wia;*.gcz"},
                                       {L"All files", L"*.*"}};
    d->SetFileTypes(2, types);
  }
  if (SUCCEEDED(d->Show(owner))) {
    IShellItem* it = nullptr;
    if (SUCCEEDED(d->GetResult(&it))) {
      PWSTR p = nullptr;
      if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out = p; CoTaskMemFree(p); }
      it->Release();
    }
  }
  d->Release();
  return out;
}

HWND make(Ui& u, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT f) {
  HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, u.px(x), u.px(y), u.px(w), u.px(h), u.wnd,
                           (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
  SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
  return c;
}

LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  Ui* up = g_ui;
  if (!up) return DefWindowProcW(h, msg, wp, lp);
  Ui& u = *up;
  switch (msg) {
    case WM_CREATE: {
      u.wnd = h;
      u.heading = make(u, L"STATIC", L"Choose your Shadow the Hedgehog disc", SS_LEFT, 24, 18, 552, 30, 0, u.f_head);
      u.body = make(u, L"STATIC",
                    L"ShadowRecomp doesn't include the game. Choose your own copy of Shadow the Hedgehog for GameCube "
                    L"(USA, GUPE8P): a disc image (.iso, .gcm, .rvz) or a disc folder extracted with Dolphin. It is checked "
                    L"and unpacked once (about 1.4 GB) into the Shadow folder next to shadow.exe.",
                    SS_LEFT, 24, 56, 552, 80, 0, u.f_body);
      u.status = make(u, L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 24, 172, 552, 40, 0, u.f_body);
      u.b_image = make(u, L"BUTTON", L"Choose disc image...", BS_DEFPUSHBUTTON | WS_TABSTOP, 24, 222, 190, 32, ID_IMAGE, u.f_body);
      u.b_folder = make(u, L"BUTTON", L"Choose extracted folder...", BS_PUSHBUTTON | WS_TABSTOP, 222, 222, 190, 32, ID_FOLDER, u.f_body);
      u.b_quit = make(u, L"BUTTON", L"Quit", BS_PUSHBUTTON | WS_TABSTOP, 476, 222, 100, 32, ID_QUIT, u.f_body);
      if (!u.auto_src.empty()) PostMessageW(h, WM_COMMAND, 0xFFFF, 0);
      return 0;
    }
    case WM_COMMAND: {
      const int id = LOWORD(wp);
      if (id == 0xFFFF) { start_import(u, u.auto_src); return 0; }
      if (id == ID_QUIT) { SendMessageW(h, WM_CLOSE, 0, 0); return 0; }
      if ((id == ID_IMAGE || id == ID_FOLDER) && !u.importing) {
        fs::path p = pick(h, id == ID_FOLDER);
        if (p.empty()) return 0;
        Info i = identify(p);
        if (!i.ok) { set_status(u, i.message, true); return 0; }
        start_import(u, p);
      }
      return 0;
    }
    case WM_TIMER: {
      const uint64_t t = std::max<uint64_t>(u.pr.total, 1), d = std::min<uint64_t>(u.pr.done, t);
      char s[400];
      snprintf(s, sizeof s, "%.0f%%   %s", 100.0 * d / t, u.pr.get().c_str());
      set_status(u, s, false);
      RECT r = u.bar();
      InvalidateRect(h, &r, FALSE);
      return 0;
    }
    case WM_IMPORT_DONE: {
      KillTimer(h, 1);
      if (u.worker.joinable()) u.worker.join();
      u.importing = false;
      RECT r = u.bar();
      InvalidateRect(h, &r, TRUE);
      if (u.result.ok) {
        u.ok = true;
        DestroyWindow(h);
        return 0;
      }
      EnableWindow(u.b_image, TRUE);
      EnableWindow(u.b_folder, TRUE);
      SetWindowTextW(u.b_quit, L"Quit");
      set_status(u, u.result.message, true);
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      if (u.importing) {
        RECT r = u.bar();
        HBRUSH frame = CreateSolidBrush(RGB(200, 200, 200)), fill = CreateSolidBrush(RGB(178, 34, 34));
        FillRect(dc, &r, frame);
        const uint64_t t = std::max<uint64_t>(u.pr.total, 1), d = std::min<uint64_t>(u.pr.done, t);
        RECT f = r;
        f.right = r.left + (LONG)((r.right - r.left) * (double)d / t);
        FillRect(dc, &f, fill);
        DeleteObject(frame);
        DeleteObject(fill);
      }
      EndPaint(h, &ps);
      return 0;
    }
    case WM_CTLCOLORSTATIC: {
      HDC dc = (HDC)wp;
      SetBkColor(dc, GetSysColor(COLOR_WINDOW));
      SetTextColor(dc, (HWND)lp == u.status && u.error ? RGB(180, 0, 0) : GetSysColor(COLOR_WINDOWTEXT));
      return (LRESULT)u.bg;
    }
    case WM_CLOSE:
      if (u.importing) {
        if (MessageBoxW(h, L"Stop unpacking the game? You can start again next time.", L"Shadow the Hedgehog", MB_YESNO | MB_ICONQUESTION) == IDYES)
          u.pr.cancel = true;   // the worker stops, cleans up and posts WM_IMPORT_DONE
        return 0;
      }
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

// Common controls 6 (modern buttons) for this window only: a temporary activation context, so the game itself is
// unchanged (its manifest has no comctl32 dependency).
HANDLE comctl6_context() {
  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  const std::wstring path = std::wstring(tmp) + L"ShadowRecomp-firstrun.manifest";
  if (FILE* f = _wfopen(path.c_str(), L"w")) {
    fputs("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
          "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\"><dependency><dependentAssembly>"
          "<assemblyIdentity type=\"win32\" name=\"Microsoft.Windows.Common-Controls\" version=\"6.0.0.0\" "
          "processorArchitecture=\"*\" publicKeyToken=\"6595b64144ccf1df\" language=\"*\"/>"
          "</dependentAssembly></dependency></assembly>\n", f);
    fclose(f);
  }
  ACTCTXW ac{sizeof ac};
  ac.lpSource = path.c_str();
  HANDLE ctx = CreateActCtxW(&ac);
  DeleteFileW(path.c_str());
  return ctx;
}

std::string ui_main(const fs::path& dest, const fs::path& auto_src, const std::string& dest_narrow, bool quiet) {
  // own thread: COM (file dialog) and the DPI mode stay out of the game's main thread
  // (system-DPI aware: sharp text at the main monitor's scale; Windows rescales it on other monitors)
  using SetCtx = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
  auto set_ctx = (SetCtx)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext");
  if (set_ctx) set_ctx(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  HANDLE ctx = comctl6_context();
  ULONG_PTR cookie = 0;
  if (ctx != INVALID_HANDLE_VALUE) ActivateActCtx(ctx, &cookie);
  INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&icc);

  Ui u;
  u.dest = dest;
  u.auto_src = auto_src;
  g_ui = &u;
  HDC sdc = GetDC(nullptr);
  u.dpi = GetDeviceCaps(sdc, LOGPIXELSY);
  ReleaseDC(nullptr, sdc);
  WNDCLASSEXW wc{sizeof wc};
  wc.lpfnWndProc = wndproc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));   // IDC_ARROW (its macro is the A form without UNICODE)
  wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
  u.bg = GetSysColorBrush(COLOR_WINDOW);
  wc.hbrBackground = u.bg;
  wc.lpszClassName = L"ShadowRecompFirstRun";
  RegisterClassExW(&wc);
  RECT wr;
  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  NONCLIENTMETRICSW ncm{sizeof ncm};
  SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
  auto make_fonts = [&] {
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = -MulDiv(10, u.dpi, 72);
    u.f_body = CreateFontIndirectW(&lf);
    lf.lfHeight = -MulDiv(15, u.dpi, 72);
    lf.lfWeight = FW_SEMIBOLD;
    u.f_head = CreateFontIndirectW(&lf);
  };
  make_fonts();
  wr = RECT{u.px(0), 0, u.px(600), u.px(270)};
  AdjustWindowRectEx(&wr, style, FALSE, 0);
  const int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
  RECT work{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  HWND h = CreateWindowExW(0, wc.lpszClassName, L"Shadow the Hedgehog - first start", style, work.left + (work.right - work.left - ww) / 2,
                      work.top + (work.bottom - work.top - wh) / 2, ww, wh, nullptr, nullptr, wc.hInstance, nullptr);
  std::string out;
  if (h) {
    ShowWindow(h, quiet ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL);
    if (!quiet) SetForegroundWindow(h);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
      if (IsDialogMessageW(h, &m)) continue;
      TranslateMessage(&m);
      DispatchMessageW(&m);
    }
    if (u.worker.joinable()) { u.pr.cancel = true; u.worker.join(); }
    if (u.ok) out = dest_narrow;
  }
  g_ui = nullptr;
  if (u.f_body) DeleteObject(u.f_body);
  if (u.f_head) DeleteObject(u.f_head);
  UnregisterClassW(wc.lpszClassName, wc.hInstance);
  if (cookie) DeactivateActCtx(0, cookie);
  if (ctx != INVALID_HANDLE_VALUE) ReleaseActCtx(ctx);
  CoUninitialize();
  return out;
}

}  // namespace

bool disc_present(const std::string& root) {
  return GetFileAttributesA((root + "\\sys\\main.dol").c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::string run(bool quiet) {
  char exe[MAX_PATH];
  GetModuleFileNameA(nullptr, exe, MAX_PATH);
  std::string dir = exe;
  dir = dir.substr(0, dir.find_last_of("\\/"));
  const std::string dest_narrow = dir + "\\Shadow";
  const fs::path dest = exe_dir() / L"Shadow";
  wchar_t from[MAX_PATH] = {};
  const bool have_from = GetEnvironmentVariableW(L"SHADOW_IMPORT_FROM", from, MAX_PATH) > 0;
  fprintf(stderr, "[first-run] no game disc found next to the exe: %s\n", quiet ? (have_from ? "importing SHADOW_IMPORT_FROM" : "--quiet, not asking") : "asking for it");
  const bool window = !quiet || GetEnvironmentVariableW(L"SHADOW_FIRSTRUN_WINDOW", nullptr, 0) > 0;
  if (!window) {
    if (!have_from) return "";
    Progress pr;
    Result r = import_disc(from, dest, pr);
    return r.ok ? dest_narrow : "";
  }
  std::string out;
  std::thread t([&] { out = ui_main(dest, have_from ? fs::path(from) : fs::path(), dest_narrow, quiet); });
  t.join();
  return out;
}

}  // namespace first_run
