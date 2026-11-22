/* Build configuration for the TinyCC copy built into the game (see THIRD_PARTY.md).
 * Only libtcc is used, compiling the translated game code into memory at launch;
 * the target is picked from the host compiler (x86-64 / ARM64, Windows / Linux). */
#define TCC_VERSION "0.9.28rc"
#define CONFIG_TCC_BACKTRACE 0
#define CONFIG_TCC_BCHECK 0
#define CONFIG_TCC_SEMLOCK 0
#define CONFIG_TCCDIR "."
#define CONFIG_TCC_PREDEFS 1
#define GCC_MAJOR 13
#define CC_NAME CC_gcc
