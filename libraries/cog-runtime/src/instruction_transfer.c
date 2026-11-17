/** @file instruction_transfer.c
 * @brief Transactional native instruction bootstrap and resource body codecs.
 * No transport, store, provider, filesystem, policy or allocation effects.
 */
#include "instruction_transfer.h"
#include "instruction_hash.h"
#include <stdbool.h>
#include <string.h>

typedef struct span {
  const char *data;
  size_t size;
} span;
typedef struct cursor {
  const unsigned char *input;
  size_t size, at;
  cog_result_t error;
} cursor;
typedef struct writer {
  unsigned char *output;
  size_t at;
} writer;
static bool zeros(const unsigned char *p, size_t n) {
  for (size_t i = 0U; i < n; ++i)
    if (p[i] != 0U)
      return false;
  return true;
}
static bool identity_zero(const cog_cli_instruction_identity_t *i) {
  return i->catalog_generation == 0U && i->snapshot_id == 0U &&
         zeros(i->sha256, 32U);
}
static bool identity_present(const cog_cli_instruction_identity_t *i) {
  return i->catalog_generation != 0U && i->snapshot_id != 0U;
}
static bool identity_equal(const cog_cli_instruction_identity_t *a,
                           const cog_cli_instruction_identity_t *b) {
  return a->catalog_generation == b->catalog_generation &&
         a->snapshot_id == b->snapshot_id &&
         memcmp(a->sha256, b->sha256, 32U) == 0;
}
static bool capsule_zero(const cog_cli_instruction_capsule_t *c) {
  return identity_zero(&c->identity) && c->bytes == NULL && c->length == 0U;
}
static bool sum(size_t *n, size_t add) {
  if (add > SIZE_MAX - *n)
    return false;
  *n += add;
  return true;
}
static bool utf8(span s) {
  if (s.data == NULL)
    return false;
  unsigned remaining = 0U;
  uint32_t value = 0U, minimum = 0U;
  for (size_t i = 0U; i < s.size; ++i) {
    const unsigned c = (unsigned char)s.data[i];
    if (remaining == 0U) {
      if (c == 0U)
        return false;
      if (c < 128U)
        continue;
      if (c >= 194U && c <= 223U) {
        value = c & 31U;
        minimum = 128U;
        remaining = 1U;
      } else if (c >= 224U && c <= 239U) {
        value = c & 15U;
        minimum = 2048U;
        remaining = 2U;
      } else if (c >= 240U && c <= 244U) {
        value = c & 7U;
        minimum = 65536U;
        remaining = 3U;
      } else
        return false;
    } else {
      if ((c & 192U) != 128U)
        return false;
      value = (value << 6U) | (c & 63U);
      --remaining;
      if (remaining == 0U && (value < minimum || value > 1114111U ||
                              (value >= 55296U && value <= 57343U)))
        return false;
    }
  }
  return remaining == 0U;
}
static bool root_valid(span s) {
  if (s.size == 0U || !utf8(s))
    return false;
  for (size_t i = 0U; i < s.size; ++i)
    if (s.data[i] == ':' || s.data[i] == '/')
      return false;
  return true;
}
static bool name_valid(span s) {
  if (s.data == NULL || s.size == 0U || s.size > 64U || s.data[0] == '-' ||
      s.data[s.size - 1U] == '-')
    return false;
  for (size_t i = 0U; i < s.size; ++i) {
    const unsigned char c = (unsigned char)s.data[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') ||
        (c == '-' && i != 0U && s.data[i - 1U] == '-'))
      return false;
  }
  return true;
}
static bool path_valid(span s, bool empty) {
  if (!utf8(s))
    return false;
  if (s.size == 0U)
    return empty;
  if (s.data[0] == '/' || s.data[s.size - 1U] == '/')
    return false;
  size_t begin = 0U;
  for (size_t i = 0U; i <= s.size; ++i)
    if (i == s.size || s.data[i] == '/') {
      const size_t n = i - begin;
      if (n == 0U || (n == 1U && s.data[begin] == '.') ||
          (n == 2U && s.data[begin] == '.' && s.data[begin + 1U] == '.'))
        return false;
      begin = i + 1U;
    }
  return true;
}
static uint32_t little32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) |
         ((uint32_t)p[3] << 24U);
}
static cog_result_t capsule_valid(const cog_cli_instruction_capsule_t *c,
                                  bool allow_empty) {
  if (c->length == 0U)
    return allow_empty && capsule_zero(c) ? COG_OK : COG_ERROR_PROTOCOL;
  if (c->length > UINT32_MAX)
    return COG_ERROR_LIMIT;
  if (c->bytes == NULL || !identity_present(&c->identity) || c->length < 12U)
    return COG_ERROR_PROTOCOL;
  const unsigned char *p = c->bytes;
  if (memcmp(p, "CGIM", 4U) != 0)
    return COG_ERROR_PROTOCOL;
  if (little32(p + 4U) != 1U || little32(p + 8U) != 1U)
    return COG_ERROR_UNSUPPORTED;
  unsigned char hash[32];
  nt_hash(c->bytes, c->length, hash);
  return memcmp(hash, c->identity.sha256, 32U) == 0 ? COG_OK
                                                    : COG_ERROR_PROTOCOL;
}
static const unsigned char *read_bytes(cursor *r, size_t n) {
  if (r->error != COG_OK)
    return NULL;
  if (n > r->size - r->at) {
    r->error = COG_ERROR_PROTOCOL;
    return NULL;
  }
  const unsigned char *p = r->input + r->at;
  r->at += n;
  return p;
}
static uint8_t read8(cursor *r) {
  const unsigned char *p = read_bytes(r, 1U);
  return p == NULL ? 0U : p[0];
}
static uint16_t read16(cursor *r) {
  const unsigned char *p = read_bytes(r, 2U);
  if (p == NULL) return 0U;
  return (uint16_t)(((uint16_t)p[0] << 8U) | p[1]);
}
static uint32_t read32(cursor *r) {
  const unsigned char *p = read_bytes(r, 4U);
  uint32_t n = 0U;
  if (p != NULL)
    for (size_t i = 0U; i < 4U; ++i)
      n = (n << 8U) | p[i];
  return n;
}
static uint64_t read64(cursor *r) {
  const unsigned char *p = read_bytes(r, 8U);
  uint64_t n = 0U;
  if (p != NULL)
    for (size_t i = 0U; i < 8U; ++i)
      n = (n << 8U) | p[i];
  return n;
}
static span read_string(cursor *r) {
  const uint32_t size = read32(r);
#if SIZE_MAX < UINT32_MAX
  if (size > SIZE_MAX) {
    r->error = COG_ERROR_LIMIT;
    return (span){0};
  }
#endif
  const char *data = (const char *)read_bytes(r, (size_t)size);
  return (span){data, (size_t)size};
}
static cog_cli_instruction_identity_t read_identity(cursor *r) {
  cog_cli_instruction_identity_t i = {0};
  i.catalog_generation = read64(r);
  i.snapshot_id = read64(r);
  const unsigned char *p = read_bytes(r, 32U);
  if (p != NULL)
    memcpy(i.sha256, p, 32U);
  return i;
}
static void write_bytes(writer *w, const void *p, size_t n) {
  if (n != 0U)
    memcpy(w->output + w->at, p, n);
  w->at += n;
}
static void write8(writer *w, uint8_t n) { write_bytes(w, &n, 1U); }
static void write16(writer *w, uint16_t n) {
  const unsigned char b[2] = {(unsigned char)(n >> 8U), (unsigned char)n};
  write_bytes(w, b, 2U);
}
static void write32(writer *w, uint32_t n) {
  unsigned char b[4];
  for (size_t i = 0U; i < 4U; ++i)
    b[3U - i] = (unsigned char)(n >> (8U * i));
  write_bytes(w, b, 4U);
}
static void write64(writer *w, uint64_t n) {
  unsigned char b[8];
  for (size_t i = 0U; i < 8U; ++i)
    b[7U - i] = (unsigned char)(n >> (8U * i));
  write_bytes(w, b, 8U);
}
static void write_identity(writer *w, const cog_cli_instruction_identity_t *i) {
  write64(w, i->catalog_generation);
  write64(w, i->snapshot_id);
  write_bytes(w, i->sha256, 32U);
}
static void write_string(writer *w, span s) {
  write32(w, (uint32_t)s.size);
  write_bytes(w, s.data, s.size);
}
static cog_result_t bootstrap_shape(const cog_cli_instruction_bootstrap_t *b,
                                    size_t *wire, size_t *storage) {
  if (b->configuration_bytes > UINT32_MAX ||
      b->instructions.length > UINT32_MAX)
    return COG_ERROR_LIMIT;
  if (b->configuration == NULL || b->configuration_bytes < 4U)
    return COG_ERROR_PROTOCOL;
  if (memcmp(b->configuration, "CGB1", 4U) != 0)
    return memcmp(b->configuration, "CGB", 3U) == 0 ? COG_ERROR_UNSUPPORTED
                                                    : COG_ERROR_PROTOCOL;
  cog_result_t rc = capsule_valid(&b->instructions, true);
  if (rc != COG_OK)
    return rc;
  size_t n = b->configuration_bytes;
  if (!sum(&n, b->instructions.length))
    return COG_ERROR_LIMIT;
  *storage = n;
  if (!sum(&n, 60U))
    return COG_ERROR_LIMIT;
  *wire = n;
  return COG_OK;
}
cog_result_t
cog_cli_instruction_bootstrap_encode(const cog_cli_instruction_bootstrap_t *b,
                                     void *output, size_t capacity,
                                     size_t *required) {
  if (b == NULL || required == NULL || (output == NULL && capacity != 0U))
    return COG_ERROR_NULL_PTR;
  size_t size = 0U, storage = 0U;
  cog_result_t rc = bootstrap_shape(b, &size, &storage);
  (void)storage;
  if (rc != COG_OK)
    return rc == COG_ERROR_LIMIT ? rc : COG_ERROR_INVALID_ARG;
  if (capacity < size) {
    *required = size;
    return COG_ERROR_OVERFLOW;
  }
  writer w = {output, 0U};
  write_bytes(&w, "CGB2", 4U);
  write32(&w, (uint32_t)b->configuration_bytes);
  write_bytes(&w, b->configuration, b->configuration_bytes);
  write_identity(&w, &b->instructions.identity);
  write32(&w, (uint32_t)b->instructions.length);
  write_bytes(&w, b->instructions.bytes, b->instructions.length);
  *required = w.at;
  return COG_OK;
}
cog_result_t cog_cli_instruction_bootstrap_decode(
    const void *input, size_t length, cog_cli_instruction_bootstrap_t *b,
    void *storage, size_t capacity, size_t *required) {
  if (input == NULL || b == NULL || required == NULL ||
      (storage == NULL && capacity != 0U))
    return COG_ERROR_NULL_PTR;
  cursor r = {input, length, 0U, COG_OK};
  const unsigned char *magic = read_bytes(&r, 4U);
  if (magic == NULL)
    return r.error;
  if (memcmp(magic, "CGB2", 4U) != 0)
    return memcmp(magic, "CGB", 3U) == 0 ? COG_ERROR_UNSUPPORTED
                                         : COG_ERROR_PROTOCOL;
  cog_cli_instruction_bootstrap_t value = {0};
  value.configuration_bytes = read32(&r);
  value.configuration = read_bytes(&r, value.configuration_bytes);
  value.instructions.identity = read_identity(&r);
  value.instructions.length = read32(&r);
  value.instructions.bytes = read_bytes(&r, value.instructions.length);
  if (value.instructions.length == 0U)
    value.instructions.bytes = NULL;
  if (r.error != COG_OK)
    return r.error;
  if (r.at != r.size)
    return COG_ERROR_PROTOCOL;
  size_t wire = 0U, need = 0U;
  cog_result_t rc = bootstrap_shape(&value, &wire, &need);
  if (rc != COG_OK)
    return rc;
  if (capacity < need) {
    *required = need;
    return COG_ERROR_OVERFLOW;
  }
  unsigned char *p = storage;
  memcpy(p, value.configuration, value.configuration_bytes);
  value.configuration = p;
  if (value.instructions.length != 0U) {
    memcpy(p + value.configuration_bytes, value.instructions.bytes,
           value.instructions.length);
    value.instructions.bytes = p + value.configuration_bytes;
  }
  *b = value;
  *required = need;
  return COG_OK;
}

typedef struct resource_view {
  cog_cli_instruction_resource_t value;
  span root, skill, path, call;
} resource_view;
static bool status_code(cog_result_t result, uint8_t *code) {
  static const cog_result_t statuses[] = {COG_OK,
                                          COG_ERROR_NOT_FOUND,
                                          COG_ERROR_DENIED,
                                          COG_ERROR_LIMIT,
                                          COG_ERROR_IO,
                                          COG_ERROR_ABORTED,
                                          COG_ERROR_TIMEOUT,
                                          COG_ERROR_UNSUPPORTED,
                                          COG_ERROR_PROTOCOL};
  for (size_t i = 0U; i < sizeof(statuses) / sizeof(statuses[0]); ++i)
    if (result == statuses[i]) {
      *code = (uint8_t)i;
      return true;
    }
  return false;
}
static cog_result_t decode_status(uint8_t code, cog_result_t *status) {
  static const cog_result_t statuses[] = {COG_OK,
                                          COG_ERROR_NOT_FOUND,
                                          COG_ERROR_DENIED,
                                          COG_ERROR_LIMIT,
                                          COG_ERROR_IO,
                                          COG_ERROR_ABORTED,
                                          COG_ERROR_TIMEOUT,
                                          COG_ERROR_UNSUPPORTED,
                                          COG_ERROR_PROTOCOL};
  if (code >= sizeof(statuses) / sizeof(statuses[0]))
    return COG_ERROR_PROTOCOL;
  *status = statuses[code];
  return COG_OK;
}
static cog_result_t resource_shape(const resource_view *v, size_t *wire,
                                   size_t *storage) {
  const cog_cli_instruction_resource_t *r = &v->value;
  if (!identity_present(&r->context) ||
      r->kind < COG_CLI_INSTRUCTION_RESOURCE_REQUEST ||
      r->kind > COG_CLI_INSTRUCTION_RESOURCE_READY)
    return COG_ERROR_PROTOCOL;
  size_t n = 54U, copy = 0U;
  if (r->kind == COG_CLI_INSTRUCTION_RESOURCE_REQUEST) {
    if (r->operation < COG_CLI_INSTRUCTION_RESOURCE_LOAD ||
        r->operation > COG_CLI_INSTRUCTION_RESOURCE_SCOPE ||
        !root_valid(v->root) || !utf8(v->skill) ||
        !path_valid(v->path,
                    r->operation != COG_CLI_INSTRUCTION_RESOURCE_LOAD) ||
        !utf8(v->call) || v->call.size == 0U || v->call.size > 255U ||
        r->assistant_node_id == 0U || r->request_checkpoint_generation == 0U ||
        r->call_ordinal > 15U || r->result != COG_OK ||
        !capsule_zero(&r->candidate) ||
        r->outcome != COG_CLI_INSTRUCTION_COMMIT_NONE ||
        r->checkpoint_generation != 0U)
      return COG_ERROR_PROTOCOL;
    if (r->operation == COG_CLI_INSTRUCTION_RESOURCE_SCOPE) {
      if (v->skill.size != 0U)
        return COG_ERROR_PROTOCOL;
    } else if (!name_valid(v->skill) ||
               (r->operation == COG_CLI_INSTRUCTION_RESOURCE_ACTIVATE &&
                v->path.size != 0U))
      return COG_ERROR_PROTOCOL;
    const span strings[4] = {v->root, v->skill, v->path, v->call};
    for (size_t i = 0U; i < 4U; ++i) {
      if (strings[i].size > UINT32_MAX)
        return COG_ERROR_LIMIT;
      if (!sum(&n, strings[i].size) || !sum(&copy, strings[i].size) ||
          !sum(&copy, 1U))
        return COG_ERROR_LIMIT;
    }
    if (!sum(&n, 35U))
      return COG_ERROR_LIMIT; /* operation + four lengths +
                                 node/ordinal/checkpoint */
  } else {
    if (r->operation != 0 || r->root_id != NULL || r->skill_name != NULL ||
        r->path != NULL || r->assistant_node_id != 0U || r->call_id != NULL ||
        r->call_ordinal != 0U || r->request_checkpoint_generation != 0U)
      return COG_ERROR_PROTOCOL;
    if (r->kind == COG_CLI_INSTRUCTION_RESOURCE_RESULT) {
      uint8_t code = 0U;
      if (!status_code(r->result, &code) ||
          r->outcome != COG_CLI_INSTRUCTION_COMMIT_NONE ||
          r->checkpoint_generation != 0U)
        return COG_ERROR_PROTOCOL;
      ++n;
      if (code == 0U) {
        cog_result_t rc = capsule_valid(&r->candidate, false);
        if (rc != COG_OK)
          return rc;
        if (r->candidate.identity.catalog_generation !=
                r->context.catalog_generation ||
            (r->candidate.identity.snapshot_id == r->context.snapshot_id &&
             !identity_equal(&r->candidate.identity, &r->context)))
          return COG_ERROR_PROTOCOL;
        if (!sum(&n, 52U) || !sum(&n, r->candidate.length))
          return COG_ERROR_LIMIT;
        copy = r->candidate.length;
      } else if (!capsule_zero(&r->candidate))
        return COG_ERROR_PROTOCOL;
    } else if (r->kind == COG_CLI_INSTRUCTION_RESOURCE_CONSUMED) {
      if (r->result != COG_OK || !capsule_zero(&r->candidate) ||
          r->outcome != COG_CLI_INSTRUCTION_COMMIT_NONE ||
          r->checkpoint_generation != 0U)
        return COG_ERROR_PROTOCOL;
    } else {
      if (r->result != COG_OK || r->candidate.bytes != NULL ||
          r->candidate.length != 0U ||
          r->outcome < COG_CLI_INSTRUCTION_COMMIT_SAVED ||
          r->outcome > COG_CLI_INSTRUCTION_COMMIT_UNCHANGED)
        return COG_ERROR_PROTOCOL;
      const bool present = identity_present(&r->candidate.identity),
                 absent = identity_zero(&r->candidate.identity);
      if ((!present && !absent) ||
          (present && r->candidate.identity.catalog_generation !=
                          r->context.catalog_generation))
        return COG_ERROR_PROTOCOL;
      if (r->outcome == COG_CLI_INSTRUCTION_COMMIT_SAVED ||
          r->outcome == COG_CLI_INSTRUCTION_COMMIT_UNCERTAIN) {
        if (!present ||
            r->candidate.identity.snapshot_id == r->context.snapshot_id)
          return COG_ERROR_PROTOCOL;
        if ((r->outcome == COG_CLI_INSTRUCTION_COMMIT_SAVED) !=
            (r->checkpoint_generation != 0U))
          return COG_ERROR_PROTOCOL;
      } else {
        if (r->checkpoint_generation != 0U)
          return COG_ERROR_PROTOCOL;
        if (r->outcome == COG_CLI_INSTRUCTION_COMMIT_UNCHANGED &&
            !identity_equal(&r->candidate.identity, &r->context))
          return COG_ERROR_PROTOCOL;
      }
      n += 57U;
    }
  }
  *wire = n;
  *storage = copy;
  return COG_OK;
}
static span cstring(const char *s) {
  return (span){s, s == NULL ? 0U : strlen(s)};
}
cog_result_t
cog_cli_instruction_resource_encode(const cog_cli_instruction_resource_t *r,
                                    void *output, size_t capacity,
                                    size_t *required) {
  if (r == NULL || required == NULL || (output == NULL && capacity != 0U))
    return COG_ERROR_NULL_PTR;
  const resource_view v = {*r, cstring(r->root_id), cstring(r->skill_name),
                           cstring(r->path), cstring(r->call_id)};
  size_t need = 0U, storage = 0U;
  cog_result_t rc = resource_shape(&v, &need, &storage);
  (void)storage;
  if (rc != COG_OK)
    return rc == COG_ERROR_LIMIT ? rc : COG_ERROR_INVALID_ARG;
  if (capacity < need) {
    *required = need;
    return COG_ERROR_OVERFLOW;
  }
  writer w = {output, 0U};
  write_bytes(&w, "CGIR", 4U);
  write8(&w, 2U);
  write8(&w, (uint8_t)r->kind);
  write_identity(&w, &r->context);
  if (r->kind == COG_CLI_INSTRUCTION_RESOURCE_REQUEST) {
    write8(&w, (uint8_t)r->operation);
    write_string(&w, v.root);
    write_string(&w, v.skill);
    write_string(&w, v.path);
    write64(&w, r->assistant_node_id);
    write16(&w, r->call_ordinal);
    write_string(&w, v.call);
    write64(&w, r->request_checkpoint_generation);
  } else if (r->kind == COG_CLI_INSTRUCTION_RESOURCE_RESULT) {
    uint8_t code = 0U;
    (void)status_code(r->result, &code);
    write8(&w, code);
    if (code == 0U) {
      write_identity(&w, &r->candidate.identity);
      write32(&w, (uint32_t)r->candidate.length);
      write_bytes(&w, r->candidate.bytes, r->candidate.length);
    }
  } else if (r->kind == COG_CLI_INSTRUCTION_RESOURCE_COMMIT ||
             r->kind == COG_CLI_INSTRUCTION_RESOURCE_READY) {
    write_identity(&w, &r->candidate.identity);
    write8(&w, (uint8_t)r->outcome);
    write64(&w, r->checkpoint_generation);
  }
  *required = w.at;
  return COG_OK;
}
static char *copy_string(unsigned char *storage, size_t *at, span s) {
  char *out = (char *)storage + *at;
  if (s.size != 0U)
    memcpy(out, s.data, s.size);
  out[s.size] = '\0';
  *at += s.size + 1U;
  return out;
}
cog_result_t cog_cli_instruction_resource_decode(
    const void *input, size_t length, cog_cli_instruction_resource_t *out,
    void *storage, size_t capacity, size_t *required) {
  if (input == NULL || out == NULL || required == NULL ||
      (storage == NULL && capacity != 0U))
    return COG_ERROR_NULL_PTR;
  cursor c = {input, length, 0U, COG_OK};
  const unsigned char *magic = read_bytes(&c, 4U);
  if (magic == NULL)
    return c.error;
  if (memcmp(magic, "CGIR", 4U) != 0)
    return COG_ERROR_PROTOCOL;
  const uint8_t version = read8(&c);
  if (c.error != COG_OK)
    return c.error;
  if (version != 2U)
    return COG_ERROR_UNSUPPORTED;
  resource_view v = {0};
  cog_cli_instruction_resource_t *r = &v.value;
  const uint8_t kind = read8(&c);
  if (c.error != COG_OK)
    return c.error;
  if (kind < 1U || kind > 5U)
    return COG_ERROR_PROTOCOL;
  r->kind = (cog_cli_instruction_resource_kind_t)kind;
  r->context = read_identity(&c);
  if (r->kind == COG_CLI_INSTRUCTION_RESOURCE_REQUEST) {
    r->operation = (cog_cli_instruction_resource_operation_t)read8(&c);
    v.root = read_string(&c);
    v.skill = read_string(&c);
    v.path = read_string(&c);
    r->root_id = v.root.data;
    r->skill_name = v.skill.data;
    r->path = v.path.data;
    r->assistant_node_id = read64(&c);
    r->call_ordinal = read16(&c);
    v.call = read_string(&c);
    r->call_id = v.call.data;
    r->request_checkpoint_generation = read64(&c);
  } else if (r->kind == COG_CLI_INSTRUCTION_RESOURCE_RESULT) {
    const uint8_t code = read8(&c);
    if (c.error != COG_OK)
      return c.error;
    if (decode_status(code, &r->result) != COG_OK)
      return COG_ERROR_PROTOCOL;
    if (code == 0U) {
      r->candidate.identity = read_identity(&c);
      r->candidate.length = read32(&c);
      r->candidate.bytes = read_bytes(&c, r->candidate.length);
    }
  } else if (r->kind == COG_CLI_INSTRUCTION_RESOURCE_COMMIT ||
             r->kind == COG_CLI_INSTRUCTION_RESOURCE_READY) {
    r->candidate.identity = read_identity(&c);
    r->outcome = (cog_cli_instruction_commit_outcome_t)read8(&c);
    r->checkpoint_generation = read64(&c);
  }
  if (c.error != COG_OK)
    return c.error;
  if (c.at != c.size)
    return COG_ERROR_PROTOCOL;
  size_t wire = 0U, need = 0U;
  cog_result_t rc = resource_shape(&v, &wire, &need);
  if (rc != COG_OK)
    return rc;
  if (capacity < need) {
    *required = need;
    return COG_ERROR_OVERFLOW;
  }
  unsigned char *p = storage;
  size_t at = 0U;
  if (r->kind == COG_CLI_INSTRUCTION_RESOURCE_REQUEST) {
    r->root_id = copy_string(p, &at, v.root);
    r->skill_name = copy_string(p, &at, v.skill);
    r->path = copy_string(p, &at, v.path);
    r->call_id = copy_string(p, &at, v.call);
  } else if (r->candidate.length != 0U) {
    memcpy(p, r->candidate.bytes, r->candidate.length);
    r->candidate.bytes = p;
  }
  *out = *r;
  *required = need;
  return COG_OK;
}
