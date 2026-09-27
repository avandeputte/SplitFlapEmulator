// FFat.h -- the gateway's FATFS partition is a directory under the data volume.
#pragma once
#include "FS.h"
#define FFAT_WIPE_QUICK 0
#define FFAT_WIPE_FULL  1
namespace fs {
class F_Fat : public FS {
public:
  bool   begin(bool formatOnFail = false, const char* basePath = "/ffat", uint8_t maxOpenFiles = 10, const char* label = "ffat");
  bool   format(bool full = false, const char* label = "ffat");
  size_t totalBytes() { return 9 * 1024 * 1024; }
  size_t usedBytes();
  size_t freeBytes() { size_t u = usedBytes(); return u > totalBytes() ? 0 : totalBytes() - u; }
  void   end() {}
};
}
extern fs::F_Fat FFat;
