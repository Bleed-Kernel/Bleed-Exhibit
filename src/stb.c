#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_SIMD            // its sse path wants headers the cross compiler doesnt ship here
#define STBI_FAILURE_USERMSG
#define STBI_NO_THREAD_LOCALS   // bleed doesnt set fs up for tls, its error message would fault
#include <stb_image.h>
