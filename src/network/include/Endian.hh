#ifndef OPENSOCIALNET_ENDIAN_HH
#define OPENSOCIALNET_ENDIAN_HH

// Portable host<->big-endian byte swaps.
//
// glibc exposes htobe*/be*toh via <endian.h>. macOS has no such header and
// spells the same operations in <libkern/OSByteOrder.h>, so alias the names
// there. Include this header instead of <endian.h> anywhere those helpers
// are needed.
#if defined(__APPLE__)
#  include <libkern/OSByteOrder.h>
#  define htobe16(x) OSSwapHostToBigInt16(x)
#  define htobe32(x) OSSwapHostToBigInt32(x)
#  define htobe64(x) OSSwapHostToBigInt64(x)
#  define be16toh(x) OSSwapBigToHostInt16(x)
#  define be32toh(x) OSSwapBigToHostInt32(x)
#  define be64toh(x) OSSwapBigToHostInt64(x)
#else
#  include <endian.h>
#endif

#endif // OPENSOCIALNET_ENDIAN_HH
