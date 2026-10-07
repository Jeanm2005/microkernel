#include <kernel/cmdline.h>

static int is_space(char c) { return c == ' ' || c == '\t'; }

bool cmdline_get(const char *cmdline, const char *key, char *out, size_t out_size)
{
    const char *p = cmdline;
    while (*p) {
        while (is_space(*p))
            p++;

        /* Does this word start with `key`, followed by '=' or its end? */
        const char *k = key, *w = p;
        while (*k && *w == *k) {
            k++;
            w++;
        }
        bool match = !*k && (*w == '=' || *w == '\0' || is_space(*w));

        if (match) {
            if (*w == '=')
                w++;
            size_t n = 0;
            while (w[n] && !is_space(w[n]) && n + 1 < out_size) {
                out[n] = w[n];
                n++;
            }
            if (out_size)
                out[n] = '\0';
            return true;
        }

        while (*p && !is_space(*p))   /* skip to the next word */
            p++;
    }
    return false;
}