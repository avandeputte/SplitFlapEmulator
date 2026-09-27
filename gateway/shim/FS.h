// FS.h -- the fs::FS / fs::File API over a directory on the host (used by FFat and WebServer).
#pragma once
#include "Arduino.h"
#include <memory>
#include <stdio.h>

#define FILE_READ   "r"
#define FILE_WRITE  "w"
#define FILE_APPEND "a"

namespace fs {

class File : public Stream {
public:
  File() {}
  File(FILE* f, const String& name, bool isDir = false) : _f(f ? std::shared_ptr<FILE>(f, [](FILE* p) { if (p) fclose(p); }) : nullptr), _name(name), _isDir(isDir) {}
  size_t write(uint8_t b) override { return _f ? fwrite(&b, 1, 1, _f.get()) : 0; }
  size_t write(const uint8_t* b, size_t n) override { return _f ? fwrite(b, 1, n, _f.get()) : 0; }
  int available() override { if (!_f) return 0; long p = ftell(_f.get()); return (int)((long)size() - p); }
  int read() override { if (!_f) return -1; int c = fgetc(_f.get()); return c == EOF ? -1 : c; }
  int peek() override { if (!_f) return -1; int c = fgetc(_f.get()); if (c != EOF) ungetc(c, _f.get()); return c == EOF ? -1 : c; }
  void flush() override { if (_f) fflush(_f.get()); }
  size_t read(uint8_t* buf, size_t n) { return _f ? fread(buf, 1, n, _f.get()) : 0; }
  size_t readBytes(char* buf, size_t n) override { return read((uint8_t*)buf, n); }
  bool seek(uint32_t pos) { return _f && fseek(_f.get(), (long)pos, SEEK_SET) == 0; }
  size_t position() const { return _f ? (size_t)ftell(_f.get()) : 0; }
  size_t size() const { if (!_f) return 0; long p = ftell(_f.get()); fseek(_f.get(), 0, SEEK_END); long e = ftell(_f.get()); fseek(_f.get(), p, SEEK_SET); return (size_t)e; }
  void close() { _f.reset(); }
  const char* name() const { return _name.c_str(); }
  const char* path() const { return _name.c_str(); }
  bool isDirectory() const { return _isDir; }
  operator bool() const { return (bool)_f || _isDir; }
private:
  std::shared_ptr<FILE> _f;
  String _name;
  bool _isDir = false;
};

class FS {
public:
  explicit FS(const char* base = "") : _base(base) {}
  void setBase(const String& b) { _base = b; }
  File open(const char* path, const char* mode = FILE_READ, bool create = false);
  File open(const String& path, const char* mode = FILE_READ, bool create = false) { return open(path.c_str(), mode, create); }
  bool exists(const char* path);
  bool exists(const String& path) { return exists(path.c_str()); }
  bool remove(const char* path);
  bool remove(const String& path) { return remove(path.c_str()); }
  bool rename(const char* from, const char* to);
  bool rename(const String& f, const String& t) { return rename(f.c_str(), t.c_str()); }
  bool mkdir(const char* path);
  bool rmdir(const char* path);
  String hostPath(const char* p) const;
protected:
  String _base;
};

} // namespace fs

using fs::File;
using fs::FS;
