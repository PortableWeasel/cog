# Guard ownership source review

Independent narrow source review: production `60b6ec9`, with corrected
`execution.c` from `09e08552341bd009ed9282363b7ee7f3cf711a85`.

The original source passed IO/LIMIT consumption, INVALID_STATE retention,
borrower refusal, normal/recovery allocation-before-callback, and publication
without post-callback allocation. It failed the approved consuming-AGAIN rule:
source release used the general driver's AGAIN-filtering `remember` helper.
The correction latches every consuming cleanup failure directly without changing
ordinary driver progress handling.

Independent GCC and Clang strict direct compilation each pass all three actual
fault fixtures: `test_app_close_guard_critic`, `test_guard_backend_ownership`,
`test_guard_again_critic`. The latter additionally checks direct app disposal
and checked execution close with both OK and a prior PROTOCOL error. The original
60b6ec9 is the failing negative control for retained source AGAIN. Both compilers
reproduced it. After correction, source remains frozen, owner record unchanged,
no second guard release occurs, and the first failure survives step/end.

Exact changed core `session_store.c` and A-owned app sources were compiled
explicitly; remaining actual dependency archives are the immutable 260ab25
snapshot in review-o4-app/build-app-review/joined. No API stubs. Commands and logs
are in this checkout's ignored build-guard-review directory. Full instrumented
dependency validation is pending A's shared profile availability; this report
is not a whole O4/native gate approval.

Store constructor and release return paths now match ab056c5: successful custom
acquire cannot encounter subsequent wrapper allocation failure; callback failure
never publishes a wrapper; INVALID_STATE alone retains an admitted guard; all
other returns unlink/free the wrapper. App cleanup preserves caller primary,
clears consumed aliases, and retains a cleanup-only owner on late refusal.

Follow-up independent sanitizer validation: all three fixtures also PASS with
Clang ASan+UBSan, leak detection and halt-on-error enabled. All project archive
inputs were copied from A's stable build-o4-asan at exact5e24284 (production
09e0855 plus enhanced regression), and explicit owned sources were instrumented.
External recovered libcurl remains uninstrumented. The narrow fae1e27 source fix
is independently PASS; no whole O4/native acceptance is implied.
