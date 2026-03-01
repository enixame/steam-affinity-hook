#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

typedef int (*sched_setaffinity_fn_t)(pid_t, size_t, const cpu_set_t *);
typedef int (*pthread_setaffinity_np_fn_t)(pthread_t, size_t, const cpu_set_t *);

static sched_setaffinity_fn_t real_sched_setaffinity = NULL;
static pthread_setaffinity_np_fn_t real_pthread_setaffinity_np = NULL;
static pthread_once_t init_once = PTHREAD_ONCE_INIT;

static int g_log_enabled = 0;
static int g_block_enabled = 0;
static int g_rewrite_enabled = 0;
static int g_match_configured = 0;

static char *g_match_spec = NULL;
static char *g_rewrite_spec = NULL;

static __thread int g_in_hook = 0;

static long gettid_linux(void) {
    return syscall(SYS_gettid);
}

static void safe_log(const char *fmt, ...) {
    if (!g_log_enabled) return;
    if (g_in_hook > 1) return;

    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n < 0) return;
    if ((size_t)n >= sizeof(buf)) n = (int)(sizeof(buf) - 1);

    (void)write(STDERR_FILENO, buf, (size_t)n);
}

static int env_flag_is_true(const char *name) {
    const char *v = getenv(name);
    if (!v || !*v) return 0;
    return (strcmp(v, "1") == 0 ||
            strcasecmp(v, "true") == 0 ||
            strcasecmp(v, "yes") == 0 ||
            strcasecmp(v, "on") == 0);
}

__attribute__((constructor))
static void affinity_hook_ctor(void) {
    const char *v = getenv("AFFINITY_HOOK_LOG");
    if (v && *v && strcmp(v, "0") != 0) {
        char buf[256];
        int n = snprintf(buf, sizeof(buf),
                         "[affinity-hook] loaded pid=%ld\n", (long)getpid());
        if (n > 0) {
            (void)write(STDERR_FILENO, buf, (size_t)n);
        }
    }
}

static void init_real_symbols(void) {
    real_sched_setaffinity =
        (sched_setaffinity_fn_t)dlsym(RTLD_NEXT, "sched_setaffinity");
    real_pthread_setaffinity_np =
        (pthread_setaffinity_np_fn_t)dlsym(RTLD_NEXT, "pthread_setaffinity_np");

    g_log_enabled = env_flag_is_true("AFFINITY_HOOK_LOG");
    g_block_enabled = env_flag_is_true("AFFINITY_HOOK_BLOCK");

    const char *m = getenv("AFFINITY_HOOK_MATCH");
    if (m && *m) {
        g_match_spec = strdup(m);
        if (g_match_spec) g_match_configured = 1;
    }

    const char *r = getenv("AFFINITY_HOOK_REWRITE");
    if (r && *r) {
        g_rewrite_spec = strdup(r);
        if (g_rewrite_spec) g_rewrite_enabled = 1;
    }

    safe_log("[affinity-hook] init pid=%ld tid=%ld log=%d block=%d match='%s' rewrite='%s'\n",
             (long)getpid(), gettid_linux(),
             g_log_enabled, g_block_enabled,
             g_match_spec ? g_match_spec : "",
             g_rewrite_spec ? g_rewrite_spec : "");
}

static void ensure_init(void) {
    pthread_once(&init_once, init_real_symbols);
}

/* ---------- Helpers bitset / mask parsing ---------- */

static size_t bits_to_bytes(size_t nbits) {
    return (nbits + 7u) / 8u;
}

static void bitset_zero(unsigned char *buf, size_t nbytes) {
    memset(buf, 0, nbytes);
}

static void bitset_set(unsigned char *buf, size_t cpu) {
    buf[cpu / 8u] |= (unsigned char)(1u << (cpu % 8u));
}

static int bitset_test(const unsigned char *buf, size_t cpu) {
    return !!(buf[cpu / 8u] & (unsigned char)(1u << (cpu % 8u)));
}

static int parse_cpu_list_to_bitset(const char *spec,
                                    unsigned char *out,
                                    size_t nbits,
                                    char *errbuf,
                                    size_t errbuf_sz) {
    if (!spec || !*spec) {
        snprintf(errbuf, errbuf_sz, "empty cpu list");
        return -1;
    }

    size_t nbytes = bits_to_bytes(nbits);
    bitset_zero(out, nbytes);

    char *copy = strdup(spec);
    if (!copy) {
        snprintf(errbuf, errbuf_sz, "strdup failed");
        return -1;
    }

    char *saveptr = NULL;
    for (char *tok = strtok_r(copy, ",", &saveptr);
         tok != NULL;
         tok = strtok_r(NULL, ",", &saveptr)) {

        while (*tok == ' ' || *tok == '\t') tok++;
        if (*tok == '\0') {
            free(copy);
            snprintf(errbuf, errbuf_sz, "empty token");
            return -1;
        }

        char *dash = strchr(tok, '-');
        if (!dash) {
            char *end = NULL;
            errno = 0;
            unsigned long cpu = strtoul(tok, &end, 10);
            if (errno != 0 || end == tok || *end != '\0') {
                free(copy);
                snprintf(errbuf, errbuf_sz, "bad cpu token '%s'", tok);
                return -1;
            }
            if (cpu >= nbits) {
                free(copy);
                snprintf(errbuf, errbuf_sz, "cpu %lu out of range (max %zu)", cpu, nbits - 1);
                return -1;
            }
            bitset_set(out, (size_t)cpu);
        } else {
            *dash = '\0';
            const char *a_str = tok;
            const char *b_str = dash + 1;

            char *end1 = NULL;
            char *end2 = NULL;
            errno = 0;
            unsigned long a = strtoul(a_str, &end1, 10);
            unsigned long b = strtoul(b_str, &end2, 10);

            if (errno != 0 || end1 == a_str || *end1 != '\0' ||
                end2 == b_str || *end2 != '\0') {
                free(copy);
                snprintf(errbuf, errbuf_sz, "bad range token '%s-%s'", a_str, b_str);
                return -1;
            }
            if (a > b) {
                free(copy);
                snprintf(errbuf, errbuf_sz, "descending range '%lu-%lu'", a, b);
                return -1;
            }
            if (b >= nbits) {
                free(copy);
                snprintf(errbuf, errbuf_sz, "cpu %lu out of range (max %zu)", b, nbits - 1);
                return -1;
            }
            for (unsigned long cpu = a; cpu <= b; cpu++) {
                bitset_set(out, (size_t)cpu);
            }
        }
    }

    free(copy);
    return 0;
}

static int cpuset_to_string(const cpu_set_t *set,
                            size_t cpusetsize,
                            char *buf,
                            size_t bufsz) {
    if (!buf || bufsz == 0) return -1;
    buf[0] = '\0';

    size_t nbits = cpusetsize * 8u;
    size_t off = 0;
    int first = 1;

    for (size_t i = 0; i < nbits; i++) {
        if (!CPU_ISSET_S(i, cpusetsize, set)) continue;

        size_t start = i;
        size_t end = i;
        while (end + 1 < nbits && CPU_ISSET_S(end + 1, cpusetsize, set)) {
            end++;
        }

        int n;
        if (start == end) {
            n = snprintf(buf + off, bufsz - off, "%s%zu", first ? "" : ",", start);
        } else {
            n = snprintf(buf + off, bufsz - off, "%s%zu-%zu", first ? "" : ",", start, end);
        }

        if (n < 0 || (size_t)n >= bufsz - off) {
            if (bufsz > 0) buf[bufsz - 1] = '\0';
            return -1;
        }

        off += (size_t)n;
        first = 0;
        i = end;
    }

    if (first) {
        snprintf(buf, bufsz, "<empty>");
    }
    return 0;
}

static int cpuset_equals_cpuset(const cpu_set_t *a,
                                const cpu_set_t *b,
                                size_t cpusetsize) {
    size_t nbits = cpusetsize * 8u;
    for (size_t i = 0; i < nbits; i++) {
        int abit = CPU_ISSET_S(i, cpusetsize, a);
        int bbit = CPU_ISSET_S(i, cpusetsize, b);
        if (abit != bbit) return 0;
    }
    return 1;
}

static int mask_matches_spec_exact(const cpu_set_t *set,
                                   size_t cpusetsize,
                                   const char *spec,
                                   char *errbuf,
                                   size_t errbuf_sz) {
    size_t nbits = cpusetsize * 8u;
    size_t nbytes = bits_to_bytes(nbits);

    unsigned char *want = (unsigned char *)calloc(1, nbytes);
    unsigned char *have = (unsigned char *)calloc(1, nbytes);
    if (!want || !have) {
        free(want);
        free(have);
        snprintf(errbuf, errbuf_sz, "allocation failure");
        return -1;
    }

    if (parse_cpu_list_to_bitset(spec, want, nbits, errbuf, errbuf_sz) != 0) {
        free(want);
        free(have);
        return -1;
    }

    for (size_t i = 0; i < nbits; i++) {
        if (CPU_ISSET_S(i, cpusetsize, set)) {
            bitset_set(have, i);
        }
    }

    int match = (memcmp(want, have, nbytes) == 0);

    free(want);
    free(have);
    return match;
}

static int build_cpuset_from_spec(const char *spec,
                                  size_t cpusetsize,
                                  cpu_set_t *out_set,
                                  char *errbuf,
                                  size_t errbuf_sz) {
    size_t nbits = cpusetsize * 8u;
    size_t nbytes = bits_to_bytes(nbits);

    unsigned char *tmp = (unsigned char *)calloc(1, nbytes);
    if (!tmp) {
        snprintf(errbuf, errbuf_sz, "allocation failure");
        return -1;
    }

    if (parse_cpu_list_to_bitset(spec, tmp, nbits, errbuf, errbuf_sz) != 0) {
        free(tmp);
        return -1;
    }

    CPU_ZERO_S(cpusetsize, out_set);
    for (size_t i = 0; i < nbits; i++) {
        if (bitset_test(tmp, i)) {
            CPU_SET_S(i, cpusetsize, out_set);
        }
    }

    free(tmp);
    return 0;
}

static int build_full_machine_cpuset(size_t cpusetsize,
                                     cpu_set_t *out_set,
                                     char *specbuf,
                                     size_t specbuf_sz,
                                     char *errbuf,
                                     size_t errbuf_sz) {
    long ncpu = sysconf(_SC_NPROCESSORS_CONF);
    if (ncpu <= 0) {
        snprintf(errbuf, errbuf_sz, "sysconf(_SC_NPROCESSORS_CONF) failed");
        return -1;
    }

    size_t max_cpu = (size_t)(ncpu - 1);
    size_t nbits = cpusetsize * 8u;
    if (max_cpu >= nbits) {
        snprintf(errbuf, errbuf_sz,
                 "cpusetsize too small for %ld configured CPUs (max bit %zu, capacity %zu)",
                 ncpu, max_cpu, nbits);
        return -1;
    }

    snprintf(specbuf, specbuf_sz, "0-%zu", max_cpu);
    CPU_ZERO_S(cpusetsize, out_set);
    for (size_t i = 0; i <= max_cpu; i++) {
        CPU_SET_S(i, cpusetsize, out_set);
    }
    return 0;
}

static int build_default_rewrite_cpuset(size_t cpusetsize,
                                        cpu_set_t *out_set,
                                        char *out_str,
                                        size_t out_str_sz,
                                        char *errbuf,
                                        size_t errbuf_sz) {
    if (g_rewrite_enabled && g_rewrite_spec && *g_rewrite_spec) {
        if (build_cpuset_from_spec(g_rewrite_spec, cpusetsize,
                                   out_set, errbuf, errbuf_sz) != 0) {
            return -1;
        }
        if (out_str && out_str_sz > 0) {
            snprintf(out_str, out_str_sz, "%s", g_rewrite_spec);
        }
        return 0;
    }

    return build_full_machine_cpuset(cpusetsize, out_set,
                                     out_str, out_str_sz,
                                     errbuf, errbuf_sz);
}

struct hook_decision {
    int matched;
    int block;
    int rewrite;
    cpu_set_t rewritten_set_storage;
};

static void decide_for_mask(const cpu_set_t *incoming_set,
                            size_t cpusetsize,
                            struct hook_decision *d,
                            char *whybuf,
                            size_t whybuf_sz) {
    memset(d, 0, sizeof(*d));
    if (whybuf_sz) whybuf[0] = '\0';

    char errbuf[256];
    char default_str[512];

    /* Mode explicite MATCH */
    if (g_match_configured && g_match_spec && *g_match_spec) {
        int match = mask_matches_spec_exact(incoming_set, cpusetsize,
                                            g_match_spec, errbuf, sizeof(errbuf));
        if (match < 0) {
            snprintf(whybuf, whybuf_sz, "MATCH parse/check error: %s", errbuf);
            return;
        }

        if (!match) {
            snprintf(whybuf, whybuf_sz, "mask not matched");
            return;
        }

        d->matched = 1;

        if (g_block_enabled) {
            d->block = 1;
            snprintf(whybuf, whybuf_sz, "matched and blocked");
            return;
        }

        if (build_default_rewrite_cpuset(cpusetsize,
                                         &d->rewritten_set_storage,
                                         default_str, sizeof(default_str),
                                         errbuf, sizeof(errbuf)) == 0) {
            d->rewrite = 1;
            snprintf(whybuf, whybuf_sz,
                     "matched and rewritten using %s '%s'",
                     (g_rewrite_enabled && g_rewrite_spec && *g_rewrite_spec) ? "REWRITE" : "default full mask",
                     default_str);
            return;
        }

        snprintf(whybuf, whybuf_sz,
                 "matched but default rewrite build failed: %s", errbuf);
        return;
    }

    /* Mode implicite : pas de MATCH => si mask != full-machine-mask, rewrite */
    cpu_set_t fullset;
    char fullspec[64];
    if (build_full_machine_cpuset(cpusetsize, &fullset,
                                  fullspec, sizeof(fullspec),
                                  errbuf, sizeof(errbuf)) != 0) {
        snprintf(whybuf, whybuf_sz, "default full-mask build failed: %s", errbuf);
        return;
    }

    if (cpuset_equals_cpuset(incoming_set, &fullset, cpusetsize)) {
        snprintf(whybuf, whybuf_sz, "mask already equals full-machine-mask %s", fullspec);
        return;
    }

    d->matched = 1;

    if (build_default_rewrite_cpuset(cpusetsize,
                                     &d->rewritten_set_storage,
                                     default_str, sizeof(default_str),
                                     errbuf, sizeof(errbuf)) == 0) {
        d->rewrite = 1;
        snprintf(whybuf, whybuf_sz,
                 "no MATCH configured and non-full mask rewritten using %s '%s'",
                 (g_rewrite_enabled && g_rewrite_spec && *g_rewrite_spec) ? "REWRITE" : "default full mask",
                 default_str);
        return;
    }

    snprintf(whybuf, whybuf_sz,
             "no MATCH configured and default rewrite build failed: %s", errbuf);
}

/* ---------- Hook sched_setaffinity ---------- */

static int do_sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t *mask) {
    ensure_init();

    if (!real_sched_setaffinity) {
        errno = ENOSYS;
        return -1;
    }

    if (g_in_hook) {
        return real_sched_setaffinity(pid, cpusetsize, mask);
    }

    g_in_hook++;

    char orig[512];
    char newm[512];
    char why[256];

    cpuset_to_string(mask, cpusetsize, orig, sizeof(orig));

    struct hook_decision d;
    decide_for_mask(mask, cpusetsize, &d, why, sizeof(why));

    if (d.block) {
        safe_log("[affinity-hook] sched_setaffinity caller_pid=%ld caller_tid=%ld target_pid=%ld orig=%s action=block reason=\"%s\"\n",
                 (long)getpid(), gettid_linux(), (long)pid, orig, why);
        g_in_hook--;
        return 0;
    }

    if (d.rewrite) {
        cpuset_to_string(&d.rewritten_set_storage, cpusetsize, newm, sizeof(newm));
        int rc = real_sched_setaffinity(pid, cpusetsize, &d.rewritten_set_storage);
        int saved_errno = errno;
        safe_log("[affinity-hook] sched_setaffinity caller_pid=%ld caller_tid=%ld target_pid=%ld orig=%s rewritten=%s action=rewrite rc=%d errno=%d reason=\"%s\"\n",
                 (long)getpid(), gettid_linux(), (long)pid, orig, newm, rc, saved_errno, why);
        errno = saved_errno;
        g_in_hook--;
        return rc;
    }

    int rc = real_sched_setaffinity(pid, cpusetsize, mask);
    int saved_errno = errno;
    safe_log("[affinity-hook] sched_setaffinity caller_pid=%ld caller_tid=%ld target_pid=%ld orig=%s action=pass rc=%d errno=%d reason=\"%s\"\n",
             (long)getpid(), gettid_linux(), (long)pid, orig, rc, saved_errno, why);
    errno = saved_errno;
    g_in_hook--;
    return rc;
}

/* ---------- Hook pthread_setaffinity_np ---------- */

static int do_pthread_setaffinity_np(pthread_t thread, size_t cpusetsize, const cpu_set_t *mask) {
    ensure_init();

    if (!real_pthread_setaffinity_np) {
        return ENOSYS;
    }

    if (g_in_hook) {
        return real_pthread_setaffinity_np(thread, cpusetsize, mask);
    }

    g_in_hook++;

    char orig[512];
    char newm[512];
    char why[256];

    cpuset_to_string(mask, cpusetsize, orig, sizeof(orig));

    struct hook_decision d;
    decide_for_mask(mask, cpusetsize, &d, why, sizeof(why));

    if (d.block) {
        safe_log("[affinity-hook] pthread_setaffinity_np caller_pid=%ld caller_tid=%ld target_thread=%p orig=%s action=block reason=\"%s\"\n",
                 (long)getpid(), gettid_linux(), (void *)thread, orig, why);
        g_in_hook--;
        return 0;
    }

    if (d.rewrite) {
        cpuset_to_string(&d.rewritten_set_storage, cpusetsize, newm, sizeof(newm));
        int rc = real_pthread_setaffinity_np(thread, cpusetsize, &d.rewritten_set_storage);
        safe_log("[affinity-hook] pthread_setaffinity_np caller_pid=%ld caller_tid=%ld target_thread=%p orig=%s rewritten=%s action=rewrite rc=%d reason=\"%s\"\n",
                 (long)getpid(), gettid_linux(), (void *)thread, orig, newm, rc, why);
        g_in_hook--;
        return rc;
    }

    int rc = real_pthread_setaffinity_np(thread, cpusetsize, mask);
    safe_log("[affinity-hook] pthread_setaffinity_np caller_pid=%ld caller_tid=%ld target_thread=%p orig=%s action=pass rc=%d reason=\"%s\"\n",
             (long)getpid(), gettid_linux(), (void *)thread, orig, rc, why);
    g_in_hook--;
    return rc;
}

/* ---------- Symbols exportés ---------- */

int sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t *mask) {
    return do_sched_setaffinity(pid, cpusetsize, mask);
}

int pthread_setaffinity_np(pthread_t thread, size_t cpusetsize, const cpu_set_t *mask) {
    return do_pthread_setaffinity_np(thread, cpusetsize, mask);
}

int __sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t *mask) {
    return do_sched_setaffinity(pid, cpusetsize, mask);
}

int __pthread_setaffinity_np(pthread_t thread, size_t cpusetsize, const cpu_set_t *mask) {
    return do_pthread_setaffinity_np(thread, cpusetsize, mask);
}
