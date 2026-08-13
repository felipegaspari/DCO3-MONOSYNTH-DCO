// Shim: calibration storage definitions live in the shared library.
// Sorts first among the sketch .ino files, so these definitions precede the
// autotune impls that call them. Format: _shared/docs/CALIBRATION_STORAGE.md
#include "_shared/FS_impl.h"
