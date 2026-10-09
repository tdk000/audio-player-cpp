// Единственное место, где компилируется реализация miniaudio.
//
// Кодирование и генерацию сигналов отключаем — плееру они не нужны, а время
// сборки и размер exe это заметно уменьшает.
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
