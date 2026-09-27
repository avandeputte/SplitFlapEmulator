// fs.cpp -- FATFS (a directory) and NVS Preferences (a JSON file per namespace).
#include "FS.h"
#include "FFat.h"
#include "Preferences.h"
#include "emu.h"
#include <sys/stat.h>
#include <dirent.h>
#include <ArduinoJson.h>

fs::F_Fat FFat;

namespace fs {

String FS::hostPath(const char* p) const {
  String s = _base;
  if (!p || !*p) return s;
  if (p[0] != '/') s += "/";
  s += p;
  return s;
}
File FS::open(const char* path, const char* mode, bool) {
  String hp = hostPath(path);
  struct stat st;
  if (stat(hp.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return File(nullptr, path, true);
  const char* m = (mode && mode[0] == 'w') ? "wb" : (mode && mode[0] == 'a') ? "ab" : "rb";
  FILE* f = fopen(hp.c_str(), m);
  return f ? File(f, path) : File();
}
bool FS::exists(const char* path) { struct stat st; return stat(hostPath(path).c_str(), &st) == 0; }
bool FS::remove(const char* path) { return ::unlink(hostPath(path).c_str()) == 0; }
bool FS::rename(const char* from, const char* to) { return ::rename(hostPath(from).c_str(), hostPath(to).c_str()) == 0; }
bool FS::mkdir(const char* path) { return ::mkdir(hostPath(path).c_str(), 0755) == 0; }
bool FS::rmdir(const char* path) { return ::rmdir(hostPath(path).c_str()) == 0; }

bool F_Fat::begin(bool, const char*, uint8_t, const char*) {
  _base = String(g_emu.dataDir.c_str()) + "/ffat";
  ::mkdir(g_emu.dataDir.c_str(), 0755);
  ::mkdir(_base.c_str(), 0755);
  return true;
}
bool F_Fat::format(bool, const char*) {
  if (!_base.length()) begin(true);
  DIR* d = opendir(_base.c_str());
  if (!d) return true;
  struct dirent* e;
  while ((e = readdir(d)) != nullptr) {
    if (e->d_name[0] == '.') continue;
    ::unlink((_base + "/" + e->d_name).c_str());
  }
  closedir(d);
  printf("[emu] FATFS formatted\n");
  return true;
}
size_t F_Fat::usedBytes() {
  size_t total = 0;
  DIR* d = opendir(_base.c_str());
  if (!d) return 0;
  struct dirent* e;
  while ((e = readdir(d)) != nullptr) {
    if (e->d_name[0] == '.') continue;
    struct stat st;
    if (stat((_base + "/" + e->d_name).c_str(), &st) == 0) total += (size_t)st.st_size;
  }
  closedir(d);
  return total;
}

} // namespace fs

// ---- Preferences -----------------------------------------------------------------------------
static String nvsPath(const String& name) {
  String dir = String(g_emu.dataDir.c_str()) + "/nvs";
  ::mkdir(g_emu.dataDir.c_str(), 0755);
  ::mkdir(dir.c_str(), 0755);
  return dir + "/" + name + ".json";
}
bool Preferences::begin(const char* name, bool readOnly, const char*) {
  _name = name; _ro = readOnly; _open = true; _dirty = false;
  auto* doc = new JsonDocument();
  FILE* f = fopen(nvsPath(_name).c_str(), "rb");
  if (f) {
    String s; int c;
    while ((c = fgetc(f)) != EOF) s += (char)c;
    fclose(f);
    if (deserializeJson(*doc, s) != DeserializationError::Ok) doc->clear();
  }
  _doc = doc;
  return true;
}
void Preferences::save() {
  if (!_doc) return;
  String tmp = nvsPath(_name) + ".tmp";
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) return;
  String s; serializeJsonPretty(*(JsonDocument*)_doc, s);
  fwrite(s.c_str(), 1, s.length(), f);
  fclose(f);
  ::rename(tmp.c_str(), nvsPath(_name).c_str());
  _dirty = false;
}
void Preferences::end() { if (_dirty) save(); delete (JsonDocument*)_doc; _doc = nullptr; _open = false; }
bool Preferences::clear() { if (!_doc || _ro) return false; ((JsonDocument*)_doc)->clear(); _dirty = true; save(); return true; }
bool Preferences::remove(const char* key) { if (!_doc || _ro) return false; ((JsonDocument*)_doc)->remove(key); _dirty = true; save(); return true; }
bool Preferences::isKey(const char* key) { return _doc && !(*(JsonDocument*)_doc)[key].isNull(); }
size_t Preferences::putNum(const char* k, double v) { if (!_doc || _ro) return 0; (*(JsonDocument*)_doc)[k] = v; _dirty = true; save(); return 4; }
double Preferences::getNum(const char* k, double d) { if (!_doc) return d; JsonVariant v = (*(JsonDocument*)_doc)[k]; return v.isNull() ? d : v.as<double>(); }
size_t Preferences::putString(const char* k, const char* v) { if (!_doc || _ro) return 0; (*(JsonDocument*)_doc)[k] = String(v ? v : ""); _dirty = true; save(); return strlen(v ? v : ""); }
String Preferences::getString(const char* k, String d) { if (!_doc) return d; JsonVariant v = (*(JsonDocument*)_doc)[k]; if (v.isNull() || !v.is<const char*>()) return d; return String(v.as<const char*>()); }
size_t Preferences::getString(const char* k, char* out, size_t maxLen) { String s = getString(k, String()); strlcpy(out, s.c_str(), maxLen); return s.length(); }
