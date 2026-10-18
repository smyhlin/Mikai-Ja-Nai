#if defined(_WIN32)
#  define MIKAI_TEST_EXPORT extern "C" __declspec(dllexport)
#else
#  define MIKAI_TEST_EXPORT extern "C" __attribute__((visibility("default")))
#endif

MIKAI_TEST_EXPORT int aji_api_version() { return 8; }
MIKAI_TEST_EXPORT int aji_rife_heavy_supported() { return 1; }
