// Compiler helpers imported from Apple's libgcc_s, with ARM soft-float ABI.
#include "hle/common.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cctype>

namespace {
float addsf(float a, float b) { return a + b; }
float subsf(float a, float b) { return a - b; }
float mulsf(float a, float b) { return a * b; }
float divsf(float a, float b) { return a / b; }
double adddf(double a, double b) { return a + b; }
double muldf(double a, double b) { return a * b; }
double extend(float a) { return a; }
float truncdf(double a) { return a; }
int fixdf(double a) { return (int)a; }
int fixsf(float a) { return (int)a; }
unsigned fixunsdf(double a) { return (unsigned)a; }
unsigned fixunssf(float a) { return (unsigned)a; }
int64_t fixdfdi(double a) { return (int64_t)a; }
int64_t fixsfdi(float a) { return (int64_t)a; }
double floatsidf(int a) { return a; }
float floatsisf(int a) { return a; }
double floatunsidf(unsigned a) { return a; }
double floatdidf(int64_t a) { return a; }
float floatundisf(uint64_t a) { return a; }
int divsi(int a, int b) { return a / b; }
int modsi(int a, int b) { return a % b; }
unsigned udivsi(unsigned a, unsigned b) { return a / b; }
unsigned umodsi(unsigned a, unsigned b) { return a % b; }
int64_t divdi(int64_t a, int64_t b) { return a / b; }
// Darwin's *vfp comparison helpers return booleans, unlike generic libgcc.
int eqsf(float a, float b) { return a == b; }
int nesf(float a, float b) { return a != b; }
int gesf(float a, float b) { return a >= b; }
int gtsf(float a, float b) { return a > b; }
int lesf(float a, float b) { return a <= b; }
int ltsf(float a, float b) { return a < b; }
int eqdf(double a, double b) { return a == b; }
int gedf(double a, double b) { return a >= b; }
int gtdf(double a, double b) { return a > b; }
int ledf(double a, double b) { return a <= b; }
int ltdf(double a, double b) { return a < b; }
int unordsf(float a, float b) { return std::isnan(a) || std::isnan(b); }
int unorddf(double a, double b) { return std::isnan(a) || std::isnan(b); }
void sjlj_register(void*) {}  // No thrown game exception supported yet.
void sjlj_unregister(void*) {}
[[noreturn]] void sjlj_resume(void*) { LOGE("unsupported SJLJ exception resume"); abort(); }
void ios_init(void*) {}
void* ostream_string(void* stream, const char* str) { LOGI("game cout: %s", str ? str : ""); return stream; }
void* ostream_int(void* stream, int value) { LOGI("game cout: %d", value); return stream; }
void* ostream_uint(void* stream, unsigned value) { LOGI("game cout: %u", value); return stream; }
char cout_storage[256]{};
int tolower32(int value) { return std::tolower(value); }
}

HLE_EXPORT(__addsf3vfp, addsf);
HLE_EXPORT(__subsf3vfp, subsf);
HLE_EXPORT(__mulsf3vfp, mulsf);
HLE_EXPORT(__divsf3vfp, divsf);
HLE_EXPORT(__adddf3vfp, adddf);
HLE_EXPORT(__muldf3vfp, muldf);
HLE_EXPORT(__extendsfdf2vfp, extend);
HLE_EXPORT(__truncdfsf2vfp, truncdf);
HLE_EXPORT(__fixdfsivfp, fixdf);
HLE_EXPORT(__fixsfsivfp, fixsf);
HLE_EXPORT(__fixunsdfsivfp, fixunsdf);
HLE_EXPORT(__fixunssfsivfp, fixunssf);
HLE_EXPORT(__fixdfdi, fixdfdi);
HLE_EXPORT(__fixsfdi, fixsfdi);
HLE_EXPORT(__floatsidfvfp, floatsidf);
HLE_EXPORT(__floatsisfvfp, floatsisf);
HLE_EXPORT(__floatunssidfvfp, floatunsidf);
HLE_EXPORT(__floatdidf, floatdidf);
HLE_EXPORT(__floatundisf, floatundisf);
HLE_EXPORT(__divsi3, divsi);
HLE_EXPORT(__modsi3, modsi);
HLE_EXPORT(__udivsi3, udivsi);
HLE_EXPORT(__umodsi3, umodsi);
HLE_EXPORT(__divdi3, divdi);
HLE_EXPORT(__eqsf2vfp, eqsf);
HLE_EXPORT(__nesf2vfp, nesf);
HLE_EXPORT(__gesf2vfp, gesf);
HLE_EXPORT(__gtsf2vfp, gtsf);
HLE_EXPORT(__lesf2vfp, lesf);
HLE_EXPORT(__ltsf2vfp, ltsf);
HLE_EXPORT(__eqdf2vfp, eqdf);
HLE_EXPORT(__gedf2vfp, gedf);
HLE_EXPORT(__gtdf2vfp, gtdf);
HLE_EXPORT(__ledf2vfp, ledf);
HLE_EXPORT(__ltdf2vfp, ltdf);
HLE_EXPORT(__unordsf2vfp, unordsf);
HLE_EXPORT(__unorddf2vfp, unorddf);
HLE_EXPORT(_Unwind_SjLj_Register, sjlj_register);
HLE_EXPORT(_Unwind_SjLj_Unregister, sjlj_unregister);
HLE_EXPORT(_Unwind_SjLj_Resume, sjlj_resume);
HLE_EXPORT_AS("__ZNSt8ios_base4InitC1Ev", ios_init);
HLE_EXPORT_AS("__ZNSt8ios_base4InitD1Ev", ios_init);
HLE_EXPORT_AS("__ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc", ostream_string);
HLE_EXPORT_AS("__ZNSolsEi", ostream_int);
HLE_EXPORT_AS("__ZNSolsEj", ostream_uint);
HLE_EXPORT_AS("__ZSt4cout", cout_storage);
HLE_EXPORT(__tolower, tolower32);
HLE_EXPORT(arc4random, arc4random);
HLE_EXPORT(floorf, floorf);
HLE_EXPORT(round, (double (*)(double))round);
HLE_EXPORT(roundf, roundf);
HLE_EXPORT(exit, exit);
HLE_EXPORT(puts, puts);
HLE_EXPORT(rand, rand);
HLE_EXPORT(srand, srand);
HLE_EXPORT(strcat, strcat);
HLE_EXPORT(strcpy, strcpy);
HLE_EXPORT(strcspn, strcspn);
HLE_EXPORT(strncmp, strncmp);
HLE_EXPORT(strncpy, strncpy);
HLE_EXPORT(time, time);
