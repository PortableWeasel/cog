/** @file session_admission.h @brief Private durable session admission contract. */
#ifndef COG_CLI_SESSION_ADMISSION_H
#define COG_CLI_SESSION_ADMISSION_H
#include <cog_cli/cli.h>

/** Construct/load app session and matching store/guard before provider work.
 * @param[in,out] app Constructor-owned app with immutable resolved options.
 * Required output members start NULL and remain owned by the app on any failure;
 * outer app_new consumes them and publishes no app pointer until full success.
 *
 * NEW/DURABLE: preserve ID collision refusal, admit a fresh epoch and acquire its
 * normal guard, then save an empty session checkpoint before success. Empty is
 * zero nodes/ledger/high-water/active ID, with its exact session identity, never
 * a fabricated user/system message or guessed model/configuration provenance.
 * Save failure propagates and INDETERMINATE freezes admission; no provider/tool/
 * input activity occurs before success. Owner admission may remain durable on
 * constructor failure; cleanup never deletes/resets the ID.
 *
 * RESUME only: a valid nonzero owner with checkpoint0 may be an interrupted empty
 * reservation. Read owner/checkpoint, require replay load_ex to report NOT_FOUND,
 * construct an empty matching-ID session, then CAS exact owner/checkpoint0 to a
 * fresh incarnation/epoch and acquire normal guard. The store's locked CAS must
 * refuse legacy state/stale authority/live guard; no read alone authorizes work.
 * cog_store_checkpoint_save commits the empty checkpoint under that exact guard
 * before success. No checkpoint/token/owner is reused
 * or reset. Epoch exhaustion, CAS/acquire/save failure propagates; no retry loop.
 *
 * An absent owner never creates a session through RESUME. Successful legacy load
 * is ineligible for empty repair and follows the existing migration/refusal path.
 * Unowned valid v1/v2, unreadable/corrupt legacy, committed data and uncertain
 * effects retain existing load/migration/recovery rules. FORK/RECOVER do not
 * fabricate an empty source. Save failure never resets/reuses identities or
 * discards committed data; a changed durable owner or checkpoint may remain.
 * Outer construction releases guard/store/session with no model/effects.
 * Normal owned RESUME needs no new operator fencing assertion for this no-data
 * case: a live guard excludes CAS and no committed STARTED/effect exists at0.
 * Current configuration resolves exactly as ordinary RESUME; stored nonempty
 * model/provider/message provenance is never replaced or inferred from epochs.
 * No app-side filesystem inspection or private store layout dependency.
 * Requires reviewed G correction9a95735: checkpoint0 recognized legacy remnants
 * must be refused by existing store operations, never returned as empty proof.
 *
 * LIST/disabled store and existing checkpoint admission retain their contracts.
 * @return Existing session/store constructor, load, CAS, acquire and save errors;
 * OK only after required durable checkpoint and guard; NULL_PTR for app.
 */
cog_result_t cog_cli_sessions_open(cog_cli_app_t *app);
#endif
