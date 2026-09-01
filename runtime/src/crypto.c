/* crypto.c — std/crypto: message digests, HMAC, and the process's entropy
 * source.
 *
 * Everything here is written from the specifications, in C11, over nothing but
 * libc. There is no OpenSSL, no libsodium, and no third-party C anywhere in
 * this runtime — these four compression functions and RFC 2104's two-pass
 * construction are short and completely specified, and a dependency that has
 * to be found, versioned and audited on every platform costs more than they
 * do. Every constant below is transcribed from FIPS 180-4 (SHA-1, SHA-256,
 * SHA-512) and RFC 1321 (MD5), and the conformance test pins the algorithms
 * against those documents' own published vectors.
 *
 * WHAT IS AND IS NOT SAFE HERE
 *
 *   SHA-256 and SHA-512 are current. Use them.
 *
 *   SHA-1 and MD5 are BROKEN FOR SECURITY and are provided only for
 *   checksums and for interoperating with formats that already chose them.
 *   Practical collisions exist for both — MD5 in seconds on a laptop, SHA-1
 *   since SHAttered (2017) — so neither may carry a signature, a certificate,
 *   a content-addressed identity, or anything else whose value depends on an
 *   attacker being unable to produce a second input with the same digest.
 *   They are exported under their own names, never as a default, so choosing
 *   one is always something a program said out loud.
 *
 *   HMAC repairs a great deal (there is no practical forgery against
 *   HMAC-MD5 or HMAC-SHA-1) but a new protocol still wants HMAC-SHA-256.
 *
 *   crypto.random is a CSPRNG and is the ONLY random source in this runtime
 *   fit for a key, a token, a nonce or a session id. std/rand is a fast
 *   reproducible sequence and is not — see the note over vl_random_bytes.
 *
 * TYPES. A message may be spelled either way a program has bytes: a `str`
 * (which in this runtime is a byte string — os.read and net.recv already
 * carry binary in one) or a `[]byte`. Digests come back two ways: the plain
 * name yields LOWERCASE HEX, which is what the published vectors are written
 * in and what is safe to `say`, and the `_raw` name yields the digest bytes
 * as a str, so that `.hex()`, `.b64()` and `bytes()` — which already exist —
 * are the whole encoding layer and this file needs no opinion about it.
 *
 * Portability: getentropy(2) and /dev/urandom are POSIX-ish, so the entropy
 * source alone sits behind a _WIN32 stub arm, exactly as io.c does. The four
 * digests are pure arithmetic and portable as written.
 */
#include "voila_int.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- entropy */

#if defined(_WIN32)
/* No Windows port exists yet. BCryptGenRandom (bcrypt.lib) is the call this
 * arm wants; until someone writes it, a Windows build fails loudly at the
 * first draw rather than quietly returning zeros. */
static bool cry_entropy(uint8_t *out, size_t n) {
  (void)out;
  (void)n;
  errno = ENOSYS; /* so vl_random_bytes's abort names a reason, as io.c does */
  return false;
}
#else
#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif

/* getentropy(2) is the modern kernel interface: it cannot fail transiently,
 * needs no file descriptor, and works inside a chroot or with the descriptor
 * table exhausted, all of which /dev/urandom can lose to. macOS has had it
 * since 10.12 and glibc since 2.25; both declare it in <sys/random.h>, while
 * OpenBSD (which invented it) declares it in <unistd.h>, already included. */
#if defined(__APPLE__) ||                                                      \
    (defined(__GLIBC__) &&                                                     \
     (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 25))) ||          \
    (defined(__FreeBSD__) && __FreeBSD__ >= 12)
#include <sys/random.h>
#define CRY_HAVE_GETENTROPY 1
#elif defined(__OpenBSD__)
#define CRY_HAVE_GETENTROPY 1 /* declared in <unistd.h>, already included */
#endif
/* Anywhere else — musl, an older glibc, any other libc — the guard stays off
 * and cry_urandom below is the source. That is a correct CSPRNG, not a
 * degraded one; what it loses is only getentropy's immunity to a missing
 * /dev and an exhausted descriptor table. */

/* cry_urandom is the LAST resort, and it is the only source here that can
 * hand back output from a pool the kernel has not finished seeding —
 * /dev/urandom never blocks, by design. That is why the two syscalls above
 * are tried first on every platform that has one, and why Linux reaches
 * getrandom directly rather than only through a libc wrapper.
 *
 * It opens the device on EVERY call. That is deliberate: the runtime is
 * multi-threaded (spawn/group), and a cached descriptor would need a lock and
 * would still have to answer what happens when user code closes it. O_CLOEXEC
 * matters for the same reason the lock would: os.run forks and execs
 * (std.c), and without it a concurrent draw leaks this descriptor into the
 * child.
 *
 * Both loops are the point. read(2) may return short or be interrupted, and
 * treating a short read as success is how a "random" buffer ends up with a
 * tail of zeros. */
static bool cry_urandom(uint8_t *out, size_t n) {
  int fd;
  do {
    fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0) return false;
  size_t got = 0;
  while (got < n) {
    ssize_t r = read(fd, out + got, n - got);
    if (r < 0) {
      if (errno == EINTR) continue;
      close(fd);
      return false;
    }
    if (r == 0) { /* EOF on /dev/urandom means the device is not the device */
      close(fd);
      return false;
    }
    got += (size_t)r;
  }
  close(fd);
  return true;
}

#if defined(__linux__) && defined(SYS_getrandom)
#define CRY_HAVE_GETRANDOM 1
/* On Linux the getrandom(2) SYSTEM CALL is reached directly rather than only
 * through a libc wrapper, because the WRAPPER is what varies: glibc grew
 * getentropy in 2.25 and musl in 1.1.20, and there is no reliable way to
 * detect musl from the preprocessor — so an Alpine build, a very common
 * deployment base, would otherwise fall all the way through to /dev/urandom.
 * getrandom(…, 0) blocks until the pool is seeded, which is the property the
 * comment over vl_random_bytes claims and /dev/urandom cannot give.
 *
 * Any failure falls through to the sources below rather than being fatal
 * here: ENOSYS on a kernel older than 3.17, EPERM under a seccomp filter
 * that does not allow it. A partial fill is discarded, not patched up — the
 * next source re-fills the whole buffer. */
static bool cry_getrandom(uint8_t *out, size_t n) {
  size_t got = 0;
  while (got < n) {
    long r = syscall(SYS_getrandom, out + got, n - got, 0);
    if (r < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (r == 0) return false;
    got += (size_t)r;
  }
  return true;
}
#endif

static bool cry_entropy(uint8_t *out, size_t n) {
#if defined(CRY_HAVE_GETRANDOM)
  if (cry_getrandom(out, n)) return true;
#endif
#if defined(CRY_HAVE_GETENTROPY)
  /* getentropy is capped at 256 bytes per call on every platform that has
   * it, so a larger request is a loop rather than one call that silently
   * fills part of the buffer. */
  {
    size_t got = 0;
    bool ok = true;
    while (got < n) {
      size_t chunk = n - got;
      if (chunk > 256) chunk = 256;
      if (getentropy(out + got, chunk) != 0) {
        if (errno == EINTR) continue;
        ok = false; /* fall through; the next source re-fills the WHOLE buffer */
        break;
      }
      got += chunk;
    }
    if (ok) return true;
  }
#endif
  return cry_urandom(out, n);
}
#endif /* _WIN32 */

/* vl_try_random_bytes fills `out` with n cryptographically secure random
 * bytes and REPORTS failure instead of ending the process.
 *
 * It exists for exactly one caller. vl_init seeds std/rand's default sequence
 * from here (std.c), and that call runs in EVERY program — including the vast
 * majority that never ask for a random number at all. Making it fail-stop
 * would turn "this machine has no reachable entropy source" (a chroot with no
 * /dev, a seccomp filter, an exhausted descriptor table) into "no Voilà
 * program starts", which is a far worse failure than the one being prevented:
 * std/rand is documented as unfit for security whatever seeds it. So that one
 * caller degrades, deliberately and in the open, and every other caller does
 * not. */
bool vl_try_random_bytes(void *out, size_t n) {
  if (n == 0) return true;
  return cry_entropy((uint8_t *)out, n);
}

/* vl_random_bytes is the same thing for everyone who needs REAL entropy:
 * crypto.random, crypto.random_int and uuid.new. It aborts instead.
 *
 * IT NEVER FALLS BACK TO A WEAK SOURCE. A CSPRNG that silently degrades into
 * random(3) when the kernel is unreachable is worse than no CSPRNG at all:
 * the program keeps running, the keys it mints look fine, and nothing on any
 * code path ever says so. The only honest answers are "here is entropy" and
 * "this process cannot continue", and vl_abort is the second one. */
void vl_random_bytes(void *out, size_t n) {
  if (!vl_try_random_bytes(out, n))
    vl_abort("crypto: no entropy source (getrandom, getentropy and "
             "/dev/urandom all failed: %s)",
             strerror(errno));
}

/* ------------------------------------------------------------- digests */

typedef enum { CRY_MD5, CRY_SHA1, CRY_SHA256, CRY_SHA512 } CryAlg;

#define CRY_MAXDIGEST 64 /* SHA-512 */
#define CRY_MAXBLOCK 128 /* SHA-512 */

/* One incremental context for all four. The state is a union because MD5 and
 * SHA-1/256 keep 32-bit words and SHA-512 keeps 64-bit ones; `len` counts
 * bytes absorbed, which every one of the four turns into its own length
 * encoding at the end. Streaming rather than one-shot is what lets HMAC feed
 * the key pad and the message in turn without copying the message. */
typedef struct {
  CryAlg alg;
  uint64_t len; /* bytes absorbed */
  size_t nbuf;  /* bytes held in buf, always < block size */
  uint8_t buf[CRY_MAXBLOCK];
  union {
    uint32_t w32[8];
    uint64_t w64[8];
  } h;
} CryCtx;

static uint32_t rotl32(uint32_t x, int k) {
  return (uint32_t)((x << k) | (x >> (32 - k)));
}
static uint32_t rotr32(uint32_t x, int k) {
  return (uint32_t)((x >> k) | (x << (32 - k)));
}
static uint64_t rotr64(uint64_t x, int k) {
  return (uint64_t)((x >> k) | (x << (64 - k)));
}

static uint32_t rd_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint32_t rd_le32(const uint8_t *p) {
  return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[1] << 8) | (uint32_t)p[0];
}
static uint64_t rd_be64(const uint8_t *p) {
  return ((uint64_t)rd_be32(p) << 32) | (uint64_t)rd_be32(p + 4);
}
static void wr_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}
static void wr_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}
static void wr_be64(uint8_t *p, uint64_t v) {
  wr_be32(p, (uint32_t)(v >> 32));
  wr_be32(p + 4, (uint32_t)v);
}

/* ---- MD5 (RFC 1321). LEGACY: collisions are trivial; checksums only. */

/* K[i] = floor(2^32 * abs(sin(i + 1))), i in radians — RFC 1321 §3.4. */
static const uint32_t MD5_K[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu,
    0x4787c62au, 0xa8304613u, 0xfd469501u, 0x698098d8u, 0x8b44f7afu,
    0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u, 0xa679438eu,
    0x49b40821u, 0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
    0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u, 0x21e1cde6u,
    0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u,
    0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u,
    0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u, 0xd9d4d039u,
    0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u, 0xf4292244u, 0x432aff97u,
    0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du,
    0x85845dd1u, 0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u};

static const uint8_t MD5_S[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

static void md5_block(uint32_t *h, const uint8_t *p) {
  uint32_t m[16];
  for (int i = 0; i < 16; i++) m[i] = rd_le32(p + i * 4);
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
  for (int i = 0; i < 64; i++) {
    uint32_t f;
    int g;
    if (i < 16) {
      f = (b & c) | (~b & d);
      g = i;
    } else if (i < 32) {
      f = (d & b) | (~d & c);
      g = (5 * i + 1) & 15;
    } else if (i < 48) {
      f = b ^ c ^ d;
      g = (3 * i + 5) & 15;
    } else {
      f = c ^ (b | ~d);
      g = (7 * i) & 15;
    }
    f += a + MD5_K[i] + m[g];
    a = d;
    d = c;
    c = b;
    b += rotl32(f, MD5_S[i]);
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
}

/* ---- SHA-1 (FIPS 180-4 §6.1). LEGACY: SHAttered, 2017. Interop only. */

static void sha1_block(uint32_t *h, const uint8_t *p) {
  uint32_t w[80];
  for (int i = 0; i < 16; i++) w[i] = rd_be32(p + i * 4);
  for (int i = 16; i < 80; i++)
    w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20) {
      f = (b & c) | (~b & d);
      k = 0x5a827999u;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ed9eba1u;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8f1bbcdcu;
    } else {
      f = b ^ c ^ d;
      k = 0xca62c1d6u;
    }
    uint32_t t = rotl32(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = rotl32(b, 30);
    b = a;
    a = t;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
}

/* ---- SHA-256 (FIPS 180-4 §6.2) */

/* The first 32 bits of the fractional parts of the cube roots of the first
 * 64 primes — FIPS 180-4 §4.2.2. */
static const uint32_t SHA256_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

static void sha256_block(uint32_t *h, const uint8_t *p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++) w[i] = rd_be32(p + i * 4);
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
  uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = hh + S1 + ch + SHA256_K[i] + w[i];
    uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = S0 + maj;
    hh = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
  h[5] += f;
  h[6] += g;
  h[7] += hh;
}

/* ---- SHA-512 (FIPS 180-4 §6.4) */

/* The first 64 bits of the fractional parts of the cube roots of the first
 * 80 primes — FIPS 180-4 §4.2.3. */
static const uint64_t SHA512_K[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL,
    0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL,
    0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL, 0x2de92c6f592b0275ULL,
    0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL,
    0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL,
    0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL,
    0x92722c851482353bULL, 0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL,
    0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL,
    0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL,
    0xc67178f2e372532bULL, 0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL,
    0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};

static void sha512_block(uint64_t *h, const uint8_t *p) {
  uint64_t w[80];
  for (int i = 0; i < 16; i++) w[i] = rd_be64(p + i * 8);
  for (int i = 16; i < 80; i++) {
    uint64_t s0 =
        rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
    uint64_t s1 =
        rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint64_t a = h[0], b = h[1], c = h[2], d = h[3];
  uint64_t e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 80; i++) {
    uint64_t S1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
    uint64_t ch = (e & f) ^ (~e & g);
    uint64_t t1 = hh + S1 + ch + SHA512_K[i] + w[i];
    uint64_t S0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
    uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint64_t t2 = S0 + maj;
    hh = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
  h[5] += f;
  h[6] += g;
  h[7] += hh;
}

/* ---- the shared incremental driver */

static size_t alg_block(CryAlg a) { return a == CRY_SHA512 ? 128u : 64u; }

static size_t alg_digest(CryAlg a) {
  switch (a) {
  case CRY_MD5: return 16;
  case CRY_SHA1: return 20;
  case CRY_SHA256: return 32;
  case CRY_SHA512: return 64;
  }
  return 0;
}

static void cry_init(CryCtx *c, CryAlg a) {
  memset(c, 0, sizeof *c);
  c->alg = a;
  switch (a) {
  case CRY_MD5:
    c->h.w32[0] = 0x67452301u;
    c->h.w32[1] = 0xefcdab89u;
    c->h.w32[2] = 0x98badcfeu;
    c->h.w32[3] = 0x10325476u;
    break;
  case CRY_SHA1:
    c->h.w32[0] = 0x67452301u;
    c->h.w32[1] = 0xefcdab89u;
    c->h.w32[2] = 0x98badcfeu;
    c->h.w32[3] = 0x10325476u;
    c->h.w32[4] = 0xc3d2e1f0u;
    break;
  case CRY_SHA256:
    /* the first 32 bits of the fractional parts of the square roots of the
     * first eight primes — FIPS 180-4 §5.3.3 */
    c->h.w32[0] = 0x6a09e667u;
    c->h.w32[1] = 0xbb67ae85u;
    c->h.w32[2] = 0x3c6ef372u;
    c->h.w32[3] = 0xa54ff53au;
    c->h.w32[4] = 0x510e527fu;
    c->h.w32[5] = 0x9b05688cu;
    c->h.w32[6] = 0x1f83d9abu;
    c->h.w32[7] = 0x5be0cd19u;
    break;
  case CRY_SHA512:
    /* the first 64 bits of the same eight square roots — §5.3.5 */
    c->h.w64[0] = 0x6a09e667f3bcc908ULL;
    c->h.w64[1] = 0xbb67ae8584caa73bULL;
    c->h.w64[2] = 0x3c6ef372fe94f82bULL;
    c->h.w64[3] = 0xa54ff53a5f1d36f1ULL;
    c->h.w64[4] = 0x510e527fade682d1ULL;
    c->h.w64[5] = 0x9b05688c2b3e6c1fULL;
    c->h.w64[6] = 0x1f83d9abfb41bd6bULL;
    c->h.w64[7] = 0x5be0cd19137e2179ULL;
    break;
  }
}

static void cry_compress(CryCtx *c, const uint8_t *p) {
  switch (c->alg) {
  case CRY_MD5: md5_block(c->h.w32, p); break;
  case CRY_SHA1: sha1_block(c->h.w32, p); break;
  case CRY_SHA256: sha256_block(c->h.w32, p); break;
  case CRY_SHA512: sha512_block(c->h.w64, p); break;
  }
}

static void cry_update(CryCtx *c, const uint8_t *p, size_t n) {
  size_t bs = alg_block(c->alg);
  c->len += n;
  if (c->nbuf) { /* top up the partial block first */
    size_t want = bs - c->nbuf;
    if (want > n) want = n;
    memcpy(c->buf + c->nbuf, p, want);
    c->nbuf += want;
    p += want;
    n -= want;
    if (c->nbuf < bs) return;
    cry_compress(c, c->buf);
    c->nbuf = 0;
  }
  while (n >= bs) {
    cry_compress(c, p);
    p += bs;
    n -= bs;
  }
  if (n) {
    memcpy(c->buf, p, n);
    c->nbuf = n;
  }
}

/* cry_final appends the padding every one of the four shares — a 0x80 byte,
 * zeros, then the message length in BITS — and writes the digest.
 *
 * The two families differ in exactly two places, and both are here rather
 * than in the block functions: MD5 encodes its length little-endian and emits
 * little-endian words, while the SHA family does both big-endian; and SHA-512
 * reserves 128 bits for the length where the others reserve 64. The high half
 * of SHA-512's length is written as zero, which is exact for any message a
 * process can actually hold: `bits` below is a uint64_t, so both it and the
 * high half stop being right at the same place — 2^61 bytes, two million
 * terabytes in one `str`.
 *
 * This is the code path the empty-message vectors in the conformance test
 * exist to pin: with no message at all, padding is the entire input, so a
 * wrong length encoding or a wrong reserve shows up there and nowhere else. */
static void cry_final(CryCtx *c, uint8_t *out) {
  size_t bs = alg_block(c->alg);
  size_t lenfield = (c->alg == CRY_SHA512) ? 16u : 8u;
  uint64_t bits = c->len * 8u;
  uint8_t pad[CRY_MAXBLOCK * 2];
  size_t npad = 0;

  pad[npad++] = 0x80;
  while ((c->nbuf + npad + lenfield) % bs != 0) pad[npad++] = 0;
  if (c->alg == CRY_MD5) {
    for (int i = 0; i < 8; i++) pad[npad++] = (uint8_t)(bits >> (8 * i));
  } else {
    if (lenfield == 16) {
      memset(pad + npad, 0, 8); /* the high 64 bits of the bit count */
      npad += 8;
    }
    wr_be64(pad + npad, bits);
    npad += 8;
  }
  cry_update(c, pad, npad); /* c->len goes stale here; nothing reads it again */

  switch (c->alg) {
  case CRY_MD5:
    for (int i = 0; i < 4; i++) wr_le32(out + i * 4, c->h.w32[i]);
    break;
  case CRY_SHA1:
    for (int i = 0; i < 5; i++) wr_be32(out + i * 4, c->h.w32[i]);
    break;
  case CRY_SHA256:
    for (int i = 0; i < 8; i++) wr_be32(out + i * 4, c->h.w32[i]);
    break;
  case CRY_SHA512:
    for (int i = 0; i < 8; i++) wr_be64(out + i * 8, c->h.w64[i]);
    break;
  }
}

static void cry_hash(CryAlg a, const uint8_t *p, size_t n, uint8_t *out) {
  CryCtx c;
  cry_init(&c, a);
  cry_update(&c, p, n);
  cry_final(&c, out);
}

/* ---- HMAC (RFC 2104) */

/* K' is the key padded to one block, or H(key) padded when the key is longer
 * than a block. Then HMAC = H((K' ^ opad) || H((K' ^ ipad) || message)).
 * The message is fed through cry_update, never copied. */
static void cry_hmac(CryAlg a, const uint8_t *key, size_t keylen,
                     const uint8_t *msg, size_t msglen, uint8_t *out) {
  size_t bs = alg_block(a), ds = alg_digest(a);
  uint8_t k[CRY_MAXBLOCK], pad[CRY_MAXBLOCK], inner[CRY_MAXDIGEST];
  CryCtx c;

  memset(k, 0, bs);
  if (keylen > bs) cry_hash(a, key, keylen, k);
  else memcpy(k, key, keylen);

  for (size_t i = 0; i < bs; i++) pad[i] = (uint8_t)(k[i] ^ 0x36);
  cry_init(&c, a);
  cry_update(&c, pad, bs);
  cry_update(&c, msg, msglen);
  cry_final(&c, inner);

  for (size_t i = 0; i < bs; i++) pad[i] = (uint8_t)(k[i] ^ 0x5c);
  cry_init(&c, a);
  cry_update(&c, pad, bs);
  cry_update(&c, inner, ds);
  cry_final(&c, out);

  /* Every local here is key material and none of it may be left on the stack
   * for the next frame to inherit. The CONTEXT is the one worth naming: `c`
   * has just held H's running state over `K' ^ ipad`, and anyone who recovers
   * that state can forge a MAC for any message under this key without ever
   * learning the key itself. Wiping the pads and not `c` would be the
   * cheap half of the job. (volatile so the stores are not optimised away as
   * dead.) This is hygiene, not a guarantee: C says nothing about the copies
   * the compiler may have spilled into registers or other stack slots. */
  volatile uint8_t *wk = k, *wp = pad, *wi = inner, *wc = (volatile uint8_t *)&c;
  for (size_t i = 0; i < bs; i++) {
    wk[i] = 0;
    wp[i] = 0;
  }
  for (size_t i = 0; i < ds; i++) wi[i] = 0;
  for (size_t i = 0; i < sizeof c; i++) wc[i] = 0;
}

/* ------------------------------------------------- the Voilà boundary */

/* CryIn is a message as bytes, plus whatever had to be allocated to get it
 * that way. `owned` is NULL when the bytes are borrowed straight out of a
 * str, which is the common case and copies nothing. */
typedef struct {
  const uint8_t *p;
  size_t n;
  char *owned;
} CryIn;

/* cry_in accepts the two spellings a program has for bytes, and only those.
 * A str is used byte for byte — no UTF-8 interpretation, because a digest is
 * over octets. A []byte is a slice of ints, so it has to be flattened; the
 * low eight bits of each element are taken, which is exactly what `byte`
 * holds and is also what makes `bytes(s)` round-trip.
 *
 * Anything else ABORTS rather than returning an error value or hashing a
 * rendering of it. Hashing str(v) of some other value would be the dangerous
 * answer: it would succeed, produce a stable-looking digest, and mean nothing
 * — and a signature over it would verify against a different program's idea
 * of the same object. */
static CryIn cry_in(Value v, const char *who) {
  CryIn in;
  in.owned = NULL;
  if (v.t == VL_OBJ && v.u.o->kind == O_STR) {
    in.p = (const uint8_t *)vl_cstr(v);
    in.n = (size_t)vl_strlen(v);
    return in;
  }
  if (v.t == VL_OBJ && v.u.o->kind == O_SLICE) {
    VlSlice *s = (VlSlice *)v.u.o;
    char *b = (char *)vl_alloc((size_t)s->len + 1);
    for (int64_t i = 0; i < s->len; i++) {
      Value e = s->e[i];
      unsigned long u = 0;
      if (e.t == VL_INT) u = (unsigned long)e.u.i;
      else if (e.t == VL_RUNE) u = (unsigned long)e.u.r;
      else
        vl_abort("%s: a []byte element must be a byte, got %s", who,
                 vl_kind_name(e));
      b[i] = (char)(u & 0xFFu);
    }
    in.p = (const uint8_t *)b;
    in.n = (size_t)s->len;
    in.owned = b;
    return in;
  }
  vl_abort("%s: expected a str or []byte, got %s", who, vl_kind_name(v));
  in.p = NULL; /* unreachable; vl_abort does not return */
  in.n = 0;
  return in;
}

static void cry_in_free(CryIn *in) {
  if (in->owned) free(in->owned);
  in->owned = NULL;
}

/* cry_out renders a digest either as lowercase hex or as the raw bytes in a
 * str. There is one encoder here rather than a call into str.hex because the
 * digest is bytes, not a Voilà value, and a round trip through one to reach
 * the other would be the only allocation on the path. */
static Value cry_out(const uint8_t *d, size_t n, bool hex) {
  if (!hex) return vl_str_n((const char *)d, (int64_t)n);
  static const char H[] = "0123456789abcdef";
  char *b = (char *)vl_alloc(n * 2 + 1);
  for (size_t i = 0; i < n; i++) {
    b[i * 2] = H[d[i] >> 4];
    b[i * 2 + 1] = H[d[i] & 15];
  }
  return vl_str_take(b, (int64_t)(n * 2));
}

static Value digest_of(CryAlg a, Value subject, bool hex, const char *who) {
  uint8_t d[CRY_MAXDIGEST];
  CryIn in = cry_in(subject, who);
  cry_hash(a, in.p, in.n, d);
  cry_in_free(&in);
  return cry_out(d, alg_digest(a), hex);
}

static Value hmac_of(CryAlg a, Value key, Value msg, bool hex,
                     const char *who) {
  uint8_t d[CRY_MAXDIGEST];
  CryIn k = cry_in(key, who);
  CryIn m = cry_in(msg, who);
  cry_hmac(a, k.p, k.n, m.p, m.n, d);
  cry_in_free(&k);
  cry_in_free(&m);
  return cry_out(d, alg_digest(a), hex);
}

/* The natives. Each is one line over the two helpers above; the macro exists
 * so that adding an algorithm cannot produce a body that disagrees with its
 * name. std.c's table[] points straight at these. */
#define CRY_FN(fn, alg, hex)                                                   \
  Value vl_crypto_##fn(Value *argv, int argc, VlFrame *up) {                   \
    (void)argc, (void)up;                                                      \
    return digest_of(alg, argv[0], hex, "crypto." #fn);                        \
  }
CRY_FN(md5, CRY_MD5, true)
CRY_FN(md5_raw, CRY_MD5, false)
CRY_FN(sha1, CRY_SHA1, true)
CRY_FN(sha1_raw, CRY_SHA1, false)
CRY_FN(sha256, CRY_SHA256, true)
CRY_FN(sha256_raw, CRY_SHA256, false)
CRY_FN(sha512, CRY_SHA512, true)
CRY_FN(sha512_raw, CRY_SHA512, false)
#undef CRY_FN

#define CRY_MAC(fn, alg, hex)                                                  \
  Value vl_crypto_hmac_##fn(Value *argv, int argc, VlFrame *up) {              \
    (void)argc, (void)up;                                                      \
    return hmac_of(alg, argv[0], argv[1], hex, "crypto.hmac_" #fn);            \
  }
CRY_MAC(md5, CRY_MD5, true)
CRY_MAC(md5_raw, CRY_MD5, false)
CRY_MAC(sha1, CRY_SHA1, true)
CRY_MAC(sha1_raw, CRY_SHA1, false)
CRY_MAC(sha256, CRY_SHA256, true)
CRY_MAC(sha256_raw, CRY_SHA256, false)
CRY_MAC(sha512, CRY_SHA512, true)
CRY_MAC(sha512_raw, CRY_SHA512, false)
#undef CRY_MAC

/* crypto.random(n) — n cryptographically secure random bytes, as a str.
 * `.hex()` and `.b64()` turn it into a printable token, `bytes()` into a
 * []byte; this returns the bytes so that none of those three is privileged. */
Value vl_crypto_random(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  int64_t n = 0;
  if (argv[0].t == VL_INT) n = argv[0].u.i;
  else vl_abort("crypto.random(n): expected an int, got %s",
                vl_kind_name(argv[0]));
  /* A bad COUNT is a RangeError, not an abort: `crypto.random(k)` with a k
   * that came out wrong is an ordinary program error and every other range
   * error in this runtime is catchable — division by zero (ops.c), a slice
   * bound (coll.c), str.repeat's size cap (str.c). A bad TYPE still aborts,
   * matching raw_int in std.c. */
  if (n < 0) {
    vl_throwf("RangeError", "crypto.random(%lld): a count cannot be negative",
              (long long)n);
    return vl_nil(); /* vl_throwf does not return; this is for the reader */
  }
  if (n == 0) return vl_str("");
  char *b = (char *)vl_alloc((size_t)n + 1);
  vl_random_bytes(b, (size_t)n);
  return vl_str_take(b, n);
}

/* crypto.random_int(n) — a uniform integer in [0, n) from the CSPRNG.
 *
 * The rejection loop is the whole reason this exists rather than being left
 * to `crypto.random(8)` and a modulo: 2^64 is not a multiple of most n, so
 * `x % n` makes the low residues more likely, by a margin that matters as
 * soon as n is large. Values at or above the largest multiple of n that fits
 * are thrown away and redrawn, which is unbiased and terminates with
 * probability 1 (each draw succeeds at least half the time). */
Value vl_crypto_random_int(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  if (argv[0].t != VL_INT)
    vl_abort("crypto.random_int(n): expected an int, got %s",
             vl_kind_name(argv[0]));
  int64_t n = argv[0].u.i;
  /* Catchable, for the same reason as crypto.random above — and here it is
   * the likelier spelling: `crypto.random_int(len(xs))` over an empty
   * collection must be something a program can handle, not a process kill. */
  if (n <= 0) {
    vl_throwf("RangeError", "crypto.random_int(%lld): the bound must be positive",
              (long long)n);
    return vl_nil(); /* vl_throwf does not return; un below would be 0 */
  }
  uint64_t un = (uint64_t)n;
  uint64_t limit = UINT64_MAX - (UINT64_MAX % un) - 1; /* last accepted draw */
  for (;;) {
    uint64_t x;
    vl_random_bytes(&x, sizeof x);
    if (x <= limit) return vl_int((int64_t)(x % un));
  }
}

/* crypto.equal(a, b) — equality in time that does not depend on WHERE the
 * two differ.
 *
 * `a == b` on strings stops at the first differing byte, and that timing is
 * enough to recover a MAC one byte at a time from a service that will verify
 * a guess. So every byte is compared and the results are OR-ed. The LENGTHS
 * are compared normally and short-circuit: a digest's length is fixed and
 * public, so it is not a secret this can leak. */
Value vl_crypto_equal(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  CryIn a = cry_in(argv[0], "crypto.equal");
  CryIn b = cry_in(argv[1], "crypto.equal");
  bool same = a.n == b.n;
  if (same) {
    unsigned diff = 0;
    for (size_t i = 0; i < a.n; i++) diff |= (unsigned)(a.p[i] ^ b.p[i]);
    same = diff == 0;
  }
  cry_in_free(&a);
  cry_in_free(&b);
  return vl_bool(same);
}

/* uuid.new's body (std.c registers the name). A version-4 UUID is 122 random
 * bits with six fixed: the version nibble and the two variant bits of RFC
 * 4122 §4.4. It is here, not next to std/rand, because the whole point of a
 * UUID is that two of them never collide — which is a claim about the SOURCE,
 * and random(3) cannot make it. */
Value vl_uuid_v4(void) {
  uint8_t b[16];
  vl_random_bytes(b, sizeof b);
  b[6] = (uint8_t)((b[6] & 0x0F) | 0x40); /* version 4 */
  b[8] = (uint8_t)((b[8] & 0x3F) | 0x80); /* variant 10x */
  char out[37];
  static const char H[] = "0123456789abcdef";
  int o = 0;
  for (int i = 0; i < 16; i++) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out[o++] = '-';
    out[o++] = H[b[i] >> 4];
    out[o++] = H[b[i] & 15];
  }
  out[o] = '\0';
  return vl_str(out);
}
