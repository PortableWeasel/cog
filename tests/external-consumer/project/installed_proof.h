/**
 * @file installed_proof.h
 * @brief Test-only public installed consumer proof boundaries.
 *
 * This header belongs to the separately copied external C11 project. It never
 * ships as a Cog API and includes no implementation headers. Each proof returns
 * zero on success and consumes all locally acquired resources before returning.
 * The caller creates scratch directories inside the producer build tree.
 */
#ifndef INSTALLED_PROOF_H
#define INSTALLED_PROOF_H

/** Exercise a legitimate caller-owned fenced backend through installed public
 * callbacks. The backend wraps a real file store and obeys consuming release:
 * INVALID_STATE retains ownership; IO/AGAIN consume once. Check normal/recovery
 * acquisition preallocation failure before callback and reverse context cleanup.
 */
int installed_fenced_store_proof(const char *directory);

/** Spawn the normally installed, twice-relocated cog-cli native worker through
 * the public orchestrator. Observe exact real kernel PID/UID READY and retained
 * events; drain asynchronous shutdown and prove exact native reap before free.
 * Also round-trip public CGW1/CGH1 and exercise fenced local owner route closure.
 * This does not promote private host_runtime/execution APIs to public headers.
 */
int installed_orchestrator_proof(const char *executable, const char *directory);

/** Execute the public application against actual offline REAL TLS, using an
 * explicit caller-owned net context/CA and durable tool policy. Python owns the
 * finite provider script and validates real requests/effects. No private host
 * runtime API, mock fallback or dependency-global lifecycle hidden in Cog.
 */
int installed_real_app_proof(const char *directory, const char *url,
                             const char *certificate);

#endif
