/** @file test_instruction_transfer.c @brief Pure native capsule codec
 * contracts. */
#include "instruction_hash.h"
#include "instruction_transfer.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      (void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);            \
      abort();                                                                 \
    }                                                                          \
  } while (0)
static volatile size_t allocations;
void *__wrap_malloc(size_t n);
void *__wrap_calloc(size_t n, size_t z);
void *__wrap_realloc(void *p, size_t n);
void *__wrap_malloc(size_t n) {
  (void)n;
  ++allocations;
  return NULL;
}
void *__wrap_calloc(size_t n, size_t z) {
  (void)n;
  (void)z;
  ++allocations;
  return NULL;
}
void *__wrap_realloc(void *p, size_t n) {
  (void)p;
  (void)n;
  ++allocations;
  return NULL;
}
/* Codecs validate only framing/hash, not inner manifest semantics. This fixed
 * CGIM prefix deliberately proves they do not silently invoke constructors. */
static const unsigned char capsule[] = {'C', 'G', 'I', 'M', 1U, 0U,   0U, 0U,
                                        1U,  0U,  0U,  0U,  0U, 255U, 42U};
static cog_cli_instruction_capsule_t make_capsule(uint64_t snapshot) {
  cog_cli_instruction_capsule_t c = {
      .identity = {.catalog_generation = 7U, .snapshot_id = snapshot},
      .bytes = capsule,
      .length = sizeof(capsule)};
  nt_hash(c.bytes, c.length, c.identity.sha256);
  return c;
}
static void check_bytes(const unsigned char *p, size_t n, unsigned char value) {
  for (size_t i = 0U; i < n; ++i)
    CHECK(p[i] == value);
}
static void bootstrap(void) {
  const unsigned char config[] = {'C', 'G', 'B', '1', 0U, 255U};
  const cog_cli_instruction_bootstrap_t input = {config, sizeof(config),
                                                 make_capsule(2U)};
  unsigned char bytes[512], storage[512], copy[512];
  size_t required = 999U, n = 0U;
  memset(bytes, 0xa5, sizeof(bytes));
  CHECK(cog_cli_instruction_bootstrap_encode(&input, NULL, 0U, &required) ==
        COG_ERROR_OVERFLOW);
  CHECK(required == 60U + sizeof(config) + sizeof(capsule));
  CHECK(cog_cli_instruction_bootstrap_encode(&input, bytes, required - 1U,
                                             &required) == COG_ERROR_OVERFLOW);
  check_bytes(bytes, sizeof(bytes), 0xa5U);
  CHECK(cog_cli_instruction_bootstrap_encode(&input, bytes, sizeof(bytes),
                                             &n) == COG_OK &&
        n == required);
  CHECK(memcmp(bytes, "CGB2\0\0\0\6CGB1", 12U) == 0);
  cog_cli_instruction_bootstrap_t decoded = {0};
  CHECK(cog_cli_instruction_bootstrap_decode(bytes, n, &decoded, NULL, 0U,
                                             &required) == COG_ERROR_OVERFLOW);
  CHECK(required == sizeof(config) + sizeof(capsule));
  memset(storage, 0x5a, sizeof(storage));
  decoded.configuration_bytes = 999U;
  CHECK(cog_cli_instruction_bootstrap_decode(bytes, n, &decoded, storage,
                                             required - 1U,
                                             &required) == COG_ERROR_OVERFLOW);
  CHECK(decoded.configuration_bytes == 999U);
  check_bytes(storage, sizeof(storage), 0x5aU);
  CHECK(cog_cli_instruction_bootstrap_decode(
            bytes, n, &decoded, storage, sizeof(storage), &required) == COG_OK);
  CHECK(decoded.configuration == storage &&
        memcmp(decoded.configuration, config, sizeof(config)) == 0);
  CHECK(decoded.instructions.bytes == storage + sizeof(config));
  CHECK(memcmp(decoded.instructions.bytes, capsule, sizeof(capsule)) == 0);
  size_t again = 0U;
  CHECK(cog_cli_instruction_bootstrap_encode(&decoded, copy, sizeof(copy),
                                             &again) == COG_OK);
  CHECK(n == again && memcmp(bytes, copy, n) == 0);
  for (size_t k = 0U; k < n; ++k) {
    decoded.configuration_bytes = 999U;
    required = 888U;
    memset(storage, 0x5a, sizeof(storage));
    CHECK(cog_cli_instruction_bootstrap_decode(bytes, k, &decoded, storage,
                                               sizeof(storage),
                                               &required) != COG_OK);
    CHECK(decoded.configuration_bytes == 999U && required == 888U);
    check_bytes(storage, sizeof(storage), 0x5aU);
  }
  bytes[n] = 0U;
  CHECK(cog_cli_instruction_bootstrap_decode(bytes, n + 1U, &decoded, NULL, 0U,
                                             &required) == COG_ERROR_PROTOCOL);
  bytes[3] = '9';
  CHECK(cog_cli_instruction_bootstrap_decode(
            bytes, n, &decoded, NULL, 0U, &required) == COG_ERROR_UNSUPPORTED);
  bytes[3] = '2';
  bytes[n - 1U] ^= 1U;
  CHECK(cog_cli_instruction_bootstrap_decode(bytes, n, &decoded, NULL, 0U,
                                             &required) == COG_ERROR_PROTOCOL);
  bytes[n - 1U] ^= 1U;
  cog_cli_instruction_bootstrap_t legacy = {.configuration=config, .configuration_bytes=sizeof(config)};
  CHECK(cog_cli_instruction_bootstrap_encode(&legacy, bytes, sizeof(bytes),
                                             &n) == COG_OK);
  CHECK(cog_cli_instruction_bootstrap_decode(
            bytes, n, &decoded, storage, sizeof(storage), &required) == COG_OK);
  CHECK(decoded.instructions.bytes == NULL &&
        decoded.instructions.length == 0U &&
        decoded.instructions.identity.snapshot_id == 0U);
  legacy.instructions.identity.snapshot_id = 1U;
  CHECK(cog_cli_instruction_bootstrap_encode(&legacy, NULL, 0U, &required) ==
        COG_ERROR_INVALID_ARG);
}
static cog_cli_instruction_resource_t
request(cog_cli_instruction_resource_operation_t operation) {
  const cog_cli_instruction_resource_t r = {
      .kind = COG_CLI_INSTRUCTION_RESOURCE_REQUEST,
      .context = make_capsule(2U).identity,
      .operation = operation,
      .root_id = "work",
      .skill_name =
          operation == COG_CLI_INSTRUCTION_RESOURCE_SCOPE ? "" : "build",
      .path = operation == COG_CLI_INSTRUCTION_RESOURCE_LOAD
                  ? "docs/example.txt"
                  : "",
      .assistant_node_id = 19U,
      .call_id = "call",
      .call_ordinal = 3U,
      .request_checkpoint_generation = 27U};
  return r;
}
static void roundtrip(const cog_cli_instruction_resource_t *r) {
  unsigned char bytes[1024], copy[1024], storage[1024];
  size_t n = 0U, again = 0U, required = 0U;
  memset(bytes, 0x5a, sizeof(bytes));
  CHECK(cog_cli_instruction_resource_encode(r, NULL, 0U, &n) ==
        COG_ERROR_OVERFLOW);
  CHECK(cog_cli_instruction_resource_encode(r, bytes, n - 1U, &required) ==
            COG_ERROR_OVERFLOW &&
        required == n);
  check_bytes(bytes, sizeof(bytes), 0x5aU);
  CHECK(cog_cli_instruction_resource_encode(r, bytes, sizeof(bytes),
                                            &required) == COG_OK &&
        required == n);
  CHECK(memcmp(bytes, "CGIR\2", 5U) == 0 && bytes[5] == (unsigned char)r->kind);
  cog_cli_instruction_resource_t out = {0};
  CHECK(cog_cli_instruction_resource_decode(
            bytes, n, &out, storage, sizeof(storage), &required) == COG_OK);
  CHECK(out.kind == r->kind &&
        out.context.snapshot_id == r->context.snapshot_id &&
        out.outcome == r->outcome);
  if (r->kind == COG_CLI_INSTRUCTION_RESOURCE_REQUEST) {
    CHECK(strcmp(out.root_id, r->root_id) == 0 &&
          strcmp(out.skill_name, r->skill_name) == 0 &&
          strcmp(out.path, r->path) == 0);
    CHECK(out.assistant_node_id == 19U && out.call_ordinal == 3U &&
          out.request_checkpoint_generation == 27U &&
          strcmp(out.call_id, "call") == 0);
  }
  CHECK(cog_cli_instruction_resource_encode(&out, copy, sizeof(copy), &again) ==
            COG_OK &&
        again == n && memcmp(copy, bytes, n) == 0);
  if (required != 0U) {
    const size_t full = required;
    out.assistant_node_id = 999U;
    memset(storage, 0xa5, sizeof(storage));
    CHECK(cog_cli_instruction_resource_decode(bytes, n, &out, storage,
                                              full - 1U, &required) ==
              COG_ERROR_OVERFLOW &&
          required == full);
    CHECK(out.assistant_node_id == 999U);
    check_bytes(storage, sizeof(storage), 0xa5U);
  }
  for (size_t k = 0U; k < n; ++k) {
    out.assistant_node_id = 999U;
    required = 888U;
    memset(storage, 0xa5, sizeof(storage));
    CHECK(cog_cli_instruction_resource_decode(
              bytes, k, &out, storage, sizeof(storage), &required) != COG_OK);
    CHECK(out.assistant_node_id == 999U && required == 888U);
    check_bytes(storage, sizeof(storage), 0xa5U);
  }
  bytes[n] = 0U;
  CHECK(cog_cli_instruction_resource_decode(bytes, n + 1U, &out, NULL, 0U,
                                            &required) == COG_ERROR_PROTOCOL);
  bytes[4] = 3U;
  CHECK(cog_cli_instruction_resource_decode(
            bytes, n, &out, NULL, 0U, &required) == COG_ERROR_UNSUPPORTED);
  bytes[4] = 2U;
  for (size_t k = 0U; k < n; ++k) {
    const unsigned char saved = bytes[k];
    bytes[k] ^= 0xffU;
    if (cog_cli_instruction_resource_decode(
            bytes, n, &out, storage, sizeof(storage), &required) == COG_OK) {
      CHECK(cog_cli_instruction_resource_encode(&out, copy, sizeof(copy),
                                                &again) == COG_OK);
      CHECK(again == n && memcmp(copy, bytes, n) == 0);
    }
    bytes[k] = saved;
  }
}
static void resources(void) {
  cog_cli_instruction_resource_t r = request(COG_CLI_INSTRUCTION_RESOURCE_LOAD);
  roundtrip(&r);
  r = request(COG_CLI_INSTRUCTION_RESOURCE_ACTIVATE);
  roundtrip(&r);
  r = request(COG_CLI_INSTRUCTION_RESOURCE_SCOPE);
  roundtrip(&r);
  r.path = "src";
  roundtrip(&r);
  r = (cog_cli_instruction_resource_t){.kind =
                                           COG_CLI_INSTRUCTION_RESOURCE_RESULT,
                                       .context = make_capsule(2U).identity,
                                       .candidate = make_capsule(3U)};
  roundtrip(&r);
  r.candidate = make_capsule(2U);
  roundtrip(&r);
  static const cog_result_t errors[] = {
      COG_ERROR_NOT_FOUND,   COG_ERROR_DENIED,  COG_ERROR_LIMIT,
      COG_ERROR_IO,          COG_ERROR_ABORTED, COG_ERROR_TIMEOUT,
      COG_ERROR_UNSUPPORTED, COG_ERROR_PROTOCOL};
  r.candidate = (cog_cli_instruction_capsule_t){0};
  for (size_t i = 0U; i < sizeof(errors) / sizeof(errors[0]); ++i) {
    r.result = errors[i];
    roundtrip(&r);
  }
  r = (cog_cli_instruction_resource_t){
      .kind = COG_CLI_INSTRUCTION_RESOURCE_CONSUMED,
      .context = make_capsule(2U).identity};
  roundtrip(&r);
  for (unsigned kind = 4U; kind <= 5U; ++kind) {
    r = (cog_cli_instruction_resource_t){
        .kind = (cog_cli_instruction_resource_kind_t)kind,
        .context = make_capsule(2U).identity,
        .candidate = {.identity = make_capsule(3U).identity},
        .outcome = COG_CLI_INSTRUCTION_COMMIT_SAVED,
        .checkpoint_generation = 29U};
    roundtrip(&r);
    r.outcome = COG_CLI_INSTRUCTION_COMMIT_UNCERTAIN;
    r.checkpoint_generation = 0U;
    roundtrip(&r);
    r.outcome = COG_CLI_INSTRUCTION_COMMIT_REFUSED;
    roundtrip(&r);
    r.candidate = (cog_cli_instruction_capsule_t){0};
    roundtrip(&r);
    r.candidate.identity = r.context;
    r.outcome = COG_CLI_INSTRUCTION_COMMIT_UNCHANGED;
    roundtrip(&r);
  }
  unsigned char output[1024];
  memset(output, 0xa5, sizeof(output));
  size_t required = 777U;
  r = request(COG_CLI_INSTRUCTION_RESOURCE_LOAD);
  r.path = "../escape";
  CHECK(cog_cli_instruction_resource_encode(
            &r, output, sizeof(output), &required) == COG_ERROR_INVALID_ARG &&
        required == 777U);
  check_bytes(output, sizeof(output), 0xa5U);
  r = request(COG_CLI_INSTRUCTION_RESOURCE_LOAD);
  r.call_ordinal = 16U;
  CHECK(cog_cli_instruction_resource_encode(&r, NULL, 0U, &required) ==
        COG_ERROR_INVALID_ARG);
  r = request(COG_CLI_INSTRUCTION_RESOURCE_LOAD);
  r.request_checkpoint_generation = 0U;
  CHECK(cog_cli_instruction_resource_encode(&r, NULL, 0U, &required) ==
        COG_ERROR_INVALID_ARG);
  r = request(COG_CLI_INSTRUCTION_RESOURCE_SCOPE);
  r.skill_name = "build";
  CHECK(cog_cli_instruction_resource_encode(&r, NULL, 0U, &required) ==
        COG_ERROR_INVALID_ARG);
  r = (cog_cli_instruction_resource_t){
      .kind = COG_CLI_INSTRUCTION_RESOURCE_COMMIT,
      .context = make_capsule(2U).identity,
      .candidate = {.identity = make_capsule(3U).identity},
      .outcome = COG_CLI_INSTRUCTION_COMMIT_SAVED};
  CHECK(cog_cli_instruction_resource_encode(&r, NULL, 0U, &required) ==
        COG_ERROR_INVALID_ARG);
  r.outcome = COG_CLI_INSTRUCTION_COMMIT_UNCHANGED;
  CHECK(cog_cli_instruction_resource_encode(&r, NULL, 0U, &required) ==
        COG_ERROR_INVALID_ARG);
}
static void sha_golden(void) {
  static const unsigned char expected[32] = {
      0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
      0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
      0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
  unsigned char hash[32];
  nt_hash("abc", 3U, hash);
  CHECK(memcmp(expected, hash, 32U) == 0);
}
int main(void) {
  sha_golden();
  bootstrap();
  resources();
  CHECK(allocations == 0U);
  (void)puts("native instruction codecs passed");
  return 0;
}
