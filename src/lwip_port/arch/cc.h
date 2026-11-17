/* lwIP compiler / platform glue for RoboOS (clang or GCC, AArch64, freestanding) */
#ifndef ARCH_CC_H
#define ARCH_CC_H

#define LWIP_NO_INTTYPES_H 1
#define LWIP_NO_CTYPE_H    1
#define X8_F  "02x"
#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "lu"

#define BYTE_ORDER LITTLE_ENDIAN
#define LWIP_PROVIDE_ERRNO 1

/* code may run with strict alignment: let the compiler see the packing */
#define PACK_STRUCT_STRUCT __attribute__((packed))

void kprintf(const char *fmt, ...);
void lwip_port_assert(const char *msg, int line, const char *file);
unsigned int lwip_port_rand(void);
#define LWIP_PLATFORM_DIAG(x)   do { kprintf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) lwip_port_assert(x, __LINE__, __FILE__)
#define LWIP_RAND()             lwip_port_rand()

#endif
