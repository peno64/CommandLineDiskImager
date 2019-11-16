#if defined WIN32 || defined _WIN32

#define noreturn
#define _Noreturn

#define _GL_ATTRIBUTE_CONST
#define _GL_ATTRIBUTE_PURE
#define _GL_ATTRIBUTE_MALLOC
#define _GL_INLINE static
#define _GL_INLINE_HEADER_END

#define S_IWUSR 0200
#define RETSIGTYPE void

#endif
