#include "internal.h"
typedef struct line {
    size_t start, end, next;
} line;
static line line_at(const char *p, size_t n, size_t at) {
    line l = {at, at, at};
    while (l.end < n && p[l.end] != '\n')
        ++l.end;
    l.next = l.end < n ? l.end + 1U : l.end;
    if (l.end > at && p[l.end - 1U] == '\r')
        --l.end;
    return l;
}
static bool space(char c) { return c == ' ' || c == '\t'; }
static int hex(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
static bool emit(char *out, size_t cap, size_t *n, unsigned c) {
    unsigned char b[4];
    size_t k;
    if (c < 128U) {
        b[0] = (unsigned char)c;
        k = 1;
    } else if (c < 2048U) {
        b[0] = (unsigned char)(192U | (c >> 6U));
        b[1] = (unsigned char)(128U | (c & 63U));
        k = 2;
    } else if (c < 65536U) {
        b[0] = (unsigned char)(224U | (c >> 12U));
        b[1] = (unsigned char)(128U | ((c >> 6U) & 63U));
        b[2] = (unsigned char)(128U | (c & 63U));
        k = 3;
    } else {
        b[0] = (unsigned char)(240U | (c >> 18U));
        b[1] = (unsigned char)(128U | ((c >> 12U) & 63U));
        b[2] = (unsigned char)(128U | ((c >> 6U) & 63U));
        b[3] = (unsigned char)(128U | (c & 63U));
        k = 4;
    }
    if (k > cap - *n)
        return false;
    memcpy(out + *n, b, k);
    *n += k;
    return true;
}
static cog_result_t scalar(const char *p, size_t n, char *out, size_t cap, size_t *used) {
    while (n && space(*p)) {
        ++p;
        --n;
    }
    while (n && space(p[n - 1U]))
        --n;
    size_t w = 0;
    if (!n) {
        *used = 0;
        return COG_OK;
    }
    char quote = p[0];
    if (quote == '\'' || quote == '"') {
        size_t i = 1;
        bool closed = false;
        while (i < n) {
            char c = p[i++];
            if (c == quote) {
                if (quote == '\'' && i < n && p[i] == '\'') {
                    ++i;
                } else {
                    closed = true;
                    break;
                }
            }
            if (quote == '"' && c == '\\') {
                if (i == n)
                    return COG_ERROR_PROTOCOL;
                c = p[i++];
                switch (c) {
                case '"':
                case '\\':
                case '/':
                    break;
                case 'b':
                    c = '\b';
                    break;
                case 'f':
                    c = '\f';
                    break;
                case 'n':
                    c = '\n';
                    break;
                case 'r':
                    c = '\r';
                    break;
                case 't':
                    c = '\t';
                    break;
                case 'u': {
                    if (n - i < 4U)
                        return COG_ERROR_PROTOCOL;
                    unsigned u = 0;
                    for (size_t j = 0; j < 4U; ++j) {
                        int v = hex(p[i++]);
                        if (v < 0)
                            return COG_ERROR_PROTOCOL;
                        u = (u << 4U) | (unsigned)v;
                    }
                    if (u >= 0xd800U && u <= 0xdbffU) {
                        if (n - i < 6U || p[i] != '\\' || p[i + 1U] != 'u')
                            return COG_ERROR_PROTOCOL;
                        i += 2U;
                        unsigned lo = 0;
                        for (size_t j = 0; j < 4U; ++j) {
                            int v = hex(p[i++]);
                            if (v < 0)
                                return COG_ERROR_PROTOCOL;
                            lo = (lo << 4U) | (unsigned)v;
                        }
                        if (lo < 0xdc00U || lo > 0xdfffU)
                            return COG_ERROR_PROTOCOL;
                        u = 0x10000U + ((u - 0xd800U) << 10U) + (lo - 0xdc00U);
                    } else if (u >= 0xdc00U && u <= 0xdfffU)
                        return COG_ERROR_PROTOCOL;
                    if (u == 0U)
                        return COG_ERROR_PROTOCOL;
                    if (!emit(out, cap, &w, u))
                        return COG_ERROR_LIMIT;
                    continue;
                }
                default:
                    return COG_ERROR_PROTOCOL;
                }
            } else if ((unsigned char)c < 32U && c != '\t')
                return COG_ERROR_PROTOCOL;
            if (w == cap)
                return COG_ERROR_LIMIT;
            out[w++] = c;
        }
        if (!closed)
            return COG_ERROR_PROTOCOL;
        if (i < n && !space(p[i]))
            return COG_ERROR_PROTOCOL;
        while (i < n && space(p[i]))
            ++i;
        if (i < n && p[i] != '#')
            return COG_ERROR_PROTOCOL;
    } else {
        if (strchr("&*![]{}|>%@`", quote) != NULL ||
            ((quote == '-' || quote == '?' || quote == ':') && (n == 1U || space(p[1]))))
            return COG_ERROR_UNSUPPORTED;
        for (size_t i = 0; i < n; ++i) {
            if (p[i] == '#' && (i == 0U || space(p[i - 1U])))
                break;
            if (p[i] == ':' && i + 1U < n && space(p[i + 1U]))
                return COG_ERROR_PROTOCOL;
            if ((unsigned char)p[i] < 32U && p[i] != '\t')
                return COG_ERROR_PROTOCOL;
            if (w == cap)
                return COG_ERROR_LIMIT;
            out[w++] = p[i];
        }
        while (w && space(out[w - 1U]))
            --w;
    }
    if (!ci_text(out, w, NULL))
        return COG_ERROR_PROTOCOL;
    *used = w;
    return COG_OK;
}
static cog_result_t number(const char *s, uint64_t max, uint64_t *out) {
    if (s == NULL || *s == '\0')
        return COG_ERROR_PROTOCOL;
    uint64_t n = 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9')
            return COG_ERROR_PROTOCOL;
        unsigned d = (unsigned)(*s - '0');
        if (n > (max - d) / 10U)
            return COG_ERROR_LIMIT;
        n = n * 10U + d;
    }
    if (n == 0)
        return COG_ERROR_PROTOCOL;
    *out = n;
    return COG_OK;
}
static bool keyeq(const char *p, size_t n, const char *key) {
    return strlen(key) == n && memcmp(p, key, n) == 0;
}
static size_t colon(const char *p, size_t n) {
    char q = 0;
    for (size_t i = 0; i < n; ++i) {
        char c = p[i];
        if (q) {
            if (q == '"' && c == '\\') {
                if (i + 1U < n)
                    ++i;
            } else if (c == q) {
                if (q == '\'' && i + 1U < n && p[i + 1U] == q)
                    ++i;
                else
                    q = 0;
            }
        } else if (c == '"' || c == '\'')
            q = c;
        else if (c == ':')
            return i;
    }
    return n;
}
cog_result_t ci_parse(ci_arena *a, const cog_instruction_limits_t *lim,
                      const cog_instruction_source_t *s, cog_instruction_entry_t *e, size_t *body,
                      size_t *bad) {
    const char *p = s->text;
    size_t n = s->text_bytes;
    line first = line_at(p, n, 0);
    *bad = 0;
    if (first.end != 3U || memcmp(p, "---", 3U) != 0 || first.next == first.end)
        return COG_ERROR_PROTOCOL;
    size_t end = first.next, after = 0;
    while (end < n) {
        line l = line_at(p, n, end);
        if (l.end - l.start == 3U && memcmp(p + l.start, "---", 3U) == 0) {
            after = l.next;
            break;
        }
        end = l.next;
    }
    if (after == 0U)
        return COG_ERROR_PROTOCOL;
    if (end - first.next > lim->max_metadata_bytes)
        return COG_ERROR_LIMIT;
    char *tmp = malloc(lim->max_string_bytes);
    char *map = malloc(lim->max_metadata_bytes + 1U);
    if (tmp == NULL || map == NULL) {
        free(tmp);
        free(map);
        return COG_ERROR_NOMEM;
    }
    size_t map_used = 0;
    unsigned seen = 0;
    bool metadata = false;
    size_t at = first.next;
    bool agent = s->kind == COG_INSTRUCTION_AGENT;
    cog_result_t rc = COG_OK;
    *e = (cog_instruction_entry_t){.kind = s->kind, .root_id = s->root_id, .path = s->path};
    static const char *const keys[] = {
        "name",           "description",   "license",           "compatibility",
        "metadata",       "allowed-tools", "cog-agent",         "model",
        "tools",          "skills",        "max-context-bytes", "max-output-tokens",
        "max-tool-rounds"};
    while (at < end) {
        line l = line_at(p, n, at);
        *bad = at;
        at = l.next;
        size_t begin = l.start;
        while (begin < l.end && p[begin] == ' ')
            ++begin;
        if (begin == l.end || p[begin] == '#')
            continue;
        size_t indent = begin - l.start;
        if (indent != 0U && (!metadata || indent != 2U)) {
            rc = COG_ERROR_UNSUPPORTED;
            break;
        }
        size_t k = colon(p + begin, l.end - begin);
        if (k == l.end - begin || k == 0U) {
            rc = COG_ERROR_PROTOCOL;
            break;
        }
        size_t ke = k;
        while (ke && space(p[begin + ke - 1U]))
            --ke;
        size_t val = begin + k + 1U;
        if (val < l.end && !space(p[val])) {
            rc = COG_ERROR_PROTOCOL;
            break;
        }
        while (val < l.end && space(p[val]))
            ++val;
        unsigned index = 0;
        if (indent == 0U) {
            metadata = false;
            for (; index < 13U; ++index)
                if (keyeq(p + begin, ke, keys[index]))
                    break;
            if (index == 13U) {
                rc = COG_ERROR_UNSUPPORTED;
                break;
            }
            if ((seen & (1U << index)) != 0U) {
                rc = COG_ERROR_PROTOCOL;
                break;
            }
            seen |= 1U << index;
            if ((agent && (index >= 2U && index <= 5U)) || (!agent && index >= 6U)) {
                rc = COG_ERROR_UNSUPPORTED;
                break;
            }
            if (index == 4U) {
                if (val != l.end && p[val] != '#') {
                    rc = COG_ERROR_UNSUPPORTED;
                    break;
                }
                metadata = true;
                continue;
            }
        } else {
            size_t kw = 0;
            rc = scalar(p + begin, ke, tmp, lim->max_string_bytes - 1U, &kw);
            if (rc != COG_OK)
                break;
            if (kw == 0U) {
                rc = COG_ERROR_PROTOCOL;
                break;
            }
            tmp[kw] = '\0';
            for (size_t j = 0; j < map_used; j += strlen(map + j) + 1U)
                if (strcmp(map + j, tmp) == 0) {
                    rc = COG_ERROR_PROTOCOL;
                    break;
                }
            if (rc != COG_OK)
                break;
            if (kw + 1U > lim->max_metadata_bytes + 1U - map_used) {
                rc = COG_ERROR_LIMIT;
                break;
            }
            memcpy(map + map_used, tmp, kw + 1U);
            map_used += kw + 1U;
        }
        size_t w = 0;
        if (val < l.end && (p[val] == '|' || p[val] == '>')) {
            char fold = p[val];
            size_t tail = val + 1U;
            while (tail < l.end && space(p[tail]))
                ++tail;
            if (tail < l.end && p[tail] != '#') {
                rc = COG_ERROR_UNSUPPORTED;
                break;
            }
            bool prior = false;
            bool prior_more = false;
            size_t pending_blank = 0;
            unsigned block_indent = (unsigned)indent + 2U;
            while (at < end) {
                line b = line_at(p, n, at);
                size_t pos = b.start;
                while (pos < b.end && p[pos] == ' ')
                    ++pos;
                bool blank = pos == b.end;
                if (!blank && pos - b.start < block_indent)
                    break;
                at = b.next;
                if (blank) {
                    ++pending_blank;
                    continue;
                }
                bool more = pos - b.start > block_indent;
                size_t count = b.end - b.start - block_indent;
                if (prior) {
                    size_t breaks = (fold == '|' || prior_more || more)
                                        ? pending_blank + 1U
                                        : (pending_blank ? pending_blank : 1U);
                    char sep =
                        fold == '>' && pending_blank == 0U && !prior_more && !more ? ' ' : '\n';
                    if (breaks > lim->max_string_bytes - 1U - w) {
                        rc = COG_ERROR_LIMIT;
                        break;
                    }
                    for (size_t j = 0; j < breaks; ++j)
                        tmp[w++] = sep;
                } else {
                    if (pending_blank > lim->max_string_bytes - 1U - w) {
                        rc = COG_ERROR_LIMIT;
                        break;
                    }
                    while (pending_blank) {
                        tmp[w++] = '\n';
                        --pending_blank;
                    }
                }
                pending_blank = 0;
                if (count > lim->max_string_bytes - 1U - w) {
                    rc = COG_ERROR_LIMIT;
                    break;
                }
                memcpy(tmp + w, p + b.start + block_indent, count);
                w += count;
                prior = true;
                prior_more = more;
            }
            if (rc != COG_OK)
                break;
            if (prior || pending_blank) {
                if (w == lim->max_string_bytes - 1U) {
                    rc = COG_ERROR_LIMIT;
                    break;
                }
                tmp[w++] = '\n';
            }
        } else
            rc = scalar(p + val, l.end - val, tmp, lim->max_string_bytes - 1U, &w);
        if (rc != COG_OK)
            break;
        tmp[w] = '\0';
        if (indent != 0U) { /* Validate and retain metadata strings in the catalog arena. */
            if (ci_copy(a, tmp, w) == NULL) {
                rc = COG_ERROR_LIMIT;
                break;
            }
            continue;
        }
        size_t chars = 0;
        if (!ci_text(tmp, w, &chars)) {
            rc = COG_ERROR_PROTOCOL;
            break;
        }
        if ((index == 1U && (chars == 0U || chars > 1024U)) || (index == 3U && chars > 500U)) {
            rc = COG_ERROR_PROTOCOL;
            break;
        }
        if (index >= 6U && (index == 6U || index >= 10U)) {
            uint64_t v = 0;
            rc = number(tmp, index == 10U ? (uint64_t)SIZE_MAX : UINT32_MAX, &v);
            if (rc != COG_OK)
                break;
            if (index == 6U) {
                if (v != 1U) {
                    rc = COG_ERROR_UNSUPPORTED;
                    break;
                }
            } else if (index == 10U)
                e->context_byte_limit = (size_t)v;
            else if (index == 11U)
                e->output_token_limit = (uint32_t)v;
            else
                e->tool_round_limit = (uint32_t)v;
            continue;
        }
        char *copy = ci_copy(a, tmp, w);
        if (copy == NULL) {
            rc = COG_ERROR_LIMIT;
            break;
        }
        switch (index) {
        case 0:
            e->name = copy;
            break;
        case 1:
            e->description = copy;
            break;
        case 5:
        case 8:
            e->tool_names = copy;
            break;
        case 7:
            e->model_hint = copy;
            break;
        case 9:
            e->skill_names = copy;
            break;
        default:
            break;
        }
    }
    free(tmp);
    free(map);
    if (rc != COG_OK)
        return rc;
    if ((seen & 3U) != 3U || (agent && (seen & (1U << 6U)) == 0U) || !ci_name(e->name))
        return COG_ERROR_PROTOCOL;
    const char *base = strrchr(s->path, '/');
    base = base ? base + 1 : s->path;
    size_t namelen = strlen(e->name);
    if (agent) {
        if (strlen(base) != namelen + 3U || memcmp(base, e->name, namelen) != 0 ||
            strcmp(base + namelen, ".md") != 0)
            return COG_ERROR_PROTOCOL;
    } else {
        if (strcmp(base, "SKILL.md") != 0 || base == s->path)
            return COG_ERROR_PROTOCOL;
        const char *finish = base - 1, *start = finish;
        while (start > s->path && start[-1] != '/')
            --start;
        if ((size_t)(finish - start) != namelen || memcmp(start, e->name, namelen) != 0)
            return COG_ERROR_PROTOCOL;
    }
    ci_hash(p, n, e->sha256);
    *body = after;
    return COG_OK;
}
